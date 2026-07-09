#include "dstcmd/commands/highlight.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#define WINXTERM_DSTCMD_HIGHLIGHT_CHUNK_SIZE 1024u
#define WINXTERM_DSTCMD_HIGHLIGHT_USAGE "highlight: usage: highlight [-i] STRING...\r\n"

typedef struct WinxtermDstcmdHighlightPattern {
    char *text;
    size_t length;
} WinxtermDstcmdHighlightPattern;

static const char *const winxterm_dstcmd_highlight_colors[] = {
    "\x1b[38;2;0;255;255;48;2;0;0;0m",
    "\x1b[38;2;0;128;255;48;2;0;0;0m",
    "\x1b[38;2;0;255;0;48;2;0;0;0m",
    "\x1b[38;2;255;255;0;48;2;0;0;0m",
    "\x1b[38;2;255;165;0;48;2;0;0;0m",
    "\x1b[38;2;255;105;180;48;2;0;0;0m",
    "\x1b[38;2;128;0;128;48;2;0;0;0m",
    "\x1b[38;2;255;0;255;48;2;0;0;0m",
};

static const char *winxterm_dstcmd_highlight_color(size_t index)
{
    size_t color_count = sizeof(winxterm_dstcmd_highlight_colors) /
                         sizeof(winxterm_dstcmd_highlight_colors[0]);
    return winxterm_dstcmd_highlight_colors[index % color_count];
}

static bool winxterm_dstcmd_highlight_wide_to_utf8(const wchar_t *text,
                                                   char **utf8,
                                                   size_t *length)
{
    if (utf8 == 0 || length == 0) {
        return false;
    }
    *utf8 = 0;
    *length = 0u;
    if (text == 0) {
        return false;
    }
    int byte_count = WideCharToMultiByte(CP_UTF8, 0, text, -1, 0, 0, 0, 0);
    if (byte_count <= 0) {
        return false;
    }
    char *buffer = (char *)calloc((size_t)byte_count, sizeof(*buffer));
    if (buffer == 0) {
        return false;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, buffer, byte_count, 0, 0) <= 0) {
        free(buffer);
        return false;
    }
    *utf8 = buffer;
    *length = (size_t)byte_count - 1u;
    return true;
}

static void winxterm_dstcmd_highlight_dispose_patterns(WinxtermDstcmdHighlightPattern *patterns,
                                                       size_t count)
{
    if (patterns == 0) {
        return;
    }
    for (size_t i = 0u; i < count; ++i) {
        free(patterns[i].text);
    }
    free(patterns);
}

static bool winxterm_dstcmd_highlight_parse_patterns(const WinxtermDstcmdArgv *argv,
                                                     bool *case_insensitive,
                                                     WinxtermDstcmdHighlightPattern **patterns,
                                                     size_t *pattern_count,
                                                     size_t *max_pattern_length)
{
    if (argv == 0 || case_insensitive == 0 || patterns == 0 ||
        pattern_count == 0 || max_pattern_length == 0) {
        return false;
    }
    *case_insensitive = false;
    *patterns = 0;
    *pattern_count = 0u;
    *max_pattern_length = 0u;

    int pattern_start = 1;
    if (argv->count > 1 && argv->items[1] != 0 && wcscmp(argv->items[1], L"-i") == 0) {
        *case_insensitive = true;
        pattern_start = 2;
    }
    if (argv->count <= pattern_start) {
        return false;
    }

    size_t count = (size_t)(argv->count - pattern_start);
    WinxtermDstcmdHighlightPattern *parsed =
        (WinxtermDstcmdHighlightPattern *)calloc(count, sizeof(*parsed));
    if (parsed == 0) {
        return false;
    }

    for (size_t i = 0u; i < count; ++i) {
        const wchar_t *source = argv->items[pattern_start + (int)i];
        if (source != 0 && wcscmp(source, L"\\-i") == 0) {
            source = L"-i";
        }
        if (!winxterm_dstcmd_highlight_wide_to_utf8(source, &parsed[i].text, &parsed[i].length) ||
            parsed[i].length == 0u) {
            winxterm_dstcmd_highlight_dispose_patterns(parsed, count);
            return false;
        }
        if (parsed[i].length > *max_pattern_length) {
            *max_pattern_length = parsed[i].length;
        }
    }

    *patterns = parsed;
    *pattern_count = count;
    return true;
}

static uint8_t winxterm_dstcmd_highlight_ascii_lower(uint8_t ch)
{
    if (ch >= (uint8_t)'A' && ch <= (uint8_t)'Z') {
        return (uint8_t)(ch + ((uint8_t)'a' - (uint8_t)'A'));
    }
    return ch;
}

static bool winxterm_dstcmd_highlight_byte_equal(uint8_t left,
                                                 uint8_t right,
                                                 bool case_insensitive)
{
    if (!case_insensitive) {
        return left == right;
    }
    return winxterm_dstcmd_highlight_ascii_lower(left) ==
           winxterm_dstcmd_highlight_ascii_lower(right);
}

