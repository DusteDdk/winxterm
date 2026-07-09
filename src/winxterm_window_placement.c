#include "winxterm_window_placement.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WINXTERM_WINDOW_PLACEMENT_CLASS_NAME L"WinxtermMainWindow"
#define WINXTERM_WINDOW_PLACEMENT_PATH_CAPACITY WINXTERM_LOG_PATH_CAPACITY
#define WINXTERM_WINDOW_PLACEMENT_MAX_FILE_BYTES 8192u
#define WINXTERM_WINDOW_PLACEMENT_MAX_MONITORS 32u
#define WINXTERM_WINDOW_PLACEMENT_FULLSCREEN_PENALTY 20000
#define WINXTERM_WINDOW_PLACEMENT_MAXIMIZED_PENALTY 10000

typedef struct WinxtermWindowPlacementMonitor {
    HMONITOR handle;
    MONITORINFOEXW info;
    WinxtermWindowPlacementMonitorScore score;
    RECT first_windowed_rect;
    bool has_first_windowed_rect;
    unsigned int first_windowed_z_order;
} WinxtermWindowPlacementMonitor;

typedef struct WinxtermWindowPlacementContext {
    HWND self;
    WinxtermWindowPlacementMonitor monitors[WINXTERM_WINDOW_PLACEMENT_MAX_MONITORS];
    size_t monitor_count;
    HWND foreground;
    unsigned int z_order;
} WinxtermWindowPlacementContext;

typedef struct WinxtermWindowPlacementOtherWindowContext {
    HWND self;
    bool found_other;
} WinxtermWindowPlacementOtherWindowContext;

static bool winxterm_window_placement_append_child(const wchar_t *directory,
                                                   const wchar_t *name,
                                                   wchar_t *out,
                                                   size_t out_count)
{
    if (directory == 0 || directory[0] == L'\0' || name == 0 || name[0] == L'\0' ||
        out == 0 || out_count == 0u) {
        return false;
    }

    const wchar_t *separator = L"\\";
    size_t length = wcslen(directory);
    if (length != 0u && (directory[length - 1u] == L'\\' || directory[length - 1u] == L'/')) {
        separator = L"";
    }

    return _snwprintf_s(out, out_count, _TRUNCATE, L"%ls%ls%ls", directory, separator, name) >= 0;
}

static bool winxterm_window_placement_get_home_directory(wchar_t *buffer, size_t buffer_count)
{
    if (buffer == 0 || buffer_count == 0u) {
        return false;
    }

    DWORD length = GetEnvironmentVariableW(L"USERPROFILE", buffer, (DWORD)buffer_count);
    if (length != 0u && length < buffer_count) {
        return true;
    }

    wchar_t drive[16];
    wchar_t path[WINXTERM_WINDOW_PLACEMENT_PATH_CAPACITY];
    DWORD drive_length = GetEnvironmentVariableW(L"HOMEDRIVE", drive, 16u);
    DWORD path_length =
        GetEnvironmentVariableW(L"HOMEPATH", path, WINXTERM_WINDOW_PLACEMENT_PATH_CAPACITY);
    if (drive_length == 0u || drive_length >= 16u || path_length == 0u ||
        path_length >= WINXTERM_WINDOW_PLACEMENT_PATH_CAPACITY) {
        buffer[0] = L'\0';
        return false;
    }

    return _snwprintf_s(buffer, buffer_count, _TRUNCATE, L"%ls%ls", drive, path) >= 0;
}

static bool winxterm_window_placement_format_state_directory(wchar_t *buffer, size_t buffer_count)
{
    wchar_t home[WINXTERM_WINDOW_PLACEMENT_PATH_CAPACITY];
    if (!winxterm_window_placement_get_home_directory(home, WINXTERM_WINDOW_PLACEMENT_PATH_CAPACITY) ||
        !winxterm_window_placement_append_child(home, L".winxterm", buffer, buffer_count)) {
        if (buffer != 0 && buffer_count != 0u) {
            buffer[0] = L'\0';
        }
        return false;
    }
    return true;
}

