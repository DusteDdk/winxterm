#include <windows.h>

static void mutate_input_mode(void)
{
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    if (input == 0 || input == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD mode = 0;
    if (!GetConsoleMode(input, &mode)) {
        return;
    }
    mode |= ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT;
    mode &= ~ENABLE_VIRTUAL_TERMINAL_INPUT;
    (void)SetConsoleMode(input, mode);
}

static void mutate_output_mode(DWORD stream_id)
{
    HANDLE output = GetStdHandle(stream_id);
    if (output == 0 || output == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD mode = 0;
    if (!GetConsoleMode(output, &mode)) {
        return;
    }
    mode &= ~ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    (void)SetConsoleMode(output, mode);
}

int wmain(void)
{
    mutate_input_mode();
    mutate_output_mode(STD_OUTPUT_HANDLE);
    mutate_output_mode(STD_ERROR_HANDLE);
    return 0;
}
