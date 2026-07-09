#include "dstcmd/commands/echo.h"

int winxterm_dstcmd_cmd_echo(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    if (argv == 0) {
        return 0;
    }
    for (int i = 1; i < argv->count; ++i) {
        if (i != 1) {
            (void)winxterm_dstcmd_shell_write_wide(shell, L" ");
        }
        (void)winxterm_dstcmd_shell_write_wide(shell, argv->items[i]);
    }
    (void)winxterm_dstcmd_shell_write_wide(shell, L"\r\n");
    return 0;
}
