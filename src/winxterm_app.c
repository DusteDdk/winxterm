#include "winxterm_app.h"

#include "resource.h"
#include "winxterm_clipboard.h"
#include "winxterm_input.h"
#include "winxterm_macro.h"
#include "winxterm_mouse.h"
#include "winxterm_options.h"
#include "winxterm_render.h"
#include "winxterm_scale.h"
#include "winxterm_settings.h"
#include "winxterm_ux.h"
#include "winxterm_window_placement.h"

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wincodec.h>
#include <windowsx.h>

struct WinxtermAppSessionUx {
    uint64_t session_id;
    WinxtermUxState state;
    WinxtermAppSessionUx *next;
};

static const wchar_t WINXTERM_WINDOW_CLASS_NAME[] = L"WinxtermMainWindow";
static const wchar_t WINXTERM_WINDOW_TITLE[] = L"XTerm for Windows";
static const UINT_PTR WINXTERM_CURSOR_TIMER_ID = 1u;
static const UINT_PTR WINXTERM_BELL_TIMER_ID = 2u;
static const UINT_PTR WINXTERM_FRAME_TIMER_ID = 3u;
static const UINT_PTR WINXTERM_MACRO_TIMER_ID = 4u;
static const UINT_PTR WINXTERM_CLICK_EVENT_TIMER_ID = 5u;
static const UINT_PTR WINXTERM_COPY_OVERLAY_TIMER_ID = 6u;
static const UINT WINXTERM_DEFAULT_CURSOR_BLINK_MS = 500u;
static const UINT WINXTERM_FRAME_THROTTLE_MS = 16u;
static const UINT WINXTERM_COPY_OVERLAY_FRAME_MS = 33u;
static const DWORD WINXTERM_COPY_OVERLAY_SOLID_MS = 250u;
static const DWORD WINXTERM_COPY_OVERLAY_FADE_MS = 250u;
static const int WINXTERM_LOCAL_DRAG_DISTANCE_PIXELS = 2;
static const int WINXTERM_MIN_COLUMNS = 20;
static const int WINXTERM_MIN_ROWS = 4;
static const size_t WINXTERM_PASTE_CHUNK_SIZE = 512u;
static const ULONGLONG WINXTERM_ALT_DRAG_THROTTLE_MS = 20ull;
static const uint32_t WINXTERM_HOVER_OUTLINE_RGB = 0x0000ff00u;
static const uint8_t WINXTERM_HOVER_OUTLINE_ALPHA = 128u;
static const uint8_t WINXTERM_CLICK_PREVIEW_ALPHA = 255u;
static const uint32_t WINXTERM_COPY_OVERLAY_RGB = 0x00ff9900u;

enum {
    WINXTERM_OUTPUT_COMMIT_MAX_PASSES =
        (WINXTERM_BRIDGE_OUTPUT_MAX_CAPACITY / WINXTERM_BRIDGE_OUTPUT_COMMIT_MAX_BYTES) + 1u
};

enum {
    WINXTERM_ALT_DRAG_EDGE_LEFT = 1u,
    WINXTERM_ALT_DRAG_EDGE_TOP = 2u,
    WINXTERM_ALT_DRAG_EDGE_RIGHT = 4u,
    WINXTERM_ALT_DRAG_EDGE_BOTTOM = 8u
};

enum {
    WINXTERM_CLOSE_DIALOG_MARGIN = 16,
    WINXTERM_CLOSE_DIALOG_GAP = 12,
    WINXTERM_CLOSE_DIALOG_INTRO_HEIGHT = 36,
    WINXTERM_CLOSE_DIALOG_CHILD_HEIGHT = 150,
    WINXTERM_CLOSE_DIALOG_CHECK_HEIGHT = 24,
    WINXTERM_CLOSE_DIALOG_BUTTON_WIDTH = 80,
    WINXTERM_CLOSE_DIALOG_BUTTON_HEIGHT = 26,
    WINXTERM_CLOSE_DIALOG_MIN_CLIENT_WIDTH = 560,
    WINXTERM_CLOSE_DIALOG_MIN_CLIENT_HEIGHT = 320
};

typedef enum WinxtermCloseDecision {
    WINXTERM_CLOSE_DECISION_CANCEL = 0,
    WINXTERM_CLOSE_DECISION_CLOSE,
    WINXTERM_CLOSE_DECISION_HEADLESS,
    WINXTERM_CLOSE_DECISION_TERMINATE
} WinxtermCloseDecision;

typedef struct WinxtermCloseDialog {
    HWND hwnd;
    HWND intro_label;
    HWND child_label;
    HWND checkbox;
    HWND ok_button;
    HWND cancel_button;
    bool done;
    WinxtermCloseDecision decision;
    wchar_t child_text[65536];
} WinxtermCloseDialog;

typedef struct WinxtermRenderWorker {
    struct WinxtermRenderWorkerPool *pool;
    HANDLE thread;
    HANDLE work_event;
    HANDLE done_event;
    DWORD thread_id;
    unsigned int index;
    int row_start;
    int row_count;
    const WinxtermScreenRenderSnapshot *snapshot;
    uint32_t *pixels;
    uint64_t render_ns;
    WinxtermRenderContext context;
} WinxtermRenderWorker;

struct WinxtermRenderWorkerPool {
    HINSTANCE instance;
    WinxtermLog *log;
    HANDLE shutdown_event;
    unsigned int worker_count;
    WinxtermRenderWorker workers[WINXTERM_MAX_RENDER_THREADS];
};

enum {
    WINXTERM_MENU_COPY = 1001,
    WINXTERM_MENU_PASTE,
    WINXTERM_MENU_CLEAR_SCROLLBACK,
    WINXTERM_MENU_RESET_TERMINAL,
    WINXTERM_MENU_SCALE_1,
    WINXTERM_MENU_SCALE_2,
    WINXTERM_MENU_SCALE_3,
    WINXTERM_MENU_SCALE_4,
    WINXTERM_MENU_RENDER_SPANS,
    WINXTERM_MENU_RENDER_ROW_MASKS,
    WINXTERM_MENU_RENDER_PRECOLORED,
    WINXTERM_MENU_DIAGNOSTICS,
    WINXTERM_MENU_RENDER_THREADS_BASE = 1100,
    WINXTERM_MENU_RENDER_THREADS_LAST =
        WINXTERM_MENU_RENDER_THREADS_BASE + WINXTERM_MAX_RENDER_THREADS - 1
};

static LRESULT CALLBACK winxterm_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
static LRESULT CALLBACK winxterm_close_dialog_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
static void winxterm_app_request_frame(WinxtermApp *app, unsigned int causes);
static void winxterm_app_handle_macro_update(WinxtermApp *app);
static void winxterm_app_handle_command(WinxtermApp *app, WPARAM wparam);
static SIZE winxterm_app_min_window_size(WinxtermApp *app);
static void winxterm_app_snap_window_to_cells(WinxtermApp *app);
static void winxterm_app_update_scrollbar(WinxtermApp *app);

/* AdjustWindowRectEx ignores WS_VSCROLL, so client-to-outer sizing must add the
   scrollbar width itself whenever the style carries one. */
static bool winxterm_app_adjust_window_rect(RECT *rect, DWORD style, DWORD ex_style)
{
    if (rect == 0 || !AdjustWindowRectEx(rect, style, FALSE, ex_style)) {
        return false;
    }
    if ((style & WS_VSCROLL) != 0u) {
        rect->right += GetSystemMetrics(SM_CXVSCROLL);
    }
    return true;
}

static unsigned int winxterm_app_clamp_render_thread_count(unsigned int count)
{
    if (count == 0u) {
        return 1u;
    }
    return count > WINXTERM_MAX_RENDER_THREADS ? WINXTERM_MAX_RENDER_THREADS : count;
}

static unsigned int winxterm_app_active_render_thread_count(const WinxtermApp *app)
{
    if (app == 0) {
        return 1u;
    }
    if (app->render_workers != 0 && app->render_workers->worker_count != 0u) {
        return app->render_workers->worker_count;
    }
    return winxterm_app_clamp_render_thread_count(app->render_thread_count);
}

static DWORD WINAPI winxterm_render_worker_thread_proc(void *context)
{
    WinxtermRenderWorker *worker = (WinxtermRenderWorker *)context;
    if (worker == 0 || worker->pool == 0) {
        return 1;
    }

    winxterm_render_context_init(&worker->context);
    (void)winxterm_render_context_load_fallback_fonts(&worker->context,
                                                      worker->pool->instance,
                                                      worker->pool->log);

    HANDLE waits[2] = {worker->pool->shutdown_event, worker->work_event};
    for (;;) {
        DWORD wait_result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (wait_result == WAIT_OBJECT_0 || wait_result == WAIT_FAILED) {
            break;
        }
        if (wait_result == WAIT_OBJECT_0 + 1u) {
            uint64_t start_ns = winxterm_bridge_timestamp_ns();
            worker->context.active_diagnostics = 0;
            if (worker->snapshot != 0 && worker->pixels != 0 && worker->row_count > 0) {
                winxterm_screen_render_snapshot_range(worker->snapshot,
                                                       &worker->context,
                                                       worker->pixels,
                                                       worker->row_start,
                                                       worker->row_count);
            }
            uint64_t end_ns = winxterm_bridge_timestamp_ns();
            worker->render_ns = end_ns >= start_ns ? end_ns - start_ns : 0u;
            SetEvent(worker->done_event);
        }
    }

    winxterm_render_context_dispose(&worker->context);
    return 0;
}

static void winxterm_render_worker_close_handle(HANDLE *handle)
{
    if (handle != 0 && *handle != 0) {
        CloseHandle(*handle);
        *handle = 0;
    }
}

static void winxterm_render_worker_pool_destroy(WinxtermRenderWorkerPool *pool)
{
    if (pool == 0) {
        return;
    }
    if (pool->shutdown_event != 0) {
        SetEvent(pool->shutdown_event);
    }
    for (unsigned int i = 0u; i < pool->worker_count; ++i) {
        if (pool->workers[i].work_event != 0) {
            SetEvent(pool->workers[i].work_event);
        }
    }
    for (unsigned int i = 0u; i < pool->worker_count; ++i) {
        if (pool->workers[i].thread != 0) {
            WaitForSingleObject(pool->workers[i].thread, INFINITE);
        }
        winxterm_render_worker_close_handle(&pool->workers[i].thread);
        winxterm_render_worker_close_handle(&pool->workers[i].work_event);
        winxterm_render_worker_close_handle(&pool->workers[i].done_event);
    }
    winxterm_render_worker_close_handle(&pool->shutdown_event);
    free(pool);
}

static WinxtermRenderWorkerPool *winxterm_render_worker_pool_create(HINSTANCE instance,
                                                                    WinxtermLog *log,
                                                                    unsigned int requested_count)
{
    unsigned int worker_count = winxterm_app_clamp_render_thread_count(requested_count);
    WinxtermRenderWorkerPool *pool = (WinxtermRenderWorkerPool *)calloc(1u, sizeof(*pool));
    if (pool == 0) {
        return 0;
    }

    pool->instance = instance;
    pool->log = log;
    pool->worker_count = worker_count;
    pool->shutdown_event = CreateEventW(0, TRUE, FALSE, 0);
    if (pool->shutdown_event == 0) {
        free(pool);
        return 0;
    }

    for (unsigned int i = 0u; i < worker_count; ++i) {
        WinxtermRenderWorker *worker = &pool->workers[i];
        worker->pool = pool;
        worker->index = i;
        worker->work_event = CreateEventW(0, FALSE, FALSE, 0);
        worker->done_event = CreateEventW(0, TRUE, FALSE, 0);
        if (worker->work_event == 0 || worker->done_event == 0) {
            winxterm_render_worker_pool_destroy(pool);
            return 0;
        }
        worker->thread = CreateThread(0,
                                      0,
                                      winxterm_render_worker_thread_proc,
                                      worker,
                                      0,
                                      &worker->thread_id);
        if (worker->thread == 0) {
            winxterm_render_worker_pool_destroy(pool);
            return 0;
        }
    }

    return pool;
}

static void winxterm_app_apply_render_thread_count(WinxtermApp *app, unsigned int requested_count)
{
    if (app == 0) {
        return;
    }

    unsigned int count = winxterm_app_clamp_render_thread_count(requested_count);
    if (winxterm_app_active_render_thread_count(app) == count) {
        return;
    }

    winxterm_render_worker_pool_destroy(app->render_workers);
    app->render_workers = 0;
    app->render_thread_count = count;
    app->render_workers = winxterm_render_worker_pool_create(app->instance, app->log, count);
    if (app->render_workers == 0) {
        winxterm_log_writef(app->log,
                            "render worker pool unavailable after thread count change to %u; using UI-thread renderer",
                            count);
    } else {
        winxterm_log_writef(app->log,
                            "render worker pool changed threads=%u",
                            app->render_workers->worker_count);
    }
    winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
}

static unsigned int winxterm_render_worker_effective_count(const WinxtermRenderWorkerPool *pool,
                                                           int rows)
{
    if (pool == 0 || pool->worker_count == 0u || rows <= 0) {
        return 0u;
    }
    unsigned int count = pool->worker_count;
    if (count > (unsigned int)rows) {
        count = (unsigned int)rows;
    }
    return count;
}

static bool winxterm_render_worker_pool_render(WinxtermRenderWorkerPool *pool,
                                               const WinxtermScreenRenderSnapshot *snapshot,
                                               uint32_t *pixels,
                                               uint64_t *worker_total_ns,
                                               uint64_t *worker_max_ns)
{
    if (worker_total_ns != 0) {
        *worker_total_ns = 0u;
    }
    if (worker_max_ns != 0) {
        *worker_max_ns = 0u;
    }
    unsigned int count = winxterm_render_worker_effective_count(pool, snapshot != 0 ? snapshot->rows : 0);
    if (pool == 0 || snapshot == 0 || pixels == 0 || count == 0u) {
        return false;
    }

    HANDLE done_events[WINXTERM_MAX_RENDER_THREADS];
    int row = 0;
    int base_rows = snapshot->rows / (int)count;
    int extra_rows = snapshot->rows % (int)count;
    for (unsigned int i = 0u; i < count; ++i) {
        int rows = base_rows + ((int)i < extra_rows ? 1 : 0);
        WinxtermRenderWorker *worker = &pool->workers[i];
        worker->snapshot = snapshot;
        worker->pixels = pixels;
        worker->row_start = row;
        worker->row_count = rows;
        worker->render_ns = 0u;
        ResetEvent(worker->done_event);
        done_events[i] = worker->done_event;
        row += rows;
    }

    for (unsigned int i = 0u; i < count; ++i) {
        SetEvent(pool->workers[i].work_event);
    }

    DWORD wait_result = WaitForMultipleObjects(count, done_events, TRUE, INFINITE);
    if (wait_result == WAIT_FAILED) {
        return false;
    }

    uint64_t total_ns = 0u;
    uint64_t max_ns = 0u;
    for (unsigned int i = 0u; i < count; ++i) {
        uint64_t elapsed = pool->workers[i].render_ns;
        total_ns += elapsed;
        if (elapsed > max_ns) {
            max_ns = elapsed;
        }
        pool->workers[i].snapshot = 0;
        pool->workers[i].pixels = 0;
        pool->workers[i].row_start = 0;
        pool->workers[i].row_count = 0;
    }
    if (worker_total_ns != 0) {
        *worker_total_ns = total_ns;
    }
    if (worker_max_ns != 0) {
        *worker_max_ns = max_ns;
    }
    return true;
}

static int winxterm_app_min_int(int a, int b)
{
    return a < b ? a : b;
}

static uint32_t winxterm_app_paint_background_rgb(const WinxtermApp *app)
{
    (void)app;
    return WINXTERM_DEFAULT_BACKGROUND_RGB;
}

static COLORREF winxterm_app_colorref_from_rgb(uint32_t rgb)
{
    return RGB((BYTE)((rgb >> 16) & 0xffu),
               (BYTE)((rgb >> 8) & 0xffu),
               (BYTE)(rgb & 0xffu));
}

static void winxterm_app_fill_rect_rgb(HDC dc, const RECT *rect, uint32_t rgb)
{
    if (dc == 0 || rect == 0 || rect->left >= rect->right || rect->top >= rect->bottom) {
        return;
    }
    HBRUSH brush = CreateSolidBrush(winxterm_app_colorref_from_rgb(rgb));
    if (brush == 0) {
        return;
    }
    FillRect(dc, rect, brush);
    DeleteObject(brush);
}

static void winxterm_app_copy_front_overlap(uint32_t *dst,
                                            int dst_width,
                                            int dst_height,
                                            const uint32_t *src,
                                            int src_width,
                                            int src_height)
{
    if (dst == 0 || src == 0 || dst_width <= 0 || dst_height <= 0 ||
        src_width <= 0 || src_height <= 0) {
        return;
    }
    int copy_width = winxterm_app_min_int(dst_width, src_width);
    int copy_height = winxterm_app_min_int(dst_height, src_height);
    for (int row = 0; row < copy_height; ++row) {
        memcpy(dst + (size_t)row * (size_t)dst_width,
               src + (size_t)row * (size_t)src_width,
               (size_t)copy_width * sizeof(*dst));
    }
}

static void winxterm_app_free_buffers(WinxtermApp *app)
{
    free(app->front_pixels);
    free(app->back_pixels);
    app->front_pixels = 0;
    app->back_pixels = 0;
    app->bitmap_width = 0;
    app->bitmap_height = 0;
}

static bool winxterm_app_resize_bitmap(WinxtermApp *app, int width, int height)
{
    if (width <= 0 || height <= 0) {
        winxterm_app_free_buffers(app);
        return true;
    }

    if (app->bitmap_width == width && app->bitmap_height == height &&
        app->front_pixels != 0 && app->back_pixels != 0) {
        return true;
    }

    if ((size_t)width > SIZE_MAX / (size_t)height) {
        return false;
    }

    size_t pixel_count = (size_t)width * (size_t)height;
    if (pixel_count > SIZE_MAX / sizeof(uint32_t)) {
        return false;
    }

    uint32_t *front = (uint32_t *)malloc(pixel_count * sizeof(uint32_t));
    uint32_t *back = (uint32_t *)malloc(pixel_count * sizeof(uint32_t));
    if (front == 0 || back == 0) {
        free(front);
        free(back);
        return false;
    }

    winxterm_render_clear(front, width, height, WINXTERM_DEFAULT_BACKGROUND_RGB);
    winxterm_render_clear(back, width, height, WINXTERM_DEFAULT_BACKGROUND_RGB);
    winxterm_app_copy_front_overlap(front,
                                    width,
                                    height,
                                    app->front_pixels,
                                    app->bitmap_width,
                                    app->bitmap_height);

    winxterm_app_free_buffers(app);
    app->front_pixels = front;
    app->back_pixels = back;
    app->bitmap_width = width;
    app->bitmap_height = height;
    return true;
}

static unsigned int winxterm_app_display_scale(const WinxtermApp *app)
{
    return app != 0 && winxterm_display_scale_valid(app->display_scale) ?
        app->display_scale : WINXTERM_DEFAULT_DISPLAY_SCALE;
}

static int winxterm_app_logical_width_from_client(const WinxtermApp *app, int width)
{
    return winxterm_physical_to_logical_pixels(width, winxterm_app_display_scale(app));
}

static int winxterm_app_logical_height_from_client(const WinxtermApp *app, int height)
{
    return winxterm_physical_to_logical_pixels(height, winxterm_app_display_scale(app));
}

static WinxtermCellSize winxterm_app_visible_cells_for_bitmap(const WinxtermScreen *screen,
                                                              int bitmap_width,
                                                              int bitmap_height)
{
    WinxtermCellSize cells = winxterm_pixels_to_cells(bitmap_width, bitmap_height);
    if (screen == 0) {
        cells.columns = 0;
        cells.rows = 0;
        return cells;
    }
    if (cells.columns > screen->columns) {
        cells.columns = screen->columns;
    }
    if (cells.rows > screen->rows) {
        cells.rows = screen->rows;
    }
    if (cells.columns < 0) {
        cells.columns = 0;
    }
    if (cells.rows < 0) {
        cells.rows = 0;
    }
    return cells;
}

static WinxtermCellSize winxterm_app_visible_cells_locked(const WinxtermApp *app)
{
    if (app == 0 || app->bridge == 0) {
        WinxtermCellSize cells = {0, 0};
        return cells;
    }
    return winxterm_app_visible_cells_for_bitmap(&app->bridge->screen,
                                                 app->bitmap_width,
                                                 app->bitmap_height);
}

static bool winxterm_app_hit_test_lparam(WinxtermApp *app,
                                         LPARAM lparam,
                                         WinxtermUxPosition *position,
                                         int *view_row)
{
    if (app == 0 || app->bridge == 0) {
        return false;
    }

    bool hit = false;
    EnterCriticalSection(&app->bridge->screen_lock);
    WinxtermCellSize visible = winxterm_app_visible_cells_locked(app);
    hit = winxterm_ux_hit_test_cells(&app->ux,
                                     &app->bridge->screen,
                                     GET_X_LPARAM(lparam),
                                     GET_Y_LPARAM(lparam),
                                     winxterm_app_display_scale(app),
                                     visible.columns,
                                     visible.rows,
                                     position);
    LeaveCriticalSection(&app->bridge->screen_lock);

    if (hit && view_row != 0) {
        int logical_y = winxterm_physical_to_logical_pixels(GET_Y_LPARAM(lparam),
                                                           winxterm_app_display_scale(app));
        *view_row = logical_y / WINXTERM_CELL_HEIGHT_PIXELS;
    }
    return hit;
}

static void winxterm_app_clear_hover(WinxtermApp *app)
{
    if (app == 0 || !app->hover_valid) {
        return;
    }
    app->hover_valid = false;
    app->hover_column = 0;
    app->hover_view_row = 0;
    winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
}

static void winxterm_app_update_hover(WinxtermApp *app, LPARAM lparam)
{
    if (app == 0 || app->hwnd == 0) {
        return;
    }
    if (!app->hover_tracking) {
        TRACKMOUSEEVENT track;
        memset(&track, 0, sizeof(track));
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = app->hwnd;
        app->hover_tracking = TrackMouseEvent(&track) != FALSE;
    }

    WinxtermUxPosition position;
    int view_row = 0;
    if (!winxterm_app_hit_test_lparam(app, lparam, &position, &view_row)) {
        winxterm_app_clear_hover(app);
        return;
    }

    if (app->hover_valid &&
        app->hover_column == position.column &&
        app->hover_view_row == view_row) {
        return;
    }
    app->hover_valid = true;
    app->hover_column = position.column;
    app->hover_view_row = view_row;
    winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
}

static bool winxterm_app_capture_scale_anchor_locked(WinxtermApp *app)
{
    if (app == 0 || app->bridge == 0) {
        return false;
    }

    app->scale_resize_anchor_valid = false;
    if (app->ux.viewport.follow_output || app->bridge->screen.alternate_active) {
        return false;
    }

    WinxtermCellSize visible = winxterm_app_visible_cells_locked(app);
    size_t first_row = winxterm_ux_primary_first_row_for_rows(&app->ux,
                                                              &app->bridge->screen,
                                                              visible.rows);
    app->scale_resize_anchor_valid =
        winxterm_screen_primary_anchor_from_global_row(&app->bridge->screen,
                                                       first_row,
                                                       &app->scale_resize_anchor);
    return app->scale_resize_anchor_valid;
}

static void winxterm_app_restore_scale_anchor_locked(WinxtermApp *app, int visible_rows)
{
    if (app == 0 || app->bridge == 0 || !app->scale_resize_anchor_valid) {
        return;
    }

    size_t first_row = 0u;
    if (winxterm_screen_primary_global_row_from_anchor(&app->bridge->screen,
                                                       &app->scale_resize_anchor,
                                                       &first_row)) {
        size_t bottom_first =
            winxterm_screen_default_primary_first_row_for_rows(&app->bridge->screen,
                                                               visible_rows,
                                                               0u);
        if (first_row > bottom_first) {
            first_row = bottom_first;
        }
        app->ux.viewport.line_offset_from_bottom = bottom_first - first_row;
        app->ux.viewport.follow_output = app->ux.viewport.line_offset_from_bottom == 0u;
        app->ux.viewport.last_primary_row_count =
            winxterm_screen_primary_view_row_count(&app->bridge->screen);
        app->ux.viewport.bottom_anchor_valid = false;
        winxterm_ux_clamp_viewport_for_rows(&app->ux, &app->bridge->screen, visible_rows);
    }
    app->scale_resize_anchor_valid = false;
}

