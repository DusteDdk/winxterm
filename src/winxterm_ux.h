#ifndef WINXTERM_UX_H
#define WINXTERM_UX_H

#include "winxterm_screen.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <windows.h>

#define WINXTERM_UX_BELL_TIMER_MS 250u
#define WINXTERM_UX_BELL_DURATION_MS 2000u

typedef enum WinxtermUxRowKind {
    WINXTERM_UX_ROW_PRIMARY_SCROLLBACK,
    WINXTERM_UX_ROW_PRIMARY_VISIBLE,
    WINXTERM_UX_ROW_ALTERNATE_VISIBLE
} WinxtermUxRowKind;

typedef struct WinxtermUxPosition {
    WinxtermUxRowKind kind;
    size_t row;
    int column;
} WinxtermUxPosition;

typedef enum WinxtermSelectionMode {
    WINXTERM_SELECTION_LINEAR = 0,
    WINXTERM_SELECTION_RECTANGULAR
} WinxtermSelectionMode;

typedef enum WinxtermSelectionCopyFormat {
    WINXTERM_SELECTION_COPY_DEFAULT = 0,
    WINXTERM_SELECTION_COPY_RECTANGULAR_PRESERVE_ROWS
} WinxtermSelectionCopyFormat;

typedef struct WinxtermViewport {
    size_t line_offset_from_bottom;
    size_t last_primary_row_count;
    WinxtermScreenPrimaryAnchor bottom_anchor;
    bool follow_output;
    bool bottom_anchor_valid;
} WinxtermViewport;

typedef struct WinxtermSelection {
    bool active;
    bool selecting;
    WinxtermSelectionMode mode;
    WinxtermUxPosition anchor;
    WinxtermUxPosition extent;
} WinxtermSelection;

typedef struct WinxtermBellState {
    bool active;
    DWORD start_tick;
} WinxtermBellState;

typedef struct WinxtermUxState {
    WinxtermViewport viewport;
    WinxtermSelection selection;
    WinxtermBellState bell;
} WinxtermUxState;

void winxterm_ux_init(WinxtermUxState *ux);
void winxterm_ux_note_screen_changed_for_rows(WinxtermUxState *ux,
                                              const WinxtermScreen *screen,
                                              int visible_rows);
void winxterm_ux_note_screen_changed(WinxtermUxState *ux, const WinxtermScreen *screen);
void winxterm_ux_capture_resize_anchor_for_rows(WinxtermUxState *ux,
                                                const WinxtermScreen *screen,
                                                int visible_rows);
void winxterm_ux_capture_resize_anchor(WinxtermUxState *ux, const WinxtermScreen *screen);
void winxterm_ux_restore_resize_anchor_for_rows(WinxtermUxState *ux,
                                                const WinxtermScreen *screen,
                                                int visible_rows);
void winxterm_ux_restore_resize_anchor(WinxtermUxState *ux, const WinxtermScreen *screen);
void winxterm_ux_clamp_viewport_for_rows(WinxtermUxState *ux,
                                         const WinxtermScreen *screen,
                                         int visible_rows);
void winxterm_ux_clamp_viewport(WinxtermUxState *ux, const WinxtermScreen *screen);
void winxterm_ux_scroll_lines_for_rows(WinxtermUxState *ux,
                                       const WinxtermScreen *screen,
                                       int visible_rows,
                                       int lines_up);
void winxterm_ux_scroll_lines(WinxtermUxState *ux, const WinxtermScreen *screen, int lines_up);
void winxterm_ux_scroll_page_for_rows(WinxtermUxState *ux,
                                      const WinxtermScreen *screen,
                                      int visible_rows,
                                      int pages_up);
void winxterm_ux_scroll_page(WinxtermUxState *ux, const WinxtermScreen *screen, int pages_up);
void winxterm_ux_scroll_to_bottom(WinxtermUxState *ux);
void winxterm_ux_scroll_to_top_for_rows(WinxtermUxState *ux,
                                        const WinxtermScreen *screen,
                                        int visible_rows);
void winxterm_ux_scroll_to_top(WinxtermUxState *ux, const WinxtermScreen *screen);
size_t winxterm_ux_primary_first_row_for_rows(const WinxtermUxState *ux,
                                              const WinxtermScreen *screen,
                                              int visible_rows);
size_t winxterm_ux_primary_first_row(const WinxtermUxState *ux, const WinxtermScreen *screen);

bool winxterm_ux_hit_test_cells(const WinxtermUxState *ux,
                                const WinxtermScreen *screen,
                                int pixel_x,
                                int pixel_y,
                                unsigned int display_scale,
                                int visible_columns,
                                int visible_rows,
                                WinxtermUxPosition *position);
bool winxterm_ux_hit_test(const WinxtermUxState *ux,
                          const WinxtermScreen *screen,
                          int pixel_x,
                          int pixel_y,
                          unsigned int display_scale,
                          WinxtermUxPosition *position);
void winxterm_ux_begin_selection(WinxtermUxState *ux, WinxtermUxPosition position);
void winxterm_ux_begin_selection_mode(WinxtermUxState *ux,
                                      WinxtermUxPosition position,
                                      WinxtermSelectionMode mode);
void winxterm_ux_update_selection(WinxtermUxState *ux, WinxtermUxPosition position);
void winxterm_ux_finish_selection(WinxtermUxState *ux);
void winxterm_ux_clear_selection(WinxtermUxState *ux);
void winxterm_ux_select_all(WinxtermUxState *ux, const WinxtermScreen *screen);
bool winxterm_ux_select_word_at(WinxtermUxState *ux,
                                const WinxtermScreen *screen,
                                WinxtermUxPosition position);
bool winxterm_ux_select_non_space_run_at(WinxtermUxState *ux,
                                         const WinxtermScreen *screen,
                                         WinxtermUxPosition position);
bool winxterm_ux_select_real_line_at(WinxtermUxState *ux,
                                     const WinxtermScreen *screen,
                                     WinxtermUxPosition position);
bool winxterm_ux_has_selection(const WinxtermUxState *ux);
WinxtermScreenSelectionRange winxterm_ux_render_selection(const WinxtermUxState *ux,
                                                          const WinxtermScreen *screen);
bool winxterm_ux_extract_selection_utf8(const WinxtermUxState *ux,
                                        const WinxtermScreen *screen,
                                        char **out_text,
                                        size_t *out_length);
bool winxterm_ux_extract_selection_utf8_format(const WinxtermUxState *ux,
                                               const WinxtermScreen *screen,
                                               WinxtermSelectionCopyFormat format,
                                               char **out_text,
                                               size_t *out_length);

void winxterm_ux_start_bell(WinxtermUxState *ux, DWORD now);
bool winxterm_ux_bell_active(const WinxtermUxState *ux, DWORD now);
const wchar_t *winxterm_ux_bell_title_prefix(const WinxtermUxState *ux, DWORD now);

#endif
