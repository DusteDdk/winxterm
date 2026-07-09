#include "winxterm_ux.h"

#include "winxterm_render.h"
#include "winxterm_scale.h"

#include <stdlib.h>
#include <string.h>

typedef struct WinxtermUxBuffer {
    char *data;
    size_t length;
    size_t capacity;
} WinxtermUxBuffer;

void winxterm_ux_init(WinxtermUxState *ux)
{
    if (ux == 0) {
        return;
    }
    memset(ux, 0, sizeof(*ux));
    ux->viewport.follow_output = true;
}

static int winxterm_ux_clamped_visible_rows(const WinxtermScreen *screen, int visible_rows)
{
    if (screen == 0 || screen->rows <= 0 || visible_rows <= 0) {
        return 0;
    }
    return visible_rows < screen->rows ? visible_rows : screen->rows;
}

static int winxterm_ux_clamped_visible_columns(const WinxtermScreen *screen, int visible_columns)
{
    if (screen == 0 || screen->columns <= 0 || visible_columns <= 0) {
        return 0;
    }
    return visible_columns < screen->columns ? visible_columns : screen->columns;
}

static size_t winxterm_ux_max_offset_for_rows(const WinxtermScreen *screen, int visible_rows)
{
    if (screen == 0 || screen->alternate_active) {
        return 0u;
    }
    int view_rows = winxterm_ux_clamped_visible_rows(screen, visible_rows);
    if (view_rows <= 0) {
        return 0u;
    }
    size_t total_rows = winxterm_screen_primary_view_row_count(screen);
    return total_rows > (size_t)view_rows ? total_rows - (size_t)view_rows : 0u;
}

void winxterm_ux_clamp_viewport_for_rows(WinxtermUxState *ux,
                                         const WinxtermScreen *screen,
                                         int visible_rows)
{
    if (ux == 0) {
        return;
    }
    size_t max_offset = winxterm_ux_max_offset_for_rows(screen, visible_rows);
    if (ux->viewport.line_offset_from_bottom > max_offset) {
        ux->viewport.line_offset_from_bottom = max_offset;
    }
    if (ux->viewport.line_offset_from_bottom == 0u) {
        ux->viewport.follow_output = true;
        ux->viewport.bottom_anchor_valid = false;
    }
    if (screen == 0 || screen->alternate_active) {
        ux->viewport.bottom_anchor_valid = false;
    }
}

void winxterm_ux_clamp_viewport(WinxtermUxState *ux, const WinxtermScreen *screen)
{
    winxterm_ux_clamp_viewport_for_rows(ux, screen, screen != 0 ? screen->rows : 0);
}

static void winxterm_ux_record_bottom_anchor_for_rows(WinxtermUxState *ux,
                                                      const WinxtermScreen *screen,
                                                      int visible_rows)
{
    int view_rows = winxterm_ux_clamped_visible_rows(screen, visible_rows);
    if (ux == 0 || screen == 0 || screen->alternate_active ||
        ux->viewport.follow_output || ux->viewport.line_offset_from_bottom == 0u ||
        view_rows <= 0) {
        if (ux != 0) {
            ux->viewport.bottom_anchor_valid = false;
        }
        return;
    }

    size_t total_rows = winxterm_screen_primary_view_row_count(screen);
    if (total_rows == 0u) {
        ux->viewport.bottom_anchor_valid = false;
        return;
    }

    size_t first_row = winxterm_screen_default_primary_first_row_for_rows(screen,
                                                                         view_rows,
                                                                         ux->viewport.line_offset_from_bottom);
    size_t bottom_row = first_row + (size_t)(view_rows - 1);
    if (bottom_row >= total_rows) {
        bottom_row = total_rows - 1u;
    }

    ux->viewport.bottom_anchor_valid =
        winxterm_screen_primary_anchor_from_global_row(screen,
                                                       bottom_row,
                                                       &ux->viewport.bottom_anchor);
}

