#ifndef WINXTERM_DSTCMD_API_ENV_H
#define WINXTERM_DSTCMD_API_ENV_H

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

typedef enum WinxtermDstcmdEnvApplyStatus {
    WINXTERM_DSTCMD_ENV_APPLY_OK,
    WINXTERM_DSTCMD_ENV_APPLY_INVALID,
    WINXTERM_DSTCMD_ENV_APPLY_NAME_TOO_LONG,
    WINXTERM_DSTCMD_ENV_APPLY_SET_FAILED,
} WinxtermDstcmdEnvApplyStatus;

typedef enum WinxtermDstcmdEnvSaveStatus {
    WINXTERM_DSTCMD_ENV_SAVE_CREATED,
    WINXTERM_DSTCMD_ENV_SAVE_REPLACED,
    WINXTERM_DSTCMD_ENV_SAVE_FAILED,
} WinxtermDstcmdEnvSaveStatus;

typedef enum WinxtermDstcmdEnvCwdStatus {
    WINXTERM_DSTCMD_ENV_CWD_UPDATED,
    WINXTERM_DSTCMD_ENV_CWD_UNCHANGED,
    WINXTERM_DSTCMD_ENV_CWD_FAILED,
} WinxtermDstcmdEnvCwdStatus;

WinxtermDstcmdEnvApplyStatus winxterm_dstcmd_env_apply_assignment(const wchar_t *assignment,
                                                                  wchar_t *variable,
                                                                  size_t variable_count);
bool winxterm_dstcmd_env_load_file(const wchar_t *path);
WinxtermDstcmdEnvSaveStatus winxterm_dstcmd_env_save_rc(wchar_t *env_path,
                                                        size_t env_path_count,
                                                        wchar_t *backup_path,
                                                        size_t backup_path_count,
                                                        size_t *variable_count,
                                                        wchar_t *error,
                                                        size_t error_count);
WinxtermDstcmdEnvCwdStatus winxterm_dstcmd_env_save_cwd_rc(const wchar_t *cwd,
                                                           wchar_t *env_path,
                                                           size_t env_path_count,
                                                           wchar_t *error,
                                                           size_t error_count);
WinxtermDstcmdEnvCwdStatus winxterm_dstcmd_env_clear_cwd_rc(wchar_t *env_path,
                                                            size_t env_path_count,
                                                            wchar_t *error,
                                                            size_t error_count);

#endif
