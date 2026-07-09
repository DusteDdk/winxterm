#include "winxterm_macro.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WINXTERM_MACRO_MAX_FILE_BYTES (16u * 1024u * 1024u)
#define WINXTERM_MACRO_INITIAL_COMMAND_CAPACITY 32u
#define WINXTERM_MACRO_WAIT_REDRAW_TIMEOUT_MS 2000u
#define WINXTERM_MACRO_WAIT_HOST_TIMEOUT_MS 5000u
#define WINXTERM_MACRO_WAIT_REDRAW_POLL_MS 16u

struct WinxtermMacro {
    WinxtermMacroCommand *commands;
    size_t command_count;
    size_t command_capacity;
    size_t command_index;
    size_t text_index;
    unsigned int phase;
    DWORD typedelay_ms;
    DWORD wait_start_tick;
    bool shift_down;
    bool ctrl_down;
    bool alt_down;
    bool running;
};

typedef struct WinxtermMacroLineBuilder {
    char *data;
    size_t length;
    size_t capacity;
} WinxtermMacroLineBuilder;

static void winxterm_macro_format_error(wchar_t *error, size_t error_count, const wchar_t *format, ...)
{
    if (error == 0 || error_count == 0u || format == 0) {
        return;
    }
    va_list args;
    va_start(args, format);
    (void)vswprintf_s(error, error_count, format, args);
    va_end(args);
}

static void winxterm_macro_command_dispose(WinxtermMacroCommand *command)
{
    if (command == 0) {
        return;
    }
    free(command->text);
    memset(command, 0, sizeof(*command));
}

static void winxterm_macro_clear_commands(WinxtermMacro *macro)
{
    if (macro == 0) {
        return;
    }
    for (size_t i = 0u; i < macro->command_count; ++i) {
        winxterm_macro_command_dispose(macro->commands + i);
    }
    macro->command_count = 0u;
    macro->command_index = 0u;
    macro->text_index = 0u;
    macro->phase = 0u;
    macro->typedelay_ms = WINXTERM_MACRO_DEFAULT_TYPE_DELAY_MS;
    macro->wait_start_tick = 0u;
    macro->shift_down = false;
    macro->ctrl_down = false;
    macro->alt_down = false;
    macro->running = false;
}

bool winxterm_macro_create(WinxtermMacro **macro)
{
    if (macro == 0) {
        return false;
    }
    *macro = (WinxtermMacro *)calloc(1u, sizeof(**macro));
    if (*macro == 0) {
        return false;
    }
    (*macro)->typedelay_ms = WINXTERM_MACRO_DEFAULT_TYPE_DELAY_MS;
    return true;
}

void winxterm_macro_destroy(WinxtermMacro *macro)
{
    if (macro == 0) {
        return;
    }
    winxterm_macro_clear_commands(macro);
    free(macro->commands);
    free(macro);
}

void winxterm_macro_reset(WinxtermMacro *macro)
{
    winxterm_macro_clear_commands(macro);
}

bool winxterm_macro_canonicalize_path(const wchar_t *path, wchar_t *out, size_t out_count)
{
    if (path == 0 || path[0] == L'\0' || out == 0 || out_count == 0u) {
        return false;
    }
    DWORD needed = GetFullPathNameW(path, (DWORD)out_count, out, 0);
    if (needed == 0u || needed >= out_count) {
        if (out_count != 0u) {
            out[0] = L'\0';
        }
        return false;
    }
    return true;
}

bool winxterm_macro_format_not_found_message(const wchar_t *path, wchar_t *out, size_t out_count)
{
    if (path == 0 || out == 0 || out_count == 0u) {
        return false;
    }
    wchar_t canonical[MAX_PATH * 8u];
    if (!winxterm_macro_canonicalize_path(path, canonical, sizeof(canonical) / sizeof(canonical[0]))) {
        int fallback = swprintf_s(out, out_count, L"\"Macro: %ls\" was not found.", path);
        return fallback > 0;
    }
    int written = swprintf_s(out, out_count, L"\"Macro: %ls\" was not found.", canonical);
    return written > 0;
}

static bool winxterm_macro_reserve_command(WinxtermMacro *macro)
{
    if (macro == 0) {
        return false;
    }
    if (macro->command_count < macro->command_capacity) {
        return true;
    }
    size_t new_capacity = macro->command_capacity == 0u ?
        WINXTERM_MACRO_INITIAL_COMMAND_CAPACITY : macro->command_capacity * 2u;
    if (new_capacity <= macro->command_capacity) {
        return false;
    }
    WinxtermMacroCommand *new_commands =
        (WinxtermMacroCommand *)realloc(macro->commands, new_capacity * sizeof(*new_commands));
    if (new_commands == 0) {
        return false;
    }
    macro->commands = new_commands;
    macro->command_capacity = new_capacity;
    return true;
}