static bool winxterm_ux_restore_bottom_anchor_for_rows(WinxtermUxState *ux,
                                                       const WinxtermScreen *screen,
                                                       int visible_rows)
{
    int view_rows = winxterm_ux_clamped_visible_rows(screen, visible_rows);
    if (ux == 0 || screen == 0 || screen->alternate_active ||
        !ux->viewport.bottom_anchor_valid || view_rows <= 0) {
        return false;
    }

    size_t anchor_row = 0u;
    if (!winxterm_screen_primary_global_row_from_anchor(screen,
                                                        &ux->viewport.bottom_anchor,
                                                        &anchor_row)) {
        ux->viewport.bottom_anchor_valid = false;
        return false;
    }

    size_t target_first = anchor_row >= (size_t)view_rows ? anchor_row - (size_t)view_rows + 1u : 0u;
    size_t bottom_first = winxterm_screen_default_primary_first_row_for_rows(screen, view_rows, 0u);
    if (target_first > bottom_first) {
        target_first = bottom_first;
    }
    ux->viewport.line_offset_from_bottom = bottom_first - target_first;
    ux->viewport.follow_output = ux->viewport.line_offset_from_bottom == 0u;
    return true;
}

static void winxterm_ux_apply_row_count_delta(WinxtermUxState *ux, size_t rows)
{
    if (ux == 0 || rows <= ux->viewport.last_primary_row_count) {
        return;
    }
    size_t delta = rows - ux->viewport.last_primary_row_count;
    if (SIZE_MAX - ux->viewport.line_offset_from_bottom < delta) {
        ux->viewport.line_offset_from_bottom = SIZE_MAX;
    } else {
        ux->viewport.line_offset_from_bottom += delta;
    }
}

void winxterm_ux_note_screen_changed_for_rows(WinxtermUxState *ux,
                                              const WinxtermScreen *screen,
                                              int visible_rows)
{
    if (ux == 0 || screen == 0) {
        return;
    }

    size_t rows = winxterm_screen_primary_view_row_count(screen);
    if (ux->viewport.follow_output || screen->alternate_active) {
        ux->viewport.line_offset_from_bottom = 0u;
        ux->viewport.bottom_anchor_valid = false;
    } else if (!winxterm_ux_restore_bottom_anchor_for_rows(ux, screen, visible_rows)) {
        winxterm_ux_apply_row_count_delta(ux, rows);
    }
    ux->viewport.last_primary_row_count = rows;
    winxterm_ux_clamp_viewport_for_rows(ux, screen, visible_rows);
}

void winxterm_ux_note_screen_changed(WinxtermUxState *ux, const WinxtermScreen *screen)
{
    winxterm_ux_note_screen_changed_for_rows(ux, screen, screen != 0 ? screen->rows : 0);
}

void winxterm_ux_capture_resize_anchor_for_rows(WinxtermUxState *ux,
                                                const WinxtermScreen *screen,
                                                int visible_rows)
{
    if (ux == 0 || screen == 0) {
        return;
    }
    winxterm_ux_note_screen_changed_for_rows(ux, screen, visible_rows);
    winxterm_ux_record_bottom_anchor_for_rows(ux, screen, visible_rows);
}

void winxterm_ux_capture_resize_anchor(WinxtermUxState *ux, const WinxtermScreen *screen)
{
    winxterm_ux_capture_resize_anchor_for_rows(ux, screen, screen != 0 ? screen->rows : 0);
}

void winxterm_ux_restore_resize_anchor_for_rows(WinxtermUxState *ux,
                                                const WinxtermScreen *screen,
                                                int visible_rows)
{
    if (ux == 0 || screen == 0) {
        return;
    }
    size_t rows = winxterm_screen_primary_view_row_count(screen);
    if (ux->viewport.follow_output || screen->alternate_active) {
        ux->viewport.line_offset_from_bottom = 0u;
        ux->viewport.bottom_anchor_valid = false;
    } else if (!winxterm_ux_restore_bottom_anchor_for_rows(ux, screen, visible_rows)) {
        winxterm_ux_apply_row_count_delta(ux, rows);
    }
    ux->viewport.last_primary_row_count = rows;
    winxterm_ux_clamp_viewport_for_rows(ux, screen, visible_rows);
}

void winxterm_ux_restore_resize_anchor(WinxtermUxState *ux, const WinxtermScreen *screen)
{
    winxterm_ux_restore_resize_anchor_for_rows(ux, screen, screen != 0 ? screen->rows : 0);
}

void winxterm_ux_scroll_lines_for_rows(WinxtermUxState *ux,
                                       const WinxtermScreen *screen,
                                       int visible_rows,
                                       int lines_up)
{
    if (ux == 0 || screen == 0 || lines_up == 0) {
        return;
    }
    if (lines_up > 0) {
        size_t delta = (size_t)lines_up;
        if (SIZE_MAX - ux->viewport.line_offset_from_bottom < delta) {
            ux->viewport.line_offset_from_bottom = SIZE_MAX;
        } else {
            ux->viewport.line_offset_from_bottom += delta;
        }
    } else {
        size_t delta = (size_t)(-lines_up);
        ux->viewport.line_offset_from_bottom =
            delta >= ux->viewport.line_offset_from_bottom ? 0u :
                                                            ux->viewport.line_offset_from_bottom - delta;
    }
    ux->viewport.follow_output = ux->viewport.line_offset_from_bottom == 0u;
    winxterm_ux_clamp_viewport_for_rows(ux, screen, visible_rows);
    winxterm_ux_record_bottom_anchor_for_rows(ux, screen, visible_rows);
}

