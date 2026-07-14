#include "winxterm_screen.h"

#include "winxterm_diagnostics.h"
#include "winxterm_font_6x13.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t winxterm_screen_ansi_dark_rgb[8] = {
    0x00000000u,
    0x00800000u,
    0x00008000u,
    0x00808000u,
    0x00000080u,
    0x00800080u,
    0x00008080u,
    0x00c0c0c0u,
};

static const uint32_t winxterm_screen_ansi_bright_rgb[8] = {
    0x00808080u,
    0x00ff0000u,
    0x0000ff00u,
    0x00ffff00u,
    0x000000ffu,
    0x00ff00ffu,
    0x0000ffffu,
    0x00ffffffu,
};

static uint32_t winxterm_screen_rgb_from_256(int index)
{
    static const uint8_t cube_values[6] = {0u, 95u, 135u, 175u, 215u, 255u};
    if (index < 0) {
        index = 0;
    } else if (index > 255) {
        index = 255;
    }
    if (index < 8) {
        return winxterm_screen_ansi_dark_rgb[index];
    }
    if (index < 16) {
        return winxterm_screen_ansi_bright_rgb[index - 8];
    }
    if (index < 232) {
        int cube = index - 16;
        uint8_t red = cube_values[cube / 36];
        uint8_t green = cube_values[(cube / 6) % 6];
        uint8_t blue = cube_values[cube % 6];
        return ((uint32_t)red << 16) | ((uint32_t)green << 8) | (uint32_t)blue;
    }
    {
        uint8_t gray = (uint8_t)(8 + (index - 232) * 10);
        return ((uint32_t)gray << 16) | ((uint32_t)gray << 8) | (uint32_t)gray;
    }
}

static void winxterm_screen_reset_palette(WinxtermScreenPalette *palette)
{
    if (palette == 0) {
        return;
    }
    for (int i = 0; i < 256; ++i) {
        palette->slots[i] = winxterm_screen_rgb_from_256(i);
    }
    palette->default_foreground_rgb = WINXTERM_DEFAULT_FOREGROUND_RGB;
    palette->default_background_rgb = WINXTERM_DEFAULT_BACKGROUND_RGB;
}

static WinxtermScreenCell winxterm_screen_blank_cell(const WinxtermScreen *screen)
{
    WinxtermScreenCell cell;
    cell.codepoint = (uint32_t)' ';
    cell.glyph_index = (uint32_t)' ';
    cell.source_byte_count = 0u;
    cell.foreground_rgb = screen != 0 ? screen->foreground_rgb : WINXTERM_DEFAULT_FOREGROUND_RGB;
    cell.background_rgb = screen != 0 ? screen->background_rgb : WINXTERM_DEFAULT_BACKGROUND_RGB;
    cell.attribute_flags = screen != 0 ? screen->attribute_flags : 0u;
    if (screen != 0 && screen->protected_mode) {
        cell.attribute_flags |= WINXTERM_SCREEN_CELL_PROTECTED;
    }
    memset(cell.combining_codepoints, 0, sizeof(cell.combining_codepoints));
    cell.foreground_palette_index = screen != 0 ? screen->foreground_palette_index : 0u;
    cell.background_palette_index = screen != 0 ? screen->background_palette_index : 0u;
    cell.combining_count = 0u;
    cell.color_flags = screen != 0 ? screen->color_flags : (uint8_t)(WINXTERM_SCREEN_COLOR_FOREGROUND_DEFAULT |
                                                                    WINXTERM_SCREEN_COLOR_BACKGROUND_DEFAULT);
    cell.width = 1u;
    cell.source_rendered = false;
    cell.continuation = false;
    cell.occupied = false;
    return cell;
}

static WinxtermScreenCell winxterm_screen_erased_cell(const WinxtermScreen *screen)
{
    WinxtermScreenCell cell = winxterm_screen_blank_cell(screen);
    cell.attribute_flags &= WINXTERM_SCREEN_CELL_PROTECTED;
    cell.foreground_rgb = screen != 0 ? screen->palette.default_foreground_rgb : WINXTERM_DEFAULT_FOREGROUND_RGB;
    cell.foreground_palette_index = 7u;
    cell.color_flags &= (uint8_t)(0xffu ^ WINXTERM_SCREEN_COLOR_FOREGROUND_INDEXED);
    cell.color_flags |= WINXTERM_SCREEN_COLOR_FOREGROUND_DEFAULT;
    return cell;
}

static WinxtermScreenCell *winxterm_screen_active_cells(WinxtermScreen *screen)
{
    if (screen == 0) {
        return 0;
    }
    return screen->alternate_active ? screen->alternate_cells : screen->primary_cells;
}

static WinxtermScreenLineMeta *winxterm_screen_active_line_meta(WinxtermScreen *screen)
{
    if (screen == 0) {
        return 0;
    }
    return screen->alternate_active ? screen->alternate_line_meta : screen->primary_line_meta;
}

static void winxterm_screen_note_skipped_cell(WinxtermScreen *screen, const WinxtermScreenCell *cell)
{
    if (screen == 0 || cell == 0 || screen->active_diagnostics == 0 ||
        cell->source_byte_count == 0u || cell->source_rendered) {
        return;
    }
    winxterm_diag_inc_u64(&screen->active_diagnostics->skipped_cells);
    winxterm_diag_add_u64(&screen->active_diagnostics->skipped_output_bytes, cell->source_byte_count);
}

static void winxterm_screen_note_skipped_cells(WinxtermScreen *screen,
                                               const WinxtermScreenCell *cells,
                                               size_t count)
{
    if (screen == 0 || cells == 0 || screen->active_diagnostics == 0) {
        return;
    }
    for (size_t i = 0u; i < count; ++i) {
        winxterm_screen_note_skipped_cell(screen, cells + i);
    }
}

static void winxterm_screen_note_rendered_cell(WinxtermScreen *screen, WinxtermScreenCell *cell)
{
    if (screen == 0 || cell == 0 || screen->active_diagnostics == 0) {
        return;
    }
    winxterm_diag_inc_u64(&screen->active_diagnostics->rendered_cells);
    if (cell->source_byte_count != 0u && !cell->source_rendered) {
        winxterm_diag_add_u64(&screen->active_diagnostics->rendered_output_bytes,
                              cell->source_byte_count);
        cell->source_rendered = true;
    }
}

static size_t winxterm_screen_grid_count(int columns, int rows)
{
    if (columns <= 0 || rows <= 0) {
        return 0u;
    }
    return (size_t)columns * (size_t)rows;
}

void winxterm_screen_mark_full_repaint(WinxtermScreen *screen)
{
    if (screen == 0) {
        return;
    }
    screen->damage.full_repaint = true;
    screen->damage.dirty = false;
    screen->damage.dirty_top = 0;
    screen->damage.dirty_bottom = screen->rows > 0 ? screen->rows - 1 : 0;
    screen->damage.scroll_delta = 0;
}

static void winxterm_screen_mark_dirty_row(WinxtermScreen *screen, int row)
{
    if (screen == 0 || row < 0 || row >= screen->rows || screen->damage.full_repaint) {
        return;
    }
    if (!screen->damage.dirty) {
        screen->damage.dirty = true;
        screen->damage.dirty_top = row;
        screen->damage.dirty_bottom = row;
    } else {
        if (row < screen->damage.dirty_top) {
            screen->damage.dirty_top = row;
        }
        if (row > screen->damage.dirty_bottom) {
            screen->damage.dirty_bottom = row;
        }
    }
}

bool winxterm_screen_take_damage(WinxtermScreen *screen, WinxtermScreenDamage *damage)
{
    if (damage != 0) {
        memset(damage, 0, sizeof(*damage));
    }
    if (screen == 0 || damage == 0) {
        return false;
    }
    *damage = screen->damage;
    memset(&screen->damage, 0, sizeof(screen->damage));
    return damage->full_repaint || damage->dirty || damage->scroll_delta != 0;
}

static void winxterm_screen_fill_cells(WinxtermScreen *screen,
                                       WinxtermScreenCell *cells,
                                       size_t count)
{
    if (screen == 0 || cells == 0) {
        return;
    }
    winxterm_screen_note_skipped_cells(screen, cells, count);
    WinxtermScreenCell blank = winxterm_screen_blank_cell(screen);
    for (size_t i = 0; i < count; ++i) {
        cells[i] = blank;
    }
}

static bool winxterm_screen_allocate_grid(WinxtermScreen *screen)
{
    size_t count = winxterm_screen_grid_count(screen->columns, screen->rows);
    if (count == 0u) {
        return false;
    }
    screen->primary_cells = (WinxtermScreenCell *)calloc(count, sizeof(*screen->primary_cells));
    screen->alternate_cells = (WinxtermScreenCell *)calloc(count, sizeof(*screen->alternate_cells));
    screen->primary_line_meta = (WinxtermScreenLineMeta *)calloc((size_t)screen->rows,
                                                                 sizeof(*screen->primary_line_meta));
    screen->alternate_line_meta = (WinxtermScreenLineMeta *)calloc((size_t)screen->rows,
                                                                   sizeof(*screen->alternate_line_meta));
    screen->tab_stops = (bool *)calloc((size_t)screen->columns, sizeof(*screen->tab_stops));
    if (screen->primary_cells == 0 || screen->alternate_cells == 0 ||
        screen->primary_line_meta == 0 || screen->alternate_line_meta == 0 ||
        screen->tab_stops == 0) {
        return false;
    }
    winxterm_screen_fill_cells(screen, screen->primary_cells, count);
    winxterm_screen_fill_cells(screen, screen->alternate_cells, count);
    for (int column = 8; column < screen->columns; column += 8) {
        screen->tab_stops[column] = true;
    }
    return true;
}

static void winxterm_screen_line_dispose(WinxtermScreenLine *line)
{
    if (line == 0) {
        return;
    }
    free(line->cells);
    memset(line, 0, sizeof(*line));
}

static void winxterm_screen_dispose_scrollback(WinxtermScreen *screen)
{
    if (screen == 0) {
        return;
    }
    for (size_t i = 0; i < screen->scrollback_count; ++i) {
        winxterm_screen_line_dispose(&screen->scrollback_lines[i]);
    }
    free(screen->scrollback_lines);
    screen->scrollback_lines = 0;
    screen->scrollback_count = 0;
    screen->scrollback_capacity = 0;
}

static void winxterm_screen_reset_cursor_state(WinxtermScreen *screen)
{
    winxterm_mode_state_reset_hard(&screen->mode_state);
    winxterm_mode_state_reset_hard(&screen->saved_mode_state);
    screen->cursor_row = 0;
    screen->cursor_col = 0;
    screen->scroll_top = 0;
    screen->scroll_bottom = screen->rows > 0 ? screen->rows - 1 : 0;
    screen->left_margin = 0;
    screen->right_margin = screen->columns > 0 ? screen->columns - 1 : 0;
    screen->foreground_rgb = screen->palette.default_foreground_rgb;
    screen->background_rgb = screen->palette.default_background_rgb;
    screen->foreground_palette_index = 7u;
    screen->background_palette_index = 0u;
    screen->attribute_flags = 0u;
    screen->color_flags = (uint8_t)(WINXTERM_SCREEN_COLOR_FOREGROUND_DEFAULT |
                                    WINXTERM_SCREEN_COLOR_BACKGROUND_DEFAULT);
    screen->attribute_stack_count = 0u;
    screen->origin_mode = screen->mode_state.origin;
    screen->auto_wrap = screen->mode_state.auto_wrap;
    screen->pending_wrap = false;
    screen->protected_mode = false;
    screen->cursor_visible = screen->mode_state.cursor_visible;
    screen->cursor_style = WINXTERM_TERMINAL_CURSOR_STYLE_DEFAULT;
    screen->g0_charset = WINXTERM_TERMINAL_CHARSET_ASCII;
    screen->last_printed_codepoint = (uint32_t)' ';
}

bool winxterm_screen_init(WinxtermScreen *screen, int columns, int rows)
{
    if (screen == 0) {
        return false;
    }
    memset(screen, 0, sizeof(*screen));
    screen->columns = columns > 0 ? columns : WINXTERM_TERMINAL_COLUMNS;
    screen->rows = rows > 0 ? rows : WINXTERM_TERMINAL_ROWS;
    winxterm_screen_reset_palette(&screen->palette);
    winxterm_screen_reset_cursor_state(screen);
    if (!winxterm_screen_allocate_grid(screen)) {
        winxterm_screen_dispose(screen);
        return false;
    }
    winxterm_screen_mark_full_repaint(screen);
    return true;
}

void winxterm_screen_dispose(WinxtermScreen *screen)
{
    if (screen == 0) {
        return;
    }
    free(screen->primary_cells);
    free(screen->alternate_cells);
    free(screen->primary_line_meta);
    free(screen->alternate_line_meta);
    free(screen->tab_stops);
    winxterm_screen_dispose_scrollback(screen);
    memset(screen, 0, sizeof(*screen));
}

bool winxterm_screen_start_session(WinxtermScreen *screen)
{
    if (screen == 0) {
        return false;
    }
    winxterm_screen_dispose_scrollback(screen);
    winxterm_screen_fill_cells(screen,
                               screen->primary_cells,
                               winxterm_screen_grid_count(screen->columns, screen->rows));
    winxterm_screen_fill_cells(screen,
                               screen->alternate_cells,
                               winxterm_screen_grid_count(screen->columns, screen->rows));
    if (screen->primary_line_meta != 0) {
        memset(screen->primary_line_meta, 0, (size_t)screen->rows * sizeof(*screen->primary_line_meta));
    }
    if (screen->alternate_line_meta != 0) {
        memset(screen->alternate_line_meta, 0, (size_t)screen->rows * sizeof(*screen->alternate_line_meta));
    }
    screen->alternate_active = false;
    winxterm_screen_reset_cursor_state(screen);
    winxterm_screen_mark_full_repaint(screen);
    return true;
}

uint32_t winxterm_screen_map_codepoint_to_glyph(uint32_t codepoint)
{
    return codepoint <= 0xffu ? codepoint : WINXTERM_DYNAMIC_GLYPH_INDEX;
}

WinxtermScreenCell *winxterm_screen_cell_at(WinxtermScreen *screen, int row, int column)
{
    if (screen == 0 || row < 0 || column < 0 || row >= screen->rows || column >= screen->columns) {
        return 0;
    }
    WinxtermScreenCell *cells = winxterm_screen_active_cells(screen);
    return cells != 0 ? &cells[(size_t)row * (size_t)screen->columns + (size_t)column] : 0;
}

static void winxterm_screen_clamp_cursor(WinxtermScreen *screen)
{
    if (screen->cursor_row < 0) {
        screen->cursor_row = 0;
    } else if (screen->cursor_row >= screen->rows) {
        screen->cursor_row = screen->rows - 1;
    }
    if (screen->cursor_col < 0) {
        screen->cursor_col = 0;
    } else if (screen->cursor_col >= screen->columns) {
        screen->cursor_col = screen->columns - 1;
    }
}

static bool winxterm_screen_has_horizontal_margins(const WinxtermScreen *screen)
{
    return screen != 0 &&
           (screen->left_margin > 0 || screen->right_margin < screen->columns - 1);
}

static void winxterm_screen_clamp_cursor_to_margins(WinxtermScreen *screen)
{
    winxterm_screen_clamp_cursor(screen);
    if (winxterm_screen_has_horizontal_margins(screen)) {
        if (screen->cursor_col < screen->left_margin) {
            screen->cursor_col = screen->left_margin;
        } else if (screen->cursor_col > screen->right_margin) {
            screen->cursor_col = screen->right_margin;
        }
    }
}

static void winxterm_screen_set_row_soft_wrapped(WinxtermScreen *screen, int row, bool soft_wrapped)
{
    WinxtermScreenLineMeta *meta = winxterm_screen_active_line_meta(screen);
    if (meta != 0 && row >= 0 && row < screen->rows) {
        meta[row].soft_wrapped = soft_wrapped;
    }
}

static int winxterm_screen_row_content_columns(const WinxtermScreenCell *cells, int columns)
{
    int end = columns;
    while (end > 0 && !cells[end - 1].occupied) {
        --end;
    }
    return end;
}