static wchar_t *winxterm_macro_wide_from_utf8_slice(const char *text, size_t length)
{
    if (text == 0 && length != 0u) {
        return 0;
    }
    if (length > (size_t)INT_MAX) {
        return 0;
    }
    if (length == 0u) {
        wchar_t *empty = (wchar_t *)calloc(1u, sizeof(*empty));
        return empty;
    }

    int required = MultiByteToWideChar(CP_UTF8,
                                       MB_ERR_INVALID_CHARS,
                                       text,
                                       (int)length,
                                       0,
                                       0);
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (required <= 0) {
        code_page = CP_ACP;
        flags = 0;
        required = MultiByteToWideChar(code_page, flags, text, (int)length, 0, 0);
    }
    if (required <= 0) {
        return 0;
    }
    wchar_t *wide = (wchar_t *)calloc((size_t)required + 1u, sizeof(*wide));
    if (wide == 0) {
        return 0;
    }
    int written = MultiByteToWideChar(code_page, flags, text, (int)length, wide, required);
    if (written <= 0) {
        free(wide);
        return 0;
    }
    wide[written] = L'\0';
    return wide;
}

static char *winxterm_macro_copy_slice(const char *text, size_t length)
{
    char *copy = (char *)calloc(length + 1u, sizeof(*copy));
    if (copy == 0) {
        return 0;
    }
    if (length != 0u) {
        memcpy(copy, text, length);
    }
    copy[length] = '\0';
    return copy;
}

static char *winxterm_macro_trim_ascii(char *text)
{
    if (text == 0) {
        return 0;
    }
    while (*text == ' ' || *text == '\t') {
        ++text;
    }
    size_t length = strlen(text);
    while (length != 0u && (text[length - 1u] == ' ' || text[length - 1u] == '\t')) {
        text[--length] = '\0';
    }
    return text;
}

static bool winxterm_macro_parse_uint32(const char *text, DWORD *out)
{
    if (text == 0 || text[0] == '\0' || out == 0) {
        return false;
    }
    char *end = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || *end != '\0' || value > 0xfffffffful) {
        return false;
    }
    *out = (DWORD)value;
    return true;
}

static char *winxterm_macro_next_token(char **cursor)
{
    if (cursor == 0 || *cursor == 0) {
        return 0;
    }
    char *p = *cursor;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    if (*p == '\0') {
        *cursor = p;
        return 0;
    }
    char *start = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') {
        ++p;
    }
    if (*p != '\0') {
        *p++ = '\0';
    }
    *cursor = p;
    return start;
}

static int winxterm_macro_ascii_icmp(const char *left, const char *right)
{
    if (left == 0 || right == 0) {
        return left == right ? 0 : left == 0 ? -1 : 1;
    }
    while (*left != '\0' && *right != '\0') {
        char a = *left;
        char b = *right;
        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }
        if (a != b) {
            return (int)(unsigned char)a - (int)(unsigned char)b;
        }
        ++left;
        ++right;
    }
    return (int)(unsigned char)*left - (int)(unsigned char)*right;
}

bool winxterm_macro_key_name_to_virtual_key(const wchar_t *name, WPARAM *virtual_key)
{
    if (name == 0 || virtual_key == 0) {
        return false;
    }
    typedef struct WinxtermMacroKeyMap {
        const wchar_t *name;
        WPARAM virtual_key;
    } WinxtermMacroKeyMap;
    static const WinxtermMacroKeyMap keys[] = {
        {L"KEY_ENTER", VK_RETURN},
        {L"KEY_RETURN", VK_RETURN},
        {L"KEY_ARROW_UP", VK_UP},
        {L"KEY_ARROW_DOWN", VK_DOWN},
        {L"KEY_ARROW_LEFT", VK_LEFT},
        {L"KEY_ARROW_RIGHT", VK_RIGHT},
        {L"KEY_ESCAPE", VK_ESCAPE},
        {L"KEY_BACKSPACE", VK_BACK},
        {L"KEY_DELETE", VK_DELETE},
        {L"KEY_LEFT_ALT", VK_LMENU},
        {L"KEY_RIGHT_ALT", VK_RMENU},
        {L"KEY_LEFT_SHIFT", VK_LSHIFT},
        {L"KEY_RIGHT_SHIFT", VK_RSHIFT},
        {L"KEY_LEFT_CTRL", VK_LCONTROL},
        {L"KEY_RIGHT_CTRL", VK_RCONTROL},
        {L"KEY_INSERT", VK_INSERT},
        {L"KEY_PAGEUP", VK_PRIOR},
        {L"KEY_PAGEDOWN", VK_NEXT},
        {L"KEY_HOME", VK_HOME},
        {L"KEY_END", VK_END},
    };
    for (size_t i = 0u; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        if (_wcsicmp(name, keys[i].name) == 0) {
            *virtual_key = keys[i].virtual_key;
            return true;
        }
    }
    return false;
}

static bool winxterm_macro_key_name_to_virtual_key_utf8(const char *name, WPARAM *virtual_key)
{
    wchar_t *wide = winxterm_macro_wide_from_utf8_slice(name, name != 0 ? strlen(name) : 0u);
    if (wide == 0) {
        return false;
    }
    bool ok = winxterm_macro_key_name_to_virtual_key(wide, virtual_key);
    free(wide);
    return ok;
}

