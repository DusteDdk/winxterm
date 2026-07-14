#include "dstcmd/winxterm_dstcmd.h"

#include "dstcmd/api/env.h"
#include "dstcmd/api/path.h"
#include "dstcmd/api/unicode.h"
#include "dstcmd/dispatch.h"
#include "dstcmd/winxterm_dstcmd_exec.h"
#include "dstcmd/winxterm_dstcmd_selector.h"
#include "dstcmd/winxterm_dstcmd_parse.h"

#include "sqlite3.h"

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WINXTERM_DSTCMD_PROMPT_COMMAND_PREFIX_COLUMNS 2u
#define WINXTERM_DSTCMD_PROMPT_COLOR_RESET L"\x1b[0m"
#define WINXTERM_DSTCMD_PROMPT_COLOR_TIME L"\x1b[38;2;255;255;0m"
#define WINXTERM_DSTCMD_PROMPT_COLOR_GREEN L"\x1b[38;2;0;255;0m"
#define WINXTERM_DSTCMD_PROMPT_COLOR_DARK_GREEN L"\x1b[38;2;0;128;0m"
#define WINXTERM_DSTCMD_PROMPT_COLOR_RED L"\x1b[38;2;255;0;0m"
#define WINXTERM_DSTCMD_PROMPT_COLOR_WHITE L"\x1b[38;2;255;255;255m"
#define WINXTERM_DSTCMD_PROMPT_COLOR_BLUE L"\x1b[38;2;0;128;255m"
#define WINXTERM_DSTCMD_COMPLETION_HIGHLIGHT_ON L"\x1b[1;48;2;0;0;0m"
#define WINXTERM_DSTCMD_COMPLETION_HIGHLIGHT_OFF L"\x1b[22;49m"
#define WINXTERM_DSTCMD_COMPLETION_COLOR_BUILTIN L"\x1b[38;2;255;165;0m"
#define WINXTERM_DSTCMD_COMPLETION_COLOR_PATH L"\x1b[38;2;192;192;192m"
#define WINXTERM_DSTCMD_OUTPUT_WRITE_CHUNK_BYTES (64u * 1024u)
#define WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_ON "\x1b[1;48;2;0;0;0m"
#define WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_OFF "\x1b[22;49m"
#define WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_CYAN "\x1b[38;2;0;255;255;48;2;0;0;0m"
#define WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_BLUE "\x1b[38;2;0;128;255;48;2;0;0;0m"
#define WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_GREEN "\x1b[38;2;0;255;0;48;2;0;0;0m"
#define WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_YELLOW "\x1b[38;2;255;255;0;48;2;0;0;0m"
#define WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_RESET "\x1b[0m"
#define WINXTERM_DSTCMD_SMOKE_HISTORY_STYLE "\x1b[38;2;235;245;255;48;2;0;0;64m"
#define WINXTERM_DSTCMD_SMOKE_HISTORY_SELECTED_STYLE "\x1b[38;2;255;255;0;48;2;0;0;0m"
#define WINXTERM_DSTCMD_TITLE_MAX_COLUMNS 240u
#define WINXTERM_DSTCMD_TITLE_PREFIX_COMPONENTS 4u
#define WINXTERM_DSTCMD_TITLE_ELLIPSIS L"...."
#define WINXTERM_DSTCMD_TITLE_ELLIPSIS_COLUMNS 4u
#define WINXTERM_DSTCMD_TITLE_MIN_COMPONENT_COLUMNS 5u
#define WINXTERM_DSTCMD_HISTORY_DB_RETRY_ATTEMPTS 10u
#define WINXTERM_DSTCMD_HISTORY_DB_RETRY_INTERVAL_MS 200u
#define WINXTERM_DSTCMD_HISTORY_DB_BUSY_TIMEOUT_MS 1
#define WINXTERM_DSTCMD_HISTORY_SEARCH_MAX_TERMS 16u
#define WINXTERM_DSTCMD_HISTORY_SEARCH_OVERLAY_ROWS 5u
#define WINXTERM_DSTCMD_HISTORY_SEARCH_STYLE "\x1b[38;2;235;245;255;48;2;0;0;64m"
#define WINXTERM_DSTCMD_HISTORY_SEARCH_SELECTED_STYLE "\x1b[38;2;255;255;0;48;2;0;0;0m"
#define WINXTERM_DSTCMD_HISTORY_SEARCH_RESET "\x1b[0m"

static bool winxterm_dstcmd_utf8_to_wide(const char *line, wchar_t **wide_line);
static bool winxterm_dstcmd_shell_clear_rendered_prompt(WinxtermDstcmdShell *shell);

void winxterm_dstcmd_output_builder_init(WinxtermDstcmdOutputBuilder *builder)
{
    if (builder != 0) {
        memset(builder, 0, sizeof(*builder));
    }
}

void winxterm_dstcmd_output_builder_dispose(WinxtermDstcmdOutputBuilder *builder)
{
    if (builder == 0) {
        return;
    }
    free(builder->chars);
    memset(builder, 0, sizeof(*builder));
}

static bool winxterm_dstcmd_output_builder_reserve(WinxtermDstcmdOutputBuilder *builder,
                                                   size_t additional)
{
    if (builder == 0 || builder->failed) {
        return false;
    }
    if (additional > ((size_t)-1) - builder->count - 1u) {
        builder->failed = true;
        return false;
    }

    size_t needed = builder->count + additional + 1u;
    if (needed <= builder->capacity) {
        return true;
    }

    size_t new_capacity = builder->capacity == 0u ? 256u : builder->capacity;
    while (new_capacity < needed) {
        if (new_capacity > ((size_t)-1) / 2u) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2u;
    }

    wchar_t *new_chars = (wchar_t *)realloc(builder->chars, new_capacity * sizeof(*new_chars));
    if (new_chars == 0) {
        builder->failed = true;
        return false;
    }
    builder->chars = new_chars;
    builder->capacity = new_capacity;
    builder->chars[builder->count] = L'\0';
    return true;
}

bool winxterm_dstcmd_output_builder_append_wide(WinxtermDstcmdOutputBuilder *builder,
                                                const wchar_t *text)
{
    if (text == 0 || text[0] == L'\0') {
        return builder != 0 && !builder->failed;
    }
    size_t length = wcslen(text);
    if (!winxterm_dstcmd_output_builder_reserve(builder, length)) {
        return false;
    }
    memcpy(builder->chars + builder->count, text, length * sizeof(*builder->chars));
    builder->count += length;
    builder->chars[builder->count] = L'\0';
    return true;
}

bool winxterm_dstcmd_output_builder_append_widef(WinxtermDstcmdOutputBuilder *builder,
                                                 const wchar_t *format,
                                                 ...)
{
    if (builder == 0 || builder->failed || format == 0) {
        return false;
    }

    va_list args;
    va_start(args, format);
    int needed = _vscwprintf(format, args);
    va_end(args);
    if (needed < 0) {
        builder->failed = true;
        return false;
    }
    if (needed == 0) {
        return true;
    }
    if (!winxterm_dstcmd_output_builder_reserve(builder, (size_t)needed)) {
        return false;
    }

    va_start(args, format);
    int written = vswprintf_s(builder->chars + builder->count,
                              builder->capacity - builder->count,
                              format,
                              args);
    va_end(args);
    if (written < 0) {
        builder->failed = true;
        return false;
    }
    builder->count += (size_t)written;
    return true;
}

bool winxterm_dstcmd_output_builder_append_repeat(WinxtermDstcmdOutputBuilder *builder,
                                                  wchar_t ch,
                                                  size_t count)
{
    if (count == 0u) {
        return builder != 0 && !builder->failed;
    }
    if (!winxterm_dstcmd_output_builder_reserve(builder, count)) {
        return false;
    }
    for (size_t i = 0u; i < count; ++i) {
        builder->chars[builder->count + i] = ch;
    }
    builder->count += count;
    builder->chars[builder->count] = L'\0';
    return true;
}

bool winxterm_dstcmd_output_builder_flush(WinxtermDstcmdOutputBuilder *builder,
                                          WinxtermDstcmdShell *shell)
{
    if (builder == 0 || builder->failed) {
        return false;
    }
    if (builder->count == 0u) {
        return true;
    }
    return winxterm_dstcmd_shell_write_wide(shell, builder->chars);
}

static bool winxterm_dstcmd_shell_capture_append(WinxtermDstcmdShell *shell,
                                                 const uint8_t *bytes,
                                                 size_t byte_count)
{
    if (shell == 0 || bytes == 0 || byte_count == 0u) {
        return true;
    }
    if (shell->capture_count + byte_count < shell->capture_count) {
        return false;
    }
    size_t needed = shell->capture_count + byte_count;
    if (needed > shell->capture_capacity) {
        size_t new_capacity = shell->capture_capacity == 0u ? 4096u : shell->capture_capacity;
        while (new_capacity < needed) {
            if (new_capacity > ((size_t)-1) / 2u) {
                new_capacity = needed;
                break;
            }
            new_capacity *= 2u;
        }
        uint8_t *new_bytes = (uint8_t *)realloc(shell->capture_bytes, new_capacity);
        if (new_bytes == 0) {
            shell->capture_failed = true;
            return false;
        }
        shell->capture_bytes = new_bytes;
        shell->capture_capacity = new_capacity;
    }
    memcpy(shell->capture_bytes + shell->capture_count, bytes, byte_count);
    shell->capture_count += byte_count;
    return true;
}

static bool winxterm_dstcmd_shell_write_handle_bytes(HANDLE output,
                                                     const uint8_t *bytes,
                                                     size_t byte_count)
{
    if (output == 0 || output == INVALID_HANDLE_VALUE) {
        return false;
    }
    size_t offset = 0u;
    while (offset < byte_count) {
        DWORD chunk = byte_count - offset > WINXTERM_DSTCMD_OUTPUT_WRITE_CHUNK_BYTES ?
            (DWORD)WINXTERM_DSTCMD_OUTPUT_WRITE_CHUNK_BYTES : (DWORD)(byte_count - offset);
        DWORD written = 0;
        if (!WriteFile(output, bytes + offset, chunk, &written, 0) || written == 0u) {
            return false;
        }
        offset += (size_t)written;
    }
    return true;
}

bool winxterm_dstcmd_shell_write_bytes(WinxtermDstcmdShell *shell,
                                       const uint8_t *bytes,
                                       size_t byte_count)
{
    if (shell == 0 || bytes == 0 || byte_count == 0u) {
        return true;
    }
    if (shell->stream_output_handle != 0 && shell->stream_output_handle != INVALID_HANDLE_VALUE) {
        if (!winxterm_dstcmd_shell_write_handle_bytes(shell->stream_output_handle,
                                                      bytes,
                                                      byte_count)) {
            shell->stream_output_failed = true;
            return false;
        }
        return true;
    }
    if (shell->capture_active) {
        return winxterm_dstcmd_shell_capture_append(shell, bytes, byte_count);
    }
    if (shell->output_lock_initialized) {
        EnterCriticalSection(&shell->output_lock);
    }
    if (shell->timing_verbose_active) {
        winxterm_diag_inc_u64(&shell->timing_diagnostics.shell_write_calls);
        winxterm_diag_add_u64(&shell->timing_diagnostics.builtin_output_bytes, (uint64_t)byte_count);
        winxterm_diag_add_u64(&shell->timing_diagnostics.total_output_bytes, (uint64_t)byte_count);
    }
    HANDLE output = shell->output_handle != 0 && shell->output_handle != INVALID_HANDLE_VALUE ?
        shell->output_handle : GetStdHandle(STD_OUTPUT_HANDLE);
    bool ok = winxterm_dstcmd_shell_write_handle_bytes(output, bytes, byte_count);
    if (shell->output_lock_initialized) {
        LeaveCriticalSection(&shell->output_lock);
    }
    return ok;
}

bool winxterm_dstcmd_shell_write_utf8(WinxtermDstcmdShell *shell, const char *text)
{
    if (text == 0) {
        return true;
    }
    return winxterm_dstcmd_shell_write_bytes(shell, (const uint8_t *)text, strlen(text));
}

bool winxterm_dstcmd_shell_write_wide(WinxtermDstcmdShell *shell, const wchar_t *text)
{
    if (text == 0) {
        return true;
    }
    int byte_count = WideCharToMultiByte(CP_UTF8, 0, text, -1, 0, 0, 0, 0);
    if (byte_count <= 0) {
        return false;
    }
    char *utf8 = (char *)calloc((size_t)byte_count, sizeof(*utf8));
    if (utf8 == 0) {
        return false;
    }
    bool ok = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, byte_count, 0, 0) > 0 &&
              winxterm_dstcmd_shell_write_utf8(shell, utf8);
    free(utf8);
    return ok;
}

bool winxterm_dstcmd_shell_write_widef(WinxtermDstcmdShell *shell, const wchar_t *format, ...)
{
    if (format == 0) {
        return true;
    }
    va_list args;
    va_start(args, format);
    int needed = _vscwprintf(format, args);
    va_end(args);
    if (needed < 0) {
        return false;
    }
    wchar_t *buffer = (wchar_t *)calloc((size_t)needed + 1u, sizeof(*buffer));
    if (buffer == 0) {
        return false;
    }
    va_start(args, format);
    int written = vswprintf_s(buffer, (size_t)needed + 1u, format, args);
    va_end(args);
    bool ok = written >= 0 && winxterm_dstcmd_shell_write_wide(shell, buffer);
    free(buffer);
    return ok;
}

static bool winxterm_dstcmd_shell_write_error_bytes(WinxtermDstcmdShell *shell,
                                                    const uint8_t *bytes,
                                                    size_t byte_count)
{
    if (shell == 0 || bytes == 0 || byte_count == 0u) {
        return true;
    }
    if (shell->output_lock_initialized) {
        EnterCriticalSection(&shell->output_lock);
    }
    HANDLE output = shell->error_handle != 0 && shell->error_handle != INVALID_HANDLE_VALUE ?
        shell->error_handle :
        (shell->output_handle != 0 && shell->output_handle != INVALID_HANDLE_VALUE ?
            shell->output_handle : GetStdHandle(STD_ERROR_HANDLE));
    bool ok = winxterm_dstcmd_shell_write_handle_bytes(output, bytes, byte_count);
    if (shell->output_lock_initialized) {
        LeaveCriticalSection(&shell->output_lock);
    }
    return ok;
}

bool winxterm_dstcmd_shell_write_error_wide(WinxtermDstcmdShell *shell, const wchar_t *text)
{
    if (text == 0) {
        return true;
    }
    int byte_count = WideCharToMultiByte(CP_UTF8, 0, text, -1, 0, 0, 0, 0);
    if (byte_count <= 0) {
        return false;
    }
    char *utf8 = (char *)calloc((size_t)byte_count, sizeof(*utf8));
    if (utf8 == 0) {
        return false;
    }
    bool ok = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, byte_count, 0, 0) > 0 &&
              winxterm_dstcmd_shell_write_error_bytes(shell,
                                                      (const uint8_t *)utf8,
                                                      (size_t)byte_count - 1u);
    free(utf8);
    return ok;
}

bool winxterm_dstcmd_shell_write_error_widef(WinxtermDstcmdShell *shell,
                                             const wchar_t *format,
                                             ...)
{
    if (format == 0) {
        return true;
    }
    va_list args;
    va_start(args, format);
    int needed = _vscwprintf(format, args);
    va_end(args);
    if (needed < 0) {
        return false;
    }
    wchar_t *buffer = (wchar_t *)calloc((size_t)needed + 1u, sizeof(*buffer));
    if (buffer == 0) {
        return false;
    }
    va_start(args, format);
    int written = vswprintf_s(buffer, (size_t)needed + 1u, format, args);
    va_end(args);
    bool ok = written >= 0 && winxterm_dstcmd_shell_write_error_wide(shell, buffer);
    free(buffer);
    return ok;
}

static bool winxterm_dstcmd_title_is_slash(wchar_t ch)
{
    return ch == L'\\' || ch == L'/';
}

static size_t winxterm_dstcmd_wide_range_columns(const wchar_t *text, size_t start, size_t end)
{
    if (text == 0 || end <= start) {
        return 0u;
    }
    size_t columns = 0u;
    size_t offset = start;
    while (offset < end) {
        uint32_t codepoint = 0u;
        size_t before = offset;
        (void)winxterm_dstcmd_wide_decode_next(text, end, &offset, &codepoint);
        if (offset <= before) {
            offset = before + 1u;
        }
        int width = winxterm_dstcmd_codepoint_width(codepoint);
        if (width > 0) {
            columns += (size_t)width;
        }
    }
    return columns;
}

static size_t winxterm_dstcmd_title_columns(const wchar_t *text)
{
    return text != 0 ? winxterm_dstcmd_wide_range_columns(text, 0u, wcslen(text)) : 0u;
}

static bool winxterm_dstcmd_shell_write_wide_range(WinxtermDstcmdShell *shell,
                                                   const wchar_t *text,
                                                   size_t start,
                                                   size_t end)
{
    if (text == 0 || end <= start) {
        return true;
    }
    size_t length = end - start;
    wchar_t *buffer = (wchar_t *)malloc((length + 1u) * sizeof(*buffer));
    if (buffer == 0) {
        return false;
    }
    memcpy(buffer, text + start, length * sizeof(*buffer));
    buffer[length] = L'\0';
    bool ok = winxterm_dstcmd_shell_write_wide(shell, buffer);
    free(buffer);
    return ok;
}

static size_t winxterm_dstcmd_title_room(const WinxtermDstcmdShell *shell)
{
    (void)shell;
    return WINXTERM_DSTCMD_TITLE_MAX_COLUMNS;
}

static size_t winxterm_dstcmd_title_root_end(const wchar_t *path, size_t length)
{
    if (path == 0 || length == 0u) {
        return 0u;
    }
    if (length >= 3u &&
        path[1] == L':' &&
        winxterm_dstcmd_title_is_slash(path[2])) {
        return 3u;
    }
    if (winxterm_dstcmd_title_is_slash(path[0])) {
        if (length >= 2u && winxterm_dstcmd_title_is_slash(path[1])) {
            size_t parts = 0u;
            size_t i = 2u;
            while (i < length) {
                while (i < length && winxterm_dstcmd_title_is_slash(path[i])) {
                    ++i;
                }
                while (i < length && !winxterm_dstcmd_title_is_slash(path[i])) {
                    ++i;
                }
                if (i <= length) {
                    ++parts;
                    if (parts == 2u) {
                        while (i < length && winxterm_dstcmd_title_is_slash(path[i])) {
                            ++i;
                        }
                        return i;
                    }
                }
            }
        }
        return 1u;
    }
    return 0u;
}

static bool winxterm_dstcmd_title_find_component(const wchar_t *path,
                                                size_t *start,
                                                size_t *end)
{
    if (path == 0 || start == 0 || end == 0) {
        return false;
    }
    size_t length = wcslen(path);
    size_t root_end = winxterm_dstcmd_title_root_end(path, length);
    size_t index = 0u;
    size_t fallback_start = 0u;
    size_t fallback_end = 0u;
    size_t fallback_columns = 0u;
    size_t i = root_end;
    while (i < length) {
        while (i < length && winxterm_dstcmd_title_is_slash(path[i])) {
            ++i;
        }
        size_t component_start = i;
        while (i < length && !winxterm_dstcmd_title_is_slash(path[i])) {
            ++i;
        }
        size_t component_end = i;
        if (component_end > component_start) {
            size_t columns = winxterm_dstcmd_wide_range_columns(path, component_start, component_end);
            if (columns > fallback_columns) {
                fallback_start = component_start;
                fallback_end = component_end;
                fallback_columns = columns;
            }
            if (index >= WINXTERM_DSTCMD_TITLE_PREFIX_COMPONENTS &&
                columns > WINXTERM_DSTCMD_TITLE_MIN_COMPONENT_COLUMNS) {
                *start = component_start;
                *end = component_end;
                return true;
            }
            ++index;
        }
    }
    if (fallback_columns > WINXTERM_DSTCMD_TITLE_MIN_COMPONENT_COLUMNS) {
        *start = fallback_start;
        *end = fallback_end;
        return true;
    }
    return false;
}

static size_t winxterm_dstcmd_title_advance_boundaries(const wchar_t *text,
                                                       size_t start,
                                                       size_t end,
                                                       size_t count)
{
    size_t offset = start;
    for (size_t i = 0u; i < count && offset < end; ++i) {
        offset = winxterm_dstcmd_wide_next_boundary(text, end, offset);
    }
    return offset > end ? end : offset;
}

static size_t winxterm_dstcmd_title_retreat_boundaries(const wchar_t *text,
                                                       size_t start,
                                                       size_t end,
                                                       size_t count)
{
    size_t offset = end;
    for (size_t i = 0u; i < count && offset > start; ++i) {
        offset = winxterm_dstcmd_wide_prev_boundary(text, end, offset);
    }
    return offset < start ? start : offset;
}

static bool winxterm_dstcmd_title_truncate_component(wchar_t *path,
                                                     size_t path_capacity,
                                                     size_t start,
                                                     size_t end,
                                                     size_t target_columns)
{
    if (path == 0 || path_capacity == 0u || start >= end) {
        return false;
    }
    size_t current_columns = winxterm_dstcmd_wide_range_columns(path, start, end);
    if (target_columns >= current_columns) {
        return false;
    }
    if (target_columns <= WINXTERM_DSTCMD_TITLE_ELLIPSIS_COLUMNS) {
        target_columns = WINXTERM_DSTCMD_TITLE_ELLIPSIS_COLUMNS + 1u;
    }
    size_t visible_columns = target_columns - WINXTERM_DSTCMD_TITLE_ELLIPSIS_COLUMNS;
    size_t prefix_columns = visible_columns > 4u ? 4u : visible_columns / 2u;
    size_t suffix_columns = visible_columns - prefix_columns;
    if (prefix_columns == 0u && visible_columns != 0u) {
        prefix_columns = 1u;
        --suffix_columns;
    }

    wchar_t replacement[WINXTERM_DSTCMD_PATH_CAPACITY];
    size_t offset = 0u;
    size_t prefix_end = winxterm_dstcmd_title_advance_boundaries(path, start, end, prefix_columns);
    size_t suffix_start = winxterm_dstcmd_title_retreat_boundaries(path, start, end, suffix_columns);
    if (suffix_start < prefix_end) {
        suffix_start = prefix_end;
    }
    size_t prefix_count = prefix_end - start;
    size_t suffix_count = end - suffix_start;
    if (prefix_count + WINXTERM_DSTCMD_TITLE_ELLIPSIS_COLUMNS + suffix_count >=
        WINXTERM_DSTCMD_PATH_CAPACITY) {
        return false;
    }
    memcpy(replacement + offset, path + start, prefix_count * sizeof(*replacement));
    offset += prefix_count;
    memcpy(replacement + offset,
           WINXTERM_DSTCMD_TITLE_ELLIPSIS,
           WINXTERM_DSTCMD_TITLE_ELLIPSIS_COLUMNS * sizeof(*replacement));
    offset += WINXTERM_DSTCMD_TITLE_ELLIPSIS_COLUMNS;
    memcpy(replacement + offset, path + suffix_start, suffix_count * sizeof(*replacement));
    offset += suffix_count;
    replacement[offset] = L'\0';

    size_t path_length = wcslen(path);
    size_t tail_count = path_length - end + 1u;
    if (start + offset + tail_count > path_capacity) {
        return false;
    }
    memmove(path + start + offset, path + end, tail_count * sizeof(*path));
    memcpy(path + start, replacement, offset * sizeof(*path));
    return true;
}

static void winxterm_dstcmd_fit_cwd_title(WinxtermDstcmdShell *shell,
                                          wchar_t *title,
                                          size_t title_capacity)
{
    if (shell == 0 || title == 0 || title_capacity == 0u) {
        return;
    }
    size_t room = winxterm_dstcmd_title_room(shell);
    for (size_t i = 0u; i < 128u && winxterm_dstcmd_title_columns(title) > room; ++i) {
        size_t start = 0u;
        size_t end = 0u;
        if (!winxterm_dstcmd_title_find_component(title, &start, &end)) {
            break;
        }
        size_t current_columns = winxterm_dstcmd_wide_range_columns(title, start, end);
        size_t overflow = winxterm_dstcmd_title_columns(title) - room;
        size_t target_columns = current_columns > overflow ?
            current_columns - overflow : WINXTERM_DSTCMD_TITLE_MIN_COMPONENT_COLUMNS;
        if (target_columns < WINXTERM_DSTCMD_TITLE_MIN_COMPONENT_COLUMNS) {
            target_columns = WINXTERM_DSTCMD_TITLE_MIN_COMPONENT_COLUMNS;
        }
        if (!winxterm_dstcmd_title_truncate_component(title,
                                                      title_capacity,
                                                      start,
                                                      end,
                                                      target_columns)) {
            break;
        }
    }
}

bool winxterm_dstcmd_shell_set_title_wide(WinxtermDstcmdShell *shell, const wchar_t *title)
{
    if (shell == 0 || title == 0) {
        return false;
    }
    size_t title_length = wcslen(title);
    wchar_t *sanitized = (wchar_t *)calloc(title_length + 1u, sizeof(*sanitized));
    if (sanitized == 0) {
        return false;
    }
    for (size_t i = 0u; i < title_length; ++i) {
        wchar_t ch = title[i];
        sanitized[i] = (ch < 0x20 || ch == 0x7f) ? L' ' : ch;
    }
    int utf8_count = WideCharToMultiByte(CP_UTF8, 0, sanitized, -1, 0, 0, 0, 0);
    if (utf8_count <= 0) {
        free(sanitized);
        return false;
    }
    const char prefix[] = "\x1b]0;";
    size_t prefix_length = sizeof(prefix) - 1u;
    size_t sequence_length = prefix_length + (size_t)utf8_count + 1u;
    char *sequence = (char *)calloc(sequence_length + 1u, sizeof(*sequence));
    if (sequence == 0) {
        free(sanitized);
        return false;
    }
    memcpy(sequence, prefix, prefix_length);
    bool ok = WideCharToMultiByte(CP_UTF8,
                                  0,
                                  sanitized,
                                  -1,
                                  sequence + prefix_length,
                                  utf8_count,
                                  0,
                                  0) > 0;
    free(sanitized);
    if (ok) {
        sequence[prefix_length + (size_t)utf8_count - 1u] = '\x1b';
        sequence[prefix_length + (size_t)utf8_count] = '\\';
        ok = winxterm_dstcmd_shell_write_bytes(shell,
                                               (const uint8_t *)sequence,
                                               sequence_length);
    }
    free(sequence);
    return ok;
}

bool winxterm_dstcmd_shell_update_cwd_title(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    wchar_t title[WINXTERM_DSTCMD_PATH_CAPACITY];
    if (wcscpy_s(title, WINXTERM_DSTCMD_PATH_CAPACITY, shell->cwd) != 0) {
        return false;
    }
    winxterm_dstcmd_fit_cwd_title(shell, title, WINXTERM_DSTCMD_PATH_CAPACITY);
    return winxterm_dstcmd_shell_set_title_wide(shell, title);
}

static bool winxterm_dstcmd_console_handle_is_valid(HANDLE handle)
{
    return handle != 0 && handle != INVALID_HANDLE_VALUE;
}

static bool winxterm_dstcmd_shell_capture_console_mode(HANDLE handle,
                                                       DWORD *original_mode,
                                                       DWORD *shell_mode,
                                                       bool *saved)
{
    if (saved != 0) {
        *saved = false;
    }
    if (!winxterm_dstcmd_console_handle_is_valid(handle) ||
        original_mode == 0 || shell_mode == 0 || saved == 0) {
        return false;
    }
    DWORD mode = 0;
    if (!GetConsoleMode(handle, &mode)) {
        return false;
    }
    *original_mode = mode;
    *shell_mode = mode;
    *saved = true;
    return true;
}

static void winxterm_dstcmd_shell_capture_console_modes(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    (void)winxterm_dstcmd_shell_capture_console_mode(shell->input_handle,
                                                     &shell->original_input_console_mode,
                                                     &shell->shell_input_console_mode,
                                                     &shell->input_console_mode_saved);
    (void)winxterm_dstcmd_shell_capture_console_mode(shell->output_handle,
                                                     &shell->original_output_console_mode,
                                                     &shell->shell_output_console_mode,
                                                     &shell->output_console_mode_saved);
    (void)winxterm_dstcmd_shell_capture_console_mode(shell->error_handle,
                                                     &shell->original_error_console_mode,
                                                     &shell->shell_error_console_mode,
                                                     &shell->error_console_mode_saved);
}

static void winxterm_dstcmd_shell_select_input_line_editor_mode(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || !shell->input_console_mode_saved ||
        !winxterm_dstcmd_console_handle_is_valid(shell->input_handle)) {
        return;
    }
    DWORD original_mode = shell->original_input_console_mode;
    DWORD raw_mode = original_mode & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT);
    DWORD desired_mode = raw_mode | ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (desired_mode == original_mode || SetConsoleMode(shell->input_handle, desired_mode)) {
        shell->shell_input_console_mode = desired_mode;
        return;
    }
    if (raw_mode == original_mode || SetConsoleMode(shell->input_handle, raw_mode)) {
        shell->shell_input_console_mode = raw_mode;
        return;
    }
    shell->shell_input_console_mode = original_mode;
}

static void winxterm_dstcmd_shell_select_output_line_editor_mode(HANDLE handle,
                                                                DWORD original_mode,
                                                                DWORD *shell_mode,
                                                                bool saved)
{
    if (!saved || !winxterm_dstcmd_console_handle_is_valid(handle) || shell_mode == 0) {
        return;
    }
    DWORD desired_mode = original_mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (desired_mode == original_mode || SetConsoleMode(handle, desired_mode)) {
        *shell_mode = desired_mode;
        return;
    }
    *shell_mode = original_mode;
}

static void winxterm_dstcmd_shell_select_line_editor_modes(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    winxterm_dstcmd_shell_select_input_line_editor_mode(shell);
    winxterm_dstcmd_shell_select_output_line_editor_mode(shell->output_handle,
                                                        shell->original_output_console_mode,
                                                        &shell->shell_output_console_mode,
                                                        shell->output_console_mode_saved);
    winxterm_dstcmd_shell_select_output_line_editor_mode(shell->error_handle,
                                                        shell->original_error_console_mode,
                                                        &shell->shell_error_console_mode,
                                                        shell->error_console_mode_saved);
}

static void winxterm_dstcmd_shell_apply_console_mode(HANDLE handle, DWORD mode, bool saved)
{
    if (saved && winxterm_dstcmd_console_handle_is_valid(handle)) {
        (void)SetConsoleMode(handle, mode);
    }
}

void winxterm_dstcmd_shell_enter_line_editor_mode(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    winxterm_dstcmd_shell_apply_console_mode(shell->input_handle,
                                             shell->shell_input_console_mode,
                                             shell->input_console_mode_saved);
    winxterm_dstcmd_shell_apply_console_mode(shell->output_handle,
                                             shell->shell_output_console_mode,
                                             shell->output_console_mode_saved);
    winxterm_dstcmd_shell_apply_console_mode(shell->error_handle,
                                             shell->shell_error_console_mode,
                                             shell->error_console_mode_saved);
    shell->prompt_cursor_saved = false;
}

void winxterm_dstcmd_shell_enter_foreground_child_mode(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    winxterm_dstcmd_shell_apply_console_mode(shell->input_handle,
                                             shell->original_input_console_mode,
                                             shell->input_console_mode_saved);
    winxterm_dstcmd_shell_apply_console_mode(shell->output_handle,
                                             shell->original_output_console_mode,
                                             shell->output_console_mode_saved);
    winxterm_dstcmd_shell_apply_console_mode(shell->error_handle,
                                             shell->original_error_console_mode,
                                             shell->error_console_mode_saved);
    shell->prompt_cursor_saved = false;
}

void winxterm_dstcmd_shell_restore_original_console_modes(WinxtermDstcmdShell *shell)
{
    winxterm_dstcmd_shell_enter_foreground_child_mode(shell);
}

static void winxterm_dstcmd_shell_configure_console_modes(WinxtermDstcmdShell *shell)
{
    winxterm_dstcmd_shell_capture_console_modes(shell);
    winxterm_dstcmd_shell_select_line_editor_modes(shell);
    winxterm_dstcmd_shell_enter_line_editor_mode(shell);
}

size_t winxterm_dstcmd_shell_read_input(WinxtermDstcmdShell *shell,
                                        uint8_t *buffer,
                                        size_t buffer_capacity,
                                        bool wait)
{
    if (shell == 0 || buffer == 0 || buffer_capacity == 0u) {
        return 0u;
    }
    if (shell->pending_input_count != 0u) {
        size_t copied = shell->pending_input_count < buffer_capacity ?
            shell->pending_input_count : buffer_capacity;
        memcpy(buffer, shell->pending_input, copied);
        shell->pending_input_count -= copied;
        if (shell->pending_input_count != 0u) {
            memmove(shell->pending_input, shell->pending_input + copied, shell->pending_input_count);
        }
        return copied;
    }

    HANDLE input = shell->stream_input_handle != 0 &&
                   shell->stream_input_handle != INVALID_HANDLE_VALUE ?
        shell->stream_input_handle :
        (shell->input_handle != 0 && shell->input_handle != INVALID_HANDLE_VALUE ?
            shell->input_handle : GetStdHandle(STD_INPUT_HANDLE));
    if (input == 0 || input == INVALID_HANDLE_VALUE) {
        return 0u;
    }
    if (!wait) {
        DWORD available = 0;
        if (PeekNamedPipe(input, 0, 0, 0, &available, 0)) {
            if (available == 0u) {
                return 0u;
            }
            if ((size_t)available < buffer_capacity) {
                buffer_capacity = (size_t)available;
            }
        } else {
            if (GetFileType(input) != FILE_TYPE_CHAR ||
                WaitForSingleObject(input, 0) != WAIT_OBJECT_0) {
                return 0u;
            }
            if (buffer_capacity > 1u) {
                buffer_capacity = 1u;
            }
        }
    }
    DWORD read_count = 0;
    if (!ReadFile(input, buffer, (DWORD)buffer_capacity, &read_count, 0)) {
        return 0u;
    }
    return (size_t)read_count;
}

static int winxterm_dstcmd_overlay_terminal_columns(const WinxtermDstcmdShell *shell)
{
    HANDLE output = shell != 0 && shell->output_handle != 0 &&
        shell->output_handle != INVALID_HANDLE_VALUE ?
        shell->output_handle : GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (output != 0 && output != INVALID_HANDLE_VALUE &&
        GetConsoleScreenBufferInfo(output, &info)) {
        int columns = (int)(info.srWindow.Right - info.srWindow.Left + 1);
        if (columns > 0) {
            return columns;
        }
        if (info.dwSize.X > 0) {
            return (int)info.dwSize.X;
        }
    }
    return 80;
}

static int winxterm_dstcmd_shell_terminal_rows(const WinxtermDstcmdShell *shell)
{
    HANDLE output = shell != 0 && shell->output_handle != 0 &&
        shell->output_handle != INVALID_HANDLE_VALUE ?
        shell->output_handle : GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (output != 0 && output != INVALID_HANDLE_VALUE &&
        GetConsoleScreenBufferInfo(output, &info)) {
        int rows = (int)(info.srWindow.Bottom - info.srWindow.Top + 1);
        if (rows > 0) {
            return rows;
        }
        if (info.dwSize.Y > 0) {
            return (int)info.dwSize.Y;
        }
    }
    return 24;
}