static void winxterm_screen_recompute_row_content(WinxtermScreen *screen, int row)
{
    WinxtermScreenCell *cells = winxterm_screen_active_cells(screen);
    WinxtermScreenLineMeta *meta = winxterm_screen_active_line_meta(screen);
    if (cells != 0 && meta != 0 && row >= 0 && row < screen->rows) {
        meta[row].content_columns = winxterm_screen_row_content_columns(
            &cells[(size_t)row * (size_t)screen->columns], screen->columns);
    }
}

static uint32_t winxterm_screen_map_dec_special_graphics(uint32_t codepoint)
{
    switch (codepoint) {
    case '`': return 0x25c6u;
    case 'a': return 0x2592u;
    case 'b': return 0x2409u;
    case 'c': return 0x240cu;
    case 'd': return 0x240du;
    case 'e': return 0x240au;
    case 'f': return 0x00b0u;
    case 'g': return 0x00b1u;
    case 'h': return 0x2424u;
    case 'i': return 0x240bu;
    case 'j': return 0x2518u;
    case 'k': return 0x2510u;
    case 'l': return 0x250cu;
    case 'm': return 0x2514u;
    case 'n': return 0x253cu;
    case 'o': return 0x23bau;
    case 'p': return 0x23bbu;
    case 'q': return 0x2500u;
    case 'r': return 0x23bcu;
    case 's': return 0x23bdu;
    case 't': return 0x251cu;
    case 'u': return 0x2524u;
    case 'v': return 0x2534u;
    case 'w': return 0x252cu;
    case 'x': return 0x2502u;
    case 'y': return 0x2264u;
    case 'z': return 0x2265u;
    case '{': return 0x03c0u;
    case '|': return 0x2260u;
    case '}': return 0x00a3u;
    case '~': return 0x00b7u;
    default: return codepoint;
    }
}

static uint32_t winxterm_screen_map_cell_codepoint(const WinxtermScreen *screen, uint32_t codepoint)
{
    if (screen != 0 && screen->g0_charset == WINXTERM_TERMINAL_CHARSET_DEC_SPECIAL_GRAPHICS) {
        return winxterm_screen_map_dec_special_graphics(codepoint);
    }
    return codepoint;
}

static bool winxterm_screen_codepoint_is_combining(uint32_t codepoint)
{
    return (codepoint >= 0x0300u && codepoint <= 0x036fu) ||
           (codepoint >= 0x1ab0u && codepoint <= 0x1affu) ||
           (codepoint >= 0x1dc0u && codepoint <= 0x1dffu) ||
           (codepoint >= 0x20d0u && codepoint <= 0x20ffu) ||
           (codepoint >= 0xfe00u && codepoint <= 0xfe0fu) ||
           (codepoint >= 0xfe20u && codepoint <= 0xfe2fu) ||
           (codepoint >= 0x1f3fbu && codepoint <= 0x1f3ffu);
}

static int winxterm_screen_codepoint_width(uint32_t codepoint)
{
    if (winxterm_screen_codepoint_is_combining(codepoint)) {
        return 0;
    }
    if (codepoint < 0x20u ||
        (codepoint >= 0x7fu && codepoint < 0xa0u)) {
        return 0;
    }
    /* Every independently rendered glyph uses one fixed terminal cell. */
    return 1;
}

static WinxtermScreenCell *winxterm_screen_previous_printed_cell(WinxtermScreen *screen)
{
    if (screen == 0) {
        return 0;
    }
    int row = screen->cursor_row;
    int column = screen->cursor_col - 1;
    if (column < screen->left_margin && row > 0) {
        --row;
        column = screen->right_margin;
    }
    while (row >= 0 && column >= 0) {
        WinxtermScreenCell *cell = winxterm_screen_cell_at(screen, row, column);
        if (cell != 0 && !cell->continuation && cell->width != 0u) {
            return cell;
        }
        --column;
    }
    return 0;
}

static bool winxterm_screen_reserve_scrollback(WinxtermScreen *screen, size_t capacity)
{
    if (screen->scrollback_capacity >= capacity) {
        return true;
    }
    size_t new_capacity = screen->scrollback_capacity == 0u ? 64u : screen->scrollback_capacity * 2u;
    while (new_capacity < capacity) {
        new_capacity *= 2u;
    }
    WinxtermScreenLine *lines =
        (WinxtermScreenLine *)realloc(screen->scrollback_lines, new_capacity * sizeof(*lines));
    if (lines == 0) {
        return false;
    }
    for (size_t i = screen->scrollback_capacity; i < new_capacity; ++i) {
        memset(&lines[i], 0, sizeof(lines[i]));
    }
    screen->scrollback_lines = lines;
    screen->scrollback_capacity = new_capacity;
    return true;
}

static bool winxterm_screen_push_scrollback_row(WinxtermScreen *screen,
                                                const WinxtermScreenCell *row_cells,
                                                bool soft_wrapped)
{
    if (screen == 0 || row_cells == 0 || screen->alternate_active) {
        return true;
    }
    winxterm_screen_note_skipped_cells(screen, row_cells, (size_t)screen->columns);
    if (screen->scrollback_count == WINXTERM_SCREEN_SCROLLBACK_LINE_CAP) {
        winxterm_screen_note_skipped_cells(screen,
                                           screen->scrollback_lines[0].cells,
                                           (size_t)screen->scrollback_lines[0].columns);
        winxterm_screen_line_dispose(&screen->scrollback_lines[0]);
        memmove(&screen->scrollback_lines[0],
                &screen->scrollback_lines[1],
                (screen->scrollback_count - 1u) * sizeof(screen->scrollback_lines[0]));
        --screen->scrollback_count;
        memset(&screen->scrollback_lines[screen->scrollback_count], 0, sizeof(screen->scrollback_lines[0]));
    }
    if (!winxterm_screen_reserve_scrollback(screen, screen->scrollback_count + 1u)) {
        return false;
    }
    WinxtermScreenLine *line = &screen->scrollback_lines[screen->scrollback_count];
    line->cells = (WinxtermScreenCell *)malloc((size_t)screen->columns * sizeof(*line->cells));
    if (line->cells == 0) {
        return false;
    }
    memcpy(line->cells, row_cells, (size_t)screen->columns * sizeof(*line->cells));
    line->columns = screen->columns;
    line->soft_wrapped = soft_wrapped;
    line->content_columns = soft_wrapped ? screen->columns :
        winxterm_screen_row_content_columns(row_cells, screen->columns);
    ++screen->scrollback_count;
    return true;
}

static void winxterm_screen_clear_row(WinxtermScreen *screen, int row)
{
    WinxtermScreenCell *cells = winxterm_screen_active_cells(screen);
    if (cells == 0 || row < 0 || row >= screen->rows) {
        return;
    }
    winxterm_screen_note_skipped_cells(screen,
                                       &cells[(size_t)row * (size_t)screen->columns],
                                       (size_t)screen->columns);
    WinxtermScreenCell blank = winxterm_screen_erased_cell(screen);
    for (int column = 0; column < screen->columns; ++column) {
        cells[(size_t)row * (size_t)screen->columns + (size_t)column] = blank;
    }
    WinxtermScreenLineMeta *meta = winxterm_screen_active_line_meta(screen);
    if (meta != 0) {
        meta[row].content_columns = 0;
    }
    winxterm_screen_recompute_row_content(screen, row);
    winxterm_screen_mark_dirty_row(screen, row);
}

static bool winxterm_screen_scroll_region_up(WinxtermScreen *screen, int count)
{
    if (screen == 0 || count <= 0) {
        return true;
    }
    if (count > screen->scroll_bottom - screen->scroll_top + 1) {
        count = screen->scroll_bottom - screen->scroll_top + 1;
    }
    bool full_width_scroll = screen->scroll_top == 0 && screen->scroll_bottom == screen->rows - 1;
    if (full_width_scroll) {
        screen->damage.scroll_delta += count;
    } else {
        winxterm_screen_mark_full_repaint(screen);
    }
    WinxtermScreenCell *cells = winxterm_screen_active_cells(screen);
    if (cells == 0) {
        return false;
    }
    WinxtermScreenLineMeta *meta = winxterm_screen_active_line_meta(screen);
    WinxtermScreenCursorState *saved = screen->alternate_active ?
        &screen->saved_alternate_cursor : &screen->saved_primary_cursor;
    for (int i = 0; i < count; ++i) {
        int top = screen->scroll_top;
        if (top == 0) {
            bool soft_wrapped = meta != 0 ? meta[0].soft_wrapped : false;
            if (!winxterm_screen_push_scrollback_row(screen, &cells[0], soft_wrapped)) {
                return false;
            }
        }
        size_t row_cells = (size_t)screen->columns * sizeof(*cells);
        if (screen->scroll_bottom > top) {
            memmove(&cells[(size_t)top * (size_t)screen->columns],
                    &cells[(size_t)(top + 1) * (size_t)screen->columns],
                    (size_t)(screen->scroll_bottom - top) * row_cells);
            if (meta != 0) {
                memmove(&meta[top],
                        &meta[top + 1],
                        (size_t)(screen->scroll_bottom - top) * sizeof(meta[0]));
            }
        }
        winxterm_screen_clear_row(screen, screen->scroll_bottom);
        if (meta != 0) {
            meta[screen->scroll_bottom].soft_wrapped = false;
        }
        if (saved->row >= screen->scroll_top && saved->row <= screen->scroll_bottom) {
            if (saved->row > screen->scroll_top) --saved->row;
            else saved->row = screen->scroll_top;
        }
    }
    return true;
}

static void winxterm_screen_scroll_region_down(WinxtermScreen *screen, int count)
{
    if (screen == 0 || count <= 0) {
        return;
    }
    winxterm_screen_mark_full_repaint(screen);
    if (count > screen->scroll_bottom - screen->scroll_top + 1) {
        count = screen->scroll_bottom - screen->scroll_top + 1;
    }
    WinxtermScreenCell *cells = winxterm_screen_active_cells(screen);
    if (cells == 0) {
        return;
    }
    WinxtermScreenLineMeta *meta = winxterm_screen_active_line_meta(screen);
    WinxtermScreenCursorState *saved = screen->alternate_active ?
        &screen->saved_alternate_cursor : &screen->saved_primary_cursor;
    for (int i = 0; i < count; ++i) {
        size_t row_cells = (size_t)screen->columns * sizeof(*cells);
        if (screen->scroll_bottom > screen->scroll_top) {
            memmove(&cells[(size_t)(screen->scroll_top + 1) * (size_t)screen->columns],
                    &cells[(size_t)screen->scroll_top * (size_t)screen->columns],
                    (size_t)(screen->scroll_bottom - screen->scroll_top) * row_cells);
            if (meta != 0) {
                memmove(&meta[screen->scroll_top + 1],
                        &meta[screen->scroll_top],
                        (size_t)(screen->scroll_bottom - screen->scroll_top) * sizeof(meta[0]));
            }
        }
        winxterm_screen_clear_row(screen, screen->scroll_top);
        if (meta != 0) {
            meta[screen->scroll_top].soft_wrapped = false;
        }
        if (saved->row >= screen->scroll_top && saved->row <= screen->scroll_bottom) {
            if (saved->row < screen->scroll_bottom) ++saved->row;
            else saved->row = screen->scroll_bottom;
        }
    }
}

static bool winxterm_screen_linefeed(WinxtermScreen *screen)
{
    int old_row = screen->cursor_row;
    screen->pending_wrap = false;
    if (screen->visual_line_advances != UINT64_MAX) {
        ++screen->visual_line_advances;
    }
    winxterm_screen_set_row_soft_wrapped(screen, screen->cursor_row, false);
    if (screen->cursor_row == screen->scroll_bottom) {
        screen->cursor_col = 0;
        return winxterm_screen_scroll_region_up(screen, 1);
    }
    ++screen->cursor_row;
    screen->cursor_col = 0;
    winxterm_screen_clamp_cursor(screen);
    winxterm_screen_mark_dirty_row(screen, old_row);
    winxterm_screen_mark_dirty_row(screen, screen->cursor_row);
    return true;
}

static bool winxterm_screen_index(WinxtermScreen *screen)
{
    int column = screen->cursor_col;
    int old_row = screen->cursor_row;
    screen->pending_wrap = false;
    if (screen->cursor_row == screen->scroll_bottom) {
        bool ok = winxterm_screen_scroll_region_up(screen, 1);
        screen->cursor_col = column;
        return ok;
    }
    ++screen->cursor_row;
    winxterm_screen_clamp_cursor(screen);
    screen->cursor_col = column;
    winxterm_screen_mark_dirty_row(screen, old_row);
    winxterm_screen_mark_dirty_row(screen, screen->cursor_row);
    return true;
}

static void winxterm_screen_reverse_index(WinxtermScreen *screen)
{
    int column = screen->cursor_col;
    int old_row = screen->cursor_row;
    screen->pending_wrap = false;
    if (screen->cursor_row == screen->scroll_top) {
        winxterm_screen_scroll_region_down(screen, 1);
        screen->cursor_col = column;
        return;
    }
    --screen->cursor_row;
    winxterm_screen_clamp_cursor(screen);
    screen->cursor_col = column;
    winxterm_screen_mark_dirty_row(screen, old_row);
    winxterm_screen_mark_dirty_row(screen, screen->cursor_row);
}

static void winxterm_screen_set_cell_width(WinxtermScreen *screen,
                                           int row,
                                           int column,
                                           uint32_t codepoint,
                                           int width,
                                           uint64_t source_byte_count)
{
    WinxtermScreenCell *cell = winxterm_screen_cell_at(screen, row, column);
    if (cell == 0) {
        return;
    }
    winxterm_screen_note_skipped_cell(screen, cell);
    if (cell->continuation && column > 0) {
        WinxtermScreenCell *leader = winxterm_screen_cell_at(screen, row, column - 1);
        if (leader != 0 && leader->width == 2u) {
            winxterm_screen_note_skipped_cell(screen, leader);
            *leader = winxterm_screen_blank_cell(screen);
        }
    }
    codepoint = winxterm_screen_map_cell_codepoint(screen, codepoint);
    cell->codepoint = codepoint;
    cell->glyph_index = winxterm_screen_map_codepoint_to_glyph(codepoint);
    cell->source_byte_count = source_byte_count;
    cell->foreground_rgb = screen->foreground_rgb;
    cell->background_rgb = screen->background_rgb;
    cell->attribute_flags = screen->attribute_flags;
    if (screen->protected_mode) {
        cell->attribute_flags |= WINXTERM_SCREEN_CELL_PROTECTED;
    }
    memset(cell->combining_codepoints, 0, sizeof(cell->combining_codepoints));
    cell->foreground_palette_index = screen->foreground_palette_index;
    cell->background_palette_index = screen->background_palette_index;
    cell->combining_count = 0u;
    cell->color_flags = screen->color_flags;
    cell->width = (uint8_t)width;
    cell->source_rendered = false;
    cell->continuation = false;
    cell->occupied = true;
    if (width == 2 && column + 1 < screen->columns) {
        WinxtermScreenCell *next = winxterm_screen_cell_at(screen, row, column + 1);
        if (next != 0) {
            winxterm_screen_note_skipped_cell(screen, next);
            *next = *cell;
            next->codepoint = 0u;
            next->glyph_index = (uint32_t)' ';
            next->combining_count = 0u;
            memset(next->combining_codepoints, 0, sizeof(next->combining_codepoints));
            next->source_byte_count = 0u;
            next->width = 0u;
            next->source_rendered = false;
            next->continuation = true;
            next->occupied = true;
        }
    } else if (column + 1 < screen->columns) {
        WinxtermScreenCell *next = winxterm_screen_cell_at(screen, row, column + 1);
        if (next != 0 && next->continuation) {
            winxterm_screen_note_skipped_cell(screen, next);
            *next = winxterm_screen_blank_cell(screen);
        }
    }
    screen->last_printed_codepoint = codepoint;
    WinxtermScreenLineMeta *meta = winxterm_screen_active_line_meta(screen);
    int extent = column + width;
    if (meta != 0 && row >= 0 && row < screen->rows && meta[row].content_columns < extent) {
        meta[row].content_columns = extent;
    }
    winxterm_screen_mark_dirty_row(screen, row);
}

static void winxterm_screen_set_cell(WinxtermScreen *screen, int row, int column, uint32_t codepoint)
{
    winxterm_screen_set_cell_width(screen, row, column, codepoint, 1, 0u);
}