void winxterm_ux_scroll_lines(WinxtermUxState *ux, const WinxtermScreen *screen, int lines_up)
{
    winxterm_ux_scroll_lines_for_rows(ux, screen, screen != 0 ? screen->rows : 0, lines_up);
}

void winxterm_ux_scroll_page_for_rows(WinxtermUxState *ux,
                                      const WinxtermScreen *screen,
                                      int visible_rows,
                                      int pages_up)
{
    if (screen == 0) {
        return;
    }
    int view_rows = winxterm_ux_clamped_visible_rows(screen, visible_rows);
    int rows = view_rows > 1 ? view_rows - 1 : 1;
    winxterm_ux_scroll_lines_for_rows(ux, screen, visible_rows, pages_up * rows);
}

void winxterm_ux_scroll_page(WinxtermUxState *ux, const WinxtermScreen *screen, int pages_up)
{
    winxterm_ux_scroll_page_for_rows(ux, screen, screen != 0 ? screen->rows : 0, pages_up);
}

void winxterm_ux_scroll_to_bottom(WinxtermUxState *ux)
{
    if (ux == 0) {
        return;
    }
    ux->viewport.line_offset_from_bottom = 0u;
    ux->viewport.follow_output = true;
    ux->viewport.bottom_anchor_valid = false;
}

void winxterm_ux_scroll_to_top_for_rows(WinxtermUxState *ux,
                                        const WinxtermScreen *screen,
                                        int visible_rows)
{
    if (ux == 0) {
        return;
    }
    ux->viewport.line_offset_from_bottom = winxterm_ux_max_offset_for_rows(screen, visible_rows);
    ux->viewport.follow_output = ux->viewport.line_offset_from_bottom == 0u;
    winxterm_ux_clamp_viewport_for_rows(ux, screen, visible_rows);
    winxterm_ux_record_bottom_anchor_for_rows(ux, screen, visible_rows);
}

void winxterm_ux_scroll_to_top(WinxtermUxState *ux, const WinxtermScreen *screen)
{
    winxterm_ux_scroll_to_top_for_rows(ux, screen, screen != 0 ? screen->rows : 0);
}

size_t winxterm_ux_primary_first_row_for_rows(const WinxtermUxState *ux,
                                              const WinxtermScreen *screen,
                                              int visible_rows)
{
    size_t offset = ux != 0 ? ux->viewport.line_offset_from_bottom : 0u;
    return winxterm_screen_default_primary_first_row_for_rows(screen, visible_rows, offset);
}

size_t winxterm_ux_primary_first_row(const WinxtermUxState *ux, const WinxtermScreen *screen)
{
    return winxterm_ux_primary_first_row_for_rows(ux, screen, screen != 0 ? screen->rows : 0);
}

static size_t winxterm_ux_position_global_row(const WinxtermScreen *screen, WinxtermUxPosition position)
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

bool winxterm_ux_hit_test_cells(const WinxtermUxState *ux,
                                const WinxtermScreen *screen,
                                int pixel_x,
                                int pixel_y,
                                unsigned int display_scale,
                                int visible_columns,
                                int visible_rows,
                                WinxtermUxPosition *position)
{
    if (screen == 0 || position == 0 || pixel_x < 0 || pixel_y < 0) {
        return false;
    }
    pixel_x = winxterm_physical_to_logical_pixels(pixel_x, display_scale);
    pixel_y = winxterm_physical_to_logical_pixels(pixel_y, display_scale);
    int column = pixel_x / WINXTERM_CELL_WIDTH_PIXELS;
    int view_row = pixel_y / WINXTERM_CELL_HEIGHT_PIXELS;
    int view_columns = winxterm_ux_clamped_visible_columns(screen, visible_columns);
    int view_rows = winxterm_ux_clamped_visible_rows(screen, visible_rows);
    if (column < 0 || column >= view_columns || view_row < 0 || view_row >= view_rows) {
        return false;
    }

    if (screen->alternate_active) {
        position->kind = WINXTERM_UX_ROW_ALTERNATE_VISIBLE;
        position->row = (size_t)view_row;
        position->column = column;
        return true;
    }

    size_t global_row = winxterm_ux_primary_first_row_for_rows(ux, screen, view_rows) + (size_t)view_row;
    size_t scrollback = winxterm_screen_scrollback_count(screen);
    if (global_row < scrollback) {
        position->kind = WINXTERM_UX_ROW_PRIMARY_SCROLLBACK;
        position->row = global_row;
    } else {
        position->kind = WINXTERM_UX_ROW_PRIMARY_VISIBLE;
        position->row = global_row - scrollback;
    }
    position->column = column;
    return true;
}

