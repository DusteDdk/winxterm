#ifndef WINXTERM_DSTCMD_EXEC_H
#define WINXTERM_DSTCMD_EXEC_H

#include "dstcmd/winxterm_dstcmd.h"
#include "dstcmd/winxterm_dstcmd_parse.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

typedef enum WinxtermDstcmdExecKind {
    WINXTERM_DSTCMD_EXEC_NOT_FOUND = 0,
    WINXTERM_DSTCMD_EXEC_NATIVE,
    WINXTERM_DSTCMD_EXEC_POWERSHELL,
    WINXTERM_DSTCMD_EXEC_CMD,
    WINXTERM_DSTCMD_EXEC_BASH,
    WINXTERM_DSTCMD_EXEC_UNSUPPORTED
} WinxtermDstcmdExecKind;

typedef struct WinxtermDstcmdResolvedExec {
    WinxtermDstcmdExecKind kind;
    wchar_t path[WINXTERM_DSTCMD_PATH_CAPACITY];
    wchar_t interpreter[WINXTERM_DSTCMD_PATH_CAPACITY];
} WinxtermDstcmdResolvedExec;

typedef bool (*WinxtermDstcmdExecPathMatchVisitor)(const WinxtermDstcmdResolvedExec *resolved,
                                                   void *context);

typedef enum WinxtermDstcmdStreamEndpointKind {
    WINXTERM_DSTCMD_STREAM_TERMINAL = 0,
    WINXTERM_DSTCMD_STREAM_PIPE,
    WINXTERM_DSTCMD_STREAM_REDIRECT
} WinxtermDstcmdStreamEndpointKind;

typedef struct WinxtermDstcmdStreamEndpoint {
    WinxtermDstcmdStreamEndpointKind kind;
    const wchar_t *path;
    bool append;
    bool tee_to_terminal;
} WinxtermDstcmdStreamEndpoint;

typedef struct WinxtermDstcmdExecStage {
    const WinxtermDstcmdArgv *argv;
    WinxtermDstcmdStreamEndpoint stdin_endpoint;
    WinxtermDstcmdStreamEndpoint stdout_endpoint;
    WinxtermDstcmdStreamEndpoint stderr_endpoint;
    bool isolate_shell_state;
} WinxtermDstcmdExecStage;

bool winxterm_dstcmd_exec_resolve(const wchar_t *cwd,
                                  const wchar_t *command,
                                  WinxtermDstcmdResolvedExec *resolved);
bool winxterm_dstcmd_exec_resolve_scratch(WinxtermDstcmdScratch *scratch,
                                          const wchar_t *cwd,
                                          const wchar_t *command,
                                          WinxtermDstcmdResolvedExec *resolved);
bool winxterm_dstcmd_exec_for_each_path_match(const wchar_t *command,
                                              WinxtermDstcmdScratch *scratch,
                                              WinxtermDstcmdExecPathMatchVisitor visitor,
                                              void *context);
bool winxterm_dstcmd_exec_uses_interactive_client(const WinxtermDstcmdExecStage *stages,
                                                 size_t stage_count);
int winxterm_dstcmd_exec_run(WinxtermDstcmdShell *shell,
                             const WinxtermDstcmdExecStage *stages,
                             size_t stage_count);
int winxterm_dstcmd_exec_run_managed_background(WinxtermDstcmdShell *shell,
                                                const WinxtermDstcmdArgv *argv,
                                                bool connectable_stdin,
                                                uint64_t *job_id);
int winxterm_dstcmd_exec_run_managed_foreground(WinxtermDstcmdShell *shell,
                                                const WinxtermDstcmdArgv *argv,
                                                uint64_t *job_id);
int winxterm_dstcmd_exec_run_managed_stages_background(
    WinxtermDstcmdShell *shell, const WinxtermDstcmdExecStage *stages,
    size_t stage_count, bool connectable_stdin, uint64_t *job_id);

#endif
