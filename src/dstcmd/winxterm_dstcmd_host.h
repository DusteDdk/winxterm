#ifndef WINXTERM_DSTCMD_HOST_H
#define WINXTERM_DSTCMD_HOST_H

#include "dstcmd/winxterm_dstcmd.h"

#include <stdbool.h>

int winxterm_dstcmd_host_query(WinxtermDstcmdShell *shell);
int winxterm_dstcmd_host_set_scale(WinxtermDstcmdShell *shell, unsigned int scale);
int winxterm_dstcmd_host_set_bell(WinxtermDstcmdShell *shell, bool enabled);
int winxterm_dstcmd_host_set_scrollbar(WinxtermDstcmdShell *shell, bool enabled);
int winxterm_dstcmd_host_set_debuglog(WinxtermDstcmdShell *shell, bool enabled);
int winxterm_dstcmd_host_playmacro(WinxtermDstcmdShell *shell, const wchar_t *path);
int winxterm_dstcmd_host_playmacro_text(WinxtermDstcmdShell *shell, const wchar_t *text);

#endif