bool winxterm_ux_hit_test(const WinxtermUxState *ux,
                          const WinxtermScreen *screen,
                          int pixel_x,
                          int pixel_y,
                          unsigned int display_scale,
                          WinxtermUxPosition *position)
{
    return winxterm_ux_hit_test_cells(ux,
                                      screen,
                                      pixel_x,
                                      pixel_y,
                                      display_scale,
                                      screen != 0 ? screen->columns : 0,
                                      screen != 0 ? screen->rows : 0,
                                      position);
}

void winxterm_ux_begin_selection(WinxtermUxState *ux, WinxtermUxPosition position)
{
    winxterm_ux_begin_selection_mode(ux, position, WINXTERM_SELECTION_LINEAR);
}

void winxterm_ux_begin_selection_mode(WinxtermUxState *ux,
                                      WinxtermUxPosition position,
                                      WinxtermSelectionMode mode)
{
    if (ux == 0) {
        return;
    }
    ux->selection.active = true;
    ux->selection.selecting = true;
    ux->selection.mode = mode;
    ux->selection.anchor = position;
    ux->selection.extent = position;
}

void winxterm_ux_update_selection(WinxtermUxState *ux, WinxtermUxPosition position)
{
    if (ux == 0 || !ux->selection.selecting) {
        return;
    }
    ux->selection.extent = position;
}

void winxterm_ux_finish_selection(WinxtermUxState *ux)
{
    if (ux != 0) {
        ux->selection.selecting = false;
    }
}

void winxterm_ux_clear_selection(WinxtermUxState *ux)
{
    if (ux != 0) {
        memset(&ux->selection, 0, sizeof(ux->selection));
    }
}

void winxterm_ux_select_all(WinxtermUxState *ux, const WinxtermScreen *screen)
{
    if (ux == 0 || screen == 0 || screen->columns <= 0 || screen->rows <= 0) {
        return;
    }
    ux->selection.active = true;
    ux->selection.selecting = false;
    ux->selection.mode = WINXTERM_SELECTION_LINEAR;
    ux->selection.anchor.column = 0;
    ux->selection.extent.column = screen->columns - 1;
    if (screen->alternate_active) {
        ux->selection.anchor.kind = WINXTERM_UX_ROW_ALTERNATE_VISIBLE;
        ux->selection.anchor.row = 0u;
        ux->selection.extent.kind = WINXTERM_UX_ROW_ALTERNATE_VISIBLE;
        ux->selection.extent.row = (size_t)(screen->rows - 1);
    } else {
        ux->selection.anchor.kind = WINXTERM_UX_ROW_PRIMARY_SCROLLBACK;
        ux->selection.anchor.row = 0u;
        ux->selection.extent.kind = WINXTERM_UX_ROW_PRIMARY_VISIBLE;
        ux->selection.extent.row = (size_t)(screen->rows - 1);
    }
}

static bool winxterm_ux_word_char(uint32_t codepoint)
{
    return (codepoint >= (uint32_t)'A' && codepoint <= (uint32_t)'Z') ||
           (codepoint >= (uint32_t)'a' && codepoint <= (uint32_t)'z') ||
           (codepoint >= (uint32_t)'0' && codepoint <= (uint32_t)'9') ||
           codepoint == (uint32_t)'_';
}