static bool winxterm_macro_append_command(WinxtermMacro *macro, const WinxtermMacroCommand *command)
{
    if (!winxterm_macro_reserve_command(macro)) {
        return false;
    }
    macro->commands[macro->command_count++] = *command;
    return true;
}

static bool winxterm_macro_parse_string_command(WinxtermMacro *macro,
                                                WinxtermMacroCommandKind kind,
                                                const char *line,
                                                size_t command_length,
                                                size_t prefix_length,
                                                unsigned int line_number,
                                                wchar_t *error,
                                                size_t error_count)
{
    if (command_length < prefix_length) {
        winxterm_macro_format_error(error, error_count, L"line %u: missing string payload", line_number);
        return false;
    }
    wchar_t *payload = winxterm_macro_wide_from_utf8_slice(line + prefix_length,
                                                          command_length - prefix_length);
    if (payload == 0) {
        winxterm_macro_format_error(error, error_count, L"line %u: string conversion failed", line_number);
        return false;
    }
    WinxtermMacroCommand command;
    memset(&command, 0, sizeof(command));
    command.kind = kind;
    command.text = payload;
    command.line = line_number;
    if (!winxterm_macro_append_command(macro, &command)) {
        free(payload);
        winxterm_macro_format_error(error, error_count, L"line %u: out of memory", line_number);
        return false;
    }
    return true;
}

static bool winxterm_macro_parse_path_command(WinxtermMacro *macro,
                                              WinxtermMacroCommandKind kind,
                                              const char *command_text,
                                              const char *command_name,
                                              unsigned int line_number,
                                              wchar_t *error,
                                              size_t error_count)
{
    size_t name_length = strlen(command_name);
    const char *path = command_text + name_length;
    while (*path == ' ' || *path == '\t') {
        ++path;
    }
    size_t path_length = strlen(path);
    while (path_length != 0u && (path[path_length - 1u] == ' ' || path[path_length - 1u] == '\t')) {
        --path_length;
    }
    if (path_length == 0u) {
        winxterm_macro_format_error(error, error_count, L"line %u: missing filename", line_number);
        return false;
    }
    wchar_t *wide = winxterm_macro_wide_from_utf8_slice(path, path_length);
    if (wide == 0) {
        winxterm_macro_format_error(error, error_count, L"line %u: filename conversion failed", line_number);
        return false;
    }
    WinxtermMacroCommand command;
    memset(&command, 0, sizeof(command));
    command.kind = kind;
    command.text = wide;
    command.line = line_number;
    if (!winxterm_macro_append_command(macro, &command)) {
        free(wide);
        winxterm_macro_format_error(error, error_count, L"line %u: out of memory", line_number);
        return false;
    }
    return true;
}

