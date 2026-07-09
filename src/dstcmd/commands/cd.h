#ifndef WINXTERM_DSTCMD_COMMANDS_CD_H
#define WINXTERM_DSTCMD_COMMANDS_CD_H

#include "dstcmd/api/command.h"

int winxterm_dstcmd_cmd_cd(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv);
int winxterm_dstcmd_cmd_pushd(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv);
int winxterm_dstcmd_cmd_popd(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv);

#endif
