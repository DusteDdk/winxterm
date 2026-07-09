#ifndef WINXTERM_DSTCMD_DISPATCH_H
#define WINXTERM_DSTCMD_DISPATCH_H

#include "dstcmd/api/command.h"

const WinxtermDstcmdCommandEntry *winxterm_dstcmd_find_builtin_entry(const wchar_t *command);
bool winxterm_dstcmd_is_builtin(const wchar_t *command);
int winxterm_dstcmd_dispatch_builtin(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv);
void winxterm_dstcmd_for_each_builtin_name(WinxtermDstcmdCommandNameVisitor visitor, void *context);

#endif