static bool winxterm_screen_print_with_source(WinxtermScreen *screen,
                                              uint32_t codepoint,
                                              uint64_t source_byte_count)
{
    int codepoint_width = winxterm_screen_codepoint_width(codepoint);
    if (codepoint_width == 0) {
        WinxtermScreenCell *base = winxterm_screen_previous_printed_cell(screen);
        if (base != 0 && base->combining_count < WINXTERM_SCREEN_COMBINING_CAPACITY) {
            base->combining_codepoints[base->combining_count++] = codepoint;
            winxterm_screen_mark_dirty_row(screen, screen->cursor_row);
        }
        return true;
    }
    if (screen->pending_wrap) {
        if (screen->visual_line_advances != UINT64_MAX) {
            ++screen->visual_line_advances;
        }
        winxterm_screen_set_row_soft_wrapped(screen, screen->cursor_row, true);
        screen->cursor_col = screen->left_margin;
        if (!winxterm_screen_index(screen)) {
            return false;
        }
        screen->cursor_col = screen->left_margin;
    }
    winxterm_screen_clamp_cursor_to_margins(screen);
    if (codepoint_width == 2 && screen->cursor_col == screen->right_margin) {
        if (screen->auto_wrap) {
            winxterm_screen_set_row_soft_wrapped(screen, screen->cursor_row, true);
            screen->cursor_col = screen->left_margin;
            if (!winxterm_screen_index(screen)) {
                return false;
            }
            screen->cursor_col = screen->left_margin;
        } else {
            codepoint_width = 1;
        }
    }
    winxterm_screen_set_cell_width(screen,
                                   screen->cursor_row,
                                   screen->cursor_col,
                                   codepoint,
                                   codepoint_width,
                                   source_byte_count);
    if (screen->cursor_col + codepoint_width - 1 >= screen->right_margin) {
        screen->pending_wrap = screen->auto_wrap;
    } else {
        screen->cursor_col += codepoint_width;
        screen->pending_wrap = false;
    }
    return true;
}

static bool winxterm_screen_print(WinxtermScreen *screen, uint32_t codepoint)
{
    return winxterm_screen_print_with_source(screen, codepoint, 0u);
}

static void winxterm_screen_move_relative(WinxtermScreen *screen, int row_delta, int col_delta)
{
    int old_row = screen->cursor_row;
    screen->cursor_row += row_delta;
    screen->cursor_col += col_delta;
    if (screen->origin_mode) {
        if (screen->cursor_row < screen->scroll_top) {
            screen->cursor_row = screen->scroll_top;
        } else if (screen->cursor_row > screen->scroll_bottom) {
            screen->cursor_row = screen->scroll_bottom;
        }
    }
    if (winxterm_screen_has_horizontal_margins(screen)) {
        winxterm_screen_clamp_cursor_to_margins(screen);
    } else {
        winxterm_screen_clamp_cursor(screen);
    }
    screen->pending_wrap = false;
    winxterm_screen_mark_dirty_row(screen, old_row);
    winxterm_screen_mark_dirty_row(screen, screen->cursor_row);
}

static void winxterm_screen_cursor_next_line(WinxtermScreen *screen, int count)
{
    int old_row = screen->cursor_row;
    if (count <= 0) {
        count = 1;
    }
    screen->cursor_row += count;
    screen->cursor_col = 0;
    if (screen->origin_mode && screen->cursor_row > screen->scroll_bottom) {
        screen->cursor_row = screen->scroll_bottom;
    }
    winxterm_screen_clamp_cursor(screen);
    screen->pending_wrap = false;
    winxterm_screen_mark_dirty_row(screen, old_row);
    winxterm_screen_mark_dirty_row(screen, screen->cursor_row);
}

static void winxterm_screen_cursor_previous_line(WinxtermScreen *screen, int count)
{
    int old_row = screen->cursor_row;
    if (count <= 0) {
        count = 1;
    }
    screen->cursor_row -= count;
    screen->cursor_col = 0;
    if (screen->origin_mode && screen->cursor_row < screen->scroll_top) {
        screen->cursor_row = screen->scroll_top;
    }
    winxterm_screen_clamp_cursor(screen);
    screen->pending_wrap = false;
    winxterm_screen_mark_dirty_row(screen, old_row);
    winxterm_screen_mark_dirty_row(screen, screen->cursor_row);
}

static void winxterm_screen_cursor_horizontal_absolute(WinxtermScreen *screen, int column)
{
    int old_row = screen->cursor_row;
    screen->cursor_col = column > 0 ? column - 1 : 0;
    if (screen->origin_mode && winxterm_screen_has_horizontal_margins(screen)) {
        screen->cursor_col += screen->left_margin;
    }
    winxterm_screen_clamp_cursor_to_margins(screen);
    screen->pending_wrap = false;
    winxterm_screen_mark_dirty_row(screen, old_row);
    winxterm_screen_mark_dirty_row(screen, screen->cursor_row);
}

static void winxterm_screen_cursor_vertical_absolute(WinxtermScreen *screen, int row)
{
    int old_row = screen->cursor_row;
    screen->cursor_row = row > 0 ? row - 1 : 0;
    if (screen->origin_mode) {
        screen->cursor_row += screen->scroll_top;
        if (screen->cursor_row > screen->scroll_bottom) {
            screen->cursor_row = screen->scroll_bottom;
        }
    }
    winxterm_screen_clamp_cursor(screen);
    screen->pending_wrap = false;
    winxterm_screen_mark_dirty_row(screen, old_row);
    winxterm_screen_mark_dirty_row(screen, screen->cursor_row);
}

static void winxterm_screen_set_cursor_position(WinxtermScreen *screen, int row, int column)
{
    int old_row = screen->cursor_row;
    int target_row = row > 0 ? row - 1 : 0;
    int target_col = column > 0 ? column - 1 : 0;
    if (screen->origin_mode) {
        target_row += screen->scroll_top;
        if (winxterm_screen_has_horizontal_margins(screen)) {
            target_col += screen->left_margin;
        }
        if (target_row > screen->scroll_bottom) {
            target_row = screen->scroll_bottom;
        }
    }
    screen->cursor_row = target_row;
    screen->cursor_col = target_col;
    if (winxterm_screen_has_horizontal_margins(screen)) {
        winxterm_screen_clamp_cursor_to_margins(screen);
    } else {
        winxterm_screen_clamp_cursor(screen);
    }
    screen->pending_wrap = false;
    winxterm_screen_mark_dirty_row(screen, old_row);
    winxterm_screen_mark_dirty_row(screen, screen->cursor_row);
}

static void winxterm_screen_erase_line_range(WinxtermScreen *screen,
                                             int row,
                                             int first_col,
                                             int last_col,
                                             bool selective)
{
    if (row < 0 || row >= screen->rows) {
        return;
    }
    if (first_col < 0) {
        first_col = 0;
    }
    if (last_col >= screen->columns) {
        last_col = screen->columns - 1;
    }
    WinxtermScreenCell blank = winxterm_screen_erased_cell(screen);
    for (int column = first_col; column <= last_col; ++column) {
        WinxtermScreenCell *cell = winxterm_screen_cell_at(screen, row, column);
        if (cell != 0) {
            if (selective && (cell->attribute_flags & WINXTERM_SCREEN_CELL_PROTECTED) != 0u) {
                continue;
            }
            *cell = blank;
        }
    }
    winxterm_screen_recompute_row_content(screen, row);
    if (first_col == 0 && last_col == screen->columns - 1) {
        WinxtermScreenLineMeta *meta = winxterm_screen_active_line_meta(screen);
        if (meta != 0 && (!selective || meta[row].content_columns == 0)) {
            meta[row].soft_wrapped = false;
        }
    }
    winxterm_screen_mark_dirty_row(screen, row);
}

static void winxterm_screen_erase_display(WinxtermScreen *screen, WinxtermTerminalEraseMode mode, bool selective)
{
    if (mode == WINXTERM_TERMINAL_ERASE_SAVED_LINES) {
        if (!selective) {
            winxterm_screen_dispose_scrollback(screen);
        }
        return;
    }
    if (mode == WINXTERM_TERMINAL_ERASE_ALL) {
        for (int row = 0; row < screen->rows; ++row) {
            winxterm_screen_erase_line_range(screen, row, 0, screen->columns - 1, selective);
        }
    } else if (mode == WINXTERM_TERMINAL_ERASE_TO_BEGINNING) {
        for (int row = 0; row < screen->cursor_row; ++row) {
            winxterm_screen_erase_line_range(screen, row, 0, screen->columns - 1, selective);
        }
        winxterm_screen_erase_line_range(screen, screen->cursor_row, 0, screen->cursor_col, selective);
    } else {
        winxterm_screen_erase_line_range(screen,
                                         screen->cursor_row,
                                         screen->cursor_col,
                                         screen->columns - 1,
                                         selective);
        for (int row = screen->cursor_row + 1; row < screen->rows; ++row) {
            winxterm_screen_erase_line_range(screen, row, 0, screen->columns - 1, selective);
        }
    }
}

static void winxterm_screen_erase_line(WinxtermScreen *screen, WinxtermTerminalEraseMode mode, bool selective)
{
    if (mode == WINXTERM_TERMINAL_ERASE_ALL) {
        winxterm_screen_erase_line_range(screen, screen->cursor_row, 0, screen->columns - 1, selective);
    } else if (mode == WINXTERM_TERMINAL_ERASE_TO_BEGINNING) {
        winxterm_screen_erase_line_range(screen, screen->cursor_row, 0, screen->cursor_col, selective);
    } else {
        winxterm_screen_erase_line_range(screen,
                                         screen->cursor_row,
                                         screen->cursor_col,
                                         screen->columns - 1,
                                         selective);
    }
}

static bool winxterm_screen_tab_forward(WinxtermScreen *screen, int count)
{
    if (screen == 0) {
        return false;
    }
    if (count <= 0) {
        count = 1;
    }
    while (count-- > 0) {
        int next_tab = screen->columns - 1;
        for (int column = screen->cursor_col + 1; column < screen->columns; ++column) {
            if (screen->tab_stops != 0 && screen->tab_stops[column]) {
                next_tab = column;
                break;
            }
        }
        screen->cursor_col = next_tab;
    }
    screen->pending_wrap = false;
    return true;
}

static bool winxterm_screen_tab_back(WinxtermScreen *screen, int count)
{
    if (screen == 0) {
        return false;
    }
    if (count <= 0) {
        count = 1;
    }
    while (count-- > 0) {
        int previous_tab = 0;
        for (int column = screen->cursor_col - 1; column > 0; --column) {
            if (screen->tab_stops != 0 && screen->tab_stops[column]) {
                previous_tab = column;
                break;
            }
        }
        screen->cursor_col = previous_tab;
    }
    screen->pending_wrap = false;
    return true;
}

static void winxterm_screen_tab_set(WinxtermScreen *screen)
{
    if (screen != 0 && screen->tab_stops != 0 &&
        screen->cursor_col >= 0 && screen->cursor_col < screen->columns) {
        screen->tab_stops[screen->cursor_col] = true;
    }
}

static void winxterm_screen_tab_clear(WinxtermScreen *screen, WinxtermTerminalTabClearMode mode)
{
    if (screen == 0 || screen->tab_stops == 0) {
        return;
    }
    if (mode == WINXTERM_TERMINAL_TAB_CLEAR_ALL) {
        memset(screen->tab_stops, 0, (size_t)screen->columns * sizeof(*screen->tab_stops));
        return;
    }
    if (screen->cursor_col >= 0 && screen->cursor_col < screen->columns) {
        screen->tab_stops[screen->cursor_col] = false;
    }
}

static void winxterm_screen_insert_chars(WinxtermScreen *screen, int count)
{
    if (count <= 0) {
        count = 1;
    }
    int right = winxterm_screen_has_horizontal_margins(screen) ? screen->right_margin : screen->columns - 1;
    if (screen->cursor_col > right) {
        return;
    }
    if (count > right - screen->cursor_col + 1) {
        count = right - screen->cursor_col + 1;
    }
    WinxtermScreenCell *row = winxterm_screen_cell_at(screen, screen->cursor_row, 0);
    if (row == 0) {
        return;
    }
    memmove(&row[screen->cursor_col + count],
            &row[screen->cursor_col],
            (size_t)(right - screen->cursor_col + 1 - count) * sizeof(row[0]));
    WinxtermScreenCell blank = winxterm_screen_blank_cell(screen);
    for (int i = 0; i < count; ++i) {
        row[screen->cursor_col + i] = blank;
    }
    winxterm_screen_recompute_row_content(screen, screen->cursor_row);
}

static void winxterm_screen_delete_chars(WinxtermScreen *screen, int count)
{
    if (count <= 0) {
        count = 1;
    }
    int right = winxterm_screen_has_horizontal_margins(screen) ? screen->right_margin : screen->columns - 1;
    if (screen->cursor_col > right) {
        return;
    }
    if (count > right - screen->cursor_col + 1) {
        count = right - screen->cursor_col + 1;
    }
    WinxtermScreenCell *row = winxterm_screen_cell_at(screen, screen->cursor_row, 0);
    if (row == 0) {
        return;
    }
    memmove(&row[screen->cursor_col],
            &row[screen->cursor_col + count],
            (size_t)(right - screen->cursor_col + 1 - count) * sizeof(row[0]));
    WinxtermScreenCell blank = winxterm_screen_blank_cell(screen);
    for (int i = right - count + 1; i <= right; ++i) {
        row[i] = blank;
    }
    winxterm_screen_recompute_row_content(screen, screen->cursor_row);
}

static void winxterm_screen_insert_lines(WinxtermScreen *screen, int count)
{
    if (screen->cursor_row < screen->scroll_top || screen->cursor_row > screen->scroll_bottom) {
        return;
    }
    int old_top = screen->scroll_top;
    screen->scroll_top = screen->cursor_row;
    winxterm_screen_scroll_region_down(screen, count <= 0 ? 1 : count);
    screen->scroll_top = old_top;
}

static bool winxterm_screen_delete_lines(WinxtermScreen *screen, int count)
{
    if (screen->cursor_row < screen->scroll_top || screen->cursor_row > screen->scroll_bottom) {
        return true;
    }
    int old_top = screen->scroll_top;
    screen->scroll_top = screen->cursor_row;
    bool ok = winxterm_screen_scroll_region_up(screen, count <= 0 ? 1 : count);
    screen->scroll_top = old_top;
    return ok;
}

static void winxterm_screen_insert_columns(WinxtermScreen *screen, int count)
{
    if (count <= 0) {
        count = 1;
    }
    int left = screen->cursor_col < screen->left_margin ? screen->left_margin : screen->cursor_col;
    int right = screen->right_margin;
    if (left > right) {
        return;
    }
    if (count > right - left + 1) {
        count = right - left + 1;
    }
    WinxtermScreenCell blank = winxterm_screen_blank_cell(screen);
    WinxtermScreenCell *cells = winxterm_screen_active_cells(screen);
    if (cells == 0) {
        return;
    }
    for (int row = screen->scroll_top; row <= screen->scroll_bottom; ++row) {
        WinxtermScreenCell *line = &cells[(size_t)row * (size_t)screen->columns];
        memmove(&line[left + count], &line[left], (size_t)(right - left + 1 - count) * sizeof(line[0]));
        for (int column = left; column < left + count; ++column) {
            line[column] = blank;
        }
        winxterm_screen_recompute_row_content(screen, row);
    }
}

static void winxterm_screen_delete_columns(WinxtermScreen *screen, int count)
{
    if (count <= 0) {
        count = 1;
    }
    int left = screen->cursor_col < screen->left_margin ? screen->left_margin : screen->cursor_col;
    int right = screen->right_margin;
    if (left > right) {
        return;
    }
    if (count > right - left + 1) {
        count = right - left + 1;
    }
    WinxtermScreenCell blank = winxterm_screen_blank_cell(screen);
    WinxtermScreenCell *cells = winxterm_screen_active_cells(screen);
    if (cells == 0) {
        return;
    }
    for (int row = screen->scroll_top; row <= screen->scroll_bottom; ++row) {
        WinxtermScreenCell *line = &cells[(size_t)row * (size_t)screen->columns];
        memmove(&line[left], &line[left + count], (size_t)(right - left + 1 - count) * sizeof(line[0]));
        for (int column = right - count + 1; column <= right; ++column) {
            line[column] = blank;
        }
        winxterm_screen_recompute_row_content(screen, row);
    }
}

