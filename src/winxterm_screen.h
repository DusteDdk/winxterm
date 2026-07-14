#ifndef WINXTERM_SCREEN_H
#define WINXTERM_SCREEN_H

#include "winxterm_render.h"
#include "winxterm_modes.h"
#include "winxterm_terminal_ops.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WINXTERM_SCREEN_SCROLLBACK_LINE_CAP 10000u
#define WINXTERM_SCREEN_CELL_PROTECTED 0x00000001u
#define WINXTERM_SCREEN_CELL_BOLD 0x00000002u
#define WINXTERM_SCREEN_CELL_FAINT 0x00000004u
#define WINXTERM_SCREEN_CELL_ITALIC 0x00000008u
#define WINXTERM_SCREEN_CELL_UNDERLINE 0x00000010u
#define WINXTERM_SCREEN_CELL_DOUBLE_UNDERLINE 0x00000020u
#define WINXTERM_SCREEN_CELL_BLINK 0x00000040u
#define WINXTERM_SCREEN_CELL_INVERSE 0x00000080u
#define WINXTERM_SCREEN_CELL_INVISIBLE 0x00000100u
#define WINXTERM_SCREEN_CELL_CROSSED_OUT 0x00000200u
#define WINXTERM_SCREEN_CELL_OVERLINE 0x00000400u
#define WINXTERM_SCREEN_CELL_VISUAL_MASK 0x000007feu

#define WINXTERM_SCREEN_COLOR_FOREGROUND_DEFAULT 0x01u
#define WINXTERM_SCREEN_COLOR_BACKGROUND_DEFAULT 0x02u
#define WINXTERM_SCREEN_COLOR_FOREGROUND_INDEXED 0x04u
#define WINXTERM_SCREEN_COLOR_BACKGROUND_INDEXED 0x08u

#define WINXTERM_SCREEN_COMBINING_CAPACITY 4u
#define WINXTERM_SCREEN_ATTRIBUTE_STACK_CAPACITY 16u

typedef struct WinxtermScreenPalette {
    uint32_t slots[256];
    uint32_t default_foreground_rgb;
    uint32_t default_background_rgb;
} WinxtermScreenPalette;

typedef struct WinxtermScreenAttributeState {
    uint32_t foreground_rgb;
    uint32_t background_rgb;
    uint16_t foreground_palette_index;
    uint16_t background_palette_index;
    uint32_t attribute_flags;
    uint8_t color_flags;
} WinxtermScreenAttributeState;

typedef struct WinxtermScreenCell {
    uint32_t codepoint;
    uint32_t glyph_index;
    uint32_t foreground_rgb;
    uint32_t background_rgb;
    uint32_t attribute_flags;
    uint32_t combining_codepoints[WINXTERM_SCREEN_COMBINING_CAPACITY];
    uint16_t foreground_palette_index;
    uint16_t background_palette_index;
    uint8_t combining_count;
    uint8_t color_flags;
    uint8_t width;
    bool continuation;
    bool occupied;
} WinxtermScreenCell;

typedef struct WinxtermScreenLineMeta {
    bool soft_wrapped;
    int content_columns;
} WinxtermScreenLineMeta;

typedef struct WinxtermScreenLine {
    WinxtermScreenCell *cells;
    int columns;
    bool soft_wrapped;
    int content_columns;
} WinxtermScreenLine;

typedef struct WinxtermScreenRowView {
    const WinxtermScreenCell *cells;
    int columns;
    bool soft_wrapped;
    int content_columns;
} WinxtermScreenRowView;

typedef struct WinxtermScreenPrimaryAnchor {
    size_t logical_line;
    size_t cell_offset;
} WinxtermScreenPrimaryAnchor;

typedef struct WinxtermScreenSelectionRange {
    bool enabled;
    bool alternate;
    bool rectangular;
    size_t start_row;
    size_t end_row;
    int start_column;
    int end_column;
} WinxtermScreenSelectionRange;

typedef struct WinxtermScreenRenderView {
    size_t primary_first_row;
    WinxtermScreenSelectionRange selection;
} WinxtermScreenRenderView;

typedef struct WinxtermScreenRenderState {
    int columns;
    int rows;
    int bitmap_width;
    int bitmap_height;
    bool alternate_active;
    bool cursor_visible;
    bool screen_cursor_visible;
    int cursor_col;
    size_t cursor_global_row;
    size_t first_row;
    WinxtermScreenPalette palette;
    WinxtermModeState mode_state;
    WinxtermScreenSelectionRange selection;
    uint32_t clear_rgb;
} WinxtermScreenRenderState;

typedef struct WinxtermScreenCursorState {
    int row;
    int column;
    uint32_t foreground_rgb;
    uint32_t background_rgb;
    uint16_t foreground_palette_index;
    uint16_t background_palette_index;
    uint32_t attribute_flags;
    uint8_t color_flags;
    bool origin_mode;
    bool auto_wrap;
    bool protected_mode;
    WinxtermTerminalCursorStyle cursor_style;
} WinxtermScreenCursorState;