static bool winxterm_macro_parse_command_text(WinxtermMacro *macro,
                                              const char *line,
                                              size_t command_length,
                                              unsigned int line_number,
                                              wchar_t *error,
                                              size_t error_count)
{
    static const char typestring_prefix[] = "typestring ";
    static const char enterstring_prefix[] = "enterstring ";
    if (command_length == 0u) {
        return true;
    }
    if (command_length >= sizeof(typestring_prefix) - 1u &&
        memcmp(line, typestring_prefix, sizeof(typestring_prefix) - 1u) == 0) {
        return winxterm_macro_parse_string_command(macro,
                                                   WINXTERM_MACRO_COMMAND_TYPE_STRING,
                                                   line,
                                                   command_length,
                                                   sizeof(typestring_prefix) - 1u,
                                                   line_number,
                                                   error,
                                                   error_count);
    }
    if (command_length >= sizeof(enterstring_prefix) - 1u &&
        memcmp(line, enterstring_prefix, sizeof(enterstring_prefix) - 1u) == 0) {
        return winxterm_macro_parse_string_command(macro,
                                                   WINXTERM_MACRO_COMMAND_ENTER_STRING,
                                                   line,
                                                   command_length,
                                                   sizeof(enterstring_prefix) - 1u,
                                                   line_number,
                                                   error,
                                                   error_count);
    }

    char *copy = winxterm_macro_copy_slice(line, command_length);
    if (copy == 0) {
        winxterm_macro_format_error(error, error_count, L"line %u: out of memory", line_number);
        return false;
    }
    char *trimmed = winxterm_macro_trim_ascii(copy);
    if (trimmed[0] == '\0') {
        free(copy);
        return true;
    }

    WinxtermMacroCommand command;
    memset(&command, 0, sizeof(command));
    command.line = line_number;

    if (winxterm_macro_ascii_icmp(trimmed, "exit") == 0) {
        command.kind = WINXTERM_MACRO_COMMAND_EXIT;
    } else if (winxterm_macro_ascii_icmp(trimmed, "maximize") == 0) {
        command.kind = WINXTERM_MACRO_COMMAND_MAXIMIZE;
    } else if (winxterm_macro_ascii_icmp(trimmed, "minimize") == 0) {
        command.kind = WINXTERM_MACRO_COMMAND_MINIMIZE;
    } else if (winxterm_macro_ascii_icmp(trimmed, "restore") == 0) {
        command.kind = WINXTERM_MACRO_COMMAND_RESTORE;
    } else if (strncmp(trimmed, "screenshot", 10u) == 0 &&
               (trimmed[10] == ' ' || trimmed[10] == '\t')) {
        bool ok = winxterm_macro_parse_path_command(macro,
                                                    WINXTERM_MACRO_COMMAND_SCREENSHOT,
                                                    trimmed,
                                                    "screenshot",
                                                    line_number,
                                                    error,
                                                    error_count);
        free(copy);
        return ok;
    } else if (strncmp(trimmed, "screendump", 10u) == 0 &&
               (trimmed[10] == ' ' || trimmed[10] == '\t')) {
        bool ok = winxterm_macro_parse_path_command(macro,
                                                    WINXTERM_MACRO_COMMAND_SCREEN_DUMP,
                                                    trimmed,
                                                    "screendump",
                                                    line_number,
                                                    error,
                                                    error_count);
        free(copy);
        return ok;
    } else if (strncmp(trimmed, "histdump", 8u) == 0 &&
               (trimmed[8] == ' ' || trimmed[8] == '\t')) {
        bool ok = winxterm_macro_parse_path_command(macro,
                                                    WINXTERM_MACRO_COMMAND_HIST_DUMP,
                                                    trimmed,
                                                    "histdump",
                                                    line_number,
                                                    error,
                                                    error_count);
        free(copy);
        return ok;
    } else {
        char *cursor = trimmed;
        char *token0 = winxterm_macro_next_token(&cursor);
        if (token0 != 0 && winxterm_macro_ascii_icmp(token0, "set") == 0) {
            char *token1 = winxterm_macro_next_token(&cursor);
            char *token2 = winxterm_macro_next_token(&cursor);
            char *extra = winxterm_macro_next_token(&cursor);
            DWORD value = 0u;
            if (token1 == 0 || token2 == 0 || extra != 0 ||
                winxterm_macro_ascii_icmp(token1, "typedelayms") != 0 ||
                !winxterm_macro_parse_uint32(token2, &value)) {
                winxterm_macro_format_error(error, error_count, L"line %u: invalid set typedelayms", line_number);
                free(copy);
                return false;
            }
            command.kind = WINXTERM_MACRO_COMMAND_SET_TYPE_DELAY;
            command.first_ms = value;
        } else if (token0 != 0 &&
                   (winxterm_macro_ascii_icmp(token0, "keydown") == 0 ||
                    winxterm_macro_ascii_icmp(token0, "keyup") == 0 ||
                    winxterm_macro_ascii_icmp(token0, "keypress") == 0)) {
            char *key_name = winxterm_macro_next_token(&cursor);
            if (key_name == 0 || !winxterm_macro_key_name_to_virtual_key_utf8(key_name, &command.virtual_key)) {
                winxterm_macro_format_error(error, error_count, L"line %u: unknown key name", line_number);
                free(copy);
                return false;
            }
            char *token1 = winxterm_macro_next_token(&cursor);
            char *token2 = winxterm_macro_next_token(&cursor);
            char *extra = winxterm_macro_next_token(&cursor);
            if (extra != 0) {
                winxterm_macro_format_error(error, error_count, L"line %u: too many key command arguments", line_number);
                free(copy);
                return false;
            }
            if (winxterm_macro_ascii_icmp(token0, "keypress") == 0) {
                command.kind = WINXTERM_MACRO_COMMAND_KEY_PRESS;
                if (token1 != 0 && !winxterm_macro_parse_uint32(token1, &command.first_ms)) {
                    winxterm_macro_format_error(error, error_count, L"line %u: invalid holdms", line_number);
                    free(copy);
                    return false;
                }
                if (token2 != 0 && !winxterm_macro_parse_uint32(token2, &command.second_ms)) {
                    winxterm_macro_format_error(error, error_count, L"line %u: invalid waitms", line_number);
                    free(copy);
                    return false;
                }
            } else {
                command.kind = winxterm_macro_ascii_icmp(token0, "keydown") == 0 ?
                    WINXTERM_MACRO_COMMAND_KEY_DOWN : WINXTERM_MACRO_COMMAND_KEY_UP;
                if (token1 != 0 && !winxterm_macro_parse_uint32(token1, &command.first_ms)) {
                    winxterm_macro_format_error(error, error_count, L"line %u: invalid waitms", line_number);
                    free(copy);
                    return false;
                }
                if (token2 != 0) {
                    winxterm_macro_format_error(error, error_count, L"line %u: too many key command arguments", line_number);
                    free(copy);
                    return false;
                }
            }
        } else if (token0 != 0 && winxterm_macro_ascii_icmp(token0, "waitms") == 0) {
            char *token1 = winxterm_macro_next_token(&cursor);
            char *extra = winxterm_macro_next_token(&cursor);
            if (token1 == 0 || extra != 0 || !winxterm_macro_parse_uint32(token1, &command.first_ms)) {
                winxterm_macro_format_error(error, error_count, L"line %u: invalid waitms", line_number);
                free(copy);
                return false;
            }
            command.kind = WINXTERM_MACRO_COMMAND_WAIT;
        } else if (token0 != 0 && winxterm_macro_ascii_icmp(token0, "waitredraw") == 0) {
            char *token1 = winxterm_macro_next_token(&cursor);
            char *extra = winxterm_macro_next_token(&cursor);
            if (extra != 0 || (token1 != 0 && strcmp(token1, "-w") != 0)) {
                winxterm_macro_format_error(error, error_count, L"line %u: invalid waitredraw", line_number);
                free(copy);
                return false;
            }
            command.kind = WINXTERM_MACRO_COMMAND_WAIT_REDRAW;
            command.first_ms = token1 != 0 ? 1u : 0u;
        } else if (token0 != 0 && winxterm_macro_ascii_icmp(token0, "waithost") == 0) {
            char *token1 = winxterm_macro_next_token(&cursor);
            char *extra = winxterm_macro_next_token(&cursor);
            DWORD timeout_ms = WINXTERM_MACRO_WAIT_HOST_TIMEOUT_MS;
            if (extra != 0 || (token1 != 0 && !winxterm_macro_parse_uint32(token1, &timeout_ms))) {
                winxterm_macro_format_error(error, error_count, L"line %u: invalid waithost", line_number);
                free(copy);
                return false;
            }
            command.kind = WINXTERM_MACRO_COMMAND_WAIT_HOST;
            command.first_ms = timeout_ms;
        } else {
            winxterm_macro_format_error(error, error_count, L"line %u: unknown macro command", line_number);
            free(copy);
            return false;
        }
    }

    if (!winxterm_macro_append_command(macro, &command)) {
        winxterm_macro_format_error(error, error_count, L"line %u: out of memory", line_number);
        free(copy);
        return false;
    }
    free(copy);
    return true;
}

