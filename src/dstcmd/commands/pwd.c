#include "dstcmd/commands/pwd.h"

#include <windows.h>

int winxterm_dstcmd_cmd_pwd(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    if (argv != 0 && argv->count > 1) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"pwd: too many arguments\r\n");
        return 2;
    }

    wchar_t cwd[WINXTERM_DSTCMD_PATH_CAPACITY];
    SetLastError(ERROR_SUCCESS);
    DWORD length = GetEnvironmentVariableW(L"CWD", cwd, WINXTERM_DSTCMD_PATH_CAPACITY);
    if (length == 0u) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"pwd: CWD is not set\r\n");
        return 0;
    }
    if (length >= WINXTERM_DSTCMD_PATH_CAPACITY) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"pwd: CWD is too long to print\r\n");
        return 1;
    }

    (void)winxterm_dstcmd_shell_write_widef(shell, L"%ls\r\n", cwd);
    return 0;
}
