#include "dstcmd/commands/mv.h"

#include "dstcmd/api/path.h"

#include <string.h>
#include <windows.h>

static const wchar_t *winxterm_dstcmd_display_path(const wchar_t *path,
                                                   wchar_t *buffer,
                                                   size_t buffer_count)
{
    return winxterm_dstcmd_path_to_display(path, buffer, buffer_count) ? buffer : path;
}

static bool winxterm_dstcmd_parse_mv_options(const WinxtermDstcmdArgv *argv,
                                             int *operand_start,
                                             bool *force)
{
    *operand_start = 1;
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
            } else {
                return false;
            }
        }
    }
    *operand_start = argv->count;
    return true;
}

int winxterm_dstcmd_cmd_mv(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    int operand_start = 1;
    bool force = false;
    if (!winxterm_dstcmd_parse_mv_options(argv, &operand_start, &force) ||
        argv->count - operand_start < 2) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"mv: usage: mv [-f] source... destination\r\n");
        return 2;
    }
    const wchar_t *destination_operand = argv->items[argv->count - 1];
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    wchar_t *destination = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *source = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *display_source = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *actual_destination = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    if (destination == 0 || source == 0 || display_source == 0 || actual_destination == 0) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"mv: out of memory\r\n");
        return 1;
    }

    int status = 0;
    if (!winxterm_dstcmd_path_resolve_scratch(&shell->scratch,
                                             shell->cwd,
                                             destination_operand,
                                             destination,
                                             WINXTERM_DSTCMD_PATH_CAPACITY)) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"mv: destination path too long\r\n");
        status = 1;
        goto cleanup;
    }
    bool multiple_sources = argv->count - operand_start > 2;
    bool destination_is_directory = winxterm_dstcmd_path_is_directory_scratch(&shell->scratch, destination);
    if (multiple_sources && !destination_is_directory) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"mv: target is not a directory\r\n");
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
                                                    L"mv: source path too long: %ls\r\n",
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
                (void)winxterm_dstcmd_shell_write_wide(shell, L"mv: destination path too long\r\n");
                status = 1;
                continue;
            }
        } else {
            wcscpy_s(actual_destination, WINXTERM_DSTCMD_PATH_CAPACITY, destination);
        }
        DWORD flags = MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING;
        if (force) {
            (void)winxterm_dstcmd_path_set_attributes_scratch(&shell->scratch,
                                                              actual_destination,
                                                              FILE_ATTRIBUTE_NORMAL);
        }
        if (!winxterm_dstcmd_path_move_file_scratch(&shell->scratch, source, actual_destination, flags)) {
            (void)winxterm_dstcmd_shell_write_widef(shell, L"mv: cannot move '%ls' to '%ls'\r\n", source, actual_destination);
            status = 1;
        }
    }
cleanup:
    winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
    return status;
}