bool winxterm_ux_select_word_at(WinxtermUxState *ux,
                                const WinxtermScreen *screen,
                                WinxtermUxPosition position)
{
    if (ux == 0 || screen == 0) {
        return false;
    }
    WinxtermScreenRowView row;
    bool ok = position.kind == WINXTERM_UX_ROW_ALTERNATE_VISIBLE ?
        winxterm_screen_get_alternate_view_row(screen, position.row, &row) :
        winxterm_screen_get_primary_view_row(screen, winxterm_ux_position_global_row(screen, position), &row);
    if (!ok || row.cells == 0 || position.column < 0 || position.column >= row.columns) {
        return false;
    }
    uint32_t codepoint = row.cells[position.column].codepoint;
    bool word = winxterm_ux_word_char(codepoint);
    int start = position.column;
    int end = position.column;
    while (start > 0 && winxterm_ux_word_char(row.cells[start - 1].codepoint) == word) {
        --start;
    }
    while (end + 1 < row.columns && winxterm_ux_word_char(row.cells[end + 1].codepoint) == word) {
        ++end;
    }
    ux->selection.active = true;
    ux->selection.selecting = false;
    ux->selection.mode = WINXTERM_SELECTION_LINEAR;
    ux->selection.anchor = position;
    ux->selection.extent = position;
    ux->selection.anchor.column = start;
    ux->selection.extent.column = end;
    return true;
}

static WinxtermUxPosition winxterm_ux_position_from_global_row(const WinxtermScreen *screen,
                                                               bool alternate,
                                                               size_t global_row,
                                                               int column)
{
    WinxtermUxPosition position;
    position.column = column;
    if (alternate) {
        position.kind = WINXTERM_UX_ROW_ALTERNATE_VISIBLE;
        position.row = global_row;
        return position;
    }

    size_t scrollback = winxterm_screen_scrollback_count(screen);
    if (global_row < scrollback) {
        position.kind = WINXTERM_UX_ROW_PRIMARY_SCROLLBACK;
        position.row = global_row;
    } else {
        position.kind = WINXTERM_UX_ROW_PRIMARY_VISIBLE;
        position.row = global_row - scrollback;
    }
    return position;
}

static bool winxterm_ux_get_row(const WinxtermScreen *screen,
                                bool alternate,
                                size_t global_row,
                                WinxtermScreenRowView *row)
{
    return alternate ?
        winxterm_screen_get_alternate_view_row(screen, global_row, row) :
        winxterm_screen_get_primary_view_row(screen, global_row, row);
}

static bool winxterm_ux_real_line_bounds(const WinxtermScreen *screen,
                                         WinxtermUxPosition position,
                                         bool *alternate,
                                         size_t *start_row,
                                         size_t *end_row)
{
    if (screen == 0 || alternate == 0 || start_row == 0 || end_row == 0) {
        return false;
    }

    bool is_alternate = position.kind == WINXTERM_UX_ROW_ALTERNATE_VISIBLE;
    size_t total_rows = is_alternate ? (size_t)screen->rows : winxterm_screen_primary_view_row_count(screen);
    size_t row_index = winxterm_ux_position_global_row(screen, position);
    if (row_index >= total_rows) {
        return false;
    }

    size_t first = row_index;
    while (first > 0u) {
        WinxtermScreenRowView previous;
        if (!winxterm_ux_get_row(screen, is_alternate, first - 1u, &previous) || !previous.soft_wrapped) {
            break;
        }
        --first;
    }

    size_t last = row_index;
    while (last + 1u < total_rows) {
        WinxtermScreenRowView current;
        if (!winxterm_ux_get_row(screen, is_alternate, last, &current) || !current.soft_wrapped) {
            break;
        }
        ++last;
    }

    *alternate = is_alternate;
    *start_row = first;
    *end_row = last;
    return true;
}

static bool winxterm_ux_cell_non_space(const WinxtermScreenCell *cell)
{
    if (cell == 0 || (cell->attribute_flags & WINXTERM_SCREEN_CELL_INVISIBLE) != 0u) {
        return false;
    }
    if (cell->continuation) {
        return true;
    }
    return cell->codepoint != 0u && cell->codepoint != (uint32_t)' ';
}

static bool winxterm_ux_previous_non_space_cell(const WinxtermScreen *screen,
                                                bool alternate,
                                                size_t line_start,
                                                size_t *row_index,
                                                int *column)
{
    if (screen == 0 || row_index == 0 || column == 0 || *row_index < line_start) {
        return false;
    }

    if (*column > 0) {
        --*column;
    } else {
        if (*row_index == line_start) {
            return false;
        }
        --*row_index;
        WinxtermScreenRowView row;
        if (!winxterm_ux_get_row(screen, alternate, *row_index, &row) || row.columns <= 0) {
            return false;
        }
        *column = row.columns - 1;
    }

    WinxtermScreenRowView row;
    if (!winxterm_ux_get_row(screen, alternate, *row_index, &row) ||
        *column < 0 || *column >= row.columns ||
        !winxterm_ux_cell_non_space(&row.cells[*column])) {
        return false;
    }
    return true;
}

