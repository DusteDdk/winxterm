#include "dstcmd/commands/which.h"

#include "dstcmd/dispatch.h"
#include "dstcmd/winxterm_dstcmd_exec.h"

#include <string.h>
#include <wchar.h>

#define WINXTERM_DSTCMD_WHICH_COLOR_RESET L"\x1b[0m"
#define WINXTERM_DSTCMD_WHICH_COLOR_STRONG_WHITE L"\x1b[1;38;2;255;255;255m"
#define WINXTERM_DSTCMD_WHICH_COLOR_GREEN L"\x1b[38;2;0;255;0m"
#define WINXTERM_DSTCMD_WHICH_COLOR_YELLOW L"\x1b[38;2;255;255;0m"
#define WINXTERM_DSTCMD_WHICH_COLOR_LIGHT_GREY L"\x1b[38;2;192;192;192m"

typedef struct WinxtermDstcmdWhichContext {
    WinxtermDstcmdShell *shell;
    size_t match_count;
    bool failed;
} WinxtermDstcmdWhichContext;

static bool winxterm_dstcmd_which_write_prefix(WinxtermDstcmdShell *shell, size_t match_index)
{
    if (shell == 0) {
        return false;
    }
    if (match_index == 0u) {
        return winxterm_dstcmd_shell_write_wide(shell, L"  ✅ ");
    }
    return winxterm_dstcmd_shell_write_widef(shell,
                                            L"  "
                                            WINXTERM_DSTCMD_WHICH_COLOR_LIGHT_GREY
                                            L"%zu"
                                            WINXTERM_DSTCMD_WHICH_COLOR_RESET
                                            L" ",
                                            match_index + 1u);
}

static const wchar_t *winxterm_dstcmd_which_basename(const wchar_t *path)
{
    const wchar_t *base = path != 0 ? path : L"";
    for (const wchar_t *p = base; *p != L'\0'; ++p) {
        if (*p == L'/' || *p == L'\\') {
            base = p + 1;
        }
    }
    return base;
}

static bool winxterm_dstcmd_which_write_builtin(WinxtermDstcmdWhichContext *context,
                                                const WinxtermDstcmdCommandEntry *entry)
{
    if (context == 0 || context->shell == 0 || entry == 0 || entry->name == 0) {
        return false;
    }
    if (!winxterm_dstcmd_which_write_prefix(context->shell, context->match_count)) {
        return false;
    }
    const wchar_t *usage_args = entry->usage_args != 0 ? entry->usage_args : L"";
    if (usage_args[0] == L'\0') {
        bool ok = winxterm_dstcmd_shell_write_widef(context->shell,
                                                    WINXTERM_DSTCMD_WHICH_COLOR_STRONG_WHITE
                                                    L"[dstcmd builtin]"
                                                    WINXTERM_DSTCMD_WHICH_COLOR_RESET
                                                    L" "
                                                    WINXTERM_DSTCMD_WHICH_COLOR_GREEN
                                                    L"%ls"
                                                    WINXTERM_DSTCMD_WHICH_COLOR_RESET
                                                    L"\r\n",
                                                    entry->name);
        if (ok) {
            ++context->match_count;
        }
        return ok;
    }
    bool ok = winxterm_dstcmd_shell_write_widef(context->shell,
                                                WINXTERM_DSTCMD_WHICH_COLOR_STRONG_WHITE
                                                L"[dstcmd builtin]"
                                                WINXTERM_DSTCMD_WHICH_COLOR_RESET
                                                L" "
                                                WINXTERM_DSTCMD_WHICH_COLOR_GREEN
                                                L"%ls"
                                                WINXTERM_DSTCMD_WHICH_COLOR_RESET
                                                L" "
                                                WINXTERM_DSTCMD_WHICH_COLOR_LIGHT_GREY
                                                L"%ls"
                                                WINXTERM_DSTCMD_WHICH_COLOR_RESET
                                                L"\r\n",
                                                entry->name,
                                                usage_args);
    if (ok) {
        ++context->match_count;
    }
    return ok;
}

