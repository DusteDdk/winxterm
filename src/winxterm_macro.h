#ifndef WINXTERM_MACRO_H
#define WINXTERM_MACRO_H

#include "winxterm_input.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <windows.h>

#define WINXTERM_MACRO_DEFAULT_TYPE_DELAY_MS 20u
#define WINXTERM_MACRO_DONE_DELAY INFINITE

typedef enum WinxtermMacroCommandKind {
    WINXTERM_MACRO_COMMAND_SET_TYPE_DELAY = 0,
    WINXTERM_MACRO_COMMAND_TYPE_STRING,
    WINXTERM_MACRO_COMMAND_ENTER_STRING,
    WINXTERM_MACRO_COMMAND_KEY_DOWN,
    WINXTERM_MACRO_COMMAND_KEY_UP,
    WINXTERM_MACRO_COMMAND_KEY_PRESS,
    WINXTERM_MACRO_COMMAND_WAIT,
    WINXTERM_MACRO_COMMAND_SCREENSHOT,
    WINXTERM_MACRO_COMMAND_SCREEN_DUMP,
    WINXTERM_MACRO_COMMAND_CELL_DUMP,
    WINXTERM_MACRO_COMMAND_HIST_DUMP,
    WINXTERM_MACRO_COMMAND_WAIT_REDRAW,
    WINXTERM_MACRO_COMMAND_WAIT_HOST,
    WINXTERM_MACRO_COMMAND_EXIT,
    WINXTERM_MACRO_COMMAND_MAXIMIZE,
    WINXTERM_MACRO_COMMAND_MINIMIZE,
    WINXTERM_MACRO_COMMAND_RESTORE
} WinxtermMacroCommandKind;

typedef struct WinxtermMacroCommand {
    WinxtermMacroCommandKind kind;
    wchar_t *text;
    WPARAM virtual_key;
    DWORD first_ms;
    DWORD second_ms;
    unsigned int line;
} WinxtermMacroCommand;

typedef struct WinxtermMacro WinxtermMacro;

typedef struct WinxtermMacroCallbacks {
    void *context;
    bool (*queue_char)(void *context, wchar_t ch, WinxtermInputModifiers modifiers);
    bool (*key_down)(void *context, WPARAM virtual_key, WinxtermInputModifiers modifiers);
    bool (*key_up)(void *context, WPARAM virtual_key, WinxtermInputModifiers modifiers);
    bool (*write_screenshot)(void *context, const wchar_t *path);
    bool (*write_screendump)(void *context, const wchar_t *path);
    bool (*write_celldump)(void *context, const wchar_t *path);
    bool (*write_histdump)(void *context, const wchar_t *path);
    bool (*wait_redraw)(void *context, bool process, bool *ready);
    bool (*wait_host)(void *context, bool *ready);
    bool (*render_barrier)(void *context);
    void (*show_window)(void *context, int show_command);
    void (*request_exit)(void *context);
    void (*log_error)(void *context, const wchar_t *message);
} WinxtermMacroCallbacks;

bool winxterm_macro_create(WinxtermMacro **macro);
void winxterm_macro_destroy(WinxtermMacro *macro);
void winxterm_macro_reset(WinxtermMacro *macro);
bool winxterm_macro_canonicalize_path(const wchar_t *path, wchar_t *out, size_t out_count);
bool winxterm_macro_format_not_found_message(const wchar_t *path, wchar_t *out, size_t out_count);
bool winxterm_macro_load_file(WinxtermMacro *macro,
                              const wchar_t *path,
                              wchar_t *error,
                              size_t error_count);
bool winxterm_macro_parse_text_utf8(WinxtermMacro *macro,
                                    const char *text,
                                    size_t text_length,
                                    wchar_t *error,
                                    size_t error_count);
bool winxterm_macro_running(const WinxtermMacro *macro);
DWORD winxterm_macro_step(WinxtermMacro *macro, const WinxtermMacroCallbacks *callbacks);
size_t winxterm_macro_command_count(const WinxtermMacro *macro);
const WinxtermMacroCommand *winxterm_macro_command_at(const WinxtermMacro *macro, size_t index);
bool winxterm_macro_key_name_to_virtual_key(const wchar_t *name, WPARAM *virtual_key);

#endif