uint64_t winxterm_dstcmd_shell_timestamp_ns(void)
{
    static LARGE_INTEGER frequency;
    if (frequency.QuadPart == 0) {
        (void)QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER counter;
    (void)QueryPerformanceCounter(&counter);
    if (frequency.QuadPart <= 0) {
        return 0u;
    }
    uint64_t ticks = (uint64_t)counter.QuadPart;
    uint64_t ticks_per_second = (uint64_t)frequency.QuadPart;
    uint64_t seconds = ticks / ticks_per_second;
    uint64_t remainder = ticks % ticks_per_second;
    return seconds * 1000000000ull + (remainder * 1000000000ull) / ticks_per_second;
}

void winxterm_dstcmd_shell_notify_async_error(WinxtermDstcmdShell *shell, const wchar_t *message)
{
    if (shell == 0 || message == 0 || !shell->output_lock_initialized) {
        return;
    }
    EnterCriticalSection(&shell->output_lock);
    bool write_to_prompt = !shell->disposing &&
                           shell->prompt_active &&
                           !shell->command_running &&
                           !shell->exit_requested;
    if (write_to_prompt) {
        (void)winxterm_dstcmd_shell_clear_rendered_prompt(shell);
        (void)winxterm_dstcmd_shell_write_wide(shell, message);
        (void)winxterm_dstcmd_shell_write_utf8(shell, "\r\n");
        (void)winxterm_dstcmd_shell_refresh_line(shell);
    }
    LeaveCriticalSection(&shell->output_lock);
    if (!write_to_prompt) {
        (void)winxterm_dstcmd_shell_write_wide(shell, message);
        (void)winxterm_dstcmd_shell_write_utf8(shell, "\r\n");
    }
}

bool winxterm_dstcmd_shell_notify_async_widef(WinxtermDstcmdShell *shell,
                                             const wchar_t *format,
                                             ...)
{
    if (shell == 0 || format == 0) {
        return false;
    }
    va_list args;
    va_start(args, format);
    int needed = _vscwprintf(format, args);
    va_end(args);
    if (needed < 0) {
        return false;
    }
    wchar_t *message = (wchar_t *)malloc(((size_t)needed + 1u) * sizeof(*message));
    if (message == 0) {
        return false;
    }
    va_start(args, format);
    int written = vswprintf_s(message, (size_t)needed + 1u, format, args);
    va_end(args);
    if (written < 0) {
        free(message);
        return false;
    }
    winxterm_dstcmd_shell_notify_async_error(shell, message);
    free(message);
    return true;
}

bool winxterm_dstcmd_shell_notify_async_output_widef(WinxtermDstcmdShell *shell,
                                                    const wchar_t *format,
                                                    ...)
{
    if (shell == 0 || format == 0 || !shell->output_lock_initialized) {
        return false;
    }
    va_list args;
    va_start(args, format);
    int needed = _vscwprintf(format, args);
    va_end(args);
    if (needed < 0) {
        return false;
    }
    wchar_t *message = (wchar_t *)malloc(((size_t)needed + 1u) * sizeof(*message));
    if (message == 0) {
        return false;
    }
    va_start(args, format);
    int written = vswprintf_s(message, (size_t)needed + 1u, format, args);
    va_end(args);
    if (written < 0) {
        free(message);
        return false;
    }

    EnterCriticalSection(&shell->output_lock);
    bool can_write = !shell->disposing && !shell->exit_requested;
    bool refresh_prompt = shell->prompt_active && !shell->command_running;
    if (can_write && refresh_prompt) {
        (void)winxterm_dstcmd_shell_clear_rendered_prompt(shell);
    } else if (can_write) {
        (void)winxterm_dstcmd_shell_write_utf8(shell, "\r\n");
    }
    if (can_write) {
        (void)winxterm_dstcmd_shell_write_wide(shell, message);
        if (message[0] == L'\0' || message[wcslen(message) - 1u] != L'\n') {
            (void)winxterm_dstcmd_shell_write_utf8(shell, "\r\n");
        }
        if (refresh_prompt) {
            (void)winxterm_dstcmd_shell_refresh_line(shell);
        }
    }
    LeaveCriticalSection(&shell->output_lock);
    free(message);
    return can_write;
}

static char *winxterm_dstcmd_strdup(const char *text)
{
    if (text == 0) {
        return 0;
    }
    size_t length = strlen(text);
    char *copy = (char *)calloc(length + 1u, sizeof(*copy));
    if (copy == 0) {
        return 0;
    }
    memcpy(copy, text, length + 1u);
    return copy;
}

typedef struct WinxtermDstcmdLinePosition {
    size_t row;
    size_t column;
} WinxtermDstcmdLinePosition;

static WinxtermDstcmdLinePosition winxterm_dstcmd_shell_line_position(const char *line,
                                                                      size_t byte_offset,
                                                                      int columns,
                                                                      size_t initial_column)
{
    WinxtermDstcmdLinePosition position = {0u, 0u};
    if (line == 0 || columns <= 0) {
        return position;
    }
    while (initial_column >= (size_t)columns) {
        ++position.row;
        initial_column -= (size_t)columns;
    }
    position.column = initial_column;
    size_t length = strlen(line);
    if (byte_offset > length) {
        byte_offset = length;
    }
    bool pending_wrap = false;
    size_t offset = 0u;
    while (offset < byte_offset) {
        uint32_t codepoint = 0u;
        size_t before = offset;
        (void)winxterm_dstcmd_utf8_decode_next(line, byte_offset, &offset, &codepoint);
        if (offset <= before) {
            offset = before + 1u;
        }
        if (codepoint == (uint32_t)'\n') {
            ++position.row;
            position.column = 0u;
            pending_wrap = false;
            continue;
        }
        int width = winxterm_dstcmd_codepoint_width(codepoint);
        if (width <= 0) {
            continue;
        }
        if (pending_wrap) {
            ++position.row;
            position.column = 0u;
            pending_wrap = false;
        }
        if (width == 2 && position.column + 1u >= (size_t)columns) {
            ++position.row;
            position.column = 0u;
        }
        if (position.column + (size_t)width - 1u >= (size_t)columns - 1u) {
            pending_wrap = true;
        } else {
            position.column += (size_t)width;
        }
    }
    return position;
}

static bool winxterm_dstcmd_shell_write_prompt_cwd_range(WinxtermDstcmdShell *shell,
                                                         const wchar_t *cwd_color,
                                                         const wchar_t *cwd,
                                                         size_t start,
                                                         size_t end)
{
    if (end <= start) {
        return true;
    }
    return winxterm_dstcmd_shell_write_wide(shell, cwd_color) &&
           winxterm_dstcmd_shell_write_wide_range(shell, cwd, start, end);
}

static bool winxterm_dstcmd_shell_write_prompt_prefix(WinxtermDstcmdShell *shell,
                                                      const SYSTEMTIME *now,
                                                      const wchar_t *username)
{
    return now != 0 &&
           winxterm_dstcmd_shell_write_widef(shell,
                                             WINXTERM_DSTCMD_PROMPT_COLOR_TIME
                                             L"[%02u:%02u:%02u]"
                                             WINXTERM_DSTCMD_PROMPT_COLOR_RESET
                                             L" "
                                             WINXTERM_DSTCMD_PROMPT_COLOR_GREEN
                                             L"%ls"
                                             WINXTERM_DSTCMD_PROMPT_COLOR_RESET
                                             L" ",
                                             (unsigned int)now->wHour,
                                             (unsigned int)now->wMinute,
                                             (unsigned int)now->wSecond,
                                             username);
}

static bool winxterm_dstcmd_shell_write_prompt_command_prefix(WinxtermDstcmdShell *shell)
{
    return winxterm_dstcmd_shell_write_wide(shell,
                                            WINXTERM_DSTCMD_PROMPT_COLOR_RED
                                            L"$"
                                            WINXTERM_DSTCMD_PROMPT_COLOR_RESET
                                            L" "
                                            WINXTERM_DSTCMD_PROMPT_COLOR_WHITE);
}

static bool winxterm_dstcmd_shell_write_prompt(WinxtermDstcmdShell *shell,
                                               const SYSTEMTIME *now,
                                               const wchar_t *username,
                                               const wchar_t *cwd_color,
                                               int columns,
                                               size_t *rows_before_input)
{
    if (shell == 0 || now == 0 || username == 0 || cwd_color == 0 || rows_before_input == 0) {
        return false;
    }
    (void)columns;
    *rows_before_input = 0u;
    return winxterm_dstcmd_shell_write_prompt_prefix(shell, now, username) &&
           winxterm_dstcmd_shell_write_prompt_cwd_range(shell,
                                                        cwd_color,
                                                        shell->cwd,
                                                        0u,
                                                        wcslen(shell->cwd)) &&
           winxterm_dstcmd_shell_write_wide(shell,
                                            WINXTERM_DSTCMD_PROMPT_COLOR_RESET L"\r\n") &&
           winxterm_dstcmd_shell_write_prompt_command_prefix(shell);
}

static bool winxterm_dstcmd_shell_write_cursor_up(WinxtermDstcmdShell *shell, size_t rows)
{
    if (rows == 0u) {
        return true;
    }
    char sequence[64];
    int written = sprintf_s(sequence, sizeof(sequence), "\x1b[%zuA", rows);
    return written > 0 && winxterm_dstcmd_shell_write_utf8(shell, sequence);
}

static bool winxterm_dstcmd_shell_restore_prompt_start(WinxtermDstcmdShell *shell)
{
    if (shell != 0 && shell->prompt_cursor_saved) {
        return winxterm_dstcmd_shell_write_utf8(shell, "\x1b[u");
    }
    return winxterm_dstcmd_shell_write_utf8(shell, "\r");
}

static bool winxterm_dstcmd_shell_save_prompt_start_from_input_end(
    WinxtermDstcmdShell *shell,
    WinxtermDstcmdLinePosition end_position,
    size_t rows_before_input)
{
    return winxterm_dstcmd_shell_write_cursor_up(shell, end_position.row + rows_before_input) &&
           winxterm_dstcmd_shell_write_utf8(shell, "\r\x1b[3G\x1b[s");
}

static bool winxterm_dstcmd_shell_write_input_range(WinxtermDstcmdShell *shell,
                                                    const char *line,
                                                    size_t byte_count)
{
    if (shell == 0 || line == 0) {
        return false;
    }
    size_t start = 0u;
    for (size_t i = 0u; i < byte_count; ++i) {
        if (line[i] != '\n') {
            continue;
        }
        if ((i == start || winxterm_dstcmd_shell_write_bytes(
                              shell, (const uint8_t *)line + start, i - start)) &&
            winxterm_dstcmd_shell_write_utf8(shell, "\r\n")) {
            start = i + 1u;
            continue;
        }
        return false;
    }
    return start == byte_count ||
           winxterm_dstcmd_shell_write_bytes(shell,
                                             (const uint8_t *)line + start,
                                             byte_count - start);
}

static bool winxterm_dstcmd_shell_move_to_line_offset(WinxtermDstcmdShell *shell, size_t byte_offset)
{
    if (shell == 0 || !shell->prompt_cursor_saved) {
        return true;
    }
    if (byte_offset > shell->line_length) byte_offset = shell->line_length;
    return winxterm_dstcmd_shell_restore_prompt_start(shell) &&
           winxterm_dstcmd_shell_write_input_range(shell, shell->line, byte_offset);
}

static bool winxterm_dstcmd_shell_clear_rendered_prompt(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    bool ok = winxterm_dstcmd_shell_restore_prompt_start(shell) &&
              winxterm_dstcmd_shell_write_utf8(shell, "\x1b[J\r\n");
    shell->prompt_cursor_saved = false;
    return ok;
}

bool winxterm_dstcmd_shell_refresh_line(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    if (shell->timing_verbose_active) {
        winxterm_diag_inc_u64(&shell->timing_diagnostics.terminal_refresh_calls);
    }
    if (shell->output_lock_initialized) {
        EnterCriticalSection(&shell->output_lock);
    }
    SYSTEMTIME now;
    GetLocalTime(&now);
    wchar_t username[256];
    DWORD username_length = GetEnvironmentVariableW(L"USERNAME", username, (DWORD)(sizeof(username) / sizeof(username[0])));
    if (username_length == 0u || username_length >= (DWORD)(sizeof(username) / sizeof(username[0]))) {
        username_length = GetEnvironmentVariableW(L"USER", username, (DWORD)(sizeof(username) / sizeof(username[0])));
    }
    if (username_length == 0u || username_length >= (DWORD)(sizeof(username) / sizeof(username[0]))) {
        wcscpy_s(username, sizeof(username) / sizeof(username[0]), L"user");
    }

    wchar_t home[WINXTERM_DSTCMD_PATH_CAPACITY];
    wchar_t home_display[WINXTERM_DSTCMD_PATH_CAPACITY];
    DWORD home_length = GetEnvironmentVariableW(L"USERPROFILE", home, WINXTERM_DSTCMD_PATH_CAPACITY);
    bool is_home = home_length != 0u &&
                   home_length < WINXTERM_DSTCMD_PATH_CAPACITY &&
                   winxterm_dstcmd_path_to_display(home, home_display, WINXTERM_DSTCMD_PATH_CAPACITY) &&
                   _wcsicmp(shell->cwd, home_display) == 0;
    const wchar_t *cwd_color = is_home ?
        WINXTERM_DSTCMD_PROMPT_COLOR_DARK_GREEN : WINXTERM_DSTCMD_PROMPT_COLOR_GREEN;
    bool ok = true;
    if (shell->prompt_cursor_saved) {
        ok = winxterm_dstcmd_shell_restore_prompt_start(shell) &&
             winxterm_dstcmd_shell_write_utf8(shell, "\x1b[J");
    } else {
        size_t ignored_rows = 0u;
        ok = winxterm_dstcmd_shell_write_prompt(shell,
                                                &now,
                                                username,
                                                cwd_color,
                                                0,
                                                &ignored_rows) &&
             winxterm_dstcmd_shell_write_utf8(shell, "\x1b[s");
        shell->prompt_cursor_saved = ok;
    }
    if (!ok ||
        !winxterm_dstcmd_shell_write_input_range(shell, shell->line, shell->line_length) ||
        !winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_PROMPT_COLOR_RESET) ||
        !winxterm_dstcmd_shell_restore_prompt_start(shell) ||
        !winxterm_dstcmd_shell_write_input_range(shell, shell->line, shell->line_cursor)) {
        if (shell->output_lock_initialized) {
            LeaveCriticalSection(&shell->output_lock);
        }
        return false;
    }
    shell->prompt_rows_before_input = 0u;
    shell->prompt_cursor_saved = true;
    if (shell->output_lock_initialized) {
        LeaveCriticalSection(&shell->output_lock);
    }
    return true;
}

static bool winxterm_dstcmd_shell_replace_line(WinxtermDstcmdShell *shell, const char *line)
{
    if (shell == 0 || line == 0) {
        return false;
    }
    size_t length = strlen(line);
    if (length >= WINXTERM_DSTCMD_LINE_CAPACITY) {
        length = WINXTERM_DSTCMD_LINE_CAPACITY - 1u;
    }
    memcpy(shell->line, line, length);
    shell->line[length] = '\0';
    shell->line_length = length;
    shell->line_cursor = length;
    return winxterm_dstcmd_shell_refresh_line(shell);
}

static bool winxterm_dstcmd_get_home_directory(WinxtermDstcmdScratch *scratch,
                                               wchar_t *out,
                                               size_t out_count)
{
    if (scratch == 0 || out == 0 || out_count == 0u) {
        return false;
    }
    DWORD length = GetEnvironmentVariableW(L"USERPROFILE", out, (DWORD)out_count);
    if (length != 0u && length < out_count) {
        return true;
    }
    wchar_t drive[16];
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(scratch);
    wchar_t *path = winxterm_dstcmd_scratch_alloc_path(scratch);
    if (path == 0) {
        return false;
    }
    DWORD drive_length = GetEnvironmentVariableW(L"HOMEDRIVE", drive, 16u);
    DWORD path_length = GetEnvironmentVariableW(L"HOMEPATH", path, WINXTERM_DSTCMD_PATH_CAPACITY);
    if (drive_length == 0u || drive_length >= 16u || path_length == 0u ||
        path_length >= WINXTERM_DSTCMD_PATH_CAPACITY) {
        winxterm_dstcmd_scratch_rewind(scratch, mark);
        return false;
    }
    bool ok = _snwprintf_s(out, out_count, _TRUNCATE, L"%ls%ls", drive, path) >= 0;
    winxterm_dstcmd_scratch_rewind(scratch, mark);
    return ok;
}

static bool winxterm_dstcmd_home_file_path(WinxtermDstcmdScratch *scratch,
                                           const wchar_t *name,
                                           wchar_t *out,
                                           size_t out_count)
{
    if (scratch == 0 || name == 0 || out == 0 || out_count == 0u) {
        return false;
    }
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(scratch);
    wchar_t *home = winxterm_dstcmd_scratch_alloc_path(scratch);
    wchar_t *directory = winxterm_dstcmd_scratch_alloc_path(scratch);
    if (home == 0 || directory == 0) {
        return false;
    }
    bool ok = false;
    if (!winxterm_dstcmd_get_home_directory(scratch, home, WINXTERM_DSTCMD_PATH_CAPACITY)) {
        goto cleanup;
    }
    if (_snwprintf_s(directory,
                     WINXTERM_DSTCMD_PATH_CAPACITY,
                     _TRUNCATE,
                     L"%ls\\.winxterm",
                     home) < 0) {
        goto cleanup;
    }
    if (!winxterm_dstcmd_path_create_directory_scratch(scratch, directory) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        goto cleanup;
    }
    ok = _snwprintf_s(out,
                      out_count,
                      _TRUNCATE,
                      L"%ls\\%ls",
                      directory,
                      name) >= 0;

cleanup:
    winxterm_dstcmd_scratch_rewind(scratch, mark);
    return ok;
}

static bool winxterm_dstcmd_shell_init_history_db_path(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    return winxterm_dstcmd_home_file_path(&shell->scratch,
                                          L"history.sqlite3",
                                          shell->history_db_path,
                                          WINXTERM_DSTCMD_PATH_CAPACITY);
}

static void winxterm_dstcmd_shell_load_env_rc(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    wchar_t *env_path = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    if (env_path != 0 &&
        winxterm_dstcmd_home_file_path(&shell->scratch, L"env.rc", env_path, WINXTERM_DSTCMD_PATH_CAPACITY)) {
        (void)winxterm_dstcmd_env_load_file(env_path);
    }
    winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
}

static bool winxterm_dstcmd_shell_sync_cwd_env(const WinxtermDstcmdShell *shell)
{
    if (shell == 0 || !shell->cwd_env_sync_enabled || shell->cwd[0] == L'\0') {
        return false;
    }
    return SetEnvironmentVariableW(L"CWD", shell->cwd) != 0;
}

static void winxterm_dstcmd_shell_reconcile_cwd_env(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    wchar_t *saved_cwd = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    if (saved_cwd == 0) {
        (void)winxterm_dstcmd_shell_sync_cwd_env(shell);
        winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
        return;
    }

    SetLastError(ERROR_SUCCESS);
    DWORD length = GetEnvironmentVariableW(L"CWD", saved_cwd, WINXTERM_DSTCMD_PATH_CAPACITY);
    if (length != 0u && length < WINXTERM_DSTCMD_PATH_CAPACITY) {
        if (!winxterm_dstcmd_shell_set_cwd(shell, saved_cwd)) {
            (void)winxterm_dstcmd_shell_write_widef(
                shell,
                L"Note: CWD points to a nonexisting directory %ls\r\n",
                saved_cwd);
            (void)winxterm_dstcmd_shell_sync_cwd_env(shell);
        }
        winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
        return;
    }
    if (length >= WINXTERM_DSTCMD_PATH_CAPACITY) {
        (void)winxterm_dstcmd_shell_write_wide(
            shell,
            L"Note: CWD is too long to read; using current directory\r\n");
    }
    (void)winxterm_dstcmd_shell_sync_cwd_env(shell);
    winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
}

static bool winxterm_dstcmd_shell_add_history_memory(WinxtermDstcmdShell *shell, const char *line)
{
    if (shell == 0 || line == 0 || line[0] == '\0') {
        return true;
    }
    if (shell->history_count != 0u &&
        strcmp(shell->history[shell->history_count - 1u], line) == 0) {
        shell->history_index = shell->history_count;
        return true;
    }
    char *copy = winxterm_dstcmd_strdup(line);
    if (copy == 0) {
        return false;
    }
    if (shell->history_count == WINXTERM_DSTCMD_HISTORY_CAPACITY) {
        free(shell->history[0]);
        memmove(shell->history,
                shell->history + 1,
                (WINXTERM_DSTCMD_HISTORY_CAPACITY - 1u) * sizeof(shell->history[0]));
        --shell->history_count;
    }
    shell->history[shell->history_count++] = copy;
    shell->history_index = shell->history_count;
    return true;
}

static bool winxterm_dstcmd_shell_line_has_nonspace(const char *line)
{
    if (line == 0) {
        return false;
    }
    for (const char *p = line; *p != '\0'; ++p) {
        if (*p != ' ' && *p != '\t') {
            return true;
        }
    }
    return false;
}

static int64_t winxterm_dstcmd_unix_epoch_ms(void)
{
    FILETIME file_time;
    GetSystemTimeAsFileTime(&file_time);
    ULARGE_INTEGER value;
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    const uint64_t unix_epoch_offset_100ns = UINT64_C(116444736000000000);
    if (value.QuadPart <= unix_epoch_offset_100ns) {
        return 0;
    }
    return (int64_t)((value.QuadPart - unix_epoch_offset_100ns) / UINT64_C(10000));
}

static bool winxterm_dstcmd_sqlite_is_busy(int result)
{
    int primary = result & 0xff;
    return primary == SQLITE_BUSY || primary == SQLITE_LOCKED;
}

static bool winxterm_dstcmd_history_db_result_is_busy(WinxtermDstcmdShell *shell, int result)
{
    if (winxterm_dstcmd_sqlite_is_busy(result)) {
        return true;
    }
    if (shell != 0 && shell->history_db != 0) {
        return winxterm_dstcmd_sqlite_is_busy(sqlite3_extended_errcode(shell->history_db));
    }
    return false;
}

static void winxterm_dstcmd_history_pending_write_dispose(
    WinxtermDstcmdHistoryPendingWrite *write)
{
    if (write == 0) {
        return;
    }
    free(write->command);
    free(write);
}

static void winxterm_dstcmd_shell_free_persisted_history_unlocked(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    for (size_t i = 0u; i < shell->persisted_history_count; ++i) {
        free(shell->persisted_history[i]);
        shell->persisted_history[i] = 0;
    }
    shell->persisted_history_count = 0u;
    shell->persisted_history_index = 0u;
    shell->persisted_history_loaded = false;
}

static void winxterm_dstcmd_shell_replace_persisted_history(
    WinxtermDstcmdShell *shell,
    char **items,
    size_t item_count)
{
    if (shell == 0) {
        for (size_t i = 0u; i < item_count; ++i) {
            free(items[i]);
        }
        return;
    }
    if (shell->history_state_lock_initialized) {
        EnterCriticalSection(&shell->history_state_lock);
    }
    winxterm_dstcmd_shell_free_persisted_history_unlocked(shell);
    size_t count = item_count < WINXTERM_DSTCMD_HISTORY_CAPACITY ? item_count :
        WINXTERM_DSTCMD_HISTORY_CAPACITY;
    for (size_t i = 0u; i < count; ++i) {
        shell->persisted_history[i] = items[i];
        items[i] = 0;
    }
    shell->persisted_history_count = count;
    shell->persisted_history_loaded = true;
    if (shell->persisted_history_index >= shell->persisted_history_count) {
        shell->persisted_history_index = shell->persisted_history_count;
    }
    if (shell->history_state_lock_initialized) {
        LeaveCriticalSection(&shell->history_state_lock);
    }
    for (size_t i = 0u; i < item_count; ++i) {
        free(items[i]);
    }
}

static void winxterm_dstcmd_shell_history_db_close_locked(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    if (shell->history_upsert_stmt != 0) {
        sqlite3_finalize(shell->history_upsert_stmt);
        shell->history_upsert_stmt = 0;
    }
    if (shell->history_query_stmt != 0) {
        sqlite3_finalize(shell->history_query_stmt);
        shell->history_query_stmt = 0;
    }
    if (shell->history_search_stmt != 0) {
        sqlite3_finalize(shell->history_search_stmt);
        shell->history_search_stmt = 0;
    }
    if (shell->history_db != 0) {
        sqlite3_close(shell->history_db);
        shell->history_db = 0;
    }
    shell->history_db_ready = false;
}

static void winxterm_dstcmd_shell_history_db_disable_locked(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    winxterm_dstcmd_shell_history_db_close_locked(shell);
    shell->history_db_disabled = true;
}

static int winxterm_dstcmd_shell_history_db_exec_locked(WinxtermDstcmdShell *shell,
                                                       const char *sql)
{
    char *message = 0;
    int result = sqlite3_exec(shell->history_db, sql, 0, 0, &message);
    sqlite3_free(message);
    return result;
}

static bool winxterm_dstcmd_shell_history_db_ensure_ready_locked(WinxtermDstcmdShell *shell,
                                                                 bool *busy)
{
    if (busy != 0) {
        *busy = false;
    }
    if (shell == 0 || shell->history_db_disabled || shell->history_db_path[0] == L'\0') {
        return false;
    }
    if (shell->history_db_ready) {
        return true;
    }
    if (shell->history_db == 0) {
        int result = sqlite3_open16(shell->history_db_path, &shell->history_db);
        if (result != SQLITE_OK) {
            if (busy != 0) {
                *busy = winxterm_dstcmd_history_db_result_is_busy(shell, result);
            }
            if (busy != 0 && *busy) {
                return false;
            }
            winxterm_dstcmd_shell_history_db_disable_locked(shell);
            return false;
        }
        sqlite3_extended_result_codes(shell->history_db, 1);
        sqlite3_busy_timeout(shell->history_db, WINXTERM_DSTCMD_HISTORY_DB_BUSY_TIMEOUT_MS);
    }

    int result = winxterm_dstcmd_shell_history_db_exec_locked(shell, "PRAGMA journal_mode=WAL;");
    if (result != SQLITE_OK && winxterm_dstcmd_history_db_result_is_busy(shell, result)) {
        if (busy != 0) {
            *busy = true;
        }
        return false;
    }

    result = winxterm_dstcmd_shell_history_db_exec_locked(
        shell,
        "CREATE TABLE IF NOT EXISTS history ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "pid INTEGER NOT NULL,"
        "firstInvocationDate INTEGER NOT NULL,"
        "lastInvocationDate INTEGER NOT NULL,"
        "command TEXT NOT NULL UNIQUE,"
        "invocationCount INTEGER NOT NULL DEFAULT 1"
        ");");
    if (result != SQLITE_OK) {
        if (busy != 0) {
            *busy = winxterm_dstcmd_history_db_result_is_busy(shell, result);
        }
        if (busy != 0 && *busy) {
            return false;
        }
        winxterm_dstcmd_shell_history_db_disable_locked(shell);
        return false;
    }

    if (shell->history_upsert_stmt == 0) {
        result = sqlite3_prepare_v2(
            shell->history_db,
            "INSERT INTO history "
            "(pid, firstInvocationDate, lastInvocationDate, command, invocationCount) "
            "VALUES (?1, ?2, ?2, ?3, 1) "
            "ON CONFLICT(command) DO UPDATE SET "
            "pid = excluded.pid, "
            "lastInvocationDate = excluded.lastInvocationDate, "
            "invocationCount = history.invocationCount + 1;",
            -1,
            &shell->history_upsert_stmt,
            0);
        if (result != SQLITE_OK) {
            if (busy != 0) {
                *busy = winxterm_dstcmd_history_db_result_is_busy(shell, result);
            }
            if (busy != 0 && *busy) {
                return false;
            }
            winxterm_dstcmd_shell_history_db_disable_locked(shell);
            return false;
        }
    }

    if (shell->history_query_stmt == 0) {
        result = sqlite3_prepare_v2(
            shell->history_db,
            "SELECT command FROM history "
            "ORDER BY lastInvocationDate DESC, id DESC "
            "LIMIT ?1;",
            -1,
            &shell->history_query_stmt,
            0);
        if (result != SQLITE_OK) {
            if (busy != 0) {
                *busy = winxterm_dstcmd_history_db_result_is_busy(shell, result);
            }
            if (busy != 0 && *busy) {
                return false;
            }
            winxterm_dstcmd_shell_history_db_disable_locked(shell);
            return false;
        }
    }

    shell->history_db_ready = true;
    return true;
}

static bool winxterm_dstcmd_shell_history_db_execute_upsert_locked(
    WinxtermDstcmdShell *shell,
    const WinxtermDstcmdHistoryPendingWrite *write,
    bool *busy)
{
    if (busy != 0) {
        *busy = false;
    }
    if (shell == 0 || write == 0 ||
        !winxterm_dstcmd_shell_history_db_ensure_ready_locked(shell, busy)) {
        return false;
    }
    sqlite3_stmt *statement = shell->history_upsert_stmt;
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    int result = sqlite3_bind_int64(statement, 1, (sqlite3_int64)write->pid);
    if (result == SQLITE_OK) {
        result = sqlite3_bind_int64(statement, 2, (sqlite3_int64)write->invocation_date_ms);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_bind_text(statement, 3, write->command, -1, SQLITE_TRANSIENT);
    }
    if (result == SQLITE_OK) {
        result = sqlite3_step(statement);
    }
    bool ok = result == SQLITE_DONE;
    if (!ok && busy != 0) {
        *busy = winxterm_dstcmd_history_db_result_is_busy(shell, result);
    }
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    return ok;
}

static bool winxterm_dstcmd_shell_history_db_refresh_persisted_locked(
    WinxtermDstcmdShell *shell,
    bool *busy)
{
    if (busy != 0) {
        *busy = false;
    }
    if (shell == 0 || !winxterm_dstcmd_shell_history_db_ensure_ready_locked(shell, busy)) {
        return false;
    }

    char *items[WINXTERM_DSTCMD_HISTORY_CAPACITY];
    size_t item_count = 0u;
    memset(items, 0, sizeof(items));

    sqlite3_stmt *statement = shell->history_query_stmt;
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    int result = sqlite3_bind_int(statement, 1, (int)WINXTERM_DSTCMD_HISTORY_CAPACITY);
    if (result != SQLITE_OK) {
        if (busy != 0) {
            *busy = winxterm_dstcmd_history_db_result_is_busy(shell, result);
        }
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
        return false;
    }

    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(statement, 0);
        if (text == 0) {
            continue;
        }
        char *copy = winxterm_dstcmd_strdup((const char *)text);
        if (copy == 0) {
            break;
        }
        items[item_count++] = copy;
        if (item_count == WINXTERM_DSTCMD_HISTORY_CAPACITY) {
            break;
        }
    }

    bool ok = result == SQLITE_DONE || item_count == WINXTERM_DSTCMD_HISTORY_CAPACITY;
    if (!ok && busy != 0) {
        *busy = winxterm_dstcmd_history_db_result_is_busy(shell, result);
    }
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    if (!ok) {
        for (size_t i = 0u; i < item_count; ++i) {
            free(items[i]);
        }
        return false;
    }
    winxterm_dstcmd_shell_replace_persisted_history(shell, items, item_count);
    return true;
}

static void winxterm_dstcmd_shell_history_enqueue_write(WinxtermDstcmdShell *shell,
                                                       const char *line,
                                                       int64_t invocation_date_ms)
{
    if (shell == 0 || line == 0 || line[0] == '\0' ||
        shell->history_retry_event == 0 || !shell->history_state_lock_initialized) {
        return;
    }
    WinxtermDstcmdHistoryPendingWrite *write =
        (WinxtermDstcmdHistoryPendingWrite *)calloc(1u, sizeof(*write));
    if (write == 0) {
        return;
    }
    write->command = winxterm_dstcmd_strdup(line);
    if (write->command == 0) {
        winxterm_dstcmd_history_pending_write_dispose(write);
        return;
    }
    write->pid = GetCurrentProcessId();
    write->invocation_date_ms = invocation_date_ms;

    EnterCriticalSection(&shell->history_state_lock);
    if (shell->history_retry_shutdown) {
        LeaveCriticalSection(&shell->history_state_lock);
        winxterm_dstcmd_history_pending_write_dispose(write);
        return;
    }
    if (shell->history_write_tail != 0) {
        shell->history_write_tail->next = write;
    } else {
        shell->history_write_head = write;
    }
    shell->history_write_tail = write;
    LeaveCriticalSection(&shell->history_state_lock);
    SetEvent(shell->history_retry_event);
}

static WinxtermDstcmdHistoryPendingWrite *winxterm_dstcmd_shell_history_pop_write(
    WinxtermDstcmdShell *shell)
{
    if (shell == 0 || !shell->history_state_lock_initialized) {
        return 0;
    }
    EnterCriticalSection(&shell->history_state_lock);
    WinxtermDstcmdHistoryPendingWrite *write = shell->history_write_head;
    if (write != 0) {
        shell->history_write_head = write->next;
        if (shell->history_write_head == 0) {
            shell->history_write_tail = 0;
        }
        write->next = 0;
    }
    LeaveCriticalSection(&shell->history_state_lock);
    return write;
}

static void winxterm_dstcmd_shell_history_requeue_write(WinxtermDstcmdShell *shell,
                                                       WinxtermDstcmdHistoryPendingWrite *write)
{
    if (shell == 0 || write == 0 || !shell->history_state_lock_initialized) {
        winxterm_dstcmd_history_pending_write_dispose(write);
        return;
    }
    EnterCriticalSection(&shell->history_state_lock);
    if (shell->history_retry_shutdown) {
        LeaveCriticalSection(&shell->history_state_lock);
        winxterm_dstcmd_history_pending_write_dispose(write);
        return;
    }
    write->next = 0;
    if (shell->history_write_tail != 0) {
        shell->history_write_tail->next = write;
    } else {
        shell->history_write_head = write;
    }
    shell->history_write_tail = write;
    LeaveCriticalSection(&shell->history_state_lock);
    SetEvent(shell->history_retry_event);
}

static bool winxterm_dstcmd_shell_history_shutdown_requested(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || !shell->history_state_lock_initialized) {
        return true;
    }
    EnterCriticalSection(&shell->history_state_lock);
    bool shutdown = shell->history_retry_shutdown;
    LeaveCriticalSection(&shell->history_state_lock);
    return shutdown;
}

static void winxterm_dstcmd_shell_history_request_persisted_refresh(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || shell->history_retry_event == 0 || !shell->history_state_lock_initialized) {
        return;
    }
    EnterCriticalSection(&shell->history_state_lock);
    if (!shell->history_retry_shutdown && !shell->persisted_history_refresh_requested) {
        shell->persisted_history_refresh_requested = true;
        shell->persisted_history_refresh_attempts = 0u;
    }
    LeaveCriticalSection(&shell->history_state_lock);
    SetEvent(shell->history_retry_event);
}

static void winxterm_dstcmd_shell_history_invalidate_persisted(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || !shell->history_state_lock_initialized) {
        return;
    }
    EnterCriticalSection(&shell->history_state_lock);
    shell->persisted_history_loaded = false;
    shell->persisted_history_active = false;
    shell->persisted_history_index = 0u;
    LeaveCriticalSection(&shell->history_state_lock);
}

static bool winxterm_dstcmd_shell_history_take_persisted_refresh(WinxtermDstcmdShell *shell,
                                                                unsigned int *attempts)
{
    if (attempts != 0) {
        *attempts = 0u;
    }
    if (shell == 0 || !shell->history_state_lock_initialized) {
        return false;
    }
    EnterCriticalSection(&shell->history_state_lock);
    bool requested = shell->persisted_history_refresh_requested;
    if (requested) {
        shell->persisted_history_refresh_requested = false;
        if (attempts != 0) {
            *attempts = shell->persisted_history_refresh_attempts;
        }
    }
    LeaveCriticalSection(&shell->history_state_lock);
    return requested;
}

static void winxterm_dstcmd_shell_history_retry_persisted_refresh(
    WinxtermDstcmdShell *shell,
    unsigned int attempts)
{
    if (shell == 0 || !shell->history_state_lock_initialized) {
        return;
    }
    EnterCriticalSection(&shell->history_state_lock);
    if (!shell->history_retry_shutdown) {
        shell->persisted_history_refresh_requested = true;
        shell->persisted_history_refresh_attempts = attempts;
    }
    LeaveCriticalSection(&shell->history_state_lock);
    SetEvent(shell->history_retry_event);
}

static bool winxterm_dstcmd_shell_history_refresh_persisted(WinxtermDstcmdShell *shell,
                                                           bool wait_for_db_lock,
                                                           bool *busy)
{
    if (busy != 0) {
        *busy = false;
    }
    if (shell == 0 || !shell->history_db_lock_initialized) {
        return false;
    }
    bool locked = false;
    if (wait_for_db_lock) {
        EnterCriticalSection(&shell->history_db_lock);
        locked = true;
    } else {
        locked = TryEnterCriticalSection(&shell->history_db_lock) != 0;
    }
    if (!locked) {
        if (busy != 0) {
            *busy = true;
        }
        return false;
    }
    bool ok = winxterm_dstcmd_shell_history_db_refresh_persisted_locked(shell, busy);
    LeaveCriticalSection(&shell->history_db_lock);
    return ok;
}

static bool winxterm_dstcmd_shell_history_persisted_loaded(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || !shell->history_state_lock_initialized) {
        return false;
    }
    EnterCriticalSection(&shell->history_state_lock);
    bool loaded = shell->persisted_history_loaded;
    LeaveCriticalSection(&shell->history_state_lock);
    if (loaded) {
        return true;
    }

    bool busy = false;
    bool ok = winxterm_dstcmd_shell_history_refresh_persisted(shell, false, &busy);
    if (!ok && busy) {
        winxterm_dstcmd_shell_history_request_persisted_refresh(shell);
    }
    if (!ok) {
        return false;
    }

    EnterCriticalSection(&shell->history_state_lock);
    loaded = shell->persisted_history_loaded;
    LeaveCriticalSection(&shell->history_state_lock);
    return loaded;
}

static bool winxterm_dstcmd_shell_history_command_in_session(
    const WinxtermDstcmdShell *shell,
    const char *command)
{
    if (shell == 0 || command == 0) {
        return false;
    }
    for (size_t i = 0u; i < shell->history_count; ++i) {
        if (shell->history[i] != 0 && strcmp(shell->history[i], command) == 0) {
            return true;
        }
    }
    return false;
}

static bool winxterm_dstcmd_shell_history_copy_persisted_at_or_after(
    WinxtermDstcmdShell *shell,
    size_t start,
    size_t *out_index,
    char **out_command)
{
    if (out_index != 0) {
        *out_index = 0u;
    }
    if (out_command != 0) {
        *out_command = 0;
    }
    if (shell == 0 || out_index == 0 || out_command == 0 ||
        !winxterm_dstcmd_shell_history_persisted_loaded(shell)) {
        return false;
    }

    EnterCriticalSection(&shell->history_state_lock);
    for (size_t i = start; i < shell->persisted_history_count; ++i) {
        const char *command = shell->persisted_history[i];
        if (command == 0 || winxterm_dstcmd_shell_history_command_in_session(shell, command)) {
            continue;
        }
        char *copy = winxterm_dstcmd_strdup(command);
        if (copy == 0) {
            break;
        }
        *out_index = i;
        *out_command = copy;
        LeaveCriticalSection(&shell->history_state_lock);
        return true;
    }
    LeaveCriticalSection(&shell->history_state_lock);
    return false;
}

static bool winxterm_dstcmd_shell_history_copy_persisted_before(
    WinxtermDstcmdShell *shell,
    size_t start,
    size_t *out_index,
    char **out_command)
{
    if (out_index != 0) {
        *out_index = 0u;
    }
    if (out_command != 0) {
        *out_command = 0;
    }
    if (shell == 0 || out_index == 0 || out_command == 0 ||
        !winxterm_dstcmd_shell_history_persisted_loaded(shell)) {
        return false;
    }

    EnterCriticalSection(&shell->history_state_lock);
    size_t i = start;
    while (i != 0u) {
        --i;
        const char *command = shell->persisted_history[i];
        if (command == 0 || winxterm_dstcmd_shell_history_command_in_session(shell, command)) {
            continue;
        }
        char *copy = winxterm_dstcmd_strdup(command);
        if (copy == 0) {
            break;
        }
        *out_index = i;
        *out_command = copy;
        LeaveCriticalSection(&shell->history_state_lock);
        return true;
    }
    LeaveCriticalSection(&shell->history_state_lock);
    return false;
}

static void winxterm_dstcmd_shell_history_process_write(WinxtermDstcmdShell *shell,
                                                       WinxtermDstcmdHistoryPendingWrite *write)
{
    if (shell == 0 || write == 0) {
        winxterm_dstcmd_history_pending_write_dispose(write);
        return;
    }
    bool busy = false;
    bool ok = false;
    if (shell->history_db_lock_initialized) {
        EnterCriticalSection(&shell->history_db_lock);
        ok = winxterm_dstcmd_shell_history_db_execute_upsert_locked(shell, write, &busy);
        LeaveCriticalSection(&shell->history_db_lock);
    }
    if (ok || !busy || write->attempts + 1u >= WINXTERM_DSTCMD_HISTORY_DB_RETRY_ATTEMPTS ||
        winxterm_dstcmd_shell_history_shutdown_requested(shell)) {
        winxterm_dstcmd_history_pending_write_dispose(write);
        return;
    }
    ++write->attempts;
    Sleep(WINXTERM_DSTCMD_HISTORY_DB_RETRY_INTERVAL_MS);
    winxterm_dstcmd_shell_history_requeue_write(shell, write);
}

static void winxterm_dstcmd_shell_history_process_persisted_refresh(
    WinxtermDstcmdShell *shell,
    unsigned int attempts)
{
    bool busy = false;
    bool ok = winxterm_dstcmd_shell_history_refresh_persisted(shell, true, &busy);
    if (ok || !busy || attempts + 1u >= WINXTERM_DSTCMD_HISTORY_DB_RETRY_ATTEMPTS ||
        winxterm_dstcmd_shell_history_shutdown_requested(shell)) {
        if (shell != 0 && shell->history_state_lock_initialized) {
            EnterCriticalSection(&shell->history_state_lock);
            shell->persisted_history_refresh_attempts = 0u;
            LeaveCriticalSection(&shell->history_state_lock);
        }
        return;
    }
    Sleep(WINXTERM_DSTCMD_HISTORY_DB_RETRY_INTERVAL_MS);
    winxterm_dstcmd_shell_history_retry_persisted_refresh(shell, attempts + 1u);
}