static void winxterm_app_update_title(WinxtermApp *app)
{
    if (app == 0 || app->hwnd == 0 || app->bridge == 0) {
        return;
    }

    char terminal_title[WINXTERM_BRIDGE_TITLE_CAPACITY];
    terminal_title[0] = '\0';
    EnterCriticalSection(&app->bridge->input_lock);
    strcpy_s(terminal_title, sizeof(terminal_title), app->bridge->terminal_title);
    LeaveCriticalSection(&app->bridge->input_lock);

    wchar_t terminal_title_wide[WINXTERM_BRIDGE_TITLE_CAPACITY];
    terminal_title_wide[0] = L'\0';
    if (terminal_title[0] != '\0') {
        int converted = MultiByteToWideChar(CP_UTF8,
                                           MB_ERR_INVALID_CHARS,
                                           terminal_title,
                                           -1,
                                           terminal_title_wide,
                                           (int)(sizeof(terminal_title_wide) /
                                                 sizeof(terminal_title_wide[0])));
        if (converted <= 0) {
            terminal_title_wide[0] = L'\0';
        }
    }

    wchar_t title[256];
    const wchar_t *bell_prefix = winxterm_bridge_bell_enabled(app->bridge) ?
        winxterm_ux_bell_title_prefix(&app->ux, GetTickCount()) : L"";
    size_t title_count = sizeof(title) / sizeof(title[0]);
    int written = 0;
    if (app->bridge->show_render_stats_in_title) {
        written = terminal_title_wide[0] != L'\0' ?
            swprintf_s(title,
                       title_count,
                       L"%ls%ls - %S %.1f FPS",
                       bell_prefix,
                       terminal_title_wide,
                       winxterm_render_backend_name(winxterm_bridge_backend(app->bridge)),
                       app->bridge->last_fps) :
            swprintf_s(title,
                       title_count,
                       L"%ls%ls - %S %.1f FPS",
                       bell_prefix,
                       WINXTERM_WINDOW_TITLE,
                       winxterm_render_backend_name(winxterm_bridge_backend(app->bridge)),
                       app->bridge->last_fps);
    } else {
        written = terminal_title_wide[0] != L'\0' ?
            swprintf_s(title, title_count, L"%ls%ls", bell_prefix, terminal_title_wide) :
            swprintf_s(title, title_count, L"%ls%ls", bell_prefix, WINXTERM_WINDOW_TITLE);
    }
    if (written > 0) {
        SetWindowTextW(app->hwnd, title);
    }
}

static void winxterm_app_note_rendered_cursor(WinxtermApp *app,
                                              const WinxtermScreenRenderSnapshot *snapshot)
{
    if (app == 0 || snapshot == 0 ||
        !snapshot->cursor_visible ||
        !snapshot->screen_cursor_visible ||
        snapshot->cursor_global_row < snapshot->first_row + (size_t)snapshot->row_offset ||
        snapshot->cursor_global_row >= snapshot->first_row + (size_t)snapshot->row_offset + (size_t)snapshot->rows) {
        if (app != 0) {
            app->rendered_cursor_valid = false;
            app->rendered_cursor_row = 0;
        }
        return;
    }
    size_t local_row = snapshot->cursor_global_row - snapshot->first_row - (size_t)snapshot->row_offset;
    if (local_row >= (size_t)snapshot->rows) {
        app->rendered_cursor_valid = false;
        app->rendered_cursor_row = 0;
        return;
    }
    app->rendered_cursor_valid = true;
    app->rendered_cursor_row = (int)local_row;
}

static void winxterm_app_include_render_row_range(int *top, int *bottom, int row_start, int row_end)
{
    if (top == 0 || bottom == 0 || row_start > row_end) {
        return;
    }
    if (*top < 0 || row_start < *top) {
        *top = row_start;
    }
    if (*bottom < 0 || row_end > *bottom) {
        *bottom = row_end;
    }
}

static void winxterm_app_include_render_row(int *top, int *bottom, int row)
{
    winxterm_app_include_render_row_range(top, bottom, row, row);
}

static bool winxterm_app_snapshot_cursor_local_row(const WinxtermScreenRenderSnapshot *snapshot, int *row)
{
    if (row != 0) {
        *row = 0;
    }
    if (snapshot == 0 ||
        !snapshot->cursor_visible ||
        !snapshot->screen_cursor_visible ||
        snapshot->cursor_global_row < snapshot->first_row + (size_t)snapshot->row_offset ||
        snapshot->cursor_global_row >= snapshot->first_row + (size_t)snapshot->row_offset + (size_t)snapshot->rows) {
        return false;
    }
    size_t local_row = snapshot->cursor_global_row - snapshot->first_row - (size_t)snapshot->row_offset;
    if (local_row >= (size_t)snapshot->rows) {
        return false;
    }
    if (row != 0) {
        *row = (int)local_row;
    }
    return true;
}

static void winxterm_app_draw_hover_outline(WinxtermApp *app)
{
    if (app == 0 || !app->hover_valid || app->back_pixels == 0 ||
        app->hover_column < 0 || app->hover_view_row < 0) {
        return;
    }

    int x = app->hover_column * WINXTERM_CELL_WIDTH_PIXELS;
    int y = app->hover_view_row * WINXTERM_CELL_HEIGHT_PIXELS;
    winxterm_render_blend_rect_outline(app->back_pixels,
                                       app->bitmap_width,
                                       app->bitmap_height,
                                       x,
                                       y,
                                       WINXTERM_CELL_WIDTH_PIXELS,
                                       WINXTERM_CELL_HEIGHT_PIXELS,
                                       WINXTERM_HOVER_OUTLINE_RGB,
                                       WINXTERM_HOVER_OUTLINE_ALPHA);
}

static DWORD winxterm_app_copy_overlay_total_ms(void)
{
    return WINXTERM_COPY_OVERLAY_SOLID_MS + WINXTERM_COPY_OVERLAY_FADE_MS;
}

static bool winxterm_app_copy_overlay_visible(const WinxtermApp *app, DWORD now)
{
    return app != 0 &&
           app->copy_overlay_active &&
           app->copy_overlay_range.enabled &&
           now - app->copy_overlay_start_tick < winxterm_app_copy_overlay_total_ms();
}

static uint8_t winxterm_app_copy_overlay_alpha(const WinxtermApp *app, DWORD now)
{
    if (!winxterm_app_copy_overlay_visible(app, now)) {
        return 0u;
    }

    DWORD elapsed = now - app->copy_overlay_start_tick;
    if (elapsed < WINXTERM_COPY_OVERLAY_SOLID_MS) {
        return 255u;
    }

    DWORD fade_elapsed = elapsed - WINXTERM_COPY_OVERLAY_SOLID_MS;
    if (fade_elapsed >= WINXTERM_COPY_OVERLAY_FADE_MS) {
        return 0u;
    }
    DWORD remaining = WINXTERM_COPY_OVERLAY_FADE_MS - fade_elapsed;
    return (uint8_t)((remaining * 255u + WINXTERM_COPY_OVERLAY_FADE_MS / 2u) /
                     WINXTERM_COPY_OVERLAY_FADE_MS);
}

static bool winxterm_app_copy_overlay_row_span(const WinxtermScreenSelectionRange *range,
                                               size_t row,
                                               int columns,
                                               int *start_column,
                                               int *end_column)
{
    if (range == 0 || !range->enabled || columns <= 0 ||
        row < range->start_row || row > range->end_row ||
        start_column == 0 || end_column == 0) {
        return false;
    }

    int start = range->rectangular || row == range->start_row ? range->start_column : 0;
    int end = range->rectangular || row == range->end_row ? range->end_column : columns - 1;
    if (start < 0) {
        start = 0;
    }
    if (end >= columns) {
        end = columns - 1;
    }
    if (start > end) {
        return false;
    }
    *start_column = start;
    *end_column = end;
    return true;
}

static void winxterm_app_draw_copy_overlay_column_span(WinxtermApp *app,
                                                       int view_row,
                                                       int y_offset,
                                                       int start_column,
                                                       int end_column,
                                                       uint8_t alpha)
{
    if (app == 0 || app->back_pixels == 0 || start_column > end_column) {
        return;
    }
    int x = start_column * WINXTERM_CELL_WIDTH_PIXELS;
    int y = view_row * WINXTERM_CELL_HEIGHT_PIXELS + y_offset;
    int width = (end_column - start_column + 1) * WINXTERM_CELL_WIDTH_PIXELS;
    winxterm_render_blend_rect_outline(app->back_pixels,
                                       app->bitmap_width,
                                       app->bitmap_height,
                                       x,
                                       y,
                                       width,
                                       1,
                                       WINXTERM_COPY_OVERLAY_RGB,
                                       alpha);
}

static void winxterm_app_draw_copy_overlay_uncovered_span(WinxtermApp *app,
                                                          int view_row,
                                                          int y_offset,
                                                          int start_column,
                                                          int end_column,
                                                          bool cover_valid,
                                                          int cover_start,
                                                          int cover_end,
                                                          uint8_t alpha)
{
    if (!cover_valid) {
        winxterm_app_draw_copy_overlay_column_span(app,
                                                  view_row,
                                                  y_offset,
                                                  start_column,
                                                  end_column,
                                                  alpha);
        return;
    }
    if (start_column < cover_start) {
        int left_end = end_column < cover_start - 1 ? end_column : cover_start - 1;
        winxterm_app_draw_copy_overlay_column_span(app,
                                                  view_row,
                                                  y_offset,
                                                  start_column,
                                                  left_end,
                                                  alpha);
    }
    if (end_column > cover_end) {
        int right_start = start_column > cover_end + 1 ? start_column : cover_end + 1;
        winxterm_app_draw_copy_overlay_column_span(app,
                                                  view_row,
                                                  y_offset,
                                                  right_start,
                                                  end_column,
                                                  alpha);
    }
}

static void winxterm_app_draw_copy_overlay_vertical_edge(WinxtermApp *app,
                                                        int view_row,
                                                        int column,
                                                        bool right_edge,
                                                        uint8_t alpha)
{
    if (app == 0 || app->back_pixels == 0) {
        return;
    }
    int x = right_edge ?
        (column + 1) * WINXTERM_CELL_WIDTH_PIXELS - 1 :
        column * WINXTERM_CELL_WIDTH_PIXELS;
    int y = view_row * WINXTERM_CELL_HEIGHT_PIXELS;
    winxterm_render_blend_rect_outline(app->back_pixels,
                                       app->bitmap_width,
                                       app->bitmap_height,
                                       x,
                                       y,
                                       1,
                                       WINXTERM_CELL_HEIGHT_PIXELS,
                                       WINXTERM_COPY_OVERLAY_RGB,
                                       alpha);
}

static void winxterm_app_draw_click_preview_row(WinxtermApp *app,
                                                int view_row,
                                                int columns)
{
    if (app == 0 || app->back_pixels == 0 || columns <= 0) {
        return;
    }
    int y = view_row * WINXTERM_CELL_HEIGHT_PIXELS + WINXTERM_CELL_HEIGHT_PIXELS / 2;
    winxterm_render_blend_rect_outline(app->back_pixels,
                                       app->bitmap_width,
                                       app->bitmap_height,
                                       0,
                                       y,
                                       columns * WINXTERM_CELL_WIDTH_PIXELS,
                                       1,
                                       WINXTERM_HOVER_OUTLINE_RGB,
                                       WINXTERM_CLICK_PREVIEW_ALPHA);
}

static void winxterm_app_draw_click_preview_column_span(WinxtermApp *app,
                                                        int view_row,
                                                        int y_offset,
                                                        int start_column,
                                                        int end_column)
{
    if (app == 0 || app->back_pixels == 0 || start_column > end_column) {
        return;
    }
    int x = start_column * WINXTERM_CELL_WIDTH_PIXELS;
    int y = view_row * WINXTERM_CELL_HEIGHT_PIXELS + y_offset;
    int width = (end_column - start_column + 1) * WINXTERM_CELL_WIDTH_PIXELS;
    winxterm_render_blend_rect_outline(app->back_pixels,
                                       app->bitmap_width,
                                       app->bitmap_height,
                                       x,
                                       y,
                                       width,
                                       1,
                                       WINXTERM_HOVER_OUTLINE_RGB,
                                       WINXTERM_CLICK_PREVIEW_ALPHA);
}

static void winxterm_app_draw_click_preview_vertical_edge(WinxtermApp *app,
                                                         int view_row,
                                                         int column,
                                                         bool right_edge)
{
    if (app == 0 || app->back_pixels == 0) {
        return;
    }
    int x = right_edge ?
        (column + 1) * WINXTERM_CELL_WIDTH_PIXELS - 1 :
        column * WINXTERM_CELL_WIDTH_PIXELS;
    int y = view_row * WINXTERM_CELL_HEIGHT_PIXELS;
    winxterm_render_blend_rect_outline(app->back_pixels,
                                       app->bitmap_width,
                                       app->bitmap_height,
                                       x,
                                       y,
                                       1,
                                       WINXTERM_CELL_HEIGHT_PIXELS,
                                       WINXTERM_HOVER_OUTLINE_RGB,
                                       WINXTERM_CLICK_PREVIEW_ALPHA);
}

static void winxterm_app_draw_click_preview_uncovered_span(WinxtermApp *app,
                                                           int view_row,
                                                           int y_offset,
                                                           int start_column,
                                                           int end_column,
                                                           bool cover_valid,
                                                           int cover_start,
                                                           int cover_end)
{
    if (!cover_valid) {
        winxterm_app_draw_click_preview_column_span(app,
                                                   view_row,
                                                   y_offset,
                                                   start_column,
                                                   end_column);
        return;
    }
    if (start_column < cover_start) {
        int left_end = end_column < cover_start - 1 ? end_column : cover_start - 1;
        winxterm_app_draw_click_preview_column_span(app,
                                                   view_row,
                                                   y_offset,
                                                   start_column,
                                                   left_end);
    }
    if (end_column > cover_end) {
        int right_start = start_column > cover_end + 1 ? start_column : cover_end + 1;
        winxterm_app_draw_click_preview_column_span(app,
                                                   view_row,
                                                   y_offset,
                                                   right_start,
                                                   end_column);
    }
}

static void winxterm_app_draw_click_preview(WinxtermApp *app,
                                            const WinxtermScreenRenderSnapshot *snapshot)
{
    if (app == 0 || snapshot == 0 || app->back_pixels == 0 ||
        app->click_preview_kind == WINXTERM_CLICK_PREVIEW_NONE ||
        !app->click_preview_range.enabled ||
        app->click_preview_range.alternate != snapshot->alternate_active ||
        snapshot->columns <= 0 || snapshot->rows <= 0) {
        return;
    }

    const WinxtermScreenSelectionRange *range = &app->click_preview_range;
    size_t visible_first = snapshot->first_row + (size_t)snapshot->row_offset;
    size_t visible_last = visible_first + (size_t)(snapshot->rows - 1);
    if (range->end_row < visible_first || range->start_row > visible_last) {
        return;
    }

    if (app->click_preview_kind == WINXTERM_CLICK_PREVIEW_ROW) {
        int view_row = (int)(range->start_row - snapshot->first_row);
        winxterm_app_draw_click_preview_row(app, view_row, snapshot->columns);
        return;
    }

    size_t start_row = range->start_row > visible_first ? range->start_row : visible_first;
    size_t end_row = range->end_row < visible_last ? range->end_row : visible_last;
    for (size_t row = start_row;; ++row) {
        int start_column = 0;
        int end_column = 0;
        if (winxterm_app_copy_overlay_row_span(range,
                                               row,
                                               snapshot->columns,
                                               &start_column,
                                               &end_column)) {
            int view_row = (int)(row - snapshot->first_row);
            int previous_start = 0;
            int previous_end = 0;
            bool previous_valid =
                row > 0u &&
                winxterm_app_copy_overlay_row_span(range,
                                                   row - 1u,
                                                   snapshot->columns,
                                                   &previous_start,
                                                   &previous_end);
            int next_start = 0;
            int next_end = 0;
            bool next_valid =
                row < SIZE_MAX &&
                winxterm_app_copy_overlay_row_span(range,
                                                   row + 1u,
                                                   snapshot->columns,
                                                   &next_start,
                                                   &next_end);

            winxterm_app_draw_click_preview_uncovered_span(app,
                                                           view_row,
                                                           0,
                                                           start_column,
                                                           end_column,
                                                           previous_valid,
                                                           previous_start,
                                                           previous_end);
            winxterm_app_draw_click_preview_uncovered_span(app,
                                                           view_row,
                                                           WINXTERM_CELL_HEIGHT_PIXELS - 1,
                                                           start_column,
                                                           end_column,
                                                           next_valid,
                                                           next_start,
                                                           next_end);
            if (app->click_preview_kind == WINXTERM_CLICK_PREVIEW_BOX) {
                winxterm_app_draw_click_preview_vertical_edge(app, view_row, start_column, false);
                winxterm_app_draw_click_preview_vertical_edge(app, view_row, end_column, true);
            }
        }
        if (row == end_row) {
            break;
        }
    }
}

static void winxterm_app_draw_copy_overlay(WinxtermApp *app,
                                           const WinxtermScreenRenderSnapshot *snapshot)
{
    if (app == 0 || snapshot == 0 || app->back_pixels == 0 ||
        snapshot->columns <= 0 || snapshot->rows <= 0) {
        return;
    }
    DWORD now = GetTickCount();
    uint8_t alpha = winxterm_app_copy_overlay_alpha(app, now);
    if (alpha == 0u || app->copy_overlay_range.alternate != snapshot->alternate_active) {
        return;
    }

    const WinxtermScreenSelectionRange *range = &app->copy_overlay_range;
    size_t visible_first = snapshot->first_row + (size_t)snapshot->row_offset;
    size_t visible_last = visible_first + (size_t)(snapshot->rows - 1);
    if (range->end_row < visible_first || range->start_row > visible_last) {
        return;
    }

    size_t start_row = range->start_row > visible_first ? range->start_row : visible_first;
    size_t end_row = range->end_row < visible_last ? range->end_row : visible_last;
    for (size_t row = start_row;; ++row) {
        int start_column = 0;
        int end_column = 0;
        if (winxterm_app_copy_overlay_row_span(range,
                                               row,
                                               snapshot->columns,
                                               &start_column,
                                               &end_column)) {
            int view_row = (int)(row - snapshot->first_row);
            int previous_start = 0;
            int previous_end = 0;
            bool previous_valid =
                row > 0u &&
                winxterm_app_copy_overlay_row_span(range,
                                                   row - 1u,
                                                   snapshot->columns,
                                                   &previous_start,
                                                   &previous_end);
            int next_start = 0;
            int next_end = 0;
            bool next_valid =
                row < SIZE_MAX &&
                winxterm_app_copy_overlay_row_span(range,
                                                   row + 1u,
                                                   snapshot->columns,
                                                   &next_start,
                                                   &next_end);

            winxterm_app_draw_copy_overlay_uncovered_span(app,
                                                          view_row,
                                                          0,
                                                          start_column,
                                                          end_column,
                                                          previous_valid,
                                                          previous_start,
                                                          previous_end,
                                                          alpha);
            winxterm_app_draw_copy_overlay_uncovered_span(app,
                                                          view_row,
                                                          WINXTERM_CELL_HEIGHT_PIXELS - 1,
                                                          start_column,
                                                          end_column,
                                                          next_valid,
                                                          next_start,
                                                          next_end,
                                                          alpha);
            winxterm_app_draw_copy_overlay_vertical_edge(app, view_row, start_column, false, alpha);
            winxterm_app_draw_copy_overlay_vertical_edge(app, view_row, end_column, true, alpha);
        }
        if (row == end_row) {
            break;
        }
    }
}

static bool winxterm_app_try_render_scroll_frame(WinxtermApp *app,
                                                 const WinxtermScreenRenderSnapshot *snapshot)
{
    if (app == 0 || snapshot == 0 || app->front_pixels == 0 || app->back_pixels == 0 ||
        app->bridge == 0 || !app->render_damage_valid || snapshot->alternate_active ||
        !app->ux.viewport.follow_output || winxterm_ux_has_selection(&app->ux) || app->hover_valid ||
        app->copy_overlay_active || app->click_preview_kind != WINXTERM_CLICK_PREVIEW_NONE) {
        return false;
    }

    const WinxtermScreenDamage *damage = &app->render_damage;
    if (damage->full_repaint || damage->scroll_delta <= 0 || damage->scroll_delta >= snapshot->rows) {
        return false;
    }

    int top = -1;
    int bottom = -1;
    int exposed_top = snapshot->rows - damage->scroll_delta;
    winxterm_app_include_render_row_range(&top, &bottom, exposed_top, snapshot->rows - 1);

    if (damage->dirty) {
        size_t global_top = app->bridge->screen.scrollback_count + (size_t)damage->dirty_top;
        size_t global_bottom = app->bridge->screen.scrollback_count + (size_t)damage->dirty_bottom;
        size_t visible_top = snapshot->first_row + (size_t)snapshot->row_offset;
        size_t visible_bottom = visible_top + (size_t)snapshot->rows - 1u;
        if (global_bottom >= visible_top && global_top <= visible_bottom) {
            size_t clipped_top = global_top > visible_top ? global_top : visible_top;
            size_t clipped_bottom = global_bottom < visible_bottom ? global_bottom : visible_bottom;
            winxterm_app_include_render_row_range(&top,
                                                  &bottom,
                                                  (int)(clipped_top - visible_top),
                                                  (int)(clipped_bottom - visible_top));
        }
    }

    if (app->rendered_cursor_valid) {
        int moved_cursor_row = app->rendered_cursor_row - damage->scroll_delta;
        if (moved_cursor_row >= 0 && moved_cursor_row < snapshot->rows) {
            winxterm_app_include_render_row(&top, &bottom, moved_cursor_row);
        }
    }

    int cursor_row = 0;
    if (winxterm_app_snapshot_cursor_local_row(snapshot, &cursor_row)) {
        winxterm_app_include_render_row(&top, &bottom, cursor_row);
    }

    if (top < 0 || bottom < top || bottom >= snapshot->rows) {
        return false;
    }
    int row_count = bottom - top + 1;
    if (row_count > snapshot->rows / 2) {
        return false;
    }

    winxterm_render_scroll_lines(app->front_pixels,
                                 app->back_pixels,
                                 app->bitmap_width,
                                 app->bitmap_height,
                                 damage->scroll_delta,
                                 snapshot->clear_rgb);
    winxterm_screen_render_snapshot_range(snapshot,
                                          &app->render_context,
                                          app->back_pixels,
                                          top,
                                          row_count);
    return true;
}

static void winxterm_app_render_main_area(WinxtermApp *app)
{
    if (app == 0 || app->bridge == 0 || app->front_pixels == 0 || app->back_pixels == 0 || app->bitmap_width <= 0 ||
        app->bitmap_height <= 0) {
        return;
    }
    app->render_invalid_full = false;

    uint64_t worker_total_ns = 0u;
    uint64_t worker_max_ns = 0u;
    WinxtermScreenRenderSnapshot snapshot;
    memset(&snapshot, 0, sizeof(snapshot));

    EnterCriticalSection(&app->bridge->screen_lock);
    WinxtermCellSize visible = winxterm_app_visible_cells_locked(app);
    winxterm_ux_note_screen_changed_for_rows(&app->ux, &app->bridge->screen, visible.rows);
    WinxtermScreenRenderView view;
    memset(&view, 0, sizeof(view));
    view.primary_first_row =
        winxterm_ux_primary_first_row_for_rows(&app->ux, &app->bridge->screen, visible.rows);
    view.selection = winxterm_ux_render_selection(&app->ux, &app->bridge->screen);
    bool snapshot_ready = winxterm_screen_render_snapshot_init(&snapshot,
                                                              &app->bridge->screen,
                                                              app->bitmap_width,
                                                              app->bitmap_height,
                                                              winxterm_bridge_backend(app->bridge),
                                                              app->cursor_visible,
                                                              &view);
    LeaveCriticalSection(&app->bridge->screen_lock);

    if (!snapshot_ready) {
        winxterm_log_writef(app->log, "render snapshot allocation failed");
        return;
    }

    bool used_scroll_blit = winxterm_app_try_render_scroll_frame(app, &snapshot);
    if (!used_scroll_blit &&
        !winxterm_render_worker_pool_render(app->render_workers,
                                            &snapshot,
                                            app->back_pixels,
                                            &worker_total_ns,
                                            &worker_max_ns)) {
        winxterm_screen_render_snapshot(&snapshot, &app->render_context, app->back_pixels);
    }
    winxterm_app_note_rendered_cursor(app, &snapshot);
    winxterm_app_draw_copy_overlay(app, &snapshot);
    winxterm_app_draw_click_preview(app, &snapshot);
    winxterm_app_draw_hover_outline(app);
    winxterm_screen_render_snapshot_dispose(&snapshot);

    winxterm_render_swap(&app->front_pixels, &app->back_pixels);
    app->render_invalid_full = true;
    app->render_damage_valid = false;
    winxterm_bridge_note_frame(app->bridge);
    if (!winxterm_bridge_bell_enabled(app->bridge) && app->ux.bell.active) {
        app->ux.bell.active = false;
        if (app->hwnd != 0 && app->bell_timer_id != 0u) {
            KillTimer(app->hwnd, app->bell_timer_id);
            app->bell_timer_id = 0u;
        }
    }
    if (winxterm_bridge_take_bell(app->bridge)) {
        winxterm_ux_start_bell(&app->ux, GetTickCount());
        if (app->bell_timer_id == 0u && app->hwnd != 0) {
            app->bell_timer_id = SetTimer(app->hwnd,
                                          WINXTERM_BELL_TIMER_ID,
                                          WINXTERM_UX_BELL_TIMER_MS,
                                          0);
        }
    }
    winxterm_app_update_title(app);
}