static void winxterm_screen_scroll_left(WinxtermScreen *screen, int count)
{
    int old_col = screen->cursor_col;
    screen->cursor_col = screen->left_margin;
    winxterm_screen_delete_columns(screen, count);
    screen->cursor_col = old_col;
    screen->pending_wrap = false;
}

static void winxterm_screen_scroll_right(WinxtermScreen *screen, int count)
{
    int old_col = screen->cursor_col;
    screen->cursor_col = screen->left_margin;
    winxterm_screen_insert_columns(screen, count);
    screen->cursor_col = old_col;
    screen->pending_wrap = false;
}

static void winxterm_screen_erase_chars(WinxtermScreen *screen, int count)
{
    if (count <= 0) {
        count = 1;
    }
    int right = winxterm_screen_has_horizontal_margins(screen) ? screen->right_margin : screen->columns - 1;
    int last = screen->cursor_col + count - 1;
    if (last > right) {
        last = right;
    }
    winxterm_screen_erase_line_range(screen, screen->cursor_row, screen->cursor_col, last, false);
    screen->pending_wrap = false;
}

static bool winxterm_screen_repeat_char(WinxtermScreen *screen, int count)
{
    if (count <= 0) {
        count = 1;
    }
    uint32_t codepoint = screen->last_printed_codepoint != 0u ? screen->last_printed_codepoint : (uint32_t)' ';
    for (int i = 0; i < count; ++i) {
        if (!winxterm_screen_print(screen, codepoint)) {
            return false;
        }
    }
    return true;
}

static void winxterm_screen_set_horizontal_margins(WinxtermScreen *screen, int left, int right)
{
    int target_left = left > 0 ? left - 1 : 0;
    int target_right = right > 0 ? right - 1 : screen->columns - 1;
    if (target_left < 0 || target_right >= screen->columns || target_right <= target_left) {
        screen->left_margin = 0;
        screen->right_margin = screen->columns - 1;
    } else {
        screen->left_margin = target_left;
        screen->right_margin = target_right;
    }
    winxterm_screen_set_cursor_position(screen, 1, 1);
}

static void winxterm_screen_rect_clip(WinxtermScreen *screen,
                                      int *top,
                                      int *left,
                                      int *bottom,
                                      int *right)
{
    *top = *top > 0 ? *top - 1 : 0;
    *left = *left > 0 ? *left - 1 : 0;
    *bottom = *bottom > 0 ? *bottom - 1 : screen->rows - 1;
    *right = *right > 0 ? *right - 1 : screen->columns - 1;
    if (*top < 0) *top = 0;
    if (*left < 0) *left = 0;
    if (*bottom >= screen->rows) *bottom = screen->rows - 1;
    if (*right >= screen->columns) *right = screen->columns - 1;
}

static void winxterm_screen_rect_erase(WinxtermScreen *screen,
                                       const WinxtermTerminalOp *op,
                                       bool selective)
{
    int top = op->data.rectangle.top;
    int left = op->data.rectangle.left;
    int bottom = op->data.rectangle.bottom;
    int right = op->data.rectangle.right;
    winxterm_screen_rect_clip(screen, &top, &left, &bottom, &right);
    if (top > bottom || left > right) {
        return;
    }
    for (int row = top; row <= bottom; ++row) {
        winxterm_screen_erase_line_range(screen, row, left, right, selective);
    }
}

static void winxterm_screen_rect_fill(WinxtermScreen *screen, const WinxtermTerminalOp *op)
{
    int top = op->data.rectangle.top;
    int left = op->data.rectangle.left;
    int bottom = op->data.rectangle.bottom;
    int right = op->data.rectangle.right;
    winxterm_screen_rect_clip(screen, &top, &left, &bottom, &right);
    if (top > bottom || left > right) {
        return;
    }
    for (int row = top; row <= bottom; ++row) {
        for (int column = left; column <= right; ++column) {
            winxterm_screen_set_cell(screen, row, column, op->data.rectangle.codepoint);
        }
    }
}

static void winxterm_screen_rect_attr(WinxtermScreen *screen, const WinxtermTerminalOp *op)
{
    int top = op->data.rectangle.top;
    int left = op->data.rectangle.left;
    int bottom = op->data.rectangle.bottom;
    int right = op->data.rectangle.right;
    winxterm_screen_rect_clip(screen, &top, &left, &bottom, &right);
    if (top > bottom || left > right) {
        return;
    }
    for (int row = top; row <= bottom; ++row) {
        for (int column = left; column <= right; ++column) {
            WinxtermScreenCell *cell = winxterm_screen_cell_at(screen, row, column);
            if (cell != 0) {
                if (op->data.rectangle.set_flags) {
                    cell->attribute_flags |= op->data.rectangle.flags;
                } else {
                    cell->attribute_flags &= ~op->data.rectangle.flags;
                }
            }
        }
    }
}

static void winxterm_screen_rect_copy(WinxtermScreen *screen, const WinxtermTerminalOp *op)
{
    int top = op->data.rectangle.top;
    int left = op->data.rectangle.left;
    int bottom = op->data.rectangle.bottom;
    int right = op->data.rectangle.right;
    winxterm_screen_rect_clip(screen, &top, &left, &bottom, &right);
    if (top > bottom || left > right) {
        return;
    }
    int height = bottom - top + 1;
    int width = right - left + 1;
    WinxtermScreenCell *temporary = (WinxtermScreenCell *)malloc((size_t)height * (size_t)width * sizeof(*temporary));
    if (temporary == 0) {
        return;
    }
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            temporary[(size_t)row * (size_t)width + (size_t)column] =
                *winxterm_screen_cell_at(screen, top + row, left + column);
        }
    }
    int dest_top = op->data.rectangle.dest_top > 0 ? op->data.rectangle.dest_top - 1 : 0;
    int dest_left = op->data.rectangle.dest_left > 0 ? op->data.rectangle.dest_left - 1 : 0;
    for (int row = 0; row < height; ++row) {
        for (int column = 0; column < width; ++column) {
            WinxtermScreenCell *dest = winxterm_screen_cell_at(screen, dest_top + row, dest_left + column);
            if (dest != 0) {
                *dest = temporary[(size_t)row * (size_t)width + (size_t)column];
            }
        }
        if (dest_top + row >= 0 && dest_top + row < screen->rows) {
            winxterm_screen_recompute_row_content(screen, dest_top + row);
        }
    }
    free(temporary);
}

static void winxterm_screen_save_cursor(WinxtermScreen *screen)
{
    WinxtermScreenCursorState *saved =
        screen->alternate_active ? &screen->saved_alternate_cursor : &screen->saved_primary_cursor;
    saved->row = screen->cursor_row;
    saved->column = screen->cursor_col;
    saved->foreground_rgb = screen->foreground_rgb;
    saved->background_rgb = screen->background_rgb;
    saved->foreground_palette_index = screen->foreground_palette_index;
    saved->background_palette_index = screen->background_palette_index;
    saved->attribute_flags = screen->attribute_flags;
    saved->color_flags = screen->color_flags;
    saved->origin_mode = screen->origin_mode;
    saved->auto_wrap = screen->auto_wrap;
    saved->protected_mode = screen->protected_mode;
    saved->cursor_style = screen->cursor_style;
}

static void winxterm_screen_restore_cursor(WinxtermScreen *screen)
{
    WinxtermScreenCursorState *saved =
        screen->alternate_active ? &screen->saved_alternate_cursor : &screen->saved_primary_cursor;
    screen->cursor_row = saved->row;
    screen->cursor_col = saved->column;
    screen->foreground_rgb = saved->foreground_rgb;
    screen->background_rgb = saved->background_rgb;
    screen->foreground_palette_index = saved->foreground_palette_index;
    screen->background_palette_index = saved->background_palette_index;
    screen->attribute_flags = saved->attribute_flags & WINXTERM_SCREEN_CELL_VISUAL_MASK;
    screen->color_flags = saved->color_flags;
    if (screen->color_flags == 0u && screen->foreground_rgb == 0u && screen->background_rgb == 0u) {
        winxterm_screen_reset_attributes(screen);
    }
    screen->origin_mode = saved->origin_mode;
    screen->auto_wrap = saved->auto_wrap;
    screen->protected_mode = saved->protected_mode;
    screen->cursor_style = saved->cursor_style;
    winxterm_screen_clamp_cursor(screen);
    screen->pending_wrap = false;
}

static bool winxterm_screen_alt_mode(WinxtermTerminalMode mode)
{
    return mode == WINXTERM_TERMINAL_MODE_ALT_SCREEN ||
           mode == WINXTERM_TERMINAL_MODE_ALT_SCREEN_1047 ||
           mode == WINXTERM_TERMINAL_MODE_ALT_SCREEN_1049;
}

static bool winxterm_screen_any_alt_mode_enabled(const WinxtermScreen *screen)
{
    return screen != 0 &&
           (screen->mode_state.alt_screen_47 ||
            screen->mode_state.alt_screen_1047 ||
            screen->mode_state.alt_screen_1049);
}

static void winxterm_screen_set_mode(WinxtermScreen *screen, WinxtermTerminalMode mode, bool enabled)
{
    if (mode == WINXTERM_TERMINAL_MODE_SAVE_CURSOR_1048) {
        if (enabled) {
            winxterm_screen_save_cursor(screen);
        } else {
            winxterm_screen_restore_cursor(screen);
        }
    } else if (mode == WINXTERM_TERMINAL_MODE_ALT_SCREEN_1049 && enabled) {
        winxterm_screen_save_cursor(screen);
    }

    if (!winxterm_mode_state_set(&screen->mode_state, mode, enabled)) {
        return;
    }

    screen->origin_mode = screen->mode_state.origin;
    screen->auto_wrap = screen->mode_state.auto_wrap;
    screen->cursor_visible = screen->mode_state.cursor_visible;

    if (mode == WINXTERM_TERMINAL_MODE_ORIGIN) {
        winxterm_screen_set_cursor_position(screen, 1, 1);
    } else if (mode == WINXTERM_TERMINAL_MODE_LEFT_RIGHT_MARGIN && !enabled) {
        screen->left_margin = 0;
        screen->right_margin = screen->columns - 1;
    } else if (winxterm_screen_alt_mode(mode)) {
        screen->alternate_active = winxterm_screen_any_alt_mode_enabled(screen);
        screen->pending_wrap = false;
        winxterm_screen_set_cursor_position(screen, 1, 1);
        if (enabled &&
            (mode == WINXTERM_TERMINAL_MODE_ALT_SCREEN_1047 ||
             mode == WINXTERM_TERMINAL_MODE_ALT_SCREEN_1049)) {
            winxterm_screen_fill_cells(screen,
                                       screen->alternate_cells,
                                       winxterm_screen_grid_count(screen->columns, screen->rows));
            if (screen->alternate_line_meta != 0) {
                memset(screen->alternate_line_meta,
                       0,
                       (size_t)screen->rows * sizeof(*screen->alternate_line_meta));
            }
        }
        if (mode == WINXTERM_TERMINAL_MODE_ALT_SCREEN_1049 && !enabled) {
            winxterm_screen_restore_cursor(screen);
        }
    }
    winxterm_screen_mark_full_repaint(screen);
}

static void winxterm_screen_save_mode(WinxtermScreen *screen, WinxtermTerminalMode mode)
{
    (void)winxterm_mode_state_set(&screen->saved_mode_state,
                                  mode,
                                  winxterm_mode_state_enabled(&screen->mode_state, mode));
}

static void winxterm_screen_restore_mode(WinxtermScreen *screen, WinxtermTerminalMode mode)
{
    winxterm_screen_set_mode(screen,
                             mode,
                             winxterm_mode_state_enabled(&screen->saved_mode_state, mode));
}

static void winxterm_screen_reset_soft(WinxtermScreen *screen)
{
    bool preserve_alt = screen->alternate_active;
    winxterm_mode_state_reset_soft(&screen->mode_state);
    screen->alternate_active = false;
    screen->origin_mode = screen->mode_state.origin;
    screen->auto_wrap = screen->mode_state.auto_wrap;
    screen->cursor_visible = screen->mode_state.cursor_visible;
    screen->scroll_top = 0;
    screen->scroll_bottom = screen->rows - 1;
    screen->left_margin = 0;
    screen->right_margin = screen->columns - 1;
    winxterm_screen_reset_attributes(screen);
    screen->protected_mode = false;
    screen->cursor_style = WINXTERM_TERMINAL_CURSOR_STYLE_DEFAULT;
    screen->g0_charset = WINXTERM_TERMINAL_CHARSET_ASCII;
    screen->pending_wrap = false;
    if (preserve_alt) {
        winxterm_screen_set_cursor_position(screen, 1, 1);
    }
    winxterm_screen_mark_full_repaint(screen);
}

static void winxterm_screen_reset_hard(WinxtermScreen *screen)
{
    if (screen == 0) {
        return;
    }
    winxterm_screen_dispose_scrollback(screen);
    winxterm_screen_fill_cells(screen,
                               screen->primary_cells,
                               winxterm_screen_grid_count(screen->columns, screen->rows));
    winxterm_screen_fill_cells(screen,
                               screen->alternate_cells,
                               winxterm_screen_grid_count(screen->columns, screen->rows));
    if (screen->primary_line_meta != 0) {
        memset(screen->primary_line_meta, 0, (size_t)screen->rows * sizeof(*screen->primary_line_meta));
    }
    if (screen->alternate_line_meta != 0) {
        memset(screen->alternate_line_meta, 0, (size_t)screen->rows * sizeof(*screen->alternate_line_meta));
    }
    screen->alternate_active = false;
    winxterm_screen_reset_palette(&screen->palette);
    winxterm_screen_reset_cursor_state(screen);
    winxterm_screen_mark_full_repaint(screen);
}

static void winxterm_screen_alignment_test(WinxtermScreen *screen)
{
    if (screen == 0) {
        return;
    }
    WinxtermScreenCell cell = winxterm_screen_blank_cell(screen);
    cell.codepoint = (uint32_t)'E';
    cell.glyph_index = winxterm_screen_map_codepoint_to_glyph((uint32_t)'E');
    WinxtermScreenCell *cells = winxterm_screen_active_cells(screen);
    size_t count = winxterm_screen_grid_count(screen->columns, screen->rows);
    for (size_t i = 0; i < count; ++i) {
        cells[i] = cell;
    }
    screen->cursor_row = 0;
    screen->cursor_col = 0;
    screen->pending_wrap = false;
    winxterm_screen_mark_full_repaint(screen);
}

static int winxterm_screen_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool winxterm_screen_parse_hex_byte(const char *text, size_t length, uint32_t *value)
{
    if (text == 0 || value == 0 || length == 0u) {
        return false;
    }
    uint32_t parsed = 0u;
    for (size_t i = 0u; i < length; ++i) {
        int digit = winxterm_screen_hex_value(text[i]);
        if (digit < 0) {
            return false;
        }
        parsed = (parsed << 4) | (uint32_t)digit;
    }
    if (length > 2u) {
        parsed >>= (unsigned)((length - 2u) * 4u);
    } else if (length == 1u) {
        parsed = (parsed << 4) | parsed;
    }
    *value = parsed & 0xffu;
    return true;
}

static bool winxterm_screen_parse_color_spec(const char *text, size_t length, uint32_t *rgb)
{
    if (text == 0 || rgb == 0 || length == 0u) {
        return false;
    }
    if (length == 7u && text[0] == '#') {
        uint32_t red = 0u;
        uint32_t green = 0u;
        uint32_t blue = 0u;
        if (!winxterm_screen_parse_hex_byte(text + 1, 2u, &red) ||
            !winxterm_screen_parse_hex_byte(text + 3, 2u, &green) ||
            !winxterm_screen_parse_hex_byte(text + 5, 2u, &blue)) {
            return false;
        }
        *rgb = (red << 16) | (green << 8) | blue;
        return true;
    }
    if (length > 4u && memcmp(text, "rgb:", 4u) == 0) {
        const char *start = text + 4;
        const char *end = text + length;
        const char *first_slash = 0;
        const char *second_slash = 0;
        for (const char *p = start; p < end; ++p) {
            if (*p == '/') {
                if (first_slash == 0) {
                    first_slash = p;
                } else {
                    second_slash = p;
                    break;
                }
            }
        }
        if (first_slash == 0 || second_slash == 0 || second_slash + 1 >= end) {
            return false;
        }
        uint32_t red = 0u;
        uint32_t green = 0u;
        uint32_t blue = 0u;
        if (!winxterm_screen_parse_hex_byte(start, (size_t)(first_slash - start), &red) ||
            !winxterm_screen_parse_hex_byte(first_slash + 1, (size_t)(second_slash - first_slash - 1), &green) ||
            !winxterm_screen_parse_hex_byte(second_slash + 1, (size_t)(end - second_slash - 1), &blue)) {
            return false;
        }
        *rgb = (red << 16) | (green << 8) | blue;
        return true;
    }
    return false;
}