static DWORD WINAPI winxterm_dstcmd_shell_history_retry_thread_proc(void *context)
{
    WinxtermDstcmdShell *shell = (WinxtermDstcmdShell *)context;
    for (;;) {
        if (winxterm_dstcmd_shell_history_shutdown_requested(shell)) {
            break;
        }
        WinxtermDstcmdHistoryPendingWrite *write =
            winxterm_dstcmd_shell_history_pop_write(shell);
        if (write != 0) {
            winxterm_dstcmd_shell_history_process_write(shell, write);
            continue;
        }
        unsigned int attempts = 0u;
        if (winxterm_dstcmd_shell_history_take_persisted_refresh(shell, &attempts)) {
            winxterm_dstcmd_shell_history_process_persisted_refresh(shell, attempts);
            continue;
        }
        if (shell == 0 || shell->history_retry_event == 0) {
            break;
        }
        WaitForSingleObject(shell->history_retry_event, INFINITE);
    }
    return 0;
}

static void winxterm_dstcmd_shell_history_init_db(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || !winxterm_dstcmd_shell_init_history_db_path(shell)) {
        return;
    }
    InitializeCriticalSection(&shell->history_db_lock);
    shell->history_db_lock_initialized = true;
    InitializeCriticalSection(&shell->history_state_lock);
    shell->history_state_lock_initialized = true;
    shell->history_retry_event = CreateEventW(0, FALSE, FALSE, 0);
    if (shell->history_retry_event == 0) {
        return;
    }
    shell->history_retry_thread = CreateThread(0,
                                              0,
                                              winxterm_dstcmd_shell_history_retry_thread_proc,
                                              shell,
                                              0,
                                              0);
    if (shell->history_retry_thread == 0) {
        CloseHandle(shell->history_retry_event);
        shell->history_retry_event = 0;
    }
    if (shell->history_db_lock_initialized) {
        bool busy = false;
        EnterCriticalSection(&shell->history_db_lock);
        (void)winxterm_dstcmd_shell_history_db_ensure_ready_locked(shell, &busy);
        LeaveCriticalSection(&shell->history_db_lock);
    }
}

static void winxterm_dstcmd_shell_history_dispose_db(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    if (shell->history_state_lock_initialized) {
        EnterCriticalSection(&shell->history_state_lock);
        shell->history_retry_shutdown = true;
        LeaveCriticalSection(&shell->history_state_lock);
    }
    if (shell->history_retry_event != 0) {
        SetEvent(shell->history_retry_event);
    }
    if (shell->history_retry_thread != 0) {
        WaitForSingleObject(shell->history_retry_thread, INFINITE);
        CloseHandle(shell->history_retry_thread);
        shell->history_retry_thread = 0;
    }
    if (shell->history_retry_event != 0) {
        CloseHandle(shell->history_retry_event);
        shell->history_retry_event = 0;
    }
    if (shell->history_state_lock_initialized) {
        EnterCriticalSection(&shell->history_state_lock);
        WinxtermDstcmdHistoryPendingWrite *write = shell->history_write_head;
        shell->history_write_head = 0;
        shell->history_write_tail = 0;
        winxterm_dstcmd_shell_free_persisted_history_unlocked(shell);
        LeaveCriticalSection(&shell->history_state_lock);
        while (write != 0) {
            WinxtermDstcmdHistoryPendingWrite *next = write->next;
            winxterm_dstcmd_history_pending_write_dispose(write);
            write = next;
        }
        DeleteCriticalSection(&shell->history_state_lock);
        shell->history_state_lock_initialized = false;
    }
    if (shell->history_db_lock_initialized) {
        EnterCriticalSection(&shell->history_db_lock);
        winxterm_dstcmd_shell_history_db_close_locked(shell);
        LeaveCriticalSection(&shell->history_db_lock);
        DeleteCriticalSection(&shell->history_db_lock);
        shell->history_db_lock_initialized = false;
    }
}

static void winxterm_dstcmd_shell_record_history(WinxtermDstcmdShell *shell, const char *line)
{
    if (shell == 0 || !winxterm_dstcmd_shell_line_has_nonspace(line)) {
        if (shell != 0) {
            shell->history_index = shell->history_count;
        }
        return;
    }
    int64_t now_ms = winxterm_dstcmd_unix_epoch_ms();
    (void)winxterm_dstcmd_shell_add_history_memory(shell, line);
    winxterm_dstcmd_shell_history_invalidate_persisted(shell);
    winxterm_dstcmd_shell_history_enqueue_write(shell, line, now_ms);
}

static void winxterm_dstcmd_history_search_free_candidates(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    for (size_t i = 0u; i < shell->history_search_candidate_count; ++i) {
        free(shell->history_search_candidates[i].command);
        shell->history_search_candidates[i].command = 0;
    }
    shell->history_search_candidate_count = 0u;
    shell->history_search_result_count = 0u;
    shell->history_search_selected = 0u;
    shell->history_search_scroll = 0u;
}

static bool winxterm_dstcmd_history_search_command_equals(const char *left, const char *right)
{
    return left != 0 && right != 0 && strcmp(left, right) == 0;
}

static bool winxterm_dstcmd_history_search_add_candidate(WinxtermDstcmdShell *shell,
                                                        const char *command,
                                                        int64_t last_invocation_ms,
                                                        unsigned int invocation_count,
                                                        bool session,
                                                        size_t order)
{
    if (shell == 0 || command == 0 || command[0] == '\0') {
        return true;
    }
    for (size_t i = 0u; i < shell->history_search_candidate_count; ++i) {
        WinxtermDstcmdHistorySearchCandidate *candidate = shell->history_search_candidates + i;
        if (!winxterm_dstcmd_history_search_command_equals(candidate->command, command)) {
            continue;
        }
        if (session) {
            candidate->session = true;
        }
        if (last_invocation_ms > candidate->last_invocation_ms) {
            candidate->last_invocation_ms = last_invocation_ms;
        }
        if (invocation_count > candidate->invocation_count) {
            candidate->invocation_count = invocation_count;
        } else if (session && candidate->invocation_count < UINT_MAX) {
            ++candidate->invocation_count;
        }
        if (order < candidate->order) {
            candidate->order = order;
        }
        return true;
    }
    if (shell->history_search_candidate_count == WINXTERM_DSTCMD_HISTORY_CAPACITY) {
        return true;
    }
    char *copy = winxterm_dstcmd_strdup(command);
    if (copy == 0) {
        return false;
    }
    WinxtermDstcmdHistorySearchCandidate *candidate =
        shell->history_search_candidates + shell->history_search_candidate_count++;
    candidate->command = copy;
    candidate->last_invocation_ms = last_invocation_ms;
    candidate->invocation_count = invocation_count != 0u ? invocation_count : 1u;
    candidate->session = session;
    candidate->order = order;
    return true;
}

static bool winxterm_dstcmd_history_search_prepare_stmt_locked(WinxtermDstcmdShell *shell,
                                                              bool *busy)
{
    if (busy != 0) {
        *busy = false;
    }
    if (shell == 0 || !winxterm_dstcmd_shell_history_db_ensure_ready_locked(shell, busy)) {
        return false;
    }
    if (shell->history_search_stmt != 0) {
        return true;
    }
    int result = sqlite3_prepare_v2(
        shell->history_db,
        "SELECT command, lastInvocationDate, invocationCount FROM history "
        "ORDER BY lastInvocationDate DESC, id DESC "
        "LIMIT ?1;",
        -1,
        &shell->history_search_stmt,
        0);
    if (result != SQLITE_OK) {
        if (busy != 0) {
            *busy = winxterm_dstcmd_history_db_result_is_busy(shell, result);
        }
        if (busy == 0 || !*busy) {
            winxterm_dstcmd_shell_history_db_disable_locked(shell);
        }
        return false;
    }
    return true;
}

static void winxterm_dstcmd_history_search_collect_session(WinxtermDstcmdShell *shell,
                                                          int64_t now_ms)
{
    if (shell == 0) {
        return;
    }
    size_t order = 0u;
    size_t i = shell->history_count;
    while (i != 0u) {
        --i;
        int64_t age_bias = order <= (size_t)INT_MAX ? (int64_t)order : (int64_t)INT_MAX;
        (void)winxterm_dstcmd_history_search_add_candidate(shell,
                                                           shell->history[i],
                                                           now_ms - age_bias,
                                                           1u,
                                                           true,
                                                           order);
        ++order;
    }
}

static bool winxterm_dstcmd_history_search_collect_saved(WinxtermDstcmdShell *shell,
                                                        size_t order_base,
                                                        bool *unavailable)
{
    if (unavailable != 0) {
        *unavailable = false;
    }
    if (shell == 0 || !shell->history_db_lock_initialized) {
        if (unavailable != 0) {
            *unavailable = true;
        }
        return false;
    }
    if (TryEnterCriticalSection(&shell->history_db_lock) == 0) {
        if (unavailable != 0) {
            *unavailable = true;
        }
        winxterm_dstcmd_shell_history_request_persisted_refresh(shell);
        return false;
    }

    bool busy = false;
    bool ok = winxterm_dstcmd_history_search_prepare_stmt_locked(shell, &busy);
    if (!ok) {
        LeaveCriticalSection(&shell->history_db_lock);
        if (unavailable != 0) {
            *unavailable = true;
        }
        if (busy) {
            winxterm_dstcmd_shell_history_request_persisted_refresh(shell);
        }
        return false;
    }

    sqlite3_stmt *statement = shell->history_search_stmt;
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    int result = sqlite3_bind_int(statement, 1, (int)WINXTERM_DSTCMD_HISTORY_CAPACITY);
    if (result != SQLITE_OK) {
        sqlite3_reset(statement);
        sqlite3_clear_bindings(statement);
        LeaveCriticalSection(&shell->history_db_lock);
        if (unavailable != 0) {
            *unavailable = true;
        }
        return false;
    }

    size_t order = order_base;
    while ((result = sqlite3_step(statement)) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(statement, 0);
        if (text == 0) {
            continue;
        }
        int64_t last_invocation_ms = (int64_t)sqlite3_column_int64(statement, 1);
        int count = sqlite3_column_int(statement, 2);
        unsigned int invocation_count = count > 0 ? (unsigned int)count : 1u;
        if (!winxterm_dstcmd_history_search_add_candidate(shell,
                                                          (const char *)text,
                                                          last_invocation_ms,
                                                          invocation_count,
                                                          false,
                                                          order++)) {
            break;
        }
        if (shell->history_search_candidate_count == WINXTERM_DSTCMD_HISTORY_CAPACITY) {
            break;
        }
    }

    bool complete = result == SQLITE_DONE ||
                    shell->history_search_candidate_count == WINXTERM_DSTCMD_HISTORY_CAPACITY;
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    LeaveCriticalSection(&shell->history_db_lock);
    if (!complete && unavailable != 0) {
        *unavailable = true;
    }
    return complete;
}

static bool winxterm_dstcmd_history_search_query_has_upper(const char *query)
{
    if (query == 0) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)query; *p != '\0'; ++p) {
        if (*p >= 'A' && *p <= 'Z') {
            return true;
        }
    }
    return false;
}

static const char *winxterm_dstcmd_history_search_strstr_ci(const char *text,
                                                            const char *needle,
                                                            bool smart_case)
{
    return winxterm_dstcmd_selector_contains_utf8(text, needle, smart_case);
}

static bool winxterm_dstcmd_history_search_is_word_start(const char *text, const char *p)
{
    if (text == 0 || p == 0 || p <= text) {
        return p == text;
    }
    char prev = p[-1];
    return prev == ' ' || prev == '\t' || prev == '/' || prev == '\\' ||
           prev == '-' || prev == '_' || prev == '.';
}

static bool winxterm_dstcmd_history_search_fuzzy_match(const char *text,
                                                       const char *term,
                                                       bool smart_case,
                                                       int *gap_penalty)
{
    return winxterm_dstcmd_selector_fuzzy_utf8(text, term, smart_case, gap_penalty);
}

static size_t winxterm_dstcmd_history_search_split_terms(char *query, char **terms, size_t term_capacity)
{
    if (query == 0 || terms == 0 || term_capacity == 0u) {
        return 0u;
    }
    size_t count = 0u;
    char *p = query;
    while (*p != '\0' && count < term_capacity) {
        while (*p == ' ' || *p == '\t') {
            *p++ = '\0';
        }
        if (*p == '\0') {
            break;
        }
        terms[count++] = p;
        while (*p != '\0' && *p != ' ' && *p != '\t') {
            ++p;
        }
    }
    return count;
}

static bool winxterm_dstcmd_history_search_score_candidate(
    const WinxtermDstcmdShell *shell,
    const WinxtermDstcmdHistorySearchCandidate *candidate,
    int *score)
{
    if (score != 0) {
        *score = 0;
    }
    if (shell == 0 || candidate == 0 || candidate->command == 0 || score == 0) {
        return false;
    }

    char query[WINXTERM_DSTCMD_LINE_CAPACITY];
    memcpy(query, shell->history_search_query, shell->history_search_query_length + 1u);
    char *terms[WINXTERM_DSTCMD_HISTORY_SEARCH_MAX_TERMS];
    size_t term_count =
        winxterm_dstcmd_history_search_split_terms(query, terms, WINXTERM_DSTCMD_HISTORY_SEARCH_MAX_TERMS);
    bool smart_case = winxterm_dstcmd_history_search_query_has_upper(shell->history_search_query);

    int total = 0;
    for (size_t i = 0u; i < term_count; ++i) {
        const char *term = terms[i];
        const char *exact =
            winxterm_dstcmd_history_search_strstr_ci(candidate->command, term, smart_case);
        if (shell->history_search_matching == WINXTERM_DSTCMD_HISTORY_SEARCH_MATCH_CONTAINS) {
            if (exact == 0) {
                return false;
            }
            total += 1200 + (int)strlen(term) * 12;
            if (winxterm_dstcmd_history_search_is_word_start(candidate->command, exact)) {
                total += 120;
            }
            continue;
        }
        if (exact != 0) {
            total += 1000 + (int)strlen(term) * 10;
            if (winxterm_dstcmd_history_search_is_word_start(candidate->command, exact)) {
                total += 160;
            }
            continue;
        }
        int gap_penalty = 0;
        if (!winxterm_dstcmd_history_search_fuzzy_match(candidate->command,
                                                        term,
                                                        smart_case,
                                                        &gap_penalty)) {
            return false;
        }
        total += 550 + (int)strlen(term) * 6 - gap_penalty;
    }

    if (term_count == 0u) {
        total += 100;
    }
    if (shell->history_search_ranking == WINXTERM_DSTCMD_HISTORY_SEARCH_RANK_BEST) {
        if (candidate->session) {
            total += 180;
        }
        unsigned int count = candidate->invocation_count;
        total += count > 25u ? 75 : (int)(count * 3u);
    }
    if (candidate->order < 500u) {
        total += (int)(500u - candidate->order);
    }
    *score = total;
    return true;
}

static bool winxterm_dstcmd_history_search_result_before(
    const WinxtermDstcmdShell *shell,
    const WinxtermDstcmdHistorySearchResult *left,
    const WinxtermDstcmdHistorySearchResult *right)
{
    const WinxtermDstcmdHistorySearchCandidate *left_candidate =
        shell->history_search_candidates + left->candidate_index;
    const WinxtermDstcmdHistorySearchCandidate *right_candidate =
        shell->history_search_candidates + right->candidate_index;
    if (shell->history_search_ranking == WINXTERM_DSTCMD_HISTORY_SEARCH_RANK_RECENT) {
        if (left_candidate->order != right_candidate->order) {
            return left_candidate->order < right_candidate->order;
        }
    } else if (left->score != right->score) {
        return left->score > right->score;
    }
    if (left_candidate->session != right_candidate->session) {
        return left_candidate->session;
    }
    return left_candidate->order < right_candidate->order;
}

static void winxterm_dstcmd_history_search_insert_result(WinxtermDstcmdShell *shell,
                                                        size_t candidate_index,
                                                        int score)
{
    if (shell == 0 || shell->history_search_result_count == WINXTERM_DSTCMD_HISTORY_CAPACITY) {
        return;
    }
    WinxtermDstcmdHistorySearchResult result = {candidate_index, score};
    size_t index = shell->history_search_result_count++;
    while (index != 0u &&
           winxterm_dstcmd_history_search_result_before(shell,
                                                       &result,
                                                       shell->history_search_results + index - 1u)) {
        shell->history_search_results[index] = shell->history_search_results[index - 1u];
        --index;
    }
    shell->history_search_results[index] = result;
}

static void winxterm_dstcmd_history_search_rebuild(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    winxterm_dstcmd_history_search_free_candidates(shell);
    int64_t now_ms = winxterm_dstcmd_unix_epoch_ms();
    winxterm_dstcmd_history_search_collect_session(shell, now_ms);
    bool saved_unavailable = false;
    (void)winxterm_dstcmd_history_search_collect_saved(shell,
                                                       shell->history_search_candidate_count,
                                                       &saved_unavailable);

    shell->history_search_result_count = 0u;
    for (size_t i = 0u; i < shell->history_search_candidate_count; ++i) {
        int score = 0;
        if (winxterm_dstcmd_history_search_score_candidate(shell,
                                                          shell->history_search_candidates + i,
                                                          &score)) {
            winxterm_dstcmd_history_search_insert_result(shell, i, score);
        }
    }
    if (shell->history_search_selected >= shell->history_search_result_count) {
        shell->history_search_selected =
            shell->history_search_result_count != 0u ? shell->history_search_result_count - 1u : 0u;
    }
    if (shell->history_search_scroll > shell->history_search_selected) {
        shell->history_search_scroll = shell->history_search_selected;
    }
    if (saved_unavailable && shell->history_search_status[0] == '\0') {
        strncpy_s(shell->history_search_status,
                  sizeof(shell->history_search_status),
                  "saved history unavailable; showing session history",
                  _TRUNCATE);
    }
}

static void winxterm_dstcmd_history_search_set_line_without_refresh(WinxtermDstcmdShell *shell,
                                                                   const char *line,
                                                                   size_t cursor)
{
    if (shell == 0 || line == 0) {
        return;
    }
    size_t length = strlen(line);
    if (length >= WINXTERM_DSTCMD_LINE_CAPACITY) {
        length = WINXTERM_DSTCMD_LINE_CAPACITY - 1u;
    }
    memcpy(shell->line, line, length);
    shell->line[length] = '\0';
    shell->line_length = length;
    shell->line_cursor = cursor <= length ? cursor : length;
}

static const char *winxterm_dstcmd_history_search_ranking_label(const WinxtermDstcmdShell *shell)
{
    return shell != 0 &&
        shell->history_search_ranking == WINXTERM_DSTCMD_HISTORY_SEARCH_RANK_RECENT ?
        "recent" : "best";
}

static const char *winxterm_dstcmd_history_search_matching_label(const WinxtermDstcmdShell *shell)
{
    return shell != 0 &&
        shell->history_search_matching == WINXTERM_DSTCMD_HISTORY_SEARCH_MATCH_CONTAINS ?
        "contains" : "fuzzy";
}

typedef struct WinxtermDstcmdHistorySearchRenderBuffer {
    char *bytes;
    size_t count;
    size_t capacity;
    bool failed;
} WinxtermDstcmdHistorySearchRenderBuffer;

static void winxterm_dstcmd_history_search_render_buffer_dispose(
    WinxtermDstcmdHistorySearchRenderBuffer *buffer)
{
    if (buffer == 0) {
        return;
    }
    free(buffer->bytes);
    memset(buffer, 0, sizeof(*buffer));
}

static bool winxterm_dstcmd_history_search_render_buffer_reserve(
    WinxtermDstcmdHistorySearchRenderBuffer *buffer,
    size_t additional)
{
    if (buffer == 0 || buffer->failed) {
        return false;
    }
    if (additional > ((size_t)-1) - buffer->count - 1u) {
        buffer->failed = true;
        return false;
    }
    size_t needed = buffer->count + additional + 1u;
    if (needed <= buffer->capacity) {
        return true;
    }
    size_t capacity = buffer->capacity == 0u ? 4096u : buffer->capacity;
    while (capacity < needed) {
        if (capacity > ((size_t)-1) / 2u) {
            capacity = needed;
            break;
        }
        capacity *= 2u;
    }
    char *bytes = (char *)realloc(buffer->bytes, capacity);
    if (bytes == 0) {
        buffer->failed = true;
        return false;
    }
    buffer->bytes = bytes;
    buffer->capacity = capacity;
    buffer->bytes[buffer->count] = '\0';
    return true;
}

static bool winxterm_dstcmd_history_search_render_buffer_append_n(
    WinxtermDstcmdHistorySearchRenderBuffer *buffer,
    const char *text,
    size_t length)
{
    if (text == 0 || length == 0u) {
        return buffer != 0 && !buffer->failed;
    }
    if (!winxterm_dstcmd_history_search_render_buffer_reserve(buffer, length)) {
        return false;
    }
    memcpy(buffer->bytes + buffer->count, text, length);
    buffer->count += length;
    buffer->bytes[buffer->count] = '\0';
    return true;
}

static bool winxterm_dstcmd_history_search_render_buffer_append(
    WinxtermDstcmdHistorySearchRenderBuffer *buffer,
    const char *text)
{
    return text == 0 ?
        buffer != 0 && !buffer->failed :
        winxterm_dstcmd_history_search_render_buffer_append_n(buffer, text, strlen(text));
}

static bool winxterm_dstcmd_history_search_render_buffer_appendf(
    WinxtermDstcmdHistorySearchRenderBuffer *buffer,
    const char *format,
    ...)
{
    if (buffer == 0 || buffer->failed || format == 0) {
        return false;
    }
    va_list args;
    va_start(args, format);
    int needed = _vscprintf(format, args);
    va_end(args);
    if (needed < 0 ||
        !winxterm_dstcmd_history_search_render_buffer_reserve(buffer, (size_t)needed)) {
        buffer->failed = true;
        return false;
    }
    va_start(args, format);
    int written = vsprintf_s(buffer->bytes + buffer->count,
                             buffer->capacity - buffer->count,
                             format,
                             args);
    va_end(args);
    if (written < 0) {
        buffer->failed = true;
        return false;
    }
    buffer->count += (size_t)written;
    return true;
}

static size_t winxterm_dstcmd_history_search_truncate_length(const char *text, size_t limit)
{
    if (text == 0 || limit == 0u) {
        return 0u;
    }
    size_t length = strlen(text);
    if (length <= limit) {
        return length;
    }
    size_t truncated = limit <= 3u ? limit : limit - 3u;
    while (truncated > 0u && ((unsigned char)text[truncated] & 0xc0u) == 0x80u) {
        truncated = winxterm_dstcmd_utf8_prev_boundary(text, length, truncated);
    }
    return truncated;
}

static bool winxterm_dstcmd_history_search_render_buffer_append_truncated(
    WinxtermDstcmdHistorySearchRenderBuffer *buffer,
    const char *text,
    size_t limit)
{
    if (text == 0 || limit == 0u) {
        return buffer != 0 && !buffer->failed;
    }
    size_t length = strlen(text);
    if (length <= limit) {
        return winxterm_dstcmd_history_search_render_buffer_append_n(buffer, text, length);
    }
    size_t truncated = winxterm_dstcmd_history_search_truncate_length(text, limit);
    return winxterm_dstcmd_history_search_render_buffer_append_n(buffer, text, truncated) &&
           (limit <= 3u ||
            winxterm_dstcmd_history_search_render_buffer_append(buffer, "..."));
}

static bool winxterm_dstcmd_history_search_render_buffer_cursor_down(
    WinxtermDstcmdHistorySearchRenderBuffer *buffer,
    size_t rows)
{
    return rows == 0u ||
           winxterm_dstcmd_history_search_render_buffer_appendf(buffer, "\x1b[%zuB", rows);
}

static bool winxterm_dstcmd_history_search_render_buffer_cursor_column(
    WinxtermDstcmdHistorySearchRenderBuffer *buffer,
    size_t column)
{
    return winxterm_dstcmd_history_search_render_buffer_appendf(buffer, "\x1b[%zuG", column + 1u);
}

static WinxtermDstcmdLinePosition winxterm_dstcmd_history_search_line_end_position(
    const WinxtermDstcmdShell *shell,
    int columns)
{
    return winxterm_dstcmd_shell_line_position(shell->line,
                                               shell->line_length,
                                               columns,
                                               WINXTERM_DSTCMD_PROMPT_COMMAND_PREFIX_COLUMNS);
}

static bool winxterm_dstcmd_history_search_render_buffer_move_to_overlay(
    WinxtermDstcmdHistorySearchRenderBuffer *buffer,
    const WinxtermDstcmdShell *shell,
    WinxtermDstcmdLinePosition line_end)
{
    if (!winxterm_dstcmd_history_search_render_buffer_append(
            buffer,
            shell->prompt_cursor_saved ? "\x1b[u" : "\r") ||
        !winxterm_dstcmd_history_search_render_buffer_cursor_down(
            buffer,
            line_end.row + shell->prompt_rows_before_input) ||
        !winxterm_dstcmd_history_search_render_buffer_cursor_column(buffer, line_end.column)) {
        return false;
    }
    return winxterm_dstcmd_history_search_render_buffer_append(buffer, "\r\n");
}

static bool winxterm_dstcmd_history_search_enter_overlay(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    if (shell->history_search_overlay_active) {
        return true;
    }
    if (!shell->prompt_cursor_saved && !winxterm_dstcmd_shell_refresh_line(shell)) {
        return false;
    }
    int columns = winxterm_dstcmd_overlay_terminal_columns(shell);
    WinxtermDstcmdLinePosition line_end =
        winxterm_dstcmd_history_search_line_end_position(shell, columns);
    if (!winxterm_dstcmd_shell_move_to_line_offset(shell, shell->line_length)) {
        return false;
    }
    for (size_t i = 0u; i < WINXTERM_DSTCMD_HISTORY_SEARCH_OVERLAY_ROWS; ++i) {
        if (!winxterm_dstcmd_shell_write_utf8(shell, "\r\n")) {
            return false;
        }
    }
    if (!winxterm_dstcmd_shell_write_cursor_up(shell, WINXTERM_DSTCMD_HISTORY_SEARCH_OVERLAY_ROWS) ||
        !winxterm_dstcmd_shell_save_prompt_start_from_input_end(shell,
                                                               line_end,
                                                               shell->prompt_rows_before_input)) {
        return false;
    }
    shell->history_search_overlay_active = true;
    shell->history_search_overlay_reserved_rows = WINXTERM_DSTCMD_HISTORY_SEARCH_OVERLAY_ROWS;
    shell->history_search_overlay_columns = 0;
    shell->history_search_overlay_rows = 0;
    return true;
}

static bool winxterm_dstcmd_history_search_clear_overlay(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || !shell->history_search_overlay_active) {
        return true;
    }
    int columns = winxterm_dstcmd_overlay_terminal_columns(shell);
    WinxtermDstcmdLinePosition line_end =
        winxterm_dstcmd_history_search_line_end_position(shell, columns);
    WinxtermDstcmdHistorySearchRenderBuffer buffer = {0};
    if (!winxterm_dstcmd_history_search_render_buffer_move_to_overlay(&buffer, shell, line_end)) {
        winxterm_dstcmd_history_search_render_buffer_dispose(&buffer);
        return false;
    }
    for (size_t i = 0u; i < shell->history_search_overlay_reserved_rows; ++i) {
        if (!winxterm_dstcmd_history_search_render_buffer_append(&buffer, "\x1b[0m\r\x1b[2K") ||
            (i + 1u < shell->history_search_overlay_reserved_rows &&
             !winxterm_dstcmd_history_search_render_buffer_append(&buffer, "\r\n"))) {
            winxterm_dstcmd_history_search_render_buffer_dispose(&buffer);
            return false;
        }
    }
    bool ok = !buffer.failed &&
              winxterm_dstcmd_history_search_render_buffer_append(&buffer, "\x1b[0m\x1b[u") &&
              winxterm_dstcmd_shell_write_bytes(shell,
                                                (const uint8_t *)buffer.bytes,
                                                buffer.count);
    winxterm_dstcmd_history_search_render_buffer_dispose(&buffer);
    shell->history_search_overlay_active = false;
    shell->history_search_overlay_reserved_rows = 0u;
    shell->history_search_overlay_columns = 0;
    shell->history_search_overlay_rows = 0;
    return ok;
}

static bool winxterm_dstcmd_history_search_render_header(
    WinxtermDstcmdHistorySearchRenderBuffer *buffer,
    const WinxtermDstcmdShell *shell,
    size_t usable_columns,
    size_t *query_cursor_column)
{
    const char *ranking = winxterm_dstcmd_history_search_ranking_label(shell);
    const char *matching = winxterm_dstcmd_history_search_matching_label(shell);
    char prefix[96];
    int written = sprintf_s(prefix, sizeof(prefix), "history [%s %s] > ", ranking, matching);
    if (written <= 0) {
        return false;
    }
    size_t prefix_columns = (size_t)written;
    size_t suffix_columns = shell->history_search_status[0] != '\0' ? 3u + strlen(shell->history_search_status) :
        12u;
    size_t query_limit = usable_columns > prefix_columns + suffix_columns ?
        usable_columns - prefix_columns - suffix_columns : usable_columns > prefix_columns ?
        usable_columns - prefix_columns : 0u;
    if (query_limit > WINXTERM_DSTCMD_LINE_CAPACITY / 2u) {
        query_limit = WINXTERM_DSTCMD_LINE_CAPACITY / 2u;
    }
    size_t query_visible = winxterm_dstcmd_history_search_truncate_length(shell->history_search_query,
                                                                          query_limit);
    size_t cursor_offset = shell->history_search_query_cursor < query_visible ?
        shell->history_search_query_cursor : query_visible;
    WinxtermDstcmdLinePosition cursor =
        winxterm_dstcmd_shell_line_position(shell->history_search_query,
                                            cursor_offset,
                                            (int)usable_columns,
                                            prefix_columns);
    *query_cursor_column = cursor.column;
    if (!winxterm_dstcmd_history_search_render_buffer_append(buffer, prefix) ||
        !winxterm_dstcmd_history_search_render_buffer_append_truncated(buffer,
                                                                       shell->history_search_query,
                                                                       query_limit)) {
        return false;
    }
    if (shell->history_search_status[0] != '\0') {
        return winxterm_dstcmd_history_search_render_buffer_append(buffer, "  ") &&
               winxterm_dstcmd_history_search_render_buffer_append_truncated(buffer,
                                                                             shell->history_search_status,
                                                                             usable_columns);
    }
    const char *noun = shell->history_search_result_count == 1u ? "match" : "matches";
    return winxterm_dstcmd_history_search_render_buffer_appendf(buffer,
                                                               "  %zu %s",
                                                               shell->history_search_result_count,
                                                               noun);
}

static bool winxterm_dstcmd_history_search_render_panel(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || !shell->history_search_active) {
        return false;
    }
    if (!winxterm_dstcmd_history_search_enter_overlay(shell)) {
        return false;
    }
    int columns = winxterm_dstcmd_overlay_terminal_columns(shell);
    int rows = winxterm_dstcmd_shell_terminal_rows(shell);
    if (columns < 20) {
        columns = 20;
    }
    shell->history_search_overlay_columns = columns;
    shell->history_search_overlay_rows = rows;
    size_t usable_columns = (size_t)columns;
    WinxtermDstcmdLinePosition line_end =
        winxterm_dstcmd_history_search_line_end_position(shell, columns);
    WinxtermDstcmdHistorySearchRenderBuffer buffer = {0};
    if (!winxterm_dstcmd_history_search_render_buffer_move_to_overlay(&buffer, shell, line_end)) {
        winxterm_dstcmd_history_search_render_buffer_dispose(&buffer);
        return false;
    }

    size_t query_cursor_column = 0u;
    for (size_t row = 0u; row < WINXTERM_DSTCMD_HISTORY_SEARCH_OVERLAY_ROWS; ++row) {
        bool selected_row = false;
        if (row == 0u) {
            if (!winxterm_dstcmd_history_search_render_buffer_append(&buffer,
                                                                     WINXTERM_DSTCMD_HISTORY_SEARCH_STYLE
                                                                     "\r\x1b[2K") ||
                !winxterm_dstcmd_history_search_render_header(&buffer,
                                                              shell,
                                                              usable_columns,
                                                              &query_cursor_column)) {
                winxterm_dstcmd_history_search_render_buffer_dispose(&buffer);
                return false;
            }
        } else if (shell->history_search_result_count == 0u && row == 1u) {
            if (!winxterm_dstcmd_history_search_render_buffer_append(&buffer,
                                                                     WINXTERM_DSTCMD_HISTORY_SEARCH_STYLE
                                                                     "\r\x1b[2K  no history matches")) {
                winxterm_dstcmd_history_search_render_buffer_dispose(&buffer);
                return false;
            }
        } else if (shell->history_search_result_count != 0u) {
            size_t visible_index = shell->history_search_scroll + row - 1u;
            if (visible_index < shell->history_search_result_count) {
                WinxtermDstcmdHistorySearchResult *result =
                    shell->history_search_results + visible_index;
                WinxtermDstcmdHistorySearchCandidate *candidate =
                    shell->history_search_candidates + result->candidate_index;
                selected_row = visible_index == shell->history_search_selected;
                const char *style = selected_row ?
                    WINXTERM_DSTCMD_HISTORY_SEARCH_SELECTED_STYLE : WINXTERM_DSTCMD_HISTORY_SEARCH_STYLE;
                size_t command_limit = usable_columns > (selected_row ? 4u : 2u) ?
                    usable_columns - (selected_row ? 4u : 2u) : 0u;
                if (!winxterm_dstcmd_history_search_render_buffer_append(&buffer, style) ||
                    !winxterm_dstcmd_history_search_render_buffer_append(&buffer, "\r\x1b[2K") ||
                    !winxterm_dstcmd_history_search_render_buffer_append(&buffer,
                                                                         selected_row ? "> " : "  ") ||
                    !winxterm_dstcmd_history_search_render_buffer_append_truncated(&buffer,
                                                                                   candidate->command,
                                                                                   command_limit) ||
                    (selected_row &&
                     !winxterm_dstcmd_history_search_render_buffer_append(&buffer, " <"))) {
                    winxterm_dstcmd_history_search_render_buffer_dispose(&buffer);
                    return false;
                }
            } else if (!winxterm_dstcmd_history_search_render_buffer_append(&buffer,
                                                                            WINXTERM_DSTCMD_HISTORY_SEARCH_STYLE
                                                                            "\r\x1b[2K")) {
                winxterm_dstcmd_history_search_render_buffer_dispose(&buffer);
                return false;
            }
        } else if (!winxterm_dstcmd_history_search_render_buffer_append(&buffer,
                                                                        WINXTERM_DSTCMD_HISTORY_SEARCH_STYLE
                                                                        "\r\x1b[2K")) {
            winxterm_dstcmd_history_search_render_buffer_dispose(&buffer);
            return false;
        }
        if (row + 1u < WINXTERM_DSTCMD_HISTORY_SEARCH_OVERLAY_ROWS &&
            !winxterm_dstcmd_history_search_render_buffer_append(&buffer, "\r\n")) {
            winxterm_dstcmd_history_search_render_buffer_dispose(&buffer);
            return false;
        }
    }

    size_t cursor_down = line_end.row + shell->prompt_rows_before_input + 1u;
    bool ok = !buffer.failed &&
              winxterm_dstcmd_history_search_render_buffer_append(&buffer,
                                                                  WINXTERM_DSTCMD_HISTORY_SEARCH_RESET) &&
              winxterm_dstcmd_history_search_render_buffer_append(&buffer,
                                                                  shell->prompt_cursor_saved ? "\x1b[u" : "\r") &&
              winxterm_dstcmd_history_search_render_buffer_cursor_down(&buffer, cursor_down) &&
              winxterm_dstcmd_history_search_render_buffer_cursor_column(&buffer, query_cursor_column) &&
              winxterm_dstcmd_shell_write_bytes(shell,
                                                (const uint8_t *)buffer.bytes,
                                                buffer.count);
    winxterm_dstcmd_history_search_render_buffer_dispose(&buffer);
    return ok;
}

static bool winxterm_dstcmd_history_search_redraw(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || !shell->history_search_active) {
        return false;
    }
    return winxterm_dstcmd_history_search_render_panel(shell);
}

static bool winxterm_dstcmd_history_search_enter(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    shell->history_search_active = true;
    shell->history_search_ranking = WINXTERM_DSTCMD_HISTORY_SEARCH_RANK_BEST;
    shell->history_search_matching = WINXTERM_DSTCMD_HISTORY_SEARCH_MATCH_FUZZY;
    shell->history_search_original_cursor = shell->line_cursor;
    memcpy(shell->history_search_original_line, shell->line, shell->line_length + 1u);
    memcpy(shell->history_search_query, shell->line, shell->line_length + 1u);
    shell->history_search_query_length = shell->line_length;
    shell->history_search_query_cursor = shell->history_search_query_length;
    shell->history_search_selected = 0u;
    shell->history_search_scroll = 0u;
    shell->history_search_status[0] = '\0';
    shell->escape_sequence_length = 0u;
    shell->escape_sequence[0] = '\0';
    if (!winxterm_dstcmd_history_search_enter_overlay(shell)) {
        shell->history_search_active = false;
        return false;
    }
    winxterm_dstcmd_history_search_rebuild(shell);
    return winxterm_dstcmd_history_search_redraw(shell);
}

static bool winxterm_dstcmd_history_search_cancel(WinxtermDstcmdShell *shell, bool visible_interrupt)
{
    if (shell == 0) {
        return false;
    }
    shell->history_search_active = false;
    winxterm_dstcmd_history_search_free_candidates(shell);
    winxterm_dstcmd_history_search_set_line_without_refresh(shell,
                                                            shell->history_search_original_line,
                                                            shell->history_search_original_cursor);
    if (!winxterm_dstcmd_history_search_clear_overlay(shell)) {
        return false;
    }
    if (visible_interrupt) {
        (void)winxterm_dstcmd_shell_clear_rendered_prompt(shell);
        (void)winxterm_dstcmd_shell_write_utf8(shell, "^C\r\n");
    }
    return winxterm_dstcmd_shell_refresh_line(shell);
}

static bool winxterm_dstcmd_history_search_accept(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    const char *accepted = shell->history_search_query;
    if (shell->history_search_result_count != 0u) {
        WinxtermDstcmdHistorySearchResult *result =
            shell->history_search_results + shell->history_search_selected;
        accepted = shell->history_search_candidates[result->candidate_index].command;
    }
    char accepted_copy[WINXTERM_DSTCMD_LINE_CAPACITY];
    strncpy_s(accepted_copy, sizeof(accepted_copy), accepted, _TRUNCATE);
    shell->history_search_active = false;
    winxterm_dstcmd_history_search_free_candidates(shell);
    if (!winxterm_dstcmd_history_search_clear_overlay(shell)) {
        return false;
    }
    return winxterm_dstcmd_shell_replace_line(shell, accepted_copy);
}