static bool winxterm_macro_find_terminator(const char *line,
                                           size_t length,
                                           size_t *command_length)
{
    char quote = '\0';
    bool escaped = false;
    for (size_t i = 0u; i < length; ++i) {
        char ch = line[i];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            continue;
        }
        if (quote != '\0') {
            if (ch == quote) {
                quote = '\0';
            }
            continue;
        }
        if (ch == '"' || ch == '\'' || ch == '`') {
            quote = ch;
            continue;
        }
        if (ch == ';') {
            *command_length = i != 0u && line[i - 1u] == ' ' ? i - 1u : i;
            return true;
        }
    }
    *command_length = length;
    return false;
}

static bool winxterm_macro_parse_logical_line(WinxtermMacro *macro,
                                              const char *line,
                                              size_t length,
                                              unsigned int line_number,
                                              wchar_t *error,
                                              size_t error_count)
{
    size_t command_length = 0u;
    (void)winxterm_macro_find_terminator(line, length, &command_length);
    if (winxterm_macro_parse_command_text(macro, line, command_length, line_number, error, error_count)) {
        return true;
    }
    wchar_t *wide_line = winxterm_macro_wide_from_utf8_slice(line, length);
    if (wide_line != 0) {
        (void)_snwprintf_s(error,
                           error_count,
                           _TRUNCATE,
                           L"Macro error on line %u: %ls",
                           line_number,
                           wide_line);
        free(wide_line);
    } else {
        (void)_snwprintf_s(error,
                           error_count,
                           _TRUNCATE,
                           L"Macro error on line %u: <unprintable line>",
                           line_number);
    }
    return false;
}

static void winxterm_macro_line_builder_dispose(WinxtermMacroLineBuilder *builder)
{
    if (builder == 0) {
        return;
    }
    free(builder->data);
    memset(builder, 0, sizeof(*builder));
}

static bool winxterm_macro_line_builder_append(WinxtermMacroLineBuilder *builder,
                                               const char *text,
                                               size_t length)
{
    if (builder == 0 || (text == 0 && length != 0u)) {
        return false;
    }
    if (length > SIZE_MAX - builder->length - 1u) {
        return false;
    }
    size_t needed = builder->length + length + 1u;
    if (needed > builder->capacity) {
        size_t new_capacity = builder->capacity == 0u ? 256u : builder->capacity;
        while (new_capacity < needed) {
            size_t doubled = new_capacity * 2u;
            if (doubled <= new_capacity) {
                new_capacity = needed;
                break;
            }
            new_capacity = doubled;
        }
        char *new_data = (char *)realloc(builder->data, new_capacity);
        if (new_data == 0) {
            return false;
        }
        builder->data = new_data;
        builder->capacity = new_capacity;
    }
    if (length != 0u) {
        memcpy(builder->data + builder->length, text, length);
    }
    builder->length += length;
    builder->data[builder->length] = '\0';
    return true;
}