static bool winxterm_ux_next_non_space_cell(const WinxtermScreen *screen,
                                            bool alternate,
                                            size_t line_end,
                                            size_t *row_index,
                                            int *column)
{
    if (screen == 0 || row_index == 0 || column == 0 || *row_index > line_end) {
        return false;
    }

    WinxtermScreenRowView row;
    if (!winxterm_ux_get_row(screen, alternate, *row_index, &row)) {
        return false;
    }
    if (*column + 1 < row.columns) {
        ++*column;
    } else {
        if (*row_index == line_end) {
            return false;
        }
        ++*row_index;
        if (!winxterm_ux_get_row(screen, alternate, *row_index, &row) || row.columns <= 0) {
            return false;
        }
        *column = 0;
    }

    if (*column < 0 || *column >= row.columns || !winxterm_ux_cell_non_space(&row.cells[*column])) {
        return false;
    }
    return true;
}

bool winxterm_ux_select_non_space_run_at(WinxtermUxState *ux,
                                         const WinxtermScreen *screen,
                                         WinxtermUxPosition position)
{
    if (ux == 0 || screen == 0) {
        return false;
    }

    bool alternate = false;
    size_t line_start = 0u;
    size_t line_end = 0u;
    if (!winxterm_ux_real_line_bounds(screen, position, &alternate, &line_start, &line_end)) {
        return false;
    }

    size_t click_row = winxterm_ux_position_global_row(screen, position);
    WinxtermScreenRowView row;
    if (!winxterm_ux_get_row(screen, alternate, click_row, &row) ||
        position.column < 0 || position.column >= row.columns ||
        !winxterm_ux_cell_non_space(&row.cells[position.column])) {
        return false;
    }

    size_t start_row = click_row;
    size_t end_row = click_row;
    int start_col = position.column;
    int end_col = position.column;
    for (;;) {
        size_t previous_row = start_row;
        int previous_col = start_col;
        if (!winxterm_ux_previous_non_space_cell(screen, alternate, line_start, &previous_row, &previous_col)) {
            break;
        }
        start_row = previous_row;
        start_col = previous_col;
    }
    for (;;) {
        size_t next_row = end_row;
        int next_col = end_col;
        if (!winxterm_ux_next_non_space_cell(screen, alternate, line_end, &next_row, &next_col)) {
            break;
        }
        end_row = next_row;
        end_col = next_col;
    }

    ux->selection.active = true;
    ux->selection.selecting = false;
    ux->selection.mode = WINXTERM_SELECTION_LINEAR;
    ux->selection.anchor = winxterm_ux_position_from_global_row(screen, alternate, start_row, start_col);
    ux->selection.extent = winxterm_ux_position_from_global_row(screen, alternate, end_row, end_col);
    return true;
}

bool winxterm_ux_select_real_line_at(WinxtermUxState *ux,
                                     const WinxtermScreen *screen,
                                     WinxtermUxPosition position)
{
    if (ux == 0 || screen == 0) {
        return false;
    }

    bool alternate = false;
    size_t line_start = 0u;
    size_t line_end = 0u;
    if (!winxterm_ux_real_line_bounds(screen, position, &alternate, &line_start, &line_end)) {
        return false;
    }

    WinxtermScreenRowView row;
    if (!winxterm_ux_get_row(screen, alternate, line_end, &row) || row.columns <= 0) {
        return false;
    }

    ux->selection.active = true;
    ux->selection.selecting = false;
    ux->selection.mode = WINXTERM_SELECTION_LINEAR;
    ux->selection.anchor = winxterm_ux_position_from_global_row(screen, alternate, line_start, 0);
    ux->selection.extent =
        winxterm_ux_position_from_global_row(screen, alternate, line_end, row.columns - 1);
    return true;
}

bool winxterm_ux_has_selection(const WinxtermUxState *ux)
{
    return ux != 0 && ux->selection.active;
}