static void winxterm_screen_apply_default_color_change(WinxtermScreen *screen, bool foreground, uint32_t rgb)
{
    if (foreground) {
        screen->palette.default_foreground_rgb = rgb & 0x00ffffffu;
        if ((screen->color_flags & WINXTERM_SCREEN_COLOR_FOREGROUND_DEFAULT) != 0u) {
            screen->foreground_rgb = screen->palette.default_foreground_rgb;
        }
    } else {
        screen->palette.default_background_rgb = rgb & 0x00ffffffu;
        if ((screen->color_flags & WINXTERM_SCREEN_COLOR_BACKGROUND_DEFAULT) != 0u) {
            screen->background_rgb = screen->palette.default_background_rgb;
        }
    }
}

static void winxterm_screen_apply_osc_palette(WinxtermScreen *screen, const WinxtermTerminalOp *op)
{
    if (screen == 0 || op == 0 || op->data.osc.payload_length == 0u) {
        return;
    }
    const char *payload = op->data.osc.payload;
    size_t length = op->data.osc.payload_length;
    if (payload[0] == '?') {
        return;
    }
    if (op->data.osc.command == WINXTERM_TERMINAL_OSC_FOREGROUND ||
        op->data.osc.command == WINXTERM_TERMINAL_OSC_BACKGROUND) {
        uint32_t rgb = 0u;
        if (winxterm_screen_parse_color_spec(payload, length, &rgb)) {
            winxterm_screen_apply_default_color_change(screen,
                                                       op->data.osc.command == WINXTERM_TERMINAL_OSC_FOREGROUND,
                                                       rgb);
        }
        return;
    }
    if (op->data.osc.command != WINXTERM_TERMINAL_OSC_PALETTE) {
        return;
    }
    size_t offset = 0u;
    while (offset < length) {
        int index = 0;
        bool saw_digit = false;
        while (offset < length && payload[offset] >= '0' && payload[offset] <= '9') {
            index = index * 10 + (int)(payload[offset] - '0');
            saw_digit = true;
            ++offset;
        }
        if (!saw_digit || offset >= length || payload[offset] != ';') {
            return;
        }
        ++offset;
        size_t color_start = offset;
        while (offset < length && payload[offset] != ';') {
            ++offset;
        }
        uint32_t rgb = 0u;
        if (index >= 0 && index < 256 &&
            winxterm_screen_parse_color_spec(payload + color_start, offset - color_start, &rgb)) {
            screen->palette.slots[index] = rgb & 0x00ffffffu;
            if ((screen->color_flags & WINXTERM_SCREEN_COLOR_FOREGROUND_INDEXED) != 0u &&
                screen->foreground_palette_index == (uint16_t)index) {
                screen->foreground_rgb = screen->palette.slots[index];
            }
            if ((screen->color_flags & WINXTERM_SCREEN_COLOR_BACKGROUND_INDEXED) != 0u &&
                screen->background_palette_index == (uint16_t)index) {
                screen->background_rgb = screen->palette.slots[index];
            }
        }
        if (offset < length && payload[offset] == ';') {
            ++offset;
        }
    }
    winxterm_screen_mark_full_repaint(screen);
}

bool winxterm_screen_apply_op(WinxtermScreen *screen, const WinxtermTerminalOp *op)
{
    if (screen == 0 || op == 0 || screen->columns <= 0 || screen->rows <= 0) {
        return false;
    }
    switch (op->type) {
    case WINXTERM_TERMINAL_OP_PRINT:
        return winxterm_screen_print_with_source(screen,
                                                 op->data.codepoint,
                                                 (uint64_t)op->source_byte_count);
    case WINXTERM_TERMINAL_OP_CONTROL:
        if (op->data.control == WINXTERM_TERMINAL_CONTROL_BS) {
            int left = winxterm_screen_has_horizontal_margins(screen) ? screen->left_margin : 0;
            if (screen->cursor_col == left &&
                winxterm_screen_mode_enabled(screen, WINXTERM_TERMINAL_MODE_REVERSE_WRAP) &&
                screen->cursor_row > 0) {
                --screen->cursor_row;
                screen->cursor_col = winxterm_screen_has_horizontal_margins(screen) ?
                    screen->right_margin : screen->columns - 1;
                screen->pending_wrap = false;
            } else {
                winxterm_screen_move_relative(screen, 0, -1);
            }
        } else if (op->data.control == WINXTERM_TERMINAL_CONTROL_TAB) {
            return winxterm_screen_tab(screen);
        } else if (op->data.control == WINXTERM_TERMINAL_CONTROL_LF) {
            return winxterm_screen_linefeed(screen);
        } else if (op->data.control == WINXTERM_TERMINAL_CONTROL_CR) {
            screen->cursor_col = 0;
            screen->pending_wrap = false;
        }
        return true;
    case WINXTERM_TERMINAL_OP_CURSOR_UP:
        winxterm_screen_move_relative(screen, -(op->data.count <= 0 ? 1 : op->data.count), 0);
        return true;
    case WINXTERM_TERMINAL_OP_CURSOR_DOWN:
        winxterm_screen_move_relative(screen, op->data.count <= 0 ? 1 : op->data.count, 0);
        return true;
    case WINXTERM_TERMINAL_OP_CURSOR_FORWARD:
        winxterm_screen_move_relative(screen, 0, op->data.count <= 0 ? 1 : op->data.count);
        return true;
    case WINXTERM_TERMINAL_OP_CURSOR_BACK:
        winxterm_screen_move_relative(screen, 0, -(op->data.count <= 0 ? 1 : op->data.count));
        return true;
    case WINXTERM_TERMINAL_OP_CURSOR_POSITION:
        winxterm_screen_set_cursor_position(screen, op->data.cursor.row, op->data.cursor.column);
        return true;
    case WINXTERM_TERMINAL_OP_ERASE_DISPLAY:
        winxterm_screen_erase_display(screen, op->data.erase.mode, false);
        return true;
    case WINXTERM_TERMINAL_OP_ERASE_LINE:
        winxterm_screen_erase_line(screen, op->data.erase.mode, false);
        return true;
    case WINXTERM_TERMINAL_OP_INSERT_CHARS:
        winxterm_screen_insert_chars(screen, op->data.count);
        return true;
    case WINXTERM_TERMINAL_OP_DELETE_CHARS:
        winxterm_screen_delete_chars(screen, op->data.count);
        return true;
    case WINXTERM_TERMINAL_OP_INSERT_LINES:
        winxterm_screen_insert_lines(screen, op->data.count);
        return true;
    case WINXTERM_TERMINAL_OP_DELETE_LINES:
        return winxterm_screen_delete_lines(screen, op->data.count);
    case WINXTERM_TERMINAL_OP_SCROLL_UP:
        return winxterm_screen_scroll_region_up(screen, op->data.count <= 0 ? 1 : op->data.count);
    case WINXTERM_TERMINAL_OP_SCROLL_DOWN:
        winxterm_screen_scroll_region_down(screen, op->data.count <= 0 ? 1 : op->data.count);
        return true;
    case WINXTERM_TERMINAL_OP_SET_SCROLL_REGION:
        if (op->data.scroll_region.top > 0 && op->data.scroll_region.bottom > op->data.scroll_region.top &&
            op->data.scroll_region.bottom <= screen->rows) {
            screen->scroll_top = op->data.scroll_region.top - 1;
            screen->scroll_bottom = op->data.scroll_region.bottom - 1;
        } else {
            screen->scroll_top = 0;
            screen->scroll_bottom = screen->rows - 1;
        }
        winxterm_screen_set_cursor_position(screen, 1, 1);
        return true;
    case WINXTERM_TERMINAL_OP_SAVE_CURSOR:
        winxterm_screen_save_cursor(screen);
        return true;
    case WINXTERM_TERMINAL_OP_RESTORE_CURSOR:
        winxterm_screen_restore_cursor(screen);
        return true;
    case WINXTERM_TERMINAL_OP_SET_MODE:
        winxterm_screen_set_mode(screen, op->data.mode.mode, true);
        return true;
    case WINXTERM_TERMINAL_OP_RESET_MODE:
        winxterm_screen_set_mode(screen, op->data.mode.mode, false);
        return true;
    case WINXTERM_TERMINAL_OP_SAVE_MODE:
        winxterm_screen_save_mode(screen, op->data.mode.mode);
        return true;
    case WINXTERM_TERMINAL_OP_RESTORE_MODE:
        winxterm_screen_restore_mode(screen, op->data.mode.mode);
        return true;
    case WINXTERM_TERMINAL_OP_RESET_ATTRIBUTES:
        winxterm_screen_reset_attributes(screen);
        return true;
    case WINXTERM_TERMINAL_OP_SET_FOREGROUND:
        winxterm_screen_set_foreground_rgb(screen, op->data.rgb);
        return true;
    case WINXTERM_TERMINAL_OP_SET_BACKGROUND:
        winxterm_screen_set_background_rgb(screen, op->data.rgb);
        return true;
    case WINXTERM_TERMINAL_OP_SET_FOREGROUND_INDEX:
        winxterm_screen_set_foreground_index(screen, op->data.count);
        return true;
    case WINXTERM_TERMINAL_OP_SET_BACKGROUND_INDEX:
        winxterm_screen_set_background_index(screen, op->data.count);
        return true;
    case WINXTERM_TERMINAL_OP_RESET_FOREGROUND:
        winxterm_screen_reset_foreground(screen);
        return true;
    case WINXTERM_TERMINAL_OP_RESET_BACKGROUND:
        winxterm_screen_reset_background(screen);
        return true;
    case WINXTERM_TERMINAL_OP_SET_TEXT_ATTRIBUTES:
        winxterm_screen_set_text_attributes(screen, op->data.flags);
        return true;
    case WINXTERM_TERMINAL_OP_RESET_TEXT_ATTRIBUTES:
        winxterm_screen_reset_text_attributes(screen, op->data.flags);
        return true;
    case WINXTERM_TERMINAL_OP_PUSH_ATTRIBUTES:
        winxterm_screen_push_attributes(screen);
        return true;
    case WINXTERM_TERMINAL_OP_POP_ATTRIBUTES:
        winxterm_screen_pop_attributes(screen);
        return true;
    case WINXTERM_TERMINAL_OP_RESIZE:
        winxterm_screen_resize(screen, op->data.resize.columns, op->data.resize.rows);
        return true;
    case WINXTERM_TERMINAL_OP_RESET:
        if (op->data.reset.kind == WINXTERM_TERMINAL_RESET_HARD) {
            winxterm_screen_reset_hard(screen);
        } else {
            winxterm_screen_reset_soft(screen);
        }
        return true;
    case WINXTERM_TERMINAL_OP_DECALN:
        winxterm_screen_alignment_test(screen);
        return true;
    case WINXTERM_TERMINAL_OP_CURSOR_HORIZONTAL_ABSOLUTE:
        winxterm_screen_cursor_horizontal_absolute(screen, op->data.count);
        return true;
    case WINXTERM_TERMINAL_OP_CURSOR_VERTICAL_ABSOLUTE:
        winxterm_screen_cursor_vertical_absolute(screen, op->data.count);
        return true;
    case WINXTERM_TERMINAL_OP_CURSOR_NEXT_LINE:
        winxterm_screen_cursor_next_line(screen, op->data.count);
        return true;
    case WINXTERM_TERMINAL_OP_CURSOR_PREVIOUS_LINE:
        winxterm_screen_cursor_previous_line(screen, op->data.count);
        return true;
    case WINXTERM_TERMINAL_OP_CURSOR_TAB_FORWARD:
        return winxterm_screen_tab_forward(screen, op->data.count);
    case WINXTERM_TERMINAL_OP_CURSOR_TAB_BACK:
        return winxterm_screen_tab_back(screen, op->data.count);
    case WINXTERM_TERMINAL_OP_TAB_SET:
        winxterm_screen_tab_set(screen);
        return true;
    case WINXTERM_TERMINAL_OP_TAB_CLEAR:
        winxterm_screen_tab_clear(screen, op->data.tab_clear.mode);
        return true;
    case WINXTERM_TERMINAL_OP_INDEX:
        return winxterm_screen_index(screen);
    case WINXTERM_TERMINAL_OP_NEXT_LINE:
        screen->cursor_col = 0;
        return winxterm_screen_index(screen);
    case WINXTERM_TERMINAL_OP_REVERSE_INDEX:
        winxterm_screen_reverse_index(screen);
        return true;
    case WINXTERM_TERMINAL_OP_ERASE_CHARS:
        winxterm_screen_erase_chars(screen, op->data.count);
        return true;
    case WINXTERM_TERMINAL_OP_REPEAT_CHAR:
        return winxterm_screen_repeat_char(screen, op->data.count);
    case WINXTERM_TERMINAL_OP_SET_HORIZONTAL_MARGINS:
        winxterm_screen_set_horizontal_margins(screen,
                                              op->data.horizontal_margins.left,
                                              op->data.horizontal_margins.right);
        return true;
    case WINXTERM_TERMINAL_OP_INSERT_COLUMNS:
        winxterm_screen_insert_columns(screen, op->data.count);
        return true;
    case WINXTERM_TERMINAL_OP_DELETE_COLUMNS:
        winxterm_screen_delete_columns(screen, op->data.count);
        return true;
    case WINXTERM_TERMINAL_OP_SCROLL_LEFT:
        winxterm_screen_scroll_left(screen, op->data.count);
        return true;
    case WINXTERM_TERMINAL_OP_SCROLL_RIGHT:
        winxterm_screen_scroll_right(screen, op->data.count);
        return true;
    case WINXTERM_TERMINAL_OP_SET_PROTECTED:
        screen->protected_mode = op->data.count != 0;
        return true;
    case WINXTERM_TERMINAL_OP_SELECTIVE_ERASE_DISPLAY:
        winxterm_screen_erase_display(screen, op->data.erase.mode, true);
        return true;
    case WINXTERM_TERMINAL_OP_SELECTIVE_ERASE_LINE:
        winxterm_screen_erase_line(screen, op->data.erase.mode, true);
        return true;
    case WINXTERM_TERMINAL_OP_RECT_ERASE:
        winxterm_screen_rect_erase(screen, op, op->private_mode);
        return true;
    case WINXTERM_TERMINAL_OP_RECT_FILL:
        winxterm_screen_rect_fill(screen, op);
        return true;
    case WINXTERM_TERMINAL_OP_RECT_COPY:
        winxterm_screen_rect_copy(screen, op);
        return true;
    case WINXTERM_TERMINAL_OP_RECT_ATTR:
        winxterm_screen_rect_attr(screen, op);
        return true;
    case WINXTERM_TERMINAL_OP_CURSOR_STYLE:
        screen->cursor_style = op->data.cursor_style.style;
        return true;
    case WINXTERM_TERMINAL_OP_SET_CHARSET:
        if (op->data.charset.slot == 0) {
            screen->g0_charset = op->data.charset.charset;
        }
        return true;
    case WINXTERM_TERMINAL_OP_TITLE:
    case WINXTERM_TERMINAL_OP_BELL:
    case WINXTERM_TERMINAL_OP_QUERY:
    case WINXTERM_TERMINAL_OP_REPLY_BYTES:
        return true;
    case WINXTERM_TERMINAL_OP_OSC:
        winxterm_screen_apply_osc_palette(screen, op);
        return true;
    default:
        return true;
    }
}