static void winxterm_dstcmd_history_search_remove_session_command(WinxtermDstcmdShell *shell,
                                                                 const char *command)
{
    if (shell == 0 || command == 0) {
        return;
    }
    size_t write_index = 0u;
    for (size_t read_index = 0u; read_index < shell->history_count; ++read_index) {
        if (shell->history[read_index] != 0 && strcmp(shell->history[read_index], command) == 0) {
            free(shell->history[read_index]);
            continue;
        }
        shell->history[write_index++] = shell->history[read_index];
    }
    for (size_t i = write_index; i < shell->history_count; ++i) {
        shell->history[i] = 0;
    }
    shell->history_count = write_index;
    if (shell->history_index > shell->history_count) {
        shell->history_index = shell->history_count;
    }
}

static bool winxterm_dstcmd_history_search_db_delete(WinxtermDstcmdShell *shell,
                                                    const char *command)
{
    if (shell == 0 || command == 0 || !shell->history_db_lock_initialized) {
        return false;
    }
    EnterCriticalSection(&shell->history_db_lock);
    bool busy = false;
    bool ok = false;
    if (winxterm_dstcmd_shell_history_db_ensure_ready_locked(shell, &busy)) {
        sqlite3_stmt *statement = 0;
        int result = sqlite3_prepare_v2(shell->history_db,
                                        "DELETE FROM history WHERE command = ?1;",
                                        -1,
                                        &statement,
                                        0);
        if (result == SQLITE_OK) {
            result = sqlite3_bind_text(statement, 1, command, -1, SQLITE_TRANSIENT);
        }
        if (result == SQLITE_OK) {
            result = sqlite3_step(statement);
        }
        ok = result == SQLITE_DONE;
        if (statement != 0) {
            sqlite3_finalize(statement);
        }
    }
    LeaveCriticalSection(&shell->history_db_lock);
    if (busy) {
        winxterm_dstcmd_shell_history_request_persisted_refresh(shell);
    }
    return ok;
}

static bool winxterm_dstcmd_history_search_db_restore(WinxtermDstcmdShell *shell,
                                                     const char *command,
                                                     int64_t last_invocation_ms,
                                                     unsigned int invocation_count)
{
    if (shell == 0 || command == 0 || !shell->history_db_lock_initialized) {
        return false;
    }
    EnterCriticalSection(&shell->history_db_lock);
    bool busy = false;
    bool ok = false;
    if (winxterm_dstcmd_shell_history_db_ensure_ready_locked(shell, &busy)) {
        sqlite3_stmt *statement = 0;
        int result = sqlite3_prepare_v2(
            shell->history_db,
            "INSERT INTO history "
            "(pid, firstInvocationDate, lastInvocationDate, command, invocationCount) "
            "VALUES (?1, ?2, ?2, ?3, ?4) "
            "ON CONFLICT(command) DO UPDATE SET "
            "pid = excluded.pid, "
            "lastInvocationDate = excluded.lastInvocationDate, "
            "invocationCount = excluded.invocationCount;",
            -1,
            &statement,
            0);
        if (result == SQLITE_OK) {
            result = sqlite3_bind_int64(statement, 1, (sqlite3_int64)GetCurrentProcessId());
        }
        if (result == SQLITE_OK) {
            result = sqlite3_bind_int64(statement, 2, (sqlite3_int64)last_invocation_ms);
        }
        if (result == SQLITE_OK) {
            result = sqlite3_bind_text(statement, 3, command, -1, SQLITE_TRANSIENT);
        }
        if (result == SQLITE_OK) {
            result = sqlite3_bind_int(statement, 4, (int)invocation_count);
        }
        if (result == SQLITE_OK) {
            result = sqlite3_step(statement);
        }
        ok = result == SQLITE_DONE;
        if (statement != 0) {
            sqlite3_finalize(statement);
        }
    }
    LeaveCriticalSection(&shell->history_db_lock);
    if (busy) {
        winxterm_dstcmd_shell_history_request_persisted_refresh(shell);
    }
    return ok;
}

static bool winxterm_dstcmd_history_search_begin_delete(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || shell->history_search_result_count == 0u) {
        return true;
    }
    WinxtermDstcmdHistorySearchResult *result =
        shell->history_search_results + shell->history_search_selected;
    WinxtermDstcmdHistorySearchCandidate *candidate =
        shell->history_search_candidates + result->candidate_index;
    if (candidate->command == 0) {
        return true;
    }
    strncpy_s(shell->history_search_pending_delete,
              sizeof(shell->history_search_pending_delete),
              candidate->command,
              _TRUNCATE);
    shell->history_search_delete_confirm_active = true;
    strncpy_s(shell->history_search_status,
              sizeof(shell->history_search_status),
              "delete from history? Y confirm | N cancel",
              _TRUNCATE);
    return winxterm_dstcmd_history_search_redraw(shell);
}

static bool winxterm_dstcmd_history_search_confirm_delete(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || !shell->history_search_delete_confirm_active ||
        shell->history_search_pending_delete[0] == '\0') {
        return true;
    }
    int64_t last_invocation_ms = winxterm_dstcmd_unix_epoch_ms();
    unsigned int invocation_count = 1u;
    for (size_t i = 0u; i < shell->history_search_candidate_count; ++i) {
        WinxtermDstcmdHistorySearchCandidate *candidate = shell->history_search_candidates + i;
        if (candidate->command != 0 &&
            strcmp(candidate->command, shell->history_search_pending_delete) == 0) {
            last_invocation_ms = candidate->last_invocation_ms;
            invocation_count = candidate->invocation_count;
            break;
        }
    }
    strncpy_s(shell->history_search_undo_delete,
              sizeof(shell->history_search_undo_delete),
              shell->history_search_pending_delete,
              _TRUNCATE);
    shell->history_search_undo_last_invocation_ms = last_invocation_ms;
    shell->history_search_undo_invocation_count = invocation_count;
    shell->history_search_undo_available = true;
    winxterm_dstcmd_history_search_remove_session_command(shell,
                                                          shell->history_search_pending_delete);
    (void)winxterm_dstcmd_history_search_db_delete(shell, shell->history_search_pending_delete);
    winxterm_dstcmd_shell_history_invalidate_persisted(shell);
    shell->history_search_pending_delete[0] = '\0';
    shell->history_search_delete_confirm_active = false;
    strncpy_s(shell->history_search_status,
              sizeof(shell->history_search_status),
              "deleted - Ctrl+Z undo",
              _TRUNCATE);
    winxterm_dstcmd_history_search_rebuild(shell);
    return winxterm_dstcmd_history_search_redraw(shell);
}

static bool winxterm_dstcmd_history_search_cancel_delete(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    shell->history_search_pending_delete[0] = '\0';
    shell->history_search_delete_confirm_active = false;
    shell->history_search_status[0] = '\0';
    return winxterm_dstcmd_history_search_redraw(shell);
}

static bool winxterm_dstcmd_history_search_undo_delete(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || !shell->history_search_undo_available ||
        shell->history_search_undo_delete[0] == '\0') {
        return true;
    }
    (void)winxterm_dstcmd_shell_add_history_memory(shell, shell->history_search_undo_delete);
    (void)winxterm_dstcmd_history_search_db_restore(shell,
                                                    shell->history_search_undo_delete,
                                                    shell->history_search_undo_last_invocation_ms,
                                                    shell->history_search_undo_invocation_count);
    winxterm_dstcmd_shell_history_invalidate_persisted(shell);
    shell->history_search_undo_available = false;
    shell->history_search_undo_delete[0] = '\0';
    strncpy_s(shell->history_search_status,
              sizeof(shell->history_search_status),
              "restored",
              _TRUNCATE);
    winxterm_dstcmd_history_search_rebuild(shell);
    return winxterm_dstcmd_history_search_redraw(shell);
}

bool winxterm_dstcmd_shell_set_cwd(WinxtermDstcmdShell *shell, const wchar_t *path)
{
    if (shell == 0 || path == 0 || path[0] == L'\0') {
        return false;
    }
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    wchar_t *resolved = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    if (resolved == 0) {
        return false;
    }
    bool ok = false;
    if (!winxterm_dstcmd_path_resolve_full_scratch(&shell->scratch,
                                                   shell->cwd,
                                                   path,
                                                   resolved,
                                                   WINXTERM_DSTCMD_PATH_CAPACITY) ||
        !winxterm_dstcmd_path_is_directory_scratch(&shell->scratch, resolved)) {
        goto cleanup;
    }
    winxterm_dstcmd_path_trim_trailing_separators(resolved);
    if (shell->dir_cache.lock_initialized) {
        EnterCriticalSection(&shell->dir_cache.lock);
    }
    ok = winxterm_dstcmd_path_to_display(resolved, shell->cwd, WINXTERM_DSTCMD_PATH_CAPACITY);
    if (shell->dir_cache.lock_initialized) {
        LeaveCriticalSection(&shell->dir_cache.lock);
    }
    if (ok) {
        (void)winxterm_dstcmd_shell_sync_cwd_env(shell);
    }
cleanup:
    winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
    return ok;
}

const wchar_t *winxterm_dstcmd_shell_cwd(const WinxtermDstcmdShell *shell)
{
    return shell != 0 ? shell->cwd : L"";
}

static size_t winxterm_dstcmd_shell_alias_index(const WinxtermDstcmdShell *shell, const wchar_t *name)
{
    if (shell == 0 || name == 0) {
        return (size_t)-1;
    }
    for (size_t i = 0u; i < shell->alias_count; ++i) {
        if (shell->aliases[i].name != 0 && wcscmp(shell->aliases[i].name, name) == 0) {
            return i;
        }
    }
    return (size_t)-1;
}

const WinxtermDstcmdAlias *winxterm_dstcmd_shell_find_alias(const WinxtermDstcmdShell *shell,
                                                           const wchar_t *name)
{
    size_t index = winxterm_dstcmd_shell_alias_index(shell, name);
    return index != (size_t)-1 ? shell->aliases + index : 0;
}

void winxterm_dstcmd_shell_dispose_aliases(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    for (size_t i = 0u; i < shell->alias_count; ++i) {
        free(shell->aliases[i].name);
        free(shell->aliases[i].target);
        free(shell->aliases[i].description);
    }
    free(shell->aliases);
    shell->aliases = 0;
    shell->alias_count = 0u;
}

bool winxterm_dstcmd_shell_set_alias(WinxtermDstcmdShell *shell,
                                     const wchar_t *name,
                                     const wchar_t *target,
                                     const wchar_t *description)
{
    if (shell == 0 || name == 0 || name[0] == L'\0' ||
        target == 0 || target[0] == L'\0' ||
        description == 0 || description[0] == L'\0') {
        return false;
    }

    wchar_t *name_copy = winxterm_dstcmd_wcsdup(name);
    wchar_t *target_copy = winxterm_dstcmd_wcsdup(target);
    wchar_t *description_copy = winxterm_dstcmd_wcsdup(description);
    if (name_copy == 0 || target_copy == 0 || description_copy == 0) {
        free(name_copy);
        free(target_copy);
        free(description_copy);
        return false;
    }

    size_t index = winxterm_dstcmd_shell_alias_index(shell, name);
    if (index != (size_t)-1) {
        WinxtermDstcmdAlias *alias = shell->aliases + index;
        free(alias->name);
        free(alias->target);
        free(alias->description);
        alias->name = name_copy;
        alias->target = target_copy;
        alias->description = description_copy;
        return true;
    }

    WinxtermDstcmdAlias *aliases =
        (WinxtermDstcmdAlias *)realloc(shell->aliases, (shell->alias_count + 1u) * sizeof(*aliases));
    if (aliases == 0) {
        free(name_copy);
        free(target_copy);
        free(description_copy);
        return false;
    }
    shell->aliases = aliases;
    WinxtermDstcmdAlias *alias = shell->aliases + shell->alias_count++;
    alias->name = name_copy;
    alias->target = target_copy;
    alias->description = description_copy;
    return true;
}

bool winxterm_dstcmd_shell_clone_aliases(WinxtermDstcmdShell *shell,
                                        const WinxtermDstcmdShell *source)
{
    if (shell == 0 || source == 0) {
        return false;
    }
    winxterm_dstcmd_shell_dispose_aliases(shell);
    for (size_t i = 0u; i < source->alias_count; ++i) {
        const WinxtermDstcmdAlias *alias = source->aliases + i;
        if (!winxterm_dstcmd_shell_set_alias(shell,
                                             alias->name,
                                             alias->target,
                                             alias->description)) {
            winxterm_dstcmd_shell_dispose_aliases(shell);
            return false;
        }
    }
    return true;
}

bool winxterm_dstcmd_shell_init(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    memset(shell, 0, sizeof(*shell));
    shell->cwd_env_sync_enabled = true;
    winxterm_dstcmd_scratch_init(&shell->scratch);
    shell->input_handle = GetStdHandle(STD_INPUT_HANDLE);
    shell->output_handle = GetStdHandle(STD_OUTPUT_HANDLE);
    shell->error_handle = GetStdHandle(STD_ERROR_HANDLE);
    winxterm_dstcmd_job_client_init(&shell->job_client);
    winxterm_dstcmd_shell_configure_console_modes(shell);
    InitializeCriticalSection(&shell->output_lock);
    shell->output_lock_initialized = true;
    if (!winxterm_dstcmd_shell_init_dir_cache(shell)) {
        winxterm_dstcmd_shell_dispose(shell);
        return false;
    }
    if (!winxterm_dstcmd_job_pool_init(&shell->jobs)) {
        winxterm_dstcmd_shell_dispose(shell);
        return false;
    }
    DWORD length = GetCurrentDirectoryW(WINXTERM_DSTCMD_PATH_CAPACITY, shell->cwd);
    if (length == 0u || length >= WINXTERM_DSTCMD_PATH_CAPACITY) {
        winxterm_dstcmd_shell_dispose(shell);
        return false;
    }
    if (!winxterm_dstcmd_path_to_display(shell->cwd, shell->cwd, WINXTERM_DSTCMD_PATH_CAPACITY)) {
        winxterm_dstcmd_shell_dispose(shell);
        return false;
    }
    winxterm_dstcmd_shell_load_env_rc(shell);
    winxterm_dstcmd_shell_reconcile_cwd_env(shell);
    winxterm_dstcmd_shell_history_init_db(shell);
    shell->history_index = shell->history_count;
    (void)winxterm_dstcmd_shell_schedule_dir_cache_refresh(shell, WINXTERM_DSTCMD_DIR_REFRESH_CD);
    return true;
}

void winxterm_dstcmd_shell_dispose(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    (void)winxterm_dstcmd_history_search_clear_overlay(shell);
    if (shell->output_lock_initialized) {
        EnterCriticalSection(&shell->output_lock);
        shell->disposing = true;
        LeaveCriticalSection(&shell->output_lock);
    }
    winxterm_dstcmd_job_pool_dispose(&shell->jobs);
    winxterm_dstcmd_job_client_dispose(&shell->job_client);
    winxterm_dstcmd_shell_dispose_dir_cache(shell);
    winxterm_dstcmd_shell_history_dispose_db(shell);
    winxterm_dstcmd_history_search_free_candidates(shell);
    for (size_t i = 0u; i < shell->directory_stack_count; ++i) {
        free(shell->directory_stack[i]);
    }
    winxterm_dstcmd_shell_dispose_aliases(shell);
    for (size_t i = 0u; i < shell->history_count; ++i) {
        free(shell->history[i]);
    }
    winxterm_dstcmd_shell_restore_original_console_modes(shell);
    free(shell->capture_bytes);
    winxterm_dstcmd_scratch_dispose(&shell->scratch);
    if (shell->output_lock_initialized) {
        DeleteCriticalSection(&shell->output_lock);
        shell->output_lock_initialized = false;
    }
    memset(shell, 0, sizeof(*shell));
}

static bool winxterm_dstcmd_shell_prompt(WinxtermDstcmdShell *shell)
{
    (void)winxterm_dstcmd_shell_update_cwd_title(shell);
    bool ok = winxterm_dstcmd_shell_refresh_line(shell);
    if (ok && shell != 0 && shell->output_lock_initialized) {
        EnterCriticalSection(&shell->output_lock);
        shell->prompt_active = true;
        LeaveCriticalSection(&shell->output_lock);
    }
    return ok;
}

static void winxterm_dstcmd_shell_begin_timing(WinxtermDstcmdShell *shell,
                                               uint64_t command_entered_ns)
{
    if (shell == 0) {
        return;
    }
    shell->timing_command_active = shell->timing_mode != WINXTERM_DSTCMD_TIMING_OFF;
    shell->timing_verbose_active = shell->timing_mode == WINXTERM_DSTCMD_TIMING_VERBOSE;
    if (shell->timing_command_active) {
        shell->timing_start_ns = winxterm_dstcmd_shell_timestamp_ns();
        memset(&shell->timing_snapshot, 0, sizeof(shell->timing_snapshot));
        if (shell->timing_verbose_active) {
            memset(&shell->timing_diagnostics, 0, sizeof(shell->timing_diagnostics));
            shell->timing_diagnostics.started_ns = shell->timing_start_ns;
            shell->timing_diagnostics.command_entered_ns =
                command_entered_ns != 0u ? command_entered_ns : shell->timing_start_ns;
        }
    }
}

static void winxterm_dstcmd_shell_add_diag_ns(WinxtermDstcmdShell *shell,
                                              uint64_t *field,
                                              uint64_t elapsed_ns)
{
    if (shell != 0 && shell->timing_verbose_active && field != 0) {
        winxterm_diag_add_u64(field, elapsed_ns);
    }
}

static void winxterm_dstcmd_shell_note_command_finished(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || !shell->timing_verbose_active) {
        return;
    }
    shell->timing_diagnostics.command_finished_ns = winxterm_dstcmd_shell_timestamp_ns();
}

static void winxterm_dstcmd_shell_finish_timing(WinxtermDstcmdShell *shell, uint64_t end_ns)
{
    if (shell == 0 || !shell->timing_command_active) {
        return;
    }
    uint64_t elapsed_ns = end_ns >= shell->timing_start_ns ? end_ns - shell->timing_start_ns : 0u;
    if (shell->timing_verbose_active) {
        shell->timing_diagnostics.prompt_ready_ns = end_ns;
        shell->timing_diagnostics.elapsed_ns = elapsed_ns;
        if (shell->timing_diagnostics.command_finished_ns != 0u &&
            end_ns >= shell->timing_diagnostics.command_finished_ns) {
            shell->timing_diagnostics.finish_to_prompt_ns =
                end_ns - shell->timing_diagnostics.command_finished_ns;
        }
        shell->timing_snapshot = shell->timing_diagnostics;
    } else {
        memset(&shell->timing_snapshot, 0, sizeof(shell->timing_snapshot));
        shell->timing_snapshot.started_ns = shell->timing_start_ns;
        shell->timing_snapshot.prompt_ready_ns = end_ns;
        shell->timing_snapshot.elapsed_ns = elapsed_ns;
    }
    shell->timing_command_active = false;
    shell->timing_verbose_active = false;
}

static bool winxterm_dstcmd_shell_write_elapsed_timing(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || shell->timing_snapshot.started_ns == 0u) {
        return true;
    }

    uint64_t elapsed_ns = shell->timing_snapshot.elapsed_ns;
    unsigned long long seconds = (unsigned long long)(elapsed_ns / 1000000000ull);
    uint64_t remainder_ns = elapsed_ns % 1000000000ull;
    unsigned long long milliseconds = (unsigned long long)(remainder_ns / 1000000ull);
    unsigned long long nanoseconds = (unsigned long long)(remainder_ns % 1000000ull);
    return winxterm_dstcmd_shell_write_widef(shell,
                                            L"Execution finished after: %llu.%03llu.%06llu\r\n",
                                            seconds,
                                            milliseconds,
                                            nanoseconds);
}

static bool winxterm_dstcmd_shell_write_verbose_timing(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || shell->timing_snapshot.started_ns == 0u ||
        shell->timing_mode != WINXTERM_DSTCMD_TIMING_VERBOSE) {
        return true;
    }
    const WinxtermCommandDiagnostics *d = &shell->timing_snapshot;
    uint64_t command_entered_ns = d->command_entered_ns != 0u ? d->command_entered_ns : d->started_ns;
    uint64_t delta_ns = d->prompt_ready_ns >= command_entered_ns ?
        d->prompt_ready_ns - command_entered_ns : 0u;
    wchar_t command_entered_text[WINXTERM_DIAG_GROUPED_U64_CAPACITY];
    wchar_t prompt_ready_text[WINXTERM_DIAG_GROUPED_U64_CAPACITY];
    wchar_t delta_text[WINXTERM_DIAG_GROUPED_U64_CAPACITY];
    if (!winxterm_diag_format_grouped_u64(command_entered_ns,
                                          command_entered_text,
                                          WINXTERM_DIAG_GROUPED_U64_CAPACITY) ||
        !winxterm_diag_format_grouped_u64(d->prompt_ready_ns,
                                          prompt_ready_text,
                                          WINXTERM_DIAG_GROUPED_U64_CAPACITY) ||
        !winxterm_diag_format_grouped_u64(delta_ns, delta_text, WINXTERM_DIAG_GROUPED_U64_CAPACITY)) {
        return false;
    }
    return winxterm_dstcmd_shell_write_widef(shell,
                                            L"Timing verbose:\r\n"
                                            L"  timestamps ns: command entered=%ls prompt ready=%ls delta=%ls\r\n"
                                            L"  phases ns: parse=%llu lookup=%llu builtin=%llu resolve=%llu exec=%llu process=%llu finish_to_prompt=%llu\r\n"
                                            L"  output bytes: total=%llu builtin=%llu external=%llu rendered=%llu skipped=%llu\r\n"
                                            L"  calls: shell_writes=%llu feeds=%llu input_enqueues=%llu refreshes=%llu updates=%llu frames=%llu external_reads=%llu\r\n"
                                            L"  batches: count=%llu bytes=%llu max_bytes=%llu\r\n"
                                            L"  render updates: coalesced=%llu messages=%llu\r\n"
                                            L"  cells: rendered=%llu skipped=%llu empty_skips=%llu continuation_skips=%llu\r\n"
                                            L"  glyphs: draws=%llu rendered=%llu cache_hits=%llu cache_misses=%llu precolored_hits=%llu precolored_misses=%llu fallback_hits=%llu fallback_misses=%llu\r\n"
                                            L"  render ns: prepare=%llu snapshot=%llu dispatch_wait=%llu worker_total=%llu worker_max=%llu flip=%llu present=%llu\r\n"
                                            L"  damage: dirty_rows=%llu full_repaints=%llu scroll_blits=%llu\r\n",
                                            command_entered_text,
                                            prompt_ready_text,
                                            delta_text,
                                            (unsigned long long)d->parse_ns,
                                            (unsigned long long)d->builtin_lookup_ns,
                                            (unsigned long long)d->builtin_run_ns,
                                            (unsigned long long)d->program_resolve_ns,
                                            (unsigned long long)d->exec_run_ns,
                                            (unsigned long long)d->process_run_ns,
                                            (unsigned long long)d->finish_to_prompt_ns,
                                            (unsigned long long)d->total_output_bytes,
                                            (unsigned long long)d->builtin_output_bytes,
                                            (unsigned long long)d->external_output_bytes,
                                            (unsigned long long)d->rendered_output_bytes,
                                            (unsigned long long)d->skipped_output_bytes,
                                            (unsigned long long)d->shell_write_calls,
                                            (unsigned long long)d->output_feed_calls,
                                            (unsigned long long)d->input_enqueue_calls,
                                            (unsigned long long)d->terminal_refresh_calls,
                                            (unsigned long long)d->update_requests,
                                            (unsigned long long)d->rendered_frames,
                                            (unsigned long long)d->external_read_calls,
                                            (unsigned long long)d->output_batches,
                                            (unsigned long long)d->output_batch_bytes,
                                            (unsigned long long)d->output_batch_max_bytes,
                                            (unsigned long long)d->coalesced_update_requests,
                                            (unsigned long long)d->render_messages_handled,
                                            (unsigned long long)d->rendered_cells,
                                            (unsigned long long)d->skipped_cells,
                                            (unsigned long long)d->empty_cell_skips,
                                            (unsigned long long)d->continuation_cell_skips,
                                            (unsigned long long)d->glyph_draw_calls,
                                            (unsigned long long)d->glyph_rendered_count,
                                            (unsigned long long)d->glyph_cache_hits,
                                            (unsigned long long)d->glyph_cache_misses,
                                            (unsigned long long)d->precolored_cache_hits,
                                            (unsigned long long)d->precolored_cache_misses,
                                            (unsigned long long)d->fallback_cache_hits,
                                            (unsigned long long)d->fallback_cache_misses,
                                            (unsigned long long)d->render_prepare_ns,
                                            (unsigned long long)d->render_snapshot_ns,
                                            (unsigned long long)d->render_dispatch_wait_ns,
                                            (unsigned long long)d->render_worker_total_ns,
                                            (unsigned long long)d->render_worker_max_ns,
                                            (unsigned long long)d->render_flip_ns,
                                            (unsigned long long)d->render_present_ns,
                                            (unsigned long long)d->dirty_rows_rendered,
                                            (unsigned long long)d->full_repaints,
                                            (unsigned long long)d->scroll_blits);
}

static bool winxterm_dstcmd_shell_prompt_after_submit(WinxtermDstcmdShell *shell)
{
    if (!winxterm_dstcmd_shell_prompt(shell)) {
        if (shell != 0) {
            shell->timing_command_active = false;
            shell->timing_verbose_active = false;
        }
        return false;
    }
    if (!shell->timing_command_active) {
        return true;
    }
    uint64_t end_ns = winxterm_dstcmd_shell_timestamp_ns();
    winxterm_dstcmd_shell_finish_timing(shell, end_ns);
    if (!winxterm_dstcmd_shell_write_utf8(shell, "\r\x1b[2K") ||
        !winxterm_dstcmd_shell_write_elapsed_timing(shell) ||
        !winxterm_dstcmd_shell_write_verbose_timing(shell)) {
        return false;
    }
    return winxterm_dstcmd_shell_prompt(shell);
}

static bool winxterm_dstcmd_utf8_to_wide(const char *line, wchar_t **wide_line)
{
    if (wide_line == 0) {
        return false;
    }
    *wide_line = 0;
    if (line == 0) {
        return false;
    }
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line, -1, 0, 0);
    if (count <= 0) {
        return false;
    }
    wchar_t *buffer = (wchar_t *)calloc((size_t)count, sizeof(*buffer));
    if (buffer == 0) {
        return false;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line, -1, buffer, count) <= 0) {
        free(buffer);
        return false;
    }
    *wide_line = buffer;
    return true;
}

typedef enum WinxtermDstcmdControlOp {
    WINXTERM_DSTCMD_CONTROL_NONE = 0,
    WINXTERM_DSTCMD_CONTROL_PIPE,
    WINXTERM_DSTCMD_CONTROL_AND,
    WINXTERM_DSTCMD_CONTROL_SEQUENCE
} WinxtermDstcmdControlOp;

typedef struct WinxtermDstcmdCommandSegment {
    wchar_t *text;
    WinxtermDstcmdControlOp op_after;
} WinxtermDstcmdCommandSegment;

typedef struct WinxtermDstcmdCommandList {
    WinxtermDstcmdCommandSegment *segments;
    size_t count;
} WinxtermDstcmdCommandList;

typedef struct WinxtermDstcmdRedirectScan {
    bool found;
    bool append;
    bool tee_to_terminal;
    size_t remove_start;
    size_t remove_end;
    size_t target_start;
    size_t target_end;
} WinxtermDstcmdRedirectScan;

static void winxterm_dstcmd_command_list_dispose(WinxtermDstcmdCommandList *list)
{
    if (list == 0) {
        return;
    }
    for (size_t i = 0u; i < list->count; ++i) {
        free(list->segments[i].text);
    }
    free(list->segments);
    memset(list, 0, sizeof(*list));
}

static bool winxterm_dstcmd_is_line_space(wchar_t ch)
{
    return ch == L' ' || ch == L'\t';
}

static bool winxterm_dstcmd_redirect_is_token_start(const wchar_t *segment, size_t index)
{
    return segment != 0 && (index == 0u || winxterm_dstcmd_is_line_space(segment[index - 1u]));
}

static bool winxterm_dstcmd_redirect_scan_target_end(const wchar_t *segment,
                                                     size_t length,
                                                     size_t start,
                                                     size_t *end,
                                                     wchar_t *error,
                                                     size_t error_count)
{
    if (segment == 0 || end == 0 || start > length) {
        return false;
    }
    wchar_t quote = L'\0';
    size_t index = start;
    while (index < length) {
        wchar_t ch = segment[index];
        if (quote == L'\0' && winxterm_dstcmd_is_line_space(ch)) {
            break;
        }
        if (ch == L'\\' && quote != L'\'' && index + 1u < length) {
            index += 2u;
            continue;
        }
        if ((ch == L'\'' || ch == L'"') && quote == L'\0') {
            quote = ch;
            ++index;
            continue;
        }
        if (quote != L'\0' && ch == quote) {
            quote = L'\0';
            ++index;
            continue;
        }
        ++index;
    }
    if (quote != L'\0') {
        wcsncpy_s(error, error_count, L"unterminated quote", _TRUNCATE);
        return false;
    }
    *end = index;
    return true;
}

static bool winxterm_dstcmd_redirect_scan_segment(const wchar_t *segment,
                                                  WinxtermDstcmdRedirectScan *scan,
                                                  wchar_t *error,
                                                  size_t error_count)
{
    if (scan != 0) {
        memset(scan, 0, sizeof(*scan));
    }
    if (segment == 0 || scan == 0) {
        return false;
    }

    size_t length = wcslen(segment);
    wchar_t quote = L'\0';
    for (size_t index = 0u; index < length; ++index) {
        wchar_t ch = segment[index];
        if (ch == L'\\' && quote != L'\'' && index + 1u < length) {
            ++index;
            continue;
        }
        if ((ch == L'\'' || ch == L'"') && quote == L'\0') {
            quote = ch;
            continue;
        }
        if (quote != L'\0' && ch == quote) {
            quote = L'\0';
            continue;
        }
        if (quote != L'\0' || ch != L'>') {
            continue;
        }

        if (scan->found) {
            wcsncpy_s(error, error_count, L"multiple stdout redirects", _TRUNCATE);
            return false;
        }

        bool tee_to_terminal = index > 0u &&
                               segment[index - 1u] == L't' &&
                               winxterm_dstcmd_redirect_is_token_start(segment, index - 1u);
        bool append = index + 1u < length && segment[index + 1u] == L'>';
        size_t operator_start = tee_to_terminal ? index - 1u : index;
        size_t operator_end = index + (append ? 2u : 1u);
        size_t target_start = operator_end;
        while (target_start < length && winxterm_dstcmd_is_line_space(segment[target_start])) {
            ++target_start;
        }
        if (target_start >= length) {
            wcsncpy_s(error, error_count, L"missing redirect target", _TRUNCATE);
            return false;
        }

        size_t target_end = target_start;
        if (!winxterm_dstcmd_redirect_scan_target_end(segment,
                                                      length,
                                                      target_start,
                                                      &target_end,
                                                      error,
                                                      error_count)) {
            return false;
        }
        if (target_end == target_start) {
            wcsncpy_s(error, error_count, L"missing redirect target", _TRUNCATE);
            return false;
        }

        scan->found = true;
        scan->append = append;
        scan->tee_to_terminal = tee_to_terminal;
        scan->remove_start = operator_start;
        scan->remove_end = target_end;
        scan->target_start = target_start;
        scan->target_end = target_end;
        index = target_end == 0u ? 0u : target_end - 1u;
    }

    if (quote != L'\0') {
        wcsncpy_s(error, error_count, L"unterminated quote", _TRUNCATE);
        return false;
    }
    return true;
}

static wchar_t *winxterm_dstcmd_copy_segment_without_redirect(const wchar_t *segment,
                                                              const WinxtermDstcmdRedirectScan *scan)
{
    if (segment == 0) {
        return 0;
    }
    if (scan == 0 || !scan->found) {
        return winxterm_dstcmd_wcsdup(segment);
    }
    size_t length = wcslen(segment);
    size_t prefix_count = scan->remove_start <= length ? scan->remove_start : length;
    size_t suffix_start = scan->remove_end <= length ? scan->remove_end : length;
    size_t suffix_count = length - suffix_start;
    wchar_t *copy = (wchar_t *)calloc(prefix_count + suffix_count + 1u, sizeof(*copy));
    if (copy == 0) {
        return 0;
    }
    if (prefix_count != 0u) {
        memcpy(copy, segment, prefix_count * sizeof(*copy));
    }
    if (suffix_count != 0u) {
        memcpy(copy + prefix_count, segment + suffix_start, suffix_count * sizeof(*copy));
    }
    copy[prefix_count + suffix_count] = L'\0';
    return copy;
}

static wchar_t *winxterm_dstcmd_copy_redirect_target_token(const wchar_t *segment,
                                                           const WinxtermDstcmdRedirectScan *scan)
{
    if (segment == 0 || scan == 0 || !scan->found || scan->target_end < scan->target_start) {
        return 0;
    }
    size_t length = scan->target_end - scan->target_start;
    wchar_t *copy = (wchar_t *)calloc(length + 1u, sizeof(*copy));
    if (copy == 0) {
        return 0;
    }
    memcpy(copy, segment + scan->target_start, length * sizeof(*copy));
    copy[length] = L'\0';
    return copy;
}

static bool winxterm_dstcmd_extract_stdout_redirect(WinxtermDstcmdShell *shell,
                                                    const wchar_t *segment,
                                                    int last_status,
                                                    wchar_t **argv_segment,
                                                    WinxtermDstcmdStreamEndpoint *endpoint,
                                                    wchar_t *error,
                                                    size_t error_count)
{
    if (argv_segment != 0) {
        *argv_segment = 0;
    }
    if (endpoint != 0) {
        memset(endpoint, 0, sizeof(*endpoint));
        endpoint->kind = WINXTERM_DSTCMD_STREAM_TERMINAL;
    }
    if (shell == 0 || segment == 0 || argv_segment == 0 || endpoint == 0) {
        return false;
    }

    WinxtermDstcmdRedirectScan scan;
    if (!winxterm_dstcmd_redirect_scan_segment(segment, &scan, error, error_count)) {
        return false;
    }

    *argv_segment = winxterm_dstcmd_copy_segment_without_redirect(segment, &scan);
    if (*argv_segment == 0) {
        wcsncpy_s(error, error_count, L"out of memory", _TRUNCATE);
        return false;
    }
    if (!scan.found) {
        return true;
    }

    wchar_t *target_text = winxterm_dstcmd_copy_redirect_target_token(segment, &scan);
    wchar_t *target = 0;
    if (target_text == 0 ||
        !winxterm_dstcmd_parse_single_token_expanding_status_scratch(&shell->scratch,
                                                                     target_text,
                                                                     last_status,
                                                                     &target,
                                                                     error,
                                                                     error_count)) {
        free(target_text);
        free(target);
        return false;
    }
    free(target_text);

    wchar_t *resolved = (wchar_t *)calloc(WINXTERM_DSTCMD_PATH_CAPACITY, sizeof(*resolved));
    if (resolved == 0) {
        free(target);
        wcsncpy_s(error, error_count, L"out of memory", _TRUNCATE);
        return false;
    }
    bool ok = winxterm_dstcmd_path_resolve_scratch(&shell->scratch,
                                                   shell->cwd,
                                                   target,
                                                   resolved,
                                                   WINXTERM_DSTCMD_PATH_CAPACITY);
    free(target);
    if (!ok) {
        free(resolved);
        wcsncpy_s(error, error_count, L"redirect target path too long", _TRUNCATE);
        return false;
    }

    endpoint->kind = WINXTERM_DSTCMD_STREAM_REDIRECT;
    endpoint->path = resolved;
    endpoint->append = scan.append;
    endpoint->tee_to_terminal = scan.tee_to_terminal;
    return true;
}

static void winxterm_dstcmd_stream_endpoint_dispose(WinxtermDstcmdStreamEndpoint *endpoint)
{
    if (endpoint == 0) {
        return;
    }
    if (endpoint->kind == WINXTERM_DSTCMD_STREAM_REDIRECT) {
        free((wchar_t *)endpoint->path);
    }
    memset(endpoint, 0, sizeof(*endpoint));
}

static bool winxterm_dstcmd_command_list_append(WinxtermDstcmdCommandList *list,
                                                const wchar_t *start,
                                                const wchar_t *end,
                                                WinxtermDstcmdControlOp op_after,
                                                wchar_t *error,
                                                size_t error_count)
{
    if (list == 0 || start == 0 || end == 0 || end < start) {
        return false;
    }
    while (start < end && winxterm_dstcmd_is_line_space(*start)) {
        ++start;
    }
    while (end > start && winxterm_dstcmd_is_line_space(end[-1])) {
        --end;
    }
    if (start == end) {
        if (op_after != WINXTERM_DSTCMD_CONTROL_NONE || list->count != 0u) {
            wcsncpy_s(error, error_count, L"syntax error near control operator", _TRUNCATE);
            return false;
        }
        return true;
    }
    size_t length = (size_t)(end - start);
    wchar_t *text = (wchar_t *)calloc(length + 1u, sizeof(*text));
    if (text == 0) {
        wcsncpy_s(error, error_count, L"out of memory", _TRUNCATE);
        return false;
    }
    memcpy(text, start, length * sizeof(*text));
    WinxtermDstcmdCommandSegment *new_segments =
        (WinxtermDstcmdCommandSegment *)realloc(list->segments, (list->count + 1u) * sizeof(*new_segments));
    if (new_segments == 0) {
        free(text);
        wcsncpy_s(error, error_count, L"out of memory", _TRUNCATE);
        return false;
    }
    list->segments = new_segments;
    list->segments[list->count].text = text;
    list->segments[list->count].op_after = op_after;
    ++list->count;
    return true;
}

static bool winxterm_dstcmd_parse_control_line(const wchar_t *line,
                                               WinxtermDstcmdCommandList *list,
                                               wchar_t *error,
                                               size_t error_count)
{
    if (list == 0) {
        return false;
    }
    memset(list, 0, sizeof(*list));
    if (error != 0 && error_count != 0u) {
        error[0] = L'\0';
    }
    if (line == 0) {
        return true;
    }

    const wchar_t *segment_start = line;
    wchar_t quote = L'\0';
    for (const wchar_t *p = line; *p != L'\0'; ++p) {
        wchar_t ch = *p;
        if (ch == L'\\' && quote != L'\'' && p[1] != L'\0') {
            ++p;
            continue;
        }
        if ((ch == L'\'' || ch == L'"') && quote == L'\0') {
            quote = ch;
            continue;
        }
        if (quote != L'\0' && ch == quote) {
            quote = L'\0';
            continue;
        }
        if (quote != L'\0') {
            continue;
        }

        WinxtermDstcmdControlOp op = WINXTERM_DSTCMD_CONTROL_NONE;
        size_t op_length = 0u;
        if (ch == L'|') {
            op = WINXTERM_DSTCMD_CONTROL_PIPE;
            op_length = 1u;
        } else if (ch == L';') {
            op = WINXTERM_DSTCMD_CONTROL_SEQUENCE;
            op_length = 1u;
        } else if (ch == L'&' && p[1] == L'&') {
            op = WINXTERM_DSTCMD_CONTROL_AND;
            op_length = 2u;
        }
        if (op != WINXTERM_DSTCMD_CONTROL_NONE) {
            if (!winxterm_dstcmd_command_list_append(list,
                                                     segment_start,
                                                     p,
                                                     op,
                                                     error,
                                                     error_count)) {
                winxterm_dstcmd_command_list_dispose(list);
                return false;
            }
            p += op_length - 1u;
            segment_start = p + 1;
        }
    }
    if (quote != L'\0') {
        winxterm_dstcmd_command_list_dispose(list);
        wcsncpy_s(error, error_count, L"unterminated quote", _TRUNCATE);
        return false;
    }
    if (!winxterm_dstcmd_command_list_append(list,
                                             segment_start,
                                             line + wcslen(line),
                                             WINXTERM_DSTCMD_CONTROL_NONE,
                                             error,
                                             error_count)) {
        winxterm_dstcmd_command_list_dispose(list);
        return false;
    }
    if (list->count != 0u && list->segments[list->count - 1u].op_after != WINXTERM_DSTCMD_CONTROL_NONE) {
        winxterm_dstcmd_command_list_dispose(list);
        wcsncpy_s(error, error_count, L"syntax error near control operator", _TRUNCATE);
        return false;
    }
    return true;
}