static void winxterm_app_invalidate_rendered_area(WinxtermApp *app, HWND hwnd)
{
    if (app == 0 || hwnd == 0) {
        return;
    }
    if (app->render_invalid_full) {
        InvalidateRect(hwnd, 0, FALSE);
        app->render_present_pending = true;
    }
    app->render_invalid_full = false;
}

static void winxterm_app_log_client_size(WinxtermApp *app, const char *event_name)
{
    if (app == 0 || app->hwnd == 0) {
        return;
    }

    RECT client;
    if (!GetClientRect(app->hwnd, &client)) {
        return;
    }

    int width = client.right - client.left;
    int height = client.bottom - client.top;
    int logical_width = winxterm_app_logical_width_from_client(app, width);
    int logical_height = winxterm_app_logical_height_from_client(app, height);
    WinxtermCellSize cells = winxterm_pixels_to_cells(logical_width, logical_height);
    winxterm_log_writef(app->log,
                        "%s: main_area=%dx%d pixels, logical=%dx%d pixels, scale=%u, chars=%dx%d",
                        event_name,
                        width,
                        height,
                        logical_width,
                        logical_height,
                        winxterm_app_display_scale(app),
                        cells.columns,
                        cells.rows);
}

static bool winxterm_app_register_class(HINSTANCE instance)
{
    HICON icon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_WINXTERM));
    if (icon == 0) {
        icon = LoadIconW(0, IDI_APPLICATION);
    }

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.style = CS_OWNDC | CS_DBLCLKS;
    wc.lpfnWndProc = winxterm_window_proc;
    wc.hInstance = instance;
    wc.hIcon = icon;
    wc.hIconSm = icon;
    wc.hCursor = LoadCursorW(0, IDC_ARROW);
    wc.hbrBackground = 0;
    wc.lpszClassName = WINXTERM_WINDOW_CLASS_NAME;

    ATOM atom = RegisterClassExW(&wc);
    if (atom == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    return true;
}

bool winxterm_app_init(WinxtermApp *app,
                       HINSTANCE instance,
                       WinxtermLog *log,
                       WinxtermBridge *bridge,
                       HANDLE shutdown_event,
                       unsigned int display_scale,
                       unsigned int render_thread_count)
{
    if (app == 0) {
        return false;
    }

    memset(app, 0, sizeof(*app));
    app->instance = instance;
    app->log = log;
    app->bridge = bridge;
    app->shutdown_event = shutdown_event;
    app->display_scale = winxterm_display_scale_valid(display_scale) ?
        display_scale : WINXTERM_DEFAULT_DISPLAY_SCALE;
    app->render_thread_count = winxterm_app_clamp_render_thread_count(render_thread_count);
    app->windowed_placement.length = sizeof(app->windowed_placement);
    app->last_size_kind = SIZE_RESTORED;
    app->cursor_visible = true;
    WinxtermSettings settings;
    winxterm_settings_init(&settings);
    (void)winxterm_settings_load(&settings);
    app->scrollbar_enabled = settings.scrollbar;
    winxterm_ux_init(&app->ux);
    app->cursor_blink_ms = GetCaretBlinkTime();
    if (app->cursor_blink_ms == 0u || app->cursor_blink_ms == INFINITE) {
        app->cursor_blink_ms = WINXTERM_DEFAULT_CURSOR_BLINK_MS;
    }
    winxterm_render_context_init(&app->render_context);
    (void)winxterm_render_context_load_fallback_fonts(&app->render_context, instance, log);
    if (!winxterm_macro_create(&app->macro)) {
        winxterm_log_writef(log, "failed to initialize macro runtime");
        return false;
    }

    SetProcessDPIAware();

    if (!winxterm_app_register_class(instance)) {
        winxterm_log_writef(log, "failed to register window class");
        return false;
    }

    DWORD style = WS_OVERLAPPEDWINDOW;
    if (app->scrollbar_enabled) {
        style |= WS_VSCROLL;
    }
    DWORD ex_style = 0;
    RECT window_rect = {0,
                        0,
                        winxterm_logical_to_physical_pixels(WINXTERM_INITIAL_PIXEL_WIDTH,
                                                            app->display_scale),
                        winxterm_logical_to_physical_pixels(WINXTERM_INITIAL_PIXEL_HEIGHT,
                                                            app->display_scale)};
    if (!winxterm_app_adjust_window_rect(&window_rect, style, ex_style)) {
        winxterm_log_writef(log, "failed to calculate initial window rectangle");
        return false;
    }

    app->previous_focus_hwnd = GetForegroundWindow();
    HWND hwnd = CreateWindowExW(ex_style,
                                WINXTERM_WINDOW_CLASS_NAME,
                                WINXTERM_WINDOW_TITLE,
                                style,
                                CW_USEDEFAULT,
                                CW_USEDEFAULT,
                                window_rect.right - window_rect.left,
                                window_rect.bottom - window_rect.top,
                                0,
                                0,
                                instance,
                                app);

    if (hwnd == 0) {
        winxterm_log_writef(log, "failed to create main window, error=%lu", (unsigned long)GetLastError());
        return false;
    }

    winxterm_bridge_set_hwnd(bridge, hwnd);
    app->render_workers =
        winxterm_render_worker_pool_create(instance, log, app->render_thread_count);
    if (app->render_workers == 0) {
        winxterm_log_writef(log,
                            "render worker pool unavailable; using UI-thread renderer");
    } else {
        winxterm_log_writef(log,
                            "render worker pool started threads=%u",
                            app->render_workers->worker_count);
    }
    app->cursor_timer_id = SetTimer(hwnd, WINXTERM_CURSOR_TIMER_ID, app->cursor_blink_ms, 0);
    if (app->cursor_timer_id == 0u) {
        winxterm_log_writef(log, "cursor blink timer unavailable");
    }
    winxterm_app_update_scrollbar(app);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    winxterm_window_placement_apply_startup(hwnd,
                                            window_rect.right - window_rect.left,
                                            window_rect.bottom - window_rect.top,
                                            log);
    winxterm_app_log_client_size(app, "startup");
    return true;
}

typedef struct WinxtermFocusFallback {
    HWND self;
    HWND best;
    DWORD best_process_id;
    unsigned int z_order;
    unsigned int best_z_order;
} WinxtermFocusFallback;

static bool winxterm_app_window_class_is(HWND hwnd, const wchar_t *expected)
{
    if (hwnd == 0 || expected == 0) {
        return false;
    }

    wchar_t class_name[128];
    int length = GetClassNameW(hwnd, class_name, (int)(sizeof(class_name) / sizeof(class_name[0])));
    return length > 0 && wcscmp(class_name, expected) == 0;
}

static bool winxterm_app_focus_target_is_shell_surface(HWND hwnd)
{
    return winxterm_app_window_class_is(hwnd, L"Shell_TrayWnd") ||
           winxterm_app_window_class_is(hwnd, L"Shell_SecondaryTrayWnd") ||
           winxterm_app_window_class_is(hwnd, L"Progman") ||
           winxterm_app_window_class_is(hwnd, L"WorkerW");
}

static bool winxterm_app_previous_focus_target_valid(WinxtermApp *app)
{
    return app != 0 &&
           app->previous_focus_hwnd != 0 &&
           app->previous_focus_hwnd != app->hwnd &&
           IsWindow(app->previous_focus_hwnd) &&
           IsWindowVisible(app->previous_focus_hwnd) &&
           IsWindowEnabled(app->previous_focus_hwnd) &&
           !winxterm_app_focus_target_is_shell_surface(app->previous_focus_hwnd);
}

static BOOL CALLBACK winxterm_app_enum_focus_fallback(HWND hwnd, LPARAM lparam)
{
    WinxtermFocusFallback *fallback = (WinxtermFocusFallback *)lparam;
    if (fallback == 0) {
        return TRUE;
    }
    ++fallback->z_order;
    if (hwnd == fallback->self ||
        !IsWindowVisible(hwnd) ||
        !IsWindowEnabled(hwnd) ||
        !winxterm_app_window_class_is(hwnd, WINXTERM_WINDOW_CLASS_NAME)) {
        return TRUE;
    }

    DWORD process_id = 0;
    GetWindowThreadProcessId(hwnd, &process_id);
    if (fallback->best == 0 ||
        process_id > fallback->best_process_id ||
        (process_id == fallback->best_process_id && fallback->z_order < fallback->best_z_order)) {
        fallback->best = hwnd;
        fallback->best_process_id = process_id;
        fallback->best_z_order = fallback->z_order;
    }
    return TRUE;
}

static HWND winxterm_app_focus_fallback_target(HWND self)
{
    WinxtermFocusFallback fallback;
    memset(&fallback, 0, sizeof(fallback));
    fallback.self = self;
    fallback.best_z_order = UINT_MAX;
    (void)EnumWindows(winxterm_app_enum_focus_fallback, (LPARAM)&fallback);
    return fallback.best;
}

static void winxterm_app_restore_previous_focus(WinxtermApp *app)
{
    if (app == 0) {
        return;
    }

    HWND foreground = GetForegroundWindow();
    if (foreground != 0 && foreground != app->hwnd) {
        return;
    }

    HWND target = winxterm_app_previous_focus_target_valid(app) ?
        app->previous_focus_hwnd : winxterm_app_focus_fallback_target(app->hwnd);
    if (target == 0 || target == app->hwnd || !IsWindow(target)) {
        return;
    }

    if (IsIconic(target)) {
        ShowWindow(target, SW_RESTORE);
    }
    BringWindowToTop(target);
    (void)SetForegroundWindow(target);
}