WinxtermScreenSelectionRange winxterm_ux_render_selection(const WinxtermUxState *ux,
                                                          const WinxtermScreen *screen)
{
    WinxtermScreenSelectionRange range;
    memset(&range, 0, sizeof(range));
    if (ux == 0 || screen == 0 || !ux->selection.active ||
        (ux->selection.anchor.kind == WINXTERM_UX_ROW_ALTERNATE_VISIBLE) !=
            (ux->selection.extent.kind == WINXTERM_UX_ROW_ALTERNATE_VISIBLE)) {
        return range;
    }
    range.enabled = true;
    range.alternate = ux->selection.anchor.kind == WINXTERM_UX_ROW_ALTERNATE_VISIBLE;
    size_t start_row = winxterm_ux_position_global_row(screen, ux->selection.anchor);
    size_t end_row = winxterm_ux_position_global_row(screen, ux->selection.extent);
    int start_col = ux->selection.anchor.column;
    int end_col = ux->selection.extent.column;
    if (ux->selection.mode == WINXTERM_SELECTION_RECTANGULAR) {
        range.rectangular = true;
        if (start_row > end_row) {
            size_t tmp_row = start_row;
            start_row = end_row;
            end_row = tmp_row;
        }
        if (start_col > end_col) {
            int tmp_col = start_col;
            start_col = end_col;
            end_col = tmp_col;
        }
    } else if (start_row > end_row || (start_row == end_row && start_col > end_col)) {
        size_t tmp_row = start_row;
        int tmp_col = start_col;
        start_row = end_row;
        start_col = end_col;
        end_row = tmp_row;
        end_col = tmp_col;
    }
    range.start_row = start_row;
    range.end_row = end_row;
    range.start_column = start_col;
    range.end_column = end_col;
    return range;
}

static bool winxterm_ux_buffer_reserve(WinxtermUxBuffer *buffer, size_t needed)
{
    if (needed <= buffer->capacity) {
        return true;
    }
    size_t capacity = buffer->capacity == 0u ? 128u : buffer->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2u) {
            return false;
        }
        capacity *= 2u;
    }
    char *data = (char *)realloc(buffer->data, capacity);
    if (data == 0) {
        return false;
    }
    buffer->data = data;
    buffer->capacity = capacity;
    return true;
}

static bool winxterm_ux_buffer_append_byte(WinxtermUxBuffer *buffer, char byte)
{
    if (!winxterm_ux_buffer_reserve(buffer, buffer->length + 2u)) {
        return false;
    }
    buffer->data[buffer->length++] = byte;
    buffer->data[buffer->length] = '\0';
    return true;
}

static bool winxterm_ux_buffer_append_codepoint(WinxtermUxBuffer *buffer, uint32_t codepoint)
{
    if (codepoint <= 0x7fu) {
        return winxterm_ux_buffer_append_byte(buffer, (char)codepoint);
    }
    if (codepoint <= 0x7ffu) {
        return winxterm_ux_buffer_append_byte(buffer, (char)(0xc0u | (codepoint >> 6))) &&
               winxterm_ux_buffer_append_byte(buffer, (char)(0x80u | (codepoint & 0x3fu)));
    }
    if (codepoint <= 0xffffu) {
        return winxterm_ux_buffer_append_byte(buffer, (char)(0xe0u | (codepoint >> 12))) &&
               winxterm_ux_buffer_append_byte(buffer, (char)(0x80u | ((codepoint >> 6) & 0x3fu))) &&
               winxterm_ux_buffer_append_byte(buffer, (char)(0x80u | (codepoint & 0x3fu)));
    }
    if (codepoint <= 0x10ffffu) {
        return winxterm_ux_buffer_append_byte(buffer, (char)(0xf0u | (codepoint >> 18))) &&
               winxterm_ux_buffer_append_byte(buffer, (char)(0x80u | ((codepoint >> 12) & 0x3fu))) &&
               winxterm_ux_buffer_append_byte(buffer, (char)(0x80u | ((codepoint >> 6) & 0x3fu))) &&
               winxterm_ux_buffer_append_byte(buffer, (char)(0x80u | (codepoint & 0x3fu)));
    }
    return false;
}

static bool winxterm_ux_buffer_append_cell(WinxtermUxBuffer *buffer,
                                           const WinxtermScreenCell *cell)
{
    if (cell == 0 || cell->continuation ||
        (cell->attribute_flags & WINXTERM_SCREEN_CELL_INVISIBLE) != 0u) {
        return true;
    }
    if (!winxterm_ux_buffer_append_codepoint(buffer, cell->codepoint)) {
        return false;
    }
    for (uint8_t i = 0u; i < cell->combining_count; ++i) {
        if (!winxterm_ux_buffer_append_codepoint(buffer, cell->combining_codepoints[i])) {
            return false;
        }
    }
    return true;
}

