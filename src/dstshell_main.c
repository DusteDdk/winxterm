#include "dstcmd/winxterm_dstcmd.h"

#include <wchar.h>
#include <windows.h>

static void dstshell_configure_console_encoding(void)
{
    (void)SetConsoleCP(CP_UTF8);
    (void)SetConsoleOutputCP(CP_UTF8);
}

static void dstshell_restore_console_encoding(UINT input_code_page, UINT output_code_page)
{
    if (input_code_page != 0u) {
        (void)SetConsoleCP(input_code_page);
    }
    if (output_code_page != 0u) {
        (void)SetConsoleOutputCP(output_code_page);
    }
}

static void dstshell_write_console_line(const wchar_t *text)
{
    HANDLE error = GetStdHandle(STD_ERROR_HANDLE);
    if (error == 0 || error == INVALID_HANDLE_VALUE || text == 0) {
        return;
    }
    DWORD written = 0;
    (void)WriteConsoleW(error, text, (DWORD)wcslen(text), &written, 0);
    (void)WriteConsoleW(error, L"\r\n", 2u, &written, 0);
}

int wmain(int argc, wchar_t **argv)
{
    UINT original_input_code_page = GetConsoleCP();
    UINT original_output_code_page = GetConsoleOutputCP();
    dstshell_configure_console_encoding();

    int exit_code = 0;
    if (argc > 1 && argv != 0 && argv[1] != 0 &&
        (wcscmp(argv[1], L"--help") == 0 || wcscmp(argv[1], L"-h") == 0)) {
        dstshell_write_console_line(L"Usage: dstshell.exe");
        dstshell_write_console_line(L"Starts the default winxterm shell.");
        exit_code = 0;
        dstshell_restore_console_encoding(original_input_code_page, original_output_code_page);
        return exit_code;
    }
    if (argc > 1 && argv != 0 && argv[1] != 0 && wcscmp(argv[1], L"--smoke") == 0) {
        exit_code = winxterm_dstcmd_smoke_run();
        dstshell_restore_console_encoding(original_input_code_page, original_output_code_page);
        return exit_code;
    }
    exit_code = (int)winxterm_dstcmd_run();
    dstshell_restore_console_encoding(original_input_code_page, original_output_code_page);
    return exit_code;
}