typedef struct WinxtermScreen {
    int columns;
    int rows;
    int cursor_row;
    int cursor_col;
    int scroll_top;
    int scroll_bottom;
    int left_margin;
    int right_margin;
    uint32_t foreground_rgb;
    uint32_t background_rgb;
    uint16_t foreground_palette_index;
    uint16_t background_palette_index;
    uint32_t attribute_flags;
    uint8_t color_flags;
    bool origin_mode;
    bool auto_wrap;
    bool alternate_active;
    bool pending_wrap;
    bool protected_mode;
    bool cursor_visible;
    WinxtermTerminalCursorStyle cursor_style;
    WinxtermTerminalCharset g0_charset;
    uint32_t last_printed_codepoint;
    WinxtermScreenPalette palette;
    WinxtermScreenAttributeState attribute_stack[WINXTERM_SCREEN_ATTRIBUTE_STACK_CAPACITY];
    size_t attribute_stack_count;
    WinxtermModeState mode_state;
    WinxtermModeState saved_mode_state;
    WinxtermScreenCursorState saved_primary_cursor;
    WinxtermScreenCursorState saved_alternate_cursor;
    WinxtermScreenCell *primary_cells;
    WinxtermScreenCell *alternate_cells;
    WinxtermScreenLineMeta *primary_line_meta;
    WinxtermScreenLineMeta *alternate_line_meta;
    WinxtermScreenLine *scrollback_lines;
    size_t scrollback_count;
    size_t scrollback_capacity;
    bool *tab_stops;
    WinxtermRenderDamage damage;
    uint64_t visual_line_advances;
} WinxtermScreen;

bool winxterm_screen_init(WinxtermScreen *screen, int columns, int rows);
void winxterm_screen_dispose(WinxtermScreen *screen);
bool winxterm_screen_start_session(WinxtermScreen *screen);
void winxterm_screen_resize(WinxtermScreen *screen, int columns, int rows);
bool winxterm_screen_apply_op(WinxtermScreen *screen, const WinxtermTerminalOp *op);
bool winxterm_screen_append_codepoint(WinxtermScreen *screen, uint32_t codepoint);
bool winxterm_screen_newline(WinxtermScreen *screen);
void winxterm_screen_carriage_return(WinxtermScreen *screen);
void winxterm_screen_backspace(WinxtermScreen *screen);
bool winxterm_screen_tab(WinxtermScreen *screen);
void winxterm_screen_clear_current_session(WinxtermScreen *screen);
void winxterm_screen_clear_line(WinxtermScreen *screen);
void winxterm_screen_set_foreground_rgb(WinxtermScreen *screen, uint32_t foreground_rgb);
void winxterm_screen_set_background_rgb(WinxtermScreen *screen, uint32_t background_rgb);
void winxterm_screen_set_foreground_index(WinxtermScreen *screen, int palette_index);
void winxterm_screen_set_background_index(WinxtermScreen *screen, int palette_index);
void winxterm_screen_reset_foreground(WinxtermScreen *screen);
void winxterm_screen_reset_background(WinxtermScreen *screen);
void winxterm_screen_set_text_attributes(WinxtermScreen *screen, uint32_t flags);
void winxterm_screen_reset_text_attributes(WinxtermScreen *screen, uint32_t flags);
void winxterm_screen_push_attributes(WinxtermScreen *screen);
void winxterm_screen_pop_attributes(WinxtermScreen *screen);
void winxterm_screen_reset_attributes(WinxtermScreen *screen);
bool winxterm_screen_mode_enabled(const WinxtermScreen *screen, WinxtermTerminalMode mode);
uint32_t winxterm_screen_map_codepoint_to_glyph(uint32_t codepoint);
WinxtermScreenCell *winxterm_screen_cell_at(WinxtermScreen *screen, int row, int column);
size_t winxterm_screen_scrollback_count(const WinxtermScreen *screen);
size_t winxterm_screen_primary_view_row_count(const WinxtermScreen *screen);
size_t winxterm_screen_default_primary_first_row_for_rows(const WinxtermScreen *screen,
                                                          int visible_rows,
                                                          size_t offset_from_bottom);
size_t winxterm_screen_default_primary_first_row(const WinxtermScreen *screen, size_t offset_from_bottom);
bool winxterm_screen_get_primary_view_row(const WinxtermScreen *screen,
                                          size_t global_row,
                                          WinxtermScreenRowView *row);
bool winxterm_screen_primary_anchor_from_global_row(const WinxtermScreen *screen,
                                                    size_t global_row,
                                                    WinxtermScreenPrimaryAnchor *anchor);
bool winxterm_screen_primary_global_row_from_anchor(const WinxtermScreen *screen,
                                                    const WinxtermScreenPrimaryAnchor *anchor,
                                                    size_t *global_row);
bool winxterm_screen_get_alternate_view_row(const WinxtermScreen *screen,
                                            size_t row_index,
                                            WinxtermScreenRowView *row);
void winxterm_screen_clear_scrollback(WinxtermScreen *screen);
void winxterm_screen_mark_full_repaint(WinxtermScreen *screen);
bool winxterm_screen_render_state_init(WinxtermScreenRenderState *state,
                                      WinxtermScreen *screen,
                                      int bitmap_width,
                                      int bitmap_height,
                                      bool cursor_visible,
                                      const WinxtermScreenRenderView *view);
void winxterm_screen_render_state_rows(const WinxtermScreenRenderState *state,
                                       WinxtermScreen *screen,
                                       WinxtermRenderContext *render_context,
                                       uint32_t *pixels,
                                       int row_start,
                                       int row_count);
uint64_t winxterm_screen_visual_line_advances(const WinxtermScreen *screen);
#endif
