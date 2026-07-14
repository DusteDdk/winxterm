#include "dstcmd/commands/playmacro.h"

#include "dstcmd/api/path.h"
#include "dstcmd/winxterm_dstcmd.h"
#include "dstcmd/winxterm_dstcmd_host.h"

static const wchar_t WINXTERM_DSTCMD_PLAYMACRO_USAGE[] =
    L"usage: playmacro FILENAME\r\n"
    L"       playmacro -i MACRO\r\n";
static const wchar_t WINXTERM_DSTCMD_PLAYMACRO_ERROR[] = L"playmacro: failed to start macro\r\n";

static void winxterm_dstcmd_playmacro_write_not_found(WinxtermDstcmdShell *shell, const wchar_t *path)
{
    (void)winxterm_dstcmd_shell_write_widef(shell,
                                            L"\"Macro: %ls\" was not found.\r\n",
                                            path != 0 ? path : L"");
}

int winxterm_dstcmd_cmd_playmacro(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    if (shell == 0 || argv == 0) {
        return 2;
    }
    if (argv->count == 3 && argv->items[1] != 0 && argv->items[2] != 0 &&
        wcscmp(argv->items[1], L"-i") == 0) {
        if (winxterm_dstcmd_host_playmacro_text(shell, argv->items[2]) != 0) {
            (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_PLAYMACRO_ERROR);
            return 1;
        }
        return 0;
    }
    if (argv->count != 2 || argv->items[1] == 0) {
        if (shell != 0) {
            (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_PLAYMACRO_USAGE);
        }
        return 2;
    }

    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    wchar_t *path = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    if (path == 0) {
        (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_PLAYMACRO_ERROR);
        return 1;
    }
    if (!winxterm_dstcmd_path_resolve_scratch(&shell->scratch,
                                             shell->cwd,
                                             argv->items[1],
                                             path,
                                             WINXTERM_DSTCMD_PATH_CAPACITY)) {
        winxterm_dstcmd_playmacro_write_not_found(shell, argv->items[1]);
        winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
        return 1;
    }
    WinxtermDstcmdPathInfo info;
    if (!winxterm_dstcmd_path_get_info_scratch(&shell->scratch, path, &info) ||
        (info.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
        winxterm_dstcmd_playmacro_write_not_found(shell, path);
        winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
        return 1;
    }
    if (winxterm_dstcmd_host_playmacro(shell, path) != 0) {
        (void)winxterm_dstcmd_shell_write_wide(shell, WINXTERM_DSTCMD_PLAYMACRO_ERROR);
        winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
        return 1;
    }
    winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
    return 0;
}
