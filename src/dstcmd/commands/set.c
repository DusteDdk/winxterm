#include "dstcmd/commands/set.h"

#include "dstcmd/api/env.h"
#include "dstcmd/api/path.h"
#include "dstcmd/winxterm_dstcmd_host.h"
#include "winxterm_scale.h"

#include <wchar.h>
#include <windows.h>

static const wchar_t WINXTERM_DSTCMD_SET_SCALE_ERROR[] = L"must be an integer >= 1 <=100\r\n";
static const wchar_t WINXTERM_DSTCMD_SET_LOG_ERROR[] =
    L"set debuglog: failed to create %USERPROFILE%\\.winxterm\\logs debug log\r\n";
static const wchar_t WINXTERM_DSTCMD_SET_USAGE[] =
    L"usage: set scale <1-100> | set timing on|off|verbose | set bell on|off | set scrollbar on|off | set debuglog on|off | set env SAVE | set CWD save|clear\r\n";

static const wchar_t *winxterm_dstcmd_set_username(wchar_t *username, size_t username_count)
{
    if (username == 0 || username_count == 0u) {
        return L"user";
    }
    DWORD length = GetEnvironmentVariableW(L"USERNAME", username, (DWORD)username_count);
    if (length != 0u && length < username_count) {
        return username;
    }
    length = GetEnvironmentVariableW(L"USER", username, (DWORD)username_count);
    if (length != 0u && length < username_count) {
        return username;
    }
    return L"user";
}

static int winxterm_dstcmd_set_cwd_save(WinxtermDstcmdShell *shell)
{
    wchar_t cwd[WINXTERM_DSTCMD_PATH_CAPACITY];
    SetLastError(ERROR_SUCCESS);
    DWORD length = GetEnvironmentVariableW(L"CWD", cwd, WINXTERM_DSTCMD_PATH_CAPACITY);
    if (length == 0u) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"set CWD save: CWD is not set\r\n");
        return 0;
    }
    if (length >= WINXTERM_DSTCMD_PATH_CAPACITY) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"set CWD save: CWD is too long to save\r\n");
        return 0;
    }
    if (!winxterm_dstcmd_path_is_directory(cwd)) {
        (void)winxterm_dstcmd_shell_write_widef(
            shell,
            L"set CWD save: CWD points to a nonexisting directory %ls\r\n",
            cwd);
        return 0;
    }

    wchar_t env_path[WINXTERM_DSTCMD_PATH_CAPACITY];
    wchar_t error[1024];
    WinxtermDstcmdEnvCwdStatus status = winxterm_dstcmd_env_save_cwd_rc(cwd,
                                                                        env_path,
                                                                        sizeof(env_path) /
                                                                            sizeof(env_path[0]),
                                                                        error,
                                                                        sizeof(error) /
                                                                            sizeof(error[0]));
    if (status == WINXTERM_DSTCMD_ENV_CWD_FAILED) {
        (void)winxterm_dstcmd_shell_write_widef(shell,
                                                L"set CWD save: could not update %ls\r\n"
                                                L"Error: %ls\r\n",
                                                env_path[0] != L'\0' ? env_path : L"env.rc",
                                                error[0] != L'\0' ? error : L"unknown error");
        return 1;
    }

    wchar_t username[256];
    (void)winxterm_dstcmd_shell_write_widef(shell,
                                            L"Saved CWD=%ls for %ls\r\n",
                                            cwd,
                                            winxterm_dstcmd_set_username(username,
                                                                        sizeof(username) /
                                                                            sizeof(username[0])));
    return 0;
}

static int winxterm_dstcmd_set_cwd_clear(WinxtermDstcmdShell *shell)
{
    wchar_t env_path[WINXTERM_DSTCMD_PATH_CAPACITY];
    wchar_t error[1024];
    WinxtermDstcmdEnvCwdStatus status =
        winxterm_dstcmd_env_clear_cwd_rc(env_path,
                                         sizeof(env_path) / sizeof(env_path[0]),
                                         error,
                                         sizeof(error) / sizeof(error[0]));
    if (status == WINXTERM_DSTCMD_ENV_CWD_UPDATED) {
        (void)winxterm_dstcmd_shell_write_widef(shell, L"Cleared CWD from %ls\r\n", env_path);
        return 0;
    }
    if (status == WINXTERM_DSTCMD_ENV_CWD_UNCHANGED) {
        (void)winxterm_dstcmd_shell_write_widef(shell,
                                                L"set CWD clear: CWD was not saved in %ls\r\n",
                                                env_path[0] != L'\0' ? env_path : L"env.rc");
        return 0;
    }
    (void)winxterm_dstcmd_shell_write_widef(shell,
                                            L"set CWD clear: could not update %ls\r\n"
                                            L"Error: %ls\r\n",
                                            env_path[0] != L'\0' ? env_path : L"env.rc",
                                            error[0] != L'\0' ? error : L"unknown error");
    return 1;
}

