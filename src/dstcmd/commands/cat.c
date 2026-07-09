#include "dstcmd/commands/cat.h"

#include "dstcmd/api/path.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <wchar.h>
#include <windows.h>

#define WINXTERM_DSTCMD_CAT_BUFFER_SIZE (1024u * 1024u)
#define WINXTERM_DSTCMD_CAT_USAGE L"cat: usage: cat [FILE|-]...\r\n"

static bool winxterm_dstcmd_cat_parse_operands(const WinxtermDstcmdArgv *argv,
                                               int *operand_start)
{
    *operand_start = 1;
    if (argv->count <= 1) {
        return true;
    }
    const wchar_t *arg = argv->items[1];
    if (arg == 0 || arg[0] != L'-' || wcscmp(arg, L"-") == 0) {
        return true;
    }
    if (wcscmp(arg, L"--") == 0) {
        *operand_start = 2;
        return true;
    }
    return false;
}

static bool winxterm_dstcmd_cat_write_all(WinxtermDstcmdShell *shell,
                                          const uint8_t *buffer,
                                          DWORD byte_count)
{
    return byte_count == 0u ||
           winxterm_dstcmd_shell_write_bytes(shell, buffer, (size_t)byte_count);
}

static int winxterm_dstcmd_cat_stream_stdin(WinxtermDstcmdShell *shell,
                                            uint8_t *buffer,
                                            size_t buffer_size)
{
    for (;;) {
        size_t read_count = winxterm_dstcmd_shell_read_input(shell, buffer, buffer_size, true);
        if (read_count == 0u) {
            return 0;
        }
        if (!winxterm_dstcmd_shell_write_bytes(shell, buffer, read_count)) {
            (void)winxterm_dstcmd_shell_write_error_wide(shell, L"cat: write error\r\n");
            return 1;
        }
    }
}

static int winxterm_dstcmd_cat_stream_file(WinxtermDstcmdShell *shell,
                                           const wchar_t *operand,
                                           uint8_t *buffer,
                                           size_t buffer_size)
{
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    wchar_t *resolved = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    if (resolved == 0) {
        (void)winxterm_dstcmd_shell_write_error_wide(shell, L"cat: out of memory\r\n");
        winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
        return 1;
    }

    int status = 0;
    if (!winxterm_dstcmd_path_resolve_scratch(&shell->scratch,
                                             shell->cwd,
                                             operand,
                                             resolved,
                                             WINXTERM_DSTCMD_PATH_CAPACITY)) {
        (void)winxterm_dstcmd_shell_write_error_widef(shell,
                                                     L"cat: %ls: path too long\r\n",
                                                     operand);
        status = 1;
        goto cleanup;
    }

    WinxtermDstcmdPathInfo info;
    if (!winxterm_dstcmd_path_get_info_scratch(&shell->scratch, resolved, &info)) {
        (void)winxterm_dstcmd_shell_write_error_widef(shell,
                                                     L"cat: %ls: cannot stat\r\n",
                                                     operand);
        status = 1;
        goto cleanup;
    }
    if ((info.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
        (void)winxterm_dstcmd_shell_write_error_widef(shell,
                                                     L"cat: %ls: is a directory\r\n",
                                                     operand);
        status = 1;
        goto cleanup;
    }

    WinxtermDstcmdWin32Path win32_path;
    if (!winxterm_dstcmd_path_prepare_win32_scratch(&shell->scratch, resolved, &win32_path)) {
        (void)winxterm_dstcmd_shell_write_error_widef(shell,
                                                     L"cat: %ls: path too long\r\n",
                                                     operand);
        status = 1;
        goto cleanup;
    }
    HANDLE file = CreateFileW(win32_path.syscall,
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              0,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
                              0);
    if (file == INVALID_HANDLE_VALUE) {
        (void)winxterm_dstcmd_shell_write_error_widef(shell,
                                                     L"cat: %ls: cannot open\r\n",
                                                     operand);
        status = 1;
        goto cleanup;
    }

    for (;;) {
        DWORD read_count = 0;
        if (!ReadFile(file, buffer, (DWORD)buffer_size, &read_count, 0)) {
            (void)winxterm_dstcmd_shell_write_error_widef(shell,
                                                         L"cat: %ls: read error\r\n",
                                                         operand);
            status = 1;
            break;
        }
        if (read_count == 0u) {
            break;
        }
        if (!winxterm_dstcmd_cat_write_all(shell, buffer, read_count)) {
            (void)winxterm_dstcmd_shell_write_error_wide(shell, L"cat: write error\r\n");
            status = 1;
            break;
        }
    }

    CloseHandle(file);

cleanup:
    winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
    return status;
}

int winxterm_dstcmd_cmd_cat(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    if (shell == 0 || argv == 0) {
        return 1;
    }

    int operand_start = 1;
    if (!winxterm_dstcmd_cat_parse_operands(argv, &operand_start)) {
        (void)winxterm_dstcmd_shell_write_error_wide(shell, WINXTERM_DSTCMD_CAT_USAGE);
        return 2;
    }

    uint8_t *buffer = (uint8_t *)malloc(WINXTERM_DSTCMD_CAT_BUFFER_SIZE);
    if (buffer == 0) {
        (void)winxterm_dstcmd_shell_write_error_wide(shell, L"cat: out of memory\r\n");
        return 1;
    }

    int status = 0;
    if (operand_start >= argv->count) {
        status = winxterm_dstcmd_cat_stream_stdin(shell,
                                                 buffer,
                                                 WINXTERM_DSTCMD_CAT_BUFFER_SIZE);
    } else {
        for (int i = operand_start; i < argv->count; ++i) {
            const wchar_t *operand = argv->items[i];
            int operand_status = wcscmp(operand, L"-") == 0 ?
                winxterm_dstcmd_cat_stream_stdin(shell,
                                                 buffer,
                                                 WINXTERM_DSTCMD_CAT_BUFFER_SIZE) :
                winxterm_dstcmd_cat_stream_file(shell,
                                                operand,
                                                buffer,
                                                WINXTERM_DSTCMD_CAT_BUFFER_SIZE);
            if (operand_status != 0) {
                status = 1;
            }
        }
    }

    free(buffer);
    return status;
}