static void winxterm_dstcmd_shell_capture_begin(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    shell->capture_count = 0u;
    shell->capture_failed = false;
    shell->capture_active = true;
}

static bool winxterm_dstcmd_shell_capture_finish(WinxtermDstcmdShell *shell,
                                                 uint8_t **bytes,
                                                 size_t *byte_count)
{
    if (bytes != 0) {
        *bytes = 0;
    }
    if (byte_count != 0) {
        *byte_count = 0u;
    }
    if (shell == 0) {
        return false;
    }
    shell->capture_active = false;
    if (shell->capture_failed) {
        free(shell->capture_bytes);
        shell->capture_bytes = 0;
        shell->capture_count = 0u;
        shell->capture_capacity = 0u;
        shell->capture_failed = false;
        return false;
    }
    if (bytes != 0 && byte_count != 0) {
        *bytes = shell->capture_bytes;
        *byte_count = shell->capture_count;
        shell->capture_bytes = 0;
        shell->capture_count = 0u;
        shell->capture_capacity = 0u;
    } else {
        shell->capture_count = 0u;
    }
    return true;
}

bool winxterm_dstcmd_shell_expand_alias(WinxtermDstcmdShell *shell,
                                        WinxtermDstcmdArgv *argv,
                                        wchar_t *error,
                                        size_t error_count)
{
    if (shell == 0 || argv == 0 || argv->count <= 0 || argv->items == 0 || argv->items[0] == 0) {
        return true;
    }
    const WinxtermDstcmdAlias *alias = winxterm_dstcmd_shell_find_alias(shell, argv->items[0]);
    if (alias == 0) {
        return true;
    }
    wchar_t *target = winxterm_dstcmd_wcsdup(alias->target);
    if (target == 0) {
        if (error != 0 && error_count != 0u) {
            wcsncpy_s(error, error_count, L"out of memory", _TRUNCATE);
        }
        return false;
    }
    free(argv->items[0]);
    argv->items[0] = target;
    return true;
}

static int winxterm_dstcmd_shell_run_pipeline_mode(WinxtermDstcmdShell *shell,
                                                   const WinxtermDstcmdCommandList *list,
                                                   size_t start,
                                                   size_t end,
                                                   wchar_t *error,
                                                   size_t error_count,
                                                   bool background,
                                                   bool connectable_stdin,
                                                   uint64_t *job_id)
{
    if (shell == 0 || list == 0 || start > end || end >= list->count) {
        return 0;
    }
    size_t stage_count = end - start + 1u;
    WinxtermDstcmdArgv *argvs =
        (WinxtermDstcmdArgv *)calloc(stage_count, sizeof(*argvs));
    WinxtermDstcmdExecStage *stages =
        (WinxtermDstcmdExecStage *)calloc(stage_count, sizeof(*stages));
    if (argvs == 0 || stages == 0) {
        free(argvs);
        free(stages);
        (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: out of memory\r\n");
        return 1;
    }

    int status = 0;
    for (size_t i = 0u; i < stage_count; ++i) {
        const wchar_t *segment = list->segments[start + i].text;
        wchar_t *argv_segment = 0;
        WinxtermDstcmdStreamEndpoint redirect_endpoint;
        if (!winxterm_dstcmd_extract_stdout_redirect(shell,
                                                     segment,
                                                     shell->last_status,
                                                     &argv_segment,
                                                     &redirect_endpoint,
                                                     error,
                                                     error_count)) {
            (void)winxterm_dstcmd_shell_write_widef(shell, L"dstcmd: %ls\r\n", error);
            free(argv_segment);
            status = 2;
            goto cleanup;
        }
        stages[i].stdout_endpoint = redirect_endpoint;
        if (redirect_endpoint.kind == WINXTERM_DSTCMD_STREAM_REDIRECT && i + 1u != stage_count) {
            (void)winxterm_dstcmd_shell_write_wide(
                shell,
                L"dstcmd: stdout redirect is only supported on the final pipeline stage\r\n");
            free(argv_segment);
            status = 2;
            goto cleanup;
        }
        if (!winxterm_dstcmd_parse_line_expanding_globs_and_status_scratch(&shell->scratch,
                                                                           argv_segment,
                                                                           shell->cwd,
                                                                           shell->last_status,
                                                                           argvs + i,
                                                                           error,
                                                                           error_count)) {
            (void)winxterm_dstcmd_shell_write_widef(shell, L"dstcmd: %ls\r\n", error);
            free(argv_segment);
            status = 2;
            goto cleanup;
        }
        free(argv_segment);
        if (argvs[i].count == 0) {
            if (redirect_endpoint.kind == WINXTERM_DSTCMD_STREAM_REDIRECT) {
                (void)winxterm_dstcmd_shell_write_wide(shell,
                                                       L"dstcmd: missing command before redirect\r\n");
                status = 2;
                goto cleanup;
            }
            continue;
        }
        if (!winxterm_dstcmd_shell_expand_alias(shell, argvs + i, error, error_count)) {
            (void)winxterm_dstcmd_shell_write_widef(shell, L"dstcmd: %ls\r\n", error);
            status = 1;
            goto cleanup;
        }
        if (stage_count == 1u && wcscmp(argvs[i].items[0], L"exit") == 0) {
            shell->exit_requested = true;
            status = 0;
            goto cleanup;
        }

        stages[i].argv = argvs + i;
        stages[i].stdin_endpoint.kind = i == 0u ?
            WINXTERM_DSTCMD_STREAM_TERMINAL : WINXTERM_DSTCMD_STREAM_PIPE;
        if (redirect_endpoint.kind != WINXTERM_DSTCMD_STREAM_REDIRECT) {
            stages[i].stdout_endpoint.kind = i + 1u == stage_count ?
                WINXTERM_DSTCMD_STREAM_TERMINAL : WINXTERM_DSTCMD_STREAM_PIPE;
        }
        stages[i].stderr_endpoint.kind = WINXTERM_DSTCMD_STREAM_TERMINAL;
        stages[i].isolate_shell_state = stage_count != 1u;
    }

    status = background ? winxterm_dstcmd_exec_run_managed_stages_background(
                              shell, stages, stage_count, connectable_stdin, job_id) :
                          winxterm_dstcmd_exec_run(shell, stages, stage_count);

cleanup:
    for (size_t i = 0u; i < stage_count; ++i) {
        winxterm_dstcmd_stream_endpoint_dispose(&stages[i].stdout_endpoint);
        winxterm_dstcmd_argv_dispose(argvs + i);
    }
    free(argvs);
    free(stages);
    return status;
}

static int winxterm_dstcmd_shell_run_pipeline(WinxtermDstcmdShell *shell,
                                              const WinxtermDstcmdCommandList *list,
                                              size_t start,
                                              size_t end,
                                              wchar_t *error,
                                              size_t error_count)
{
    return winxterm_dstcmd_shell_run_pipeline_mode(shell, list, start, end,
                                                   error, error_count,
                                                   false, false, 0);
}

static int winxterm_dstcmd_shell_run_command_list(WinxtermDstcmdShell *shell,
                                                  const WinxtermDstcmdCommandList *list,
                                                  wchar_t *error,
                                                  size_t error_count)
{
    if (shell == 0 || list == 0 || list->count == 0u) {
        return 0;
    }
    bool should_run = true;
    size_t index = 0u;
    while (index < list->count && !shell->exit_requested) {
        size_t pipeline_end = index;
        while (pipeline_end + 1u < list->count &&
               list->segments[pipeline_end].op_after == WINXTERM_DSTCMD_CONTROL_PIPE) {
            ++pipeline_end;
        }
        if (should_run) {
            shell->last_status = winxterm_dstcmd_shell_run_pipeline(shell,
                                                                    list,
                                                                    index,
                                                                    pipeline_end,
                                                                    error,
                                                                    error_count);
        }
        WinxtermDstcmdControlOp next = list->segments[pipeline_end].op_after;
        should_run = next != WINXTERM_DSTCMD_CONTROL_AND || shell->last_status == 0;
        index = pipeline_end + 1u;
    }
    return shell->last_status;
}

int winxterm_dstcmd_shell_submit_line(WinxtermDstcmdShell *shell, const wchar_t *line)
{
    if (shell == 0 || line == 0) {
        return 1;
    }
    winxterm_dstcmd_scratch_reset(&shell->scratch);
    WinxtermDstcmdCommandList commands;
    wchar_t error[128];
    WinxtermCommandDiagnostics *diagnostics =
        shell->timing_verbose_active ? &shell->timing_diagnostics : 0;
    uint64_t phase_start_ns = diagnostics != 0 ? winxterm_dstcmd_shell_timestamp_ns() : 0u;
    if (!winxterm_dstcmd_parse_control_line(line, &commands, error, 128u)) {
        if (diagnostics != 0) {
            uint64_t phase_end_ns = winxterm_dstcmd_shell_timestamp_ns();
            winxterm_dstcmd_shell_add_diag_ns(shell,
                                              &diagnostics->parse_ns,
                                              phase_end_ns >= phase_start_ns ?
                                                phase_end_ns - phase_start_ns : 0u);
        }
        (void)winxterm_dstcmd_shell_write_widef(shell, L"dstcmd: %ls\r\n", error);
        shell->last_status = 2;
        winxterm_dstcmd_scratch_reset(&shell->scratch);
        return shell->last_status;
    }
    if (diagnostics != 0) {
        uint64_t phase_end_ns = winxterm_dstcmd_shell_timestamp_ns();
        winxterm_dstcmd_shell_add_diag_ns(shell,
                                          &diagnostics->parse_ns,
                                          phase_end_ns >= phase_start_ns ?
                                            phase_end_ns - phase_start_ns : 0u);
    }
    if (commands.count == 0u) {
        shell->last_status = 0;
        winxterm_dstcmd_command_list_dispose(&commands);
        winxterm_dstcmd_scratch_reset(&shell->scratch);
        return 0;
    }
    if (shell->output_lock_initialized) {
        EnterCriticalSection(&shell->output_lock);
        shell->command_running = true;
        LeaveCriticalSection(&shell->output_lock);
    }
    (void)winxterm_dstcmd_shell_run_command_list(shell, &commands, error, 128u);
    if (shell->output_lock_initialized) {
        EnterCriticalSection(&shell->output_lock);
        shell->command_running = false;
        LeaveCriticalSection(&shell->output_lock);
    }
    winxterm_dstcmd_command_list_dispose(&commands);
    winxterm_dstcmd_scratch_reset(&shell->scratch);
    return shell->last_status;
}

int winxterm_dstcmd_shell_run_background_line(WinxtermDstcmdShell *shell,
                                              const wchar_t *line,
                                              bool connectable_stdin,
                                              uint64_t *job_id)
{
    if (job_id != 0) *job_id = 0u;
    if (shell == 0 || line == 0 || job_id == 0) return 1;
    winxterm_dstcmd_scratch_reset(&shell->scratch);
    WinxtermDstcmdCommandList commands;
    wchar_t error[128];
    if (!winxterm_dstcmd_parse_control_line(line, &commands, error, 128u)) {
        (void)winxterm_dstcmd_shell_write_widef(shell, L"job: %ls\r\n", error);
        winxterm_dstcmd_scratch_reset(&shell->scratch);
        return 2;
    }
    bool one_pipeline = commands.count != 0u;
    for (size_t i = 0u; one_pipeline && i + 1u < commands.count; ++i) {
        one_pipeline = commands.segments[i].op_after == WINXTERM_DSTCMD_CONTROL_PIPE;
    }
    if (!one_pipeline) {
        (void)winxterm_dstcmd_shell_write_wide(
            shell, L"job: run/open accepts one command or pipeline\r\n");
        winxterm_dstcmd_command_list_dispose(&commands);
        winxterm_dstcmd_scratch_reset(&shell->scratch);
        return 2;
    }
    int status = winxterm_dstcmd_shell_run_pipeline_mode(
        shell, &commands, 0u, commands.count - 1u, error, 128u,
        true, connectable_stdin, job_id);
    winxterm_dstcmd_command_list_dispose(&commands);
    winxterm_dstcmd_scratch_reset(&shell->scratch);
    return status;
}

static void winxterm_dstcmd_shell_backspace(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || shell->line_cursor == 0u) {
        return;
    }
    size_t delete_start =
        winxterm_dstcmd_utf8_prev_boundary(shell->line, shell->line_length, shell->line_cursor);
    size_t delete_count = shell->line_cursor - delete_start;
    memmove(shell->line + delete_start,
            shell->line + shell->line_cursor,
            shell->line_length - shell->line_cursor + 1u);
    shell->line_length -= delete_count;
    shell->line_cursor = delete_start;
    (void)winxterm_dstcmd_shell_refresh_line(shell);
}

static bool winxterm_dstcmd_shell_is_space_byte(char ch)
{
    return ch == ' ' || ch == '\t';
}

static bool winxterm_dstcmd_shell_is_unescaped_space_byte(const char *line, size_t index)
{
    if (line == 0 || line[index] == '\0' || !winxterm_dstcmd_shell_is_space_byte(line[index])) {
        return false;
    }
    return !(line[index] == ' ' && index > 0u && line[index - 1u] == '\\');
}

static void winxterm_dstcmd_shell_delete_range(WinxtermDstcmdShell *shell, size_t start, size_t end)
{
    if (shell == 0 || start >= end || end > shell->line_length) {
        return;
    }
    memmove(shell->line + start,
            shell->line + end,
            shell->line_length - end + 1u);
    shell->line_length -= end - start;
    shell->line_cursor = start;
    (void)winxterm_dstcmd_shell_refresh_line(shell);
}

static void winxterm_dstcmd_shell_delete_at_cursor(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || shell->line_cursor >= shell->line_length) {
        return;
    }
    size_t delete_end =
        winxterm_dstcmd_utf8_next_boundary(shell->line, shell->line_length, shell->line_cursor);
    memmove(shell->line + shell->line_cursor,
            shell->line + delete_end,
            shell->line_length - delete_end + 1u);
    shell->line_length -= delete_end - shell->line_cursor;
    (void)winxterm_dstcmd_shell_refresh_line(shell);
}

static void winxterm_dstcmd_shell_delete_word_backward(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || shell->line_cursor == 0u) {
        return;
    }
    size_t delete_start = shell->line_cursor;
    while (delete_start > 0u &&
           winxterm_dstcmd_shell_is_unescaped_space_byte(shell->line, delete_start - 1u)) {
        --delete_start;
    }
    while (delete_start > 0u &&
           !winxterm_dstcmd_shell_is_unescaped_space_byte(shell->line, delete_start - 1u)) {
        --delete_start;
    }
    winxterm_dstcmd_shell_delete_range(shell, delete_start, shell->line_cursor);
}

static void winxterm_dstcmd_shell_delete_word_forward(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || shell->line_cursor >= shell->line_length) {
        return;
    }
    size_t delete_end = shell->line_cursor;
    if (winxterm_dstcmd_shell_is_unescaped_space_byte(shell->line, delete_end)) {
        while (delete_end < shell->line_length &&
               winxterm_dstcmd_shell_is_unescaped_space_byte(shell->line, delete_end)) {
            ++delete_end;
        }
    } else {
        while (delete_end < shell->line_length &&
               !winxterm_dstcmd_shell_is_unescaped_space_byte(shell->line, delete_end)) {
            ++delete_end;
        }
    }
    winxterm_dstcmd_shell_delete_range(shell, shell->line_cursor, delete_end);
}

static void winxterm_dstcmd_shell_move_left(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || shell->line_cursor == 0u) {
        return;
    }
    shell->line_cursor =
        winxterm_dstcmd_utf8_prev_boundary(shell->line, shell->line_length, shell->line_cursor);
    (void)winxterm_dstcmd_shell_refresh_line(shell);
}

static void winxterm_dstcmd_shell_move_right(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || shell->line_cursor >= shell->line_length) {
        return;
    }
    shell->line_cursor =
        winxterm_dstcmd_utf8_next_boundary(shell->line, shell->line_length, shell->line_cursor);
    (void)winxterm_dstcmd_shell_refresh_line(shell);
}

static void winxterm_dstcmd_shell_move_word_left(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || shell->line_cursor == 0u) {
        return;
    }
    size_t cursor = shell->line_cursor;
    while (cursor > 0u) {
        --cursor;
        if (winxterm_dstcmd_shell_is_unescaped_space_byte(shell->line, cursor)) {
            shell->line_cursor = cursor;
            (void)winxterm_dstcmd_shell_refresh_line(shell);
            return;
        }
    }
    shell->line_cursor = 0u;
    (void)winxterm_dstcmd_shell_refresh_line(shell);
}

static void winxterm_dstcmd_shell_move_word_right(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || shell->line_cursor >= shell->line_length) {
        return;
    }
    size_t cursor = shell->line_cursor + 1u;
    while (cursor < shell->line_length) {
        if (winxterm_dstcmd_shell_is_unescaped_space_byte(shell->line, cursor)) {
            shell->line_cursor = cursor;
            (void)winxterm_dstcmd_shell_refresh_line(shell);
            return;
        }
        ++cursor;
    }
    shell->line_cursor = shell->line_length;
    (void)winxterm_dstcmd_shell_refresh_line(shell);
}

static void winxterm_dstcmd_shell_move_home(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    shell->line_cursor = 0u;
    (void)winxterm_dstcmd_shell_refresh_line(shell);
}

static void winxterm_dstcmd_shell_move_end(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    shell->line_cursor = shell->line_length;
    (void)winxterm_dstcmd_shell_refresh_line(shell);
}

static void winxterm_dstcmd_shell_kill_to_end(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    shell->line[shell->line_cursor] = '\0';
    shell->line_length = shell->line_cursor;
    (void)winxterm_dstcmd_shell_refresh_line(shell);
}

static void winxterm_dstcmd_shell_kill_line(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    shell->line[0] = '\0';
    shell->line_length = 0u;
    shell->line_cursor = 0u;
    (void)winxterm_dstcmd_shell_refresh_line(shell);
}

static void winxterm_dstcmd_shell_clear_line(WinxtermDstcmdShell *shell)
{
    shell->line_length = 0u;
    shell->line_cursor = 0u;
    shell->line[0] = '\0';
    shell->saved_history_line[0] = '\0';
    shell->history_index = shell->history_count;
    if (shell->history_state_lock_initialized) {
        EnterCriticalSection(&shell->history_state_lock);
        shell->persisted_history_active = false;
        shell->persisted_history_index = 0u;
        LeaveCriticalSection(&shell->history_state_lock);
    }
}

static bool winxterm_dstcmd_shell_submit_current_line(WinxtermDstcmdShell *shell,
                                                      uint64_t command_entered_ns)
{
    if (shell->output_lock_initialized) {
        EnterCriticalSection(&shell->output_lock);
        shell->prompt_active = false;
        LeaveCriticalSection(&shell->output_lock);
    }
    winxterm_dstcmd_shell_begin_timing(shell, command_entered_ns);
    (void)winxterm_dstcmd_shell_move_to_line_offset(shell, shell->line_length);
    shell->prompt_cursor_saved = false;
    (void)winxterm_dstcmd_shell_write_utf8(shell, "\r\n");
    wchar_t *wide_line = 0;
    bool ok = winxterm_dstcmd_utf8_to_wide(shell->line, &wide_line);
    if (!ok) {
        (void)winxterm_dstcmd_shell_write_utf8(shell, "dstcmd: invalid UTF-8 input\r\n");
        winxterm_dstcmd_shell_clear_line(shell);
        shell->timing_command_active = false;
        shell->timing_verbose_active = false;
        return true;
    }
    winxterm_dstcmd_shell_record_history(shell, shell->line);
    (void)winxterm_dstcmd_shell_submit_line(shell, wide_line);
    free(wide_line);
    winxterm_dstcmd_shell_clear_line(shell);
    if (!shell->exit_requested) {
        winxterm_dstcmd_shell_note_command_finished(shell);
        return winxterm_dstcmd_shell_prompt_after_submit(shell);
    }
    shell->timing_command_active = false;
    shell->timing_verbose_active = false;
    return true;
}

static bool winxterm_dstcmd_shell_append_input_byte(WinxtermDstcmdShell *shell, uint8_t byte)
{
    if (shell->line_length + 1u >= WINXTERM_DSTCMD_LINE_CAPACITY) {
        return winxterm_dstcmd_shell_write_utf8(shell, "\a");
    }
    memmove(shell->line + shell->line_cursor + 1u,
            shell->line + shell->line_cursor,
            shell->line_length - shell->line_cursor + 1u);
    shell->line[shell->line_cursor++] = (char)byte;
    ++shell->line_length;
    shell->line[shell->line_length] = '\0';
    return winxterm_dstcmd_shell_refresh_line(shell);
}

static bool winxterm_dstcmd_shell_line_has_open_quote(const WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    char quote = '\0';
    for (size_t i = 0u; i < shell->line_length; ++i) {
        char ch = shell->line[i];
        if (ch == '\\' && quote != '\'' && i + 1u < shell->line_length) {
            ++i;
            continue;
        }
        if ((ch == '\'' || ch == '"') && quote == '\0') {
            quote = ch;
        } else if (quote != '\0' && ch == quote) {
            quote = '\0';
        }
    }
    return quote != '\0';
}

static void winxterm_dstcmd_shell_history_previous(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    if (shell->history_index == shell->history_count && !shell->persisted_history_active) {
        memcpy(shell->saved_history_line, shell->line, shell->line_length + 1u);
    }

    if (!shell->persisted_history_active && shell->history_index > 0u) {
        --shell->history_index;
        (void)winxterm_dstcmd_shell_replace_line(shell, shell->history[shell->history_index]);
        return;
    }

    size_t start = 0u;
    if (shell->history_state_lock_initialized) {
        EnterCriticalSection(&shell->history_state_lock);
        if (shell->persisted_history_active) {
            start = shell->persisted_history_index + 1u;
        }
        LeaveCriticalSection(&shell->history_state_lock);
    }

    size_t persisted_index = 0u;
    char *persisted_command = 0;
    if (!winxterm_dstcmd_shell_history_copy_persisted_at_or_after(shell,
                                                                  start,
                                                                  &persisted_index,
                                                                  &persisted_command)) {
        return;
    }
    if (shell->history_state_lock_initialized) {
        EnterCriticalSection(&shell->history_state_lock);
        shell->persisted_history_active = true;
        shell->persisted_history_index = persisted_index;
        LeaveCriticalSection(&shell->history_state_lock);
    }
    (void)winxterm_dstcmd_shell_replace_line(shell, persisted_command);
    free(persisted_command);
}

static void winxterm_dstcmd_shell_history_next(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    if (shell->persisted_history_active) {
        size_t current_index = 0u;
        if (shell->history_state_lock_initialized) {
            EnterCriticalSection(&shell->history_state_lock);
            current_index = shell->persisted_history_index;
            LeaveCriticalSection(&shell->history_state_lock);
        }

        size_t persisted_index = 0u;
        char *persisted_command = 0;
        if (winxterm_dstcmd_shell_history_copy_persisted_before(shell,
                                                               current_index,
                                                               &persisted_index,
                                                               &persisted_command)) {
            if (shell->history_state_lock_initialized) {
                EnterCriticalSection(&shell->history_state_lock);
                shell->persisted_history_index = persisted_index;
                LeaveCriticalSection(&shell->history_state_lock);
            }
            (void)winxterm_dstcmd_shell_replace_line(shell, persisted_command);
            free(persisted_command);
            return;
        }

        if (shell->history_state_lock_initialized) {
            EnterCriticalSection(&shell->history_state_lock);
            shell->persisted_history_active = false;
            shell->persisted_history_index = 0u;
            LeaveCriticalSection(&shell->history_state_lock);
        }
        if (shell->history_count != 0u) {
            shell->history_index = 0u;
            (void)winxterm_dstcmd_shell_replace_line(shell, shell->history[0]);
            return;
        }
        shell->history_index = shell->history_count;
        (void)winxterm_dstcmd_shell_replace_line(shell, shell->saved_history_line);
        shell->saved_history_line[0] = '\0';
        return;
    }

    if (shell->history_count == 0u || shell->history_index == shell->history_count) {
        return;
    }
    ++shell->history_index;
    if (shell->history_index == shell->history_count) {
        (void)winxterm_dstcmd_shell_replace_line(shell, shell->saved_history_line);
        shell->saved_history_line[0] = '\0';
        return;
    }
    (void)winxterm_dstcmd_shell_replace_line(shell, shell->history[shell->history_index]);
}

static bool winxterm_dstcmd_shell_replace_range(WinxtermDstcmdShell *shell,
                                                size_t start,
                                                size_t end,
                                                const char *replacement)
{
    if (shell == 0 || replacement == 0 || start > end || end > shell->line_length) {
        return false;
    }
    size_t replacement_length = strlen(replacement);
    size_t new_length = shell->line_length - (end - start) + replacement_length;
    if (new_length >= WINXTERM_DSTCMD_LINE_CAPACITY) {
        return winxterm_dstcmd_shell_write_utf8(shell, "\a");
    }
    memmove(shell->line + start + replacement_length,
            shell->line + end,
            shell->line_length - end + 1u);
    memcpy(shell->line + start, replacement, replacement_length);
    shell->line_length = new_length;
    shell->line_cursor = start + replacement_length;
    return winxterm_dstcmd_shell_refresh_line(shell);
}

static wchar_t *winxterm_dstcmd_utf8_token_to_wide(const char *token, size_t length)
{
    if (token == 0) {
        return 0;
    }
    if (length > INT_MAX) {
        return 0;
    }
    int wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, token, (int)length, 0, 0);
    if (wide_count < 0) {
        return 0;
    }
    wchar_t *wide = (wchar_t *)calloc((size_t)wide_count + 1u, sizeof(*wide));
    if (wide == 0) {
        return 0;
    }
    if (wide_count != 0 &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, token, (int)length, wide, wide_count) <= 0) {
        free(wide);
        return 0;
    }
    wide[wide_count] = L'\0';
    return wide;
}

static char *winxterm_dstcmd_wide_to_utf8_alloc(const wchar_t *text)
{
    if (text == 0) {
        return 0;
    }
    int byte_count = WideCharToMultiByte(CP_UTF8, 0, text, -1, 0, 0, 0, 0);
    if (byte_count <= 0) {
        return 0;
    }
    char *utf8 = (char *)calloc((size_t)byte_count, sizeof(*utf8));
    if (utf8 == 0) {
        return 0;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, byte_count, 0, 0) <= 0) {
        free(utf8);
        return 0;
    }
    return utf8;
}

static bool winxterm_dstcmd_wide_starts_with_ci(const wchar_t *text, const wchar_t *prefix)
{
    if (text == 0 || prefix == 0) {
        return false;
    }
    size_t prefix_length = wcslen(prefix);
    return _wcsnicmp(text, prefix, prefix_length) == 0;
}

static bool winxterm_dstcmd_wide_starts_with_cs(const wchar_t *text, const wchar_t *prefix)
{
    if (text == 0 || prefix == 0) {
        return false;
    }
    size_t prefix_length = wcslen(prefix);
    return wcsncmp(text, prefix, prefix_length) == 0;
}

static bool winxterm_dstcmd_completion_add(wchar_t **matches,
                                           size_t *count,
                                           size_t capacity,
                                           const wchar_t *value)
{
    if (matches == 0 || count == 0 || value == 0 || *count >= capacity) {
        return false;
    }
    for (size_t i = 0u; i < *count; ++i) {
        if (_wcsicmp(matches[i], value) == 0) {
            return true;
        }
    }
    matches[*count] = winxterm_dstcmd_wcsdup(value);
    if (matches[*count] == 0) {
        return false;
    }
    ++(*count);
    return true;
}

static void winxterm_dstcmd_completion_free(wchar_t **matches, size_t count)
{
    for (size_t i = 0u; i < count; ++i) {
        free(matches[i]);
    }
}

static void winxterm_dstcmd_shell_clear_completion_repeat(WinxtermDstcmdShell *shell)
{
    if (shell != 0) {
        shell->completion_repeat_valid = false;
        shell->completion_repeat_line[0] = '\0';
        shell->completion_repeat_cursor = 0u;
    }
}

static void winxterm_dstcmd_shell_note_completion_no_change(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || shell->line_length >= WINXTERM_DSTCMD_LINE_CAPACITY) {
        return;
    }
    memcpy(shell->completion_repeat_line, shell->line, shell->line_length + 1u);
    shell->completion_repeat_cursor = shell->line_cursor;
    shell->completion_repeat_valid = true;
}

static bool winxterm_dstcmd_shell_completion_is_repeat(const WinxtermDstcmdShell *shell)
{
    return shell != 0 &&
           shell->completion_repeat_valid &&
           shell->completion_repeat_cursor == shell->line_cursor &&
           strcmp(shell->completion_repeat_line, shell->line) == 0;
}

static bool winxterm_dstcmd_completion_is_slash(wchar_t ch)
{
    return ch == L'\\' || ch == L'/';
}

static bool winxterm_dstcmd_completion_copy_unescaped_spaces(const wchar_t *text,
                                                             wchar_t *out,
                                                             size_t out_count)
{
    if (text == 0 || out == 0 || out_count == 0u) {
        return false;
    }
    size_t offset = 0u;
    for (size_t i = 0u; text[i] != L'\0'; ++i) {
        wchar_t ch = text[i];
        if (ch == L'\\' && text[i + 1u] == L' ') {
            ch = L' ';
            ++i;
        }
        if (offset + 1u >= out_count) {
            return false;
        }
        out[offset++] = ch;
    }
    out[offset] = L'\0';
    return true;
}

static bool winxterm_dstcmd_completion_copy_escaped_spaces(const wchar_t *text,
                                                           wchar_t *out,
                                                           size_t out_count)
{
    if (text == 0 || out == 0 || out_count == 0u) {
        return false;
    }
    size_t offset = 0u;
    for (const wchar_t *p = text; *p != L'\0'; ++p) {
        if (*p == L' ') {
            if (offset + 2u >= out_count) {
                return false;
            }
            out[offset++] = L'\\';
            out[offset++] = L' ';
            continue;
        }
        if (offset + 1u >= out_count) {
            return false;
        }
        out[offset++] = *p;
    }
    out[offset] = L'\0';
    return true;
}

static bool winxterm_dstcmd_completion_has_slash(const wchar_t *text)
{
    if (text == 0) {
        return false;
    }
    for (const wchar_t *p = text; *p != L'\0'; ++p) {
        if (*p == L'/' || (*p == L'\\' && p[1] != L' ')) {
            return true;
        }
    }
    return false;
}

static bool winxterm_dstcmd_completion_ends_with_slash(const wchar_t *text)
{
    size_t length = text != 0 ? wcslen(text) : 0u;
    return length != 0u && winxterm_dstcmd_completion_is_slash(text[length - 1u]);
}

static bool winxterm_dstcmd_completion_exact_directory(WinxtermDstcmdShell *shell,
                                                       const wchar_t *token,
                                                       wchar_t *completion,
                                                       size_t completion_count)
{
    if (shell == 0 || token == 0 || completion == 0 || completion_count == 0u ||
        token[0] == L'\0' || winxterm_dstcmd_completion_ends_with_slash(token)) {
        return false;
    }
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    wchar_t *resolved = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *unescaped_token = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *display_token = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *raw_completion = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    if (resolved == 0 || unescaped_token == 0 || display_token == 0 || raw_completion == 0) {
        winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
        return false;
    }
    bool ok = false;
    if (!winxterm_dstcmd_completion_copy_unescaped_spaces(token,
                                                          unescaped_token,
                                                          WINXTERM_DSTCMD_PATH_CAPACITY) ||
        !winxterm_dstcmd_path_resolve_scratch(&shell->scratch,
                                             shell->cwd,
                                             unescaped_token,
                                             resolved,
                                             WINXTERM_DSTCMD_PATH_CAPACITY) ||
        !winxterm_dstcmd_path_is_directory_scratch(&shell->scratch, resolved)) {
        goto cleanup;
    }
    if (!winxterm_dstcmd_path_to_display(unescaped_token, display_token, WINXTERM_DSTCMD_PATH_CAPACITY)) {
        goto cleanup;
    }
    ok = _snwprintf_s(raw_completion,
                      WINXTERM_DSTCMD_PATH_CAPACITY,
                      _TRUNCATE,
                      L"%ls/",
                      display_token) >= 0 &&
         winxterm_dstcmd_completion_copy_escaped_spaces(raw_completion, completion, completion_count);

cleanup:
    winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
    return ok;
}

static bool winxterm_dstcmd_completion_known_script_or_exe(const wchar_t *name)
{
    const wchar_t *extension = wcsrchr(name, L'.');
    return extension != 0 &&
           (_wcsicmp(extension, L".exe") == 0 ||
            _wcsicmp(extension, L".com") == 0 ||
            _wcsicmp(extension, L".ps1") == 0 ||
            _wcsicmp(extension, L".cmd") == 0 ||
            _wcsicmp(extension, L".bat") == 0 ||
            _wcsicmp(extension, L".sh") == 0);
}

typedef struct WinxtermDstcmdCompletionContext {
    wchar_t **matches;
    size_t *count;
    size_t capacity;
    const wchar_t *prefix;
} WinxtermDstcmdCompletionContext;

static bool winxterm_dstcmd_completion_add_builtin_name(const wchar_t *name, void *context)
{
    WinxtermDstcmdCompletionContext *completion = (WinxtermDstcmdCompletionContext *)context;
    if (completion == 0 || name == 0) {
        return false;
    }
    if (winxterm_dstcmd_wide_starts_with_ci(name, completion->prefix)) {
        (void)winxterm_dstcmd_completion_add(completion->matches,
                                             completion->count,
                                             completion->capacity,
                                             name);
    }
    return *completion->count < completion->capacity;
}

static void winxterm_dstcmd_completion_add_builtins(WinxtermDstcmdShell *shell,
                                                    wchar_t **matches,
                                                    size_t *count,
                                                    size_t capacity,
                                                    const wchar_t *prefix)
{
    WinxtermDstcmdCompletionContext context = {matches, count, capacity, prefix};
    winxterm_dstcmd_for_each_builtin_name(winxterm_dstcmd_completion_add_builtin_name, &context);
    if (shell == 0) {
        return;
    }
    for (size_t i = 0u; i < shell->alias_count && *count < capacity; ++i) {
        const WinxtermDstcmdAlias *alias = shell->aliases + i;
        if (alias->name != 0 && winxterm_dstcmd_wide_starts_with_ci(alias->name, prefix)) {
            (void)winxterm_dstcmd_completion_add(matches, count, capacity, alias->name);
        }
    }
}

