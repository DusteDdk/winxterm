#ifndef WINXTERM_WINDOW_PLACEMENT_H
#define WINXTERM_WINDOW_PLACEMENT_H

#include "winxterm_log.h"

#include <stdbool.h>
#include <stddef.h>
#include <windows.h>

#define WINXTERM_WINDOW_PLACEMENT_STATE_FILENAME L"window-placement.txt"
#define WINXTERM_WINDOW_PLACEMENT_CASCADE_DX 150
#define WINXTERM_WINDOW_PLACEMENT_CASCADE_DY 75

typedef struct WinxtermWindowPlacementSaved {
    wchar_t monitor_name[CCHDEVICENAME];
    int left;
    int top;
} WinxtermWindowPlacementSaved;

typedef struct WinxtermWindowPlacementMonitorScore {
    bool has_fullscreen_instance;
    bool has_foreground_maximized_instance;
    bool has_windowed_instance;
} WinxtermWindowPlacementMonitorScore;

bool winxterm_window_placement_parse_state(const char *text,
                                           WinxtermWindowPlacementSaved *placement);
bool winxterm_window_placement_format_state(const WinxtermWindowPlacementSaved *placement,
                                            char *buffer,
                                            size_t buffer_count);
int winxterm_window_placement_monitor_penalty(const WinxtermWindowPlacementMonitorScore *score);
bool winxterm_window_placement_rect_fits_monitor(const RECT *rect, const RECT *monitor_rect);
RECT winxterm_window_placement_clamp_rect_to_visible_area(const RECT *visible_rect,
                                                          const RECT *rect);
RECT winxterm_window_placement_rect_from_local(const RECT *monitor_rect,
                                               int left,
                                               int top,
                                               int width,
                                               int height);
RECT winxterm_window_placement_center_rect(const RECT *monitor_rect, int width, int height);
RECT winxterm_window_placement_monitor_top_left_rect(const RECT *monitor_rect,
                                                     int width,
                                                     int height);
RECT winxterm_window_placement_cascade_rect(const RECT *monitor_rect,
                                            const RECT *existing_rect,
                                            int width,
                                            int height);

void winxterm_window_placement_apply_startup(HWND hwnd,
                                             int width,
                                             int height,
                                             WinxtermLog *log);
void winxterm_window_placement_save_if_last_instance(HWND hwnd,
                                                     bool app_fullscreen,
                                                     WinxtermLog *log);

#endif
