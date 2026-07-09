#include "dstcmd/commands/cd.h"

#include "dstcmd/api/path.h"
#include "dstcmd/winxterm_dstcmd_parse.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

static const wchar_t *winxterm_dstcmd_display_path(const wchar_t *path,
                                                   wchar_t *buffer,
                                                   size_t buffer_count)
{
    return winxterm_dstcmd_path_to_display(path, buffer, buffer_count) ? buffer : path;
}

static void winxterm_dstcmd_print_stack(WinxtermDstcmdShell *shell)
{
    (void)winxterm_dstcmd_shell_write_widef(shell, L"%ls", shell->cwd);
    for (size_t i = shell->directory_stack_count; i > 0u; --i) {
        (void)winxterm_dstcmd_shell_write_widef(shell, L" %ls", shell->directory_stack[i - 1u]);
    }
    (void)winxterm_dstcmd_shell_write_wide(shell, L"\r\n");
}

static const wchar_t *winxterm_dstcmd_home_directory(WinxtermDstcmdShell *shell,
                                                     wchar_t *home,
                                                     size_t home_count)
{
    if (shell == 0 || home == 0 || home_count == 0u) {
        return 0;
    }
    DWORD length = GetEnvironmentVariableW(L"USERPROFILE", home, (DWORD)home_count);
    if (length != 0u && length < home_count) {
        return home;
    }
    wchar_t drive[16];
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    wchar_t *path = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    if (path == 0) {
        return 0;
    }
    DWORD drive_length = GetEnvironmentVariableW(L"HOMEDRIVE", drive, 16u);
    DWORD path_length = GetEnvironmentVariableW(L"HOMEPATH", path, WINXTERM_DSTCMD_PATH_CAPACITY);
    if (drive_length != 0u && drive_length < 16u && path_length != 0u && path_length < WINXTERM_DSTCMD_PATH_CAPACITY) {
        bool ok = _snwprintf_s(home, home_count, _TRUNCATE, L"%ls%ls", drive, path) >= 0;
        winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
        return ok ? home : 0;
    }
    winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
    return 0;
}

int winxterm_dstcmd_cmd_cd(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    if (argv->count > 2) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"cd: too many arguments\r\n");
        return 2;
    }
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    wchar_t *home = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *resolved = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *display_target = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *old_cwd = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    if (home == 0 || resolved == 0 || display_target == 0 || old_cwd == 0) {
        return 1;
    }
    const wchar_t *target =
        argv->count == 1 ? winxterm_dstcmd_home_directory(shell, home, WINXTERM_DSTCMD_PATH_CAPACITY) : argv->items[1];
    int status = 0;
    if (target == 0 || target[0] == L'\0') {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"cd: home directory is not set\r\n");
        status = 1;
        goto cleanup;
    }
    if (wcscmp(target, L"-") == 0) {
        if (shell->previous_cwd[0] == L'\0') {
            (void)winxterm_dstcmd_shell_write_wide(shell, L"cd: OLDPWD not set\r\n");
            status = 1;
            goto cleanup;
        }
        target = shell->previous_cwd;
    }
    if (!winxterm_dstcmd_path_resolve_scratch(&shell->scratch,
                                             shell->cwd,
                                             target,
                                             resolved,
                                             WINXTERM_DSTCMD_PATH_CAPACITY) ||
        !winxterm_dstcmd_path_is_directory_scratch(&shell->scratch, resolved)) {
        (void)winxterm_dstcmd_shell_write_widef(shell,
                                                L"cd: no such directory: %ls\r\n",
                                                winxterm_dstcmd_display_path(target,
                                                                            display_target,
                                                                            WINXTERM_DSTCMD_PATH_CAPACITY));
        status = 1;
        goto cleanup;
    }
    wcscpy_s(old_cwd, WINXTERM_DSTCMD_PATH_CAPACITY, shell->cwd);
    if (!winxterm_dstcmd_shell_set_cwd(shell, resolved)) {
        (void)winxterm_dstcmd_shell_write_widef(shell,
                                                L"cd: cannot enter: %ls\r\n",
                                                winxterm_dstcmd_display_path(target,
                                                                            display_target,
                                                                            WINXTERM_DSTCMD_PATH_CAPACITY));
        status = 1;
        goto cleanup;
    }
    wcscpy_s(shell->previous_cwd, WINXTERM_DSTCMD_PATH_CAPACITY, old_cwd);
    winxterm_dstcmd_shell_invalidate_dir_cache(shell);
    (void)winxterm_dstcmd_shell_schedule_dir_cache_refresh(shell, WINXTERM_DSTCMD_DIR_REFRESH_CD);
    if (argv->count == 2 && wcscmp(argv->items[1], L"-") == 0) {
        (void)winxterm_dstcmd_shell_write_widef(shell, L"%ls\r\n", shell->cwd);
    }
cleanup:
    winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
    return status;
}

int winxterm_dstcmd_cmd_pushd(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    if (argv->count != 2) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"pushd: expected one directory\r\n");
        return 2;
    }
    if (shell->directory_stack_count == WINXTERM_DSTCMD_STACK_CAPACITY) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"pushd: directory stack full\r\n");
        return 1;
    }
    wchar_t *old_cwd = winxterm_dstcmd_wcsdup(shell->cwd);
    if (old_cwd == 0) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"pushd: out of memory\r\n");
        return 1;
    }
    WinxtermDstcmdArgv cd_argv;
    wchar_t *items[2];
    items[0] = L"cd";
    items[1] = argv->items[1];
    cd_argv.items = items;
    cd_argv.count = 2;
    int status = winxterm_dstcmd_cmd_cd(shell, &cd_argv);
    if (status != 0) {
        free(old_cwd);
        return status;
    }
    shell->directory_stack[shell->directory_stack_count++] = old_cwd;
    winxterm_dstcmd_print_stack(shell);
    return 0;
}

int winxterm_dstcmd_cmd_popd(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    if (argv->count != 1) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"popd: too many arguments\r\n");
        return 2;
    }
    if (shell->directory_stack_count == 0u) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"popd: directory stack empty\r\n");
        return 1;
    }
    wchar_t *target = shell->directory_stack[--shell->directory_stack_count];
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    wchar_t *old_cwd = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    if (old_cwd == 0) {
        return 1;
    }
    wcscpy_s(old_cwd, WINXTERM_DSTCMD_PATH_CAPACITY, shell->cwd);
    if (!winxterm_dstcmd_shell_set_cwd(shell, target)) {
        (void)winxterm_dstcmd_shell_write_widef(shell, L"popd: cannot enter: %ls\r\n", target);
        winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
        free(target);
        return 1;
    }
    wcscpy_s(shell->previous_cwd, WINXTERM_DSTCMD_PATH_CAPACITY, old_cwd);
    winxterm_dstcmd_shell_invalidate_dir_cache(shell);
    (void)winxterm_dstcmd_shell_schedule_dir_cache_refresh(shell, WINXTERM_DSTCMD_DIR_REFRESH_CD);
    free(target);
    winxterm_dstcmd_print_stack(shell);
    winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
    return 0;
}