static bool winxterm_window_placement_prepare_state_directory(wchar_t *buffer, size_t buffer_count)
{
    if (!winxterm_window_placement_format_state_directory(buffer, buffer_count)) {
        return false;
    }
    if (!CreateDirectoryW(buffer, 0) && GetLastError() != ERROR_ALREADY_EXISTS) {
        buffer[0] = L'\0';
        return false;
    }
    return true;
}

static bool winxterm_window_placement_format_state_path(wchar_t *buffer, size_t buffer_count)
{
    wchar_t directory[WINXTERM_WINDOW_PLACEMENT_PATH_CAPACITY];
    return winxterm_window_placement_format_state_directory(directory,
                                                           WINXTERM_WINDOW_PLACEMENT_PATH_CAPACITY) &&
           winxterm_window_placement_append_child(directory,
                                                  WINXTERM_WINDOW_PLACEMENT_STATE_FILENAME,
                                                  buffer,
                                                  buffer_count);
}

static bool winxterm_window_placement_prepare_state_path(wchar_t *buffer,
                                                        size_t buffer_count,
                                                        wchar_t *directory,
                                                        size_t directory_count)
{
    if (!winxterm_window_placement_prepare_state_directory(directory, directory_count)) {
        return false;
    }
    return winxterm_window_placement_append_child(directory,
                                                 WINXTERM_WINDOW_PLACEMENT_STATE_FILENAME,
                                                 buffer,
                                                 buffer_count);
}

static bool winxterm_window_placement_read_state_file(WinxtermWindowPlacementSaved *placement)
{
    if (placement == 0) {
        return false;
    }

    wchar_t path[WINXTERM_WINDOW_PLACEMENT_PATH_CAPACITY];
    if (!winxterm_window_placement_format_state_path(path, WINXTERM_WINDOW_PLACEMENT_PATH_CAPACITY)) {
        return false;
    }

    HANDLE file = CreateFileW(path,
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              0,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              0);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > (LONGLONG)WINXTERM_WINDOW_PLACEMENT_MAX_FILE_BYTES) {
        CloseHandle(file);
        return false;
    }

    char *buffer = (char *)calloc((size_t)size.QuadPart + 1u, sizeof(*buffer));
    if (buffer == 0) {
        CloseHandle(file);
        return false;
    }

    DWORD bytes_read = 0;
    bool ok = ReadFile(file, buffer, (DWORD)size.QuadPart, &bytes_read, 0) &&
              bytes_read == (DWORD)size.QuadPart &&
              winxterm_window_placement_parse_state(buffer, placement);
    free(buffer);
    CloseHandle(file);
    return ok;
}

