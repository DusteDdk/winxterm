#ifndef WINXTERM_DSTCMD_API_COMMAND_H
#define WINXTERM_DSTCMD_API_COMMAND_H

#include "dstcmd/winxterm_dstcmd.h"
#include "dstcmd/winxterm_dstcmd_parse.h"

typedef int (*WinxtermDstcmdCommandFn)(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv);
typedef bool (*WinxtermDstcmdCommandNameVisitor)(const wchar_t *name, void *context);

typedef struct WinxtermDstcmdCommandEntry {
    const wchar_t *name;
    WinxtermDstcmdCommandFn run;
    const wchar_t *usage_args;
} WinxtermDstcmdCommandEntry;

#endif