static void winxterm_dstcmd_completion_add_path_commands(wchar_t **matches,
                                                        size_t *count,
                                                        size_t capacity,
                                                        const wchar_t *prefix,
                                                        WinxtermDstcmdScratch *scratch)
{
    DWORD needed = GetEnvironmentVariableW(L"PATH", 0, 0);
    if (needed == 0u) {
        return;
    }
    wchar_t *path = (wchar_t *)calloc((size_t)needed + 1u, sizeof(*path));
    if (path == 0) {
        return;
    }
    DWORD length = GetEnvironmentVariableW(L"PATH", path, needed + 1u);
    if (length == 0u || length > needed) {
        free(path);
        return;
    }
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(scratch);
    wchar_t *escaped_name = winxterm_dstcmd_scratch_alloc_path(scratch);
    wchar_t *unescaped_prefix = winxterm_dstcmd_scratch_alloc_path(scratch);
    if (escaped_name == 0 || unescaped_prefix == 0 ||
        !winxterm_dstcmd_completion_copy_unescaped_spaces(prefix,
                                                          unescaped_prefix,
                                                          WINXTERM_DSTCMD_PATH_CAPACITY)) {
        winxterm_dstcmd_scratch_rewind(scratch, mark);
        free(path);
        return;
    }
    wchar_t *entry = path;
    while (entry != 0 && *entry != L'\0') {
        wchar_t *separator = wcschr(entry, L';');
        if (separator != 0) {
            *separator = L'\0';
        }
        if (entry[0] != L'\0') {
            WinxtermDstcmdDirIter iter;
            if (winxterm_dstcmd_dir_iter_open_scratch(scratch, entry, &iter)) {
                const WIN32_FIND_DATAW *data = 0;
                while (*count < capacity && winxterm_dstcmd_dir_iter_next(&iter, &data)) {
                    if ((data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0u &&
                        winxterm_dstcmd_completion_known_script_or_exe(data->cFileName) &&
                        winxterm_dstcmd_wide_starts_with_ci(data->cFileName, unescaped_prefix)) {
                        if (winxterm_dstcmd_completion_copy_escaped_spaces(data->cFileName,
                                                                           escaped_name,
                                                                           WINXTERM_DSTCMD_PATH_CAPACITY)) {
                            (void)winxterm_dstcmd_completion_add(matches, count, capacity, escaped_name);
                        }
                    }
                }
                winxterm_dstcmd_dir_iter_close(&iter);
            }
        }
        entry = separator != 0 ? separator + 1 : 0;
    }
    winxterm_dstcmd_scratch_rewind(scratch, mark);
    free(path);
}

static bool winxterm_dstcmd_completion_split_path(const wchar_t *token,
                                                  wchar_t *directory_part,
                                                  size_t directory_count,
                                                  wchar_t *name_part,
                                                  size_t name_count)
{
    if (token == 0 || directory_part == 0 || name_part == 0 ||
        directory_count == 0u || name_count == 0u) {
        return false;
    }
    const wchar_t *base = token;
    for (const wchar_t *p = token; *p != L'\0'; ++p) {
        if (winxterm_dstcmd_completion_is_slash(*p)) {
            base = p + 1;
        }
    }
    size_t directory_length = (size_t)(base - token);
    if (directory_length >= directory_count || wcslen(base) >= name_count) {
        return false;
    }
    memcpy(directory_part, token, directory_length * sizeof(*directory_part));
    directory_part[directory_length] = L'\0';
    wcscpy_s(name_part, name_count, base);
    return true;
}

static void winxterm_dstcmd_completion_add_files(WinxtermDstcmdShell *shell,
                                                 wchar_t **matches,
                                                 size_t *count,
                                                 size_t capacity,
                                                 const wchar_t *token)
{
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    wchar_t *unescaped_token = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *directory_part = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *name_part = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *directory = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *completion = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *escaped_completion = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    if (unescaped_token == 0 || directory_part == 0 || name_part == 0 || directory == 0 ||
        completion == 0 || escaped_completion == 0) {
        winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
        return;
    }
    if (!winxterm_dstcmd_completion_copy_unescaped_spaces(token,
                                                          unescaped_token,
                                                          WINXTERM_DSTCMD_PATH_CAPACITY) ||
        !winxterm_dstcmd_completion_split_path(unescaped_token,
                                               directory_part,
                                               WINXTERM_DSTCMD_PATH_CAPACITY,
                                               name_part,
                                               WINXTERM_DSTCMD_PATH_CAPACITY)) {
        goto cleanup;
    }
    if (!winxterm_dstcmd_path_to_display(directory_part, directory_part, WINXTERM_DSTCMD_PATH_CAPACITY)) {
        goto cleanup;
    }
    if (!winxterm_dstcmd_path_resolve_scratch(&shell->scratch,
                                             shell->cwd,
                                             directory_part[0] != L'\0' ? directory_part : L".",
                                             directory,
                                             WINXTERM_DSTCMD_PATH_CAPACITY)) {
        goto cleanup;
    }
    WinxtermDstcmdDirIter iter;
    if (!winxterm_dstcmd_dir_iter_open_scratch(&shell->scratch, directory, &iter)) {
        goto cleanup;
    }
    const WIN32_FIND_DATAW *data = 0;
    while (*count < capacity && winxterm_dstcmd_dir_iter_next(&iter, &data)) {
        if (wcscmp(data->cFileName, L".") == 0 || wcscmp(data->cFileName, L"..") == 0 ||
            !winxterm_dstcmd_wide_starts_with_ci(data->cFileName, name_part)) {
            continue;
        }
        const wchar_t *suffix = (data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u ? L"/" : L"";
        if (_snwprintf_s(completion,
                         WINXTERM_DSTCMD_PATH_CAPACITY,
                         _TRUNCATE,
                         L"%ls%ls%ls",
                         directory_part,
                         data->cFileName,
                         suffix) >= 0) {
            if (winxterm_dstcmd_completion_copy_escaped_spaces(completion,
                                                               escaped_completion,
                                                               WINXTERM_DSTCMD_PATH_CAPACITY)) {
                (void)winxterm_dstcmd_completion_add(matches, count, capacity, escaped_completion);
            }
        }
    }
    winxterm_dstcmd_dir_iter_close(&iter);
cleanup:
    winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
}

static size_t winxterm_dstcmd_completion_common_length(wchar_t **matches, size_t count)
{
    if (matches == 0 || count == 0u) {
        return 0u;
    }
    size_t common = wcslen(matches[0]);
    for (size_t i = 1u; i < count; ++i) {
        size_t first_offset = 0u;
        size_t other_offset = 0u;
        size_t other_length = wcslen(matches[i]);
        while (first_offset < common && other_offset < other_length) {
            size_t next_first = first_offset;
            size_t next_other = other_offset;
            uint32_t first_codepoint = 0u;
            uint32_t other_codepoint = 0u;
            (void)winxterm_dstcmd_wide_decode_next(matches[0], common, &next_first, &first_codepoint);
            (void)winxterm_dstcmd_wide_decode_next(matches[i], other_length, &next_other, &other_codepoint);
            if (next_first <= first_offset || next_other <= other_offset ||
                first_codepoint != other_codepoint) {
                break;
            }
            first_offset = next_first;
            other_offset = next_other;
        }
        common = first_offset;
    }
    return common;
}

static bool winxterm_dstcmd_completion_add_active(wchar_t **active,
                                                  size_t *active_count,
                                                  size_t active_capacity,
                                                  wchar_t **matches,
                                                  size_t count)
{
    if (active == 0 || active_count == 0 || matches == 0) {
        return false;
    }
    for (size_t i = 0u; i < count; ++i) {
        if (*active_count >= active_capacity) {
            return false;
        }
        active[(*active_count)++] = matches[i];
    }
    return true;
}

static bool winxterm_dstcmd_completion_add_case_matching_active(wchar_t **active,
                                                               size_t *active_count,
                                                               size_t active_capacity,
                                                               wchar_t **matches,
                                                               size_t count,
                                                               const wchar_t *token)
{
    if (active == 0 || active_count == 0 || matches == 0 || token == 0) {
        return false;
    }
    for (size_t i = 0u; i < count; ++i) {
        if (!winxterm_dstcmd_wide_starts_with_cs(matches[i], token)) {
            continue;
        }
        if (*active_count >= active_capacity) {
            return false;
        }
        active[(*active_count)++] = matches[i];
    }
    return true;
}

static void winxterm_dstcmd_shell_show_completion_match(WinxtermDstcmdShell *shell,
                                                        wchar_t *match,
                                                        size_t highlight_index,
                                                        const wchar_t *color)
{
    if (shell == 0 || match == 0) {
        return;
    }
    if (color != 0) {
        (void)winxterm_dstcmd_shell_write_wide(shell, color);
    }
    size_t length = wcslen(match);
    size_t highlight_start = winxterm_dstcmd_wide_safe_truncate(match, length, highlight_index);
    if (highlight_start >= length) {
        (void)winxterm_dstcmd_shell_write_wide(shell, match);
        if (color != 0) {
            (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_PROMPT_COLOR_RESET);
        }
        return;
    }

    if (highlight_start != 0u) {
        wchar_t saved = match[highlight_start];
        match[highlight_start] = L'\0';
        (void)winxterm_dstcmd_shell_write_wide(shell, match);
        match[highlight_start] = saved;
    }

    size_t highlight_end = winxterm_dstcmd_wide_next_boundary(match, length, highlight_start);
    wchar_t after_highlight = match[highlight_end];
    match[highlight_end] = L'\0';
    (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_COMPLETION_HIGHLIGHT_ON);
    (void)winxterm_dstcmd_shell_write_wide(shell, match + highlight_start);
    (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_COMPLETION_HIGHLIGHT_OFF);
    match[highlight_end] = after_highlight;

    (void)winxterm_dstcmd_shell_write_wide(shell, match + highlight_end);
    if (color != 0) {
        (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_PROMPT_COLOR_RESET);
    }
}

static void winxterm_dstcmd_shell_show_completion_group(WinxtermDstcmdShell *shell,
                                                        wchar_t **matches,
                                                        size_t count,
                                                        size_t highlight_index,
                                                        const wchar_t *color)
{
    for (size_t i = 0u; i < count; ++i) {
        winxterm_dstcmd_shell_show_completion_match(shell, matches[i], highlight_index, color);
        (void)winxterm_dstcmd_shell_write_utf8(shell, "  ");
    }
}

static void winxterm_dstcmd_shell_show_completions(WinxtermDstcmdShell *shell,
                                                   wchar_t **builtin_matches,
                                                   size_t builtin_count,
                                                   wchar_t **path_matches,
                                                   size_t path_count,
                                                   wchar_t **local_matches,
                                                   size_t local_count,
                                                   bool show_external,
                                                   size_t highlight_index)
{
    (void)winxterm_dstcmd_shell_move_to_line_offset(shell, shell->line_length);
    if (shell != 0) {
        shell->prompt_cursor_saved = false;
    }
    (void)winxterm_dstcmd_shell_write_utf8(shell, "\r\n");
    if (show_external) {
        winxterm_dstcmd_shell_show_completion_group(shell,
                                                    builtin_matches,
                                                    builtin_count,
                                                    highlight_index,
                                                    WINXTERM_DSTCMD_COMPLETION_COLOR_BUILTIN);
        winxterm_dstcmd_shell_show_completion_group(shell,
                                                    path_matches,
                                                    path_count,
                                                    highlight_index,
                                                    WINXTERM_DSTCMD_COMPLETION_COLOR_PATH);
        if (local_count != 0u && (builtin_count != 0u || path_count != 0u)) {
            (void)winxterm_dstcmd_shell_write_utf8(shell, "\r\n\r\n");
        }
    }
    winxterm_dstcmd_shell_show_completion_group(shell, local_matches, local_count, highlight_index, 0);
    (void)winxterm_dstcmd_shell_write_utf8(shell, "\r\n");
    (void)winxterm_dstcmd_shell_refresh_line(shell);
}

static bool winxterm_dstcmd_shell_complete_unlocked(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    size_t token_start = shell->line_cursor;
    while (token_start > 0u &&
           !winxterm_dstcmd_shell_is_unescaped_space_byte(shell->line, token_start - 1u)) {
        --token_start;
    }
    bool command_position = true;
    for (size_t i = 0u; i < token_start; ++i) {
        if (shell->line[i] != ' ' && shell->line[i] != '\t') {
            command_position = false;
            break;
        }
    }
    wchar_t *token = winxterm_dstcmd_utf8_token_to_wide(shell->line + token_start,
                                                        shell->line_cursor - token_start);
    if (token == 0) {
        return winxterm_dstcmd_shell_write_utf8(shell, "\a");
    }
    WinxtermDstcmdScratchMark completion_mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    wchar_t *exact_directory = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    if (exact_directory == 0) {
        free(token);
        return winxterm_dstcmd_shell_write_utf8(shell, "\a");
    }
    wchar_t *local_matches[512];
    wchar_t *builtin_matches[512];
    wchar_t *path_matches[512];
    wchar_t *active_matches[512];
    wchar_t *case_matching_active[512];
    memset(local_matches, 0, sizeof(local_matches));
    memset(builtin_matches, 0, sizeof(builtin_matches));
    memset(path_matches, 0, sizeof(path_matches));
    memset(active_matches, 0, sizeof(active_matches));
    memset(case_matching_active, 0, sizeof(case_matching_active));
    size_t local_count = 0u;
    size_t builtin_count = 0u;
    size_t path_count = 0u;
    size_t active_count = 0u;
    size_t case_matching_active_count = 0u;
    bool command_external_allowed = command_position && !winxterm_dstcmd_completion_has_slash(token);
    bool repeated_completion = winxterm_dstcmd_shell_completion_is_repeat(shell);
    bool exact_directory_match = winxterm_dstcmd_completion_exact_directory(shell,
                                                                           token,
                                                                           exact_directory,
                                                                           WINXTERM_DSTCMD_PATH_CAPACITY);

    winxterm_dstcmd_completion_add_files(shell, local_matches, &local_count, 512u, token);
    bool show_external = command_external_allowed && (local_count == 0u || repeated_completion);
    if (show_external) {
        winxterm_dstcmd_completion_add_builtins(shell, builtin_matches, &builtin_count, 512u, token);
        winxterm_dstcmd_completion_add_path_commands(path_matches, &path_count, 512u, token, &shell->scratch);
    }

    if (show_external) {
        (void)winxterm_dstcmd_completion_add_active(active_matches,
                                                    &active_count,
                                                    512u,
                                                    builtin_matches,
                                                    builtin_count);
        (void)winxterm_dstcmd_completion_add_active(active_matches,
                                                    &active_count,
                                                    512u,
                                                    path_matches,
                                                    path_count);
    }
    (void)winxterm_dstcmd_completion_add_active(active_matches,
                                                &active_count,
                                                512u,
                                                local_matches,
                                                local_count);

    if (exact_directory_match && (repeated_completion || active_count == 0u)) {
        char *utf8 = winxterm_dstcmd_wide_to_utf8_alloc(exact_directory);
        bool ok = utf8 != 0 && winxterm_dstcmd_shell_replace_range(shell, token_start, shell->line_cursor, utf8);
        free(utf8);
        winxterm_dstcmd_shell_clear_completion_repeat(shell);
        winxterm_dstcmd_completion_free(local_matches, local_count);
        winxterm_dstcmd_completion_free(builtin_matches, builtin_count);
        winxterm_dstcmd_completion_free(path_matches, path_count);
        winxterm_dstcmd_scratch_rewind(&shell->scratch, completion_mark);
        free(token);
        return ok;
    }
    if (active_count == 0u) {
        winxterm_dstcmd_shell_note_completion_no_change(shell);
        winxterm_dstcmd_completion_free(local_matches, local_count);
        winxterm_dstcmd_completion_free(builtin_matches, builtin_count);
        winxterm_dstcmd_completion_free(path_matches, path_count);
        winxterm_dstcmd_scratch_rewind(&shell->scratch, completion_mark);
        free(token);
        return winxterm_dstcmd_shell_write_utf8(shell, "\a");
    }
    (void)winxterm_dstcmd_completion_add_case_matching_active(case_matching_active,
                                                             &case_matching_active_count,
                                                             512u,
                                                             active_matches,
                                                             active_count,
                                                             token);
    wchar_t **decision_matches = case_matching_active_count != 0u ? case_matching_active : active_matches;
    size_t decision_count = case_matching_active_count != 0u ? case_matching_active_count : active_count;
    bool delay_exact_directory_slash = exact_directory_match &&
                                       active_count > 1u &&
                                       decision_count == 1u &&
                                       wcscmp(decision_matches[0], exact_directory) == 0;
    if (delay_exact_directory_slash) {
        decision_matches = active_matches;
        decision_count = active_count;
    }
    if (decision_count == 1u) {
        char *utf8 = winxterm_dstcmd_wide_to_utf8_alloc(decision_matches[0]);
        bool ok = utf8 != 0 && winxterm_dstcmd_shell_replace_range(shell, token_start, shell->line_cursor, utf8);
        free(utf8);
        winxterm_dstcmd_shell_clear_completion_repeat(shell);
        winxterm_dstcmd_completion_free(local_matches, local_count);
        winxterm_dstcmd_completion_free(builtin_matches, builtin_count);
        winxterm_dstcmd_completion_free(path_matches, path_count);
        winxterm_dstcmd_scratch_rewind(&shell->scratch, completion_mark);
        free(token);
        return ok;
    }
    size_t common_length = winxterm_dstcmd_completion_common_length(decision_matches, decision_count);
    if (common_length > wcslen(token)) {
        wchar_t saved = decision_matches[0][common_length];
        decision_matches[0][common_length] = L'\0';
        char *utf8 = winxterm_dstcmd_wide_to_utf8_alloc(decision_matches[0]);
        decision_matches[0][common_length] = saved;
        bool ok = utf8 != 0 && winxterm_dstcmd_shell_replace_range(shell, token_start, shell->line_cursor, utf8);
        free(utf8);
        winxterm_dstcmd_shell_clear_completion_repeat(shell);
        winxterm_dstcmd_completion_free(local_matches, local_count);
        winxterm_dstcmd_completion_free(builtin_matches, builtin_count);
        winxterm_dstcmd_completion_free(path_matches, path_count);
        winxterm_dstcmd_scratch_rewind(&shell->scratch, completion_mark);
        free(token);
        return ok;
    }
    winxterm_dstcmd_shell_show_completions(shell,
                                           builtin_matches,
                                           builtin_count,
                                           path_matches,
                                           path_count,
                                           local_matches,
                                           local_count,
                                           show_external,
                                           common_length);
    winxterm_dstcmd_shell_note_completion_no_change(shell);
    winxterm_dstcmd_completion_free(local_matches, local_count);
    winxterm_dstcmd_completion_free(builtin_matches, builtin_count);
    winxterm_dstcmd_completion_free(path_matches, path_count);
    winxterm_dstcmd_scratch_rewind(&shell->scratch, completion_mark);
    free(token);
    return true;
}

static bool winxterm_dstcmd_shell_complete(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    if (shell->output_lock_initialized) {
        EnterCriticalSection(&shell->output_lock);
    }
    bool ok = winxterm_dstcmd_shell_complete_unlocked(shell);
    if (shell->output_lock_initialized) {
        LeaveCriticalSection(&shell->output_lock);
    }
    return ok;
}

static int winxterm_dstcmd_escape_param_or_default(const char *sequence,
                                                   size_t length,
                                                   size_t param_index,
                                                   int default_value)
{
    if (sequence == 0 || length < 3u || sequence[0] != 0x1b || sequence[1] != '[') {
        return default_value;
    }
    size_t current_index = 0u;
    size_t i = 2u;
    while (i + 1u < length) {
        bool has_value = false;
        int value = 0;
        while (i + 1u < length && sequence[i] >= '0' && sequence[i] <= '9') {
            has_value = true;
            value = value * 10 + (sequence[i] - '0');
            ++i;
        }
        if (current_index == param_index) {
            return has_value ? value : default_value;
        }
        if (i + 1u >= length || sequence[i] != ';') {
            break;
        }
        ++i;
        ++current_index;
    }
    return default_value;
}

static bool winxterm_dstcmd_escape_modifier_has_ctrl(int modifier)
{
    return modifier > 1 && ((modifier - 1) & 4) != 0;
}

static void winxterm_dstcmd_shell_handle_escape_sequence(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || shell->escape_sequence_length < 2u) {
        return;
    }
    const char *sequence = shell->escape_sequence;
    size_t length = shell->escape_sequence_length;
    char final = sequence[length - 1u];
    if (sequence[1] == 'O') {
        switch (final) {
        case 'A':
            winxterm_dstcmd_shell_history_previous(shell);
            break;
        case 'B':
            winxterm_dstcmd_shell_history_next(shell);
            break;
        case 'C':
            winxterm_dstcmd_shell_move_right(shell);
            break;
        case 'D':
            winxterm_dstcmd_shell_move_left(shell);
            break;
        case 'H':
            winxterm_dstcmd_shell_move_home(shell);
            break;
        case 'F':
            winxterm_dstcmd_shell_move_end(shell);
            break;
        default:
            break;
        }
        return;
    }
    if (sequence[1] != '[') {
        return;
    }
    switch (final) {
    case 'A':
        winxterm_dstcmd_shell_history_previous(shell);
        break;
    case 'B':
        winxterm_dstcmd_shell_history_next(shell);
        break;
    case 'C': {
        int modifier = winxterm_dstcmd_escape_param_or_default(sequence, length, 1u, 1);
        if (winxterm_dstcmd_escape_modifier_has_ctrl(modifier)) {
            winxterm_dstcmd_shell_move_word_right(shell);
        } else {
            winxterm_dstcmd_shell_move_right(shell);
        }
        break;
    }
    case 'D': {
        int modifier = winxterm_dstcmd_escape_param_or_default(sequence, length, 1u, 1);
        if (winxterm_dstcmd_escape_modifier_has_ctrl(modifier)) {
            winxterm_dstcmd_shell_move_word_left(shell);
        } else {
            winxterm_dstcmd_shell_move_left(shell);
        }
        break;
    }
    case 'H':
        winxterm_dstcmd_shell_move_home(shell);
        break;
    case 'F':
        winxterm_dstcmd_shell_move_end(shell);
        break;
    case '~': {
        int key = winxterm_dstcmd_escape_param_or_default(sequence, length, 0u, 0);
        int modifier = winxterm_dstcmd_escape_param_or_default(sequence, length, 1u, 1);
        if (key == 1 || key == 7) {
            winxterm_dstcmd_shell_move_home(shell);
        } else if (key == 4 || key == 8) {
            winxterm_dstcmd_shell_move_end(shell);
        } else if (key == 3 && winxterm_dstcmd_escape_modifier_has_ctrl(modifier)) {
            winxterm_dstcmd_shell_delete_word_forward(shell);
        } else if (key == 3) {
            winxterm_dstcmd_shell_delete_at_cursor(shell);
        }
        break;
    }
    default:
        break;
    }
}

static bool winxterm_dstcmd_escape_sequence_complete(const char *sequence, size_t length)
{
    if (sequence == 0 || length < 2u) {
        return false;
    }
    if (sequence[1] == '[') {
        return length >= 3u && sequence[length - 1u] >= 0x40 && sequence[length - 1u] <= 0x7e;
    }
    if (sequence[1] == 'O') {
        return length >= 3u && sequence[length - 1u] >= 0x40 && sequence[length - 1u] <= 0x7e;
    }
    return true;
}

static bool winxterm_dstcmd_history_search_query_changed(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    shell->history_search_delete_confirm_active = false;
    shell->history_search_pending_delete[0] = '\0';
    shell->history_search_status[0] = '\0';
    shell->history_search_selected = 0u;
    shell->history_search_scroll = 0u;
    winxterm_dstcmd_history_search_rebuild(shell);
    return winxterm_dstcmd_history_search_redraw(shell);
}

static bool winxterm_dstcmd_history_search_insert_byte(WinxtermDstcmdShell *shell, uint8_t byte)
{
    if (shell == 0) {
        return false;
    }
    if (shell->history_search_query_length + 1u >= WINXTERM_DSTCMD_LINE_CAPACITY) {
        return winxterm_dstcmd_shell_write_utf8(shell, "\a");
    }
    memmove(shell->history_search_query + shell->history_search_query_cursor + 1u,
            shell->history_search_query + shell->history_search_query_cursor,
            shell->history_search_query_length - shell->history_search_query_cursor + 1u);
    shell->history_search_query[shell->history_search_query_cursor++] = (char)byte;
    ++shell->history_search_query_length;
    shell->history_search_query[shell->history_search_query_length] = '\0';
    return winxterm_dstcmd_history_search_query_changed(shell);
}

static bool winxterm_dstcmd_history_search_backspace(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || shell->history_search_query_cursor == 0u) {
        return true;
    }
    size_t delete_start = winxterm_dstcmd_utf8_prev_boundary(shell->history_search_query,
                                                            shell->history_search_query_length,
                                                            shell->history_search_query_cursor);
    memmove(shell->history_search_query + delete_start,
            shell->history_search_query + shell->history_search_query_cursor,
            shell->history_search_query_length - shell->history_search_query_cursor + 1u);
    shell->history_search_query_length -= shell->history_search_query_cursor - delete_start;
    shell->history_search_query_cursor = delete_start;
    return winxterm_dstcmd_history_search_query_changed(shell);
}

static bool winxterm_dstcmd_history_search_delete_at_cursor(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || shell->history_search_query_cursor >= shell->history_search_query_length) {
        return true;
    }
    size_t delete_end = winxterm_dstcmd_utf8_next_boundary(shell->history_search_query,
                                                          shell->history_search_query_length,
                                                          shell->history_search_query_cursor);
    memmove(shell->history_search_query + shell->history_search_query_cursor,
            shell->history_search_query + delete_end,
            shell->history_search_query_length - delete_end + 1u);
    shell->history_search_query_length -= delete_end - shell->history_search_query_cursor;
    return winxterm_dstcmd_history_search_query_changed(shell);
}

static bool winxterm_dstcmd_history_search_delete_word_backward(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || shell->history_search_query_cursor == 0u) {
        return true;
    }
    size_t delete_start = shell->history_search_query_cursor;
    while (delete_start > 0u &&
           winxterm_dstcmd_shell_is_space_byte(shell->history_search_query[delete_start - 1u])) {
        --delete_start;
    }
    while (delete_start > 0u &&
           !winxterm_dstcmd_shell_is_space_byte(shell->history_search_query[delete_start - 1u])) {
        --delete_start;
    }
    memmove(shell->history_search_query + delete_start,
            shell->history_search_query + shell->history_search_query_cursor,
            shell->history_search_query_length - shell->history_search_query_cursor + 1u);
    shell->history_search_query_length -= shell->history_search_query_cursor - delete_start;
    shell->history_search_query_cursor = delete_start;
    return winxterm_dstcmd_history_search_query_changed(shell);
}

static bool winxterm_dstcmd_history_search_clear_query(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    shell->history_search_query[0] = '\0';
    shell->history_search_query_length = 0u;
    shell->history_search_query_cursor = 0u;
    return winxterm_dstcmd_history_search_query_changed(shell);
}

static bool winxterm_dstcmd_history_search_move_query_left(WinxtermDstcmdShell *shell)
{
    if (shell != 0 && shell->history_search_query_cursor != 0u) {
        shell->history_search_query_cursor =
            winxterm_dstcmd_utf8_prev_boundary(shell->history_search_query,
                                               shell->history_search_query_length,
                                               shell->history_search_query_cursor);
    }
    return winxterm_dstcmd_history_search_redraw(shell);
}

static bool winxterm_dstcmd_history_search_move_query_right(WinxtermDstcmdShell *shell)
{
    if (shell != 0 && shell->history_search_query_cursor < shell->history_search_query_length) {
        shell->history_search_query_cursor =
            winxterm_dstcmd_utf8_next_boundary(shell->history_search_query,
                                               shell->history_search_query_length,
                                               shell->history_search_query_cursor);
    }
    return winxterm_dstcmd_history_search_redraw(shell);
}

static bool winxterm_dstcmd_history_search_select_relative(WinxtermDstcmdShell *shell, int delta)
{
    if (shell == 0 || shell->history_search_result_count == 0u) {
        return true;
    }
    if (delta < 0) {
        size_t amount = (size_t)(-delta);
        shell->history_search_selected =
            amount > shell->history_search_selected ? 0u : shell->history_search_selected - amount;
    } else {
        shell->history_search_selected += (size_t)delta;
        if (shell->history_search_selected >= shell->history_search_result_count) {
            shell->history_search_selected = shell->history_search_result_count - 1u;
        }
    }
    shell->history_search_status[0] = '\0';
    return winxterm_dstcmd_history_search_redraw(shell);
}

static bool winxterm_dstcmd_history_search_select_first_or_last(WinxtermDstcmdShell *shell, bool last)
{
    if (shell == 0 || shell->history_search_result_count == 0u) {
        return true;
    }
    shell->history_search_selected = last ? shell->history_search_result_count - 1u : 0u;
    shell->history_search_status[0] = '\0';
    return winxterm_dstcmd_history_search_redraw(shell);
}

static bool winxterm_dstcmd_history_search_toggle_ranking(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    shell->history_search_ranking =
        shell->history_search_ranking == WINXTERM_DSTCMD_HISTORY_SEARCH_RANK_BEST ?
        WINXTERM_DSTCMD_HISTORY_SEARCH_RANK_RECENT : WINXTERM_DSTCMD_HISTORY_SEARCH_RANK_BEST;
    strncpy_s(shell->history_search_status,
              sizeof(shell->history_search_status),
              shell->history_search_ranking == WINXTERM_DSTCMD_HISTORY_SEARCH_RANK_BEST ?
              "best ranking" : "recent order",
              _TRUNCATE);
    winxterm_dstcmd_history_search_rebuild(shell);
    return winxterm_dstcmd_history_search_redraw(shell);
}

static bool winxterm_dstcmd_history_search_toggle_matching(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    shell->history_search_matching =
        shell->history_search_matching == WINXTERM_DSTCMD_HISTORY_SEARCH_MATCH_FUZZY ?
        WINXTERM_DSTCMD_HISTORY_SEARCH_MATCH_CONTAINS : WINXTERM_DSTCMD_HISTORY_SEARCH_MATCH_FUZZY;
    strncpy_s(shell->history_search_status,
              sizeof(shell->history_search_status),
              shell->history_search_matching == WINXTERM_DSTCMD_HISTORY_SEARCH_MATCH_FUZZY ?
              "fuzzy mode" : "contains mode",
              _TRUNCATE);
    winxterm_dstcmd_history_search_rebuild(shell);
    return winxterm_dstcmd_history_search_redraw(shell);
}

static bool winxterm_dstcmd_history_search_handle_escape_sequence(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || shell->escape_sequence_length < 2u) {
        return true;
    }
    const char *sequence = shell->escape_sequence;
    size_t length = shell->escape_sequence_length;
    char final = sequence[length - 1u];
    if (sequence[1] != '[' && sequence[1] != 'O') {
        if (final == 'r' || final == 'R') {
            return winxterm_dstcmd_history_search_toggle_matching(shell);
        }
        return winxterm_dstcmd_history_search_cancel(shell, false);
    }
    switch (final) {
    case 'A':
        return winxterm_dstcmd_history_search_select_relative(shell, -1);
    case 'B':
        return winxterm_dstcmd_history_search_select_relative(shell, 1);
    case 'C':
        return winxterm_dstcmd_history_search_move_query_right(shell);
    case 'D':
        return winxterm_dstcmd_history_search_move_query_left(shell);
    case 'H':
        shell->history_search_query_cursor = 0u;
        return winxterm_dstcmd_history_search_redraw(shell);
    case 'F':
        shell->history_search_query_cursor = shell->history_search_query_length;
        return winxterm_dstcmd_history_search_redraw(shell);
    case '~': {
        int key = winxterm_dstcmd_escape_param_or_default(sequence, length, 0u, 0);
        int modifier = winxterm_dstcmd_escape_param_or_default(sequence, length, 1u, 1);
        if (key == 1 || key == 7) {
            shell->history_search_query_cursor = 0u;
            return winxterm_dstcmd_history_search_redraw(shell);
        }
        if (key == 4 || key == 8) {
            shell->history_search_query_cursor = shell->history_search_query_length;
            return winxterm_dstcmd_history_search_redraw(shell);
        }
        if (key == 3 && modifier == 2) {
            return winxterm_dstcmd_history_search_begin_delete(shell);
        }
        if (key == 3) {
            return winxterm_dstcmd_history_search_delete_at_cursor(shell);
        }
        if (key == 5) {
            return winxterm_dstcmd_history_search_select_relative(shell, -5);
        }
        if (key == 6) {
            return winxterm_dstcmd_history_search_select_relative(shell, 5);
        }
        break;
    }
    default:
        break;
    }
    return true;
}

static bool winxterm_dstcmd_history_search_handle_input_byte(WinxtermDstcmdShell *shell,
                                                             uint8_t byte)
{
    if (shell == 0) {
        return false;
    }
    if (shell->escape_sequence_length != 0u) {
        if (shell->escape_sequence_length + 1u < WINXTERM_DSTCMD_ESCAPE_CAPACITY) {
            shell->escape_sequence[shell->escape_sequence_length++] = (char)byte;
            shell->escape_sequence[shell->escape_sequence_length] = '\0';
        } else {
            shell->escape_sequence_length = 0u;
            shell->escape_sequence[0] = '\0';
            return true;
        }
        if (winxterm_dstcmd_escape_sequence_complete(shell->escape_sequence,
                                                     shell->escape_sequence_length)) {
            bool ok = winxterm_dstcmd_history_search_handle_escape_sequence(shell);
            shell->escape_sequence_length = 0u;
            shell->escape_sequence[0] = '\0';
            return ok;
        }
        return true;
    }
    if (shell->history_search_delete_confirm_active) {
        if (byte == 'y' || byte == 'Y') {
            return winxterm_dstcmd_history_search_confirm_delete(shell);
        }
        if (byte == 'n' || byte == 'N' || byte == 0x07u) {
            return winxterm_dstcmd_history_search_cancel_delete(shell);
        }
        if (byte == 0x03u) {
            return winxterm_dstcmd_history_search_cancel(shell, true);
        }
        shell->history_search_delete_confirm_active = false;
        shell->history_search_pending_delete[0] = '\0';
        shell->history_search_status[0] = '\0';
    }
    if (byte == 0x1bu) {
        shell->escape_sequence[0] = (char)byte;
        shell->escape_sequence[1] = '\0';
        shell->escape_sequence_length = 1u;
        return true;
    }
    if (byte == '\r' || byte == '\n') {
        return winxterm_dstcmd_history_search_accept(shell);
    }
    if (byte == 0x03u) {
        return winxterm_dstcmd_history_search_cancel(shell, true);
    }
    if (byte == 0x07u) {
        return winxterm_dstcmd_history_search_cancel(shell, false);
    }
    if (byte == 0x12u) {
        return winxterm_dstcmd_history_search_toggle_ranking(shell);
    }
    if (byte == 0x1au) {
        return winxterm_dstcmd_history_search_undo_delete(shell);
    }
    if (byte == 0x0cu) {
        (void)winxterm_dstcmd_shell_write_utf8(shell, "\x1b[0m\x1b[H\x1b[2J");
        shell->prompt_cursor_saved = false;
        if (!winxterm_dstcmd_shell_refresh_line(shell)) {
            return false;
        }
        shell->history_search_overlay_columns = 0;
        shell->history_search_overlay_rows = 0;
        return winxterm_dstcmd_history_search_redraw(shell);
    }
    if (byte == 0x01u) {
        shell->history_search_query_cursor = 0u;
        return winxterm_dstcmd_history_search_redraw(shell);
    }
    if (byte == 0x05u) {
        shell->history_search_query_cursor = shell->history_search_query_length;
        return winxterm_dstcmd_history_search_redraw(shell);
    }
    if (byte == 0x10u) {
        return winxterm_dstcmd_history_search_select_relative(shell, -1);
    }
    if (byte == 0x0eu) {
        return winxterm_dstcmd_history_search_select_relative(shell, 1);
    }
    if (byte == 0x15u) {
        return winxterm_dstcmd_history_search_clear_query(shell);
    }
    if (byte == 0x17u) {
        return winxterm_dstcmd_history_search_delete_word_backward(shell);
    }
    if (byte == 0x02u) {
        return winxterm_dstcmd_history_search_move_query_left(shell);
    }
    if (byte == 0x06u) {
        return winxterm_dstcmd_history_search_move_query_right(shell);
    }
    if (byte == '\b' || byte == 0x7fu) {
        return winxterm_dstcmd_history_search_backspace(shell);
    }
    if (byte == '\t') {
        return winxterm_dstcmd_history_search_select_relative(shell, 1);
    }
    if (byte >= 0x20u) {
        return winxterm_dstcmd_history_search_insert_byte(shell, byte);
    }
    return true;
}

static char *winxterm_dstcmd_default_rc_line_start(char *line, bool first_line)
{
    if (line == 0) {
        return 0;
    }
    unsigned char *bytes = (unsigned char *)line;
    if (first_line && bytes[0] == 0xefu && bytes[1] == 0xbbu && bytes[2] == 0xbfu) {
        line += 3;
    }
    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    return line;
}

static void winxterm_dstcmd_shell_run_default_rc(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    wchar_t *rc_path = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    if (rc_path == 0 ||
        !winxterm_dstcmd_home_file_path(&shell->scratch, L"default.rc", rc_path, WINXTERM_DSTCMD_PATH_CAPACITY)) {
        winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
        return;
    }
    FILE *file = 0;
    if (_wfopen_s(&file, rc_path, L"rb") != 0 || file == 0) {
        winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
        return;
    }
    winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);

    char buffer[WINXTERM_DSTCMD_LINE_CAPACITY];
    bool first_line = true;
    unsigned int line_number = 0u;
    while (fgets(buffer, sizeof(buffer), file) != 0 && !shell->exit_requested) {
        ++line_number;
        size_t length = strlen(buffer);
        while (length != 0u && (buffer[length - 1u] == '\n' || buffer[length - 1u] == '\r')) {
            buffer[--length] = '\0';
        }
        char *line = winxterm_dstcmd_default_rc_line_start(buffer, first_line);
        first_line = false;
        if (line == 0 || line[0] == '\0' || line[0] == '#') {
            continue;
        }
        wchar_t *wide_line = 0;
        if (!winxterm_dstcmd_utf8_to_wide(line, &wide_line)) {
            (void)winxterm_dstcmd_shell_write_widef(shell,
                                                    L"default.rc:%u: invalid UTF-8 input\r\n",
                                                    line_number);
            continue;
        }
        (void)winxterm_dstcmd_shell_submit_line(shell, wide_line);
        free(wide_line);
    }
    fclose(file);
}

static bool winxterm_dstcmd_shell_handle_input_byte(WinxtermDstcmdShell *shell, uint8_t byte)
{
    if (byte != '\t') {
        winxterm_dstcmd_shell_clear_completion_repeat(shell);
    }
    if (shell != 0 && shell->history_search_active) {
        return winxterm_dstcmd_history_search_handle_input_byte(shell, byte);
    }
    if (shell->escape_sequence_length != 0u) {
        if (shell->escape_sequence_length + 1u < WINXTERM_DSTCMD_ESCAPE_CAPACITY) {
            shell->escape_sequence[shell->escape_sequence_length++] = (char)byte;
            shell->escape_sequence[shell->escape_sequence_length] = '\0';
        } else {
            shell->escape_sequence_length = 0u;
            shell->escape_sequence[0] = '\0';
            return true;
        }
        if (winxterm_dstcmd_escape_sequence_complete(shell->escape_sequence, shell->escape_sequence_length)) {
            winxterm_dstcmd_shell_handle_escape_sequence(shell);
            shell->escape_sequence_length = 0u;
            shell->escape_sequence[0] = '\0';
        }
        return true;
    }
    if (byte == 0x1bu) {
        shell->escape_sequence[0] = (char)byte;
        shell->escape_sequence[1] = '\0';
        shell->escape_sequence_length = 1u;
        return true;
    }
    if (byte == 0x12u) {
        return winxterm_dstcmd_history_search_enter(shell);
    }
    if (byte == '\r' || byte == '\n') {
        if (winxterm_dstcmd_shell_line_has_open_quote(shell)) {
            return winxterm_dstcmd_shell_append_input_byte(shell, (uint8_t)'\n');
        }
        uint64_t command_entered_ns = winxterm_dstcmd_shell_timestamp_ns();
        return winxterm_dstcmd_shell_submit_current_line(shell, command_entered_ns);
    }
    if (byte == 0x03u) {
        (void)winxterm_dstcmd_shell_clear_rendered_prompt(shell);
        winxterm_dstcmd_shell_clear_line(shell);
        (void)winxterm_dstcmd_shell_write_utf8(shell, "^C\r\n");
        return winxterm_dstcmd_shell_prompt(shell);
    }
    if (byte == 0x01u) {
        winxterm_dstcmd_shell_move_home(shell);
        return true;
    }
    if (byte == 0x02u) {
        winxterm_dstcmd_shell_move_left(shell);
        return true;
    }
    if (byte == 0x04u) {
        if (shell->line_length == 0u) {
            shell->exit_requested = true;
        } else {
            winxterm_dstcmd_shell_delete_at_cursor(shell);
        }
        return true;
    }
    if (byte == 0x05u) {
        winxterm_dstcmd_shell_move_end(shell);
        return true;
    }
    if (byte == 0x06u) {
        winxterm_dstcmd_shell_move_right(shell);
        return true;
    }
    if (byte == 0x0bu) {
        winxterm_dstcmd_shell_kill_to_end(shell);
        return true;
    }
    if (byte == 0x0cu) {
        (void)winxterm_dstcmd_shell_write_utf8(shell, "\x1b[H\x1b[2J");
        shell->prompt_cursor_saved = false;
        return winxterm_dstcmd_shell_refresh_line(shell);
    }
    if (byte == 0x15u) {
        winxterm_dstcmd_shell_kill_line(shell);
        return true;
    }
    if (byte == 0x17u) {
        winxterm_dstcmd_shell_delete_word_backward(shell);
        return true;
    }
    if (byte == '\b' || byte == 0x7fu) {
        winxterm_dstcmd_shell_backspace(shell);
        return true;
    }
    if (byte == '\t') {
        if (!winxterm_dstcmd_shell_complete(shell)) {
            (void)winxterm_dstcmd_shell_write_utf8(shell, "\a");
        }
        return true;
    }
    if (byte >= 0x20u) {
        return winxterm_dstcmd_shell_append_input_byte(shell, byte);
    }
    return true;
}