int winxterm_app_run(WinxtermApp *app)
{
    if (app == 0 || app->hwnd == 0) {
        return 1;
    }

    MSG message;
    while (true) {
        BOOL result = GetMessageW(&message, 0, 0, 0);
        if (result == -1) {
            winxterm_log_writef(app->log, "message loop failed, error=%lu", (unsigned long)GetLastError());
            return 1;
        }
        if (result == 0) {
            return (int)message.wParam;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

void winxterm_app_dispose(WinxtermApp *app)
{
    if (app == 0) {
        return;
    }

    winxterm_render_worker_pool_destroy(app->render_workers);
    app->render_workers = 0;
    winxterm_render_context_dispose(&app->render_context);
    if (app->hwnd != 0 && app->cursor_timer_id != 0u) {
        KillTimer(app->hwnd, app->cursor_timer_id);
        app->cursor_timer_id = 0u;
    }
    if (app->hwnd != 0 && app->bell_timer_id != 0u) {
        KillTimer(app->hwnd, app->bell_timer_id);
        app->bell_timer_id = 0u;
    }
    if (app->hwnd != 0 && app->frame_timer_id != 0u) {
        KillTimer(app->hwnd, app->frame_timer_id);
        app->frame_timer_id = 0u;
    }
    if (app->hwnd != 0 && app->macro_timer_id != 0u) {
        KillTimer(app->hwnd, app->macro_timer_id);
        app->macro_timer_id = 0u;
    }
    if (app->hwnd != 0 && app->click_event_timer_id != 0u) {
        KillTimer(app->hwnd, app->click_event_timer_id);
        app->click_event_timer_id = 0u;
        app->click_event_requested_handler = false;
    }
    if (app->hwnd != 0 && app->copy_overlay_timer_id != 0u) {
        KillTimer(app->hwnd, app->copy_overlay_timer_id);
        app->copy_overlay_timer_id = 0u;
    }
    winxterm_macro_destroy(app->macro);
    app->macro = 0;
    while (app->session_ux != 0) {
        WinxtermAppSessionUx *next = app->session_ux->next;
        free(app->session_ux);
        app->session_ux = next;
    }
    if (app->bridge != 0) {
        winxterm_bridge_set_hwnd(app->bridge, 0);
    }
    winxterm_app_free_buffers(app);
    app->hwnd = 0;
    app->log = 0;
    app->bridge = 0;
}

static bool winxterm_app_commit_pending_resize(WinxtermApp *app)
{
    if (app == 0 || !app->pending_resize) {
        return false;
    }
    RECT client;
    if (!GetClientRect(app->hwnd, &client)) {
        return false;
    }

    int width = client.right - client.left;
    int height = client.bottom - client.top;
    int logical_width = winxterm_app_logical_width_from_client(app, width);
    int logical_height = winxterm_app_logical_height_from_client(app, height);
    WPARAM size_kind = app->pending_resize_kind;
    const char *event_name = "resize";
    if (size_kind == SIZE_MINIMIZED) {
        event_name = "minimize";
    } else if (size_kind == SIZE_MAXIMIZED) {
        event_name = "maximize";
    } else if (app->last_size_kind == SIZE_MINIMIZED || app->last_size_kind == SIZE_MAXIMIZED) {
        event_name = "restore";
    }

    bool scale_anchor = false;
    EnterCriticalSection(&app->bridge->screen_lock);
    WinxtermCellSize old_visible = winxterm_app_visible_cells_locked(app);
    scale_anchor = app->scale_resize_anchor_valid;
    if (!scale_anchor) {
        winxterm_ux_capture_resize_anchor_for_rows(&app->ux, &app->bridge->screen, old_visible.rows);
    }
    LeaveCriticalSection(&app->bridge->screen_lock);

    if (!winxterm_app_resize_bitmap(app, logical_width, logical_height)) {
        winxterm_log_writef(app->log,
                            "failed to allocate bitmap for %dx%d logical area",
                            logical_width,
                            logical_height);
        DestroyWindow(app->hwnd);
        app->pending_resize = false;
        return false;
    }

    WinxtermCellSize cells = winxterm_pixels_to_cells(logical_width, logical_height);
    if (size_kind == SIZE_MINIMIZED ||
        width <= 0 ||
        height <= 0 ||
        cells.columns < WINXTERM_MIN_COLUMNS ||
        cells.rows < WINXTERM_MIN_ROWS) {
        EnterCriticalSection(&app->bridge->screen_lock);
        WinxtermCellSize visible = winxterm_app_visible_cells_locked(app);
        if (scale_anchor) {
            winxterm_app_restore_scale_anchor_locked(app, visible.rows);
        } else {
            winxterm_ux_restore_resize_anchor_for_rows(&app->ux,
                                                       &app->bridge->screen,
                                                       visible.rows);
        }
        app->scale_resize_anchor_valid = false;
        LeaveCriticalSection(&app->bridge->screen_lock);
        winxterm_app_log_client_size(app, event_name);
        app->last_size_kind = size_kind;
        app->pending_resize = false;
        return true;
    }

    int columns = cells.columns > 0 ? cells.columns : 1;
    int rows = cells.rows > 0 ? cells.rows : 1;
    EnterCriticalSection(&app->bridge->screen_lock);
    winxterm_screen_resize(&app->bridge->screen, columns, rows);
    WinxtermCellSize visible = winxterm_app_visible_cells_locked(app);
    if (scale_anchor) {
        winxterm_app_restore_scale_anchor_locked(app, visible.rows);
    } else {
        winxterm_ux_restore_resize_anchor_for_rows(&app->ux,
                                                   &app->bridge->screen,
                                                   visible.rows);
    }
    LeaveCriticalSection(&app->bridge->screen_lock);
    winxterm_bridge_queue_terminal_resize(app->bridge, columns, rows);

    winxterm_app_log_client_size(app, event_name);
    app->last_size_kind = size_kind;
    app->pending_resize = false;
    return true;
}

static void winxterm_app_handle_size(WinxtermApp *app, WPARAM size_kind)
{
    if (app == 0) {
        return;
    }
    app->pending_resize = true;
    app->pending_resize_kind = size_kind;
    winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_RESIZE);
}

static WinxtermInputModifiers winxterm_app_modifiers_from_keyboard(void)
{
    WinxtermInputModifiers modifiers;
    modifiers.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    modifiers.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    modifiers.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    return modifiers;
}

static bool winxterm_app_input_contains_line_submit(const uint8_t *bytes, size_t byte_count)
{
    if (bytes == 0) {
        return false;
    }
    for (size_t i = 0u; i < byte_count; ++i) {
        if (bytes[i] == '\r' || bytes[i] == '\n') {
            return true;
        }
    }
    return false;
}

static void winxterm_app_reset_cursor_blink(WinxtermApp *app)
{
    if (app == 0 || app->hwnd == 0) {
        return;
    }

    bool was_visible = app->cursor_visible;
    app->cursor_visible = true;
    if (app->cursor_timer_id != 0u) {
        KillTimer(app->hwnd, app->cursor_timer_id);
    }
    app->cursor_timer_id = SetTimer(app->hwnd, WINXTERM_CURSOR_TIMER_ID, app->cursor_blink_ms, 0);
    if (!was_visible) {
        winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
    }
}

static bool winxterm_app_queue_input(WinxtermApp *app, const uint8_t *bytes, size_t byte_count)
{
    if (app == 0 || app->bridge == 0 || byte_count == 0u) {
        return false;
    }
    bool line_submit = winxterm_app_input_contains_line_submit(bytes, byte_count);
    bool queued = winxterm_bridge_queue_input(app->bridge, bytes, byte_count);
    if (!queued) {
        winxterm_log_writef(app->log, "input queue full, dropped %zu bytes", byte_count);
        return false;
    }
    winxterm_app_reset_cursor_blink(app);
    if (line_submit) {
        winxterm_ux_scroll_to_bottom(&app->ux);
        winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
    }
    return true;
}

static bool winxterm_app_queue_input_chunks(WinxtermApp *app, const uint8_t *bytes, size_t byte_count)
{
    if (app == 0 || bytes == 0) {
        return false;
    }
    size_t offset = 0u;
    while (offset < byte_count) {
        size_t chunk = byte_count - offset;
        if (chunk > WINXTERM_PASTE_CHUNK_SIZE) {
            chunk = WINXTERM_PASTE_CHUNK_SIZE;
        }
        if (!winxterm_app_queue_input(app, bytes + offset, chunk)) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

static void winxterm_app_clear_copy_overlay(WinxtermApp *app, bool redraw)
{
    if (app == 0) {
        return;
    }
    bool was_active = app->copy_overlay_active;
    app->copy_overlay_active = false;
    app->copy_overlay_start_tick = 0u;
    memset(&app->copy_overlay_range, 0, sizeof(app->copy_overlay_range));
    if (app->hwnd != 0 && app->copy_overlay_timer_id != 0u) {
        KillTimer(app->hwnd, app->copy_overlay_timer_id);
        app->copy_overlay_timer_id = 0u;
    }
    if (redraw && was_active) {
        winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
    }
}

static void winxterm_app_start_copy_overlay(WinxtermApp *app,
                                            WinxtermScreenSelectionRange range)
{
    if (app == 0 || app->hwnd == 0 || !range.enabled) {
        return;
    }

    app->copy_overlay_active = true;
    app->copy_overlay_start_tick = GetTickCount();
    app->copy_overlay_range = range;
    if (app->copy_overlay_timer_id != 0u) {
        KillTimer(app->hwnd, app->copy_overlay_timer_id);
        app->copy_overlay_timer_id = 0u;
    }
    app->copy_overlay_timer_id = SetTimer(app->hwnd,
                                          WINXTERM_COPY_OVERLAY_TIMER_ID,
                                          WINXTERM_COPY_OVERLAY_FRAME_MS,
                                          0);
    if (app->copy_overlay_timer_id == 0u) {
        winxterm_log_writef(app->log, "copy overlay timer unavailable");
    }
    winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
}

static void winxterm_app_tick_copy_overlay(WinxtermApp *app)
{
    if (app == 0 || !app->copy_overlay_active) {
        return;
    }
    if (!winxterm_app_copy_overlay_visible(app, GetTickCount())) {
        winxterm_app_clear_copy_overlay(app, true);
        return;
    }
    winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
}

static size_t winxterm_app_click_position_global_row(const WinxtermScreen *screen,
                                                     WinxtermUxPosition position)
{
    size_t scrollback = winxterm_screen_scrollback_count(screen);
    switch (position.kind) {
    case WINXTERM_UX_ROW_PRIMARY_SCROLLBACK:
        return position.row;
    case WINXTERM_UX_ROW_PRIMARY_VISIBLE:
        return scrollback + position.row;
    case WINXTERM_UX_ROW_ALTERNATE_VISIBLE:
    default:
        return position.row;
    }
}

static void winxterm_app_clear_click_preview(WinxtermApp *app, bool redraw)
{
    if (app == 0) {
        return;
    }
    bool was_active = app->click_preview_kind != WINXTERM_CLICK_PREVIEW_NONE;
    app->click_preview_kind = WINXTERM_CLICK_PREVIEW_NONE;
    memset(&app->click_preview_range, 0, sizeof(app->click_preview_range));
    if (redraw && was_active) {
        winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
    }
}

static void winxterm_app_show_click_row_preview(WinxtermApp *app,
                                                WinxtermUxPosition position)
{
    if (app == 0 || app->bridge == 0) {
        return;
    }

    WinxtermScreenSelectionRange range;
    memset(&range, 0, sizeof(range));
    EnterCriticalSection(&app->bridge->screen_lock);
    size_t row = winxterm_app_click_position_global_row(&app->bridge->screen, position);
    range.enabled = true;
    range.alternate = position.kind == WINXTERM_UX_ROW_ALTERNATE_VISIBLE;
    range.start_row = row;
    range.end_row = row;
    LeaveCriticalSection(&app->bridge->screen_lock);

    app->click_preview_kind = WINXTERM_CLICK_PREVIEW_ROW;
    app->click_preview_range = range;
    winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
}

static void winxterm_app_show_click_range_preview(WinxtermApp *app,
                                                  WinxtermUxPosition position)
{
    if (app == 0 || app->bridge == 0) {
        return;
    }

    WinxtermScreenSelectionRange range;
    memset(&range, 0, sizeof(range));
    EnterCriticalSection(&app->bridge->screen_lock);
    WinxtermUxState preview_ux;
    memset(&preview_ux, 0, sizeof(preview_ux));
    if (winxterm_ux_select_non_space_run_at(&preview_ux, &app->bridge->screen, position)) {
        range = winxterm_ux_render_selection(&preview_ux, &app->bridge->screen);
    }
    LeaveCriticalSection(&app->bridge->screen_lock);

    if (!range.enabled) {
        winxterm_app_clear_click_preview(app, true);
        return;
    }
    app->click_preview_kind = WINXTERM_CLICK_PREVIEW_RANGE;
    app->click_preview_range = range;
    winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
}

static void winxterm_app_show_click_line_preview(WinxtermApp *app,
                                                 WinxtermUxPosition position)
{
    if (app == 0 || app->bridge == 0) {
        return;
    }

    WinxtermScreenSelectionRange range;
    memset(&range, 0, sizeof(range));
    EnterCriticalSection(&app->bridge->screen_lock);
    WinxtermUxState preview_ux;
    memset(&preview_ux, 0, sizeof(preview_ux));
    if (winxterm_ux_select_real_line_at(&preview_ux, &app->bridge->screen, position)) {
        range = winxterm_ux_render_selection(&preview_ux, &app->bridge->screen);
    }
    LeaveCriticalSection(&app->bridge->screen_lock);

    if (!range.enabled) {
        winxterm_app_clear_click_preview(app, true);
        return;
    }
    app->click_preview_kind = WINXTERM_CLICK_PREVIEW_BOX;
    app->click_preview_range = range;
    winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
}

static bool winxterm_app_copy_selection_format(WinxtermApp *app, WinxtermSelectionCopyFormat format)
{
    if (app == 0 || app->bridge == 0 || app->hwnd == 0 || !winxterm_ux_has_selection(&app->ux)) {
        return false;
    }

    char *text = 0;
    size_t length = 0u;
    WinxtermScreenSelectionRange copied_range;
    memset(&copied_range, 0, sizeof(copied_range));
    EnterCriticalSection(&app->bridge->screen_lock);
    copied_range = winxterm_ux_render_selection(&app->ux, &app->bridge->screen);
    bool ok = copied_range.enabled &&
              winxterm_ux_extract_selection_utf8_format(&app->ux,
                                                        &app->bridge->screen,
                                                        format,
                                                        &text,
                                                        &length);
    LeaveCriticalSection(&app->bridge->screen_lock);
    if (!ok || text == 0) {
        free(text);
        return false;
    }
    ok = winxterm_clipboard_set_text_utf8(app->hwnd, text, length);
    free(text);
    if (!ok) {
        winxterm_log_writef(app->log, "clipboard copy failed");
    } else {
        winxterm_app_start_copy_overlay(app, copied_range);
    }
    return ok;
}

static bool winxterm_app_copy_selection(WinxtermApp *app)
{
    return winxterm_app_copy_selection_format(app, WINXTERM_SELECTION_COPY_DEFAULT);
}

static bool winxterm_app_copy_selection_and_clear_format(WinxtermApp *app,
                                                         WinxtermSelectionCopyFormat format)
{
    bool ok = winxterm_app_copy_selection_format(app, format);
    if (app != 0 && app->bridge != 0) {
        EnterCriticalSection(&app->bridge->screen_lock);
        winxterm_ux_clear_selection(&app->ux);
        LeaveCriticalSection(&app->bridge->screen_lock);
        winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
    }
    return ok;
}

static bool winxterm_app_copy_selection_and_clear(WinxtermApp *app)
{
    bool ok = winxterm_app_copy_selection(app);
    if (app != 0 && app->bridge != 0) {
        EnterCriticalSection(&app->bridge->screen_lock);
        winxterm_ux_clear_selection(&app->ux);
        LeaveCriticalSection(&app->bridge->screen_lock);
        winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
    }
    return ok;
}

static bool winxterm_app_paste_clipboard(WinxtermApp *app)
{
    if (app == 0 || app->bridge == 0 || app->hwnd == 0) {
        return false;
    }
    char *text = 0;
    size_t length = 0u;
    if (!winxterm_clipboard_get_text_utf8(app->hwnd, &text, &length)) {
        return false;
    }

    WinxtermModeState modes;
    memset(&modes, 0, sizeof(modes));
    (void)winxterm_bridge_copy_mode_state(app->bridge, &modes);
    char *prepared = 0;
    size_t prepared_length = 0u;
    bool ok = winxterm_clipboard_prepare_paste(text,
                                               length,
                                               modes.bracketed_paste,
                                               &prepared,
                                               &prepared_length);
    free(text);
    if (!ok) {
        return false;
    }
    ok = winxterm_app_queue_input_chunks(app, (const uint8_t *)prepared, prepared_length);
    free(prepared);
    return ok;
}

static bool winxterm_app_damage_affects_visible(WinxtermApp *app, const WinxtermScreenDamage *damage)
{
    if (app == 0 || app->bridge == 0 || damage == 0) {
        return false;
    }
    if (damage->full_repaint) {
        return true;
    }
    WinxtermCellSize visible = winxterm_app_visible_cells_locked(app);
    if (visible.rows <= 0) {
        return false;
    }
    if (app->bridge->screen.alternate_active) {
        return damage->scroll_delta != 0 ||
               (damage->dirty &&
                damage->dirty_bottom >= 0 &&
                damage->dirty_top < visible.rows);
    }
    size_t first_row =
        winxterm_ux_primary_first_row_for_rows(&app->ux, &app->bridge->screen, visible.rows);
    size_t last_row = first_row + (size_t)visible.rows - 1u;
    if (damage->scroll_delta != 0) {
        size_t bottom_first_row =
            winxterm_screen_default_primary_first_row_for_rows(&app->bridge->screen,
                                                               visible.rows,
                                                               0u);
        if (first_row == bottom_first_row) {
            return true;
        }
    }
    if (!damage->dirty) {
        return false;
    }
    size_t global_top = app->bridge->screen.scrollback_count + (size_t)damage->dirty_top;
    size_t global_bottom = app->bridge->screen.scrollback_count + (size_t)damage->dirty_bottom;
    return global_bottom >= first_row && global_top <= last_row;
}

static bool winxterm_app_output_waiting_to_commit(WinxtermApp *app)
{
    if (app == 0 || app->bridge == 0) {
        return false;
    }

    bool pending = false;
    EnterCriticalSection(&app->bridge->input_lock);
    pending = !app->bridge->output_paused && app->bridge->output_count != 0u;
    LeaveCriticalSection(&app->bridge->input_lock);
    return pending;
}

static bool winxterm_app_commit_visible_changes(WinxtermApp *app, unsigned int causes)
{
    if (app == 0 || app->bridge == 0) {
        return false;
    }

    app->render_damage_valid = false;
    bool redraw = (causes & WINXTERM_FRAME_CAUSE_PRESENTATION) != 0u;
    if ((causes & WINXTERM_FRAME_CAUSE_RESIZE) != 0u || app->pending_resize) {
        redraw = winxterm_app_commit_pending_resize(app) || redraw;
    }

    bool output_more_pending = false;
    bool committed_content = false;
    bool presentation_changed = false;
    for (unsigned int pass = 0u; pass < WINXTERM_OUTPUT_COMMIT_MAX_PASSES; ++pass) {
        bool pass_content_changed = false;
        bool pass_more_pending = false;
        bool pass_presentation_changed = false;
        if (!winxterm_bridge_commit_output(app->bridge,
                                           WINXTERM_BRIDGE_OUTPUT_COMMIT_MAX_BYTES,
                                           &pass_content_changed,
                                           &pass_more_pending,
                                           &pass_presentation_changed)) {
            winxterm_log_writef(app->log, "output commit failed");
            output_more_pending = pass_more_pending;
            break;
        }
        committed_content = committed_content || pass_content_changed;
        presentation_changed = presentation_changed || pass_presentation_changed;
        output_more_pending = pass_more_pending;
        if (!output_more_pending) {
            break;
        }
    }
    if (presentation_changed) {
        redraw = true;
    }
    if (output_more_pending || winxterm_app_output_waiting_to_commit(app)) {
        winxterm_bridge_request_frame(app->bridge, WINXTERM_FRAME_CAUSE_CONTENT);
        return false;
    }

    EnterCriticalSection(&app->bridge->screen_lock);
    WinxtermCellSize visible = winxterm_app_visible_cells_locked(app);
    winxterm_ux_note_screen_changed_for_rows(&app->ux, &app->bridge->screen, visible.rows);
    WinxtermScreenDamage damage;
    bool has_damage = winxterm_screen_take_damage(&app->bridge->screen, &damage);
    if (committed_content && has_damage) {
        bool damage_affects_visible = winxterm_app_damage_affects_visible(app, &damage);
        if (damage_affects_visible) {
            app->render_damage = damage;
            app->render_damage_valid = true;
            redraw = true;
        }
    }
    LeaveCriticalSection(&app->bridge->screen_lock);
    return redraw;
}

static void winxterm_app_request_frame(WinxtermApp *app, unsigned int causes)
{
    if (app == 0 || app->hwnd == 0) {
        return;
    }
    winxterm_bridge_request_frame(app->bridge, causes);
}

static void winxterm_app_set_frame_timer(WinxtermApp *app, UINT delay_ms)
{
    if (app == 0 || app->hwnd == 0) {
        return;
    }
    if (delay_ms == 0u) {
        delay_ms = 1u;
    }
    app->frame_timer_id = SetTimer(app->hwnd, WINXTERM_FRAME_TIMER_ID, delay_ms, 0);
}

static void winxterm_app_handle_frame_due(WinxtermApp *app, HWND hwnd, unsigned int causes)
{
    if (app == 0 || hwnd == 0) {
        return;
    }
    app->deferred_frame_causes |= causes;
    DWORD now = GetTickCount();
    if (app->last_frame_tick != 0u) {
        DWORD elapsed = now - app->last_frame_tick;
        if (elapsed < WINXTERM_FRAME_THROTTLE_MS) {
            winxterm_app_set_frame_timer(app, WINXTERM_FRAME_THROTTLE_MS - elapsed);
            return;
        }
    }

    if (app->frame_timer_id != 0u) {
        KillTimer(hwnd, app->frame_timer_id);
        app->frame_timer_id = 0u;
    }
    causes = app->deferred_frame_causes;
    app->deferred_frame_causes = WINXTERM_FRAME_CAUSE_NONE;
    bool redraw = winxterm_app_commit_visible_changes(app, causes);
    winxterm_app_update_scrollbar(app);
    if (!redraw) {
        return;
    }
    winxterm_app_render_main_area(app);
    winxterm_app_invalidate_rendered_area(app, hwnd);
    app->last_frame_tick = GetTickCount();
}

static WinxtermAppSessionUx *winxterm_app_session_ux(WinxtermApp *app,
                                                     uint64_t session_id,
                                                     bool create)
{
    WinxtermAppSessionUx *entry = app != 0 ? app->session_ux : 0;
    while (entry != 0 && entry->session_id != session_id) entry = entry->next;
    if (entry != 0 || !create || app == 0 || session_id == 0u) return entry;
    entry = (WinxtermAppSessionUx *)calloc(1u, sizeof(*entry));
    if (entry == 0) return 0;
    entry->session_id = session_id;
    winxterm_ux_init(&entry->state);
    entry->next = app->session_ux;
    app->session_ux = entry;
    return entry;
}

static void winxterm_app_prune_session_ux(WinxtermApp *app, uint64_t keep_session_id)
{
    if (app == 0 || app->bridge == 0) return;
    WinxtermAppSessionUx **link = &app->session_ux;
    while (*link != 0) {
        WinxtermManagedJobSnapshot snapshot;
        if ((*link)->session_id != keep_session_id &&
            !winxterm_job_manager_snapshot_one(&app->bridge->job_manager,
                                               (*link)->session_id, &snapshot)) {
            WinxtermAppSessionUx *removed = *link;
            *link = removed->next;
            free(removed);
        } else {
            link = &(*link)->next;
        }
    }
}

static void winxterm_app_sync_active_session_ux(WinxtermApp *app)
{
    if (app == 0 || app->bridge == 0) return;
    uint64_t session_id = winxterm_bridge_active_session(app->bridge);
    if (session_id == 0u || session_id == app->ux_session_id) return;

    if (app->ux_session_id != 0u) {
        WinxtermAppSessionUx *old = winxterm_app_session_ux(
            app, app->ux_session_id, true);
        if (old != 0) {
            old->state = app->ux;
            old->state.selection.selecting = false;
        }
    }
    WinxtermAppSessionUx *target = winxterm_app_session_ux(app, session_id, true);
    if (target != 0) app->ux = target->state;
    else winxterm_ux_init(&app->ux);
    app->ux_session_id = session_id;
    winxterm_app_prune_session_ux(app, session_id);

    /* Pointer gestures and transient overlays refer to the old screen's row
       coordinates and must never leak into the newly presented session. */
    app->left_click_pending = false;
    app->hover_valid = false;
    app->copy_overlay_active = false;
    app->click_preview_kind = WINXTERM_CLICK_PREVIEW_NONE;
}

static void winxterm_app_handle_render_update(WinxtermApp *app, HWND hwnd)
{
    if (app == 0 || app->bridge == 0 || hwnd == 0) {
        return;
    }

    bool should_render = winxterm_bridge_take_render_update(app->bridge);
    MSG queued;
    while (PeekMessageW(&queued,
                        hwnd,
                        WINXTERM_WM_RENDER_UPDATE,
                        WINXTERM_WM_RENDER_UPDATE,
                        PM_REMOVE)) {
        if (winxterm_bridge_take_render_update(app->bridge)) {
            should_render = true;
        }
    }
    if (!should_render) {
        return;
    }

    winxterm_app_sync_active_session_ux(app);
    unsigned int causes = winxterm_bridge_take_frame_request(app->bridge);
    winxterm_app_handle_frame_due(app, hwnd, causes);
}

static void winxterm_app_scroll_lines(WinxtermApp *app, int lines_up)
{
    if (app == 0 || app->bridge == 0) {
        return;
    }
    EnterCriticalSection(&app->bridge->screen_lock);
    WinxtermCellSize visible = winxterm_app_visible_cells_locked(app);
    winxterm_ux_scroll_lines_for_rows(&app->ux,
                                      &app->bridge->screen,
                                      visible.rows,
                                      lines_up);
    LeaveCriticalSection(&app->bridge->screen_lock);
    winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
}

static void winxterm_app_scroll_page(WinxtermApp *app, int pages_up)
{
    if (app == 0 || app->bridge == 0) {
        return;
    }
    EnterCriticalSection(&app->bridge->screen_lock);
    WinxtermCellSize visible = winxterm_app_visible_cells_locked(app);
    winxterm_ux_scroll_page_for_rows(&app->ux,
                                     &app->bridge->screen,
                                     visible.rows,
                                     pages_up);
    LeaveCriticalSection(&app->bridge->screen_lock);
    winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
}

static void winxterm_app_scroll_home(WinxtermApp *app)
{
    if (app == 0 || app->bridge == 0) {
        return;
    }
    EnterCriticalSection(&app->bridge->screen_lock);
    WinxtermCellSize visible = winxterm_app_visible_cells_locked(app);
    winxterm_ux_scroll_to_top_for_rows(&app->ux, &app->bridge->screen, visible.rows);
    LeaveCriticalSection(&app->bridge->screen_lock);
    winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
}

static void winxterm_app_scroll_bottom(WinxtermApp *app)
{
    if (app == 0) {
        return;
    }
    winxterm_ux_scroll_to_bottom(&app->ux);
    winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
}

static void winxterm_app_update_scrollbar(WinxtermApp *app)
{
    if (app == 0 || app->hwnd == 0 || app->bridge == 0 || !app->scrollbar_enabled) {
        return;
    }

    size_t total_rows = 0u;
    size_t first_row = 0u;
    int view_rows = 0;
    EnterCriticalSection(&app->bridge->screen_lock);
    WinxtermCellSize visible = winxterm_app_visible_cells_locked(app);
    view_rows = visible.rows;
    if (!app->bridge->screen.alternate_active) {
        total_rows = winxterm_screen_primary_view_row_count(&app->bridge->screen);
        first_row = winxterm_ux_primary_first_row_for_rows(&app->ux,
                                                           &app->bridge->screen,
                                                           view_rows);
    }
    LeaveCriticalSection(&app->bridge->screen_lock);

    SCROLLINFO info;
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    info.fMask = SIF_RANGE | SIF_PAGE | SIF_POS | SIF_DISABLENOSCROLL;
    info.nMin = 0;
    info.nMax = total_rows > 0u ? (int)(total_rows - 1u) : 0;
    info.nPage = view_rows > 0 ? (UINT)view_rows : 1u;
    info.nPos = (int)first_row;
    (void)SetScrollInfo(app->hwnd, SB_VERT, &info, TRUE);
}

static void winxterm_app_scroll_to_row(WinxtermApp *app, int target_first_row)
{
    if (app == 0 || app->bridge == 0) {
        return;
    }
    if (target_first_row < 0) {
        target_first_row = 0;
    }
    EnterCriticalSection(&app->bridge->screen_lock);
    WinxtermCellSize visible = winxterm_app_visible_cells_locked(app);
    size_t bottom_first =
        winxterm_screen_default_primary_first_row_for_rows(&app->bridge->screen,
                                                           visible.rows,
                                                           0u);
    size_t offset = (size_t)target_first_row >= bottom_first ?
        0u : bottom_first - (size_t)target_first_row;
    winxterm_ux_scroll_to_offset_for_rows(&app->ux, &app->bridge->screen, visible.rows, offset);
    LeaveCriticalSection(&app->bridge->screen_lock);
    winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
}

static void winxterm_app_handle_vscroll(WinxtermApp *app, WPARAM wparam)
{
    if (app == 0 || app->hwnd == 0 || !app->scrollbar_enabled) {
        return;
    }
    switch (LOWORD(wparam)) {
    case SB_LINEUP:
        winxterm_app_scroll_lines(app, 1);
        break;
    case SB_LINEDOWN:
        winxterm_app_scroll_lines(app, -1);
        break;
    case SB_PAGEUP:
        winxterm_app_scroll_page(app, 1);
        break;
    case SB_PAGEDOWN:
        winxterm_app_scroll_page(app, -1);
        break;
    case SB_TOP:
        winxterm_app_scroll_home(app);
        break;
    case SB_BOTTOM:
        winxterm_app_scroll_bottom(app);
        break;
    case SB_THUMBTRACK:
    case SB_THUMBPOSITION: {
        SCROLLINFO info;
        memset(&info, 0, sizeof(info));
        info.cbSize = sizeof(info);
        info.fMask = SIF_TRACKPOS;
        if (GetScrollInfo(app->hwnd, SB_VERT, &info)) {
            winxterm_app_scroll_to_row(app, info.nTrackPos);
        }
        break;
    }
    default:
        break;
    }
}

static void winxterm_app_clear_scrollback(WinxtermApp *app)
{
    if (app == 0 || app->bridge == 0) {
        return;
    }
    EnterCriticalSection(&app->bridge->screen_lock);
    winxterm_screen_clear_scrollback(&app->bridge->screen);
    winxterm_ux_scroll_to_bottom(&app->ux);
    winxterm_ux_clear_selection(&app->ux);
    LeaveCriticalSection(&app->bridge->screen_lock);
    winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
}

static void winxterm_app_reset_terminal(WinxtermApp *app)
{
    if (app == 0 || app->bridge == 0) {
        return;
    }
    EnterCriticalSection(&app->bridge->screen_lock);
    (void)winxterm_screen_start_session(&app->bridge->screen);
    winxterm_ux_init(&app->ux);
    LeaveCriticalSection(&app->bridge->screen_lock);
    winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
}

static void winxterm_app_select_all(WinxtermApp *app)
{
    if (app == 0 || app->bridge == 0) {
        return;
    }
    EnterCriticalSection(&app->bridge->screen_lock);
    winxterm_ux_select_all(&app->ux, &app->bridge->screen);
    LeaveCriticalSection(&app->bridge->screen_lock);
    (void)winxterm_app_copy_selection_and_clear(app);
}

static bool winxterm_app_handle_local_key(WinxtermApp *app, WPARAM virtual_key)
{
    WinxtermInputModifiers modifiers = winxterm_app_modifiers_from_keyboard();
    if (modifiers.ctrl && modifiers.shift) {
        switch (virtual_key) {
        case 'C':
            (void)winxterm_app_copy_selection_and_clear(app);
            return true;
        case 'V':
            (void)winxterm_app_paste_clipboard(app);
            return true;
        case 'A':
            winxterm_app_select_all(app);
            return true;
        case VK_PRIOR:
            winxterm_app_scroll_page(app, 1);
            return true;
        case VK_NEXT:
            winxterm_app_scroll_page(app, -1);
            return true;
        case VK_HOME:
            winxterm_app_scroll_home(app);
            return true;
        case VK_END:
            winxterm_app_scroll_bottom(app);
            return true;
        default:
            break;
        }
    }
    if (modifiers.shift && !modifiers.ctrl && !modifiers.alt) {
        switch (virtual_key) {
        case VK_PRIOR:
            winxterm_app_scroll_page(app, 1);
            return true;
        case VK_NEXT:
            winxterm_app_scroll_page(app, -1);
            return true;
        default:
            break;
        }
    }
    if (modifiers.ctrl && virtual_key == VK_INSERT) {
        (void)winxterm_app_copy_selection_and_clear(app);
        return true;
    }
    if (modifiers.shift && virtual_key == VK_INSERT) {
        (void)winxterm_app_paste_clipboard(app);
        return true;
    }
    return false;
}

static bool winxterm_app_handle_virtual_key(WinxtermApp *app, WPARAM wparam)
{
    uint8_t sequence[WINXTERM_INPUT_MAX_SEQUENCE];
    WinxtermModeState modes;
    memset(&modes, 0, sizeof(modes));
    (void)winxterm_bridge_copy_mode_state(app->bridge, &modes);
    size_t length = winxterm_input_encode_virtual_key_with_modes(wparam,
                                                                 winxterm_app_modifiers_from_keyboard(),
                                                                 &modes,
                                                                 sequence,
                                                                 sizeof(sequence));
    if (length == 0u) {
        return false;
    }
    (void)winxterm_app_queue_input(app, sequence, length);
    return true;
}

static bool winxterm_app_handle_char(WinxtermApp *app, wchar_t ch, bool alt)
{
    uint8_t sequence[WINXTERM_INPUT_MAX_SEQUENCE];
    WinxtermInputModifiers modifiers = winxterm_app_modifiers_from_keyboard();
    modifiers.alt = alt;
    size_t length = winxterm_input_encode_char(ch, modifiers, sequence, sizeof(sequence));
    if (length == 0u) {
        return false;
    }
    (void)winxterm_app_queue_input(app, sequence, length);
    return true;
}

static void winxterm_app_send_focus_report(WinxtermApp *app, bool focused)
{
    if (app == 0 || app->bridge == 0 ||
        !winxterm_bridge_mode_enabled(app->bridge, WINXTERM_TERMINAL_MODE_FOCUS_REPORT)) {
        return;
    }
    const uint8_t focus_in[] = "\x1b[I";
    const uint8_t focus_out[] = "\x1b[O";
    const uint8_t *bytes = focused ? focus_in : focus_out;
    size_t length = focused ? sizeof(focus_in) - 1u : sizeof(focus_out) - 1u;
    if (!winxterm_bridge_queue_reply(app->bridge, bytes, length)) {
        winxterm_log_writef(app->log, "input queue full, dropped focus report");
    }
}

static WinxtermMouseButton winxterm_app_button_from_message(UINT message)
{
    switch (message) {
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_MBUTTONDBLCLK:
        return WINXTERM_MOUSE_BUTTON_MIDDLE;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_RBUTTONDBLCLK:
        return WINXTERM_MOUSE_BUTTON_RIGHT;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    default:
        return WINXTERM_MOUSE_BUTTON_LEFT;
    }
}

static bool winxterm_app_report_mouse(WinxtermApp *app,
                                      WinxtermMouseEventKind kind,
                                      WinxtermMouseButton button,
                                      LPARAM lparam)
{
    if (app == 0 || app->bridge == 0) {
        return false;
    }
    WinxtermModeState modes;
    memset(&modes, 0, sizeof(modes));
    (void)winxterm_bridge_copy_mode_state(app->bridge, &modes);
    if (!winxterm_mouse_reporting_enabled(&modes)) {
        return false;
    }

    WinxtermUxPosition position;
    bool hit = false;
    EnterCriticalSection(&app->bridge->screen_lock);
    WinxtermCellSize visible = winxterm_app_visible_cells_locked(app);
    hit = winxterm_ux_hit_test_cells(&app->ux,
                                     &app->bridge->screen,
                                     GET_X_LPARAM(lparam),
                                     GET_Y_LPARAM(lparam),
                                     winxterm_app_display_scale(app),
                                     visible.columns,
                                     visible.rows,
                                     &position);
    LeaveCriticalSection(&app->bridge->screen_lock);
    if (!hit) {
        return false;
    }

    WinxtermMouseEvent event;
    memset(&event, 0, sizeof(event));
    event.kind = kind;
    event.button = button;
    event.column = position.column;
    event.row = winxterm_physical_to_logical_pixels(GET_Y_LPARAM(lparam),
                                                    winxterm_app_display_scale(app)) /
        WINXTERM_CELL_HEIGHT_PIXELS;
    event.modifiers = winxterm_app_modifiers_from_keyboard();
    uint8_t sequence[WINXTERM_INPUT_MAX_SEQUENCE];
    size_t length = winxterm_mouse_encode_event(&modes, &event, sequence, sizeof(sequence));
    if (length == 0u) {
        return false;
    }
    return winxterm_app_queue_input(app, sequence, length);
}

static void winxterm_app_begin_local_selection_at(WinxtermApp *app,
                                                  WinxtermUxPosition position,
                                                  WinxtermSelectionMode mode)
{
    if (app == 0 || app->bridge == 0) {
        return;
    }
    EnterCriticalSection(&app->bridge->screen_lock);
    winxterm_ux_begin_selection_mode(&app->ux, position, mode);
    LeaveCriticalSection(&app->bridge->screen_lock);
    winxterm_bridge_set_output_paused(app->bridge, true);
    SetCapture(app->hwnd);
    winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
}

static void winxterm_app_update_local_selection(WinxtermApp *app, LPARAM lparam)
{
    if (app == 0 || app->bridge == 0 || !app->ux.selection.selecting) {
        return;
    }
    WinxtermUxPosition position;
    bool hit = false;
    EnterCriticalSection(&app->bridge->screen_lock);
    WinxtermCellSize visible = winxterm_app_visible_cells_locked(app);
    hit = winxterm_ux_hit_test_cells(&app->ux,
                                     &app->bridge->screen,
                                     GET_X_LPARAM(lparam),
                                     GET_Y_LPARAM(lparam),
                                     winxterm_app_display_scale(app),
                                     visible.columns,
                                     visible.rows,
                                     &position);
    if (hit) {
        winxterm_ux_update_selection(&app->ux, position);
    }
    LeaveCriticalSection(&app->bridge->screen_lock);
    if (hit) {
        winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
    }
}

static void winxterm_app_finish_local_selection(WinxtermApp *app, LPARAM lparam)
{
    if (app == 0 || app->bridge == 0 || !app->ux.selection.selecting) {
        return;
    }
    winxterm_app_update_local_selection(app, lparam);
    EnterCriticalSection(&app->bridge->screen_lock);
    winxterm_ux_finish_selection(&app->ux);
    LeaveCriticalSection(&app->bridge->screen_lock);
    ReleaseCapture();
    WinxtermSelectionCopyFormat copy_format = app->left_click_rectangular_preserve_rows ?
        WINXTERM_SELECTION_COPY_RECTANGULAR_PRESERVE_ROWS : WINXTERM_SELECTION_COPY_DEFAULT;
    app->left_click_rectangular_preserve_rows = false;
    (void)winxterm_app_copy_selection_and_clear_format(app, copy_format);
    winxterm_bridge_set_output_paused(app->bridge, false);
}

static UINT winxterm_app_double_click_time(void)
{
    UINT double_click_time = GetDoubleClickTime();
    return double_click_time == 0u ? 1u : double_click_time;
}

static void winxterm_app_abort_click_logic_oneshot(WinxtermApp *app)
{
    if (app == 0) {
        return;
    }
    if (app->click_event_timer_id != 0u && app->hwnd != 0) {
        KillTimer(app->hwnd, app->click_event_timer_id);
    }
    app->click_event_timer_id = 0u;
    app->click_event_requested_handler = false;
}

static void winxterm_app_reset_click_sequence(WinxtermApp *app)
{
    if (app == 0) {
        return;
    }
    winxterm_app_abort_click_logic_oneshot(app);
    app->click_sequence_count = 0u;
    app->click_sequence_last_tick = 0u;
    memset(&app->click_sequence_position, 0, sizeof(app->click_sequence_position));
    winxterm_app_clear_click_preview(app, true);
}

static bool winxterm_app_select_non_space_run_and_copy(WinxtermApp *app,
                                                       WinxtermUxPosition position)
{
    if (app == 0 || app->bridge == 0) {
        return false;
    }
    bool selected = false;
    EnterCriticalSection(&app->bridge->screen_lock);
    selected = winxterm_ux_select_non_space_run_at(&app->ux, &app->bridge->screen, position);
    LeaveCriticalSection(&app->bridge->screen_lock);
    if (selected) {
        (void)winxterm_app_copy_selection_and_clear(app);
    }
    return selected;
}

static bool winxterm_app_select_real_line_and_copy(WinxtermApp *app,
                                                   WinxtermUxPosition position)
{
    if (app == 0 || app->bridge == 0) {
        return false;
    }
    bool selected = false;
    EnterCriticalSection(&app->bridge->screen_lock);
    selected = winxterm_ux_select_real_line_at(&app->ux, &app->bridge->screen, position);
    LeaveCriticalSection(&app->bridge->screen_lock);
    if (selected) {
        (void)winxterm_app_copy_selection_and_clear(app);
    }
    return selected;
}

static void winxterm_app_handle_click_logic_oneshot(WinxtermApp *app)
{
    if (app == 0 || !app->click_event_requested_handler) {
        return;
    }
    unsigned int click_count = app->click_sequence_count;
    WinxtermUxPosition position = app->click_sequence_position;
    winxterm_app_reset_click_sequence(app);

    if (click_count == 2u) {
        (void)winxterm_app_select_non_space_run_and_copy(app, position);
        return;
    }
    if (click_count == 3u) {
        (void)winxterm_app_select_real_line_and_copy(app, position);
    }
}

static void winxterm_app_request_click_logic_oneshot(WinxtermApp *app)
{
    if (app == 0) {
        return;
    }
    app->click_event_requested_handler = true;
    if (app->hwnd == 0) {
        winxterm_app_handle_click_logic_oneshot(app);
        return;
    }
    app->click_event_timer_id = SetTimer(app->hwnd,
                                         WINXTERM_CLICK_EVENT_TIMER_ID,
                                         winxterm_app_double_click_time(),
                                         0);
    if (app->click_event_timer_id == 0u) {
        winxterm_app_handle_click_logic_oneshot(app);
    }
}

static void winxterm_app_note_local_click(WinxtermApp *app,
                                          WinxtermUxPosition position,
                                          DWORD now)
{
    if (app == 0) {
        return;
    }

    winxterm_app_abort_click_logic_oneshot(app);
    UINT double_click_time = winxterm_app_double_click_time();
    if (app->click_sequence_count == 3u &&
        now - app->click_sequence_last_tick <= double_click_time) {
        WinxtermUxPosition triple_position = app->click_sequence_position;
        winxterm_app_reset_click_sequence(app);
        (void)winxterm_app_select_real_line_and_copy(app, triple_position);
        return;
    }

    if (app->click_sequence_count == 0u ||
        now - app->click_sequence_last_tick > double_click_time) {
        app->click_sequence_count = 1u;
    } else {
        ++app->click_sequence_count;
    }
    app->click_sequence_last_tick = now;
    app->click_sequence_position = position;

    if (app->click_sequence_count == 1u) {
        winxterm_app_show_click_row_preview(app, position);
    } else if (app->click_sequence_count == 2u) {
        winxterm_app_show_click_range_preview(app, position);
    } else if (app->click_sequence_count == 3u) {
        winxterm_app_show_click_line_preview(app, position);
    } else {
        winxterm_app_clear_click_preview(app, true);
    }
    winxterm_app_request_click_logic_oneshot(app);
}

static bool winxterm_app_begin_left_click_candidate(WinxtermApp *app,
                                                    WPARAM wparam,
                                                    LPARAM lparam)
{
    if (app == 0) {
        return false;
    }
    winxterm_app_abort_click_logic_oneshot(app);
    winxterm_app_clear_click_preview(app, true);
    app->left_click_rectangular = false;
    app->left_click_rectangular_preserve_rows = false;

    WinxtermUxPosition position;
    if (!winxterm_app_hit_test_lparam(app, lparam, &position, 0)) {
        app->left_click_pending = false;
        return false;
    }

    app->left_click_pending = true;
    app->left_click_rectangular = (wparam & MK_SHIFT) != 0u;
    app->left_click_rectangular_preserve_rows =
        (wparam & (MK_SHIFT | MK_CONTROL)) == (MK_SHIFT | MK_CONTROL);
    app->left_click_start_point.x = GET_X_LPARAM(lparam);
    app->left_click_start_point.y = GET_Y_LPARAM(lparam);
    app->left_click_anchor = position;
    SetCapture(app->hwnd);
    return true;
}

static bool winxterm_app_left_click_drag_ready(WinxtermApp *app, LPARAM lparam)
{
    if (app == 0 || !app->left_click_pending) {
        return false;
    }

    int dx = GET_X_LPARAM(lparam) - app->left_click_start_point.x;
    int dy = GET_Y_LPARAM(lparam) - app->left_click_start_point.y;
    if (dx < 0) {
        dx = -dx;
    }
    if (dy < 0) {
        dy = -dy;
    }
    if (dx <= WINXTERM_LOCAL_DRAG_DISTANCE_PIXELS && dy <= WINXTERM_LOCAL_DRAG_DISTANCE_PIXELS) {
        return false;
    }
    return true;
}

static bool winxterm_app_maybe_start_left_click_drag(WinxtermApp *app, LPARAM lparam)
{
    if (!winxterm_app_left_click_drag_ready(app, lparam)) {
        return false;
    }

    WinxtermSelectionMode mode = app->left_click_rectangular ?
        WINXTERM_SELECTION_RECTANGULAR : WINXTERM_SELECTION_LINEAR;
    app->left_click_pending = false;
    winxterm_app_begin_local_selection_at(app, app->left_click_anchor, mode);
    winxterm_app_update_local_selection(app, lparam);
    winxterm_app_reset_click_sequence(app);
    return true;
}

static void winxterm_app_finish_left_click_candidate(WinxtermApp *app, LPARAM lparam)
{
    if (app == 0 || !app->left_click_pending) {
        return;
    }

    if (winxterm_app_maybe_start_left_click_drag(app, lparam)) {
        winxterm_app_finish_local_selection(app, lparam);
        return;
    }

    WinxtermUxPosition position = app->left_click_anchor;
    app->left_click_pending = false;
    app->left_click_rectangular_preserve_rows = false;
    if (GetCapture() == app->hwnd) {
        ReleaseCapture();
    }
    winxterm_app_note_local_click(app, position, GetTickCount());
}

static bool winxterm_app_alt_mouse_active(void)
{
    WinxtermInputModifiers modifiers = winxterm_app_modifiers_from_keyboard();
    return modifiers.alt && !modifiers.ctrl && !modifiers.shift;
}

static unsigned int winxterm_app_alt_resize_edges(const RECT *window, const POINT *cursor)
{
    if (window == 0 || cursor == 0) {
        return 0u;
    }

    LONG center_x = window->left + (window->right - window->left) / 2;
    LONG center_y = window->top + (window->bottom - window->top) / 2;
    unsigned int edges = cursor->x < center_x ?
        WINXTERM_ALT_DRAG_EDGE_LEFT : WINXTERM_ALT_DRAG_EDGE_RIGHT;
    edges |= cursor->y < center_y ?
        WINXTERM_ALT_DRAG_EDGE_TOP : WINXTERM_ALT_DRAG_EDGE_BOTTOM;
    return edges;
}

static void winxterm_app_clamp_alt_resize_rect(WinxtermApp *app, RECT *rect)
{
    if (app == 0 || rect == 0) {
        return;
    }

    SIZE minimum = winxterm_app_min_window_size(app);
    if (minimum.cx > 0 && rect->right - rect->left < minimum.cx) {
        if ((app->alt_drag_edges & WINXTERM_ALT_DRAG_EDGE_LEFT) != 0u) {
            rect->left = rect->right - minimum.cx;
        } else {
            rect->right = rect->left + minimum.cx;
        }
    }
    if (minimum.cy > 0 && rect->bottom - rect->top < minimum.cy) {
        if ((app->alt_drag_edges & WINXTERM_ALT_DRAG_EDGE_TOP) != 0u) {
            rect->top = rect->bottom - minimum.cy;
        } else {
            rect->bottom = rect->top + minimum.cy;
        }
    }
}

static bool winxterm_app_begin_alt_drag(WinxtermApp *app, WinxtermAltDragMode mode)
{
    if (app == 0 || app->hwnd == 0 || app->fullscreen || IsZoomed(app->hwnd) ||
        (mode != WINXTERM_ALT_DRAG_MOVE && mode != WINXTERM_ALT_DRAG_RESIZE)) {
        return false;
    }

    POINT cursor;
    RECT window;
    if (!GetCursorPos(&cursor) || !GetWindowRect(app->hwnd, &window)) {
        return false;
    }

    app->alt_drag_mode = mode;
    app->alt_drag_start_cursor = cursor;
    app->alt_drag_latest_cursor = cursor;
    app->alt_drag_start_window = window;
    app->alt_drag_edges = mode == WINXTERM_ALT_DRAG_RESIZE ?
        winxterm_app_alt_resize_edges(&window, &cursor) : 0u;
    app->alt_drag_last_apply_ms = 0ull;
    app->alt_drag_suppress_context_menu = false;
    SetCapture(app->hwnd);
    return true;
}

static bool winxterm_app_apply_alt_drag(WinxtermApp *app, bool final)
{
    if (app == 0 || app->hwnd == 0 || app->alt_drag_mode == WINXTERM_ALT_DRAG_NONE) {
        return false;
    }

    POINT cursor;
    if (!GetCursorPos(&cursor)) {
        return true;
    }
    app->alt_drag_latest_cursor = cursor;

    ULONGLONG now = GetTickCount64();
    if (!final && app->alt_drag_last_apply_ms != 0ull &&
        now - app->alt_drag_last_apply_ms < WINXTERM_ALT_DRAG_THROTTLE_MS) {
        return true;
    }

    int dx = cursor.x - app->alt_drag_start_cursor.x;
    int dy = cursor.y - app->alt_drag_start_cursor.y;
    RECT target = app->alt_drag_start_window;

    if (app->alt_drag_mode == WINXTERM_ALT_DRAG_MOVE) {
        target.left += dx;
        target.right += dx;
        target.top += dy;
        target.bottom += dy;
        SetWindowPos(app->hwnd,
                     0,
                     target.left,
                     target.top,
                     0,
                     0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
        winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
    } else {
        if ((app->alt_drag_edges & WINXTERM_ALT_DRAG_EDGE_LEFT) != 0u) {
            target.left += dx;
        }
        if ((app->alt_drag_edges & WINXTERM_ALT_DRAG_EDGE_RIGHT) != 0u) {
            target.right += dx;
        }
        if ((app->alt_drag_edges & WINXTERM_ALT_DRAG_EDGE_TOP) != 0u) {
            target.top += dy;
        }
        if ((app->alt_drag_edges & WINXTERM_ALT_DRAG_EDGE_BOTTOM) != 0u) {
            target.bottom += dy;
        }
        winxterm_app_clamp_alt_resize_rect(app, &target);
        SetWindowPos(app->hwnd,
                     0,
                     target.left,
                     target.top,
                     target.right - target.left,
                     target.bottom - target.top,
                     SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_NOACTIVATE);
    }

    app->alt_drag_last_apply_ms = now;
    return true;
}

static bool winxterm_app_finish_alt_drag(WinxtermApp *app)
{
    if (app == 0 || app->alt_drag_mode == WINXTERM_ALT_DRAG_NONE) {
        return false;
    }

    bool was_resize = app->alt_drag_mode == WINXTERM_ALT_DRAG_RESIZE;
    (void)winxterm_app_apply_alt_drag(app, true);
    if (GetCapture() == app->hwnd) {
        ReleaseCapture();
    }
    if (was_resize) {
        winxterm_app_snap_window_to_cells(app);
    }

    app->alt_drag_mode = WINXTERM_ALT_DRAG_NONE;
    app->alt_drag_edges = 0u;
    app->alt_drag_last_apply_ms = 0ull;
    app->alt_drag_suppress_context_menu = was_resize;
    return true;
}

static void winxterm_app_cancel_alt_drag(WinxtermApp *app)
{
    if (app == 0 || app->alt_drag_mode == WINXTERM_ALT_DRAG_NONE) {
        return;
    }

    if (GetCapture() == app->hwnd) {
        ReleaseCapture();
    }
    app->alt_drag_mode = WINXTERM_ALT_DRAG_NONE;
    app->alt_drag_edges = 0u;
    app->alt_drag_last_apply_ms = 0ull;
    app->alt_drag_suppress_context_menu = false;
}

static bool winxterm_app_handle_mouse_button(WinxtermApp *app, UINT message, WPARAM wparam, LPARAM lparam)
{
    bool down = message == WM_LBUTTONDOWN || message == WM_MBUTTONDOWN || message == WM_RBUTTONDOWN ||
                message == WM_LBUTTONDBLCLK || message == WM_MBUTTONDBLCLK || message == WM_RBUTTONDBLCLK;
    bool up = message == WM_LBUTTONUP || message == WM_MBUTTONUP || message == WM_RBUTTONUP;
    WinxtermMouseButton button = winxterm_app_button_from_message(message);
    if (app != 0 && app->alt_drag_suppress_context_menu && down) {
        app->alt_drag_suppress_context_menu = false;
    }
    if (app != 0 && app->alt_drag_mode != WINXTERM_ALT_DRAG_NONE) {
        bool matching_release =
            up && ((app->alt_drag_mode == WINXTERM_ALT_DRAG_MOVE &&
                    button == WINXTERM_MOUSE_BUTTON_LEFT) ||
                   (app->alt_drag_mode == WINXTERM_ALT_DRAG_RESIZE &&
                    button == WINXTERM_MOUSE_BUTTON_RIGHT));
        if (matching_release) {
            (void)winxterm_app_finish_alt_drag(app);
        }
        return true;
    }
    if (down && winxterm_app_alt_mouse_active()) {
        if (button == WINXTERM_MOUSE_BUTTON_LEFT) {
            (void)winxterm_app_begin_alt_drag(app, WINXTERM_ALT_DRAG_MOVE);
            return true;
        }
        if (button == WINXTERM_MOUSE_BUTTON_RIGHT) {
            (void)winxterm_app_begin_alt_drag(app, WINXTERM_ALT_DRAG_RESIZE);
            return true;
        }
    }
    if (down && button == WINXTERM_MOUSE_BUTTON_MIDDLE) {
        (void)winxterm_app_paste_clipboard(app);
        return true;
    }
    bool force_local = (wparam & MK_SHIFT) != 0u;
    if (!force_local && winxterm_app_report_mouse(app,
                                                  up ? WINXTERM_MOUSE_EVENT_RELEASE :
                                                       WINXTERM_MOUSE_EVENT_PRESS,
                                                  button,
                                                  lparam)) {
        if (down) {
            SetCapture(app->hwnd);
        } else if (up) {
            ReleaseCapture();
        }
        return true;
    }
    if (down && button == WINXTERM_MOUSE_BUTTON_LEFT) {
        return winxterm_app_begin_left_click_candidate(app, wparam, lparam);
    }
    if (up && button == WINXTERM_MOUSE_BUTTON_LEFT) {
        if (app != 0 && app->left_click_pending) {
            winxterm_app_finish_left_click_candidate(app, lparam);
        } else {
            winxterm_app_finish_local_selection(app, lparam);
        }
        return true;
    }
    return false;
}

static bool winxterm_app_handle_mouse_move(WinxtermApp *app, WPARAM wparam, LPARAM lparam)
{
    if (app == 0) {
        return false;
    }
    winxterm_app_update_hover(app, lparam);
    if (app->alt_drag_mode != WINXTERM_ALT_DRAG_NONE) {
        (void)winxterm_app_apply_alt_drag(app, false);
        return true;
    }
    if (app->left_click_pending) {
        (void)winxterm_app_maybe_start_left_click_drag(app, lparam);
        return true;
    }
    if (app->ux.selection.selecting) {
        winxterm_app_update_local_selection(app, lparam);
        return true;
    }
    WinxtermMouseButton button = WINXTERM_MOUSE_BUTTON_RELEASE;
    if ((wparam & MK_LBUTTON) != 0u) {
        button = WINXTERM_MOUSE_BUTTON_LEFT;
    } else if ((wparam & MK_MBUTTON) != 0u) {
        button = WINXTERM_MOUSE_BUTTON_MIDDLE;
    } else if ((wparam & MK_RBUTTON) != 0u) {
        button = WINXTERM_MOUSE_BUTTON_RIGHT;
    }
    return winxterm_app_report_mouse(app,
                                     button == WINXTERM_MOUSE_BUTTON_RELEASE ?
                                         WINXTERM_MOUSE_EVENT_MOVE : WINXTERM_MOUSE_EVENT_DRAG,
                                     button,
                                     lparam);
}

static bool winxterm_app_handle_mouse_wheel(WinxtermApp *app, WPARAM wparam, LPARAM lparam)
{
    WinxtermModeState modes;
    memset(&modes, 0, sizeof(modes));
    (void)winxterm_bridge_copy_mode_state(app->bridge, &modes);
    if (winxterm_mouse_reporting_enabled(&modes)) {
        POINT point;
        point.x = GET_X_LPARAM(lparam);
        point.y = GET_Y_LPARAM(lparam);
        ScreenToClient(app->hwnd, &point);
        LPARAM client_lparam = MAKELPARAM((SHORT)point.x, (SHORT)point.y);
        WinxtermMouseButton wheel = GET_WHEEL_DELTA_WPARAM(wparam) > 0 ?
            WINXTERM_MOUSE_WHEEL_UP : WINXTERM_MOUSE_WHEEL_DOWN;
        if (winxterm_app_report_mouse(app, WINXTERM_MOUSE_EVENT_WHEEL, wheel, client_lparam)) {
            return true;
        }
    }
    int delta = GET_WHEEL_DELTA_WPARAM(wparam);
    int lines = delta / WHEEL_DELTA;
    if (lines == 0) {
        lines = delta > 0 ? 1 : -1;
    }
    winxterm_app_scroll_lines(app, lines * 3);
    return true;
}

static void winxterm_app_show_context_menu(WinxtermApp *app, LPARAM lparam)
{
    if (app == 0 || app->hwnd == 0) {
        return;
    }
    HMENU menu = CreatePopupMenu();
    if (menu == 0) {
        return;
    }
    AppendMenuW(menu,
                MF_STRING | (winxterm_ux_has_selection(&app->ux) ? MF_ENABLED : MF_GRAYED),
                WINXTERM_MENU_COPY,
                L"Copy");
    AppendMenuW(menu,
                MF_STRING | (IsClipboardFormatAvailable(CF_UNICODETEXT) ? MF_ENABLED : MF_GRAYED),
                WINXTERM_MENU_PASTE,
                L"Paste");
    AppendMenuW(menu, MF_SEPARATOR, 0, 0);
    AppendMenuW(menu, MF_STRING, WINXTERM_MENU_CLEAR_SCROLLBACK, L"Clear scrollback");
    AppendMenuW(menu, MF_STRING, WINXTERM_MENU_RESET_TERMINAL, L"Reset terminal");

    typedef struct WinxtermMenuJobCommand {
        UINT id;
        uint64_t job_id;
        WinxtermBridgeJobAction action;
    } WinxtermMenuJobCommand;
    WinxtermManagedJobSnapshot *job_snapshot = 0;
    size_t job_count = 0u;
    WinxtermMenuJobCommand *job_commands = 0;
    size_t job_command_count = 0u;
    if (winxterm_job_manager_snapshot(&app->bridge->job_manager, 0u,
                                      &job_snapshot, &job_count) && job_count != 0u) {
        job_commands = (WinxtermMenuJobCommand *)calloc(job_count * 5u,
                                                        sizeof(*job_commands));
        HMENU jobs_menu = job_commands != 0 ? CreatePopupMenu() : 0;
        for (size_t i = 0u; jobs_menu != 0 && i < job_count; ++i) {
            WinxtermManagedJobSnapshot *job = job_snapshot + i;
            HMENU job_menu = CreatePopupMenu();
            if (job_menu == 0) continue;
            const wchar_t *state = job->state == WINXTERM_JOB_STARTING ? L"starting" :
                                   job->state == WINXTERM_JOB_FOREGROUND ? L"foreground" :
                                   job->state == WINXTERM_JOB_BACKGROUND ? L"background" :
                                   job->state == WINXTERM_JOB_STOPPING ? L"stopping" :
                                   job->state == WINXTERM_JOB_EXITED ? L"exited" : L"failed";
            wchar_t label[512];
            if (swprintf_s(label, 512, L"%llu - %ls (%ls)",
                           (unsigned long long)job->id,
                           job->display_name[0] != L'\0' ? job->display_name : L"(unknown)",
                           state) <= 0) {
                DestroyMenu(job_menu);
                continue;
            }
            bool live = job->state != WINXTERM_JOB_EXITED && job->state != WINXTERM_JOB_FAILED;
            static const struct {
                const wchar_t *label;
                WinxtermBridgeJobAction action;
            } actions[] = {
                {L"Foreground", WINXTERM_BRIDGE_JOB_ACTION_FOREGROUND},
                {L"View", WINXTERM_BRIDGE_JOB_ACTION_VIEW},
                {L"Background", WINXTERM_BRIDGE_JOB_ACTION_BACKGROUND},
                {L"Close", WINXTERM_BRIDGE_JOB_ACTION_CLOSE},
                {L"Force exit", WINXTERM_BRIDGE_JOB_ACTION_FORCE_EXIT}
            };
            for (size_t action_index = 0u; action_index < 5u; ++action_index) {
                if (action_index == 4u) AppendMenuW(job_menu, MF_SEPARATOR, 0, 0);
                UINT id = 2000u + (UINT)job_command_count;
                bool enabled = actions[action_index].action ==
                    WINXTERM_BRIDGE_JOB_ACTION_VIEW ? true : live;
                if (actions[action_index].action == WINXTERM_BRIDGE_JOB_ACTION_FOREGROUND) {
                    enabled = live && !job->foreground;
                } else if (actions[action_index].action == WINXTERM_BRIDGE_JOB_ACTION_BACKGROUND) {
                    enabled = live && job->foreground;
                }
                AppendMenuW(job_menu, MF_STRING | (enabled ? MF_ENABLED : MF_GRAYED), id,
                            actions[action_index].label);
                job_commands[job_command_count].id = id;
                job_commands[job_command_count].job_id = job->id;
                job_commands[job_command_count].action = actions[action_index].action;
                ++job_command_count;
            }
            AppendMenuW(jobs_menu, MF_POPUP, (UINT_PTR)job_menu, label);
        }
        if (jobs_menu != 0) AppendMenuW(menu, MF_POPUP, (UINT_PTR)jobs_menu, L"Jobs");
    }

    HMENU scale_menu = CreatePopupMenu();
    if (scale_menu != 0) {
        unsigned int scale = winxterm_app_display_scale(app);
        AppendMenuW(scale_menu,
                    MF_STRING | (scale == 1u ? MF_CHECKED : 0u),
                    WINXTERM_MENU_SCALE_1,
                    L"x1 (no scaling)");
        AppendMenuW(scale_menu,
                    MF_STRING | (scale == 2u ? MF_CHECKED : 0u),
                    WINXTERM_MENU_SCALE_2,
                    L"x2");
        AppendMenuW(scale_menu,
                    MF_STRING | (scale == 3u ? MF_CHECKED : 0u),
                    WINXTERM_MENU_SCALE_3,
                    L"x3");
        AppendMenuW(scale_menu,
                    MF_STRING | (scale == 4u ? MF_CHECKED : 0u),
                    WINXTERM_MENU_SCALE_4,
                    L"x4");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)scale_menu, L"Scale");
    }

    HMENU render_menu = CreatePopupMenu();
    if (render_menu != 0) {
        WinxtermRenderBackend backend = winxterm_bridge_backend(app->bridge);
        AppendMenuW(render_menu,
                    MF_STRING | (backend == WINXTERM_RENDER_BACKEND_SPANS ? MF_CHECKED : 0u),
                    WINXTERM_MENU_RENDER_SPANS,
                    L"Spans");
        AppendMenuW(render_menu,
                    MF_STRING | (backend == WINXTERM_RENDER_BACKEND_ROW_MASKS ? MF_CHECKED : 0u),
                    WINXTERM_MENU_RENDER_ROW_MASKS,
                    L"Row masks");
        AppendMenuW(render_menu,
                    MF_STRING | (backend == WINXTERM_RENDER_BACKEND_PRECLORED_CACHE ? MF_CHECKED : 0u),
                    WINXTERM_MENU_RENDER_PRECOLORED,
                    L"Precolored cache");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)render_menu, L"Renderer");
    }

    HMENU render_threads_menu = CreatePopupMenu();
    if (render_threads_menu != 0) {
        unsigned int active_threads = winxterm_app_active_render_thread_count(app);
        unsigned int thread_limit = winxterm_options_default_render_thread_count();
        for (unsigned int count = 1u; count <= thread_limit; ++count) {
            wchar_t label[16];
            if (swprintf_s(label, 16, L"%u", count) > 0) {
                AppendMenuW(render_threads_menu,
                            MF_STRING | (active_threads == count ? MF_CHECKED : 0u),
                            (UINT_PTR)(WINXTERM_MENU_RENDER_THREADS_BASE + count - 1u),
                            label);
            }
        }
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)render_threads_menu, L"renderthreads");
    }
    AppendMenuW(menu, MF_STRING, WINXTERM_MENU_DIAGNOSTICS, L"Diagnostics");

    POINT point;
    if (lparam == (LPARAM)-1) {
        GetCursorPos(&point);
    } else {
        point.x = GET_X_LPARAM(lparam);
        point.y = GET_Y_LPARAM(lparam);
    }
    UINT selected = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
                                   point.x, point.y, 0, app->hwnd, 0);
    bool handled = false;
    for (size_t i = 0u; i < job_command_count; ++i) {
        if (job_commands[i].id == selected) {
            (void)winxterm_bridge_request_job_action(app->bridge, job_commands[i].action,
                                                     job_commands[i].job_id);
            handled = true;
            break;
        }
    }
    if (!handled && selected != 0u) winxterm_app_handle_command(app, (WPARAM)selected);
    DestroyMenu(menu);
    free(job_commands);
    winxterm_job_manager_snapshot_dispose(job_snapshot);
}