static bool winxterm_dstcmd_highlight_match_at(const uint8_t *bytes,
                                               size_t byte_count,
                                               size_t offset,
                                               const WinxtermDstcmdHighlightPattern *pattern,
                                               bool case_insensitive)
{
    if (bytes == 0 || pattern == 0 || pattern->text == 0 ||
        offset + pattern->length > byte_count) {
        return false;
    }
    for (size_t i = 0u; i < pattern->length; ++i) {
        if (!winxterm_dstcmd_highlight_byte_equal(bytes[offset + i],
                                                  (uint8_t)pattern->text[i],
                                                  case_insensitive)) {
            return false;
        }
    }
    return true;
}

static void winxterm_dstcmd_highlight_mark_matches(const uint8_t *bytes,
                                                   uint8_t *marks,
                                                   size_t byte_count,
                                                   const WinxtermDstcmdHighlightPattern *patterns,
                                                   size_t pattern_count,
                                                   bool case_insensitive)
{
    if (bytes == 0 || marks == 0 || patterns == 0) {
        return;
    }
    for (size_t pattern_index = 0u; pattern_index < pattern_count; ++pattern_index) {
        const WinxtermDstcmdHighlightPattern *pattern = patterns + pattern_index;
        if (pattern->length == 0u || pattern->length > byte_count) {
            continue;
        }
        uint8_t color = (uint8_t)(pattern_index % 8u) + 1u;
        for (size_t offset = 0u; offset + pattern->length <= byte_count; ++offset) {
            if (!winxterm_dstcmd_highlight_match_at(bytes,
                                                    byte_count,
                                                    offset,
                                                    pattern,
                                                    case_insensitive)) {
                continue;
            }
            for (size_t i = 0u; i < pattern->length; ++i) {
                if (marks[offset + i] == 0u) {
                    marks[offset + i] = color;
                }
            }
        }
    }
}

static bool winxterm_dstcmd_highlight_write_bytes(WinxtermDstcmdShell *shell,
                                                  const uint8_t *bytes,
                                                  size_t byte_count)
{
    if (byte_count == 0u) {
        return true;
    }
    return winxterm_dstcmd_shell_write_bytes(shell, bytes, byte_count);
}

static bool winxterm_dstcmd_highlight_emit_marked(WinxtermDstcmdShell *shell,
                                                  const uint8_t *bytes,
                                                  const uint8_t *marks,
                                                  size_t byte_count,
                                                  uint8_t *active_color,
                                                  bool final)
{
    if (shell == 0 || bytes == 0 || marks == 0 || active_color == 0) {
        return false;
    }
    size_t offset = 0u;
    while (offset < byte_count) {
        uint8_t color = marks[offset];
        if (color == 0u) {
            if (*active_color != 0u && !winxterm_dstcmd_shell_write_utf8(shell, "\x1b[0m")) {
                return false;
            }
            *active_color = 0u;
        } else if (*active_color != color) {
            if (!winxterm_dstcmd_shell_write_utf8(shell,
                                                  winxterm_dstcmd_highlight_color((size_t)color - 1u))) {
                return false;
            }
            *active_color = color;
        }

        size_t end = offset + 1u;
        while (end < byte_count && marks[end] == color) {
            ++end;
        }
        if (!winxterm_dstcmd_highlight_write_bytes(shell, bytes + offset, end - offset)) {
            return false;
        }
        offset = end;
    }
    if (final && *active_color != 0u) {
        if (!winxterm_dstcmd_shell_write_utf8(shell, "\x1b[0m")) {
            return false;
        }
        *active_color = 0u;
    }
    return true;
}

static bool winxterm_dstcmd_highlight_append_pending(uint8_t **pending,
                                                     uint8_t **marks,
                                                     size_t *capacity,
                                                     size_t *count,
                                                     const uint8_t *chunk,
                                                     size_t chunk_count)
{
    if (pending == 0 || marks == 0 || capacity == 0 || count == 0 ||
        (chunk == 0 && chunk_count != 0u)) {
        return false;
    }
    if (*count + chunk_count > *capacity) {
        size_t needed = *count + chunk_count;
        size_t new_capacity = *capacity == 0u ? 2048u : *capacity;
        while (new_capacity < needed) {
            if (new_capacity > SIZE_MAX / 2u) {
                return false;
            }
            new_capacity *= 2u;
        }
        uint8_t *new_pending = (uint8_t *)malloc(new_capacity);
        uint8_t *new_marks = (uint8_t *)malloc(new_capacity);
        if (new_pending == 0 || new_marks == 0) {
            free(new_marks);
            free(new_pending);
            return false;
        }
        if (*count != 0u) {
            memcpy(new_pending, *pending, *count);
            memcpy(new_marks, *marks, *count);
        }
        free(*pending);
        free(*marks);
        *pending = new_pending;
        *marks = new_marks;
        *capacity = new_capacity;
    }
    if (chunk_count != 0u) {
        memcpy(*pending + *count, chunk, chunk_count);
        memset(*marks + *count, 0, chunk_count);
        *count += chunk_count;
    }
    return true;
}

