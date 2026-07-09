#include "dstcmd/commands/export.h"

#include "dstcmd/api/env.h"

#include <windows.h>

static int winxterm_dstcmd_export_list(WinxtermDstcmdShell *shell)
{
    LPWCH environment = GetEnvironmentStringsW();
    if (environment == 0) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"export: cannot read environment\r\n");
        return 1;
    }

    for (const wchar_t *entry = environment; *entry != L'\0'; entry += wcslen(entry) + 1u) {
        (void)winxterm_dstcmd_shell_write_widef(shell, L"%ls\r\n", entry);
    }
    FreeEnvironmentStringsW(environment);
    return 0;
}

int winxterm_dstcmd_cmd_export(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    if (argv == 0 || argv->count <= 1) {
        return winxterm_dstcmd_export_list(shell);
    }

    int status = 0;
    for (int i = 1; i < argv->count; ++i) {
        wchar_t variable[32768];
        WinxtermDstcmdEnvApplyStatus apply_status =
            winxterm_dstcmd_env_apply_assignment(argv->items[i],
                                                 variable,
                                                 sizeof(variable) / sizeof(variable[0]));
        if (apply_status == WINXTERM_DSTCMD_ENV_APPLY_INVALID) {
            (void)winxterm_dstcmd_shell_write_widef(shell, L"export: usage: export VARNAME=value\r\n");
            status = 2;
            continue;
        }
        if (apply_status == WINXTERM_DSTCMD_ENV_APPLY_NAME_TOO_LONG) {
            (void)winxterm_dstcmd_shell_write_widef(shell, L"export: variable name too long\r\n");
            status = 1;
            continue;
        }
        if (apply_status == WINXTERM_DSTCMD_ENV_APPLY_SET_FAILED) {
            (void)winxterm_dstcmd_shell_write_widef(shell, L"export: cannot set %ls\r\n", variable);
            status = 1;
        }
    }
    return status;
}