static void winxterm_app_handle_command(WinxtermApp *app, WPARAM wparam)
{
    unsigned int id = LOWORD(wparam);
    if (id >= WINXTERM_MENU_RENDER_THREADS_BASE &&
        id <= WINXTERM_MENU_RENDER_THREADS_LAST) {
        unsigned int count = id - WINXTERM_MENU_RENDER_THREADS_BASE + 1u;
        if (count <= winxterm_options_default_render_thread_count()) {
            winxterm_app_apply_render_thread_count(app, count);
        }
        return;
    }

    switch (id) {
    case WINXTERM_MENU_COPY:
        (void)winxterm_app_copy_selection_and_clear(app);
        break;
    case WINXTERM_MENU_PASTE:
        (void)winxterm_app_paste_clipboard(app);
        break;
    case WINXTERM_MENU_CLEAR_SCROLLBACK:
        winxterm_app_clear_scrollback(app);
        break;
    case WINXTERM_MENU_RESET_TERMINAL:
        winxterm_app_reset_terminal(app);
        break;
    case WINXTERM_MENU_SCALE_1:
        winxterm_bridge_request_display_scale(app->bridge, 1u);
        break;
    case WINXTERM_MENU_SCALE_2:
        winxterm_bridge_request_display_scale(app->bridge, 2u);
        break;
    case WINXTERM_MENU_SCALE_3:
        winxterm_bridge_request_display_scale(app->bridge, 3u);
        break;
    case WINXTERM_MENU_SCALE_4:
        winxterm_bridge_request_display_scale(app->bridge, 4u);
        break;
    case WINXTERM_MENU_RENDER_SPANS:
        winxterm_bridge_set_backend(app->bridge, WINXTERM_RENDER_BACKEND_SPANS);
        winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
        break;
    case WINXTERM_MENU_RENDER_ROW_MASKS:
        winxterm_bridge_set_backend(app->bridge, WINXTERM_RENDER_BACKEND_ROW_MASKS);
        winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
        break;
    case WINXTERM_MENU_RENDER_PRECOLORED:
        winxterm_bridge_set_backend(app->bridge, WINXTERM_RENDER_BACKEND_PRECLORED_CACHE);
        winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
        break;
    case WINXTERM_MENU_DIAGNOSTICS:
        MessageBoxW(app->hwnd,
                    winxterm_log_enabled(app->log) ?
                        winxterm_log_path(app->log) :
                        L"Debug log disabled. Run set debuglog on to create it.",
                    L"Winxterm diagnostics log",
                    MB_OK);
        break;
    default:
        break;
    }
}