static bool winxterm_screen_append_reflowed_line(WinxtermScreen *screen,
                                                 WinxtermScreenLine **lines,
                                                 size_t *count,
                                                 size_t *capacity,
                                                 const WinxtermScreenCell *cells,
                                                 int cell_count,
                                                 bool soft_wrapped)
{
    if (*count == WINXTERM_SCREEN_SCROLLBACK_LINE_CAP) {
        winxterm_screen_line_dispose(&(*lines)[0]);
        memmove(&(*lines)[0], &(*lines)[1], (*count - 1u) * sizeof((*lines)[0]));
        --*count;
    }
    if (*count >= *capacity) {
        size_t new_capacity = *capacity == 0u ? 64u : *capacity * 2u;
        WinxtermScreenLine *new_lines =
            (WinxtermScreenLine *)realloc(*lines, new_capacity * sizeof(*new_lines));
        if (new_lines == 0) {
            return false;
        }
        for (size_t i = *capacity; i < new_capacity; ++i) {
            memset(&new_lines[i], 0, sizeof(new_lines[i]));
        }
        *lines = new_lines;
        *capacity = new_capacity;
    }

    WinxtermScreenLine *line = &(*lines)[*count];
    line->cells = (WinxtermScreenCell *)malloc((size_t)screen->columns * sizeof(*line->cells));
    if (line->cells == 0) {
        return false;
    }
    WinxtermScreenCell blank = winxterm_screen_blank_cell(screen);
    for (int column = 0; column < screen->columns; ++column) {
        line->cells[column] = blank;
    }
    if (cell_count > screen->columns) {
        cell_count = screen->columns;
    }
    if (cell_count > 0) {
        memcpy(line->cells, cells, (size_t)cell_count * sizeof(*line->cells));
    }
    line->columns = screen->columns;
    line->soft_wrapped = soft_wrapped;
    line->content_columns = soft_wrapped ? screen->columns : cell_count;
    ++*count;
    return true;
}

typedef struct WinxtermScreenReflowSource {
    const WinxtermScreenCell *cells;
    int columns;
    bool soft_wrapped;
    int content_columns;
} WinxtermScreenReflowSource;

static bool winxterm_screen_reflow_sources(WinxtermScreen *screen,
                                           const WinxtermScreenReflowSource *sources,
                                           size_t source_count,
                                           size_t cursor_source,
                                           int cursor_column,
                                           WinxtermScreenLine **new_lines,
                                           size_t *new_count,
                                           size_t *new_capacity,
                                           size_t *cursor_line,
                                           int *cursor_col)
{
    if (screen == 0 || sources == 0 || new_lines == 0 || new_count == 0 ||
        new_capacity == 0 || cursor_line == 0 || cursor_col == 0) {
        return false;
    }

    *new_lines = 0;
    *new_count = 0u;
    *new_capacity = 0u;
    *cursor_line = 0u;
    *cursor_col = 0;

    for (size_t i = 0u; i < source_count; ++i) {
        size_t start = i;
        size_t end = i;
        while (end + 1u < source_count && sources[end].soft_wrapped) {
            ++end;
        }

        int total = 0;
        for (size_t line_index = start; line_index <= end; ++line_index) {
            int columns = sources[line_index].columns;
            if (line_index == end) {
                columns = sources[line_index].content_columns;
            }
            total += columns;
        }

        if (total <= 0) {
            if (!winxterm_screen_append_reflowed_line(screen,
                                                      new_lines,
                                                      new_count,
                                                      new_capacity,
                                                      0,
                                                      0,
                                                      false)) {
                return false;
            }
            if (cursor_source >= start && cursor_source <= end) {
                *cursor_line = *new_count - 1u;
                *cursor_col = 0;
            }
            i = end;
            continue;
        }

        WinxtermScreenCell *logical = (WinxtermScreenCell *)malloc((size_t)total * sizeof(*logical));
        if (logical == 0) {
            return false;
        }
        int offset = 0;
        int cursor_offset = -1;
        for (size_t line_index = start; line_index <= end; ++line_index) {
            int columns = sources[line_index].columns;
            if (line_index == end) {
                columns = sources[line_index].content_columns;
            }
            if (line_index == cursor_source) {
                int column = cursor_column;
                if (column < 0) {
                    column = 0;
                }
                if (column > columns) {
                    column = columns;
                }
                cursor_offset = offset + column;
            }
            if (columns > 0) {
                memcpy(&logical[offset], sources[line_index].cells, (size_t)columns * sizeof(*logical));
                offset += columns;
            }
        }

        size_t group_first_new_line = *new_count;
        for (int logical_offset = 0; logical_offset < total; logical_offset += screen->columns) {
            int remaining = total - logical_offset;
            int take = remaining < screen->columns ? remaining : screen->columns;
            bool soft = logical_offset + take < total;
            if (!winxterm_screen_append_reflowed_line(screen,
                                                      new_lines,
                                                      new_count,
                                                      new_capacity,
                                                      &logical[logical_offset],
                                                      take,
                                                      soft)) {
                free(logical);
                return false;
            }
        }
        if (cursor_offset >= 0) {
            int line_delta = cursor_offset / screen->columns;
            int column = cursor_offset % screen->columns;
            size_t line = group_first_new_line + (size_t)line_delta;
            if (line >= *new_count) {
                line = *new_count - 1u;
                column = screen->columns - 1;
            }
            *cursor_line = line;
            *cursor_col = column;
        }
        free(logical);
        i = end;
    }
    return true;
}

static void winxterm_screen_map_reflow_anchor(const WinxtermScreenReflowSource *sources,
                                              size_t source_count,
                                              size_t source_index,
                                              int source_column,
                                              int new_columns,
                                              size_t *new_line,
                                              int *new_column)
{
    size_t output_line = 0u;
    for (size_t start = 0u; start < source_count;) {
        size_t end = start;
        while (end + 1u < source_count && sources[end].soft_wrapped) ++end;
        int total = 0;
        int anchor_offset = -1;
        for (size_t i = start; i <= end; ++i) {
            int columns = i == end ? sources[i].content_columns : sources[i].columns;
            if (i == source_index) {
                int column = source_column < 0 ? 0 : source_column;
                if (column > columns) column = columns;
                anchor_offset = total + column;
            }
            total += columns;
        }
        size_t emitted = total > 0 ? (size_t)((total + new_columns - 1) / new_columns) : 1u;
        if (anchor_offset >= 0) {
            *new_line = output_line + (size_t)(anchor_offset / new_columns);
            *new_column = anchor_offset % new_columns;
            return;
        }
        output_line += emitted;
        start = end + 1u;
    }
    *new_line = 0u;
    *new_column = 0;
}

static void winxterm_screen_reflow_primary_history(WinxtermScreen *screen,
                                                   int old_columns,
                                                   int old_rows,
                                                   const WinxtermScreenCell *old_primary,
                                                   const WinxtermScreenLineMeta *old_primary_meta)
{
    if (screen == 0 || old_columns <= 0 || old_rows <= 0 || old_primary == 0) {
        return;
    }

    size_t source_count = screen->scrollback_count + (size_t)old_rows;
    WinxtermScreenReflowSource *sources =
        (WinxtermScreenReflowSource *)calloc(source_count, sizeof(*sources));
    if (sources == 0) {
        return;
    }

    for (size_t i = 0u; i < screen->scrollback_count; ++i) {
        sources[i].cells = screen->scrollback_lines[i].cells;
        sources[i].columns = screen->scrollback_lines[i].columns;
        sources[i].soft_wrapped = screen->scrollback_lines[i].soft_wrapped;
        sources[i].content_columns = screen->scrollback_lines[i].content_columns;
    }
    for (int row = 0; row < old_rows; ++row) {
        size_t index = screen->scrollback_count + (size_t)row;
        sources[index].cells = &old_primary[(size_t)row * (size_t)old_columns];
        sources[index].columns = old_columns;
        sources[index].soft_wrapped = old_primary_meta != 0 ? old_primary_meta[row].soft_wrapped : false;
        sources[index].content_columns = old_primary_meta != 0 ? old_primary_meta[row].content_columns :
            winxterm_screen_row_content_columns(sources[index].cells, old_columns);
    }

    WinxtermScreenLine *new_lines = 0;
    size_t new_count = 0u;
    size_t new_capacity = 0u;
    size_t cursor_line = 0u;
    int cursor_col = 0;
    size_t cursor_source = screen->scrollback_count + (size_t)screen->cursor_row;
    size_t saved_cursor_source = screen->scrollback_count +
        (size_t)(screen->saved_primary_cursor.row < 0 ? 0 : screen->saved_primary_cursor.row);
    if (!winxterm_screen_reflow_sources(screen,
                                        sources,
                                        source_count,
                                        cursor_source,
                                        screen->cursor_col,
                                        &new_lines,
                                        &new_count,
                                        &new_capacity,
                                        &cursor_line,
                                        &cursor_col)) {
        free(sources);
        for (size_t i = 0u; i < new_count; ++i) {
            winxterm_screen_line_dispose(&new_lines[i]);
        }
        free(new_lines);
        return;
    }
    size_t saved_cursor_line = 0u;
    int saved_cursor_col = 0;
    winxterm_screen_map_reflow_anchor(sources,
                                      source_count,
                                      saved_cursor_source,
                                      screen->saved_primary_cursor.column,
                                      screen->columns,
                                      &saved_cursor_line,
                                      &saved_cursor_col);
    free(sources);

    winxterm_screen_dispose_scrollback(screen);
    size_t visible_count = new_count < (size_t)screen->rows ? new_count : (size_t)screen->rows;
    size_t scrollback_count = new_count > visible_count ? new_count - visible_count : 0u;
    size_t visible_start = scrollback_count;

    for (size_t row = 0u; row < visible_count; ++row) {
        WinxtermScreenLine *line = &new_lines[visible_start + row];
        int cell_count = line->columns < screen->columns ? line->columns : screen->columns;
        if (cell_count > 0) {
            memcpy(&screen->primary_cells[row * (size_t)screen->columns],
                   line->cells,
                   (size_t)cell_count * sizeof(*screen->primary_cells));
        }
        if (screen->primary_line_meta != 0) {
            screen->primary_line_meta[row].soft_wrapped = line->soft_wrapped;
            screen->primary_line_meta[row].content_columns = line->content_columns;
        }
    }
    for (size_t i = scrollback_count; i < new_count; ++i) {
        winxterm_screen_line_dispose(&new_lines[i]);
    }

    screen->scrollback_lines = new_lines;
    screen->scrollback_count = scrollback_count;
    screen->scrollback_capacity = new_capacity;
    if (screen->scrollback_capacity < screen->scrollback_count) {
        screen->scrollback_capacity = screen->scrollback_count;
    }

    if (cursor_line < scrollback_count) {
        screen->cursor_row = 0;
    } else {
        size_t visible_cursor_row = cursor_line - scrollback_count;
        screen->cursor_row = visible_cursor_row >= (size_t)screen->rows ?
            screen->rows - 1 : (int)visible_cursor_row;
    }
    screen->cursor_col = cursor_col;
    if (saved_cursor_line < scrollback_count) {
        screen->saved_primary_cursor.row = 0;
    } else {
        size_t saved_visible_row = saved_cursor_line - scrollback_count;
        screen->saved_primary_cursor.row = saved_visible_row >= (size_t)screen->rows ?
            screen->rows - 1 : (int)saved_visible_row;
    }
    screen->saved_primary_cursor.column = saved_cursor_col;
    winxterm_screen_clamp_cursor(screen);
}

void winxterm_screen_resize(WinxtermScreen *screen, int columns, int rows)
{
    if (screen == 0) {
        return;
    }
    columns = columns > 0 ? columns : 1;
    rows = rows > 0 ? rows : 1;
    if (columns == screen->columns && rows == screen->rows) {
        return;
    }

    size_t new_count = winxterm_screen_grid_count(columns, rows);
    WinxtermScreenCell *new_primary = (WinxtermScreenCell *)calloc(new_count, sizeof(*new_primary));
    WinxtermScreenCell *new_alternate = (WinxtermScreenCell *)calloc(new_count, sizeof(*new_alternate));
    WinxtermScreenLineMeta *new_primary_meta =
        (WinxtermScreenLineMeta *)calloc((size_t)rows, sizeof(*new_primary_meta));
    WinxtermScreenLineMeta *new_alternate_meta =
        (WinxtermScreenLineMeta *)calloc((size_t)rows, sizeof(*new_alternate_meta));
    bool *new_tabs = (bool *)calloc((size_t)columns, sizeof(*new_tabs));
    if (new_primary == 0 || new_alternate == 0 || new_primary_meta == 0 ||
        new_alternate_meta == 0 || new_tabs == 0) {
        free(new_primary);
        free(new_alternate);
        free(new_primary_meta);
        free(new_alternate_meta);
        free(new_tabs);
        return;
    }

    int old_columns = screen->columns;
    int old_rows = screen->rows;
    WinxtermScreenCell *old_primary = screen->primary_cells;
    WinxtermScreenCell *old_alternate = screen->alternate_cells;
    WinxtermScreenLineMeta *old_primary_meta = screen->primary_line_meta;
    WinxtermScreenLineMeta *old_alternate_meta = screen->alternate_line_meta;
    bool *old_tabs = screen->tab_stops;
    screen->columns = columns;
    screen->rows = rows;
    screen->primary_cells = new_primary;
    screen->alternate_cells = new_alternate;
    screen->primary_line_meta = new_primary_meta;
    screen->alternate_line_meta = new_alternate_meta;
    screen->tab_stops = new_tabs;
    winxterm_screen_fill_cells(screen, new_primary, new_count);
    winxterm_screen_fill_cells(screen, new_alternate, new_count);

    int copy_rows = old_rows < rows ? old_rows : rows;
    int copy_columns = old_columns < columns ? old_columns : columns;
    for (int row = 0; row < copy_rows; ++row) {
        memcpy(&new_alternate[(size_t)row * (size_t)columns],
               &old_alternate[(size_t)row * (size_t)old_columns],
               (size_t)copy_columns * sizeof(*new_alternate));
        if (old_alternate_meta != 0) {
            new_alternate_meta[row] = old_alternate_meta[row];
        }
    }
    for (int column = 0; column < columns; ++column) {
        new_tabs[column] = (column < old_columns && old_tabs != 0) ? old_tabs[column] : ((column % 8) == 0 && column != 0);
    }

    screen->scroll_top = 0;
    screen->scroll_bottom = rows - 1;
    screen->left_margin = 0;
    screen->right_margin = columns - 1;
    winxterm_screen_clamp_cursor(screen);
    winxterm_screen_reflow_primary_history(screen,
                                           old_columns,
                                           old_rows,
                                           old_primary,
                                           old_primary_meta);
    free(old_primary);
    free(old_alternate);
    free(old_primary_meta);
    free(old_alternate_meta);
    free(old_tabs);
    winxterm_screen_mark_full_repaint(screen);
}

bool winxterm_screen_append_codepoint(WinxtermScreen *screen, uint32_t codepoint)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = WINXTERM_TERMINAL_OP_PRINT;
    op.data.codepoint = codepoint;
    return winxterm_screen_apply_op(screen, &op);
}

bool winxterm_screen_newline(WinxtermScreen *screen)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = WINXTERM_TERMINAL_OP_CONTROL;
    op.data.control = WINXTERM_TERMINAL_CONTROL_LF;
    return winxterm_screen_apply_op(screen, &op);
}

void winxterm_screen_carriage_return(WinxtermScreen *screen)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = WINXTERM_TERMINAL_OP_CONTROL;
    op.data.control = WINXTERM_TERMINAL_CONTROL_CR;
    (void)winxterm_screen_apply_op(screen, &op);
}

void winxterm_screen_backspace(WinxtermScreen *screen)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = WINXTERM_TERMINAL_OP_CONTROL;
    op.data.control = WINXTERM_TERMINAL_CONTROL_BS;
    (void)winxterm_screen_apply_op(screen, &op);
}

bool winxterm_screen_tab(WinxtermScreen *screen)
{
    return winxterm_screen_tab_forward(screen, 1);
}

void winxterm_screen_clear_current_session(WinxtermScreen *screen)
{
    if (screen == 0) {
        return;
    }
    (void)winxterm_screen_start_session(screen);
}

void winxterm_screen_clear_line(WinxtermScreen *screen)
{
    if (screen == 0) {
        return;
    }
    winxterm_screen_clear_row(screen, screen->cursor_row);
    screen->cursor_col = 0;
}