static void winxterm_dstcmd_highlight_drop_pending_prefix(uint8_t *pending,
                                                          uint8_t *marks,
                                                          size_t *count,
                                                          size_t prefix_count)
{
    if (pending == 0 || marks == 0 || count == 0 || prefix_count == 0u) {
        return;
    }
    if (prefix_count >= *count) {
        *count = 0u;
        return;
    }
    memmove(pending, pending + prefix_count, *count - prefix_count);
    memmove(marks, marks + prefix_count, *count - prefix_count);
    *count -= prefix_count;
}

static bool winxterm_dstcmd_highlight_write_prelude(WinxtermDstcmdShell *shell,
                                                    const WinxtermDstcmdHighlightPattern *patterns,
                                                    size_t pattern_count)
{
    if (shell == 0 || patterns == 0) {
        return false;
    }
    if (!winxterm_dstcmd_shell_write_utf8(shell, "Highlighting")) {
        return false;
    }
    for (size_t i = 0u; i < pattern_count; ++i) {
        if (!winxterm_dstcmd_shell_write_utf8(shell, " ") ||
            !winxterm_dstcmd_shell_write_utf8(shell, winxterm_dstcmd_highlight_color(i)) ||
            !winxterm_dstcmd_shell_write_utf8(shell, patterns[i].text) ||
            !winxterm_dstcmd_shell_write_utf8(shell, "\x1b[0m")) {
            return false;
        }
    }
    return winxterm_dstcmd_shell_write_utf8(shell, "\r\n");
}

static bool winxterm_dstcmd_highlight_process(WinxtermDstcmdShell *shell,
                                              const WinxtermDstcmdHighlightPattern *patterns,
                                              size_t pattern_count,
                                              size_t max_pattern_length,
                                              bool case_insensitive)
{
    uint8_t *pending = 0;
    uint8_t *marks = 0;
    size_t pending_capacity = 0u;
    size_t pending_count = 0u;
    uint8_t chunk[WINXTERM_DSTCMD_HIGHLIGHT_CHUNK_SIZE];
    size_t keep_count = max_pattern_length > 0u ? max_pattern_length - 1u : 0u;
    uint8_t active_color = 0u;
    bool ok = true;

    for (;;) {
        size_t read_count = winxterm_dstcmd_shell_read_input(shell, chunk, sizeof(chunk), true);
        if (read_count == 0u) {
            break;
        }
        if (!winxterm_dstcmd_highlight_append_pending(&pending,
                                                      &marks,
                                                      &pending_capacity,
                                                      &pending_count,
                                                      chunk,
                                                      read_count)) {
            ok = false;
            break;
        }
        winxterm_dstcmd_highlight_mark_matches(pending,
                                               marks,
                                               pending_count,
                                               patterns,
                                               pattern_count,
                                               case_insensitive);
        if (pending_count > keep_count) {
            size_t flush_count = pending_count - keep_count;
            if (!winxterm_dstcmd_highlight_emit_marked(shell,
                                                       pending,
                                                       marks,
                                                       flush_count,
                                                       &active_color,
                                                       false)) {
                ok = false;
                break;
            }
            winxterm_dstcmd_highlight_drop_pending_prefix(pending,
                                                          marks,
                                                          &pending_count,
                                                          flush_count);
        }
    }

    if (ok && pending_count != 0u) {
        winxterm_dstcmd_highlight_mark_matches(pending,
                                               marks,
                                               pending_count,
                                               patterns,
                                               pattern_count,
                                               case_insensitive);
        ok = winxterm_dstcmd_highlight_emit_marked(shell,
                                                   pending,
                                                   marks,
                                                   pending_count,
                                                   &active_color,
                                                   true);
    } else if (ok && active_color != 0u) {
        ok = winxterm_dstcmd_shell_write_utf8(shell, "\x1b[0m");
    }

    free(marks);
    free(pending);
    return ok;
}

int winxterm_dstcmd_cmd_highlight(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    if (shell == 0 || argv == 0) {
        return 1;
    }

    bool case_insensitive = false;
    WinxtermDstcmdHighlightPattern *patterns = 0;
    size_t pattern_count = 0u;
    size_t max_pattern_length = 0u;
    if (!winxterm_dstcmd_highlight_parse_patterns(argv,
                                                  &case_insensitive,
                                                  &patterns,
                                                  &pattern_count,
                                                  &max_pattern_length)) {
        (void)winxterm_dstcmd_shell_write_utf8(shell, WINXTERM_DSTCMD_HIGHLIGHT_USAGE);
        return 2;
    }

    bool ok = winxterm_dstcmd_highlight_write_prelude(shell, patterns, pattern_count) &&
              winxterm_dstcmd_highlight_process(shell,
                                                patterns,
                                                pattern_count,
                                                max_pattern_length,
                                                case_insensitive);
    winxterm_dstcmd_highlight_dispose_patterns(patterns, pattern_count);
    return ok ? 0 : 1;
}
