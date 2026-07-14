#ifndef WINXTERM_APP_H
#define WINXTERM_APP_H

#include "winxterm_bridge.h"
#include "winxterm_log.h"
#include "winxterm_render.h"
#include "winxterm_surface.h"
#include "winxterm_ux.h"

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

typedef enum WinxtermAltDragMode {
    WINXTERM_ALT_DRAG_NONE = 0,
    WINXTERM_ALT_DRAG_MOVE,
    WINXTERM_ALT_DRAG_RESIZE
} WinxtermAltDragMode;

typedef enum WinxtermClickPreviewKind {
    WINXTERM_CLICK_PREVIEW_NONE = 0,
    WINXTERM_CLICK_PREVIEW_ROW,
    WINXTERM_CLICK_PREVIEW_RANGE,
    WINXTERM_CLICK_PREVIEW_BOX
} WinxtermClickPreviewKind;

typedef struct WinxtermMacro WinxtermMacro;
typedef struct WinxtermAppSessionUx WinxtermAppSessionUx;

typedef struct WinxtermApp {
    HINSTANCE instance;
    HWND hwnd;
    HWND previous_focus_hwnd;
    WinxtermLog *log;

    WinxtermSurface surface;
    uint32_t rendered_background_rgb;
    WinxtermRenderContext render_context;
    WinxtermBridge *bridge;
    HANDLE shutdown_event;
    WinxtermUxState ux;
    uint64_t ux_session_id;
    WinxtermAppSessionUx *session_ux;
    unsigned int display_scale;
    WinxtermMacro *macro;
    WinxtermRenderDamage render_damage;
    bool rendered_cursor_valid;
    int rendered_cursor_row;
    bool rendered_selection_valid;
    WinxtermScreenSelectionRange rendered_selection;
    unsigned int deferred_frame_causes;
    UINT_PTR frame_timer_id;
    UINT_PTR macro_timer_id;
    UINT_PTR click_event_timer_id;
    UINT_PTR copy_overlay_timer_id;
    DWORD last_frame_tick;
    uint64_t invalidation_start_ns;
    bool pending_resize;
    WPARAM pending_resize_kind;
    DWORD last_resize_failure_log_tick;

    WinxtermScreenPrimaryAnchor scale_resize_anchor;
    bool fullscreen;
    bool closing;
    bool cursor_visible;
    bool scale_resize_anchor_valid;
    bool scrollbar_enabled;
    UINT_PTR cursor_timer_id;
    UINT_PTR bell_timer_id;
    UINT cursor_blink_ms;
    DWORD windowed_style;
    DWORD windowed_ex_style;
    WINDOWPLACEMENT windowed_placement;
    WPARAM last_size_kind;

    WinxtermAltDragMode alt_drag_mode;
    POINT alt_drag_start_cursor;
    POINT alt_drag_latest_cursor;
    RECT alt_drag_start_window;
    unsigned int alt_drag_edges;
    ULONGLONG alt_drag_last_apply_ms;
    bool alt_drag_suppress_context_menu;

    bool hover_valid;
    bool hover_tracking;
    int hover_column;
    int hover_view_row;

    bool copy_overlay_active;
    DWORD copy_overlay_start_tick;
    WinxtermScreenSelectionRange copy_overlay_range;
    WinxtermClickPreviewKind click_preview_kind;
    WinxtermScreenSelectionRange click_preview_range;

    bool left_click_pending;
    bool left_click_rectangular;
    bool left_click_rectangular_preserve_rows;
    POINT left_click_start_point;
    WinxtermUxPosition left_click_anchor;

    unsigned int click_sequence_count;
    DWORD click_sequence_last_tick;
    bool click_event_requested_handler;
    WinxtermUxPosition click_sequence_position;
} WinxtermApp;

bool winxterm_app_init(WinxtermApp *app,
                       HINSTANCE instance,
                       WinxtermLog *log,
                       WinxtermBridge *bridge,
                       HANDLE shutdown_event,
                       unsigned int display_scale);
int winxterm_app_run(WinxtermApp *app);
void winxterm_app_dispose(WinxtermApp *app);

#endif