void winxterm_screen_set_foreground_rgb(WinxtermScreen *screen, uint32_t foreground_rgb)
{
    if (screen != 0) {
        screen->foreground_rgb = foreground_rgb & 0x00ffffffu;
        screen->color_flags &= (uint8_t)(0xffu ^ (WINXTERM_SCREEN_COLOR_FOREGROUND_DEFAULT |
                                                  WINXTERM_SCREEN_COLOR_FOREGROUND_INDEXED));
    }
}

void winxterm_screen_set_background_rgb(WinxtermScreen *screen, uint32_t background_rgb)
{
    if (screen != 0) {
        screen->background_rgb = background_rgb & 0x00ffffffu;
        screen->color_flags &= (uint8_t)(0xffu ^ (WINXTERM_SCREEN_COLOR_BACKGROUND_DEFAULT |
                                                  WINXTERM_SCREEN_COLOR_BACKGROUND_INDEXED));
    }
}

void winxterm_screen_set_foreground_index(WinxtermScreen *screen, int palette_index)
{
    if (screen != 0) {
        if (palette_index < 0) {
            palette_index = 0;
        } else if (palette_index > 255) {
            palette_index = 255;
        }
        screen->foreground_palette_index = (uint16_t)palette_index;
        screen->foreground_rgb = screen->palette.slots[palette_index];
        screen->color_flags &= (uint8_t)(0xffu ^ WINXTERM_SCREEN_COLOR_FOREGROUND_DEFAULT);
        screen->color_flags |= WINXTERM_SCREEN_COLOR_FOREGROUND_INDEXED;
    }
}

void winxterm_screen_set_background_index(WinxtermScreen *screen, int palette_index)
{
    if (screen != 0) {
        if (palette_index < 0) {
            palette_index = 0;
        } else if (palette_index > 255) {
            palette_index = 255;
        }
        screen->background_palette_index = (uint16_t)palette_index;
        screen->background_rgb = screen->palette.slots[palette_index];
        screen->color_flags &= (uint8_t)(0xffu ^ WINXTERM_SCREEN_COLOR_BACKGROUND_DEFAULT);
        screen->color_flags |= WINXTERM_SCREEN_COLOR_BACKGROUND_INDEXED;
    }
}

void winxterm_screen_reset_foreground(WinxtermScreen *screen)
{
    if (screen != 0) {
        screen->foreground_rgb = screen->palette.default_foreground_rgb;
        screen->foreground_palette_index = 7u;
        screen->color_flags &= (uint8_t)(0xffu ^ WINXTERM_SCREEN_COLOR_FOREGROUND_INDEXED);
        screen->color_flags |= WINXTERM_SCREEN_COLOR_FOREGROUND_DEFAULT;
    }
}

void winxterm_screen_reset_background(WinxtermScreen *screen)
{
    if (screen != 0) {
        screen->background_rgb = screen->palette.default_background_rgb;
        screen->background_palette_index = 0u;
        screen->color_flags &= (uint8_t)(0xffu ^ WINXTERM_SCREEN_COLOR_BACKGROUND_INDEXED);
        screen->color_flags |= WINXTERM_SCREEN_COLOR_BACKGROUND_DEFAULT;
    }
}

void winxterm_screen_set_text_attributes(WinxtermScreen *screen, uint32_t flags)
{
    if (screen != 0) {
        screen->attribute_flags |= flags & WINXTERM_SCREEN_CELL_VISUAL_MASK;
    }
}

void winxterm_screen_reset_text_attributes(WinxtermScreen *screen, uint32_t flags)
{
    if (screen != 0) {
        screen->attribute_flags &= ~(flags & WINXTERM_SCREEN_CELL_VISUAL_MASK);
    }
}

void winxterm_screen_push_attributes(WinxtermScreen *screen)
{
    if (screen == 0 || screen->attribute_stack_count >= WINXTERM_SCREEN_ATTRIBUTE_STACK_CAPACITY) {
        return;
    }
    WinxtermScreenAttributeState *state = &screen->attribute_stack[screen->attribute_stack_count++];
    state->foreground_rgb = screen->foreground_rgb;
    state->background_rgb = screen->background_rgb;
    state->foreground_palette_index = screen->foreground_palette_index;
    state->background_palette_index = screen->background_palette_index;
    state->attribute_flags = screen->attribute_flags;
    state->color_flags = screen->color_flags;
}

void winxterm_screen_pop_attributes(WinxtermScreen *screen)
{
    if (screen == 0 || screen->attribute_stack_count == 0u) {
        return;
    }
    const WinxtermScreenAttributeState *state = &screen->attribute_stack[--screen->attribute_stack_count];
    screen->foreground_rgb = state->foreground_rgb;
    screen->background_rgb = state->background_rgb;
    screen->foreground_palette_index = state->foreground_palette_index;
    screen->background_palette_index = state->background_palette_index;
    screen->attribute_flags = state->attribute_flags & WINXTERM_SCREEN_CELL_VISUAL_MASK;
    screen->color_flags = state->color_flags;
}

void winxterm_screen_reset_attributes(WinxtermScreen *screen)
{
    if (screen != 0) {
        screen->foreground_rgb = screen->palette.default_foreground_rgb;
        screen->background_rgb = screen->palette.default_background_rgb;
        screen->foreground_palette_index = 7u;
        screen->background_palette_index = 0u;
        screen->attribute_flags = 0u;
        screen->color_flags = (uint8_t)(WINXTERM_SCREEN_COLOR_FOREGROUND_DEFAULT |
                                        WINXTERM_SCREEN_COLOR_BACKGROUND_DEFAULT);
    }
}

bool winxterm_screen_mode_enabled(const WinxtermScreen *screen, WinxtermTerminalMode mode)
{
    return screen != 0 && winxterm_mode_state_enabled(&screen->mode_state, mode);
}

size_t winxterm_screen_scrollback_count(const WinxtermScreen *screen)
{
    return screen != 0 ? screen->scrollback_count : 0u;
}

size_t winxterm_screen_primary_view_row_count(const WinxtermScreen *screen)
{
    if (screen == 0 || screen->rows <= 0) {
        return 0u;
    }
    return screen->scrollback_count + (size_t)screen->rows;
}

size_t winxterm_screen_default_primary_first_row_for_rows(const WinxtermScreen *screen,
                                                          int visible_rows,
                                                          size_t offset_from_bottom)
{
    if (screen == 0 || screen->rows <= 0 || visible_rows <= 0) {
        return 0u;
    }
    size_t total_rows = winxterm_screen_primary_view_row_count(screen);
    size_t view_rows = (size_t)visible_rows;
    if (view_rows > total_rows) {
        view_rows = total_rows;
    }
    size_t bottom_first = total_rows > view_rows ? total_rows - view_rows : 0u;
    if (offset_from_bottom > bottom_first) {
        offset_from_bottom = bottom_first;
    }
    return bottom_first - offset_from_bottom;
}

size_t winxterm_screen_default_primary_first_row(const WinxtermScreen *screen, size_t offset_from_bottom)
{
    return winxterm_screen_default_primary_first_row_for_rows(screen,
                                                             screen != 0 ? screen->rows : 0,
                                                             offset_from_bottom);
}

bool winxterm_screen_get_primary_view_row(const WinxtermScreen *screen,
                                          size_t global_row,
                                          WinxtermScreenRowView *row)
{
    if (screen == 0 || row == 0) {
        return false;
    }
    memset(row, 0, sizeof(*row));
    if (global_row < screen->scrollback_count) {
        const WinxtermScreenLine *line = &screen->scrollback_lines[global_row];
        row->cells = line->cells;
        row->columns = line->columns;
        row->soft_wrapped = line->soft_wrapped;
        row->content_columns = line->content_columns;
        return row->cells != 0 && row->columns > 0;
    }
    size_t visible_row = global_row - screen->scrollback_count;
    if (visible_row >= (size_t)screen->rows || screen->primary_cells == 0) {
        return false;
    }
    row->cells = &screen->primary_cells[visible_row * (size_t)screen->columns];
    row->columns = screen->columns;
    row->soft_wrapped = screen->primary_line_meta != 0 ?
        screen->primary_line_meta[visible_row].soft_wrapped : false;
    row->content_columns = screen->primary_line_meta != 0 ?
        screen->primary_line_meta[visible_row].content_columns :
        winxterm_screen_row_content_columns(row->cells, row->columns);
    return true;
}

static bool winxterm_screen_primary_group_end(const WinxtermScreen *screen,
                                              size_t start,
                                              size_t total_rows,
                                              size_t *end)
{
    if (screen == 0 || end == 0 || start >= total_rows) {
        return false;
    }

    size_t row_index = start;
    for (;;) {
        WinxtermScreenRowView row;
        if (!winxterm_screen_get_primary_view_row(screen, row_index, &row)) {
            return false;
        }
        if (!row.soft_wrapped || row_index + 1u >= total_rows) {
            *end = row_index;
            return true;
        }
        ++row_index;
    }
}

static int winxterm_screen_primary_row_content_columns(const WinxtermScreen *screen,
                                                       size_t row_index,
                                                       size_t group_end)
{
    WinxtermScreenRowView row;
    if (!winxterm_screen_get_primary_view_row(screen, row_index, &row)) {
        return 0;
    }
    return row_index == group_end ? row.content_columns : row.columns;
}

static int winxterm_screen_primary_anchor_column(const WinxtermScreenRowView *row,
                                                 int content_columns)
{
    if (row == 0 || row->cells == 0 || row->columns <= 0) {
        return 0;
    }

    int search_columns = content_columns;
    if (search_columns <= 0 || search_columns > row->columns) {
        search_columns = row->columns;
    }
    for (int column = search_columns - 1; column >= 0; --column) {
        const WinxtermScreenCell *cell = &row->cells[column];
        if (!cell->continuation && cell->codepoint != (uint32_t)' ') {
            return column;
        }
    }
    return content_columns > 0 ? content_columns - 1 : 0;
}

bool winxterm_screen_primary_anchor_from_global_row(const WinxtermScreen *screen,
                                                    size_t global_row,
                                                    WinxtermScreenPrimaryAnchor *anchor)
{
    if (screen == 0 || anchor == 0) {
        return false;
    }
    size_t total_rows = winxterm_screen_primary_view_row_count(screen);
    if (global_row >= total_rows) {
        return false;
    }

    size_t logical_line = 0u;
    for (size_t start = 0u; start < total_rows; ++logical_line) {
        size_t end = start;
        if (!winxterm_screen_primary_group_end(screen, start, total_rows, &end)) {
            return false;
        }
        if (global_row >= start && global_row <= end) {
            size_t cell_offset = 0u;
            for (size_t row_index = start; row_index < global_row; ++row_index) {
                int columns = winxterm_screen_primary_row_content_columns(screen, row_index, end);
                if (columns > 0) {
                    cell_offset += (size_t)columns;
                }
            }

            WinxtermScreenRowView row;
            if (!winxterm_screen_get_primary_view_row(screen, global_row, &row)) {
                return false;
            }
            int content_columns = winxterm_screen_primary_row_content_columns(screen, global_row, end);
            int column = winxterm_screen_primary_anchor_column(&row, content_columns);
            if (content_columns > 0 && column >= content_columns) {
                column = content_columns - 1;
            }
            if (column > 0) {
                cell_offset += (size_t)column;
            }

            anchor->logical_line = logical_line;
            anchor->cell_offset = cell_offset;
            return true;
        }
        start = end + 1u;
    }
    return false;
}

bool winxterm_screen_primary_global_row_from_anchor(const WinxtermScreen *screen,
                                                    const WinxtermScreenPrimaryAnchor *anchor,
                                                    size_t *global_row)
{
    if (screen == 0 || anchor == 0 || global_row == 0) {
        return false;
    }
    size_t total_rows = winxterm_screen_primary_view_row_count(screen);
    size_t logical_line = 0u;
    for (size_t start = 0u; start < total_rows; ++logical_line) {
        size_t end = start;
        if (!winxterm_screen_primary_group_end(screen, start, total_rows, &end)) {
            return false;
        }
        if (logical_line == anchor->logical_line) {
            size_t offset = 0u;
            for (size_t row_index = start; row_index <= end; ++row_index) {
                int columns = winxterm_screen_primary_row_content_columns(screen, row_index, end);
                if (columns <= 0) {
                    *global_row = row_index;
                    return true;
                }
                size_t next_offset = offset + (size_t)columns;
                if (anchor->cell_offset < next_offset || row_index == end) {
                    *global_row = row_index;
                    return true;
                }
                offset = next_offset;
            }
        }
        start = end + 1u;
    }
    return false;
}

bool winxterm_screen_get_alternate_view_row(const WinxtermScreen *screen,
                                            size_t row_index,
                                            WinxtermScreenRowView *row)
{
    if (screen == 0 || row == 0 || row_index >= (size_t)screen->rows || screen->alternate_cells == 0) {
        return false;
    }
    row->cells = &screen->alternate_cells[row_index * (size_t)screen->columns];
    row->columns = screen->columns;
    row->soft_wrapped = screen->alternate_line_meta != 0 ?
        screen->alternate_line_meta[row_index].soft_wrapped : false;
    row->content_columns = screen->alternate_line_meta != 0 ?
        screen->alternate_line_meta[row_index].content_columns :
        winxterm_screen_row_content_columns(row->cells, row->columns);
    return true;
}

uint64_t winxterm_screen_visual_line_advances(const WinxtermScreen *screen)
{
    return screen != 0 ? screen->visual_line_advances : 0u;
}

void winxterm_screen_clear_scrollback(WinxtermScreen *screen)
{
    winxterm_screen_dispose_scrollback(screen);
}

static uint32_t winxterm_screen_scale_rgb(uint32_t rgb, int percent)
{
    uint32_t red = (rgb >> 16) & 0xffu;
    uint32_t green = (rgb >> 8) & 0xffu;
    uint32_t blue = rgb & 0xffu;
    red = (red * (uint32_t)percent) / 100u;
    green = (green * (uint32_t)percent) / 100u;
    blue = (blue * (uint32_t)percent) / 100u;
    if (red > 255u) red = 255u;
    if (green > 255u) green = 255u;
    if (blue > 255u) blue = 255u;
    return (red << 16) | (green << 8) | blue;
}

static uint32_t winxterm_screen_cell_foreground_rgb(const WinxtermScreen *screen, const WinxtermScreenCell *cell)
{
    if ((cell->color_flags & WINXTERM_SCREEN_COLOR_FOREGROUND_DEFAULT) != 0u) {
        return screen->palette.default_foreground_rgb;
    }
    if ((cell->color_flags & WINXTERM_SCREEN_COLOR_FOREGROUND_INDEXED) != 0u) {
        return screen->palette.slots[cell->foreground_palette_index & 0xffu];
    }
    return cell->foreground_rgb;
}

static uint32_t winxterm_screen_cell_background_rgb(const WinxtermScreen *screen, const WinxtermScreenCell *cell)
{
    if ((cell->color_flags & WINXTERM_SCREEN_COLOR_BACKGROUND_DEFAULT) != 0u) {
        return screen->palette.default_background_rgb;
    }
    if ((cell->color_flags & WINXTERM_SCREEN_COLOR_BACKGROUND_INDEXED) != 0u) {
        return screen->palette.slots[cell->background_palette_index & 0xffu];
    }
    return cell->background_rgb;
}

static void winxterm_screen_resolve_visual_colors(const WinxtermScreen *screen,
                                                  const WinxtermScreenCell *cell,
                                                  uint32_t *foreground,
                                                  uint32_t *background)
{
    uint32_t fg = winxterm_screen_cell_foreground_rgb(screen, cell);
    uint32_t bg = winxterm_screen_cell_background_rgb(screen, cell);
    if ((cell->attribute_flags & WINXTERM_SCREEN_CELL_BOLD) != 0u) {
        fg = winxterm_screen_scale_rgb(fg, 135);
    }
    if ((cell->attribute_flags & WINXTERM_SCREEN_CELL_FAINT) != 0u) {
        fg = winxterm_screen_scale_rgb(fg, 65);
    }
    if (((cell->attribute_flags & WINXTERM_SCREEN_CELL_INVERSE) != 0u) != screen->mode_state.reverse_video) {
        uint32_t tmp = fg;
        fg = bg;
        bg = tmp;
    }
    if ((cell->attribute_flags & WINXTERM_SCREEN_CELL_INVISIBLE) != 0u) {
        fg = bg;
    }
    *foreground = fg;
    *background = bg;
}