bool winxterm_macro_parse_text_utf8(WinxtermMacro *macro,
                                    const char *text,
                                    size_t text_length,
                                    wchar_t *error,
                                    size_t error_count)
{
    if (error != 0 && error_count != 0u) {
        error[0] = L'\0';
    }
    if (macro == 0 || (text == 0 && text_length != 0u)) {
        winxterm_macro_format_error(error, error_count, L"invalid macro parser input");
        return false;
    }

    winxterm_macro_clear_commands(macro);
    WinxtermMacroLineBuilder builder;
    memset(&builder, 0, sizeof(builder));
    size_t offset = 0u;
    unsigned int physical_line = 1u;
    unsigned int logical_line = 1u;
    while (offset < text_length) {
        size_t line_start = offset;
        while (offset < text_length && text[offset] != '\r' && text[offset] != '\n') {
            ++offset;
        }
        size_t line_length = offset - line_start;
        bool has_newline = offset < text_length;
        if (has_newline) {
            char newline = text[offset++];
            if (offset < text_length &&
                ((newline == '\r' && text[offset] == '\n') ||
                 (newline == '\n' && text[offset] == '\r'))) {
                ++offset;
            }
        }

        bool continued = has_newline && line_length != 0u && text[line_start + line_length - 1u] == '\\';
        size_t append_length = continued ? line_length - 1u : line_length;
        if (!winxterm_macro_line_builder_append(&builder, text + line_start, append_length)) {
            winxterm_macro_line_builder_dispose(&builder);
            winxterm_macro_format_error(error, error_count, L"line %u: out of memory", physical_line);
            return false;
        }
        if (!continued) {
            if (!winxterm_macro_parse_logical_line(macro,
                                                   builder.data != 0 ? builder.data : "",
                                                   builder.length,
                                                   logical_line,
                                                   error,
                                                   error_count)) {
                winxterm_macro_line_builder_dispose(&builder);
                return false;
            }
            builder.length = 0u;
            if (builder.data != 0) {
                builder.data[0] = '\0';
            }
            logical_line = physical_line + 1u;
        }
        ++physical_line;
    }
    if (builder.length != 0u) {
        if (!winxterm_macro_parse_logical_line(macro,
                                               builder.data,
                                               builder.length,
                                               logical_line,
                                               error,
                                               error_count)) {
            winxterm_macro_line_builder_dispose(&builder);
            return false;
        }
    }
    winxterm_macro_line_builder_dispose(&builder);
    macro->running = macro->command_count != 0u;
    return true;
}

bool winxterm_macro_load_file(WinxtermMacro *macro,
                              const wchar_t *path,
                              wchar_t *error,
                              size_t error_count)
{
    if (path == 0 || path[0] == L'\0') {
        winxterm_macro_format_error(error, error_count, L"macro filename is empty");
        return false;
    }
    HANDLE file = CreateFileW(path,
                              GENERIC_READ,
                              FILE_SHARE_READ,
                              0,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              0);
    if (file == INVALID_HANDLE_VALUE) {
        DWORD last_error = GetLastError();
        if (last_error == ERROR_FILE_NOT_FOUND || last_error == ERROR_PATH_NOT_FOUND) {
            (void)winxterm_macro_format_not_found_message(path, error, error_count);
        } else {
            winxterm_macro_format_error(error, error_count, L"failed to open macro file");
        }
        return false;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        (unsigned long long)size.QuadPart > WINXTERM_MACRO_MAX_FILE_BYTES) {
        CloseHandle(file);
        winxterm_macro_format_error(error, error_count, L"macro file is too large");
        return false;
    }
    char *bytes = (char *)malloc((size_t)size.QuadPart + 1u);
    if (bytes == 0) {
        CloseHandle(file);
        winxterm_macro_format_error(error, error_count, L"out of memory reading macro file");
        return false;
    }
    DWORD read = 0u;
    bool ok = ReadFile(file, bytes, (DWORD)size.QuadPart, &read, 0) != 0 &&
        read == (DWORD)size.QuadPart;
    CloseHandle(file);
    if (!ok) {
        free(bytes);
        winxterm_macro_format_error(error, error_count, L"failed to read macro file");
        return false;
    }
    bytes[read] = '\0';
    ok = winxterm_macro_parse_text_utf8(macro, bytes, (size_t)read, error, error_count);
    free(bytes);
    return ok;
}

bool winxterm_macro_running(const WinxtermMacro *macro)
{
    return macro != 0 && macro->running;
}

static WinxtermInputModifiers winxterm_macro_modifiers(const WinxtermMacro *macro)
{
    WinxtermInputModifiers modifiers;
    modifiers.shift = macro != 0 && macro->shift_down;
    modifiers.ctrl = macro != 0 && macro->ctrl_down;
    modifiers.alt = macro != 0 && macro->alt_down;
    return modifiers;
}

static bool winxterm_macro_key_is_modifier(WPARAM key)
{
    return key == VK_LSHIFT || key == VK_RSHIFT ||
           key == VK_LCONTROL || key == VK_RCONTROL ||
           key == VK_LMENU || key == VK_RMENU;
}

