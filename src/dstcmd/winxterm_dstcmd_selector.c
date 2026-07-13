#include "dstcmd/winxterm_dstcmd_selector.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static bool winxterm_dstcmd_selector_equal(char left, char right, bool smart_case)
{
    return smart_case ? left == right :
        tolower((unsigned char)left) == tolower((unsigned char)right);
}

const char *winxterm_dstcmd_selector_contains_utf8(const char *text,
                                                   const char *query,
                                                   bool smart_case)
{
    if (text == 0 || query == 0) return 0;
    if (query[0] == '\0') return text;
    for (const char *start = text; *start != '\0'; ++start) {
        size_t i = 0u;
        while (query[i] != '\0' && start[i] != '\0' &&
               winxterm_dstcmd_selector_equal(start[i], query[i], smart_case)) ++i;
        if (query[i] == '\0') return start;
    }
    return 0;
}

bool winxterm_dstcmd_selector_fuzzy_utf8(const char *text, const char *query,
                                        bool smart_case, int *gap_penalty)
{
    if (gap_penalty != 0) *gap_penalty = 0;
    if (text == 0 || query == 0) return false;
    int gaps = 0;
    while (*query != '\0') {
        while (*text != '\0' && !winxterm_dstcmd_selector_equal(*text, *query, smart_case)) {
            ++gaps;
            ++text;
        }
        if (*text == '\0') return false;
        ++text;
        ++query;
    }
    if (gap_penalty != 0) *gap_penalty = gaps;
    return true;
}

static char *winxterm_dstcmd_selector_utf8(const wchar_t *text)
{
    int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1, 0, 0, 0, 0);
    char *utf8 = count > 0 ? (char *)malloc((size_t)count) : 0;
    if (utf8 == 0 || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                                         utf8, count, 0, 0) != count) {
        free(utf8);
        return 0;
    }
    return utf8;
}

static size_t winxterm_dstcmd_selector_filter(const wchar_t *const *items, size_t item_count,
                                              const char *query, bool contains,
                                              size_t *results)
{
    size_t count = 0u;
    for (size_t i = 0u; i < item_count; ++i) {
        char *utf8 = winxterm_dstcmd_selector_utf8(items[i]);
        bool match = utf8 != 0 &&
            (contains ? winxterm_dstcmd_selector_contains_utf8(utf8, query, false) != 0 :
                        winxterm_dstcmd_selector_fuzzy_utf8(utf8, query, false, 0));
        free(utf8);
        if (match) results[count++] = i;
    }
    return count;
}

static bool winxterm_dstcmd_selector_render(WinxtermDstcmdShell *shell, const wchar_t *title,
                                            const wchar_t *const *items,
                                            const size_t *results, size_t result_count,
                                            size_t selected, size_t scroll, size_t rows,
                                            const char *query, bool contains)
{
    if (!winxterm_dstcmd_shell_write_utf8(shell, "\x1b[H\x1b[2J\x1b[38;2;235;245;255;48;2;0;0;64m")) {
        return false;
    }
    if (!winxterm_dstcmd_shell_write_widef(shell, L"%ls  [%ls]  filter: ", title,
                                            contains ? L"contains" : L"fuzzy") ||
        !winxterm_dstcmd_shell_write_bytes(shell, (const uint8_t *)query, strlen(query)) ||
        !winxterm_dstcmd_shell_write_utf8(shell, "\x1b[0m\r\n")) return false;
    for (size_t row = 0u; row < rows; ++row) {
        size_t visible = scroll + row;
        if (visible < result_count) {
            if (!winxterm_dstcmd_shell_write_utf8(
                    shell, visible == selected ?
                        "\x1b[38;2;255;255;0;48;2;0;0;0m> " :
                        "\x1b[38;2;235;245;255;48;2;0;0;64m  ") ||
                !winxterm_dstcmd_shell_write_wide(shell, items[results[visible]]) ||
                !winxterm_dstcmd_shell_write_utf8(shell, "\x1b[0m\r\n")) return false;
        } else if (!winxterm_dstcmd_shell_write_utf8(shell, "\r\n")) return false;
    }
    return true;
}