static void winxterm_screen_draw_decoration(uint32_t *pixels,
                                            int bitmap_width,
                                            int bitmap_height,
                                            int column,
                                            int row,
                                            int y_offset,
                                            uint32_t rgb)
{
    winxterm_render_clear_rect(pixels,
                               bitmap_width,
                               bitmap_height,
                               column * WINXTERM_CELL_WIDTH_PIXELS,
                               row * WINXTERM_CELL_HEIGHT_PIXELS + y_offset,
                               WINXTERM_CELL_WIDTH_PIXELS,
                               1,
                               rgb);
}

static void winxterm_screen_draw_cell(WinxtermScreen *screen,
                                      WinxtermRenderContext *render_context,
                                      uint32_t *pixels,
                                      int bitmap_width,
                                      int bitmap_height,
                                      int column,
                                      int row,
                                      const WinxtermScreenCell *cell,
                                      WinxtermRenderBackend backend,
                                      bool selected)
{
    uint32_t foreground = 0u;
    uint32_t background = 0u;
    winxterm_screen_resolve_visual_colors(screen, cell, &foreground, &background);
    if (selected) {
        uint32_t tmp = foreground;
        foreground = background;
        background = tmp;
        if (foreground == background) {
            foreground = screen->palette.default_background_rgb;
            background = screen->palette.default_foreground_rgb;
        }
    }
    if (cell->continuation) {
        if (screen->active_diagnostics != 0) {
            winxterm_diag_inc_u64(&screen->active_diagnostics->continuation_cell_skips);
        }
        if (background != (screen->mode_state.reverse_video ?
                              screen->palette.default_foreground_rgb :
                              screen->palette.default_background_rgb)) {
            winxterm_render_clear_rect(pixels,
                                       bitmap_width,
                                       bitmap_height,
                                       column * WINXTERM_CELL_WIDTH_PIXELS,
                                       row * WINXTERM_CELL_HEIGHT_PIXELS,
                                       WINXTERM_CELL_WIDTH_PIXELS,
                                       WINXTERM_CELL_HEIGHT_PIXELS,
                                       background);
        }
        return;
    }
    bool draws_glyph = cell->codepoint != (uint32_t)' ' ||
                       background != (screen->mode_state.reverse_video ?
                                         screen->palette.default_foreground_rgb :
                                         screen->palette.default_background_rgb);
    if (draws_glyph) {
        winxterm_render_draw_cell_glyph(render_context,
                                        pixels,
                                        bitmap_width,
                                        bitmap_height,
                                        column,
                                        row,
                                        cell->glyph_index,
                                        cell->codepoint,
                                        cell->combining_codepoints,
                                        cell->combining_count,
                                        cell->width,
                                        foreground,
                                        background,
                                        backend);
    } else if (screen->active_diagnostics != 0) {
        winxterm_diag_inc_u64(&screen->active_diagnostics->empty_cell_skips);
    }
    if ((cell->attribute_flags & WINXTERM_SCREEN_CELL_UNDERLINE) != 0u) {
        winxterm_screen_draw_decoration(pixels, bitmap_width, bitmap_height, column, row, 11, foreground);
    }
    if ((cell->attribute_flags & WINXTERM_SCREEN_CELL_DOUBLE_UNDERLINE) != 0u) {
        winxterm_screen_draw_decoration(pixels, bitmap_width, bitmap_height, column, row, 9, foreground);
        winxterm_screen_draw_decoration(pixels, bitmap_width, bitmap_height, column, row, 11, foreground);
    }
    if ((cell->attribute_flags & WINXTERM_SCREEN_CELL_CROSSED_OUT) != 0u) {
        winxterm_screen_draw_decoration(pixels, bitmap_width, bitmap_height, column, row, 6, foreground);
    }
    if ((cell->attribute_flags & WINXTERM_SCREEN_CELL_OVERLINE) != 0u) {
        winxterm_screen_draw_decoration(pixels, bitmap_width, bitmap_height, column, row, 1, foreground);
    }
}

static bool winxterm_screen_selection_contains(const WinxtermScreenSelectionRange *selection,
                                               bool alternate,
                                               size_t row,
                                               int column)
{
    if (selection == 0 || !selection->enabled || selection->alternate != alternate ||
        row < selection->start_row || row > selection->end_row) {
        return false;
    }
    if (selection->rectangular) {
        return column >= selection->start_column && column <= selection->end_column;
    }
    int start_column = row == selection->start_row ? selection->start_column : 0;
    int end_column = row == selection->end_row ? selection->end_column : INT_MAX;
    return column >= start_column && column <= end_column;
}

static void winxterm_screen_snapshot_visual_screen(const WinxtermScreenRenderSnapshot *snapshot,
                                                   WinxtermScreen *visual)
{
    memset(visual, 0, sizeof(*visual));
    visual->columns = snapshot != 0 ? snapshot->columns : 0;
    visual->rows = snapshot != 0 ? snapshot->rows : 0;
    if (snapshot != 0) {
        visual->palette = snapshot->palette;
        visual->mode_state = snapshot->mode_state;
        visual->alternate_active = snapshot->alternate_active;
    }
}

bool winxterm_screen_render_snapshot_init(WinxtermScreenRenderSnapshot *snapshot,
                                          WinxtermScreen *screen,
                                          int bitmap_width,
                                          int bitmap_height,
                                          WinxtermRenderBackend backend,
                                          bool cursor_visible,
                                          const WinxtermScreenRenderView *view)
{
    return winxterm_screen_render_snapshot_init_rows(snapshot,
                                                    screen,
                                                    bitmap_width,
                                                    bitmap_height,
                                                    backend,
                                                    cursor_visible,
                                                    view,
                                                    0,
                                                    INT_MAX);
}

bool winxterm_screen_render_snapshot_init_rows(WinxtermScreenRenderSnapshot *snapshot,
                                               WinxtermScreen *screen,
                                               int bitmap_width,
                                               int bitmap_height,
                                               WinxtermRenderBackend backend,
                                               bool cursor_visible,
                                               const WinxtermScreenRenderView *view,
                                               int row_offset,
                                               int row_count)
{
    if (snapshot == 0) {
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (screen == 0 || bitmap_width <= 0 || bitmap_height <= 0) {
        return false;
    }

    WinxtermCellSize pixel_cells = winxterm_pixels_to_cells(bitmap_width, bitmap_height);
    int columns = pixel_cells.columns < screen->columns ? pixel_cells.columns : screen->columns;
    int visible_rows = pixel_cells.rows < screen->rows ? pixel_cells.rows : screen->rows;
    if (row_offset < 0) {
        row_count += row_offset;
        row_offset = 0;
    }
    if (row_offset > visible_rows) {
        row_offset = visible_rows;
    }
    int rows = visible_rows - row_offset;
    if (row_count < rows) {
        rows = row_count;
    }
    if (rows < 0) {
        rows = 0;
    }
    snapshot->bitmap_width = bitmap_width;
    snapshot->bitmap_height = bitmap_height;
    snapshot->row_offset = row_offset;
    snapshot->backend = backend;
    snapshot->cursor_visible = cursor_visible;
    snapshot->screen_cursor_visible = screen->cursor_visible;
    snapshot->cursor_col = screen->cursor_col;
    snapshot->alternate_active = screen->alternate_active;
    snapshot->palette = screen->palette;
    snapshot->mode_state = screen->mode_state;
    snapshot->clear_rgb = screen->mode_state.reverse_video ?
        screen->palette.default_foreground_rgb : screen->palette.default_background_rgb;
    snapshot->cursor_global_row = screen->alternate_active ?
        (size_t)screen->cursor_row : screen->scrollback_count + (size_t)screen->cursor_row;

    WinxtermScreenRenderView default_view;
    if (view == 0) {
        memset(&default_view, 0, sizeof(default_view));
        default_view.primary_first_row =
            winxterm_screen_default_primary_first_row_for_rows(screen, visible_rows, 0u);
        view = &default_view;
    }
    snapshot->first_row = screen->alternate_active ? 0u : view->primary_first_row;
    snapshot->selection = view->selection;

    if (columns <= 0 || rows <= 0) {
        return true;
    }

    size_t cell_count = (size_t)columns * (size_t)rows;
    WinxtermScreenCell *cells = (WinxtermScreenCell *)malloc(cell_count * sizeof(*cells));
    if (cells == 0) {
        return false;
    }

    WinxtermScreenCell blank = winxterm_screen_blank_cell(screen);
    for (size_t i = 0u; i < cell_count; ++i) {
        cells[i] = blank;
    }

    for (int row = 0; row < rows; ++row) {
        WinxtermScreenRowView row_view;
        int visible_row = row_offset + row;
        bool row_ok = screen->alternate_active ?
            winxterm_screen_get_alternate_view_row(screen, (size_t)visible_row, &row_view) :
            winxterm_screen_get_primary_view_row(screen, view->primary_first_row + (size_t)visible_row, &row_view);
        if (!row_ok || row_view.cells == 0) {
            continue;
        }
        int row_columns = row_view.columns < columns ? row_view.columns : columns;
        memcpy(cells + (size_t)row * (size_t)columns,
               row_view.cells,
               (size_t)row_columns * sizeof(*cells));
    }

    snapshot->cells = cells;
    snapshot->columns = columns;
    snapshot->rows = rows;
    return true;
}

void winxterm_screen_render_snapshot_dispose(WinxtermScreenRenderSnapshot *snapshot)
{
    if (snapshot == 0) {
        return;
    }
    free(snapshot->cells);
    memset(snapshot, 0, sizeof(*snapshot));
}

void winxterm_screen_render_snapshot_range(const WinxtermScreenRenderSnapshot *snapshot,
                                           WinxtermRenderContext *render_context,
                                           uint32_t *pixels,
                                           int row_start,
                                           int row_count)
{
    if (snapshot == 0 || pixels == 0 || snapshot->bitmap_width <= 0 || snapshot->bitmap_height <= 0) {
        return;
    }

    if (snapshot->columns <= 0 || snapshot->rows <= 0 || snapshot->cells == 0) {
        if (row_start <= 0) {
            winxterm_render_clear(pixels, snapshot->bitmap_width, snapshot->bitmap_height, snapshot->clear_rgb);
        }
        return;
    }

    if (row_start < 0) {
        row_count += row_start;
        row_start = 0;
    }
    if (row_count <= 0 || row_start >= snapshot->rows) {
        return;
    }
    if (row_start + row_count > snapshot->rows) {
        row_count = snapshot->rows - row_start;
    }

    int y = (snapshot->row_offset + row_start) * WINXTERM_CELL_HEIGHT_PIXELS;
    int height = row_count * WINXTERM_CELL_HEIGHT_PIXELS;
    winxterm_render_clear_rect(pixels,
                               snapshot->bitmap_width,
                               snapshot->bitmap_height,
                               0,
                               y,
                               snapshot->bitmap_width,
                               height,
                               snapshot->clear_rgb);
    if (row_start + row_count >= snapshot->rows) {
        int cleared_bottom = (snapshot->row_offset + snapshot->rows) * WINXTERM_CELL_HEIGHT_PIXELS;
        if (cleared_bottom < snapshot->bitmap_height) {
            winxterm_render_clear_rect(pixels,
                                       snapshot->bitmap_width,
                                       snapshot->bitmap_height,
                                       0,
                                       cleared_bottom,
                                       snapshot->bitmap_width,
                                       snapshot->bitmap_height - cleared_bottom,
                                       snapshot->clear_rgb);
        }
    }

    WinxtermScreen visual;
    winxterm_screen_snapshot_visual_screen(snapshot, &visual);
    for (int row = row_start; row < row_start + row_count; ++row) {
        int visible_row = snapshot->row_offset + row;
        size_t selection_row = snapshot->alternate_active ?
            (size_t)visible_row : snapshot->first_row + (size_t)visible_row;
        for (int column = 0; column < snapshot->columns; ++column) {
            const WinxtermScreenCell *cell =
                snapshot->cells + (size_t)row * (size_t)snapshot->columns + (size_t)column;
            winxterm_screen_draw_cell(&visual,
                                      render_context,
                                      pixels,
                                      snapshot->bitmap_width,
                                      snapshot->bitmap_height,
                                      column,
                                      visible_row,
                                      cell,
                                      snapshot->backend,
                                      winxterm_screen_selection_contains(&snapshot->selection,
                                                                         snapshot->alternate_active,
                                                                         selection_row,
                                                                         column));
        }
    }

    if (snapshot->cursor_visible &&
        snapshot->screen_cursor_visible &&
        snapshot->cursor_global_row >= snapshot->first_row + (size_t)snapshot->row_offset &&
        snapshot->cursor_global_row < snapshot->first_row + (size_t)snapshot->row_offset + (size_t)snapshot->rows &&
        snapshot->cursor_col >= 0 &&
        snapshot->cursor_col < snapshot->columns) {
        int cursor_row = (int)(snapshot->cursor_global_row - snapshot->first_row);
        int local_cursor_row = cursor_row - snapshot->row_offset;
        if (local_cursor_row >= row_start && local_cursor_row < row_start + row_count) {
            const WinxtermScreenCell *cell =
                snapshot->cells + (size_t)local_cursor_row * (size_t)snapshot->columns +
                (size_t)snapshot->cursor_col;
            uint32_t cursor_rgb = 0u;
            uint32_t text_rgb = 0u;
            winxterm_screen_resolve_visual_colors(&visual, cell, &cursor_rgb, &text_rgb);
            winxterm_render_clear_rect(pixels,
                                       snapshot->bitmap_width,
                                       snapshot->bitmap_height,
                                       snapshot->cursor_col * WINXTERM_CELL_WIDTH_PIXELS,
                                       cursor_row * WINXTERM_CELL_HEIGHT_PIXELS,
                                       WINXTERM_CELL_WIDTH_PIXELS,
                                       WINXTERM_CELL_HEIGHT_PIXELS,
                                       cursor_rgb);
            if (cell->codepoint != (uint32_t)' ' && !cell->continuation) {
                winxterm_render_draw_cell_glyph(render_context,
                                                pixels,
                                                snapshot->bitmap_width,
                                                snapshot->bitmap_height,
                                                snapshot->cursor_col,
                                                cursor_row,
                                                cell->glyph_index,
                                                cell->codepoint,
                                                cell->combining_codepoints,
                                                cell->combining_count,
                                                cell->width,
                                                text_rgb,
                                                cursor_rgb,
                                                snapshot->backend);
            }
        }
    }
}

void winxterm_screen_render_snapshot(const WinxtermScreenRenderSnapshot *snapshot,
                                     WinxtermRenderContext *render_context,
                                     uint32_t *pixels)
{
    if (snapshot == 0 || pixels == 0) {
        return;
    }
    if (snapshot->columns <= 0 || snapshot->rows <= 0 || snapshot->cells == 0) {
        winxterm_render_clear(pixels, snapshot->bitmap_width, snapshot->bitmap_height, snapshot->clear_rgb);
        return;
    }
    winxterm_screen_render_snapshot_range(snapshot, render_context, pixels, 0, snapshot->rows);
}

void winxterm_screen_render(WinxtermScreen *screen,
                            WinxtermRenderContext *render_context,
                            uint32_t *pixels,
                            int bitmap_width,
                            int bitmap_height,
                            WinxtermRenderBackend backend,
                            bool cursor_visible)
{
    winxterm_screen_render_view(screen,
                                render_context,
                                pixels,
                                bitmap_width,
                                bitmap_height,
                                backend,
                                cursor_visible,
                                0);
}

void winxterm_screen_render_view(WinxtermScreen *screen,
                                 WinxtermRenderContext *render_context,
                                 uint32_t *pixels,
                                 int bitmap_width,
                                 int bitmap_height,
                                 WinxtermRenderBackend backend,
                                 bool cursor_visible,
                                 const WinxtermScreenRenderView *view)
{
    if (screen == 0 || pixels == 0 || bitmap_width <= 0 || bitmap_height <= 0) {
        return;
    }

    WinxtermScreenRenderSnapshot snapshot;
    if (winxterm_screen_render_snapshot_init(&snapshot,
                                             screen,
                                             bitmap_width,
                                             bitmap_height,
                                             backend,
                                             cursor_visible,
                                             view)) {
        winxterm_screen_render_snapshot(&snapshot, render_context, pixels);
        winxterm_screen_render_snapshot_dispose(&snapshot);
    }
}