static bool winxterm_window_placement_write_state_file(
    const WinxtermWindowPlacementSaved *placement)
{
    if (placement == 0) {
        return false;
    }

    char text[1024];
    if (!winxterm_window_placement_format_state(placement, text, sizeof(text))) {
        return false;
    }

    wchar_t directory[WINXTERM_WINDOW_PLACEMENT_PATH_CAPACITY];
    wchar_t path[WINXTERM_WINDOW_PLACEMENT_PATH_CAPACITY];
    if (!winxterm_window_placement_prepare_state_path(path,
                                                      WINXTERM_WINDOW_PLACEMENT_PATH_CAPACITY,
                                                      directory,
                                                      WINXTERM_WINDOW_PLACEMENT_PATH_CAPACITY)) {
        return false;
    }

    wchar_t temp_path[WINXTERM_WINDOW_PLACEMENT_PATH_CAPACITY];
    if (_snwprintf_s(temp_path,
                     WINXTERM_WINDOW_PLACEMENT_PATH_CAPACITY,
                     _TRUNCATE,
                     L"%ls\\window-placement-%lu-%lu.tmp",
                     directory,
                     (unsigned long)GetCurrentProcessId(),
                     (unsigned long)GetTickCount()) < 0) {
        return false;
    }

    HANDLE file = CreateFileW(temp_path,
                              GENERIC_WRITE,
                              0,
                              0,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              0);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD byte_count = (DWORD)strlen(text);
    DWORD written = 0;
    bool ok = WriteFile(file, text, byte_count, &written, 0) && written == byte_count;
    ok = CloseHandle(file) && ok;
    if (ok) {
        ok = MoveFileExW(temp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    if (!ok) {
        DeleteFileW(temp_path);
    }
    return ok;
}

static bool winxterm_window_placement_get_line_value(const char *text,
                                                     const char *key,
                                                     char *out,
                                                     size_t out_count)
{
    if (text == 0 || key == 0 || out == 0 || out_count == 0u) {
        return false;
    }

    size_t key_length = strlen(key);
    const char *line = text;
    while (*line != '\0') {
        const char *line_end = line;
        while (*line_end != '\0' && *line_end != '\r' && *line_end != '\n') {
            ++line_end;
        }
        if ((size_t)(line_end - line) > key_length &&
            memcmp(line, key, key_length) == 0 &&
            line[key_length] == '=') {
            size_t value_length = (size_t)(line_end - (line + key_length + 1u));
            if (value_length >= out_count) {
                return false;
            }
            memcpy(out, line + key_length + 1u, value_length);
            out[value_length] = '\0';
            return true;
        }
        while (*line_end == '\r' || *line_end == '\n') {
            ++line_end;
        }
        line = line_end;
    }
    return false;
}

static bool winxterm_window_placement_parse_int_value(const char *text,
                                                      const char *key,
                                                      int *out)
{
    char buffer[64];
    if (!winxterm_window_placement_get_line_value(text, key, buffer, sizeof(buffer))) {
        return false;
    }
    char *end = 0;
    long value = strtol(buffer, &end, 10);
    if (end == buffer || *end != '\0' || value < INT_MIN || value > INT_MAX) {
        return false;
    }
    *out = (int)value;
    return true;
}

bool winxterm_window_placement_parse_state(const char *text,
                                           WinxtermWindowPlacementSaved *placement)
{
    if (text == 0 || placement == 0 ||
        (strncmp(text,
                 "winxterm-window-placement-v2",
                 strlen("winxterm-window-placement-v2")) != 0 &&
         strncmp(text,
                 "winxterm-window-placement-v1",
                 strlen("winxterm-window-placement-v1")) != 0)) {
        return false;
    }

    char monitor_name[128];
    int left = 0;
    int top = 0;
    if (!winxterm_window_placement_get_line_value(text,
                                                  "monitor",
                                                  monitor_name,
                                                  sizeof(monitor_name)) ||
        !winxterm_window_placement_parse_int_value(text, "left", &left) ||
        !winxterm_window_placement_parse_int_value(text, "top", &top)) {
        return false;
    }

    wchar_t monitor_wide[CCHDEVICENAME];
    int converted = MultiByteToWideChar(CP_UTF8,
                                        MB_ERR_INVALID_CHARS,
                                        monitor_name,
                                        -1,
                                        monitor_wide,
                                        CCHDEVICENAME);
    if (converted <= 0) {
        return false;
    }

    memset(placement, 0, sizeof(*placement));
    wcscpy_s(placement->monitor_name, CCHDEVICENAME, monitor_wide);
    placement->left = left;
    placement->top = top;
    return true;
}

bool winxterm_window_placement_format_state(const WinxtermWindowPlacementSaved *placement,
                                            char *buffer,
                                            size_t buffer_count)
{
    if (placement == 0 || buffer == 0 || buffer_count == 0u ||
        placement->monitor_name[0] == L'\0') {
        return false;
    }

    char monitor_name[128];
    int converted = WideCharToMultiByte(CP_UTF8,
                                        0,
                                        placement->monitor_name,
                                        -1,
                                        monitor_name,
                                        sizeof(monitor_name),
                                        0,
                                        0);
    if (converted <= 0) {
        return false;
    }

    int written = _snprintf_s(buffer,
                              buffer_count,
                              _TRUNCATE,
                              "winxterm-window-placement-v2\n"
                              "monitor=%s\n"
                              "left=%d\n"
                              "top=%d\n",
                              monitor_name,
                              placement->left,
                              placement->top);
    return written > 0 && (size_t)written < buffer_count;
}

int winxterm_window_placement_monitor_penalty(const WinxtermWindowPlacementMonitorScore *score)
{
    if (score == 0) {
        return 0;
    }
    if (score->has_fullscreen_instance) {
        return WINXTERM_WINDOW_PLACEMENT_FULLSCREEN_PENALTY;
    }
    if (score->has_foreground_maximized_instance) {
        return WINXTERM_WINDOW_PLACEMENT_MAXIMIZED_PENALTY;
    }
    return 0;
}

bool winxterm_window_placement_rect_fits_monitor(const RECT *rect, const RECT *monitor_rect)
{
    return rect != 0 && monitor_rect != 0 &&
           rect->left >= monitor_rect->left &&
           rect->top >= monitor_rect->top &&
           rect->right <= monitor_rect->right &&
           rect->bottom <= monitor_rect->bottom &&
           rect->right > rect->left &&
           rect->bottom > rect->top;
}

static int winxterm_window_placement_clamp_int(int value, int minimum, int maximum)
{
    if (maximum < minimum) {
        return minimum;
    }
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

RECT winxterm_window_placement_clamp_rect_to_visible_area(const RECT *visible_rect,
                                                          const RECT *rect)
{
    RECT clamped = {0, 0, 0, 0};
    if (visible_rect == 0 || rect == 0 ||
        visible_rect->right <= visible_rect->left ||
        visible_rect->bottom <= visible_rect->top ||
        rect->right <= rect->left ||
        rect->bottom <= rect->top) {
        return clamped;
    }

    int visible_width = visible_rect->right - visible_rect->left;
    int visible_height = visible_rect->bottom - visible_rect->top;
    int width = rect->right - rect->left;
    int height = rect->bottom - rect->top;
    if (width > visible_width) {
        width = visible_width;
    }
    if (height > visible_height) {
        height = visible_height;
    }

    clamped.left = winxterm_window_placement_clamp_int(rect->left,
                                                       visible_rect->left,
                                                       visible_rect->right - width);
    clamped.top = winxterm_window_placement_clamp_int(rect->top,
                                                      visible_rect->top,
                                                      visible_rect->bottom - height);
    clamped.right = clamped.left + width;
    clamped.bottom = clamped.top + height;
    return clamped;
}

RECT winxterm_window_placement_rect_from_local(const RECT *monitor_rect,
                                               int left,
                                               int top,
                                               int width,
                                               int height)
{
    RECT rect = {0, 0, 0, 0};
    if (monitor_rect == 0 || width <= 0 || height <= 0) {
        return rect;
    }
    rect.left = monitor_rect->left + left;
    rect.top = monitor_rect->top + top;
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
    return rect;
}

RECT winxterm_window_placement_center_rect(const RECT *monitor_rect, int width, int height)
{
    RECT rect = {0, 0, 0, 0};
    if (monitor_rect == 0 || width <= 0 || height <= 0) {
        return rect;
    }
    int monitor_width = monitor_rect->right - monitor_rect->left;
    int monitor_height = monitor_rect->bottom - monitor_rect->top;
    rect.left = monitor_rect->left + (monitor_width - width) / 2;
    rect.top = monitor_rect->top + (monitor_height - height) / 2;
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
    return rect;
}

RECT winxterm_window_placement_monitor_top_left_rect(const RECT *monitor_rect,
                                                     int width,
                                                     int height)
{
    return winxterm_window_placement_rect_from_local(monitor_rect, 0, 0, width, height);
}

RECT winxterm_window_placement_cascade_rect(const RECT *monitor_rect,
                                            const RECT *existing_rect,
                                            int width,
                                            int height)
{
    RECT rect = {0, 0, 0, 0};
    if (monitor_rect == 0 || existing_rect == 0 || width <= 0 || height <= 0) {
        return rect;
    }
    int local_left = existing_rect->left - monitor_rect->left;
    int local_top = existing_rect->top - monitor_rect->top;
    rect.left = monitor_rect->left + local_left + WINXTERM_WINDOW_PLACEMENT_CASCADE_DX;
    rect.top = monitor_rect->top + local_top + WINXTERM_WINDOW_PLACEMENT_CASCADE_DY;
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
    return winxterm_window_placement_clamp_rect_to_visible_area(monitor_rect, &rect);
}

static BOOL CALLBACK winxterm_window_placement_enum_monitor(HMONITOR monitor,
                                                            HDC dc,
                                                            RECT *rect,
                                                            LPARAM lparam)
{
    (void)dc;
    (void)rect;
    WinxtermWindowPlacementContext *context = (WinxtermWindowPlacementContext *)lparam;
    if (context == 0 ||
        context->monitor_count >= WINXTERM_WINDOW_PLACEMENT_MAX_MONITORS) {
        return TRUE;
    }

    WinxtermWindowPlacementMonitor *entry = context->monitors + context->monitor_count;
    memset(entry, 0, sizeof(*entry));
    entry->handle = monitor;
    entry->info.cbSize = sizeof(entry->info);
    if (!GetMonitorInfoW(monitor, (MONITORINFO *)&entry->info)) {
        return TRUE;
    }
    entry->first_windowed_z_order = UINT_MAX;
    ++context->monitor_count;
    return TRUE;
}

static int winxterm_window_placement_find_monitor(WinxtermWindowPlacementContext *context,
                                                  HMONITOR monitor)
{
    if (context == 0 || monitor == 0) {
        return -1;
    }
    for (size_t i = 0u; i < context->monitor_count; ++i) {
        if (context->monitors[i].handle == monitor) {
            return (int)i;
        }
    }
    return -1;
}

static int winxterm_window_placement_find_monitor_by_name(
    WinxtermWindowPlacementContext *context,
    const wchar_t *name)
{
    if (context == 0 || name == 0 || name[0] == L'\0') {
        return -1;
    }
    for (size_t i = 0u; i < context->monitor_count; ++i) {
        if (_wcsicmp(context->monitors[i].info.szDevice, name) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool winxterm_window_placement_window_is_winxterm(HWND hwnd)
{
    wchar_t class_name[64];
    int length = GetClassNameW(hwnd, class_name, (int)(sizeof(class_name) / sizeof(class_name[0])));
    return length > 0 && wcscmp(class_name, WINXTERM_WINDOW_PLACEMENT_CLASS_NAME) == 0;
}

static bool winxterm_window_placement_rect_covers_monitor(const RECT *window_rect,
                                                          const RECT *monitor_rect)
{
    return window_rect != 0 && monitor_rect != 0 &&
           window_rect->left <= monitor_rect->left &&
           window_rect->top <= monitor_rect->top &&
           window_rect->right >= monitor_rect->right &&
           window_rect->bottom >= monitor_rect->bottom;
}

static BOOL CALLBACK winxterm_window_placement_enum_window(HWND hwnd, LPARAM lparam)
{
    WinxtermWindowPlacementContext *context = (WinxtermWindowPlacementContext *)lparam;
    if (context == 0) {
        return TRUE;
    }
    ++context->z_order;
    if (hwnd == context->self ||
        !IsWindowVisible(hwnd) ||
        IsIconic(hwnd) ||
        !winxterm_window_placement_window_is_winxterm(hwnd)) {
        return TRUE;
    }

    RECT window_rect;
    if (!GetWindowRect(hwnd, &window_rect)) {
        return TRUE;
    }
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    int monitor_index = winxterm_window_placement_find_monitor(context, monitor);
    if (monitor_index < 0) {
        return TRUE;
    }

    WinxtermWindowPlacementMonitor *entry = context->monitors + monitor_index;
    bool zoomed = IsZoomed(hwnd) != 0;
    bool fullscreen =
        !zoomed &&
        winxterm_window_placement_rect_covers_monitor(&window_rect, &entry->info.rcMonitor);
    if (fullscreen) {
        entry->score.has_fullscreen_instance = true;
        return TRUE;
    }
    if (zoomed && hwnd == context->foreground) {
        entry->score.has_foreground_maximized_instance = true;
        return TRUE;
    }
    if (!zoomed) {
        entry->score.has_windowed_instance = true;
        if (!entry->has_first_windowed_rect ||
            context->z_order < entry->first_windowed_z_order) {
            entry->first_windowed_rect = window_rect;
            entry->first_windowed_z_order = context->z_order;
            entry->has_first_windowed_rect = true;
        }
    }
    return TRUE;
}

static bool winxterm_window_placement_collect_context(HWND self,
                                                     WinxtermWindowPlacementContext *context)
{
    if (context == 0) {
        return false;
    }
    memset(context, 0, sizeof(*context));
    context->self = self;
    context->foreground = GetForegroundWindow();
    if (!EnumDisplayMonitors(0, 0, winxterm_window_placement_enum_monitor, (LPARAM)context) ||
        context->monitor_count == 0u) {
        return false;
    }
    (void)EnumWindows(winxterm_window_placement_enum_window, (LPARAM)context);
    return true;
}

static int winxterm_window_placement_best_penalty(WinxtermWindowPlacementContext *context)
{
    int best = INT_MAX;
    if (context == 0 || context->monitor_count == 0u) {
        return 0;
    }
    for (size_t i = 0u; i < context->monitor_count; ++i) {
        int penalty = winxterm_window_placement_monitor_penalty(&context->monitors[i].score);
        if (penalty < best) {
            best = penalty;
        }
    }
    return best == INT_MAX ? 0 : best;
}

static int winxterm_window_placement_first_best_monitor(WinxtermWindowPlacementContext *context,
                                                       int best_penalty)
{
    if (context == 0) {
        return -1;
    }
    for (size_t i = 0u; i < context->monitor_count; ++i) {
        int penalty = winxterm_window_placement_monitor_penalty(&context->monitors[i].score);
        if (penalty == best_penalty) {
            return (int)i;
        }
    }
    return -1;
}

static int winxterm_window_placement_best_windowed_monitor(WinxtermWindowPlacementContext *context,
                                                          int best_penalty)
{
    int best_index = -1;
    unsigned int best_z_order = UINT_MAX;
    if (context == 0) {
        return -1;
    }
    for (size_t i = 0u; i < context->monitor_count; ++i) {
        WinxtermWindowPlacementMonitor *entry = context->monitors + i;
        int penalty = winxterm_window_placement_monitor_penalty(&entry->score);
        if (penalty == best_penalty &&
            entry->has_first_windowed_rect &&
            entry->first_windowed_z_order < best_z_order) {
            best_index = (int)i;
            best_z_order = entry->first_windowed_z_order;
        }
    }
    return best_index;
}

static int winxterm_window_placement_cursor_monitor(WinxtermWindowPlacementContext *context)
{
    POINT cursor;
    if (context == 0 || !GetCursorPos(&cursor)) {
        return -1;
    }
    HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    return winxterm_window_placement_find_monitor(context, monitor);
}

static RECT winxterm_window_placement_clamped_cursor_rect(const RECT *monitor_rect,
                                                         int width,
                                                         int height)
{
    POINT cursor;
    if (!GetCursorPos(&cursor)) {
        return winxterm_window_placement_center_rect(monitor_rect, width, height);
    }
    RECT rect = {cursor.x, cursor.y, cursor.x + width, cursor.y + height};
    return winxterm_window_placement_clamp_rect_to_visible_area(monitor_rect, &rect);
}

static bool winxterm_window_placement_choose_rect(WinxtermWindowPlacementContext *context,
                                                  int width,
                                                  int height,
                                                  RECT *out_rect,
                                                  const char **out_reason)
{
    if (context == 0 || out_rect == 0 || width <= 0 || height <= 0) {
        return false;
    }

    int best_penalty = winxterm_window_placement_best_penalty(context);
    int windowed_monitor =
        winxterm_window_placement_best_windowed_monitor(context, best_penalty);
    if (windowed_monitor >= 0) {
        WinxtermWindowPlacementMonitor *entry = context->monitors + windowed_monitor;
        *out_rect = winxterm_window_placement_cascade_rect(&entry->info.rcWork,
                                                           &entry->first_windowed_rect,
                                                           width,
                                                           height);
        if (out_reason != 0) {
            *out_reason = "windowed-instance";
        }
        return true;
    }

    WinxtermWindowPlacementSaved saved;
    if (winxterm_window_placement_read_state_file(&saved)) {
        int saved_monitor =
            winxterm_window_placement_find_monitor_by_name(context, saved.monitor_name);
        if (saved_monitor < 0) {
            saved_monitor = winxterm_window_placement_cursor_monitor(context);
        }
        if (saved_monitor >= 0) {
            WinxtermWindowPlacementMonitor *entry = context->monitors + saved_monitor;
            int saved_penalty = winxterm_window_placement_monitor_penalty(&entry->score);
            RECT saved_rect = winxterm_window_placement_rect_from_local(&entry->info.rcMonitor,
                                                                        saved.left,
                                                                        saved.top,
                                                                        width,
                                                                        height);
            saved_rect =
                winxterm_window_placement_clamp_rect_to_visible_area(&entry->info.rcWork,
                                                                     &saved_rect);
            if (saved_penalty == best_penalty &&
                winxterm_window_placement_rect_fits_monitor(&saved_rect, &entry->info.rcWork)) {
                *out_rect = saved_rect;
                if (out_reason != 0) {
                    *out_reason = "saved-placement";
                }
                return true;
            }
        }
    }

    int cursor_monitor = winxterm_window_placement_cursor_monitor(context);
    if (cursor_monitor >= 0) {
        WinxtermWindowPlacementMonitor *entry = context->monitors + cursor_monitor;
        int cursor_penalty = winxterm_window_placement_monitor_penalty(&entry->score);
        if (cursor_penalty == best_penalty) {
            *out_rect = winxterm_window_placement_clamped_cursor_rect(&entry->info.rcWork,
                                                                      width,
                                                                      height);
            if (out_reason != 0) {
                *out_reason = "cursor";
            }
            return true;
        }
    }

    int best_monitor = winxterm_window_placement_first_best_monitor(context, best_penalty);
    if (best_monitor >= 0) {
        WinxtermWindowPlacementMonitor *entry = context->monitors + best_monitor;
        RECT centered = winxterm_window_placement_center_rect(&entry->info.rcWork, width, height);
        *out_rect =
            winxterm_window_placement_clamp_rect_to_visible_area(&entry->info.rcWork, &centered);
        if (out_reason != 0) {
            *out_reason = "center";
        }
        return true;
    }
    return false;
}

static bool winxterm_window_placement_visible_area_for_rect(const RECT *rect, RECT *visible_rect)
{
    if (rect == 0 || visible_rect == 0) {
        return false;
    }

    HMONITOR monitor = MonitorFromRect(rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info;
    memset(&info, 0, sizeof(info));
    info.cbSize = sizeof(info);
    if (monitor == 0 || !GetMonitorInfoW(monitor, &info)) {
        return false;
    }

    *visible_rect = info.rcWork;
    return true;
}

static void winxterm_window_placement_raise_startup_window(HWND hwnd)
{
    SetWindowPos(hwnd,
                 HWND_TOPMOST,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    SetWindowPos(hwnd,
                 HWND_NOTOPMOST,
                 0,
                 0,
                 0,
                 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    BringWindowToTop(hwnd);
    (void)SetForegroundWindow(hwnd);
}

void winxterm_window_placement_apply_startup(HWND hwnd,
                                             int width,
                                             int height,
                                             WinxtermLog *log)
{
    if (hwnd == 0 || width <= 0 || height <= 0) {
        return;
    }

    WinxtermWindowPlacementContext context;
    RECT target;
    const char *reason = "none";
    if (!winxterm_window_placement_collect_context(hwnd, &context) ||
        !winxterm_window_placement_choose_rect(&context, width, height, &target, &reason)) {
        RECT current;
        RECT visible;
        if (GetWindowRect(hwnd, &current) &&
            winxterm_window_placement_visible_area_for_rect(&current, &visible)) {
            target = winxterm_window_placement_clamp_rect_to_visible_area(&visible, &current);
            SetWindowPos(hwnd,
                         HWND_TOPMOST,
                         target.left,
                         target.top,
                         target.right - target.left,
                         target.bottom - target.top,
                         SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
        }
        winxterm_window_placement_raise_startup_window(hwnd);
        return;
    }

    RECT visible;
    if (winxterm_window_placement_visible_area_for_rect(&target, &visible)) {
        target = winxterm_window_placement_clamp_rect_to_visible_area(&visible, &target);
    }

    SetWindowPos(hwnd,
                 HWND_TOPMOST,
                 target.left,
                 target.top,
                 target.right - target.left,
                 target.bottom - target.top,
                 SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
    winxterm_window_placement_raise_startup_window(hwnd);
    winxterm_log_writef(log,
                        "window placement: %s x=%d y=%d width=%d height=%d",
                        reason,
                        target.left,
                        target.top,
                        target.right - target.left,
                        target.bottom - target.top);
}

static BOOL CALLBACK winxterm_window_placement_enum_other_window(HWND hwnd, LPARAM lparam)
{
    WinxtermWindowPlacementOtherWindowContext *context =
        (WinxtermWindowPlacementOtherWindowContext *)lparam;
    if (context == 0 || context->found_other) {
        return FALSE;
    }
    if (hwnd != context->self && winxterm_window_placement_window_is_winxterm(hwnd)) {
        context->found_other = true;
        return FALSE;
    }
    return TRUE;
}

void winxterm_window_placement_save_if_last_instance(HWND hwnd,
                                                     bool app_fullscreen,
                                                     WinxtermLog *log)
{
    if (hwnd == 0) {
        return;
    }

    WinxtermWindowPlacementOtherWindowContext other_context;
    memset(&other_context, 0, sizeof(other_context));
    other_context.self = hwnd;
    (void)EnumWindows(winxterm_window_placement_enum_other_window, (LPARAM)&other_context);
    if (other_context.found_other) {
        return;
    }

    RECT window_rect;
    if (!GetWindowRect(hwnd, &window_rect)) {
        return;
    }
    HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW monitor_info;
    memset(&monitor_info, 0, sizeof(monitor_info));
    monitor_info.cbSize = sizeof(monitor_info);
    if (!GetMonitorInfoW(monitor, (MONITORINFO *)&monitor_info)) {
        return;
    }

    WinxtermWindowPlacementSaved placement;
    memset(&placement, 0, sizeof(placement));
    wcscpy_s(placement.monitor_name, CCHDEVICENAME, monitor_info.szDevice);
    placement.left = window_rect.left - monitor_info.rcMonitor.left;
    placement.top = window_rect.top - monitor_info.rcMonitor.top;

    bool ok = winxterm_window_placement_write_state_file(&placement);
    winxterm_log_writef(log,
                        "window placement: save %s%s monitor=%ls left=%d top=%d",
                        ok ? "ok" : "failed",
                        app_fullscreen ? " fullscreen" : "",
                        placement.monitor_name,
                        placement.left,
                        placement.top);
}