static void winxterm_macro_set_modifier(WinxtermMacro *macro, WPARAM key, bool down)
{
    if (macro == 0) {
        return;
    }
    switch (key) {
    case VK_LSHIFT:
    case VK_RSHIFT:
        macro->shift_down = down;
        break;
    case VK_LCONTROL:
    case VK_RCONTROL:
        macro->ctrl_down = down;
        break;
    case VK_LMENU:
    case VK_RMENU:
        macro->alt_down = down;
        break;
    default:
        break;
    }
}

static bool winxterm_macro_queue_key_down(WinxtermMacro *macro,
                                          const WinxtermMacroCallbacks *callbacks,
                                          WPARAM key)
{
    if (winxterm_macro_key_is_modifier(key)) {
        winxterm_macro_set_modifier(macro, key, true);
        return callbacks == 0 || callbacks->key_down == 0 ||
            callbacks->key_down(callbacks->context, key, winxterm_macro_modifiers(macro));
    }
    return callbacks != 0 && callbacks->key_down != 0 &&
        callbacks->key_down(callbacks->context, key, winxterm_macro_modifiers(macro));
}

static bool winxterm_macro_queue_key_up(WinxtermMacro *macro,
                                        const WinxtermMacroCallbacks *callbacks,
                                        WPARAM key)
{
    bool ok = callbacks == 0 || callbacks->key_up == 0 ||
        callbacks->key_up(callbacks->context, key, winxterm_macro_modifiers(macro));
    if (winxterm_macro_key_is_modifier(key)) {
        winxterm_macro_set_modifier(macro, key, false);
    }
    return ok;
}

static void winxterm_macro_finish(WinxtermMacro *macro)
{
    if (macro == 0) {
        return;
    }
    macro->running = false;
    macro->command_index = macro->command_count;
    macro->text_index = 0u;
    macro->phase = 0u;
    macro->wait_start_tick = 0u;
    macro->shift_down = false;
    macro->ctrl_down = false;
    macro->alt_down = false;
}

static void winxterm_macro_log_command_failure(const WinxtermMacroCallbacks *callbacks,
                                               const wchar_t *message)
{
    if (callbacks != 0 && callbacks->log_error != 0) {
        callbacks->log_error(callbacks->context, message);
    }
}