static DWORD winxterm_dstcmd_run_shell(WinxtermDstcmdShell *shell, const wchar_t *notice)
{
    if (notice != 0 && notice[0] != L'\0') {
        (void)winxterm_dstcmd_shell_write_widef(shell, L"%ls\r\n", notice);
    }
    winxterm_dstcmd_shell_run_default_rc(shell);
    if (shell->exit_requested) {
        return (DWORD)shell->last_status;
    }
    if (!winxterm_dstcmd_shell_prompt(shell)) {
        return 1;
    }
    while (!shell->exit_requested) {
        uint8_t input[512];
        size_t input_count = winxterm_dstcmd_shell_read_input(shell, input, sizeof(input), true);
        if (input_count == 0u) {
            break;
        }
        for (size_t i = 0u; i < input_count; ++i) {
            if (!winxterm_dstcmd_shell_handle_input_byte(shell, input[i])) {
                return 1;
            }
            if (shell->exit_requested) {
                break;
            }
        }
    }
    return (DWORD)shell->last_status;
}

DWORD winxterm_dstcmd_run_with_notice(const wchar_t *notice)
{
    WinxtermDstcmdShell *shell = (WinxtermDstcmdShell *)calloc(1u, sizeof(*shell));
    if (shell == 0) {
        return 1;
    }
    if (!winxterm_dstcmd_shell_init(shell)) {
        free(shell);
        return 1;
    }
    DWORD exit_code = winxterm_dstcmd_run_shell(shell, notice);
    winxterm_dstcmd_shell_dispose(shell);
    free(shell);
    return exit_code;
}

DWORD winxterm_dstcmd_run(void)
{
    return winxterm_dstcmd_run_with_notice(0);
}

static bool winxterm_dstcmd_smoke_set_line(WinxtermDstcmdShell *shell, const char *line)
{
    if (shell == 0 || line == 0) {
        return false;
    }
    size_t length = strlen(line);
    if (length >= WINXTERM_DSTCMD_LINE_CAPACITY) {
        return false;
    }
    memcpy(shell->line, line, length + 1u);
    shell->line_length = length;
    shell->line_cursor = length;
    shell->prompt_cursor_saved = false;
    winxterm_dstcmd_shell_clear_completion_repeat(shell);
    return true;
}

static bool winxterm_dstcmd_smoke_tab_completes_to(WinxtermDstcmdShell *shell,
                                                   const char *prefix,
                                                   const char *expected)
{
    if (!winxterm_dstcmd_smoke_set_line(shell, prefix)) {
        return false;
    }
    if (!winxterm_dstcmd_shell_handle_input_byte(shell, '\t')) {
        return false;
    }
    return strcmp(shell->line, expected) == 0 &&
           shell->line_cursor == strlen(expected);
}

static bool winxterm_dstcmd_smoke_tab_twice_completes_to(WinxtermDstcmdShell *shell,
                                                         const char *prefix,
                                                         const char *first_expected,
                                                         const char *second_expected)
{
    if (!winxterm_dstcmd_smoke_set_line(shell, prefix)) {
        return false;
    }
    if (!winxterm_dstcmd_shell_handle_input_byte(shell, '\t') ||
        strcmp(shell->line, first_expected) != 0 ||
        shell->line_cursor != strlen(first_expected)) {
        return false;
    }
    if (!winxterm_dstcmd_shell_handle_input_byte(shell, '\t')) {
        return false;
    }
    return strcmp(shell->line, second_expected) == 0 &&
           shell->line_cursor == strlen(second_expected);
}

static bool winxterm_dstcmd_smoke_feed_input(WinxtermDstcmdShell *shell, const char *input)
{
    if (shell == 0 || input == 0) {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)input; *cursor != '\0'; ++cursor) {
        if (!winxterm_dstcmd_shell_handle_input_byte(shell, *cursor)) {
            return false;
        }
    }
    return true;
}

static void winxterm_dstcmd_smoke_clear_capture(WinxtermDstcmdShell *shell);
static bool winxterm_dstcmd_smoke_capture_contains(WinxtermDstcmdShell *shell, const char *needle);

static bool winxterm_dstcmd_smoke_run_line_editing(WinxtermDstcmdShell *shell)
{
    static const char spaced[] = "alpha beta gamma";
    const size_t first_space = strlen("alpha");
    const size_t second_space = strlen("alpha beta");
    const size_t spaced_length = strlen(spaced);

    if (!winxterm_dstcmd_smoke_set_line(shell, spaced) ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[1;5D") ||
        shell->line_cursor != second_space ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[1;5D") ||
        shell->line_cursor != first_space ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[1;5D") ||
        shell->line_cursor != 0u) {
        return false;
    }

    if (!winxterm_dstcmd_smoke_set_line(shell, spaced)) {
        return false;
    }
    shell->line_cursor = 0u;
    if (!winxterm_dstcmd_smoke_feed_input(shell, "\x1b[1;5C") ||
        shell->line_cursor != first_space ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[1;5C") ||
        shell->line_cursor != second_space ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[1;5C") ||
        shell->line_cursor != spaced_length) {
        return false;
    }

    static const char escaped_spaced[] = "alpha beta\\ gamma delta";
    const size_t escaped_first_space = strlen("alpha");
    const size_t escaped_second_space = strlen("alpha beta\\ gamma");
    const size_t escaped_spaced_length = strlen(escaped_spaced);
    if (!winxterm_dstcmd_smoke_set_line(shell, escaped_spaced) ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[1;5D") ||
        shell->line_cursor != escaped_second_space ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[1;5D") ||
        shell->line_cursor != escaped_first_space ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[1;5D") ||
        shell->line_cursor != 0u) {
        return false;
    }

    if (!winxterm_dstcmd_smoke_set_line(shell, escaped_spaced)) {
        return false;
    }
    shell->line_cursor = 0u;
    if (!winxterm_dstcmd_smoke_feed_input(shell, "\x1b[1;5C") ||
        shell->line_cursor != escaped_first_space ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[1;5C") ||
        shell->line_cursor != escaped_second_space ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[1;5C") ||
        shell->line_cursor != escaped_spaced_length) {
        return false;
    }

    if (!winxterm_dstcmd_smoke_set_line(shell, "alpha beta\\ gamma") ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x17") ||
        strcmp(shell->line, "alpha ") != 0 ||
        shell->line_cursor != strlen("alpha ")) {
        return false;
    }

    if (!winxterm_dstcmd_smoke_set_line(shell, "alpha beta\\ gamma")) {
        return false;
    }
    shell->line_cursor = strlen("alpha ");
    if (!winxterm_dstcmd_smoke_feed_input(shell, "\x1b[3;5~") ||
        strcmp(shell->line, "alpha ") != 0 ||
        shell->line_cursor != strlen("alpha ")) {
        return false;
    }

    if (!winxterm_dstcmd_smoke_set_line(shell, "alphabeta") ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[1;5D") ||
        shell->line_cursor != 0u) {
        return false;
    }
    shell->line_cursor = 0u;
    if (!winxterm_dstcmd_smoke_feed_input(shell, "\x1b[1;5C") ||
        shell->line_cursor != strlen("alphabeta")) {
        return false;
    }

    if (!winxterm_dstcmd_smoke_set_line(shell, "ab") ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[D") ||
        shell->line_cursor != 1u ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[C") ||
        shell->line_cursor != 2u) {
        return false;
    }

    if (!winxterm_dstcmd_smoke_set_line(shell, "abc") ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[1H") ||
        shell->line_cursor != 0u ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[1F") ||
        shell->line_cursor != strlen("abc")) {
        return false;
    }

    static const char emoji[] = "\xf0\x9f\x98\x80";
    static const char emoji_line[] = "a" "\xf0\x9f\x98\x80" "b";
    const size_t emoji_start = strlen("a");
    const size_t emoji_end = strlen("a" "\xf0\x9f\x98\x80");
    const size_t emoji_line_length = strlen(emoji_line);
    if (!winxterm_dstcmd_smoke_set_line(shell, emoji_line) ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[D") ||
        shell->line_cursor != emoji_end ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[D") ||
        shell->line_cursor != emoji_start ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[C") ||
        shell->line_cursor != emoji_end) {
        return false;
    }

    if (!winxterm_dstcmd_smoke_set_line(shell, "a" "\xf0\x9f\x98\x80")) {
        return false;
    }
    winxterm_dstcmd_shell_backspace(shell);
    if (strcmp(shell->line, "a") != 0 || shell->line_cursor != strlen("a")) {
        return false;
    }

    if (!winxterm_dstcmd_smoke_set_line(shell, emoji_line)) {
        return false;
    }
    shell->line_cursor = emoji_start;
    winxterm_dstcmd_shell_delete_at_cursor(shell);
    if (strcmp(shell->line, "ab") != 0 || shell->line_cursor != emoji_start) {
        return false;
    }

    WinxtermDstcmdLinePosition position =
        winxterm_dstcmd_shell_line_position(emoji_line, emoji_line_length, 80, 0u);
    if (position.row != 0u || position.column != 3u || strlen(emoji) != 4u) {
        return false;
    }

    static const char multiline_prefix[] = "playmacro -i \"typestring one";
    static const char multiline_after_enter[] = "playmacro -i \"typestring one\n";
    if (!winxterm_dstcmd_smoke_set_line(shell, multiline_prefix) ||
        !winxterm_dstcmd_shell_handle_input_byte(shell, '\r') ||
        strcmp(shell->line, multiline_after_enter) != 0 ||
        shell->line_cursor != strlen(multiline_after_enter)) {
        return false;
    }
    position = winxterm_dstcmd_shell_line_position(shell->line,
                                                   shell->line_length,
                                                   80,
                                                   0u);
    if (position.row != 1u || position.column != 0u) {
        return false;
    }
    winxterm_dstcmd_shell_clear_line(shell);

    return true;
}

static const char *winxterm_dstcmd_smoke_history_search_selected(
    const WinxtermDstcmdShell *shell)
{
    if (shell == 0 || !shell->history_search_active ||
        shell->history_search_result_count == 0u ||
        shell->history_search_selected >= shell->history_search_result_count) {
        return 0;
    }
    const WinxtermDstcmdHistorySearchResult *result =
        shell->history_search_results + shell->history_search_selected;
    if (result->candidate_index >= shell->history_search_candidate_count) {
        return 0;
    }
    return shell->history_search_candidates[result->candidate_index].command;
}

static bool winxterm_dstcmd_smoke_run_history_search(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    if (!winxterm_dstcmd_shell_add_history_memory(shell, "echo smoke-alpha") ||
        !winxterm_dstcmd_shell_add_history_memory(shell, "cmake --build smoke-release") ||
        !winxterm_dstcmd_shell_add_history_memory(shell, "git status --short smoke-needle") ||
        !winxterm_dstcmd_shell_add_history_memory(shell, "echo delete-smoke-history-search")) {
        return false;
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    if (!winxterm_dstcmd_smoke_set_line(shell, "") ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x12needle") ||
        !shell->history_search_active ||
        strcmp(shell->history_search_query, "needle") != 0 ||
        strcmp(winxterm_dstcmd_smoke_history_search_selected(shell),
               "git status --short smoke-needle") != 0) {
        return false;
    }
    if (!winxterm_dstcmd_smoke_capture_contains(shell, "history [best fuzzy] > needle")) {
        return false;
    }
    if (!winxterm_dstcmd_smoke_capture_contains(shell, WINXTERM_DSTCMD_SMOKE_HISTORY_STYLE) ||
        !winxterm_dstcmd_smoke_capture_contains(shell,
                                                WINXTERM_DSTCMD_SMOKE_HISTORY_SELECTED_STYLE
                                                "\r\x1b[2K> git status --short smoke-needle <") ||
        winxterm_dstcmd_smoke_capture_contains(shell, "?1049h") ||
        winxterm_dstcmd_smoke_capture_contains(shell, "?1049l") ||
        winxterm_dstcmd_smoke_capture_contains(shell, "?25l") ||
        winxterm_dstcmd_smoke_capture_contains(shell, "keys:") ||
        winxterm_dstcmd_smoke_capture_contains(shell, "x   session") ||
        winxterm_dstcmd_smoke_capture_contains(shell, "x   saved")) {
        return false;
    }
    if (!winxterm_dstcmd_smoke_feed_input(shell, "\r") ||
        shell->history_search_active ||
        strcmp(shell->line, "git status --short smoke-needle") != 0 ||
        shell->line_cursor != strlen("git status --short smoke-needle")) {
        return false;
    }

    if (!winxterm_dstcmd_smoke_set_line(shell, "smoke")) {
        return false;
    }
    shell->line_cursor = strlen("smoke");
    winxterm_dstcmd_smoke_clear_capture(shell);
    if (!winxterm_dstcmd_smoke_feed_input(shell, "\x12-needle") ||
        strcmp(shell->history_search_query, "smoke-needle") != 0 ||
        strcmp(winxterm_dstcmd_smoke_history_search_selected(shell),
               "git status --short smoke-needle") != 0 ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\r") ||
        strcmp(shell->line, "git status --short smoke-needle") != 0) {
        return false;
    }

    if (!winxterm_dstcmd_smoke_set_line(shell, "abort line")) {
        return false;
    }
    shell->line_cursor = strlen("abort");
    winxterm_dstcmd_smoke_clear_capture(shell);
    if (!winxterm_dstcmd_smoke_feed_input(shell, "\x12zzz\x03") ||
        shell->history_search_active ||
        strcmp(shell->line, "abort line") != 0 ||
        shell->line_cursor != strlen("abort") ||
        !winxterm_dstcmd_smoke_capture_contains(shell, "^C\r\n")) {
        return false;
    }

    if (!winxterm_dstcmd_smoke_set_line(shell, "draft line")) {
        return false;
    }
    shell->line_cursor = strlen("draft");
    if (!winxterm_dstcmd_smoke_feed_input(shell, "\x12x\x07") ||
        shell->history_search_active ||
        strcmp(shell->line, "draft line") != 0 ||
        shell->line_cursor != strlen("draft")) {
        return false;
    }

    if (!winxterm_dstcmd_smoke_set_line(shell, "")) {
        return false;
    }
    if (!winxterm_dstcmd_smoke_feed_input(shell, "\x12" "dstshell-no-match-xyz-7913\r") ||
        shell->history_search_active ||
        strcmp(shell->line, "dstshell-no-match-xyz-7913") != 0) {
        return false;
    }

    if (!winxterm_dstcmd_smoke_set_line(shell, "") ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x12") ||
        shell->history_search_ranking != WINXTERM_DSTCMD_HISTORY_SEARCH_RANK_BEST ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x12") ||
        shell->history_search_ranking != WINXTERM_DSTCMD_HISTORY_SEARCH_RANK_RECENT ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1br") ||
        shell->history_search_matching != WINXTERM_DSTCMD_HISTORY_SEARCH_MATCH_CONTAINS ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x07") ||
        shell->history_search_active) {
        return false;
    }

    if (!winxterm_dstcmd_smoke_set_line(shell, "") ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x12" "delete-smoke-history-search") ||
        strcmp(winxterm_dstcmd_smoke_history_search_selected(shell),
               "echo delete-smoke-history-search") != 0 ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1b[3;2~Y") ||
        shell->history_search_delete_confirm_active) {
        return false;
    }
    const char *after_delete = winxterm_dstcmd_smoke_history_search_selected(shell);
    if ((after_delete != 0 && strcmp(after_delete, "echo delete-smoke-history-search") == 0) ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x1a") ||
        strcmp(winxterm_dstcmd_smoke_history_search_selected(shell),
               "echo delete-smoke-history-search") != 0 ||
        !winxterm_dstcmd_smoke_feed_input(shell, "\x07") ||
        shell->history_search_active) {
        return false;
    }

    return true;
}

static void winxterm_dstcmd_smoke_clear_capture(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return;
    }
    free(shell->capture_bytes);
    shell->capture_bytes = 0;
    shell->capture_count = 0u;
    shell->capture_capacity = 0u;
    shell->capture_failed = false;
}

static bool winxterm_dstcmd_smoke_capture_contains(WinxtermDstcmdShell *shell, const char *needle)
{
    if (shell == 0 || needle == 0) {
        return false;
    }
    size_t needle_length = strlen(needle);
    if (needle_length == 0u || needle_length > shell->capture_count || shell->capture_bytes == 0) {
        return false;
    }
    for (size_t i = 0u; i + needle_length <= shell->capture_count; ++i) {
        if (memcmp(shell->capture_bytes + i, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

static bool winxterm_dstcmd_smoke_capture_equals(WinxtermDstcmdShell *shell,
                                                 const uint8_t *bytes,
                                                 size_t byte_count)
{
    if (shell == 0 || bytes == 0) {
        return false;
    }
    return shell->capture_count == byte_count &&
           shell->capture_bytes != 0 &&
           memcmp(shell->capture_bytes, bytes, byte_count) == 0;
}

static size_t winxterm_dstcmd_smoke_capture_count(WinxtermDstcmdShell *shell, const char *needle)
{
    if (shell == 0 || needle == 0) {
        return 0u;
    }
    size_t needle_length = strlen(needle);
    if (needle_length == 0u || needle_length > shell->capture_count || shell->capture_bytes == 0) {
        return 0u;
    }
    size_t count = 0u;
    for (size_t i = 0u; i + needle_length <= shell->capture_count; ++i) {
        if (memcmp(shell->capture_bytes + i, needle, needle_length) == 0) {
            ++count;
            i += needle_length - 1u;
        }
    }
    return count;
}

static bool winxterm_dstcmd_smoke_create_file(const wchar_t *path)
{
    WinxtermDstcmdScratch scratch;
    winxterm_dstcmd_scratch_init(&scratch);
    WinxtermDstcmdWin32Path win32_path;
    if (!winxterm_dstcmd_path_prepare_win32_scratch(&scratch, path, &win32_path)) {
        winxterm_dstcmd_scratch_dispose(&scratch);
        return false;
    }
    HANDLE file = CreateFileW(win32_path.syscall,
                              GENERIC_WRITE,
                              0,
                              0,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              0);
    winxterm_dstcmd_scratch_dispose(&scratch);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    CloseHandle(file);
    return true;
}

static bool winxterm_dstcmd_smoke_write_file_bytes(const wchar_t *path,
                                                   const uint8_t *bytes,
                                                   size_t byte_count)
{
    WinxtermDstcmdScratch scratch;
    winxterm_dstcmd_scratch_init(&scratch);
    WinxtermDstcmdWin32Path win32_path;
    if (!winxterm_dstcmd_path_prepare_win32_scratch(&scratch, path, &win32_path)) {
        winxterm_dstcmd_scratch_dispose(&scratch);
        return false;
    }
    HANDLE file = CreateFileW(win32_path.syscall,
                              GENERIC_WRITE,
                              0,
                              0,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              0);
    winxterm_dstcmd_scratch_dispose(&scratch);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = true;
    size_t offset = 0u;
    while (offset < byte_count) {
        DWORD chunk = byte_count - offset > 4096u ? 4096u : (DWORD)(byte_count - offset);
        DWORD written = 0;
        if (!WriteFile(file, bytes + offset, chunk, &written, 0) || written == 0u) {
            ok = false;
            break;
        }
        offset += (size_t)written;
    }
    CloseHandle(file);
    return ok;
}

static bool winxterm_dstcmd_smoke_file_equals(const wchar_t *path,
                                              const uint8_t *bytes,
                                              size_t byte_count)
{
    WinxtermDstcmdScratch scratch;
    winxterm_dstcmd_scratch_init(&scratch);
    WinxtermDstcmdWin32Path win32_path;
    if (!winxterm_dstcmd_path_prepare_win32_scratch(&scratch, path, &win32_path)) {
        winxterm_dstcmd_scratch_dispose(&scratch);
        return false;
    }
    HANDLE file = CreateFileW(win32_path.syscall,
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              0,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              0);
    winxterm_dstcmd_scratch_dispose(&scratch);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    uint8_t *buffer = (uint8_t *)calloc(byte_count + 1u, sizeof(*buffer));
    if (buffer == 0) {
        CloseHandle(file);
        return false;
    }
    DWORD read_count = 0;
    bool ok = ReadFile(file, buffer, (DWORD)(byte_count + 1u), &read_count, 0) &&
              read_count == byte_count &&
              (byte_count == 0u || memcmp(buffer, bytes, byte_count) == 0);
    free(buffer);
    CloseHandle(file);
    return ok;
}

static bool winxterm_dstcmd_smoke_create_child_file(const wchar_t *directory, const wchar_t *name)
{
    wchar_t path[WINXTERM_DSTCMD_PATH_CAPACITY];
    return _snwprintf_s(path,
                        WINXTERM_DSTCMD_PATH_CAPACITY,
                        _TRUNCATE,
                        L"%ls\\%ls",
                        directory,
                        name) >= 0 &&
           winxterm_dstcmd_smoke_create_file(path);
}

static bool winxterm_dstcmd_smoke_create_directory(const wchar_t *path)
{
    WinxtermDstcmdScratch scratch;
    winxterm_dstcmd_scratch_init(&scratch);
    bool ok = winxterm_dstcmd_path_create_directory_scratch(&scratch, path);
    winxterm_dstcmd_scratch_dispose(&scratch);
    return ok;
}

static bool winxterm_dstcmd_smoke_create_child_directory(const wchar_t *directory, const wchar_t *name)
{
    wchar_t path[WINXTERM_DSTCMD_PATH_CAPACITY];
    if (_snwprintf_s(path,
                     WINXTERM_DSTCMD_PATH_CAPACITY,
                     _TRUNCATE,
                     L"%ls\\%ls",
                     directory,
                     name) < 0) {
        return false;
    }
    return winxterm_dstcmd_smoke_create_directory(path);
}

static bool winxterm_dstcmd_smoke_tab_delays_external_until_repeat(WinxtermDstcmdShell *shell)
{
    if (!winxterm_dstcmd_smoke_set_line(shell, "push")) {
        return false;
    }
    winxterm_dstcmd_smoke_clear_capture(shell);
    if (!winxterm_dstcmd_shell_handle_input_byte(shell, '\t')) {
        return false;
    }
    bool first_tab_ok =
        strcmp(shell->line, "push") == 0 &&
        shell->line_cursor == strlen("push") &&
        winxterm_dstcmd_smoke_capture_contains(shell,
                                               "push"
                                               WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_ON
                                               "A"
                                               WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_OFF) &&
        winxterm_dstcmd_smoke_capture_contains(shell,
                                               "push"
                                               WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_ON
                                               "B"
                                               WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_OFF) &&
        !winxterm_dstcmd_smoke_capture_contains(shell, "\x1b[38;2;255;165;0m") &&
        !winxterm_dstcmd_smoke_capture_contains(shell, "\x1b[38;2;192;192;192m");
    if (!first_tab_ok) {
        return false;
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    if (!winxterm_dstcmd_shell_handle_input_byte(shell, '\t')) {
        return false;
    }
    return strcmp(shell->line, "push") == 0 &&
           shell->line_cursor == strlen("push") &&
           winxterm_dstcmd_smoke_capture_contains(shell,
                                                  "\x1b[38;2;255;165;0mpush"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_ON
                                                  "d"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_OFF
                                                  "\x1b[0m") &&
           winxterm_dstcmd_smoke_capture_contains(shell,
                                                  "\x1b[38;2;192;192;192mpush"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_ON
                                                  "p"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_OFF
                                                  "ath.exe\x1b[0m") &&
           winxterm_dstcmd_smoke_capture_contains(shell,
                                                  "\r\n\r\npush"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_ON
                                                  "A"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_OFF);
}

static bool winxterm_dstcmd_smoke_tab_lists_with_highlights(WinxtermDstcmdShell *shell)
{
    if (!winxterm_dstcmd_smoke_set_line(shell, "echo A")) {
        return false;
    }
    winxterm_dstcmd_smoke_clear_capture(shell);
    if (!winxterm_dstcmd_shell_handle_input_byte(shell, '\t')) {
        return false;
    }
    return strcmp(shell->line, "echo A") == 0 &&
           shell->line_cursor == strlen("echo A") &&
           winxterm_dstcmd_smoke_capture_contains(shell,
                                                  "A"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_ON
                                                  "A"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_OFF
                                                  "AB") &&
           winxterm_dstcmd_smoke_capture_contains(shell,
                                                  "A"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_ON
                                                  "A"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_OFF
                                                  "B") &&
           winxterm_dstcmd_smoke_capture_contains(shell,
                                                  "A"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_ON
                                                  "B"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_OFF) &&
           winxterm_dstcmd_smoke_capture_contains(shell,
                                                  "A"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_ON
                                                  "B"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_OFF
                                                  "B") &&
           winxterm_dstcmd_smoke_capture_contains(shell,
                                                  "A"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_ON
                                                  "A"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_OFF
                                                  "AC");
}

static bool winxterm_dstcmd_smoke_tab_lists_aa_with_highlights(WinxtermDstcmdShell *shell)
{
    if (!winxterm_dstcmd_smoke_set_line(shell, "echo AA")) {
        return false;
    }
    winxterm_dstcmd_smoke_clear_capture(shell);
    if (!winxterm_dstcmd_shell_handle_input_byte(shell, '\t')) {
        return false;
    }
    return strcmp(shell->line, "echo AA") == 0 &&
           shell->line_cursor == strlen("echo AA") &&
           winxterm_dstcmd_smoke_capture_contains(shell,
                                                  "AA"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_ON
                                                  "A"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_OFF
                                                  "B") &&
           winxterm_dstcmd_smoke_capture_contains(shell,
                                                  "AA"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_ON
                                                  "B"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_OFF) &&
           winxterm_dstcmd_smoke_capture_contains(shell,
                                                  "AA"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_ON
                                                  "A"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_OFF
                                                  "C");
}

static bool winxterm_dstcmd_smoke_tab_lists_surrogate_highlight(WinxtermDstcmdShell *shell)
{
    static const char emoji_utf8[] = "\xf0\x9f\x98\x80";
    if (!winxterm_dstcmd_smoke_set_line(shell, "echo ")) {
        return false;
    }
    winxterm_dstcmd_smoke_clear_capture(shell);
    if (!winxterm_dstcmd_shell_handle_input_byte(shell, '\t')) {
        return false;
    }
    return strcmp(shell->line, "echo ") == 0 &&
           shell->line_cursor == strlen("echo ") &&
           winxterm_dstcmd_smoke_capture_contains(shell,
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_ON
                                                  "\xf0\x9f\x98\x80"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_OFF
                                                  ".txt") &&
           strlen(emoji_utf8) == 4u;
}

static bool winxterm_dstcmd_smoke_tab_lists_surrogate_suffix_highlight(WinxtermDstcmdShell *shell)
{
    static const char prefix[] = "echo " "\xf0\x9f\x98\x80";
    if (!winxterm_dstcmd_smoke_set_line(shell, prefix)) {
        return false;
    }
    winxterm_dstcmd_smoke_clear_capture(shell);
    if (!winxterm_dstcmd_shell_handle_input_byte(shell, '\t')) {
        return false;
    }
    return strcmp(shell->line, prefix) == 0 &&
           shell->line_cursor == strlen(prefix) &&
           winxterm_dstcmd_smoke_capture_contains(shell,
                                                  "\xf0\x9f\x98\x80"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_ON
                                                  "A"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_OFF) &&
           winxterm_dstcmd_smoke_capture_contains(shell,
                                                  "\xf0\x9f\x98\x80"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_ON
                                                  "B"
                                                  WINXTERM_DSTCMD_SMOKE_COMPLETION_HIGHLIGHT_OFF);
}

static bool winxterm_dstcmd_smoke_run_completion(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    if (!winxterm_dstcmd_smoke_tab_completes_to(shell, "push", "pushd")) {
        return false;
    }

    wchar_t original_cwd[WINXTERM_DSTCMD_PATH_CAPACITY];
    wcscpy_s(original_cwd, WINXTERM_DSTCMD_PATH_CAPACITY, shell->cwd);

    wchar_t temp_root[WINXTERM_DSTCMD_PATH_CAPACITY];
    DWORD temp_length = GetTempPathW(WINXTERM_DSTCMD_PATH_CAPACITY, temp_root);
    if (temp_length == 0u || temp_length >= WINXTERM_DSTCMD_PATH_CAPACITY) {
        return false;
    }

    wchar_t temp_dir[WINXTERM_DSTCMD_PATH_CAPACITY];
    if (_snwprintf_s(temp_dir,
                     WINXTERM_DSTCMD_PATH_CAPACITY,
                     _TRUNCATE,
                     L"%lswinxterm-dstcmd-completion-%lu-%lu",
                     temp_root,
                     (unsigned long)GetCurrentProcessId(),
                     (unsigned long)GetTickCount()) < 0) {
        return false;
    }
    if (!CreateDirectoryW(temp_dir, 0)) {
        return false;
    }

    wchar_t path_bin[WINXTERM_DSTCMD_PATH_CAPACITY];
    wchar_t path_exe[WINXTERM_DSTCMD_PATH_CAPACITY];
    path_bin[0] = L'\0';
    path_exe[0] = L'\0';
    wchar_t *original_path = 0;
    bool had_original_path = false;
    DWORD original_path_length = GetEnvironmentVariableW(L"PATH", 0, 0);
    if (original_path_length != 0u) {
        original_path = (wchar_t *)calloc((size_t)original_path_length + 1u, sizeof(*original_path));
        if (original_path == 0 ||
            GetEnvironmentVariableW(L"PATH", original_path, original_path_length + 1u) == 0u) {
            free(original_path);
            (void)winxterm_dstcmd_shell_set_cwd(shell, original_cwd);
            (void)RemoveDirectoryW(temp_dir);
            return false;
        }
        had_original_path = true;
    }

    wchar_t target_file[WINXTERM_DSTCMD_PATH_CAPACITY];
    target_file[0] = L'\0';
    bool ok = _snwprintf_s(target_file,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%ls\\completion-target.txt",
                           temp_dir) >= 0 &&
              winxterm_dstcmd_smoke_create_file(target_file) &&
              winxterm_dstcmd_shell_set_cwd(shell, temp_dir) &&
              winxterm_dstcmd_smoke_tab_completes_to(shell,
                                                     "echo completion-t",
                                                     "echo completion-target.txt") &&
              winxterm_dstcmd_smoke_create_child_file(temp_dir, L"Program Files") &&
              winxterm_dstcmd_smoke_tab_completes_to(shell,
                                                     "echo Program\\ F",
                                                     "echo Program\\ Files") &&
              winxterm_dstcmd_smoke_create_child_file(temp_dir, L"AAAB") &&
              winxterm_dstcmd_smoke_create_child_file(temp_dir, L"AAB") &&
              winxterm_dstcmd_smoke_create_child_file(temp_dir, L"AB") &&
              winxterm_dstcmd_smoke_create_child_file(temp_dir, L"ABB") &&
              winxterm_dstcmd_smoke_create_child_file(temp_dir, L"AAAC") &&
              winxterm_dstcmd_smoke_create_child_file(temp_dir, L"pushA") &&
              winxterm_dstcmd_smoke_create_child_file(temp_dir, L"pushB") &&
              winxterm_dstcmd_smoke_create_child_file(temp_dir, L"\xd83d\xde00" L".txt") &&
              winxterm_dstcmd_smoke_create_child_file(temp_dir, L"\xd83d\xde00" L"A") &&
              winxterm_dstcmd_smoke_create_child_file(temp_dir, L"\xd83d\xde00" L"B") &&
              winxterm_dstcmd_smoke_create_child_file(temp_dir, L"ThisIsOneString") &&
              winxterm_dstcmd_smoke_create_child_file(temp_dir, L"ThisIsoneStringThatsDifferent") &&
              winxterm_dstcmd_smoke_tab_completes_to(shell, "echo t", "echo ThisIs") &&
              winxterm_dstcmd_smoke_tab_completes_to(shell,
                                                     "echo ThisIso",
                                                     "echo ThisIsoneStringThatsDifferent") &&
              winxterm_dstcmd_smoke_tab_completes_to(shell,
                                                     "echo ThisIsO",
                                                     "echo ThisIsOneString") &&
              winxterm_dstcmd_smoke_create_child_directory(temp_dir, L"CaseDir") &&
              winxterm_dstcmd_smoke_create_child_file(temp_dir, L"CaseDirExtra.txt") &&
              winxterm_dstcmd_smoke_tab_twice_completes_to(shell,
                                                           "echo CaseDir",
                                                           "echo CaseDir",
                                                           "echo CaseDir/") &&
              winxterm_dstcmd_smoke_tab_completes_to(shell,
                                                     "echo " "\xf0\x9f\x98\x80" ".t",
                                                     "echo " "\xf0\x9f\x98\x80" ".txt") &&
              _snwprintf_s(path_bin,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%ls\\pathbin",
                           temp_dir) >= 0 &&
              winxterm_dstcmd_smoke_create_child_directory(temp_dir, L"pathbin") &&
              _snwprintf_s(path_exe,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%ls\\pushpath.exe",
                           path_bin) >= 0 &&
              winxterm_dstcmd_smoke_create_file(path_exe) &&
              SetEnvironmentVariableW(L"PATH", path_bin) &&
              winxterm_dstcmd_smoke_tab_lists_with_highlights(shell) &&
              winxterm_dstcmd_smoke_tab_lists_aa_with_highlights(shell) &&
              winxterm_dstcmd_smoke_tab_lists_surrogate_highlight(shell) &&
              winxterm_dstcmd_smoke_tab_lists_surrogate_suffix_highlight(shell) &&
              winxterm_dstcmd_smoke_tab_delays_external_until_repeat(shell);

    (void)winxterm_dstcmd_shell_set_cwd(shell, original_cwd);
    if (had_original_path) {
        (void)SetEnvironmentVariableW(L"PATH", original_path);
    } else {
        (void)SetEnvironmentVariableW(L"PATH", 0);
    }
    free(original_path);
    if (target_file[0] != L'\0') {
        (void)DeleteFileW(target_file);
    }
    wchar_t child_path[WINXTERM_DSTCMD_PATH_CAPACITY];
    static const wchar_t *child_names[] = {
        L"AAAB",
        L"AAB",
        L"AB",
        L"ABB",
        L"AAAC",
        L"Program Files",
        L"pushA",
        L"pushB",
        L"ThisIsOneString",
        L"ThisIsoneStringThatsDifferent",
        L"CaseDirExtra.txt",
        L"\xd83d\xde00" L".txt",
        L"\xd83d\xde00" L"A",
        L"\xd83d\xde00" L"B",
    };
    for (size_t i = 0u; i < sizeof(child_names) / sizeof(child_names[0]); ++i) {
        if (_snwprintf_s(child_path,
                         WINXTERM_DSTCMD_PATH_CAPACITY,
                         _TRUNCATE,
                         L"%ls\\%ls",
                         temp_dir,
                         child_names[i]) >= 0) {
            (void)DeleteFileW(child_path);
        }
    }
    static const wchar_t *child_directories[] = {
        L"CaseDir",
    };
    for (size_t i = 0u; i < sizeof(child_directories) / sizeof(child_directories[0]); ++i) {
        if (_snwprintf_s(child_path,
                         WINXTERM_DSTCMD_PATH_CAPACITY,
                         _TRUNCATE,
                         L"%ls\\%ls",
                         temp_dir,
                         child_directories[i]) >= 0) {
            (void)RemoveDirectoryW(child_path);
        }
    }
    if (path_exe[0] != L'\0') {
        (void)DeleteFileW(path_exe);
    }
    if (path_bin[0] != L'\0') {
        (void)RemoveDirectoryW(path_bin);
    }
    (void)RemoveDirectoryW(temp_dir);
    return ok;
}

static bool winxterm_dstcmd_smoke_run_which(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    if (winxterm_dstcmd_shell_submit_line(shell, L"which cd") != 0 ||
        !winxterm_dstcmd_smoke_capture_contains(shell, "  ✅ ") ||
        !winxterm_dstcmd_smoke_capture_contains(shell,
                                                "\x1b[1;38;2;255;255;255m[dstcmd builtin]") ||
        !winxterm_dstcmd_smoke_capture_contains(shell, "\x1b[38;2;0;255;0mcd")) {
        return false;
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    if (winxterm_dstcmd_shell_submit_line(shell, L"which exit") != 0 ||
        !winxterm_dstcmd_smoke_capture_contains(shell, "\x1b[38;2;0;255;0mexit")) {
        return false;
    }

    wchar_t temp_root[WINXTERM_DSTCMD_PATH_CAPACITY];
    DWORD temp_length = GetTempPathW(WINXTERM_DSTCMD_PATH_CAPACITY, temp_root);
    if (temp_length == 0u || temp_length >= WINXTERM_DSTCMD_PATH_CAPACITY) {
        return false;
    }

    wchar_t temp_dir[WINXTERM_DSTCMD_PATH_CAPACITY];
    wchar_t first_dir[WINXTERM_DSTCMD_PATH_CAPACITY];
    wchar_t second_dir[WINXTERM_DSTCMD_PATH_CAPACITY];
    wchar_t first_exe[WINXTERM_DSTCMD_PATH_CAPACITY];
    wchar_t second_exe[WINXTERM_DSTCMD_PATH_CAPACITY];
    wchar_t first_ls_exe[WINXTERM_DSTCMD_PATH_CAPACITY];
    wchar_t second_ls_exe[WINXTERM_DSTCMD_PATH_CAPACITY];
    wchar_t path_value[WINXTERM_DSTCMD_PATH_CAPACITY * 2u];
    temp_dir[0] = L'\0';
    first_dir[0] = L'\0';
    second_dir[0] = L'\0';
    first_exe[0] = L'\0';
    second_exe[0] = L'\0';
    first_ls_exe[0] = L'\0';
    second_ls_exe[0] = L'\0';

    wchar_t *original_path = 0;
    bool had_original_path = false;
    DWORD original_path_length = GetEnvironmentVariableW(L"PATH", 0, 0);
    if (original_path_length != 0u) {
        original_path = (wchar_t *)calloc((size_t)original_path_length + 1u, sizeof(*original_path));
        if (original_path == 0 ||
            GetEnvironmentVariableW(L"PATH", original_path, original_path_length + 1u) == 0u) {
            free(original_path);
            return false;
        }
        had_original_path = true;
    }

    bool ok = _snwprintf_s(temp_dir,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%lswinxterm-dstcmd-which-%lu-%lu",
                           temp_root,
                           (unsigned long)GetCurrentProcessId(),
                           (unsigned long)GetTickCount()) >= 0 &&
              CreateDirectoryW(temp_dir, 0) &&
              _snwprintf_s(first_dir,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%ls\\first",
                           temp_dir) >= 0 &&
              _snwprintf_s(second_dir,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%ls\\second",
                           temp_dir) >= 0 &&
              CreateDirectoryW(first_dir, 0) &&
              CreateDirectoryW(second_dir, 0) &&
              _snwprintf_s(first_exe,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%ls\\bash.exe",
                           first_dir) >= 0 &&
              _snwprintf_s(second_exe,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%ls\\bash.exe",
                           second_dir) >= 0 &&
              _snwprintf_s(first_ls_exe,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%ls\\ls.exe",
                           first_dir) >= 0 &&
              _snwprintf_s(second_ls_exe,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%ls\\ls.exe",
                           second_dir) >= 0 &&
              winxterm_dstcmd_smoke_create_file(first_exe) &&
              winxterm_dstcmd_smoke_create_file(second_exe) &&
              winxterm_dstcmd_smoke_create_file(first_ls_exe) &&
              winxterm_dstcmd_smoke_create_file(second_ls_exe) &&
              _snwprintf_s(path_value,
                           sizeof(path_value) / sizeof(path_value[0]),
                           _TRUNCATE,
                           L"%ls;%ls",
                           first_dir,
                           second_dir) >= 0 &&
              SetEnvironmentVariableW(L"PATH", path_value);

    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"which bash") == 0 &&
             winxterm_dstcmd_smoke_capture_contains(shell, "  ✅ ") &&
             winxterm_dstcmd_smoke_capture_contains(shell, "  \x1b[38;2;192;192;192m2\x1b[0m ") &&
             winxterm_dstcmd_smoke_capture_count(shell, "bash.exe") == 2u &&
             winxterm_dstcmd_smoke_capture_contains(shell, "\x1b[38;2;0;255;0mbash.exe") &&
             winxterm_dstcmd_smoke_capture_contains(shell, "\x1b[38;2;192;192;192mbash.exe");
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"which ls") == 0 &&
             winxterm_dstcmd_smoke_capture_contains(shell, "  ✅ ") &&
             winxterm_dstcmd_smoke_capture_contains(shell, "  \x1b[38;2;192;192;192m2\x1b[0m ") &&
             winxterm_dstcmd_smoke_capture_contains(shell, "  \x1b[38;2;192;192;192m3\x1b[0m ") &&
             winxterm_dstcmd_smoke_capture_contains(shell, "\x1b[38;2;0;255;0mls") &&
             winxterm_dstcmd_smoke_capture_count(shell, "ls.exe") == 2u &&
             winxterm_dstcmd_smoke_capture_contains(shell, "\x1b[38;2;192;192;192mls.exe") &&
             !winxterm_dstcmd_smoke_capture_contains(shell, "\x1b[38;2;0;255;0mls.exe");
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"which definitely-not-a-smoke-command") == 1 &&
             winxterm_dstcmd_smoke_capture_contains(shell,
                                                    "definitely-not-a-smoke-command: not found");
    }

    if (had_original_path) {
        (void)SetEnvironmentVariableW(L"PATH", original_path);
    } else {
        (void)SetEnvironmentVariableW(L"PATH", 0);
    }
    free(original_path);
    if (first_exe[0] != L'\0') {
        (void)DeleteFileW(first_exe);
    }
    if (second_exe[0] != L'\0') {
        (void)DeleteFileW(second_exe);
    }
    if (first_ls_exe[0] != L'\0') {
        (void)DeleteFileW(first_ls_exe);
    }
    if (second_ls_exe[0] != L'\0') {
        (void)DeleteFileW(second_ls_exe);
    }
    if (first_dir[0] != L'\0') {
        (void)RemoveDirectoryW(first_dir);
    }
    if (second_dir[0] != L'\0') {
        (void)RemoveDirectoryW(second_dir);
    }
    if (temp_dir[0] != L'\0') {
        (void)RemoveDirectoryW(temp_dir);
    }
    return ok;
}

static bool winxterm_dstcmd_smoke_run_cat(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }

    wchar_t original_cwd[WINXTERM_DSTCMD_PATH_CAPACITY];
    if (wcscpy_s(original_cwd, WINXTERM_DSTCMD_PATH_CAPACITY, shell->cwd) != 0) {
        return false;
    }

    wchar_t temp_root[WINXTERM_DSTCMD_PATH_CAPACITY];
    DWORD temp_length = GetTempPathW(WINXTERM_DSTCMD_PATH_CAPACITY, temp_root);
    if (temp_length == 0u || temp_length >= WINXTERM_DSTCMD_PATH_CAPACITY) {
        return false;
    }

    wchar_t temp_dir[WINXTERM_DSTCMD_PATH_CAPACITY];
    wchar_t first_path[WINXTERM_DSTCMD_PATH_CAPACITY];
    wchar_t second_path[WINXTERM_DSTCMD_PATH_CAPACITY];
    temp_dir[0] = L'\0';
    first_path[0] = L'\0';
    second_path[0] = L'\0';

    static const uint8_t first_bytes[] = {
        (uint8_t)'A',
        0x00u,
        0xffu,
        (uint8_t)'\n',
        (uint8_t)'B',
    };
    static const uint8_t second_bytes[] = {
        0x80u,
        (uint8_t)'C',
        (uint8_t)'\r',
        (uint8_t)'\n',
    };
    uint8_t combined[sizeof(first_bytes) + sizeof(second_bytes)];
    memcpy(combined, first_bytes, sizeof(first_bytes));
    memcpy(combined + sizeof(first_bytes), second_bytes, sizeof(second_bytes));

    bool ok = _snwprintf_s(temp_dir,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%lswinxterm-dstcmd-cat-%lu-%lu",
                           temp_root,
                           (unsigned long)GetCurrentProcessId(),
                           (unsigned long)GetTickCount()) >= 0 &&
              CreateDirectoryW(temp_dir, 0) &&
              _snwprintf_s(first_path,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%ls\\cat-a.bin",
                           temp_dir) >= 0 &&
              _snwprintf_s(second_path,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%ls\\cat-b.bin",
                           temp_dir) >= 0 &&
              winxterm_dstcmd_smoke_write_file_bytes(first_path,
                                                     first_bytes,
                                                     sizeof(first_bytes)) &&
              winxterm_dstcmd_smoke_write_file_bytes(second_path,
                                                     second_bytes,
                                                     sizeof(second_bytes)) &&
              winxterm_dstcmd_shell_set_cwd(shell, temp_dir);

    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"cat cat-a.bin") == 0 &&
             winxterm_dstcmd_smoke_capture_equals(shell, first_bytes, sizeof(first_bytes));
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"cat -- cat-a.bin") == 0 &&
             winxterm_dstcmd_smoke_capture_equals(shell, first_bytes, sizeof(first_bytes));
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"cat cat-a.bin cat-b.bin") == 0 &&
             winxterm_dstcmd_smoke_capture_equals(shell, combined, sizeof(combined));
    }
    if (ok) {
        static const uint8_t echo_ok[] = {
            (uint8_t)'o',
            (uint8_t)'k',
            (uint8_t)'\r',
            (uint8_t)'\n',
        };
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"echo ok | cat") == 0 &&
             winxterm_dstcmd_smoke_capture_equals(shell, echo_ok, sizeof(echo_ok));
        if (ok) {
            winxterm_dstcmd_smoke_clear_capture(shell);
            ok = winxterm_dstcmd_shell_submit_line(shell, L"echo ok | cat -") == 0 &&
                 winxterm_dstcmd_smoke_capture_equals(shell, echo_ok, sizeof(echo_ok));
        }
    }

    (void)winxterm_dstcmd_shell_set_cwd(shell, original_cwd);
    if (first_path[0] != L'\0') {
        (void)DeleteFileW(first_path);
    }
    if (second_path[0] != L'\0') {
        (void)DeleteFileW(second_path);
    }
    if (temp_dir[0] != L'\0') {
        (void)RemoveDirectoryW(temp_dir);
    }
    return ok;
}

static bool winxterm_dstcmd_smoke_run_redirect(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }

    wchar_t original_cwd[WINXTERM_DSTCMD_PATH_CAPACITY];
    if (wcscpy_s(original_cwd, WINXTERM_DSTCMD_PATH_CAPACITY, shell->cwd) != 0) {
        return false;
    }

    wchar_t temp_root[WINXTERM_DSTCMD_PATH_CAPACITY];
    DWORD temp_length = GetTempPathW(WINXTERM_DSTCMD_PATH_CAPACITY, temp_root);
    if (temp_length == 0u || temp_length >= WINXTERM_DSTCMD_PATH_CAPACITY) {
        return false;
    }

    typedef struct WinxtermDstcmdSmokeRedirectPaths {
        wchar_t temp_dir[WINXTERM_DSTCMD_PATH_CAPACITY];
        wchar_t out_path[WINXTERM_DSTCMD_PATH_CAPACITY];
        wchar_t tee_path[WINXTERM_DSTCMD_PATH_CAPACITY];
        wchar_t space_path[WINXTERM_DSTCMD_PATH_CAPACITY];
        wchar_t escaped_path[WINXTERM_DSTCMD_PATH_CAPACITY];
        wchar_t pipe_path[WINXTERM_DSTCMD_PATH_CAPACITY];
        wchar_t stderr_path[WINXTERM_DSTCMD_PATH_CAPACITY];
        wchar_t target_dir[WINXTERM_DSTCMD_PATH_CAPACITY];
    } WinxtermDstcmdSmokeRedirectPaths;
    WinxtermDstcmdSmokeRedirectPaths *paths =
        (WinxtermDstcmdSmokeRedirectPaths *)calloc(1u, sizeof(*paths));
    if (paths == 0) {
        return false;
    }
    wchar_t *temp_dir = paths->temp_dir;
    wchar_t *out_path = paths->out_path;
    wchar_t *tee_path = paths->tee_path;
    wchar_t *space_path = paths->space_path;
    wchar_t *escaped_path = paths->escaped_path;
    wchar_t *pipe_path = paths->pipe_path;
    wchar_t *stderr_path = paths->stderr_path;
    wchar_t *target_dir = paths->target_dir;
    temp_dir[0] = L'\0';
    out_path[0] = L'\0';
    tee_path[0] = L'\0';
    space_path[0] = L'\0';
    escaped_path[0] = L'\0';
    pipe_path[0] = L'\0';
    stderr_path[0] = L'\0';
    target_dir[0] = L'\0';

    bool ok = _snwprintf_s(temp_dir,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%lswinxterm-dstcmd-redirect-%lu-%lu",
                           temp_root,
                           (unsigned long)GetCurrentProcessId(),
                           (unsigned long)GetTickCount()) >= 0 &&
              CreateDirectoryW(temp_dir, 0) &&
              _snwprintf_s(out_path,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%ls\\out.txt",
                           temp_dir) >= 0 &&
              _snwprintf_s(tee_path,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%ls\\tee.txt",
                           temp_dir) >= 0 &&
              _snwprintf_s(space_path,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%ls\\space name.txt",
                           temp_dir) >= 0 &&
              _snwprintf_s(escaped_path,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%ls\\escaped name.txt",
                           temp_dir) >= 0 &&
              _snwprintf_s(pipe_path,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%ls\\pipe.txt",
                           temp_dir) >= 0 &&
              _snwprintf_s(stderr_path,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%ls\\stderr.txt",
                           temp_dir) >= 0 &&
              _snwprintf_s(target_dir,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%ls\\target-dir",
                           temp_dir) >= 0 &&
              CreateDirectoryW(target_dir, 0) &&
              winxterm_dstcmd_shell_set_cwd(shell, temp_dir);

    static const uint8_t initial_bytes[] = { 'o', 'l', 'd', '\r', '\n', 'd', 'a', 't', 'a' };
    static const uint8_t tiny_bytes[] = { 't', 'i', 'n', 'y', '\r', '\n' };
    static const uint8_t append_bytes[] = {
        't', 'i', 'n', 'y', '\r', '\n',
        'a', 'p', 'p', 'e', 'n', 'd', '\r', '\n'
    };
    static const uint8_t tee_bytes[] = { 't', 'e', 'e', '\r', '\n' };
    static const uint8_t quoted_bytes[] = { 'q', 'u', 'o', 't', 'e', 'd', '\r', '\n' };
    static const uint8_t escaped_bytes[] = { 'e', 's', 'c', 'a', 'p', 'e', 'd', '\r', '\n' };
    static const uint8_t pipe_bytes[] = { 'p', 'i', 'p', 'e', '\r', '\n' };

    if (ok) {
        ok = winxterm_dstcmd_smoke_write_file_bytes(out_path,
                                                    initial_bytes,
                                                    sizeof(initial_bytes));
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"echo tiny > out.txt") == 0 &&
             winxterm_dstcmd_smoke_file_equals(out_path, tiny_bytes, sizeof(tiny_bytes)) &&
             winxterm_dstcmd_smoke_capture_contains(shell, "6 bytes written in") &&
             !winxterm_dstcmd_smoke_capture_contains(shell, "tiny\r\n");
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"echo append >> out.txt") == 0 &&
             winxterm_dstcmd_smoke_file_equals(out_path, append_bytes, sizeof(append_bytes)) &&
             winxterm_dstcmd_smoke_capture_contains(shell, "14 bytes written in");
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"echo tee t> tee.txt") == 0 &&
             winxterm_dstcmd_smoke_file_equals(tee_path, tee_bytes, sizeof(tee_bytes)) &&
             winxterm_dstcmd_smoke_capture_contains(shell, "tee\r\n") &&
             winxterm_dstcmd_smoke_capture_contains(shell, "5 bytes written in");
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"echo quoted > \"space name.txt\"") == 0 &&
             winxterm_dstcmd_smoke_file_equals(space_path, quoted_bytes, sizeof(quoted_bytes));
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"echo escaped > escaped\\ name.txt") == 0 &&
             winxterm_dstcmd_smoke_file_equals(escaped_path, escaped_bytes, sizeof(escaped_bytes));
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"echo pipe | cat > pipe.txt") == 0 &&
             winxterm_dstcmd_smoke_file_equals(pipe_path, pipe_bytes, sizeof(pipe_bytes)) &&
             winxterm_dstcmd_smoke_capture_contains(shell, "6 bytes written in") &&
             !winxterm_dstcmd_smoke_capture_contains(shell, "pipe\r\n");
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"cat missing-file > stderr.txt") == 1 &&
             winxterm_dstcmd_smoke_file_equals(stderr_path, 0, 0u) &&
             winxterm_dstcmd_smoke_capture_contains(shell, "cat: missing-file: cannot stat");
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"echo missing >") == 2 &&
             winxterm_dstcmd_smoke_capture_contains(shell, "dstcmd: missing redirect target");
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"echo one > out.txt > tee.txt") == 2 &&
             winxterm_dstcmd_smoke_capture_contains(shell, "dstcmd: multiple stdout redirects");
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"echo before > out.txt | cat") == 2 &&
             winxterm_dstcmd_smoke_capture_contains(
                 shell,
                 "dstcmd: stdout redirect is only supported on the final pipeline stage");
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"echo fail > target-dir") == 1 &&
             winxterm_dstcmd_smoke_capture_contains(shell, "dstcmd: redirect open '") &&
             winxterm_dstcmd_smoke_capture_contains(shell, "target-dir");
    }

    (void)winxterm_dstcmd_shell_set_cwd(shell, original_cwd);
    if (out_path[0] != L'\0') {
        (void)DeleteFileW(out_path);
    }
    if (tee_path[0] != L'\0') {
        (void)DeleteFileW(tee_path);
    }
    if (space_path[0] != L'\0') {
        (void)DeleteFileW(space_path);
    }
    if (escaped_path[0] != L'\0') {
        (void)DeleteFileW(escaped_path);
    }
    if (pipe_path[0] != L'\0') {
        (void)DeleteFileW(pipe_path);
    }
    if (stderr_path[0] != L'\0') {
        (void)DeleteFileW(stderr_path);
    }
    if (target_dir[0] != L'\0') {
        (void)RemoveDirectoryW(target_dir);
    }
    if (temp_dir[0] != L'\0') {
        (void)RemoveDirectoryW(temp_dir);
    }
    free(paths);
    return ok;
}

static bool winxterm_dstcmd_smoke_run_long_paths(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }

    WinxtermDstcmdScratch scratch;
    winxterm_dstcmd_scratch_init(&scratch);
    wchar_t *original_cwd = winxterm_dstcmd_scratch_alloc_path(&scratch);
    wchar_t *temp_root = winxterm_dstcmd_scratch_alloc_path(&scratch);
    wchar_t *base_dir = winxterm_dstcmd_scratch_alloc_path(&scratch);
    wchar_t *deep_dir = winxterm_dstcmd_scratch_alloc_path(&scratch);
    wchar_t *next_dir = winxterm_dstcmd_scratch_alloc_path(&scratch);
    size_t command_count = WINXTERM_DSTCMD_PATH_CAPACITY + 64u;
    wchar_t *command = winxterm_dstcmd_scratch_alloc_wchars(&scratch, command_count);
    wchar_t *alpha_path = winxterm_dstcmd_scratch_alloc_path(&scratch);
    wchar_t *source_dir = winxterm_dstcmd_scratch_alloc_path(&scratch);
    wchar_t *payload_path = winxterm_dstcmd_scratch_alloc_path(&scratch);
    if (original_cwd == 0 || temp_root == 0 || base_dir == 0 || deep_dir == 0 ||
        next_dir == 0 || command == 0 || alpha_path == 0 || source_dir == 0 || payload_path == 0) {
        winxterm_dstcmd_scratch_dispose(&scratch);
        return false;
    }
    if (wcscpy_s(original_cwd, WINXTERM_DSTCMD_PATH_CAPACITY, shell->cwd) != 0) {
        winxterm_dstcmd_scratch_dispose(&scratch);
        return false;
    }

    DWORD temp_length = GetTempPathW(WINXTERM_DSTCMD_PATH_CAPACITY, temp_root);
    if (temp_length == 0u || temp_length >= WINXTERM_DSTCMD_PATH_CAPACITY) {
        winxterm_dstcmd_scratch_dispose(&scratch);
        return false;
    }

    base_dir[0] = L'\0';
    deep_dir[0] = L'\0';

    bool ok = _snwprintf_s(base_dir,
                           WINXTERM_DSTCMD_PATH_CAPACITY,
                           _TRUNCATE,
                           L"%lswinxterm-dstcmd-long-%lu-%lu",
                           temp_root,
                           (unsigned long)GetCurrentProcessId(),
                           (unsigned long)GetTickCount()) >= 0 &&
              winxterm_dstcmd_path_to_display(base_dir, base_dir, WINXTERM_DSTCMD_PATH_CAPACITY) &&
              winxterm_dstcmd_smoke_create_directory(base_dir) &&
              wcscpy_s(deep_dir, WINXTERM_DSTCMD_PATH_CAPACITY, base_dir) == 0;

    for (unsigned int i = 0u; ok && wcslen(deep_dir) < 280u; ++i) {
        wchar_t segment[96];
        ok = _snwprintf_s(segment,
                          sizeof(segment) / sizeof(segment[0]),
                          _TRUNCATE,
                          L"segment-%02u-abcdefghijklmnopqrstuvwxyzabcdefghijklmnop",
                          i) >= 0 &&
             winxterm_dstcmd_path_append_child(deep_dir,
                                               segment,
                                               next_dir,
                                               WINXTERM_DSTCMD_PATH_CAPACITY) &&
             winxterm_dstcmd_smoke_create_directory(next_dir) &&
             wcscpy_s(deep_dir, WINXTERM_DSTCMD_PATH_CAPACITY, next_dir) == 0;
    }
    ok = ok && wcslen(deep_dir) > 260u;

    static const char alpha_text[] = "long-path-alpha\n";
    static const char payload_text[] = "long-path-payload\n";
    if (ok) {
        ok = winxterm_dstcmd_path_append_child(deep_dir,
                                               L"alpha file.txt",
                                               alpha_path,
                                               WINXTERM_DSTCMD_PATH_CAPACITY) &&
             winxterm_dstcmd_smoke_write_file_bytes(alpha_path,
                                                    (const uint8_t *)alpha_text,
                                                    sizeof(alpha_text) - 1u) &&
             winxterm_dstcmd_path_append_child(deep_dir,
                                               L"source dir",
                                               source_dir,
                                               WINXTERM_DSTCMD_PATH_CAPACITY) &&
             winxterm_dstcmd_smoke_create_directory(source_dir) &&
             winxterm_dstcmd_path_append_child(source_dir,
                                               L"payload.txt",
                                               payload_path,
                                               WINXTERM_DSTCMD_PATH_CAPACITY) &&
             winxterm_dstcmd_smoke_write_file_bytes(payload_path,
                                                    (const uint8_t *)payload_text,
                                                    sizeof(payload_text) - 1u);
    }

    if (ok) {
        ok = _snwprintf_s(command,
                          command_count,
                          _TRUNCATE,
                          L"cd \"%ls\"",
                          deep_dir) >= 0 &&
             winxterm_dstcmd_shell_submit_line(shell, command) == 0 &&
             winxterm_dstcmd_job_pool_wait_idle(&shell->jobs, 5000u);
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"pwd") == 0 &&
             winxterm_dstcmd_smoke_capture_contains(shell, "winxterm-dstcmd-long");
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"ls") == 0 &&
             winxterm_dstcmd_smoke_capture_contains(shell, "alpha file.txt") &&
             winxterm_dstcmd_smoke_capture_contains(shell, "source dir");
    }
    if (ok) {
        ok = winxterm_dstcmd_smoke_tab_completes_to(shell,
                                                    "cat alpha\\ f",
                                                    "cat alpha\\ file.txt");
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"ls alpha*") == 0 &&
             winxterm_dstcmd_smoke_capture_contains(shell, "alpha file.txt");
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"cat \"alpha file.txt\"") == 0 &&
             winxterm_dstcmd_smoke_capture_equals(shell,
                                                  (const uint8_t *)alpha_text,
                                                  sizeof(alpha_text) - 1u);
    }
    if (ok) {
        ok = winxterm_dstcmd_shell_submit_line(shell, L"cp -R \"source dir\" \"copy dir\"") == 0;
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"cat \"copy dir/payload.txt\"") == 0 &&
             winxterm_dstcmd_smoke_capture_equals(shell,
                                                  (const uint8_t *)payload_text,
                                                  sizeof(payload_text) - 1u);
    }
    if (ok) {
        ok = winxterm_dstcmd_shell_submit_line(shell, L"mv \"copy dir\" \"moved dir\"") == 0;
    }
    if (ok) {
        winxterm_dstcmd_smoke_clear_capture(shell);
        ok = winxterm_dstcmd_shell_submit_line(shell, L"cat \"moved dir/payload.txt\"") == 0 &&
             winxterm_dstcmd_smoke_capture_equals(shell,
                                                  (const uint8_t *)payload_text,
                                                  sizeof(payload_text) - 1u);
    }
    if (ok) {
        ok = winxterm_dstcmd_shell_submit_line(shell,
                                               L"rm -rf \"source dir\" \"moved dir\" \"alpha file.txt\"") == 0 &&
             winxterm_dstcmd_job_pool_wait_idle(&shell->jobs, 5000u);
    }
    if (ok) {
        ok = winxterm_dstcmd_path_append_child(deep_dir,
                                               L"trailing dir",
                                               source_dir,
                                               WINXTERM_DSTCMD_PATH_CAPACITY) &&
             winxterm_dstcmd_smoke_create_directory(source_dir) &&
             winxterm_dstcmd_path_append_child(source_dir,
                                               L"payload.txt",
                                               payload_path,
                                               WINXTERM_DSTCMD_PATH_CAPACITY) &&
             winxterm_dstcmd_smoke_write_file_bytes(payload_path,
                                                    (const uint8_t *)payload_text,
                                                    sizeof(payload_text) - 1u);
    }
    if (ok) {
        ok = winxterm_dstcmd_shell_submit_line(shell, L"rm -rf \"trailing dir/\"") == 0 &&
             winxterm_dstcmd_job_pool_wait_idle(&shell->jobs, 5000u);
    }

    (void)winxterm_dstcmd_shell_set_cwd(shell, original_cwd);
    if (base_dir[0] != L'\0') {
        if (_snwprintf_s(command,
                         command_count,
                         _TRUNCATE,
                         L"rm -rf \"%ls\"",
                         base_dir) >= 0) {
            (void)winxterm_dstcmd_shell_submit_line(shell, command);
            (void)winxterm_dstcmd_job_pool_wait_idle(&shell->jobs, 5000u);
        }
    }
    winxterm_dstcmd_scratch_dispose(&scratch);
    return ok;
}