bool winxterm_dstcmd_selector_select_dynamic(WinxtermDstcmdShell *shell,
                                            const wchar_t *title,
                                            const wchar_t *const *items,
                                            size_t item_count,
                                            size_t visible_rows,
                                            HANDLE refresh_event,
                                            WinxtermDstcmdSelectorRefresh refresh,
                                            void *refresh_context,
                                            size_t *selected_index)
{
    if (selected_index != 0) *selected_index = 0u;
    if (shell == 0 || title == 0 || items == 0 || item_count == 0u ||
        visible_rows == 0u || selected_index == 0) return false;
    size_t results_capacity = item_count;
    size_t *results = (size_t *)malloc(results_capacity * sizeof(*results));
    if (results == 0) return false;
    char query[WINXTERM_DSTCMD_LINE_CAPACITY];
    query[0] = '\0';
    size_t query_length = 0u, selected = 0u, scroll = 0u;
    bool contains = false, escape = false, csi = false, accepted = false, done = false;
    (void)winxterm_dstcmd_shell_write_utf8(shell, "\x1b[?1049h\x1b[?25h");
    while (!done) {
        size_t count = winxterm_dstcmd_selector_filter(items, item_count, query, contains, results);
        if (selected >= count) selected = count != 0u ? count - 1u : 0u;
        if (selected < scroll) scroll = selected;
        if (selected >= scroll + visible_rows) scroll = selected - visible_rows + 1u;
        if (!winxterm_dstcmd_selector_render(shell, title, items, results, count, selected,
                                             scroll, visible_rows, query, contains)) break;
        if (refresh_event != 0 && refresh != 0) {
            HANDLE input_handle = shell->stream_input_handle != 0 ?
                shell->stream_input_handle : shell->input_handle;
            HANDLE waits[2] = {input_handle, refresh_event};
            DWORD wait = WaitForMultipleObjects(2u, waits, FALSE, INFINITE);
            if (wait == WAIT_OBJECT_0 + 1u) {
                const wchar_t *const *updated_items = items;
                size_t updated_count = item_count;
                if (!refresh(refresh_context, &updated_items, &updated_count) ||
                    updated_items == 0 || updated_count == 0u) {
                    done = true;
                    continue;
                }
                if (updated_count > results_capacity) {
                    size_t *grown = (size_t *)realloc(results,
                                                      updated_count * sizeof(*grown));
                    if (grown == 0) { done = true; continue; }
                    results = grown;
                    results_capacity = updated_count;
                }
                items = updated_items;
                item_count = updated_count;
                continue;
            }
            if (wait != WAIT_OBJECT_0) break;
        }
        uint8_t input[64];
        size_t input_count = winxterm_dstcmd_shell_read_input(shell, input, sizeof(input), true);
        if (input_count == 0u) break;
        for (size_t i = 0u; i < input_count && !done; ++i) {
            uint8_t ch = input[i];
            if (csi) {
                if (ch == 'A' && count != 0u) selected = selected == 0u ? count - 1u : selected - 1u;
                else if (ch == 'B' && count != 0u) selected = (selected + 1u) % count;
                csi = false;
                escape = false;
            } else if (escape) {
                if (ch == '[') csi = true;
                else if (ch == 'r' || ch == 'R') { contains = !contains; escape = false; }
                else { done = true; escape = false; }
            } else if (ch == 0x1bu) escape = true;
            else if (ch == 0x03u || ch == 0x07u) done = true;
            else if (ch == '\r' || ch == '\n') {
                if (count != 0u) { *selected_index = results[selected]; accepted = true; }
                done = true;
            } else if (ch == '\b' || ch == 0x7fu) {
                if (query_length != 0u) query[--query_length] = '\0';
                selected = scroll = 0u;
            } else if (ch >= 0x20u && query_length + 1u < sizeof(query)) {
                query[query_length++] = (char)ch;
                query[query_length] = '\0';
                selected = scroll = 0u;
            }
        }
    }
    (void)winxterm_dstcmd_shell_write_utf8(shell, "\x1b[?1049l");
    free(results);
    return accepted;
}

bool winxterm_dstcmd_selector_select(WinxtermDstcmdShell *shell,
                                    const wchar_t *title,
                                    const wchar_t *const *items,
                                    size_t item_count,
                                    size_t visible_rows,
                                    size_t *selected_index)
{
    return winxterm_dstcmd_selector_select_dynamic(
        shell, title, items, item_count, visible_rows, 0, 0, 0, selected_index);
}

bool winxterm_dstcmd_selector_select_menu(WinxtermDstcmdShell *shell,
                                         const wchar_t *title,
                                         const wchar_t *const *items,
                                         const bool *enabled,
                                         size_t item_count,
                                         size_t *selected_index)
{
    if (selected_index != 0) *selected_index = 0u;
    if (shell == 0 || title == 0 || items == 0 || enabled == 0 ||
        item_count == 0u || selected_index == 0) return false;
    size_t selected = 0u;
    while (selected < item_count && !enabled[selected]) ++selected;
    if (selected == item_count) return false;
    bool accepted = false, done = false, escape = false, csi = false;
    (void)winxterm_dstcmd_shell_write_utf8(shell, "\x1b[?1049h\x1b[?25l");
    while (!done) {
        if (!winxterm_dstcmd_shell_write_utf8(shell, "\x1b[H\x1b[2J") ||
            !winxterm_dstcmd_shell_write_widef(shell, L"%ls\r\n", title)) break;
        for (size_t i = 0u; i < item_count; ++i) {
            const char *style = !enabled[i] ? "\x1b[38;2;96;96;96m  " :
                i == selected ? "\x1b[38;2;255;255;0;48;2;0;0;0m> " :
                                "\x1b[38;2;235;245;255;48;2;0;0;64m  ";
            if (!winxterm_dstcmd_shell_write_utf8(shell, style) ||
                !winxterm_dstcmd_shell_write_wide(shell, items[i]) ||
                !winxterm_dstcmd_shell_write_utf8(shell, "\x1b[0m\r\n")) {
                done = true;
                break;
            }
        }
        if (done) break;
        uint8_t input[32];
        size_t count = winxterm_dstcmd_shell_read_input(shell, input, sizeof(input), true);
        if (count == 0u) break;
        for (size_t i = 0u; i < count && !done; ++i) {
            uint8_t ch = input[i];
            if (csi) {
                if (ch == 'A' || ch == 'B') {
                    size_t candidate = selected;
                    do {
                        candidate = ch == 'A' ?
                            (candidate == 0u ? item_count - 1u : candidate - 1u) :
                            (candidate + 1u) % item_count;
                    } while (!enabled[candidate] && candidate != selected);
                    selected = candidate;
                }
                csi = false;
                escape = false;
            } else if (escape) {
                if (ch == '[') csi = true;
                else done = true;
            } else if (ch == 0x1bu) escape = true;
            else if (ch == 0x03u || ch == 0x07u) done = true;
            else if (ch == '\r' || ch == '\n') {
                *selected_index = selected;
                accepted = true;
                done = true;
            }
        }
    }
    (void)winxterm_dstcmd_shell_write_utf8(shell, "\x1b[?25h\x1b[?1049l");
    return accepted;
}