DWORD winxterm_macro_step(WinxtermMacro *macro, const WinxtermMacroCallbacks *callbacks)
{
    if (macro == 0 || !macro->running) {
        return WINXTERM_MACRO_DONE_DELAY;
    }

    while (macro->command_index < macro->command_count) {
        WinxtermMacroCommand *command = macro->commands + macro->command_index;
        switch (command->kind) {
        case WINXTERM_MACRO_COMMAND_SET_TYPE_DELAY:
            macro->typedelay_ms = command->first_ms;
            ++macro->command_index;
            continue;
        case WINXTERM_MACRO_COMMAND_TYPE_STRING:
            if (command->text != 0 && command->text[macro->text_index] != L'\0') {
                bool ok = callbacks != 0 && callbacks->queue_char != 0 &&
                    callbacks->queue_char(callbacks->context,
                                          command->text[macro->text_index++],
                                          winxterm_macro_modifiers(macro));
                if (!ok) {
                    winxterm_macro_log_command_failure(callbacks, L"macro typestring input was dropped");
                }
                return macro->typedelay_ms;
            }
            macro->text_index = 0u;
            ++macro->command_index;
            continue;
        case WINXTERM_MACRO_COMMAND_ENTER_STRING:
            if (macro->phase == 0u) {
                if (command->text != 0 && command->text[macro->text_index] != L'\0') {
                    bool ok = callbacks != 0 && callbacks->queue_char != 0 &&
                        callbacks->queue_char(callbacks->context,
                                              command->text[macro->text_index++],
                                              winxterm_macro_modifiers(macro));
                    if (!ok) {
                        winxterm_macro_log_command_failure(callbacks, L"macro enterstring input was dropped");
                    }
                    return macro->typedelay_ms;
                }
                macro->text_index = 0u;
                macro->phase = 1u;
                continue;
            }
            if (macro->phase == 1u) {
                (void)winxterm_macro_queue_key_down(macro, callbacks, VK_RETURN);
                macro->phase = 2u;
                return macro->typedelay_ms;
            }
            (void)winxterm_macro_queue_key_up(macro, callbacks, VK_RETURN);
            macro->phase = 0u;
            ++macro->command_index;
            return macro->typedelay_ms;
        case WINXTERM_MACRO_COMMAND_KEY_DOWN:
            (void)winxterm_macro_queue_key_down(macro, callbacks, command->virtual_key);
            ++macro->command_index;
            return command->first_ms;
        case WINXTERM_MACRO_COMMAND_KEY_UP:
            (void)winxterm_macro_queue_key_up(macro, callbacks, command->virtual_key);
            ++macro->command_index;
            return command->first_ms;
        case WINXTERM_MACRO_COMMAND_KEY_PRESS:
            if (macro->phase == 0u) {
                (void)winxterm_macro_queue_key_down(macro, callbacks, command->virtual_key);
                macro->phase = 1u;
                return command->first_ms;
            }
            (void)winxterm_macro_queue_key_up(macro, callbacks, command->virtual_key);
            macro->phase = 0u;
            ++macro->command_index;
            return command->second_ms;
        case WINXTERM_MACRO_COMMAND_WAIT:
            ++macro->command_index;
            return command->first_ms;
        case WINXTERM_MACRO_COMMAND_WAIT_REDRAW: {
            if (macro->phase == 0u) {
                macro->phase = 1u;
                macro->wait_start_tick = GetTickCount();
            }
            bool ready = false;
            bool ok = callbacks != 0 && callbacks->wait_redraw != 0 &&
                callbacks->wait_redraw(callbacks->context, command->first_ms != 0u, &ready);
            DWORD elapsed = GetTickCount() - macro->wait_start_tick;
            if (!ok) {
                winxterm_macro_log_command_failure(callbacks, L"macro waitredraw failed");
            }
            if (ready || !ok || elapsed >= WINXTERM_MACRO_WAIT_REDRAW_TIMEOUT_MS) {
                macro->phase = 0u;
                macro->wait_start_tick = 0u;
                ++macro->command_index;
                continue;
            }
            return WINXTERM_MACRO_WAIT_REDRAW_POLL_MS;
        }
        case WINXTERM_MACRO_COMMAND_WAIT_HOST: {
            if (macro->phase == 0u) {
                macro->phase = 1u;
                macro->wait_start_tick = GetTickCount();
            }
            bool ready = false;
            bool ok = callbacks != 0 && callbacks->wait_host != 0 &&
                callbacks->wait_host(callbacks->context, &ready);
            DWORD elapsed = GetTickCount() - macro->wait_start_tick;
            if (!ok) {
                winxterm_macro_log_command_failure(callbacks, L"macro waithost failed");
            }
            if (ready || !ok || elapsed >= command->first_ms) {
                macro->phase = 0u;
                macro->wait_start_tick = 0u;
                ++macro->command_index;
                continue;
            }
            return WINXTERM_MACRO_WAIT_REDRAW_POLL_MS;
        }
        case WINXTERM_MACRO_COMMAND_SCREENSHOT:
            if (callbacks == 0 || callbacks->write_screenshot == 0 ||
                !callbacks->write_screenshot(callbacks->context, command->text)) {
                winxterm_macro_log_command_failure(callbacks, L"macro screenshot failed");
            }
            ++macro->command_index;
            continue;
        case WINXTERM_MACRO_COMMAND_SCREEN_DUMP:
            if (callbacks == 0 || callbacks->write_screendump == 0 ||
                !callbacks->write_screendump(callbacks->context, command->text)) {
                winxterm_macro_log_command_failure(callbacks, L"macro screendump failed");
            }
            ++macro->command_index;
            continue;
        case WINXTERM_MACRO_COMMAND_HIST_DUMP:
            if (callbacks == 0 || callbacks->write_histdump == 0 ||
                !callbacks->write_histdump(callbacks->context, command->text)) {
                winxterm_macro_log_command_failure(callbacks, L"macro histdump failed");
            }
            ++macro->command_index;
            continue;
        case WINXTERM_MACRO_COMMAND_EXIT:
            if (callbacks != 0 && callbacks->request_exit != 0) {
                callbacks->request_exit(callbacks->context);
            }
            winxterm_macro_finish(macro);
            return WINXTERM_MACRO_DONE_DELAY;
        case WINXTERM_MACRO_COMMAND_MAXIMIZE:
        case WINXTERM_MACRO_COMMAND_MINIMIZE:
        case WINXTERM_MACRO_COMMAND_RESTORE:
            if (callbacks != 0 && callbacks->show_window != 0) {
                int show_command = command->kind == WINXTERM_MACRO_COMMAND_MAXIMIZE ? SW_MAXIMIZE :
                    command->kind == WINXTERM_MACRO_COMMAND_MINIMIZE ? SW_MINIMIZE : SW_RESTORE;
                callbacks->show_window(callbacks->context, show_command);
            }
            if (callbacks != 0 && callbacks->render_barrier != 0) {
                (void)callbacks->render_barrier(callbacks->context);
            }
            ++macro->command_index;
            continue;
        default:
            ++macro->command_index;
            continue;
        }
    }

    winxterm_macro_finish(macro);
    return WINXTERM_MACRO_DONE_DELAY;
}

size_t winxterm_macro_command_count(const WinxtermMacro *macro)
{
    return macro != 0 ? macro->command_count : 0u;
}

const WinxtermMacroCommand *winxterm_macro_command_at(const WinxtermMacro *macro, size_t index)
{
    if (macro == 0 || index >= macro->command_count) {
        return 0;
    }
    return macro->commands + index;
}
