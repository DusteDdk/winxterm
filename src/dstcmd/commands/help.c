#include "dstcmd/commands/help.h"

#include "dstcmd/dispatch.h"

#include <wchar.h>

typedef struct WinxtermDstcmdHelpContext {
    WinxtermDstcmdShell *shell;
    bool failed;
} WinxtermDstcmdHelpContext;

static bool winxterm_dstcmd_help_write_entry(WinxtermDstcmdShell *shell,
                                             const WinxtermDstcmdCommandEntry *entry)
{
    if (shell == 0 || entry == 0 || entry->name == 0) {
        return false;
    }
    const wchar_t *usage_args = entry->usage_args != 0 ? entry->usage_args : L"";
    if (usage_args[0] == L'\0') {
        return winxterm_dstcmd_shell_write_widef(shell, L"  %ls\r\n", entry->name);
    }
    return winxterm_dstcmd_shell_write_widef(shell, L"  %ls %ls\r\n", entry->name, usage_args);
}

static bool winxterm_dstcmd_help_write_alias(WinxtermDstcmdShell *shell,
                                             const WinxtermDstcmdAlias *alias)
{
    if (shell == 0 || alias == 0 || alias->name == 0 || alias->description == 0) {
        return false;
    }
    return winxterm_dstcmd_shell_write_widef(shell,
                                             L"  %ls -> %ls\r\n",
                                             alias->name,
                                             alias->description);
}

static bool winxterm_dstcmd_help_write_ctrl_r(WinxtermDstcmdShell *shell)
{
    return winxterm_dstcmd_shell_write_wide(
        shell,
        L"  Enter inside an open quote: insert a newline and continue editing\r\n"
        L"  Ctrl+R: search command history; type to filter, Up/Down select, "
        L"Enter insert, Ctrl+C abort, Ctrl+R recent/best, Alt+R fuzzy/contains\r\n");
}

static bool winxterm_dstcmd_help_write_redirect(WinxtermDstcmdShell *shell)
{
    return winxterm_dstcmd_shell_write_wide(
        shell,
        L"  command > FILE: write stdout to FILE, creating or truncating it\r\n"
        L"  command >> FILE: append stdout to FILE, creating it if needed\r\n"
        L"  command t> FILE or t>> FILE: also tee redirected stdout to terminal stdout\r\n");
}

static bool winxterm_dstcmd_help_write_named(WinxtermDstcmdShell *shell, const wchar_t *name)
{
    if (_wcsicmp(name, L"ctrl+r") == 0 || _wcsicmp(name, L"history") == 0) {
        return winxterm_dstcmd_help_write_ctrl_r(shell);
    }
    if (_wcsicmp(name, L"redirect") == 0 || _wcsicmp(name, L"redirection") == 0) {
        return winxterm_dstcmd_help_write_redirect(shell);
    }
    const WinxtermDstcmdAlias *alias = winxterm_dstcmd_shell_find_alias(shell, name);
    if (alias != 0) {
        return winxterm_dstcmd_help_write_alias(shell, alias);
    }
    const WinxtermDstcmdCommandEntry *entry = winxterm_dstcmd_find_builtin_entry(name);
    if (entry == 0) {
        (void)winxterm_dstcmd_shell_write_widef(shell, L"help: no builtin named '%ls'\r\n", name);
        return false;
    }
    return winxterm_dstcmd_help_write_entry(shell, entry);
}

static bool winxterm_dstcmd_help_visit_name(const wchar_t *name, void *context)
{
    WinxtermDstcmdHelpContext *help_context = (WinxtermDstcmdHelpContext *)context;
    if (help_context == 0) {
        return false;
    }
    if (!winxterm_dstcmd_help_write_named(help_context->shell, name)) {
        help_context->failed = true;
        return false;
    }
    return true;
}

int winxterm_dstcmd_cmd_help(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    if (shell == 0 || argv == 0) {
        return 1;
    }

    if (argv->count <= 1) {
        if (!winxterm_dstcmd_shell_write_wide(shell, L"Builtins:\r\n")) {
            return 1;
        }
        WinxtermDstcmdHelpContext context;
        context.shell = shell;
        context.failed = false;
        winxterm_dstcmd_for_each_builtin_name(winxterm_dstcmd_help_visit_name, &context);
        for (size_t i = 0u; i < shell->alias_count && !context.failed; ++i) {
            if (!winxterm_dstcmd_help_write_alias(shell, shell->aliases + i)) {
                context.failed = true;
            }
        }
        if (!context.failed &&
            (!winxterm_dstcmd_shell_write_wide(shell, L"\r\nLine editor:\r\n") ||
             !winxterm_dstcmd_help_write_ctrl_r(shell) ||
             !winxterm_dstcmd_shell_write_wide(shell, L"\r\nRedirection:\r\n") ||
             !winxterm_dstcmd_help_write_redirect(shell))) {
            context.failed = true;
        }
        return context.failed ? 1 : 0;
    }

    int status = 0;
    for (int i = 1; i < argv->count; ++i) {
        if (argv->items[i] == 0 || !winxterm_dstcmd_help_write_named(shell, argv->items[i])) {
            status = 1;
        }
    }
    return status;
}
