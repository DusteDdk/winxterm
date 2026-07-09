#include "dstcmd/commands/cp.h"

#include "dstcmd/api/path.h"

#include <string.h>
#include <windows.h>

static const wchar_t *winxterm_dstcmd_display_path(const wchar_t *path,
                                                   wchar_t *buffer,
                                                   size_t buffer_count)
{
    return winxterm_dstcmd_path_to_display(path, buffer, buffer_count) ? buffer : path;
}

static bool winxterm_dstcmd_delete_existing_file(WinxtermDstcmdScratch *scratch,
                                                 const wchar_t *path,
                                                 bool force)
{
    if (force) {
        (void)winxterm_dstcmd_path_set_attributes_scratch(scratch, path, FILE_ATTRIBUTE_NORMAL);
    }
    if (winxterm_dstcmd_path_delete_file_scratch(scratch, path)) {
        return true;
    }
    DWORD error = GetLastError();
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
}

static int winxterm_dstcmd_copy_path(WinxtermDstcmdShell *shell,
                                     const wchar_t *source,
                                     const wchar_t *destination,
                                     bool recursive,
                                     bool force)
{
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    wchar_t *child_source = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *child_destination = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    if (child_source == 0 || child_destination == 0) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"cp: out of memory\r\n");
        return 1;
    }

    int status = 0;
    WinxtermDstcmdPathInfo info;
    if (!winxterm_dstcmd_path_get_info_scratch(&shell->scratch, source, &info)) {
        (void)winxterm_dstcmd_shell_write_widef(shell, L"cp: cannot stat '%ls'\r\n", source);
        status = 1;
        goto cleanup;
    }
    if ((info.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
        if (force) {
            (void)winxterm_dstcmd_path_set_attributes_scratch(&shell->scratch,
                                                              destination,
                                                              FILE_ATTRIBUTE_NORMAL);
        }
        if (winxterm_dstcmd_path_copy_file_scratch(&shell->scratch, source, destination, false)) {
            goto cleanup;
        }
        if (force && winxterm_dstcmd_delete_existing_file(&shell->scratch, destination, true) &&
            winxterm_dstcmd_path_copy_file_scratch(&shell->scratch, source, destination, false)) {
            goto cleanup;
        }
        (void)winxterm_dstcmd_shell_write_widef(shell, L"cp: cannot copy '%ls' to '%ls'\r\n", source, destination);
        status = 1;
        goto cleanup;
    }
    if (!recursive) {
        (void)winxterm_dstcmd_shell_write_widef(shell, L"cp: omitting directory '%ls'\r\n", source);
        status = 1;
        goto cleanup;
    }
    if (!winxterm_dstcmd_path_create_directory_scratch(&shell->scratch, destination) &&
        GetLastError() != ERROR_ALREADY_EXISTS) {
        (void)winxterm_dstcmd_shell_write_widef(shell, L"cp: cannot create directory '%ls'\r\n", destination);
        status = 1;
        goto cleanup;
    }

    WinxtermDstcmdDirIter iter;
    if (!winxterm_dstcmd_dir_iter_open_scratch(&shell->scratch, source, &iter)) {
        goto cleanup;
    }
    const WIN32_FIND_DATAW *data = 0;
    while (winxterm_dstcmd_dir_iter_next(&iter, &data)) {
        if (wcscmp(data->cFileName, L".") == 0 || wcscmp(data->cFileName, L"..") == 0) {
            continue;
        }
        if (!winxterm_dstcmd_path_append_child(source, data->cFileName, child_source, WINXTERM_DSTCMD_PATH_CAPACITY) ||
            !winxterm_dstcmd_path_append_child(destination,
                                              data->cFileName,
                                              child_destination,
                                              WINXTERM_DSTCMD_PATH_CAPACITY)) {
            (void)winxterm_dstcmd_shell_write_wide(shell, L"cp: path too long\r\n");
            status = 1;
            break;
        }
        if (winxterm_dstcmd_copy_path(shell, child_source, child_destination, recursive, force) != 0) {
            status = 1;
        }
    }
    winxterm_dstcmd_dir_iter_close(&iter);
cleanup:
    winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
    return status;
}

static bool winxterm_dstcmd_parse_cp_options(const WinxtermDstcmdArgv *argv,
                                             int *operand_start,
                                             bool *recursive,
                                             bool *force)
{
    *operand_start = 1;
    *recursive = false;
    *force = false;
    for (int i = 1; i < argv->count; ++i) {
        const wchar_t *arg = argv->items[i];
        if (arg == 0 || arg[0] != L'-' || arg[1] == L'\0') {
            *operand_start = i;
            return true;
        }
        if (wcscmp(arg, L"--") == 0) {
            *operand_start = i + 1;
            return true;
        }
        for (int j = 1; arg[j] != L'\0'; ++j) {
            if (arg[j] == L'f') {
                *force = true;
            } else if (arg[j] == L'r' || arg[j] == L'R') {
                *recursive = true;
            } else {
                return false;
            }
        }
    }
    *operand_start = argv->count;
    return true;
}

int winxterm_dstcmd_cmd_cp(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    int operand_start = 1;
    bool recursive = false;
    bool force = false;
    if (!winxterm_dstcmd_parse_cp_options(argv, &operand_start, &recursive, &force) ||
        argv->count - operand_start < 2) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"cp: usage: cp [-Rrf] source... destination\r\n");
        return 2;
    }
    const wchar_t *destination_operand = argv->items[argv->count - 1];
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    wchar_t *destination = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *source = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *display_source = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *actual_destination = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    if (destination == 0 || source == 0 || display_source == 0 || actual_destination == 0) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"cp: out of memory\r\n");
        return 1;
    }

    int status = 0;
    if (!winxterm_dstcmd_path_resolve_scratch(&shell->scratch,
                                             shell->cwd,
                                             destination_operand,
                                             destination,
                                             WINXTERM_DSTCMD_PATH_CAPACITY)) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"cp: destination path too long\r\n");
        status = 1;
        goto cleanup;
    }
    bool multiple_sources = argv->count - operand_start > 2;
    bool destination_is_directory = winxterm_dstcmd_path_is_directory_scratch(&shell->scratch, destination);
    if (multiple_sources && !destination_is_directory) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"cp: target is not a directory\r\n");
        status = 1;
        goto cleanup;
    }
    for (int i = operand_start; i < argv->count - 1; ++i) {
        if (!winxterm_dstcmd_path_resolve_scratch(&shell->scratch,
                                                 shell->cwd,
                                                 argv->items[i],
                                                 source,
                                                 WINXTERM_DSTCMD_PATH_CAPACITY)) {
            (void)winxterm_dstcmd_shell_write_widef(shell,
                                                    L"cp: source path too long: %ls\r\n",
                                                    winxterm_dstcmd_display_path(argv->items[i],
                                                                                display_source,
                                                                                WINXTERM_DSTCMD_PATH_CAPACITY));
            status = 1;
            continue;
        }
        if (destination_is_directory) {
            if (!winxterm_dstcmd_path_append_child(destination,
                                                  winxterm_dstcmd_path_basename(source),
                                                  actual_destination,
                                                  WINXTERM_DSTCMD_PATH_CAPACITY)) {
                (void)winxterm_dstcmd_shell_write_wide(shell, L"cp: destination path too long\r\n");
                status = 1;
                continue;
            }
        } else {
            wcscpy_s(actual_destination, WINXTERM_DSTCMD_PATH_CAPACITY, destination);
        }
        if (winxterm_dstcmd_copy_path(shell, source, actual_destination, recursive, force) != 0) {
            status = 1;
        }
    }
cleanup:
    winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
    return status;
}