static bool winxterm_dstcmd_smoke_run_highlight(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    if (winxterm_dstcmd_shell_submit_line(shell,
                                          L"echo OneStringVeryLong | "
                                          L"highlight One String OneStringVery OneStringVeryLong") != 0 ||
        !winxterm_dstcmd_smoke_capture_contains(
            shell,
            WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_CYAN
            "One"
            WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_BLUE
            "String"
            WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_GREEN
            "Very"
            WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_YELLOW
            "Long"
            WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_RESET
            "\r\n")) {
        return false;
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    if (winxterm_dstcmd_shell_submit_line(shell,
                                          L"echo OneStringVeryLong | "
                                          L"highlight OneStringVery One String OneStringVeryLong") != 0 ||
        !winxterm_dstcmd_smoke_capture_contains(
            shell,
            WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_CYAN
            "OneStringVery"
            WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_YELLOW
            "Long"
            WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_RESET
            "\r\n")) {
        return false;
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    if (winxterm_dstcmd_shell_submit_line(shell, L"echo one | highlight -i One") != 0 ||
        !winxterm_dstcmd_smoke_capture_contains(shell,
                                                WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_CYAN
                                                "one"
                                                WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_RESET
                                                "\r\n")) {
        return false;
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    if (winxterm_dstcmd_shell_submit_line(shell, L"echo -i | highlight \\-i") != 0 ||
        !winxterm_dstcmd_smoke_capture_contains(shell,
                                                WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_CYAN
                                                "-i"
                                                WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_RESET
                                                "\r\n")) {
        return false;
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    if (winxterm_dstcmd_shell_submit_line(shell, L"highlight \"\"") != 2 ||
        !winxterm_dstcmd_smoke_capture_contains(
            shell,
            "highlight: usage: highlight [-i] STRING...\r\n")) {
        return false;
    }

    wchar_t command[1100];
    if (wcscpy_s(command, sizeof(command) / sizeof(command[0]), L"echo ") != 0) {
        return false;
    }
    size_t offset = wcslen(command);
    for (size_t i = 0u; i < 1024u; ++i) {
        command[offset++] = L'A';
    }
    command[offset] = L'\0';
    if (wcscat_s(command,
                 sizeof(command) / sizeof(command[0]),
                 L"Boundary | highlight ABoundary") != 0) {
        return false;
    }
    winxterm_dstcmd_smoke_clear_capture(shell);
    return winxterm_dstcmd_shell_submit_line(shell, command) == 0 &&
           winxterm_dstcmd_smoke_capture_contains(shell,
                                                  WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_CYAN
                                                  "ABoundary"
                                                  WINXTERM_DSTCMD_SMOKE_HIGHLIGHT_RESET);
}

static bool winxterm_dstcmd_smoke_run_help(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    if (winxterm_dstcmd_shell_submit_line(shell, L"help") != 0) {
        return false;
    }

    static const char *needles[] = {
        "Builtins:\r\n",
        "  alias ADDITIONAL EXISTING\r\n",
        "  cat [FILE|-]...\r\n",
        "  cd [DIR|-]\r\n",
        "  cp [-Rrf] SOURCE... DESTINATION\r\n",
        "  echo [ARG]...\r\n",
        "  export [VARNAME=value]...\r\n",
        "  help [COMMAND]...\r\n",
        "  highlight [-i] STRING...\r\n",
        "  ls [-ltah] [PATH]...\r\n",
        "  mv [-f] SOURCE... DESTINATION\r\n",
        "  playmacro FILENAME | -i MACRO\r\n",
        "  popd\r\n",
        "  pushd DIRECTORY\r\n",
        "  rm [-rf] PATH...\r\n",
        "  set scale <1-100> | timing on|off|verbose | bell on|off | scrollbar on|off | debuglog on|off | env SAVE | CWD save|clear\r\n",
        "  which NAME\r\n",
        "  exit\r\n",
        "Line editor:\r\n",
        "  Enter inside an open quote: insert a newline and continue editing\r\n",
        "  Ctrl+R: search command history; type to filter, Up/Down select, Enter insert, Ctrl+C abort, Ctrl+R recent/best, Alt+R fuzzy/contains\r\n",
        "Redirection:\r\n",
        "  command > FILE: write stdout to FILE, creating or truncating it\r\n",
        "  command >> FILE: append stdout to FILE, creating it if needed\r\n",
        "  command t> FILE or t>> FILE: also tee redirected stdout to terminal stdout\r\n",
    };
    for (size_t i = 0u; i < sizeof(needles) / sizeof(needles[0]); ++i) {
        if (!winxterm_dstcmd_smoke_capture_contains(shell, needles[i])) {
            return false;
        }
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    if (winxterm_dstcmd_shell_submit_line(shell, L"help which") != 0 ||
        !winxterm_dstcmd_smoke_capture_contains(shell, "  which NAME\r\n")) {
        return false;
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    if (winxterm_dstcmd_shell_submit_line(shell, L"help history") != 0 ||
        !winxterm_dstcmd_smoke_capture_contains(shell, "Ctrl+R: search command history")) {
        return false;
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    if (winxterm_dstcmd_shell_submit_line(shell, L"help redirect") != 0 ||
        !winxterm_dstcmd_smoke_capture_contains(shell, "command t> FILE")) {
        return false;
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    return winxterm_dstcmd_shell_submit_line(shell, L"help definitely-not-a-smoke-builtin") == 1 &&
           winxterm_dstcmd_smoke_capture_contains(shell,
                                                  "help: no builtin named 'definitely-not-a-smoke-builtin'\r\n");
}

static bool winxterm_dstcmd_smoke_run_alias(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    if (winxterm_dstcmd_shell_submit_line(shell, L"alias l ls") != 0 ||
        winxterm_dstcmd_shell_submit_line(shell, L"alias list ls") != 0) {
        return false;
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    if (winxterm_dstcmd_shell_submit_line(shell, L"l -la CMakeLists.txt") != 0 ||
        !winxterm_dstcmd_smoke_capture_contains(shell, "CMakeLists.txt")) {
        return false;
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    if (winxterm_dstcmd_shell_submit_line(shell, L"help l") != 0 ||
        !winxterm_dstcmd_smoke_capture_contains(shell, "  l -> [dstbuiltin] ls\r\n")) {
        return false;
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    if (winxterm_dstcmd_shell_submit_line(shell, L"which l") != 0 ||
        !winxterm_dstcmd_smoke_capture_contains(shell, "[dstshell alias]") ||
        !winxterm_dstcmd_smoke_capture_contains(shell, "[dstbuiltin] ls")) {
        return false;
    }

    if (!winxterm_dstcmd_smoke_tab_completes_to(shell, "lis", "list")) {
        return false;
    }

    winxterm_dstcmd_smoke_clear_capture(shell);
    return winxterm_dstcmd_shell_set_title_wide(shell, L"abc") &&
           winxterm_dstcmd_smoke_capture_contains(shell, "\x1b]0;abc\x1b\\");
}

static bool winxterm_dstcmd_smoke_console_mode_matches(HANDLE handle, DWORD expected, bool saved)
{
    if (!saved) {
        return true;
    }
    DWORD mode = 0;
    return winxterm_dstcmd_console_handle_is_valid(handle) &&
           GetConsoleMode(handle, &mode) &&
           mode == expected;
}

static bool winxterm_dstcmd_smoke_console_modes_are_line_editor(WinxtermDstcmdShell *shell)
{
    return shell != 0 &&
           winxterm_dstcmd_smoke_console_mode_matches(shell->input_handle,
                                                      shell->shell_input_console_mode,
                                                      shell->input_console_mode_saved) &&
           winxterm_dstcmd_smoke_console_mode_matches(shell->output_handle,
                                                      shell->shell_output_console_mode,
                                                      shell->output_console_mode_saved) &&
           winxterm_dstcmd_smoke_console_mode_matches(shell->error_handle,
                                                      shell->shell_error_console_mode,
                                                      shell->error_console_mode_saved);
}

static bool winxterm_dstcmd_smoke_set_console_mode(HANDLE handle, DWORD mode, bool saved)
{
    if (!saved) {
        return true;
    }
    return winxterm_dstcmd_console_handle_is_valid(handle) &&
           SetConsoleMode(handle, mode);
}

static bool winxterm_dstcmd_smoke_mutate_console_modes(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    DWORD input_mode = shell->original_input_console_mode |
                       ENABLE_LINE_INPUT |
                       ENABLE_ECHO_INPUT |
                       ENABLE_PROCESSED_INPUT;
    input_mode &= ~ENABLE_VIRTUAL_TERMINAL_INPUT;
    DWORD output_mode = shell->original_output_console_mode & ~ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    DWORD error_mode = shell->original_error_console_mode & ~ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    return winxterm_dstcmd_smoke_set_console_mode(shell->input_handle,
                                                 input_mode,
                                                 shell->input_console_mode_saved) &&
           winxterm_dstcmd_smoke_set_console_mode(shell->output_handle,
                                                 output_mode,
                                                 shell->output_console_mode_saved) &&
           winxterm_dstcmd_smoke_set_console_mode(shell->error_handle,
                                                 error_mode,
                                                 shell->error_console_mode_saved);
}

static bool winxterm_dstcmd_smoke_run_console_mode_reentry(WinxtermDstcmdShell *shell)
{
    if (!winxterm_dstcmd_smoke_mutate_console_modes(shell)) {
        return false;
    }
    winxterm_dstcmd_shell_enter_line_editor_mode(shell);
    if (!winxterm_dstcmd_smoke_console_modes_are_line_editor(shell) ||
        !winxterm_dstcmd_smoke_tab_completes_to(shell, "push", "pushd")) {
        return false;
    }
    winxterm_dstcmd_smoke_clear_capture(shell);
    if (!winxterm_dstcmd_shell_handle_input_byte(shell, 0x0cu) ||
        !winxterm_dstcmd_smoke_capture_contains(shell, "\x1b[H\x1b[2J") ||
        !winxterm_dstcmd_smoke_capture_contains(shell, "$")) {
        return false;
    }
    if (!winxterm_dstcmd_smoke_set_line(shell, "")) {
        return false;
    }
    shell->exit_requested = false;
    if (!winxterm_dstcmd_shell_handle_input_byte(shell, 0x04u) || !shell->exit_requested) {
        return false;
    }
    shell->exit_requested = false;
    return true;
}

static bool winxterm_dstcmd_smoke_mode_mutator_command(wchar_t *out, size_t out_count)
{
    if (out == 0 || out_count == 0u) {
        return false;
    }
    wchar_t module_path[WINXTERM_DSTCMD_PATH_CAPACITY];
    DWORD length = GetModuleFileNameW(0, module_path, WINXTERM_DSTCMD_PATH_CAPACITY);
    if (length == 0u || length >= WINXTERM_DSTCMD_PATH_CAPACITY) {
        return false;
    }
    wchar_t *slash = wcsrchr(module_path, L'\\');
    if (slash == 0) {
        slash = wcsrchr(module_path, L'/');
    }
    if (slash == 0) {
        return false;
    }
    slash[1] = L'\0';
    if (wcscat_s(module_path, WINXTERM_DSTCMD_PATH_CAPACITY, L"dstshell_mode_mutator.exe") != 0 ||
        GetFileAttributesW(module_path) == INVALID_FILE_ATTRIBUTES) {
        return false;
    }
    return _snwprintf_s(out,
                        out_count,
                        _TRUNCATE,
                        L"\"%ls\"",
                        module_path) >= 0;
}

static bool winxterm_dstcmd_smoke_run_external_mode_mutator(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    wchar_t command[WINXTERM_DSTCMD_PATH_CAPACITY + 2u];
    if (!winxterm_dstcmd_smoke_mode_mutator_command(command,
                                                    sizeof(command) / sizeof(command[0]))) {
        return false;
    }
    return winxterm_dstcmd_shell_submit_line(shell, command) == 0 &&
           winxterm_dstcmd_smoke_console_modes_are_line_editor(shell) &&
           winxterm_dstcmd_smoke_tab_completes_to(shell, "push", "pushd");
}

int winxterm_dstcmd_smoke_run(void)
{
    WinxtermDstcmdArgv argv;
    wchar_t error[128];
    if (!winxterm_dstcmd_parse_line(L"echo \"two words\" 'literal' a\\ b", &argv, error, 128u)) {
        return 1;
    }
    bool ok = argv.count == 4 &&
              wcscmp(argv.items[1], L"two words") == 0 &&
              wcscmp(argv.items[2], L"literal") == 0 &&
              wcscmp(argv.items[3], L"a b") == 0;
    winxterm_dstcmd_argv_dispose(&argv);
    if (!ok) {
        return 1;
    }
    if (!winxterm_dstcmd_parse_line(L"playmacro -i \"typestring one\nwaitms 1\"",
                                    &argv,
                                    error,
                                    128u)) {
        return 1;
    }
    ok = argv.count == 3 &&
         wcscmp(argv.items[1], L"-i") == 0 &&
         wcscmp(argv.items[2], L"typestring one\nwaitms 1") == 0;
    winxterm_dstcmd_argv_dispose(&argv);
    if (!ok) {
        return 1;
    }
    SetEnvironmentVariableW(L"WINXTERM_DSTCMD_SMOKE_VAR", L"expanded");
    if (!winxterm_dstcmd_parse_line(L"echo $WINXTERM_DSTCMD_SMOKE_VAR", &argv, error, 128u)) {
        return 1;
    }
    ok = argv.count == 2 && wcscmp(argv.items[1], L"expanded") == 0;
    winxterm_dstcmd_argv_dispose(&argv);
    if (!ok) {
        return 1;
    }

    WinxtermDstcmdShell *shell = (WinxtermDstcmdShell *)calloc(1u, sizeof(*shell));
    if (shell == 0) {
        return 1;
    }
    if (!winxterm_dstcmd_shell_init(shell)) {
        free(shell);
        return 1;
    }
    wchar_t smoke_cwd[WINXTERM_DSTCMD_PATH_CAPACITY];
    DWORD smoke_cwd_length = GetCurrentDirectoryW(WINXTERM_DSTCMD_PATH_CAPACITY, smoke_cwd);
    if (smoke_cwd_length == 0u || smoke_cwd_length >= WINXTERM_DSTCMD_PATH_CAPACITY ||
        !winxterm_dstcmd_shell_set_cwd(shell, smoke_cwd)) {
        winxterm_dstcmd_shell_dispose(shell);
        free(shell);
        return 1;
    }
    shell->capture_active = true;
    ok = winxterm_dstcmd_smoke_run_line_editing(shell) &&
         winxterm_dstcmd_smoke_run_history_search(shell) &&
         winxterm_dstcmd_smoke_run_completion(shell) &&
         winxterm_dstcmd_smoke_run_which(shell) &&
         winxterm_dstcmd_smoke_run_cat(shell) &&
         winxterm_dstcmd_smoke_run_long_paths(shell) &&
         winxterm_dstcmd_smoke_run_highlight(shell) &&
         winxterm_dstcmd_smoke_run_help(shell) &&
         winxterm_dstcmd_smoke_run_alias(shell) &&
         winxterm_dstcmd_smoke_run_console_mode_reentry(shell) &&
         winxterm_dstcmd_smoke_run_external_mode_mutator(shell);
    static const wchar_t *commands[] = {
        L"echo ok",
        L"ls",
        L"ls CMakeLists.txt",
        L"ls -la",
        L"ls *",
        L"cd ..",
        L"echo ok | ls",
    };
    if (ok) {
        for (size_t i = 0u; i < sizeof(commands) / sizeof(commands[0]); ++i) {
            if (winxterm_dstcmd_shell_submit_line(shell, commands[i]) != 0) {
                ok = false;
                break;
            }
        }
    }
    shell->capture_active = false;
    winxterm_dstcmd_shell_dispose(shell);
    free(shell);
    return ok ? 0 : 1;
}
