#include "winxterm_log.h"
#include "winxterm_client.h"
#include "winxterm_macro.h"
#include "winxterm_options.h"
#include "winxterm_smoke.h"
#include "winxterm_threads.h"

#include <shellapi.h>
#include <wchar.h>
#include <windows.h>

static const wchar_t WINXTERM_HELP_TEXT[] =
    L"Usage: winxterm.exe [options] [--] [command args...]\n\n"
    L"--smoke                   Run built-in self-tests and exit.\n"
    L"--help macro              Show supported macro commands and parameters.\n"
    L"--demo                    Run the rendering demo producer.\n"
    L"--glyphbench              Run headless glyph renderer benchmarks and exit.\n"
    L"--macro <filename>        Play a macro after the window starts.\n"
    L"--rendermethod <method>   Choose spans, row-masks, precolored-cache, or all.\n"
    L"--unpaintedlines <count>  Producer backlog limit before paint, default 20000.\n"
    L"--ncputhreads <count>     Persistent render worker count, default hardware threads.\n"
    L"-x <integer>              Integer display scale, 1 through 100, default 1.\n"
    L"\n"
    L"The first non-option argument is used as the client only when it resolves\n"
    L"to a Windows executable. Without an explicit client, winxterm starts\n"
    L"dstshell.exe from the same directory through the normal ConPTY host.\n";

static const wchar_t WINXTERM_MACRO_HELP_TEXT[] =
    L"Usage: winxterm.exe --macro <filename>\n"
    L"       playmacro FILENAME\n\n"
    L"Macro files are parsed as text lines. A command ends at the first unescaped\n"
    L"semicolon outside double quotes, single quotes, or backtick quotes. A line\n"
    L"ending in backslash continues onto the next physical line.\n\n"
    L"Supported macro commands:\n\n"
    L"set typedelayms INTEGER\n"
    L"    Set the delay between typed characters. Default: 20 ms.\n\n"
    L"typestring STRING\n"
    L"    Type STRING one character at a time through the normal input path.\n\n"
    L"enterstring STRING\n"
    L"    Type STRING one character at a time, then press and release KEY_ENTER.\n"
    L"    The current typedelayms is used as both Enter hold time and wait time.\n\n"
    L"keydown KEY_NAME [waitms]\n"
    L"    Send a key-down event, then wait waitms milliseconds. Default waitms: 0.\n\n"
    L"keyup KEY_NAME [waitms]\n"
    L"    Send a key-up event, then wait waitms milliseconds. Default waitms: 0.\n\n"
    L"keypress KEY_NAME [holdms [waitms]]\n"
    L"    Send key-down, wait holdms, send key-up, then wait waitms.\n"
    L"    Defaults: holdms 0, waitms 0.\n\n"
    L"waitms INTEGER\n"
    L"    Wait without blocking the terminal or hosted client.\n\n"
    L"waitredraw [-w]\n"
    L"    Pause macro playback until queued input, output, and redraw work settles,\n"
    L"    timing out after 2 seconds. With -w, actively pump redraw work while\n"
    L"    waiting; without -w, only observe already-scheduled work.\n\n"
    L"waithost [timeoutms]\n"
    L"    Pause macro playback until an external hosted client is running.\n"
    L"    Default timeout: 5000 ms.\n\n"
    L"screenshot FILENAME\n"
    L"    Save the currently displayed pixels as BMP or PNG, selected by a .bmp\n"
    L"    or .png suffix. A .bmp suffix is appended when FILENAME does not\n"
    L"    already end in .bmp or .png.\n\n"
    L"screendump FILENAME.txt\n"
    L"    Save the active terminal grid as UTF-8 text.\n\n"
    L"celldump FILENAME.txt\n"
    L"    Save every visible character cell as a fixed rectangular UTF-8 grid,\n"
    L"    preserving leading/trailing spaces and blank rows.\n\n"
    L"histdump FILENAME.log\n"
    L"    Save the raw output transcript ring, preserving control and escape bytes.\n\n"
    L"maximize\n"
    L"    Maximize the window and continue after resize/render.\n\n"
    L"minimize\n"
    L"    Minimize the window.\n\n"
    L"restore\n"
    L"    Restore the window and continue after resize/render.\n\n"
    L"exit\n"
    L"    Request normal terminal shutdown.\n\n"
    L"Supported KEY_NAME values:\n"
    L"    KEY_ENTER, KEY_RETURN, KEY_ARROW_UP, KEY_ARROW_DOWN, KEY_ARROW_LEFT,\n"
    L"    KEY_ARROW_RIGHT, KEY_ESCAPE, KEY_BACKSPACE, KEY_DELETE, KEY_LEFT_ALT,\n"
    L"    KEY_RIGHT_ALT, KEY_LEFT_SHIFT, KEY_RIGHT_SHIFT, KEY_LEFT_CTRL,\n"
    L"    KEY_RIGHT_CTRL, KEY_INSERT, KEY_PAGEUP, KEY_PAGEDOWN, KEY_HOME, KEY_END.\n";