int winxterm_dstcmd_cmd_set(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    if (shell == 0 || argv == 0 || argv->count != 3 || argv->items[1] == 0) {
        if (shell != 0) {
            (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_SET_USAGE);
        }
        return 2;
    }

    if (wcscmp(argv->items[1], L"CWD") == 0) {
        if (wcscmp(argv->items[2], L"save") == 0) {
            return winxterm_dstcmd_set_cwd_save(shell);
        }
        if (wcscmp(argv->items[2], L"clear") == 0) {
            return winxterm_dstcmd_set_cwd_clear(shell);
        }
        (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_SET_USAGE);
        return 2;
    }

    if (wcscmp(argv->items[1], L"scale") == 0) {
        unsigned int scale = WINXTERM_DEFAULT_DISPLAY_SCALE;
        if (!winxterm_parse_display_scale_wide(argv->items[2], &scale)) {
            (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_SET_SCALE_ERROR);
            return 2;
        }

        return winxterm_dstcmd_host_set_scale(shell, scale);
    }

    if (wcscmp(argv->items[1], L"timing") == 0) {
        if (wcscmp(argv->items[2], L"on") == 0) {
            shell->timing_mode = WINXTERM_DSTCMD_TIMING_BASIC;
            return 0;
        }
        if (wcscmp(argv->items[2], L"off") == 0) {
            shell->timing_mode = WINXTERM_DSTCMD_TIMING_OFF;
            return 0;
        }
        if (wcscmp(argv->items[2], L"verbose") == 0) {
            shell->timing_mode = WINXTERM_DSTCMD_TIMING_VERBOSE;
            return 0;
        }
        (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_SET_USAGE);
        return 2;
    }

    if (wcscmp(argv->items[1], L"bell") == 0) {
        if (wcscmp(argv->items[2], L"on") == 0) {
            return winxterm_dstcmd_host_set_bell(shell, true);
        }
        if (wcscmp(argv->items[2], L"off") == 0) {
            return winxterm_dstcmd_host_set_bell(shell, false);
        }
        (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_SET_USAGE);
        return 2;
    }

    if (wcscmp(argv->items[1], L"scrollbar") == 0) {
        if (wcscmp(argv->items[2], L"on") == 0) {
            return winxterm_dstcmd_host_set_scrollbar(shell, true);
        }
        if (wcscmp(argv->items[2], L"off") == 0) {
            return winxterm_dstcmd_host_set_scrollbar(shell, false);
        }
        (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_SET_USAGE);
        return 2;
    }

    if (wcscmp(argv->items[1], L"debuglog") == 0) {
        if (wcscmp(argv->items[2], L"on") == 0) {
            if (winxterm_dstcmd_host_set_debuglog(shell, true) != 0) {
                (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_SET_LOG_ERROR);
                return 1;
            }
            return 0;
        }
        if (wcscmp(argv->items[2], L"off") == 0) {
            return winxterm_dstcmd_host_set_debuglog(shell, false);
        }
        (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_SET_USAGE);
        return 2;
    }

    if (wcscmp(argv->items[1], L"env") == 0) {
        if (wcscmp(argv->items[2], L"SAVE") == 0) {
            wchar_t env_path[32768];
            wchar_t backup_path[32768];
            wchar_t error[1024];
            size_t variable_count = 0u;
            WinxtermDstcmdEnvSaveStatus save_status =
                winxterm_dstcmd_env_save_rc(env_path,
                                            sizeof(env_path) / sizeof(env_path[0]),
                                            backup_path,
                                            sizeof(backup_path) / sizeof(backup_path[0]),
                                            &variable_count,
                                            error,
                                            sizeof(error) / sizeof(error[0]));
            if (save_status == WINXTERM_DSTCMD_ENV_SAVE_CREATED) {
                (void)winxterm_dstcmd_shell_write_widef(shell,
                                                        L"Created %ls with %zu environment variables.\r\n",
                                                        env_path,
                                                        variable_count);
                return 0;
            }
            if (save_status == WINXTERM_DSTCMD_ENV_SAVE_REPLACED) {
                (void)winxterm_dstcmd_shell_write_widef(
                    shell,
                    L"Replaced %ls with %zu environment variables.\r\n"
                    L"The previous environment was saved to %ls\r\n",
                    env_path,
                    variable_count,
                    backup_path);
                return 0;
            }
            (void)winxterm_dstcmd_shell_write_widef(shell,
                                                    L"Could not save environment to %ls\r\n"
                                                    L"Error: %ls\r\n",
                                                    env_path[0] != L'\0' ? env_path : L"env.rc",
                                                    error[0] != L'\0' ? error : L"unknown error");
            return 1;
        }
        (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_SET_USAGE);
        return 2;
    }

    (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_SET_USAGE);
    return 2;
}