static bool winxterm_dstcmd_which_write_alias(WinxtermDstcmdWhichContext *context,
                                              const WinxtermDstcmdAlias *alias)
{
    if (context == 0 || context->shell == 0 || alias == 0 ||
        alias->name == 0 || alias->description == 0) {
        return false;
    }
    if (!winxterm_dstcmd_which_write_prefix(context->shell, context->match_count)) {
        return false;
    }
    bool ok = winxterm_dstcmd_shell_write_widef(context->shell,
                                                WINXTERM_DSTCMD_WHICH_COLOR_STRONG_WHITE
                                                L"[dstshell alias]"
                                                WINXTERM_DSTCMD_WHICH_COLOR_RESET
                                                L" "
                                                WINXTERM_DSTCMD_WHICH_COLOR_GREEN
                                                L"%ls"
                                                WINXTERM_DSTCMD_WHICH_COLOR_RESET
                                                L" "
                                                WINXTERM_DSTCMD_WHICH_COLOR_LIGHT_GREY
                                                L"%ls"
                                                WINXTERM_DSTCMD_WHICH_COLOR_RESET
                                                L"\r\n",
                                                alias->name,
                                                alias->description);
    if (ok) {
        ++context->match_count;
    }
    return ok;
}

static bool winxterm_dstcmd_which_write_path_match(WinxtermDstcmdWhichContext *context,
                                                   const WinxtermDstcmdResolvedExec *resolved)
{
    if (context == 0 || context->shell == 0 || resolved == 0) {
        return false;
    }
    wchar_t path[WINXTERM_DSTCMD_PATH_CAPACITY];
    if (wcscpy_s(path, WINXTERM_DSTCMD_PATH_CAPACITY, resolved->path) != 0) {
        return false;
    }

    const wchar_t *base = winxterm_dstcmd_which_basename(path);
    wchar_t name[WINXTERM_DSTCMD_PATH_CAPACITY];
    if (wcscpy_s(name, WINXTERM_DSTCMD_PATH_CAPACITY, base) != 0) {
        return false;
    }
    size_t parent_length = (size_t)(base - path);
    path[parent_length] = L'\0';

    const wchar_t *parent_color = context->match_count == 0u ?
        WINXTERM_DSTCMD_WHICH_COLOR_STRONG_WHITE : WINXTERM_DSTCMD_WHICH_COLOR_YELLOW;
    const wchar_t *name_color = context->match_count == 0u ?
        WINXTERM_DSTCMD_WHICH_COLOR_GREEN : WINXTERM_DSTCMD_WHICH_COLOR_LIGHT_GREY;

    if (!winxterm_dstcmd_which_write_prefix(context->shell, context->match_count)) {
        return false;
    }
    bool ok = winxterm_dstcmd_shell_write_widef(context->shell,
                                               L"%ls%ls"
                                               WINXTERM_DSTCMD_WHICH_COLOR_RESET
                                               L" %ls%ls"
                                               WINXTERM_DSTCMD_WHICH_COLOR_RESET
                                               L"\r\n",
                                               parent_color,
                                               path,
                                               name_color,
                                               name);
    if (ok) {
        ++context->match_count;
    }
    return ok;
}

static bool winxterm_dstcmd_which_path_match_visitor(const WinxtermDstcmdResolvedExec *resolved,
                                                     void *context)
{
    WinxtermDstcmdWhichContext *which_context = (WinxtermDstcmdWhichContext *)context;
    if (!winxterm_dstcmd_which_write_path_match(which_context, resolved)) {
        if (which_context != 0) {
            which_context->failed = true;
        }
        return false;
    }
    return true;
}

int winxterm_dstcmd_cmd_which(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    if (shell == 0 || argv == 0 || argv->count != 2 || argv->items[1] == 0) {
        if (shell != 0) {
            (void)winxterm_dstcmd_shell_write_wide(shell, L"which: usage: which NAME\r\n");
        }
        return 2;
    }

    const wchar_t *name = argv->items[1];
    WinxtermDstcmdWhichContext context;
    memset(&context, 0, sizeof(context));
    context.shell = shell;

    const WinxtermDstcmdAlias *alias = winxterm_dstcmd_shell_find_alias(shell, name);
    if (alias != 0) {
        if (!winxterm_dstcmd_which_write_alias(&context, alias)) {
            return 1;
        }
    }

    const WinxtermDstcmdCommandEntry *entry = winxterm_dstcmd_find_builtin_entry(name);
    if (entry != 0) {
        if (!winxterm_dstcmd_which_write_builtin(&context, entry)) {
            return 1;
        }
    }

    if (!winxterm_dstcmd_exec_for_each_path_match(name,
                                                 &shell->scratch,
                                                 winxterm_dstcmd_which_path_match_visitor,
                                                 &context) ||
        context.failed) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"which: command lookup failed\r\n");
        return 1;
    }
    if (context.match_count == 0u) {
        (void)winxterm_dstcmd_shell_write_widef(shell, L"%ls: not found\r\n", name);
        return 1;
    }
    return 0;
}