static void winxterm_ux_buffer_trim_ascii_spaces(WinxtermUxBuffer *buffer, size_t row_start)
{
    if (buffer == 0 || buffer->data == 0 || buffer->length <= row_start) {
        return;
    }
    while (buffer->length > row_start && buffer->data[buffer->length - 1u] == ' ') {
        buffer->data[--buffer->length] = '\0';
    }
    size_t content_start = row_start;
    while (content_start < buffer->length && buffer->data[content_start] == ' ') {
        ++content_start;
    }
    if (content_start > row_start) {
        size_t content_length = buffer->length - content_start;
        memmove(buffer->data + row_start, buffer->data + content_start, content_length);
        buffer->length = row_start + content_length;
        buffer->data[buffer->length] = '\0';
    }
}

bool winxterm_ux_extract_selection_utf8_format(const WinxtermUxState *ux,
                                               const WinxtermScreen *screen,
                                               WinxtermSelectionCopyFormat format,
                                               char **out_text,
                                               size_t *out_length)
{
    if (out_text == 0 || out_length == 0) {
        return false;
    }
    *out_text = 0;
    *out_length = 0u;
    WinxtermScreenSelectionRange range = winxterm_ux_render_selection(ux, screen);
    if (!range.enabled) {
        return false;
    }

    WinxtermUxBuffer buffer = {0};
    for (size_t row_index = range.start_row; row_index <= range.end_row; ++row_index) {
        WinxtermScreenRowView row;
        bool ok = range.alternate ?
            winxterm_screen_get_alternate_view_row(screen, row_index, &row) :
            winxterm_screen_get_primary_view_row(screen, row_index, &row);
        if (!ok || row.cells == 0) {
            continue;
        }
        int first_column = range.rectangular || row_index == range.start_row ? range.start_column : 0;
        int last_column = range.rectangular || row_index == range.end_row ? range.end_column : row.columns - 1;
        if (first_column < 0) {
            first_column = 0;
        }
        if (last_column >= row.columns) {
            last_column = row.columns - 1;
        }
        size_t row_start = buffer.length;
        for (int column = first_column; column <= last_column; ++column) {
            if (!winxterm_ux_buffer_append_cell(&buffer, &row.cells[column])) {
                free(buffer.data);
                return false;
            }
        }
        if (range.rectangular) {
            if (format == WINXTERM_SELECTION_COPY_RECTANGULAR_PRESERVE_ROWS) {
                if (row_index != range.end_row && !winxterm_ux_buffer_append_byte(&buffer, '\n')) {
                    free(buffer.data);
                    return false;
                }
            } else {
                winxterm_ux_buffer_trim_ascii_spaces(&buffer, row_start);
                if (!winxterm_ux_buffer_append_byte(&buffer, ' ')) {
                    free(buffer.data);
                    return false;
                }
            }
        } else if (!row.soft_wrapped) {
            while (buffer.length > row_start && buffer.data[buffer.length - 1u] == ' ') {
                buffer.data[--buffer.length] = '\0';
            }
            if (row_index != range.end_row && !winxterm_ux_buffer_append_byte(&buffer, '\n')) {
                free(buffer.data);
                return false;
            }
        }
    }
    if (buffer.data == 0 && !winxterm_ux_buffer_append_byte(&buffer, '\0')) {
        return false;
    }
    if (buffer.length > 0u && buffer.data[buffer.length - 1u] == '\0') {
        --buffer.length;
    }
    *out_text = buffer.data;
    *out_length = buffer.length;
    return true;
}

bool winxterm_ux_extract_selection_utf8(const WinxtermUxState *ux,
                                        const WinxtermScreen *screen,
                                        char **out_text,
                                        size_t *out_length)
{
    return winxterm_ux_extract_selection_utf8_format(ux,
                                                     screen,
                                                     WINXTERM_SELECTION_COPY_DEFAULT,
                                                     out_text,
                                                     out_length);
}

void winxterm_ux_start_bell(WinxtermUxState *ux, DWORD now)
{
    if (ux != 0) {
        ux->bell.active = true;
        ux->bell.start_tick = now;
    }
}

bool winxterm_ux_bell_active(const WinxtermUxState *ux, DWORD now)
{
    return ux != 0 && ux->bell.active && now - ux->bell.start_tick < WINXTERM_UX_BELL_DURATION_MS;
}

const wchar_t *winxterm_ux_bell_title_prefix(const WinxtermUxState *ux, DWORD now)
{
    if (!winxterm_ux_bell_active(ux, now)) {
        return L"";
    }
    DWORD phase = (now - ux->bell.start_tick) / WINXTERM_UX_BELL_TIMER_MS;
    return (phase % 2u) == 0u ? L"\xD83D\xDD14 " : L"  ";
}
