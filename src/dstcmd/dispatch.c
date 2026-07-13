#include "dstcmd/dispatch.h"

#include "dstcmd/commands/alias.h"
#include "dstcmd/commands/cat.h"
#include "dstcmd/commands/cd.h"
#include "dstcmd/commands/cp.h"
#include "dstcmd/commands/echo.h"
#include "dstcmd/commands/export.h"
#include "dstcmd/commands/help.h"
#include "dstcmd/commands/highlight.h"
#include "dstcmd/commands/job.h"
#include "dstcmd/commands/ls.h"
#include "dstcmd/commands/mv.h"
#include "dstcmd/commands/playmacro.h"
#include "dstcmd/commands/pwd.h"
#include "dstcmd/commands/rm.h"
#include "dstcmd/commands/set.h"
#include "dstcmd/commands/which.h"

#include <string.h>

static const WinxtermDstcmdCommandEntry winxterm_dstcmd_commands[] = {
    {L"alias", winxterm_dstcmd_cmd_alias, L"ADDITIONAL EXISTING"},
    {L"cat", winxterm_dstcmd_cmd_cat, L"[FILE|-]..."},
    {L"cd", winxterm_dstcmd_cmd_cd, L"[DIR|-]"},
    {L"cp", winxterm_dstcmd_cmd_cp, L"[-Rrf] SOURCE... DESTINATION"},
    {L"echo", winxterm_dstcmd_cmd_echo, L"[ARG]..."},
    {L"export", winxterm_dstcmd_cmd_export, L"[VARNAME=value]..."},
    {L"help", winxterm_dstcmd_cmd_help, L"[COMMAND]..."},
    {L"highlight", winxterm_dstcmd_cmd_highlight, L"[-i] STRING..."},
    {L"job", winxterm_dstcmd_cmd_job, L"[ls]"},
    {L"ls", winxterm_dstcmd_cmd_ls, L"[-ltah] [PATH]..."},
    {L"mv", winxterm_dstcmd_cmd_mv, L"[-f] SOURCE... DESTINATION"},
    {L"playmacro", winxterm_dstcmd_cmd_playmacro, L"FILENAME"},
    {L"popd", winxterm_dstcmd_cmd_popd, L""},
    {L"pushd", winxterm_dstcmd_cmd_pushd, L"DIRECTORY"},
    {L"pwd", winxterm_dstcmd_cmd_pwd, L""},
    {L"rm", winxterm_dstcmd_cmd_rm, L"[-rf] PATH..."},
    {L"set", winxterm_dstcmd_cmd_set, L"scale <1-100> | timing on|off|verbose | bell on|off | debuglog on|off | env SAVE | CWD save|clear"},
    {L"which", winxterm_dstcmd_cmd_which, L"NAME"},
    {L"exit", 0, L""},
};

static const WinxtermDstcmdCommandEntry *winxterm_dstcmd_find_command(const wchar_t *command)
{
    if (command == 0) {
        return 0;
    }
    for (size_t i = 0u; i < sizeof(winxterm_dstcmd_commands) / sizeof(winxterm_dstcmd_commands[0]); ++i) {
        if (wcscmp(winxterm_dstcmd_commands[i].name, command) == 0) {
            return winxterm_dstcmd_commands + i;
        }
    }
    return 0;
}

const WinxtermDstcmdCommandEntry *winxterm_dstcmd_find_builtin_entry(const wchar_t *command)
{
    return winxterm_dstcmd_find_command(command);
}

bool winxterm_dstcmd_is_builtin(const wchar_t *command)
{
    const WinxtermDstcmdCommandEntry *entry = winxterm_dstcmd_find_command(command);
    return entry != 0 && entry->run != 0;
}

int winxterm_dstcmd_dispatch_builtin(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    if (shell == 0 || argv == 0 || argv->count == 0 || argv->items[0] == 0) {
        return 0;
    }
    const WinxtermDstcmdCommandEntry *entry = winxterm_dstcmd_find_command(argv->items[0]);
    if (entry == 0 || entry->run == 0) {
        return 127;
    }
    return entry->run(shell, argv);
}

void winxterm_dstcmd_for_each_builtin_name(WinxtermDstcmdCommandNameVisitor visitor, void *context)
{
    if (visitor == 0) {
        return;
    }
    for (size_t i = 0u; i < sizeof(winxterm_dstcmd_commands) / sizeof(winxterm_dstcmd_commands[0]); ++i) {
        if (!visitor(winxterm_dstcmd_commands[i].name, context)) {
            return;
        }
    }
}
