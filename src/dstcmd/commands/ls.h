#ifndef WINXTERM_DSTCMD_COMMANDS_LS_H
#define WINXTERM_DSTCMD_COMMANDS_LS_H

#include "dstcmd/api/command.h"

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

typedef struct WinxtermDstcmdLsOptions {
    bool long_format;
    bool sort_time;
    bool all;
    bool human_readable;
} WinxtermDstcmdLsOptions;

bool winxterm_dstcmd_parse_ls_options(const WinxtermDstcmdArgv *argv,
                                      int *operand_start,
                                      WinxtermDstcmdLsOptions *options,
                                      wchar_t *error,
                                      size_t error_count);
int winxterm_dstcmd_cmd_ls(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv);

#endif