static bool winxterm_stdout_is_interactive_console(HANDLE output)
{
    if (output == INVALID_HANDLE_VALUE || output == 0) {
        return false;
    }

    DWORD mode = 0;
    return GetConsoleMode(output, &mode) != 0;
}

static bool winxterm_write_help_to_console(HANDLE output, const wchar_t *text)
{
    if (text == 0 || !winxterm_stdout_is_interactive_console(output)) {
        return false;
    }

    DWORD written = 0;
    return WriteConsoleW(output,
                         text,
                         (DWORD)wcslen(text),
                         &written,
                         0) != 0;
}

static bool winxterm_write_wide_to_handle(HANDLE output, const wchar_t *text)
{
    if (output == INVALID_HANDLE_VALUE || output == 0 || text == 0) {
        return false;
    }
    DWORD mode = 0;
    if (GetConsoleMode(output, &mode)) {
        DWORD written = 0;
        return WriteConsoleW(output, text, (DWORD)wcslen(text), &written, 0) != 0;
    }
    int required = WideCharToMultiByte(CP_UTF8, 0, text, -1, 0, 0, 0, 0);
    if (required <= 0) {
        return false;
    }
    char *utf8 = (char *)LocalAlloc(LMEM_FIXED, (SIZE_T)required);
    if (utf8 == 0) {
        return false;
    }
    bool ok = false;
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, required, 0, 0) > 0) {
        DWORD written = 0;
        ok = WriteFile(output, utf8, (DWORD)(required - 1), &written, 0) != 0;
    }
    LocalFree(utf8);
    return ok;
}

static void winxterm_write_stderr_line(const wchar_t *text)
{
    if (text == 0) {
        return;
    }
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    if (winxterm_write_wide_to_handle(error, text) &&
        winxterm_write_wide_to_handle(error, L"\r\n")) {
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        error = GetStdHandle(STD_ERROR_HANDLE);
        (void)winxterm_write_wide_to_handle(error, text);
        (void)winxterm_write_wide_to_handle(error, L"\r\n");
        FreeConsole();
    }
}

static const wchar_t *winxterm_help_text_for_topic(const wchar_t *topic)
{
    if (topic != 0 && _wcsicmp(topic, L"macro") == 0) {
        return WINXTERM_MACRO_HELP_TEXT;
    }
    return WINXTERM_HELP_TEXT;
}

static void winxterm_show_help(const wchar_t *topic)
{
    const wchar_t *text = winxterm_help_text_for_topic(topic);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (winxterm_write_help_to_console(output, text)) {
        return;
    }
    if (winxterm_write_wide_to_handle(output, text)) {
        return;
    }

    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        output = GetStdHandle(STD_OUTPUT_HANDLE);
        if (winxterm_write_help_to_console(output, text) ||
            winxterm_write_wide_to_handle(output, text)) {
            FreeConsole();
            return;
        }
        FreeConsole();
    }

    MessageBoxW(0, text, L"XTerm for Windows", MB_OK | MB_ICONINFORMATION);
}

int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE previous_instance, PWSTR command_line, int show_command)
{
    (void)previous_instance;
    (void)command_line;
    (void)show_command;

    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv == 0) {
        return 2;
    }

    WinxtermOptions options;
    int parse_result = winxterm_options_parse(argc, (const wchar_t * const *)argv, &options);
    if (parse_result != 0) {
        MessageBoxW(0,
                    L"Unknown command line option.\n\nUse --help for supported options.",
                    L"XTerm for Windows",
                    MB_OK | MB_ICONERROR);
        LocalFree(argv);
        return 2;
    }

    if (options.help) {
        winxterm_show_help(options.help_topic);
        LocalFree(argv);
        return 0;
    }

    if (options.smoke) {
        int smoke_result = winxterm_smoke_run();
        LocalFree(argv);
        return smoke_result;
    }

    wchar_t startup_notice[WINXTERM_LOG_PATH_CAPACITY + 64u];
    startup_notice[0] = L'\0';
    if (options.macro_path != 0) {
        DWORD attributes = GetFileAttributesW(options.macro_path);
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
            if (winxterm_macro_format_not_found_message(options.macro_path,
                                                        startup_notice,
                                                        sizeof(startup_notice) / sizeof(startup_notice[0]))) {
                options.startup_notice = startup_notice;
                winxterm_write_stderr_line(startup_notice);
            }
            options.macro_path = 0;
        }
    }

    WinxtermLog log;
    if (!winxterm_log_init(&log)) {
        LocalFree(argv);
        return 1;
    }

    winxterm_log_writef(&log, "startup");

    if (options.glyphbench) {
        int glyphbench_result = winxterm_glyphbench_run(&log,
                                                        options.render_backend,
                                                        options.render_backend_set &&
                                                            !options.render_backend_all);
        winxterm_log_writef(&log, "glyphbench shutdown exit_code=%d", glyphbench_result);
        winxterm_log_dispose(&log);
        LocalFree(argv);
        return glyphbench_result;
    }

    int exit_code = winxterm_threads_run(instance, &log, &options, (const wchar_t * const *)argv);
    winxterm_log_writef(&log, "shutdown exit_code=%d", exit_code);
    winxterm_log_dispose(&log);
    LocalFree(argv);
    return exit_code;
}