static int winxterm_close_dialog_text_width(HWND owner, const wchar_t *text)
{
    if (text == 0 || text[0] == L'\0') {
        return 0;
    }

    HDC dc = GetDC(owner);
    if (dc == 0) {
        return 0;
    }

    HFONT font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    HGDIOBJ previous = font != 0 ? SelectObject(dc, font) : 0;
    SIZE size = {0, 0};
    bool measured = GetTextExtentPoint32W(dc, text, (int)wcslen(text), &size) != 0;
    if (previous != 0) {
        SelectObject(dc, previous);
    }
    ReleaseDC(owner, dc);
    return measured ? size.cx : 0;
}

static void winxterm_close_dialog_layout(WinxtermCloseDialog *dialog, int width, int height)
{
    if (dialog == 0) {
        return;
    }

    if (width < WINXTERM_CLOSE_DIALOG_MIN_CLIENT_WIDTH) {
        width = WINXTERM_CLOSE_DIALOG_MIN_CLIENT_WIDTH;
    }
    if (height < WINXTERM_CLOSE_DIALOG_MIN_CLIENT_HEIGHT) {
        height = WINXTERM_CLOSE_DIALOG_MIN_CLIENT_HEIGHT;
    }

    int margin = WINXTERM_CLOSE_DIALOG_MARGIN;
    int content_width = width - margin * 2;
    int button_top = height - margin - WINXTERM_CLOSE_DIALOG_BUTTON_HEIGHT;
    int cancel_left = width - margin - WINXTERM_CLOSE_DIALOG_BUTTON_WIDTH;
    int ok_left = cancel_left - WINXTERM_CLOSE_DIALOG_GAP - WINXTERM_CLOSE_DIALOG_BUTTON_WIDTH;

    if (dialog->intro_label != 0) {
        SetWindowPos(dialog->intro_label,
                     0,
                     margin,
                     margin,
                     content_width,
                     WINXTERM_CLOSE_DIALOG_INTRO_HEIGHT,
                     SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    if (dialog->child_label != 0) {
        int child_height = button_top - WINXTERM_CLOSE_DIALOG_GAP -
            WINXTERM_CLOSE_DIALOG_CHECK_HEIGHT - WINXTERM_CLOSE_DIALOG_GAP - 60;
        if (child_height < WINXTERM_CLOSE_DIALOG_CHILD_HEIGHT) {
            child_height = WINXTERM_CLOSE_DIALOG_CHILD_HEIGHT;
        }
        SetWindowPos(dialog->child_label,
                     0,
                     margin,
                     60,
                     content_width,
                     child_height,
                     SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    if (dialog->checkbox != 0) {
        SetWindowPos(dialog->checkbox,
                     0,
                     margin,
                     button_top - WINXTERM_CLOSE_DIALOG_GAP - WINXTERM_CLOSE_DIALOG_CHECK_HEIGHT,
                     content_width,
                     WINXTERM_CLOSE_DIALOG_CHECK_HEIGHT,
                     SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    if (dialog->ok_button != 0) {
        SetWindowPos(dialog->ok_button,
                     0,
                     ok_left,
                     button_top,
                     WINXTERM_CLOSE_DIALOG_BUTTON_WIDTH,
                     WINXTERM_CLOSE_DIALOG_BUTTON_HEIGHT,
                     SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
    if (dialog->cancel_button != 0) {
        SetWindowPos(dialog->cancel_button,
                     0,
                     cancel_left,
                     button_top,
                     WINXTERM_CLOSE_DIALOG_BUTTON_WIDTH,
                     WINXTERM_CLOSE_DIALOG_BUTTON_HEIGHT,
                     SWP_NOZORDER | SWP_NOOWNERZORDER);
    }
}

static void winxterm_close_dialog_handle_minmax(MINMAXINFO *info)
{
    if (info == 0) {
        return;
    }

    RECT rect = {0,
                 0,
                 WINXTERM_CLOSE_DIALOG_MIN_CLIENT_WIDTH,
                 WINXTERM_CLOSE_DIALOG_MIN_CLIENT_HEIGHT};
    AdjustWindowRectEx(&rect,
                       WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_THICKFRAME,
                       FALSE,
                       WS_EX_DLGMODALFRAME);
    info->ptMinTrackSize.x = rect.right - rect.left;
    info->ptMinTrackSize.y = rect.bottom - rect.top;
}

static LRESULT CALLBACK winxterm_close_dialog_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    WinxtermCloseDialog *dialog = (WinxtermCloseDialog *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (message) {
    case WM_NCCREATE: {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lparam;
        dialog = (WinxtermCloseDialog *)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)dialog);
        if (dialog != 0) {
            dialog->hwnd = hwnd;
        }
        return TRUE;
    }
    case WM_CREATE:
        if (dialog != 0) {
            dialog->intro_label =
                CreateWindowExW(0,
                                L"STATIC",
                                L"The following programs are running, should they keep running in the background?",
                                WS_CHILD | WS_VISIBLE,
                                16,
                                16,
                                400,
                                36,
                                hwnd,
                                0,
                                0,
                                0);
            dialog->child_label = CreateWindowExW(WS_EX_CLIENTEDGE,
                                                  L"EDIT",
                                                  dialog->child_text,
                                                  WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                                      ES_LEFT | ES_MULTILINE | ES_READONLY |
                                                      ES_AUTOVSCROLL,
                                                  16,
                                                  60,
                                                  400,
                                                  WINXTERM_CLOSE_DIALOG_CHILD_HEIGHT,
                                                  hwnd,
                                                  0,
                                                  0,
                                                  0);
            dialog->checkbox = CreateWindowExW(0,
                                               L"BUTTON",
                                               L"Keep running",
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                               16,
                                               102,
                                               180,
                                               24,
                                               hwnd,
                                               (HMENU)1001,
                                               0,
                                               0);
            if (dialog->checkbox != 0) {
                SendMessageW(dialog->checkbox, BM_SETCHECK, BST_UNCHECKED, 0);
            }
            dialog->ok_button = CreateWindowExW(0,
                                                L"BUTTON",
                                                L"OK",
                                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                                                244,
                                                142,
                                                80,
                                                26,
                                                hwnd,
                                                (HMENU)IDOK,
                                                0,
                                                0);
            dialog->cancel_button = CreateWindowExW(0,
                                                    L"BUTTON",
                                                    L"Cancel",
                                                    WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                                    336,
                                                    142,
                                                    80,
                                                    26,
                                                    hwnd,
                                                    (HMENU)IDCANCEL,
                                                    0,
                                                    0);
            RECT client;
            if (GetClientRect(hwnd, &client)) {
                winxterm_close_dialog_layout(dialog,
                                             client.right - client.left,
                                             client.bottom - client.top);
            }
        }
        return 0;
    case WM_GETMINMAXINFO:
        winxterm_close_dialog_handle_minmax((MINMAXINFO *)lparam);
        return 0;
    case WM_SIZE:
        if (dialog != 0) {
            winxterm_close_dialog_layout(dialog, LOWORD(lparam), HIWORD(lparam));
        }
        return 0;
    case WM_COMMAND:
        if (dialog != 0 && LOWORD(wparam) == IDOK) {
            LRESULT checked = dialog->checkbox != 0 ?
                SendMessageW(dialog->checkbox, BM_GETCHECK, 0, 0) : BST_UNCHECKED;
            dialog->decision = checked == BST_CHECKED ?
                WINXTERM_CLOSE_DECISION_HEADLESS : WINXTERM_CLOSE_DECISION_TERMINATE;
            dialog->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if (dialog != 0 && LOWORD(wparam) == IDCANCEL) {
            dialog->decision = WINXTERM_CLOSE_DECISION_CANCEL;
            dialog->done = true;
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        if (dialog != 0) {
            dialog->decision = WINXTERM_CLOSE_DECISION_CANCEL;
            dialog->done = true;
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (dialog != 0) {
            dialog->done = true;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static bool winxterm_app_register_close_dialog_class(HINSTANCE instance)
{
    static const wchar_t class_name[] = L"WinxtermCloseDialog";
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = winxterm_close_dialog_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(0, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = class_name;
    ATOM atom = RegisterClassExW(&wc);
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

static WinxtermCloseDecision winxterm_app_confirm_child_close(WinxtermApp *app,
                                                              const WinxtermHostChildInfo *child)
{
    if (app == 0 || child == 0 || !child->running) {
        return WINXTERM_CLOSE_DECISION_CLOSE;
    }

    WinxtermCloseDialog dialog;
    memset(&dialog, 0, sizeof(dialog));
    dialog.decision = WINXTERM_CLOSE_DECISION_TERMINATE;
    WinxtermManagedJobSnapshot *jobs = 0;
    size_t job_count = 0u;
    bool snapshot_ok = app->bridge != 0 &&
        winxterm_job_manager_snapshot(&app->bridge->job_manager, 0u, &jobs, &job_count);
    size_t offset = 0u;
    for (size_t i = 0u; snapshot_ok && i < job_count; ++i) {
        if (jobs[i].state == WINXTERM_JOB_EXITED || jobs[i].state == WINXTERM_JOB_FAILED) continue;
        if (child->is_shell && jobs[i].owner_id == 0u) continue;
        int written = swprintf_s(dialog.child_text + offset,
                                 sizeof(dialog.child_text) / sizeof(dialog.child_text[0]) - offset,
                                 offset == 0u ? L"%llu - %ls (pid %lu, %ls)" :
                                                L"\r\n%llu - %ls (pid %lu, %ls)",
                                 (unsigned long long)jobs[i].id,
                                 jobs[i].display_name[0] != L'\0' ? jobs[i].display_name : L"(unknown)",
                                 (unsigned long)jobs[i].process_id,
                                 jobs[i].foreground ? L"foreground" : L"background");
        if (written <= 0) { offset = 0u; break; }
        offset += (size_t)written;
    }
    winxterm_job_manager_snapshot_dispose(jobs);
    if (child->is_shell && offset == 0u) {
        return WINXTERM_CLOSE_DECISION_TERMINATE;
    }
    if (offset == 0u) {
        int written = swprintf_s(dialog.child_text,
                                 sizeof(dialog.child_text) / sizeof(dialog.child_text[0]),
                                 L"%ls (pid %lu)",
                                 child->display_name[0] != L'\0' ? child->display_name : L"(unknown)",
                                 (unsigned long)child->process_id);
        if (written > 0) offset = (size_t)written;
    }
    if (offset == 0u) {
        winxterm_log_writef(app->log, "close dialog content formatting failed; defaulting to terminate");
        return WINXTERM_CLOSE_DECISION_TERMINATE;
    }

    if (!winxterm_app_register_close_dialog_class(app->instance)) {
        winxterm_log_writef(app->log, "close dialog class registration failed; defaulting to terminate");
        return WINXTERM_CLOSE_DECISION_TERMINATE;
    }

    RECT parent_rect;
    if (!GetWindowRect(app->hwnd, &parent_rect)) {
        parent_rect.left = 0;
        parent_rect.top = 0;
        parent_rect.right = 480;
        parent_rect.bottom = 240;
    }
    int intro_width =
        winxterm_close_dialog_text_width(app->hwnd,
                                         L"The following programs are running, should they keep running in the background?");
    int child_width = winxterm_close_dialog_text_width(app->hwnd, dialog.child_text);
    int client_width = WINXTERM_CLOSE_DIALOG_MIN_CLIENT_WIDTH;
    int required_client_width = child_width + WINXTERM_CLOSE_DIALOG_MARGIN * 2 + 24;
    if (required_client_width > client_width) {
        client_width = required_client_width;
    }
    required_client_width = intro_width + WINXTERM_CLOSE_DIALOG_MARGIN * 2;
    if (required_client_width > client_width) {
        client_width = required_client_width;
    }

    MONITORINFO monitor;
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromWindow(app->hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
        int max_client_width = (monitor.rcWork.right - monitor.rcWork.left) - 80;
        if (max_client_width >= WINXTERM_CLOSE_DIALOG_MIN_CLIENT_WIDTH &&
            client_width > max_client_width) {
            client_width = max_client_width;
        }
    }

    DWORD style = WS_CAPTION | WS_SYSMENU | WS_POPUP | WS_THICKFRAME;
    DWORD ex_style = WS_EX_DLGMODALFRAME;
    RECT dialog_rect = {0, 0, client_width, WINXTERM_CLOSE_DIALOG_MIN_CLIENT_HEIGHT};
    if (!AdjustWindowRectEx(&dialog_rect, style, FALSE, ex_style)) {
        dialog_rect.left = 0;
        dialog_rect.top = 0;
        dialog_rect.right = client_width;
        dialog_rect.bottom = WINXTERM_CLOSE_DIALOG_MIN_CLIENT_HEIGHT;
    }
    int width = dialog_rect.right - dialog_rect.left;
    int height = dialog_rect.bottom - dialog_rect.top;
    int x = parent_rect.left + ((parent_rect.right - parent_rect.left) - width) / 2;
    int y = parent_rect.top + ((parent_rect.bottom - parent_rect.top) - height) / 2;

    HWND hwnd = CreateWindowExW(ex_style,
                                L"WinxtermCloseDialog",
                                L"Close winxterm",
                                style,
                                x,
                                y,
                                width,
                                height,
                                app->hwnd,
                                0,
                                app->instance,
                                &dialog);
    if (hwnd == 0) {
        winxterm_log_writef(app->log, "close dialog creation failed; defaulting to terminate");
        return WINXTERM_CLOSE_DECISION_TERMINATE;
    }

    EnableWindow(app->hwnd, FALSE);
    ShowWindow(hwnd, SW_SHOWNORMAL);
    SetActiveWindow(hwnd);

    MSG message;
    while (!dialog.done) {
        BOOL result = GetMessageW(&message, 0, 0, 0);
        if (result <= 0) {
            dialog.done = true;
            dialog.decision = WINXTERM_CLOSE_DECISION_TERMINATE;
            if (result == 0) {
                PostQuitMessage((int)message.wParam);
            }
            break;
        }
        if (!IsDialogMessageW(hwnd, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    EnableWindow(app->hwnd, TRUE);
    SetActiveWindow(app->hwnd);
    return dialog.decision;
}

static bool winxterm_app_request_close(WinxtermApp *app, const char *reason)
{
    if (app == 0 || app->hwnd == 0 || app->closing) {
        return true;
    }

    WinxtermHostChildInfo child;
    memset(&child, 0, sizeof(child));
    bool has_child = app->bridge != 0 && winxterm_bridge_copy_child_info(app->bridge, &child);
    WinxtermCloseDecision decision = has_child ?
        winxterm_app_confirm_child_close(app, &child) : WINXTERM_CLOSE_DECISION_CLOSE;
    if (decision == WINXTERM_CLOSE_DECISION_CANCEL) {
        winxterm_log_writef(app->log, "%s: close cancelled", reason);
        return false;
    }

    app->closing = true;
    if (decision == WINXTERM_CLOSE_DECISION_HEADLESS) {
        winxterm_log_writef(app->log, "%s: keeping managed jobs running headless", reason);
        winxterm_bridge_request_headless(app->bridge);
    } else if (decision == WINXTERM_CLOSE_DECISION_TERMINATE) {
        winxterm_log_writef(app->log, "%s: terminating direct child", reason);
        winxterm_bridge_request_terminate(app->bridge);
        if (app->shutdown_event != 0) {
            SetEvent(app->shutdown_event);
        }
    } else if (app->shutdown_event != 0) {
        SetEvent(app->shutdown_event);
    }

    DestroyWindow(app->hwnd);
    return true;
}

static SIZE winxterm_app_min_window_size(WinxtermApp *app)
{
    SIZE size = {0, 0};
    if (app == 0 || app->hwnd == 0) {
        return size;
    }
    RECT rect = {0,
                 0,
                 winxterm_columns_to_physical_pixels(WINXTERM_MIN_COLUMNS,
                                                     winxterm_app_display_scale(app)),
                 winxterm_rows_to_physical_pixels(WINXTERM_MIN_ROWS,
                                                  winxterm_app_display_scale(app))};
    DWORD style = (DWORD)GetWindowLongPtrW(app->hwnd, GWL_STYLE);
    DWORD ex_style = (DWORD)GetWindowLongPtrW(app->hwnd, GWL_EXSTYLE);
    if (winxterm_app_adjust_window_rect(&rect, style, ex_style)) {
        size.cx = rect.right - rect.left;
        size.cy = rect.bottom - rect.top;
    }
    return size;
}

static void winxterm_app_handle_minmax(WinxtermApp *app, MINMAXINFO *info)
{
    if (app == 0 || info == 0) {
        return;
    }
    SIZE minimum = winxterm_app_min_window_size(app);
    if (minimum.cx > 0) {
        info->ptMinTrackSize.x = minimum.cx;
    }
    if (minimum.cy > 0) {
        info->ptMinTrackSize.y = minimum.cy;
    }
}

static void winxterm_app_snap_window_to_cells(WinxtermApp *app)
{
    if (app == 0 || app->hwnd == 0 || app->fullscreen) {
        return;
    }
    RECT client;
    RECT window;
    if (!GetClientRect(app->hwnd, &client) || !GetWindowRect(app->hwnd, &window)) {
        return;
    }
    int width = client.right - client.left;
    int height = client.bottom - client.top;
    int scaled_cell_width = winxterm_logical_to_physical_pixels(WINXTERM_CELL_WIDTH_PIXELS,
                                                                winxterm_app_display_scale(app));
    int scaled_cell_height = winxterm_logical_to_physical_pixels(WINXTERM_CELL_HEIGHT_PIXELS,
                                                                 winxterm_app_display_scale(app));
    int columns = scaled_cell_width > 0 ? (width + scaled_cell_width / 2) / scaled_cell_width : 0;
    int rows = scaled_cell_height > 0 ? (height + scaled_cell_height / 2) / scaled_cell_height : 0;
    if (columns < 1) {
        columns = 1;
    }
    if (rows < 1) {
        rows = 1;
    }
    RECT target = {0,
                   0,
                   winxterm_columns_to_physical_pixels(columns, winxterm_app_display_scale(app)),
                   winxterm_rows_to_physical_pixels(rows, winxterm_app_display_scale(app))};
    DWORD style = (DWORD)GetWindowLongPtrW(app->hwnd, GWL_STYLE);
    DWORD ex_style = (DWORD)GetWindowLongPtrW(app->hwnd, GWL_EXSTYLE);
    if (!winxterm_app_adjust_window_rect(&target, style, ex_style)) {
        return;
    }
    SetWindowPos(app->hwnd,
                 0,
                 0,
                 0,
                 target.right - target.left,
                 target.bottom - target.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER);
}

static void winxterm_app_apply_display_scale(WinxtermApp *app, unsigned int scale)
{
    if (app == 0 || app->hwnd == 0 || !winxterm_display_scale_valid(scale)) {
        return;
    }

    if (app->display_scale == scale) {
        winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
        return;
    }

    EnterCriticalSection(&app->bridge->screen_lock);
    (void)winxterm_app_capture_scale_anchor_locked(app);
    int columns = app->bridge->screen.columns;
    int rows = app->bridge->screen.rows;
    LeaveCriticalSection(&app->bridge->screen_lock);

    app->display_scale = scale;
    winxterm_log_writef(app->log, "display scale: %u", scale);

    if (app->fullscreen || IsZoomed(app->hwnd)) {
        winxterm_app_handle_size(app, app->last_size_kind);
        return;
    }

    if (columns < 1) {
        columns = 1;
    }
    if (rows < 1) {
        rows = 1;
    }

    RECT target = {0,
                   0,
                   winxterm_columns_to_physical_pixels(columns, scale),
                   winxterm_rows_to_physical_pixels(rows, scale)};
    DWORD style = (DWORD)GetWindowLongPtrW(app->hwnd, GWL_STYLE);
    DWORD ex_style = (DWORD)GetWindowLongPtrW(app->hwnd, GWL_EXSTYLE);
    if (!winxterm_app_adjust_window_rect(&target, style, ex_style)) {
        app->scale_resize_anchor_valid = false;
        winxterm_app_handle_size(app, app->last_size_kind);
        return;
    }

    SetWindowPos(app->hwnd,
                 0,
                 0,
                 0,
                 target.right - target.left,
                 target.bottom - target.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER);
}

static void winxterm_app_apply_scrollbar(WinxtermApp *app, bool enabled)
{
    if (app == 0 || app->hwnd == 0 || app->bridge == 0) {
        return;
    }

    if (app->scrollbar_enabled == enabled) {
        return;
    }

    EnterCriticalSection(&app->bridge->screen_lock);
    (void)winxterm_app_capture_scale_anchor_locked(app);
    int columns = app->bridge->screen.columns;
    int rows = app->bridge->screen.rows;
    LeaveCriticalSection(&app->bridge->screen_lock);

    app->scrollbar_enabled = enabled;
    winxterm_log_writef(app->log, "scrollbar: %s", enabled ? "on" : "off");

    WinxtermSettings settings;
    winxterm_settings_init(&settings);
    (void)winxterm_settings_load(&settings);
    settings.scrollbar = enabled;
    if (!winxterm_settings_save(&settings)) {
        winxterm_log_writef(app->log, "scrollbar: settings save failed");
    }

    DWORD style = (DWORD)GetWindowLongPtrW(app->hwnd, GWL_STYLE);
    DWORD new_style = enabled ? (style | WS_VSCROLL) : (style & ~(DWORD)WS_VSCROLL);
    if (new_style != style) {
        SetWindowLongPtrW(app->hwnd, GWL_STYLE, (LONG_PTR)new_style);
    }
    if (app->fullscreen) {
        app->windowed_style = enabled ? (app->windowed_style | WS_VSCROLL) :
                                        (app->windowed_style & ~(DWORD)WS_VSCROLL);
    }
    if (enabled) {
        winxterm_app_update_scrollbar(app);
    }

    if (app->fullscreen || IsZoomed(app->hwnd)) {
        SetWindowPos(app->hwnd,
                     0,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        winxterm_app_handle_size(app, app->last_size_kind);
        return;
    }

    if (columns < 1) {
        columns = 1;
    }
    if (rows < 1) {
        rows = 1;
    }
    RECT target = {0,
                   0,
                   winxterm_columns_to_physical_pixels(columns, winxterm_app_display_scale(app)),
                   winxterm_rows_to_physical_pixels(rows, winxterm_app_display_scale(app))};
    DWORD ex_style = (DWORD)GetWindowLongPtrW(app->hwnd, GWL_EXSTYLE);
    if (!winxterm_app_adjust_window_rect(&target, new_style, ex_style)) {
        app->scale_resize_anchor_valid = false;
        SetWindowPos(app->hwnd,
                     0,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        winxterm_app_handle_size(app, app->last_size_kind);
        return;
    }

    SetWindowPos(app->hwnd,
                 0,
                 0,
                 0,
                 target.right - target.left,
                 target.bottom - target.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    winxterm_app_handle_size(app, app->last_size_kind);
}

static void winxterm_app_toggle_fullscreen(WinxtermApp *app)
{
    if (app == 0 || app->hwnd == 0) {
        return;
    }

    if (!app->fullscreen) {
        app->windowed_style = (DWORD)GetWindowLongPtrW(app->hwnd, GWL_STYLE);
        app->windowed_ex_style = (DWORD)GetWindowLongPtrW(app->hwnd, GWL_EXSTYLE);
        app->windowed_placement.length = sizeof(app->windowed_placement);
        GetWindowPlacement(app->hwnd, &app->windowed_placement);

        MONITORINFO monitor;
        monitor.cbSize = sizeof(monitor);
        if (!GetMonitorInfoW(MonitorFromWindow(app->hwnd, MONITOR_DEFAULTTONEAREST), &monitor)) {
            return;
        }

        DWORD fullscreen_style = app->windowed_style & ~(DWORD)WS_OVERLAPPEDWINDOW;
        DWORD fullscreen_ex_style = app->windowed_ex_style &
                                    ~(DWORD)(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE |
                                             WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);

        SetWindowLongPtrW(app->hwnd, GWL_STYLE, (LONG_PTR)fullscreen_style);
        SetWindowLongPtrW(app->hwnd, GWL_EXSTYLE, (LONG_PTR)fullscreen_ex_style);
        SetWindowPos(app->hwnd,
                     HWND_TOP,
                     monitor.rcMonitor.left,
                     monitor.rcMonitor.top,
                     monitor.rcMonitor.right - monitor.rcMonitor.left,
                     monitor.rcMonitor.bottom - monitor.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        app->fullscreen = true;
        winxterm_log_writef(app->log, "fullscreen toggle: on");
        winxterm_app_log_client_size(app, "fullscreen");
    } else {
        SetWindowLongPtrW(app->hwnd, GWL_STYLE, (LONG_PTR)app->windowed_style);
        SetWindowLongPtrW(app->hwnd, GWL_EXSTYLE, (LONG_PTR)app->windowed_ex_style);
        SetWindowPlacement(app->hwnd, &app->windowed_placement);
        SetWindowPos(app->hwnd,
                     0,
                     0,
                     0,
                     0,
                     0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        app->fullscreen = false;
        winxterm_log_writef(app->log, "fullscreen toggle: off");
        winxterm_app_log_client_size(app, "restore");
    }
}

typedef struct WinxtermAppDumpBuffer {
    uint8_t *data;
    size_t count;
    size_t capacity;
} WinxtermAppDumpBuffer;

static bool winxterm_app_write_file_bytes(const wchar_t *path, const void *bytes, size_t byte_count)
{
    if (path == 0 || path[0] == L'\0' || (bytes == 0 && byte_count != 0u)) {
        return false;
    }
    HANDLE file = CreateFileW(path,
                              GENERIC_WRITE,
                              0,
                              0,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              0);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    const uint8_t *cursor = (const uint8_t *)bytes;
    size_t remaining = byte_count;
    bool ok = true;
    while (remaining != 0u) {
        DWORD chunk = remaining > 0x7fffffffu ? 0x7fffffffu : (DWORD)remaining;
        DWORD written = 0u;
        if (!WriteFile(file, cursor, chunk, &written, 0) || written != chunk) {
            ok = false;
            break;
        }
        cursor += written;
        remaining -= written;
    }
    CloseHandle(file);
    return ok;
}

static bool winxterm_app_path_has_suffix(const wchar_t *path, const wchar_t *suffix)
{
    if (path == 0 || suffix == 0) {
        return false;
    }
    size_t path_length = wcslen(path);
    size_t suffix_length = wcslen(suffix);
    return path_length >= suffix_length &&
        _wcsicmp(path + path_length - suffix_length, suffix) == 0;
}

static wchar_t *winxterm_app_screenshot_path(const wchar_t *path, bool append_bmp)
{
    if (path == 0 || path[0] == L'\0') {
        return 0;
    }
    size_t length = wcslen(path);
    size_t extra = append_bmp ? 4u : 0u;
    wchar_t *copy = (wchar_t *)calloc(length + extra + 1u, sizeof(*copy));
    if (copy == 0) {
        return 0;
    }
    memcpy(copy, path, length * sizeof(*copy));
    if (append_bmp) {
        memcpy(copy + length, L".bmp", 4u * sizeof(*copy));
    }
    return copy;
}

static bool winxterm_app_macro_write_bmp_screenshot(WinxtermApp *app, const wchar_t *path)
{
    if (app == 0 || path == 0 || app->front_pixels == 0 || app->bitmap_width <= 0 ||
        app->bitmap_height <= 0) {
        return false;
    }
    bool append_bmp = !winxterm_app_path_has_suffix(path, L".bmp");
    wchar_t *bmp_path = winxterm_app_screenshot_path(path, append_bmp);
    if (bmp_path == 0) {
        return false;
    }

    BITMAPFILEHEADER file_header;
    BITMAPINFOHEADER info_header;
    memset(&file_header, 0, sizeof(file_header));
    memset(&info_header, 0, sizeof(info_header));
    DWORD pixel_bytes = (DWORD)((size_t)app->bitmap_width * (size_t)app->bitmap_height * sizeof(uint32_t));
    file_header.bfType = 0x4d42u;
    file_header.bfOffBits = sizeof(file_header) + sizeof(info_header);
    file_header.bfSize = file_header.bfOffBits + pixel_bytes;
    info_header.biSize = sizeof(info_header);
    info_header.biWidth = app->bitmap_width;
    info_header.biHeight = -app->bitmap_height;
    info_header.biPlanes = 1u;
    info_header.biBitCount = 32u;
    info_header.biCompression = BI_RGB;
    info_header.biSizeImage = pixel_bytes;

    size_t total = sizeof(file_header) + sizeof(info_header) + (size_t)pixel_bytes;
    uint8_t *bytes = (uint8_t *)malloc(total);
    if (bytes == 0) {
        free(bmp_path);
        return false;
    }
    memcpy(bytes, &file_header, sizeof(file_header));
    memcpy(bytes + sizeof(file_header), &info_header, sizeof(info_header));
    memcpy(bytes + sizeof(file_header) + sizeof(info_header), app->front_pixels, pixel_bytes);
    bool ok = winxterm_app_write_file_bytes(bmp_path, bytes, total);
    free(bytes);
    free(bmp_path);
    return ok;
}

static uint8_t *winxterm_app_copy_screenshot_bgr24(const uint32_t *pixels,
                                                   int width,
                                                   int height,
                                                   UINT *stride,
                                                   UINT *byte_count)
{
    if (pixels == 0 || width <= 0 || height <= 0 || stride == 0 || byte_count == 0) {
        return 0;
    }
    size_t width_size = (size_t)width;
    size_t height_size = (size_t)height;
    if (width_size > SIZE_MAX / 3u) {
        return 0;
    }
    size_t stride_size = width_size * 3u;
    if (stride_size > UINT_MAX || height_size > SIZE_MAX / stride_size) {
        return 0;
    }
    size_t total_size = stride_size * height_size;
    if (total_size > UINT_MAX) {
        return 0;
    }

    uint8_t *copy = (uint8_t *)malloc(total_size);
    if (copy == 0) {
        return 0;
    }
    for (int row = 0; row < height; ++row) {
        const uint32_t *src = pixels + (size_t)row * (size_t)width;
        uint8_t *dst = copy + (size_t)row * stride_size;
        for (int col = 0; col < width; ++col) {
            uint32_t pixel = src[col];
            dst[(size_t)col * 3u] = (uint8_t)(pixel & 0xffu);
            dst[(size_t)col * 3u + 1u] = (uint8_t)((pixel >> 8) & 0xffu);
            dst[(size_t)col * 3u + 2u] = (uint8_t)((pixel >> 16) & 0xffu);
        }
    }

    *stride = (UINT)stride_size;
    *byte_count = (UINT)total_size;
    return copy;
}

static bool winxterm_app_macro_write_png_screenshot(WinxtermApp *app, const wchar_t *path)
{
    if (app == 0 || path == 0 || path[0] == L'\0' || app->front_pixels == 0 ||
        app->bitmap_width <= 0 || app->bitmap_height <= 0) {
        return false;
    }
    if ((size_t)app->bitmap_width > UINT_MAX || (size_t)app->bitmap_height > UINT_MAX) {
        return false;
    }

    UINT stride = 0u;
    UINT byte_count = 0u;
    uint8_t *pixels = winxterm_app_copy_screenshot_bgr24(app->front_pixels,
                                                         app->bitmap_width,
                                                         app->bitmap_height,
                                                         &stride,
                                                         &byte_count);
    if (pixels == 0) {
        return false;
    }

    HRESULT com_result = CoInitializeEx(0, COINIT_APARTMENTTHREADED);
    bool uninitialize_com = SUCCEEDED(com_result);
    if (FAILED(com_result) && com_result != RPC_E_CHANGED_MODE) {
        free(pixels);
        return false;
    }

    IWICImagingFactory *factory = 0;
    IWICStream *stream = 0;
    IWICBitmapEncoder *encoder = 0;
    IWICBitmapFrameEncode *frame = 0;
    IPropertyBag2 *property_bag = 0;

    HRESULT hr = CoCreateInstance(&CLSID_WICImagingFactory,
                                  0,
                                  CLSCTX_INPROC_SERVER,
                                  &IID_IWICImagingFactory,
                                  (void **)&factory);
    if (SUCCEEDED(hr)) {
        hr = factory->lpVtbl->CreateStream(factory, &stream);
    }
    if (SUCCEEDED(hr)) {
        hr = stream->lpVtbl->InitializeFromFilename(stream, path, GENERIC_WRITE);
    }
    if (SUCCEEDED(hr)) {
        hr = factory->lpVtbl->CreateEncoder(factory, &GUID_ContainerFormatPng, 0, &encoder);
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->lpVtbl->Initialize(encoder, (IStream *)stream, WICBitmapEncoderNoCache);
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->lpVtbl->CreateNewFrame(encoder, &frame, &property_bag);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->lpVtbl->Initialize(frame, property_bag);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->lpVtbl->SetSize(frame, (UINT)app->bitmap_width, (UINT)app->bitmap_height);
    }
    if (SUCCEEDED(hr)) {
        WICPixelFormatGUID pixel_format = GUID_WICPixelFormat24bppBGR;
        hr = frame->lpVtbl->SetPixelFormat(frame, &pixel_format);
        if (SUCCEEDED(hr) && memcmp(&pixel_format, &GUID_WICPixelFormat24bppBGR, sizeof(pixel_format)) != 0) {
            hr = E_FAIL;
        }
    }
    if (SUCCEEDED(hr)) {
        hr = frame->lpVtbl->WritePixels(frame, (UINT)app->bitmap_height, stride, byte_count, pixels);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->lpVtbl->Commit(frame);
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->lpVtbl->Commit(encoder);
    }

    if (property_bag != 0) {
        property_bag->lpVtbl->Release(property_bag);
    }
    if (frame != 0) {
        frame->lpVtbl->Release(frame);
    }
    if (encoder != 0) {
        encoder->lpVtbl->Release(encoder);
    }
    if (stream != 0) {
        stream->lpVtbl->Release(stream);
    }
    if (factory != 0) {
        factory->lpVtbl->Release(factory);
    }
    if (uninitialize_com) {
        CoUninitialize();
    }
    free(pixels);
    return SUCCEEDED(hr);
}

static bool winxterm_app_macro_write_screenshot(void *context, const wchar_t *path)
{
    WinxtermApp *app = (WinxtermApp *)context;
    if (app == 0 || app->front_pixels == 0 || app->bitmap_width <= 0 || app->bitmap_height <= 0) {
        return false;
    }
    if (winxterm_app_path_has_suffix(path, L".png")) {
        return winxterm_app_macro_write_png_screenshot(app, path);
    }
    return winxterm_app_macro_write_bmp_screenshot(app, path);
}

static bool winxterm_app_dump_buffer_reserve(WinxtermAppDumpBuffer *buffer, size_t additional)
{
    if (buffer == 0 || additional > SIZE_MAX - buffer->count - 1u) {
        return false;
    }
    size_t needed = buffer->count + additional + 1u;
    if (needed <= buffer->capacity) {
        return true;
    }
    size_t new_capacity = buffer->capacity == 0u ? 1024u : buffer->capacity;
    while (new_capacity < needed) {
        size_t doubled = new_capacity * 2u;
        if (doubled <= new_capacity) {
            new_capacity = needed;
            break;
        }
        new_capacity = doubled;
    }
    uint8_t *new_data = (uint8_t *)realloc(buffer->data, new_capacity);
    if (new_data == 0) {
        return false;
    }
    buffer->data = new_data;
    buffer->capacity = new_capacity;
    return true;
}

static bool winxterm_app_dump_buffer_append_byte(WinxtermAppDumpBuffer *buffer, uint8_t byte)
{
    if (!winxterm_app_dump_buffer_reserve(buffer, 1u)) {
        return false;
    }
    buffer->data[buffer->count++] = byte;
    buffer->data[buffer->count] = 0u;
    return true;
}

static bool winxterm_app_dump_buffer_append_codepoint(WinxtermAppDumpBuffer *buffer, uint32_t codepoint)
{
    if (codepoint <= 0x7fu) {
        return winxterm_app_dump_buffer_append_byte(buffer, (uint8_t)codepoint);
    }
    if (codepoint <= 0x7ffu) {
        return winxterm_app_dump_buffer_append_byte(buffer, (uint8_t)(0xc0u | (codepoint >> 6))) &&
               winxterm_app_dump_buffer_append_byte(buffer, (uint8_t)(0x80u | (codepoint & 0x3fu)));
    }
    if (codepoint <= 0xffffu) {
        return winxterm_app_dump_buffer_append_byte(buffer, (uint8_t)(0xe0u | (codepoint >> 12))) &&
               winxterm_app_dump_buffer_append_byte(buffer, (uint8_t)(0x80u | ((codepoint >> 6) & 0x3fu))) &&
               winxterm_app_dump_buffer_append_byte(buffer, (uint8_t)(0x80u | (codepoint & 0x3fu)));
    }
    if (codepoint <= 0x10ffffu) {
        return winxterm_app_dump_buffer_append_byte(buffer, (uint8_t)(0xf0u | (codepoint >> 18))) &&
               winxterm_app_dump_buffer_append_byte(buffer, (uint8_t)(0x80u | ((codepoint >> 12) & 0x3fu))) &&
               winxterm_app_dump_buffer_append_byte(buffer, (uint8_t)(0x80u | ((codepoint >> 6) & 0x3fu))) &&
               winxterm_app_dump_buffer_append_byte(buffer, (uint8_t)(0x80u | (codepoint & 0x3fu)));
    }
    return false;
}

static bool winxterm_app_dump_row(WinxtermAppDumpBuffer *buffer, const WinxtermScreenRowView *row)
{
    if (buffer == 0 || row == 0 || row->cells == 0 || row->columns <= 0) {
        return true;
    }
    int last = row->columns - 1;
    while (last >= 0) {
        const WinxtermScreenCell *cell = row->cells + last;
        if (!cell->continuation &&
            (cell->attribute_flags & WINXTERM_SCREEN_CELL_INVISIBLE) == 0u &&
            cell->codepoint != (uint32_t)' ') {
            break;
        }
        --last;
    }
    for (int column = 0; column <= last; ++column) {
        const WinxtermScreenCell *cell = row->cells + column;
        if (cell->continuation || (cell->attribute_flags & WINXTERM_SCREEN_CELL_INVISIBLE) != 0u) {
            continue;
        }
        if (!winxterm_app_dump_buffer_append_codepoint(buffer, cell->codepoint)) {
            return false;
        }
        for (uint8_t i = 0u; i < cell->combining_count; ++i) {
            if (!winxterm_app_dump_buffer_append_codepoint(buffer, cell->combining_codepoints[i])) {
                return false;
            }
        }
    }
    return winxterm_app_dump_buffer_append_byte(buffer, '\r') &&
           winxterm_app_dump_buffer_append_byte(buffer, '\n');
}

static bool winxterm_app_dump_row_exact(WinxtermAppDumpBuffer *buffer,
                                        const WinxtermScreenRowView *row)
{
    if (buffer == 0 || row == 0 || row->cells == 0 || row->columns <= 0) {
        return true;
    }
    for (int column = 0; column < row->columns; ++column) {
        const WinxtermScreenCell *cell = row->cells + column;
        if (cell->continuation ||
            (cell->attribute_flags & WINXTERM_SCREEN_CELL_INVISIBLE) != 0u) {
            if (!winxterm_app_dump_buffer_append_byte(buffer, ' ')) return false;
            continue;
        }
        if (!winxterm_app_dump_buffer_append_codepoint(buffer, cell->codepoint)) return false;
        for (uint8_t i = 0u; i < cell->combining_count; ++i) {
            if (!winxterm_app_dump_buffer_append_codepoint(buffer,
                                                           cell->combining_codepoints[i])) {
                return false;
            }
        }
    }
    return winxterm_app_dump_buffer_append_byte(buffer, '\r') &&
           winxterm_app_dump_buffer_append_byte(buffer, '\n');
}

static bool winxterm_app_macro_write_grid_dump(void *context,
                                               const wchar_t *path,
                                               bool exact)
{
    WinxtermApp *app = (WinxtermApp *)context;
    if (app == 0 || app->bridge == 0 || path == 0 || path[0] == L'\0') {
        return false;
    }
    WinxtermAppDumpBuffer buffer;
    memset(&buffer, 0, sizeof(buffer));
    bool ok = true;
    EnterCriticalSection(&app->bridge->screen_lock);
    WinxtermCellSize visible = winxterm_app_visible_cells_locked(app);
    if (app->bridge->screen.alternate_active) {
        int rows = visible.rows < app->bridge->screen.rows ? visible.rows : app->bridge->screen.rows;
        for (int row_index = 0; ok && row_index < rows; ++row_index) {
            WinxtermScreenRowView row;
            if (winxterm_screen_get_alternate_view_row(&app->bridge->screen, (size_t)row_index, &row)) {
                ok = exact ? winxterm_app_dump_row_exact(&buffer, &row) :
                             winxterm_app_dump_row(&buffer, &row);
            }
        }
    } else {
        size_t first_row =
            winxterm_ux_primary_first_row_for_rows(&app->ux, &app->bridge->screen, visible.rows);
        size_t row_count = winxterm_screen_primary_view_row_count(&app->bridge->screen);
        for (int row_index = 0; ok && row_index < visible.rows; ++row_index) {
            size_t global_row = first_row + (size_t)row_index;
            if (global_row >= row_count) {
                break;
            }
            WinxtermScreenRowView row;
            if (winxterm_screen_get_primary_view_row(&app->bridge->screen, global_row, &row)) {
                ok = exact ? winxterm_app_dump_row_exact(&buffer, &row) :
                             winxterm_app_dump_row(&buffer, &row);
            }
        }
    }
    LeaveCriticalSection(&app->bridge->screen_lock);
    if (ok) {
        ok = winxterm_app_write_file_bytes(path, buffer.data, buffer.count);
    }
    free(buffer.data);
    return ok;
}

static bool winxterm_app_macro_write_screendump(void *context, const wchar_t *path)
{
    return winxterm_app_macro_write_grid_dump(context, path, false);
}

static bool winxterm_app_macro_write_celldump(void *context, const wchar_t *path)
{
    return winxterm_app_macro_write_grid_dump(context, path, true);
}

static bool winxterm_app_macro_write_histdump(void *context, const wchar_t *path)
{
    WinxtermApp *app = (WinxtermApp *)context;
    if (app == 0 || app->bridge == 0 || path == 0 || path[0] == L'\0') {
        return false;
    }
    uint8_t *bytes = 0;
    size_t byte_count = 0u;
    if (!winxterm_bridge_copy_transcript(app->bridge, &bytes, &byte_count)) {
        return false;
    }
    bool ok = winxterm_app_write_file_bytes(path, bytes, byte_count);
    free(bytes);
    return ok;
}

static bool winxterm_app_macro_redraw_ready(const WinxtermApp *app)
{
    if (app == 0 || app->bridge == 0) {
        return false;
    }

    bool pending = app->pending_resize ||
                   app->render_damage_valid ||
                   app->frame_timer_id != 0u ||
                   app->deferred_frame_causes != WINXTERM_FRAME_CAUSE_NONE;
    EnterCriticalSection(&app->bridge->input_lock);
    pending = pending ||
              app->bridge->input_count != 0u ||
              app->bridge->output_count != 0u ||
              app->bridge->pending_resize ||
              app->bridge->pending_scale ||
              app->bridge->render_update_pending ||
              app->bridge->pending_frame_causes != WINXTERM_FRAME_CAUSE_NONE;
    LeaveCriticalSection(&app->bridge->input_lock);
    return !pending;
}

static bool winxterm_app_macro_render_barrier(void *context)
{
    WinxtermApp *app = (WinxtermApp *)context;
    if (app == 0 || app->bridge == 0 || app->hwnd == 0) {
        return false;
    }
    for (unsigned int i = 0u; i < 256u; ++i) {
        bool pending = app->pending_resize;
        EnterCriticalSection(&app->bridge->input_lock);
        pending = pending || app->bridge->output_count != 0u || app->bridge->pending_resize;
        LeaveCriticalSection(&app->bridge->input_lock);
        app->last_frame_tick = 0u;
        winxterm_app_handle_frame_due(app,
                                      app->hwnd,
                                      WINXTERM_FRAME_CAUSE_CONTENT |
                                          WINXTERM_FRAME_CAUSE_RESIZE |
                                          WINXTERM_FRAME_CAUSE_PRESENTATION);
        if (!pending) {
            break;
        }
    }
    UpdateWindow(app->hwnd);
    return true;
}

static bool winxterm_app_macro_wait_redraw(void *context, bool process, bool *ready)
{
    WinxtermApp *app = (WinxtermApp *)context;
    if (ready != 0) {
        *ready = false;
    }
    if (app == 0 || app->bridge == 0 || ready == 0) {
        return false;
    }
    if (process && !winxterm_app_macro_render_barrier(context)) {
        return false;
    }
    *ready = winxterm_app_macro_redraw_ready(app);
    return true;
}

static bool winxterm_app_macro_wait_host(void *context, bool *ready)
{
    WinxtermApp *app = (WinxtermApp *)context;
    if (ready != 0) {
        *ready = false;
    }
    if (app == 0 || app->bridge == 0 || ready == 0) {
        return false;
    }
    WinxtermHostChildInfo child;
    memset(&child, 0, sizeof(child));
    *ready = winxterm_bridge_copy_child_info(app->bridge, &child);
    return true;
}

static bool winxterm_app_macro_queue_char(void *context,
                                          wchar_t ch,
                                          WinxtermInputModifiers modifiers)
{
    WinxtermApp *app = (WinxtermApp *)context;
    uint8_t sequence[WINXTERM_INPUT_MAX_SEQUENCE];
    size_t length = winxterm_input_encode_char(ch, modifiers, sequence, sizeof(sequence));
    return length != 0u && winxterm_app_queue_input(app, sequence, length);
}

static bool winxterm_app_macro_queue_key_down(void *context,
                                              WPARAM virtual_key,
                                              WinxtermInputModifiers modifiers)
{
    WinxtermApp *app = (WinxtermApp *)context;
    if (app == 0) {
        return false;
    }
    if (modifiers.alt && virtual_key == VK_RETURN) {
        winxterm_app_toggle_fullscreen(app);
        return true;
    }
    if (virtual_key == VK_LSHIFT || virtual_key == VK_RSHIFT ||
        virtual_key == VK_LCONTROL || virtual_key == VK_RCONTROL ||
        virtual_key == VK_LMENU || virtual_key == VK_RMENU) {
        return true;
    }
    if (virtual_key == VK_RETURN) {
        modifiers.alt = false;
        return winxterm_app_macro_queue_char(context, L'\r', modifiers);
    }
    if (virtual_key == VK_BACK) {
        if (modifiers.ctrl) {
            uint8_t sequence[WINXTERM_INPUT_MAX_SEQUENCE];
            size_t length = winxterm_input_encode_virtual_key(virtual_key,
                                                              modifiers,
                                                              sequence,
                                                              sizeof(sequence));
            return length != 0u && winxterm_app_queue_input(app, sequence, length);
        }
        return winxterm_app_macro_queue_char(context, L'\b', modifiers);
    }
    if (virtual_key == VK_ESCAPE) {
        return winxterm_app_macro_queue_char(context, 0x1b, modifiers);
    }

    uint8_t sequence[WINXTERM_INPUT_MAX_SEQUENCE];
    WinxtermModeState modes;
    memset(&modes, 0, sizeof(modes));
    (void)winxterm_bridge_copy_mode_state(app->bridge, &modes);
    size_t length = winxterm_input_encode_virtual_key_with_modes(virtual_key,
                                                                 modifiers,
                                                                 &modes,
                                                                 sequence,
                                                                 sizeof(sequence));
    return length != 0u && winxterm_app_queue_input(app, sequence, length);
}

static bool winxterm_app_macro_queue_key_up(void *context,
                                            WPARAM virtual_key,
                                            WinxtermInputModifiers modifiers)
{
    (void)context;
    (void)virtual_key;
    (void)modifiers;
    return true;
}

static void winxterm_app_macro_show_window(void *context, int show_command)
{
    WinxtermApp *app = (WinxtermApp *)context;
    if (app == 0 || app->hwnd == 0) {
        return;
    }
    ShowWindow(app->hwnd, show_command);
    winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_RESIZE | WINXTERM_FRAME_CAUSE_PRESENTATION);
}

static void winxterm_app_macro_request_exit(void *context)
{
    WinxtermApp *app = (WinxtermApp *)context;
    if (app != 0) {
        (void)winxterm_app_request_close(app, "macro exit");
    }
}

static void winxterm_app_macro_log_error(void *context, const wchar_t *message)
{
    WinxtermApp *app = (WinxtermApp *)context;
    if (app != 0 && message != 0) {
        winxterm_log_writef(app->log, "%ls", message);
    }
}

static void winxterm_app_schedule_macro(WinxtermApp *app, DWORD delay_ms)
{
    if (app == 0 || app->hwnd == 0 || delay_ms == WINXTERM_MACRO_DONE_DELAY) {
        return;
    }
    if (app->macro_timer_id != 0u) {
        KillTimer(app->hwnd, app->macro_timer_id);
        app->macro_timer_id = 0u;
    }
    if (delay_ms == 0u) {
        PostMessageW(app->hwnd, WINXTERM_WM_MACRO_UPDATE, 0, 0);
        return;
    }
    app->macro_timer_id = SetTimer(app->hwnd, WINXTERM_MACRO_TIMER_ID, delay_ms, 0);
    if (app->macro_timer_id == 0u) {
        PostMessageW(app->hwnd, WINXTERM_WM_MACRO_UPDATE, 0, 0);
    }
}

static bool winxterm_app_write_terminal_wide_line(WinxtermApp *app, const wchar_t *message)
{
    if (app == 0 || app->bridge == 0 || message == 0) {
        return false;
    }
    int required = WideCharToMultiByte(CP_UTF8, 0, message, -1, 0, 0, 0, 0);
    if (required <= 0) {
        return false;
    }
    static const char prefix[] = "\r\n";
    static const char suffix[] = "\r\n";
    size_t prefix_length = sizeof(prefix) - 1u;
    size_t suffix_length = sizeof(suffix) - 1u;
    size_t message_length = (size_t)required - 1u;
    char *utf8 = (char *)malloc(prefix_length + message_length + suffix_length + 1u);
    if (utf8 == 0) {
        return false;
    }
    memcpy(utf8, prefix, prefix_length);
    int written = WideCharToMultiByte(CP_UTF8, 0, message, -1, utf8 + prefix_length, required, 0, 0);
    if (written <= 0) {
        free(utf8);
        return false;
    }
    memcpy(utf8 + prefix_length + message_length, suffix, suffix_length);
    utf8[prefix_length + message_length + suffix_length] = '\0';
    bool ok = winxterm_bridge_enqueue_output(app->bridge,
                                             (const uint8_t *)utf8,
                                             prefix_length + message_length + suffix_length);
    free(utf8);
    return ok;
}

static void winxterm_app_handle_macro_update(WinxtermApp *app)
{
    if (app == 0 || app->macro == 0 || app->bridge == 0) {
        return;
    }
    if (app->macro_timer_id != 0u) {
        KillTimer(app->hwnd, app->macro_timer_id);
        app->macro_timer_id = 0u;
    }

    wchar_t *path = 0;
    while ((path = winxterm_bridge_take_macro_request(app->bridge)) != 0) {
        wchar_t error[WINXTERM_LOG_PATH_CAPACITY + 64u];
        error[0] = L'\0';
        if (!winxterm_macro_load_file(app->macro, path, error, sizeof(error) / sizeof(error[0]))) {
            winxterm_log_writef(app->log,
                                "macro load failed path=%ls error=%ls",
                                path,
                                error[0] != L'\0' ? error : L"(unknown)");
            if (error[0] != L'\0') {
                (void)winxterm_app_write_terminal_wide_line(app, error);
                (void)winxterm_app_macro_render_barrier(app);
            }
        } else {
            winxterm_log_writef(app->log, "macro started path=%ls", path);
        }
        free(path);
    }

    if (!winxterm_macro_running(app->macro)) {
        return;
    }
    WinxtermMacroCallbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.context = app;
    callbacks.queue_char = winxterm_app_macro_queue_char;
    callbacks.key_down = winxterm_app_macro_queue_key_down;
    callbacks.key_up = winxterm_app_macro_queue_key_up;
    callbacks.write_screenshot = winxterm_app_macro_write_screenshot;
    callbacks.write_screendump = winxterm_app_macro_write_screendump;
    callbacks.write_celldump = winxterm_app_macro_write_celldump;
    callbacks.write_histdump = winxterm_app_macro_write_histdump;
    callbacks.wait_redraw = winxterm_app_macro_wait_redraw;
    callbacks.wait_host = winxterm_app_macro_wait_host;
    callbacks.render_barrier = winxterm_app_macro_render_barrier;
    callbacks.show_window = winxterm_app_macro_show_window;
    callbacks.request_exit = winxterm_app_macro_request_exit;
    callbacks.log_error = winxterm_app_macro_log_error;
    DWORD delay = winxterm_macro_step(app->macro, &callbacks);
    if (delay != WINXTERM_MACRO_DONE_DELAY && winxterm_macro_running(app->macro)) {
        winxterm_app_schedule_macro(app, delay);
    }
}

static void winxterm_app_paint(WinxtermApp *app)
{
    PAINTSTRUCT paint;
    HDC dc = BeginPaint(app->hwnd, &paint);
    if (dc == 0) {
        return;
    }

    RECT client;
    if (!GetClientRect(app->hwnd, &client)) {
        client.left = 0;
        client.top = 0;
        client.right = 0;
        client.bottom = 0;
    }
    uint32_t background_rgb = winxterm_app_paint_background_rgb(app);
    if (app->front_pixels != 0 && app->bitmap_width > 0 && app->bitmap_height > 0) {
        BITMAPINFO info;
        memset(&info, 0, sizeof(info));
        info.bmiHeader.biSize = sizeof(info.bmiHeader);
        info.bmiHeader.biWidth = app->bitmap_width;
        info.bmiHeader.biHeight = -app->bitmap_height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        info.bmiHeader.biSizeImage =
            (DWORD)((size_t)app->bitmap_width * (size_t)app->bitmap_height * WINXTERM_BYTES_PER_PIXEL);

        unsigned int scale = winxterm_app_display_scale(app);
        int dest_width = app->bitmap_width;
        int dest_height = app->bitmap_height;
        if (scale == WINXTERM_DEFAULT_DISPLAY_SCALE) {
            StretchDIBits(dc,
                          0,
                          0,
                          dest_width,
                          dest_height,
                          0,
                          0,
                          app->bitmap_width,
                          app->bitmap_height,
                          app->front_pixels,
                          &info,
                          DIB_RGB_COLORS,
                          SRCCOPY);
        } else {
            SetStretchBltMode(dc, COLORONCOLOR);
            dest_width = winxterm_logical_to_physical_pixels(app->bitmap_width, scale);
            dest_height = winxterm_logical_to_physical_pixels(app->bitmap_height, scale);
            StretchDIBits(dc,
                          0,
                          0,
                          dest_width,
                          dest_height,
                          0,
                          0,
                          app->bitmap_width,
                          app->bitmap_height,
                          app->front_pixels,
                          &info,
                          DIB_RGB_COLORS,
                          SRCCOPY);
        }
        if (dest_width < client.right) {
            RECT gutter;
            gutter.left = dest_width;
            gutter.top = 0;
            gutter.right = client.right;
            gutter.bottom = client.bottom;
            winxterm_app_fill_rect_rgb(dc, &gutter, background_rgb);
        }
        if (dest_height < client.bottom) {
            RECT gutter;
            gutter.left = 0;
            gutter.top = dest_height;
            gutter.right = winxterm_app_min_int(dest_width, client.right);
            gutter.bottom = client.bottom;
            winxterm_app_fill_rect_rgb(dc, &gutter, background_rgb);
        }
    } else {
        winxterm_app_fill_rect_rgb(dc, &paint.rcPaint, background_rgb);
    }

    EndPaint(app->hwnd, &paint);
    if (app->render_present_pending) {
        app->render_present_pending = false;
        winxterm_bridge_mark_painted(app->bridge);
    }
}

static LRESULT CALLBACK winxterm_job_view_proc(HWND hwnd, UINT message,
                                               WPARAM wparam, LPARAM lparam)
{
    HWND edit = (HWND)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    switch (message) {
    case WM_CREATE: {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lparam;
        const wchar_t *text = create != 0 ? (const wchar_t *)create->lpCreateParams : L"";
        edit = CreateWindowExW(WS_EX_CLIENTEDGE,
                               L"EDIT",
                               text != 0 ? text : L"",
                               WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
                                   ES_LEFT | ES_MULTILINE | ES_AUTOVSCROLL |
                                   ES_AUTOHSCROLL | ES_READONLY,
                               0, 0, 0, 0,
                               hwnd, 0, create != 0 ? create->hInstance : 0, 0);
        if (edit == 0) return -1;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)edit);
        SendMessageW(edit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
        return 0;
    }
    case WM_SIZE:
        if (edit != 0) {
            MoveWindow(edit, 0, 0, LOWORD(lparam), HIWORD(lparam), TRUE);
        }
        return 0;
    case WM_SETFOCUS:
        if (edit != 0) SetFocus(edit);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

static bool winxterm_app_register_job_view_class(HINSTANCE instance)
{
    static const wchar_t class_name[] = L"WinxtermJobView";
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = winxterm_job_view_proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(0, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = class_name;
    ATOM atom = RegisterClassExW(&wc);
    return atom != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

static void winxterm_app_open_job_view(WinxtermApp *app)
{
    uint64_t job_id = 0u;
    uint8_t *bytes = 0;
    size_t byte_count = 0u;
    if (!winxterm_bridge_take_job_view(app->bridge, &job_id, &bytes, &byte_count)) return;
    uint8_t *plain = (uint8_t *)malloc(byte_count + 1u);
    size_t out = 0u;
    bool escape = false, osc = false, osc_esc = false;
    if (plain != 0) {
        for (size_t i = 0u; i < byte_count; ++i) {
            uint8_t ch = bytes[i];
            if (osc) {
                if (ch == 0x07u || (osc_esc && ch == '\\')) osc = false;
                osc_esc = ch == 0x1bu;
                continue;
            }
            if (escape) {
                if (ch == ']') { osc = true; escape = false; continue; }
                if (ch >= 0x40u && ch <= 0x7eu) escape = false;
                continue;
            }
            if (ch == 0x1bu) { escape = true; continue; }
            if (ch == '\r') {
                if (i + 1u < byte_count && bytes[i + 1u] == '\n') continue;
                ch = '\n';
            }
            if (ch < 0x20u && ch != '\n' && ch != '\t') continue;
            plain[out++] = ch;
        }
        plain[out] = '\0';
    }
    free(bytes);
    int wide_count = plain != 0 ? MultiByteToWideChar(CP_UTF8, 0, (const char *)plain,
                                                       (int)out, 0, 0) : 0;
    wchar_t *wide = (wchar_t *)calloc((size_t)(wide_count > 0 ? wide_count : 1) + 1u,
                                      sizeof(*wide));
    if (wide != 0 && wide_count > 0) {
        (void)MultiByteToWideChar(CP_UTF8, 0, (const char *)plain, (int)out,
                                  wide, wide_count);
    }
    free(plain);
    if (wide == 0) return;
    wchar_t title[96];
    (void)_snwprintf_s(title, sizeof(title) / sizeof(title[0]), _TRUNCATE,
                       L"Job %llu View", (unsigned long long)job_id);
    if (!winxterm_app_register_job_view_class(app->instance)) {
        free(wide);
        winxterm_log_writef(app->log, "job view class registration failed, error=%lu",
                            (unsigned long)GetLastError());
        return;
    }
    RECT owner = {0, 0, 800, 600};
    (void)GetWindowRect(app->hwnd, &owner);
    int width = owner.right - owner.left;
    int height = owner.bottom - owner.top;
    if (width <= 0) width = 800;
    if (height <= 0) height = 600;
    HWND view = CreateWindowExW(WS_EX_TOOLWINDOW,
                                L"WinxtermJobView", title,
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                owner.left, owner.top, width, height,
                                app->hwnd, 0, app->instance, wide);
    free(wide);
    if (view != 0) {
        bool maximize = app->fullscreen || IsZoomed(app->hwnd) != 0;
        ShowWindow(view, maximize ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
        UpdateWindow(view);
    }
}

static LRESULT CALLBACK winxterm_window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    WinxtermApp *app = (WinxtermApp *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    if (message == WM_NCCREATE) {
        CREATESTRUCTW *create = (CREATESTRUCTW *)lparam;
        app = (WinxtermApp *)create->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
        app->hwnd = hwnd;
    }

    if (app == 0) {
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

    switch (message) {
    case WINXTERM_WM_JOB_VIEW:
        winxterm_app_open_job_view(app);
        return 0;

    case WINXTERM_WM_SCALE_UPDATE: {
        unsigned int scale = WINXTERM_DEFAULT_DISPLAY_SCALE;
        while (winxterm_bridge_take_pending_display_scale(app->bridge, &scale)) {
            winxterm_app_apply_display_scale(app, scale);
        }
        return 0;
    }

    case WINXTERM_WM_SCROLLBAR_UPDATE: {
        bool scrollbar_enabled = false;
        while (winxterm_bridge_take_pending_scrollbar(app->bridge, &scrollbar_enabled)) {
            winxterm_app_apply_scrollbar(app, scrollbar_enabled);
        }
        return 0;
    }

    case WM_VSCROLL:
        winxterm_app_handle_vscroll(app, wparam);
        return 0;

    case WM_GETMINMAXINFO:
        winxterm_app_handle_minmax(app, (MINMAXINFO *)lparam);
        return 0;

    case WM_SIZE:
        winxterm_app_handle_size(app, wparam);
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_EXITSIZEMOVE:
        winxterm_app_snap_window_to_cells(app);
        if (app->pending_resize) {
            app->last_frame_tick = 0u;
            winxterm_app_handle_frame_due(app, hwnd, WINXTERM_FRAME_CAUSE_RESIZE);
        }
        return 0;

    case WM_CAPTURECHANGED:
        if ((HWND)lparam != hwnd) {
            winxterm_app_cancel_alt_drag(app);
        }
        return 0;

    case WM_CANCELMODE:
        winxterm_app_cancel_alt_drag(app);
        return 0;

    case WM_DPICHANGED:
        if (lparam != 0) {
            RECT *suggested = (RECT *)lparam;
            SetWindowPos(hwnd,
                         0,
                         suggested->left,
                         suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOOWNERZORDER);
        }
        winxterm_log_writef(app->log, "dpi changed: dpi=%u", (unsigned int)HIWORD(wparam));
        return 0;

    case WM_SETFOCUS:
        app->cursor_visible = true;
        winxterm_app_send_focus_report(app, true);
        winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
        return 0;

    case WM_KILLFOCUS:
        app->cursor_visible = false;
        winxterm_app_send_focus_report(app, false);
        winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
        return 0;

    case WM_TIMER:
        if (wparam == app->cursor_timer_id) {
            app->cursor_visible = !app->cursor_visible;
            winxterm_app_request_frame(app, WINXTERM_FRAME_CAUSE_PRESENTATION);
            return 0;
        }
        if (wparam == app->frame_timer_id) {
            winxterm_app_handle_frame_due(app, hwnd, WINXTERM_FRAME_CAUSE_NONE);
            return 0;
        }
        if (wparam == app->macro_timer_id) {
            winxterm_app_handle_macro_update(app);
            return 0;
        }
        if (wparam == app->click_event_timer_id) {
            winxterm_app_handle_click_logic_oneshot(app);
            return 0;
        }
        if (wparam == app->copy_overlay_timer_id) {
            winxterm_app_tick_copy_overlay(app);
            return 0;
        }
        if (wparam == app->bell_timer_id) {
            if (!winxterm_ux_bell_active(&app->ux, GetTickCount())) {
                KillTimer(hwnd, app->bell_timer_id);
                app->bell_timer_id = 0u;
                app->ux.bell.active = false;
            }
            winxterm_app_update_title(app);
            return 0;
        }
        break;

    case WM_CHAR:
        if (winxterm_app_handle_char(app, (wchar_t)wparam, false)) {
            return 0;
        }
        break;

    case WM_SYSCHAR:
        if (winxterm_app_handle_char(app, (wchar_t)wparam, true)) {
            return 0;
        }
        break;

    case WM_KEYDOWN:
        if (winxterm_app_handle_local_key(app, wparam)) {
            return 0;
        }
        if (winxterm_app_handle_virtual_key(app, wparam)) {
            return 0;
        }
        break;

    case WM_SYSKEYDOWN:
        if (wparam == VK_RETURN && ((HIWORD(lparam) & KF_ALTDOWN) != 0)) {
            winxterm_app_toggle_fullscreen(app);
            return 0;
        }
        if (wparam == VK_F4 && ((HIWORD(lparam) & KF_ALTDOWN) != 0)) {
            winxterm_log_writef(app->log, "alt+f4: exit requested");
            (void)winxterm_app_request_close(app, "alt+f4");
            return 0;
        }
        if (winxterm_app_handle_local_key(app, wparam)) {
            return 0;
        }
        if (winxterm_app_handle_virtual_key(app, wparam)) {
            return 0;
        }
        break;

    case WM_MOUSEWHEEL:
        if (winxterm_app_handle_mouse_wheel(app, wparam, lparam)) {
            return 0;
        }
        break;

    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MBUTTONUP:
    case WM_RBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_MBUTTONDBLCLK:
    case WM_RBUTTONDBLCLK:
        if (winxterm_app_handle_mouse_button(app, message, wparam, lparam)) {
            return 0;
        }
        break;

    case WM_MOUSEMOVE:
        if (winxterm_app_handle_mouse_move(app, wparam, lparam)) {
            return 0;
        }
        break;

    case WM_MOUSELEAVE:
        app->hover_tracking = false;
        winxterm_app_clear_hover(app);
        return 0;

    case WM_CONTEXTMENU:
        if (app->alt_drag_suppress_context_menu) {
            app->alt_drag_suppress_context_menu = false;
            return 0;
        }
        winxterm_app_show_context_menu(app, lparam);
        return 0;

    case WM_COMMAND:
        winxterm_app_handle_command(app, wparam);
        return 0;

    case WM_PASTE:
        (void)winxterm_app_paste_clipboard(app);
        return 0;

    case WM_CLOSE:
        winxterm_log_writef(app->log, "window close: exit requested");
        (void)winxterm_app_request_close(app, "window close");
        return 0;

    case WM_PAINT:
        winxterm_app_paint(app);
        return 0;

    case WINXTERM_WM_RENDER_UPDATE:
        winxterm_app_handle_render_update(app, hwnd);
        return 0;

    case WINXTERM_WM_JOB_UI:
        winxterm_app_show_context_menu(app, (LPARAM)-1);
        return 0;

    case WINXTERM_WM_MACRO_UPDATE:
        winxterm_app_handle_macro_update(app);
        return 0;

    case WM_DESTROY:
        winxterm_log_writef(app->log, "window destroyed");
        winxterm_window_placement_save_if_last_instance(hwnd, app->fullscreen, app->log);
        winxterm_app_restore_previous_focus(app);
        winxterm_bridge_set_hwnd(app->bridge, 0);
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }

    return DefWindowProcW(hwnd, message, wparam, lparam);
}
