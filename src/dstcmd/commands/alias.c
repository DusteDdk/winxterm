#include "dstcmd/commands/alias.h"

#include "dstcmd/dispatch.h"
#include "dstcmd/winxterm_dstcmd_exec.h"

#include <stdio.h>
#include <wchar.h>

static const wchar_t WINXTERM_DSTCMD_ALIAS_USAGE[] = L"usage: alias ADDITIONAL EXISTING\r\n";

static bool winxterm_dstcmd_alias_name_valid(const wchar_t *name)
{
    if (name == 0 || name[0] == L'\0') {
        return false;
    }
    for (const wchar_t *p = name; *p != L'\0'; ++p) {
        if (*p == L'/' || *p == L'\\' || *p == L':') {
            return false;
        }
    }
    return true;
}

static bool winxterm_dstcmd_alias_format_builtin(const wchar_t *target,
                                                 wchar_t *out,
                                                 size_t out_count)
{
    return _snwprintf_s(out, out_count, _TRUNCATE, L"[dstbuiltin] %ls", target) >= 0;
}

int winxterm_dstcmd_cmd_alias(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    if (shell == 0 || argv == 0 || argv->count != 3 || argv->items[1] == 0 || argv->items[2] == 0) {
        if (shell != 0) {
            (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_ALIAS_USAGE);
        }
        return 2;
    }

    const wchar_t *name = argv->items[1];
    const wchar_t *existing = argv->items[2];
    if (!winxterm_dstcmd_alias_name_valid(name)) {
        (void)winxterm_dstcmd_shell_write_wide(shell,
                                               L"alias: ADDITIONAL must be a non-path command name\r\n");
        return 2;
    }
    if (winxterm_dstcmd_find_builtin_entry(name) != 0) {
        (void)winxterm_dstcmd_shell_write_widef(shell,
                                                L"alias: '%ls' is already a dstshell builtin\r\n",
                                                name);
        return 2;
    }

    wchar_t target[WINXTERM_DSTCMD_PATH_CAPACITY];
    wchar_t description[WINXTERM_DSTCMD_PATH_CAPACITY];
    target[0] = L'\0';
    description[0] = L'\0';

    const WinxtermDstcmdCommandEntry *builtin = winxterm_dstcmd_find_builtin_entry(existing);
    if (builtin != 0) {
        if (builtin->run == 0) {
            (void)winxterm_dstcmd_shell_write_widef(shell,
                                                    L"alias: '%ls' is not an executable builtin\r\n",
                                                    existing);
            return 2;
        }
        if (wcscpy_s(target, WINXTERM_DSTCMD_PATH_CAPACITY, existing) != 0 ||
            !winxterm_dstcmd_alias_format_builtin(existing, description, WINXTERM_DSTCMD_PATH_CAPACITY)) {
            (void)winxterm_dstcmd_shell_write_wide(shell, L"alias: target is too long\r\n");
            return 2;
        }
    } else {
        WinxtermDstcmdResolvedExec resolved;
        if (!winxterm_dstcmd_exec_resolve_scratch(&shell->scratch, shell->cwd, existing, &resolved)) {
            (void)winxterm_dstcmd_shell_write_wide(shell, L"alias: command lookup failed\r\n");
            return 1;
        }
        if (resolved.kind == WINXTERM_DSTCMD_EXEC_NOT_FOUND) {
            (void)winxterm_dstcmd_shell_write_widef(shell, L"alias: %ls: command not found\r\n", existing);
            return 127;
        }
        if (resolved.kind == WINXTERM_DSTCMD_EXEC_UNSUPPORTED) {
            (void)winxterm_dstcmd_shell_write_widef(shell,
                                                    L"alias: %ls: unsupported executable or script type\r\n",
                                                    existing);
            return 126;
        }
        if (wcscpy_s(target, WINXTERM_DSTCMD_PATH_CAPACITY, resolved.path) != 0 ||
            wcscpy_s(description, WINXTERM_DSTCMD_PATH_CAPACITY, resolved.path) != 0) {
            (void)winxterm_dstcmd_shell_write_wide(shell, L"alias: resolved target is too long\r\n");
            return 2;
        }
    }

    if (!winxterm_dstcmd_shell_set_alias(shell, name, target, description)) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"alias: out of memory\r\n");
        return 1;
    }
    return 0;
}
