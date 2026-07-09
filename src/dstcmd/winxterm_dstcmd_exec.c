#include "dstcmd/winxterm_dstcmd_exec.h"

#include "dstcmd/api/unicode.h"
#include "dstcmd/api/path.h"
#include "dstcmd/dispatch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define WINXTERM_DSTCMD_CLIENT_TITLE_MAX_COLUMNS 240u
#define WINXTERM_DSTCMD_CLIENT_TITLE_ELLIPSIS L"...."
#define WINXTERM_DSTCMD_CLIENT_TITLE_ELLIPSIS_COLUMNS 4u

static bool winxterm_dstcmd_is_slash(wchar_t ch)
{
    return ch == L'\\' || ch == L'/';
}

static bool winxterm_dstcmd_command_has_path(const wchar_t *command)
{
    if (command == 0) {
        return false;
    }
    for (const wchar_t *p = command; *p != L'\0'; ++p) {
        if (winxterm_dstcmd_is_slash(*p)) {
            return true;
        }
    }
    return false;
}

static bool winxterm_dstcmd_starts_dot_slash(const wchar_t *command)
{
    return command != 0 &&
           command[0] == L'.' &&
           winxterm_dstcmd_is_slash(command[1]) &&
           command[2] != L'\0';
}

static size_t winxterm_dstcmd_exec_title_room(const WinxtermDstcmdShell *shell)
{
    int columns = winxterm_dstcmd_shell_terminal_columns(shell);
    size_t room = columns > 0 ? (size_t)columns : 80u;
    return room > WINXTERM_DSTCMD_CLIENT_TITLE_MAX_COLUMNS ?
        WINXTERM_DSTCMD_CLIENT_TITLE_MAX_COLUMNS : room;
}

static size_t winxterm_dstcmd_exec_wide_columns(const wchar_t *text)
{
    if (text == 0) {
        return 0u;
    }
    size_t length = wcslen(text);
    size_t offset = 0u;
    size_t columns = 0u;
    while (offset < length) {
        uint32_t codepoint = 0u;
        size_t before = offset;
        (void)winxterm_dstcmd_wide_decode_next(text, length, &offset, &codepoint);
        if (offset <= before) {
            offset = before + 1u;
        }
        int width = winxterm_dstcmd_codepoint_width(codepoint);
        if (width > 0) {
            columns += (size_t)width;
        }
    }
    return columns;
}

static size_t winxterm_dstcmd_exec_advance_boundaries(const wchar_t *text,
                                                      size_t length,
                                                      size_t count)
{
    size_t offset = 0u;
    for (size_t i = 0u; i < count && offset < length; ++i) {
        offset = winxterm_dstcmd_wide_next_boundary(text, length, offset);
    }
    return offset > length ? length : offset;
}

static size_t winxterm_dstcmd_exec_retreat_boundaries(const wchar_t *text,
                                                      size_t length,
                                                      size_t count)
{
    size_t offset = length;
    for (size_t i = 0u; i < count && offset > 0u; ++i) {
        offset = winxterm_dstcmd_wide_prev_boundary(text, length, offset);
    }
    return offset;
}

static bool winxterm_dstcmd_exec_copy_truncated_title_arg(const wchar_t *arg,
                                                         size_t room,
                                                         wchar_t *out,
                                                         size_t out_count)
{
    if (arg == 0 || out == 0 || out_count == 0u) {
        return false;
    }
    if (room >= out_count) {
        room = out_count - 1u;
    }
    if (room == 0u) {
        out[0] = L'\0';
        return true;
    }
    if (winxterm_dstcmd_exec_wide_columns(arg) <= room) {
        return wcscpy_s(out, out_count, arg) == 0;
    }
    size_t length = wcslen(arg);
    if (room <= WINXTERM_DSTCMD_CLIENT_TITLE_ELLIPSIS_COLUMNS) {
        size_t end = winxterm_dstcmd_exec_advance_boundaries(arg, length, room);
        if (end >= out_count) {
            end = out_count - 1u;
        }
        memcpy(out, arg, end * sizeof(*out));
        out[end] = L'\0';
        return true;
    }

    size_t visible_columns = room - WINXTERM_DSTCMD_CLIENT_TITLE_ELLIPSIS_COLUMNS;
    size_t prefix_columns = visible_columns > 4u ? 4u : visible_columns / 2u;
    size_t suffix_columns = visible_columns - prefix_columns;
    if (prefix_columns == 0u && visible_columns != 0u) {
        prefix_columns = 1u;
        --suffix_columns;
    }
    size_t prefix_end = winxterm_dstcmd_exec_advance_boundaries(arg, length, prefix_columns);
    size_t suffix_start = winxterm_dstcmd_exec_retreat_boundaries(arg, length, suffix_columns);
    if (suffix_start < prefix_end) {
        suffix_start = prefix_end;
    }
    size_t prefix_count = prefix_end;
    size_t suffix_count = length - suffix_start;
    size_t total = prefix_count + WINXTERM_DSTCMD_CLIENT_TITLE_ELLIPSIS_COLUMNS + suffix_count;
    if (total >= out_count) {
        return false;
    }
    size_t offset = 0u;
    memcpy(out + offset, arg, prefix_count * sizeof(*out));
    offset += prefix_count;
    memcpy(out + offset,
           WINXTERM_DSTCMD_CLIENT_TITLE_ELLIPSIS,
           WINXTERM_DSTCMD_CLIENT_TITLE_ELLIPSIS_COLUMNS * sizeof(*out));
    offset += WINXTERM_DSTCMD_CLIENT_TITLE_ELLIPSIS_COLUMNS;
    memcpy(out + offset, arg + suffix_start, suffix_count * sizeof(*out));
    offset += suffix_count;
    out[offset] = L'\0';
    return true;
}

static bool winxterm_dstcmd_exec_build_client_title(const WinxtermDstcmdShell *shell,
                                                   const WinxtermDstcmdArgv *argv,
                                                   wchar_t *out,
                                                   size_t out_count)
{
    if (argv == 0 || argv->count <= 0 || argv->items == 0 || argv->items[0] == 0 ||
        out == 0 || out_count == 0u) {
        return false;
    }
    size_t room = winxterm_dstcmd_exec_title_room(shell);
    if (!winxterm_dstcmd_exec_copy_truncated_title_arg(argv->items[0], room, out, out_count)) {
        return false;
    }
    size_t used = winxterm_dstcmd_exec_wide_columns(out);
    size_t offset = wcslen(out);
    for (int i = 1; i < argv->count; ++i) {
        const wchar_t *arg = argv->items[i];
        if (arg == 0) {
            continue;
        }
        size_t arg_columns = winxterm_dstcmd_exec_wide_columns(arg);
        size_t arg_length = wcslen(arg);
        if (used + 1u + arg_columns > room || offset + 1u + arg_length >= out_count) {
            break;
        }
        out[offset++] = L' ';
        memcpy(out + offset, arg, arg_length * sizeof(*out));
        offset += arg_length;
        out[offset] = L'\0';
        used += 1u + arg_columns;
    }
    return true;
}

static bool winxterm_dstcmd_file_exists(WinxtermDstcmdScratch *scratch, const wchar_t *path)
{
    WinxtermDstcmdPathInfo info;
    return winxterm_dstcmd_path_get_info_scratch(scratch, path, &info) &&
           (info.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u;
}

static const wchar_t *winxterm_dstcmd_extension(const wchar_t *path)
{
    const wchar_t *extension = wcsrchr(path, L'.');
    const wchar_t *slash = path;
    for (const wchar_t *p = path; *p != L'\0'; ++p) {
        if (winxterm_dstcmd_is_slash(*p)) {
            slash = p + 1;
        }
    }
    return extension != 0 && extension > slash ? extension : L"";
}

static bool winxterm_dstcmd_find_program_on_system_path(const wchar_t *name,
                                                        WinxtermDstcmdScratch *scratch,
                                                        wchar_t *out,
                                                        size_t out_count)
{
    if (name == 0 || out == 0 || out_count == 0u) {
        return false;
    }
    DWORD length = SearchPathW(0, name, 0, (DWORD)out_count, out, 0);
    return length != 0u &&
           length < out_count &&
           winxterm_dstcmd_file_exists(scratch, out) &&
           winxterm_dstcmd_path_to_display(out, out, out_count);
}

static WinxtermDstcmdExecKind winxterm_dstcmd_kind_from_extension(const wchar_t *extension)
{
    if (_wcsicmp(extension, L".exe") == 0 || _wcsicmp(extension, L".com") == 0) {
        return WINXTERM_DSTCMD_EXEC_NATIVE;
    }
    if (_wcsicmp(extension, L".ps1") == 0) {
        return WINXTERM_DSTCMD_EXEC_POWERSHELL;
    }
    if (_wcsicmp(extension, L".cmd") == 0 || _wcsicmp(extension, L".bat") == 0) {
        return WINXTERM_DSTCMD_EXEC_CMD;
    }
    if (_wcsicmp(extension, L".sh") == 0) {
        return WINXTERM_DSTCMD_EXEC_BASH;
    }
    return WINXTERM_DSTCMD_EXEC_UNSUPPORTED;
}

static bool winxterm_dstcmd_resolve_interpreter(WinxtermDstcmdExecKind kind,
                                                WinxtermDstcmdScratch *scratch,
                                                wchar_t *out,
                                                size_t out_count)
{
    if (out == 0 || out_count == 0u) {
        return false;
    }
    out[0] = L'\0';
    switch (kind) {
    case WINXTERM_DSTCMD_EXEC_POWERSHELL:
        return winxterm_dstcmd_find_program_on_system_path(L"powershell.exe", scratch, out, out_count) ||
               winxterm_dstcmd_find_program_on_system_path(L"pwsh.exe", scratch, out, out_count);
    case WINXTERM_DSTCMD_EXEC_CMD:
        return winxterm_dstcmd_find_program_on_system_path(L"cmd.exe", scratch, out, out_count);
    case WINXTERM_DSTCMD_EXEC_BASH:
        return winxterm_dstcmd_find_program_on_system_path(L"bash.exe", scratch, out, out_count);
    default:
        return true;
    }
}

static WinxtermDstcmdExecKind winxterm_dstcmd_kind_from_shebang_text(const char *text)
{
    if (text == 0) {
        return WINXTERM_DSTCMD_EXEC_UNSUPPORTED;
    }
    char lower[256];
    size_t i = 0u;
    for (; text[i] != '\0' && text[i] != '\r' && text[i] != '\n' && i + 1u < sizeof(lower); ++i) {
        char ch = text[i];
        lower[i] = (char)((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
    }
    lower[i] = '\0';
    if (strstr(lower, "powershell") != 0 || strstr(lower, "pwsh") != 0) {
        return WINXTERM_DSTCMD_EXEC_POWERSHELL;
    }
    if (strstr(lower, "cmd") != 0) {
        return WINXTERM_DSTCMD_EXEC_CMD;
    }
    if (strstr(lower, "bash") != 0 || strstr(lower, " sh") != 0 ||
        (i >= 3u && strcmp(lower + i - 3u, "/sh") == 0)) {
        return WINXTERM_DSTCMD_EXEC_BASH;
    }
    return WINXTERM_DSTCMD_EXEC_UNSUPPORTED;
}

static WinxtermDstcmdExecKind winxterm_dstcmd_kind_from_shebang(WinxtermDstcmdScratch *scratch,
                                                                const wchar_t *path)
{
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(scratch);
    WinxtermDstcmdWin32Path win32_path;
    WinxtermDstcmdExecKind kind = WINXTERM_DSTCMD_EXEC_UNSUPPORTED;
    if (!winxterm_dstcmd_path_prepare_win32_scratch(scratch, path, &win32_path)) {
        goto cleanup;
    }
    HANDLE file = CreateFileW(win32_path.syscall,
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              0,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              0);
    if (file == INVALID_HANDLE_VALUE) {
        goto cleanup;
    }
    char buffer[256];
    DWORD read_count = 0;
    BOOL ok = ReadFile(file, buffer, (DWORD)(sizeof(buffer) - 1u), &read_count, 0);
    CloseHandle(file);
    if (!ok || read_count < 2u || buffer[0] != '#' || buffer[1] != '!') {
        goto cleanup;
    }
    buffer[read_count] = '\0';
    kind = winxterm_dstcmd_kind_from_shebang_text(buffer + 2);

cleanup:
    winxterm_dstcmd_scratch_rewind(scratch, mark);
    return kind;
}

static bool winxterm_dstcmd_complete_resolved(WinxtermDstcmdScratch *scratch,
                                              const wchar_t *path,
                                              WinxtermDstcmdResolvedExec *resolved)
{
    if (scratch == 0 || path == 0 || resolved == 0 || !winxterm_dstcmd_file_exists(scratch, path)) {
        return false;
    }
    if (!winxterm_dstcmd_path_to_display(path, resolved->path, WINXTERM_DSTCMD_PATH_CAPACITY)) {
        return false;
    }

    resolved->kind = winxterm_dstcmd_kind_from_extension(winxterm_dstcmd_extension(resolved->path));
    if (resolved->kind == WINXTERM_DSTCMD_EXEC_UNSUPPORTED) {
        resolved->kind = winxterm_dstcmd_kind_from_shebang(scratch, resolved->path);
    }
    if (resolved->kind != WINXTERM_DSTCMD_EXEC_NATIVE &&
        resolved->kind != WINXTERM_DSTCMD_EXEC_UNSUPPORTED &&
        !winxterm_dstcmd_resolve_interpreter(resolved->kind,
                                             scratch,
                                             resolved->interpreter,
                                             WINXTERM_DSTCMD_PATH_CAPACITY)) {
        resolved->kind = WINXTERM_DSTCMD_EXEC_UNSUPPORTED;
    }
    return true;
}

static bool winxterm_dstcmd_try_candidate(WinxtermDstcmdScratch *scratch,
                                          const wchar_t *candidate,
                                          WinxtermDstcmdResolvedExec *resolved)
{
    return winxterm_dstcmd_file_exists(scratch, candidate) &&
           winxterm_dstcmd_complete_resolved(scratch, candidate, resolved);
}

static bool winxterm_dstcmd_join_path(const wchar_t *directory,
                                      const wchar_t *name,
                                      wchar_t *out,
                                      size_t out_count)
{
    if (directory == 0 || name == 0 || out == 0 || out_count == 0u) {
        return false;
    }
    size_t directory_length = wcslen(directory);
    size_t name_length = wcslen(name);
    bool needs_slash = directory_length != 0u && !winxterm_dstcmd_is_slash(directory[directory_length - 1u]);
    if (directory_length + (needs_slash ? 1u : 0u) + name_length >= out_count) {
        return false;
    }
    memcpy(out, directory, directory_length * sizeof(*out));
    size_t offset = directory_length;
    if (needs_slash) {
        out[offset++] = L'/';
    }
    memcpy(out + offset, name, (name_length + 1u) * sizeof(*out));
    for (size_t i = 0u; out[i] != L'\0'; ++i) {
        if (winxterm_dstcmd_is_slash(out[i])) {
            out[i] = L'/';
        }
    }
    return true;
}

static bool winxterm_dstcmd_visit_path_candidate(WinxtermDstcmdScratch *scratch,
                                                 const wchar_t *candidate,
                                                 WinxtermDstcmdExecPathMatchVisitor visitor,
                                                 void *context,
                                                 bool *stop)
{
    if (stop != 0) {
        *stop = false;
    }
    WinxtermDstcmdResolvedExec resolved;
    memset(&resolved, 0, sizeof(resolved));
    if (!winxterm_dstcmd_try_candidate(scratch, candidate, &resolved)) {
        return true;
    }
    if (visitor != 0 && !visitor(&resolved, context) && stop != 0) {
        *stop = true;
    }
    return true;
}

static bool winxterm_dstcmd_search_path_entry(const wchar_t *directory,
                                              const wchar_t *command,
                                              WinxtermDstcmdScratch *scratch,
                                              WinxtermDstcmdExecPathMatchVisitor visitor,
                                              void *context,
                                              bool *stop)
{
    static const wchar_t *extensions[] = {
        L"",
        L".exe",
        L".com",
        L".ps1",
        L".cmd",
        L".bat",
        L".sh",
    };
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(scratch);
    wchar_t *candidate = winxterm_dstcmd_scratch_alloc_path(scratch);
    wchar_t *name = winxterm_dstcmd_scratch_alloc_path(scratch);
    if (candidate == 0 || name == 0) {
        winxterm_dstcmd_scratch_rewind(scratch, mark);
        return false;
    }
    if (stop != 0) {
        *stop = false;
    }
    if (wcschr(command, L'.') != 0) {
        if (winxterm_dstcmd_join_path(directory, command, candidate, WINXTERM_DSTCMD_PATH_CAPACITY) &&
            !winxterm_dstcmd_visit_path_candidate(scratch, candidate, visitor, context, stop)) {
            winxterm_dstcmd_scratch_rewind(scratch, mark);
            return false;
        }
        winxterm_dstcmd_scratch_rewind(scratch, mark);
        return true;
    }
    for (size_t i = 0u; i < sizeof(extensions) / sizeof(extensions[0]) && (stop == 0 || !*stop); ++i) {
        int written = _snwprintf_s(name,
                                   WINXTERM_DSTCMD_PATH_CAPACITY,
                                   _TRUNCATE,
                                   L"%ls%ls",
                                   command,
                                   extensions[i]);
        if (written < 0) {
            continue;
        }
        if (winxterm_dstcmd_join_path(directory, name, candidate, WINXTERM_DSTCMD_PATH_CAPACITY) &&
            !winxterm_dstcmd_visit_path_candidate(scratch, candidate, visitor, context, stop)) {
            winxterm_dstcmd_scratch_rewind(scratch, mark);
            return false;
        }
    }
    winxterm_dstcmd_scratch_rewind(scratch, mark);
    return true;
}

bool winxterm_dstcmd_exec_for_each_path_match(const wchar_t *command,
                                              WinxtermDstcmdScratch *scratch,
                                              WinxtermDstcmdExecPathMatchVisitor visitor,
                                              void *context)
{
    if (command == 0 || scratch == 0 || visitor == 0) {
        return false;
    }
    DWORD needed = GetEnvironmentVariableW(L"PATH", 0, 0);
    if (needed == 0u) {
        return true;
    }
    wchar_t *path = (wchar_t *)calloc((size_t)needed + 1u, sizeof(*path));
    if (path == 0) {
        return false;
    }
    DWORD length = GetEnvironmentVariableW(L"PATH", path, needed + 1u);
    if (length == 0u || length > needed) {
        free(path);
        return false;
    }
    bool ok = true;
    bool stop = false;
    wchar_t *entry = path;
    while (entry != 0 && *entry != L'\0' && !stop) {
        wchar_t *separator = wcschr(entry, L';');
        if (separator != 0) {
            *separator = L'\0';
        }
        if (entry[0] != L'\0') {
            wchar_t *start = entry;
            wchar_t *end = entry + wcslen(entry);
            if (*start == L'"' && end > start + 1 && end[-1] == L'"') {
                ++start;
                end[-1] = L'\0';
            }
            if (!winxterm_dstcmd_search_path_entry(start, command, scratch, visitor, context, &stop)) {
                ok = false;
                break;
            }
        }
        entry = separator != 0 ? separator + 1 : 0;
    }
    free(path);
    return ok;
}

typedef struct WinxtermDstcmdFirstPathMatch {
    WinxtermDstcmdResolvedExec *resolved;
    bool found;
} WinxtermDstcmdFirstPathMatch;

static bool winxterm_dstcmd_first_path_match_visitor(const WinxtermDstcmdResolvedExec *resolved,
                                                     void *context)
{
    WinxtermDstcmdFirstPathMatch *match = (WinxtermDstcmdFirstPathMatch *)context;
    if (match == 0 || match->resolved == 0 || resolved == 0) {
        return false;
    }
    *match->resolved = *resolved;
    match->found = true;
    return false;
}

static bool winxterm_dstcmd_search_environment_path(const wchar_t *command,
                                                    WinxtermDstcmdScratch *scratch,
                                                    WinxtermDstcmdResolvedExec *resolved)
{
    WinxtermDstcmdFirstPathMatch match = {resolved, false};
    if (!winxterm_dstcmd_exec_for_each_path_match(command,
                                                  scratch,
                                                  winxterm_dstcmd_first_path_match_visitor,
                                                  &match)) {
        return false;
    }
    return match.found;
}

bool winxterm_dstcmd_exec_resolve_scratch(WinxtermDstcmdScratch *scratch,
                                          const wchar_t *cwd,
                                          const wchar_t *command,
                                          WinxtermDstcmdResolvedExec *resolved)
{
    if (scratch == 0 || resolved == 0) {
        return false;
    }
    memset(resolved, 0, sizeof(*resolved));
    resolved->kind = WINXTERM_DSTCMD_EXEC_NOT_FOUND;
    if (cwd == 0 || command == 0 || command[0] == L'\0') {
        return false;
    }
    if (winxterm_dstcmd_starts_dot_slash(command) || winxterm_dstcmd_command_has_path(command)) {
        WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(scratch);
        wchar_t *path = winxterm_dstcmd_scratch_alloc_path(scratch);
        if (path == 0 ||
            !winxterm_dstcmd_path_resolve_scratch(scratch,
                                                 cwd,
                                                 command,
                                                 path,
                                                 WINXTERM_DSTCMD_PATH_CAPACITY)) {
            resolved->kind = WINXTERM_DSTCMD_EXEC_UNSUPPORTED;
            winxterm_dstcmd_scratch_rewind(scratch, mark);
            return true;
        }
        if (!winxterm_dstcmd_complete_resolved(scratch, path, resolved)) {
            resolved->kind = WINXTERM_DSTCMD_EXEC_NOT_FOUND;
        }
        winxterm_dstcmd_scratch_rewind(scratch, mark);
        return true;
    }
    if (winxterm_dstcmd_search_environment_path(command, scratch, resolved)) {
        return true;
    }
    resolved->kind = WINXTERM_DSTCMD_EXEC_NOT_FOUND;
    return true;
}

bool winxterm_dstcmd_exec_resolve(const wchar_t *cwd,
                                  const wchar_t *command,
                                  WinxtermDstcmdResolvedExec *resolved)
{
    WinxtermDstcmdScratch scratch;
    winxterm_dstcmd_scratch_init(&scratch);
    bool ok = winxterm_dstcmd_exec_resolve_scratch(&scratch, cwd, command, resolved);
    winxterm_dstcmd_scratch_dispose(&scratch);
    return ok;
}

static void winxterm_dstcmd_free_argv(const wchar_t **argv)
{
    free((void *)argv);
}

static const wchar_t **winxterm_dstcmd_build_process_argv(const WinxtermDstcmdResolvedExec *resolved,
                                                          const WinxtermDstcmdArgv *argv,
                                                          int *argc)
{
    int fixed_count = 1;
    switch (resolved->kind) {
    case WINXTERM_DSTCMD_EXEC_POWERSHELL:
        fixed_count = 6;
        break;
    case WINXTERM_DSTCMD_EXEC_CMD:
        fixed_count = 3;
        break;
    case WINXTERM_DSTCMD_EXEC_BASH:
        fixed_count = 2;
        break;
    default:
        fixed_count = 1;
        break;
    }
    int total = fixed_count + (argv->count - 1);
    const wchar_t **process_argv = (const wchar_t **)calloc((size_t)total, sizeof(*process_argv));
    if (process_argv == 0) {
        return 0;
    }
    int offset = 0;
    switch (resolved->kind) {
    case WINXTERM_DSTCMD_EXEC_POWERSHELL:
        process_argv[offset++] = resolved->interpreter;
        process_argv[offset++] = L"-NoLogo";
        process_argv[offset++] = L"-ExecutionPolicy";
        process_argv[offset++] = L"Bypass";
        process_argv[offset++] = L"-File";
        break;
    case WINXTERM_DSTCMD_EXEC_CMD:
        process_argv[offset++] = resolved->interpreter;
        process_argv[offset++] = L"/c";
        break;
    case WINXTERM_DSTCMD_EXEC_BASH:
        process_argv[offset++] = resolved->interpreter;
        break;
    default:
        process_argv[offset++] = resolved->path;
        break;
    }
    if (resolved->kind != WINXTERM_DSTCMD_EXEC_NATIVE) {
        process_argv[offset++] = resolved->path;
    }
    for (int i = 1; i < argv->count; ++i) {
        process_argv[offset++] = argv->items[i];
    }
    *argc = offset;
    return process_argv;
}

static size_t winxterm_dstcmd_process_command_length(const wchar_t * const *argv, int argc)
{
    size_t total = 1u;
    for (int i = 0; i < argc; ++i) {
        if (argv[i] != 0) {
            total += wcslen(argv[i]) * 2u + 4u;
        }
    }
    return total;
}

static bool winxterm_dstcmd_process_append_quoted_arg(wchar_t *buffer,
                                                      size_t capacity,
                                                      size_t *offset,
                                                      const wchar_t *arg)
{
    if (buffer == 0 || offset == 0 || arg == 0 || *offset + 2u >= capacity) {
        return false;
    }
    if (*offset != 0u) {
        buffer[(*offset)++] = L' ';
    }
    buffer[(*offset)++] = L'"';
    for (const wchar_t *p = arg; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'"') {
            if (*offset + 1u >= capacity) {
                return false;
            }
            buffer[(*offset)++] = L'\\';
        }
        if (*offset + 1u >= capacity) {
            return false;
        }
        buffer[(*offset)++] = *p;
    }
    if (*offset + 1u >= capacity) {
        return false;
    }
    buffer[(*offset)++] = L'"';
    buffer[*offset] = L'\0';
    return true;
}

static wchar_t *winxterm_dstcmd_process_build_command_line(const wchar_t * const *argv, int argc)
{
    if (argv == 0 || argc <= 0) {
        return 0;
    }
    size_t capacity = winxterm_dstcmd_process_command_length(argv, argc);
    wchar_t *command_line = (wchar_t *)calloc(capacity, sizeof(*command_line));
    if (command_line == 0) {
        return 0;
    }
    size_t offset = 0u;
    for (int i = 0; i < argc; ++i) {
        if (!winxterm_dstcmd_process_append_quoted_arg(command_line, capacity, &offset, argv[i])) {
            free(command_line);
            return 0;
        }
    }
    return command_line;
}

static void winxterm_dstcmd_close_handle(HANDLE *handle)
{
    if (handle != 0 && *handle != 0 && *handle != INVALID_HANDLE_VALUE) {
        CloseHandle(*handle);
        *handle = 0;
    }
}

static bool winxterm_dstcmd_create_stdio_pipe(HANDLE *read_handle,
                                              HANDLE *write_handle)
{
    SECURITY_ATTRIBUTES security;
    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = FALSE;
    return CreatePipe(read_handle, write_handle, &security, 0) != 0;
}

static bool winxterm_dstcmd_duplicate_inheritable_handle(HANDLE source, HANDLE *out)
{
    if (out != 0) {
        *out = 0;
    }
    if (source == 0 || source == INVALID_HANDLE_VALUE || out == 0) {
        return false;
    }
    HANDLE process = GetCurrentProcess();
    return DuplicateHandle(process,
                           source,
                           process,
                           out,
                           0,
                           TRUE,
                           DUPLICATE_SAME_ACCESS) != 0;
}

typedef struct WinxtermDstcmdRedirectSink {
    const WinxtermDstcmdStreamEndpoint *endpoint;
    HANDLE file;
    uint64_t bytes_written;
    uint64_t started_ns;
    uint64_t finished_ns;
    bool write_failed;
    bool error_reported;
} WinxtermDstcmdRedirectSink;

static void winxterm_dstcmd_trim_win32_message(wchar_t *message)
{
    if (message == 0) {
        return;
    }
    size_t length = wcslen(message);
    while (length != 0u &&
           (message[length - 1u] == L'\r' ||
            message[length - 1u] == L'\n' ||
            message[length - 1u] == L' ' ||
            message[length - 1u] == L'\t')) {
        message[--length] = L'\0';
    }
}

static void winxterm_dstcmd_write_redirect_win32_error(WinxtermDstcmdShell *shell,
                                                       const wchar_t *operation,
                                                       const wchar_t *path,
                                                       DWORD code)
{
    wchar_t message[512];
    DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                  0,
                                  code,
                                  0,
                                  message,
                                  (DWORD)(sizeof(message) / sizeof(message[0])),
                                  0);
    if (length == 0u) {
        (void)_snwprintf_s(message,
                           sizeof(message) / sizeof(message[0]),
                           _TRUNCATE,
                           L"Windows error %lu",
                           (unsigned long)code);
    }
    winxterm_dstcmd_trim_win32_message(message);
    (void)winxterm_dstcmd_shell_write_widef(shell,
                                            L"dstcmd: redirect %ls '%ls' failed: %ls\r\n",
                                            operation != 0 ? operation : L"operation",
                                            path != 0 ? path : L"",
                                            message);
}

static bool winxterm_dstcmd_open_redirect_file(WinxtermDstcmdShell *shell,
                                               WinxtermDstcmdRedirectSink *sink)
{
    if (shell == 0 || sink == 0 || sink->endpoint == 0 ||
        sink->endpoint->kind != WINXTERM_DSTCMD_STREAM_REDIRECT) {
        return true;
    }

    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    WinxtermDstcmdWin32Path win32_path;
    if (!winxterm_dstcmd_path_prepare_win32_scratch(&shell->scratch, sink->endpoint->path, &win32_path)) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: redirect target path too long\r\n");
        winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
        return false;
    }

    DWORD disposition = sink->endpoint->append ? OPEN_ALWAYS : CREATE_ALWAYS;
    sink->file = CreateFileW(win32_path.syscall,
                             GENERIC_WRITE,
                             FILE_SHARE_READ,
                             0,
                             disposition,
                             FILE_ATTRIBUTE_NORMAL,
                             0);
    winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
    if (sink->file == INVALID_HANDLE_VALUE) {
        sink->file = 0;
        winxterm_dstcmd_write_redirect_win32_error(shell,
                                                   L"open",
                                                   sink->endpoint->path,
                                                   GetLastError());
        return false;
    }
    if (sink->endpoint->append) {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        if (!SetFilePointerEx(sink->file, zero, 0, FILE_END)) {
            DWORD error = GetLastError();
            winxterm_dstcmd_close_handle(&sink->file);
            winxterm_dstcmd_write_redirect_win32_error(shell,
                                                       L"seek",
                                                       sink->endpoint->path,
                                                       error);
            return false;
        }
    }
    sink->started_ns = winxterm_dstcmd_shell_timestamp_ns();
    return true;
}

static bool winxterm_dstcmd_write_redirect_file(WinxtermDstcmdShell *shell,
                                                WinxtermDstcmdRedirectSink *sink,
                                                const uint8_t *bytes,
                                                DWORD byte_count)
{
    if (sink == 0 || sink->endpoint == 0 || sink->endpoint->kind != WINXTERM_DSTCMD_STREAM_REDIRECT ||
        sink->write_failed || byte_count == 0u) {
        return true;
    }
    size_t offset = 0u;
    while (offset < (size_t)byte_count) {
        DWORD chunk = (size_t)byte_count - offset > 4096u ?
            4096u : (DWORD)((size_t)byte_count - offset);
        DWORD written = 0;
        DWORD error = ERROR_SUCCESS;
        bool ok = sink->file != 0 && sink->file != INVALID_HANDLE_VALUE;
        if (ok && !WriteFile(sink->file, bytes + offset, chunk, &written, 0)) {
            ok = false;
            error = GetLastError();
        }
        if (ok && written == 0u) {
            ok = false;
            error = ERROR_WRITE_FAULT;
        }
        if (!ok) {
            if (error == ERROR_SUCCESS) {
                error = ERROR_INVALID_HANDLE;
            }
            sink->write_failed = true;
            if (!sink->error_reported) {
                sink->error_reported = true;
                winxterm_dstcmd_write_redirect_win32_error(shell,
                                                           L"write",
                                                           sink->endpoint->path,
                                                           error);
            }
            return true;
        }
        sink->bytes_written += (uint64_t)written;
        offset += (size_t)written;
    }
    return true;
}

static bool winxterm_dstcmd_read_output_pipe_available(WinxtermDstcmdShell *shell,
                                                       HANDLE *pipe,
                                                       bool close_on_eof,
                                                       WinxtermDstcmdRedirectSink *redirect,
                                                       bool *read_any)
{
    if (read_any != 0) {
        *read_any = false;
    }
    if (pipe == 0 || *pipe == 0 || *pipe == INVALID_HANDLE_VALUE) {
        return true;
    }
    DWORD available = 0;
    if (!PeekNamedPipe(*pipe, 0, 0, 0, &available, 0)) {
        DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
            if (close_on_eof) {
                winxterm_dstcmd_close_handle(pipe);
            }
            return true;
        }
        return false;
    }
    if (available == 0u) {
        return true;
    }
    uint8_t buffer[4096];
    DWORD to_read = available > sizeof(buffer) ? (DWORD)sizeof(buffer) : available;
    DWORD bytes_read = 0;
    if (!ReadFile(*pipe, buffer, to_read, &bytes_read, 0)) {
        DWORD error = GetLastError();
        if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
            if (close_on_eof) {
                winxterm_dstcmd_close_handle(pipe);
            }
            return true;
        }
        return false;
    }
    if (bytes_read == 0u) {
        return true;
    }
    if (read_any != 0) {
        *read_any = true;
    }
    if (shell != 0 && shell->timing_verbose_active) {
        winxterm_diag_inc_u64(&shell->timing_diagnostics.external_read_calls);
        winxterm_diag_add_u64(&shell->timing_diagnostics.external_output_bytes, (uint64_t)bytes_read);
        winxterm_diag_add_u64(&shell->timing_diagnostics.total_output_bytes, (uint64_t)bytes_read);
    }
    if (redirect != 0 &&
        redirect->endpoint != 0 &&
        redirect->endpoint->kind == WINXTERM_DSTCMD_STREAM_REDIRECT) {
        if (!winxterm_dstcmd_write_redirect_file(shell, redirect, buffer, bytes_read)) {
            return false;
        }
        if (!redirect->endpoint->tee_to_terminal) {
            return true;
        }
    }
    return winxterm_dstcmd_shell_write_bytes(shell, buffer, bytes_read);
}

static bool winxterm_dstcmd_write_process_stdin(HANDLE input_write,
                                                const uint8_t *stdin_bytes,
                                                size_t stdin_count)
{
    size_t offset = 0u;
    while (offset < stdin_count) {
        DWORD chunk = stdin_count - offset > 4096u ? 4096u : (DWORD)(stdin_count - offset);
        DWORD written = 0;
        if (!WriteFile(input_write, stdin_bytes + offset, chunk, &written, 0)) {
            return false;
        }
        if (written == 0u) {
            return false;
        }
        offset += written;
    }
    return true;
}

typedef struct WinxtermDstcmdPreparedExec {
    wchar_t native_cwd[WINXTERM_DSTCMD_PATH_CAPACITY];
    const wchar_t **argv;
    int argc;
    wchar_t *command_line;
} WinxtermDstcmdPreparedExec;

static void winxterm_dstcmd_prepared_exec_dispose(WinxtermDstcmdPreparedExec *prepared)
{
    if (prepared == 0) {
        return;
    }
    winxterm_dstcmd_free_argv(prepared->argv);
    free(prepared->command_line);
    memset(prepared, 0, sizeof(*prepared));
}

static int winxterm_dstcmd_prepare_external(WinxtermDstcmdShell *shell,
                                            const WinxtermDstcmdArgv *argv,
                                            bool need_command_line,
                                            WinxtermDstcmdPreparedExec *prepared)
{
    if (prepared != 0) {
        memset(prepared, 0, sizeof(*prepared));
    }
    if (shell == 0 || argv == 0 || argv->count == 0 || prepared == 0) {
        return 0;
    }

    WinxtermDstcmdResolvedExec *resolved =
        (WinxtermDstcmdResolvedExec *)calloc(1u, sizeof(*resolved));
    wchar_t *native_cwd = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    if (resolved == 0 || native_cwd == 0) {
        free(resolved);
        (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: out of memory\r\n");
        return 1;
    }
    WinxtermCommandDiagnostics *diagnostics =
        shell->timing_verbose_active ? &shell->timing_diagnostics : 0;
    uint64_t resolve_start_ns = diagnostics != 0 ? winxterm_dstcmd_shell_timestamp_ns() : 0u;
    if (!winxterm_dstcmd_exec_resolve_scratch(&shell->scratch, shell->cwd, argv->items[0], resolved)) {
        if (diagnostics != 0) {
            uint64_t resolve_end_ns = winxterm_dstcmd_shell_timestamp_ns();
            winxterm_diag_add_u64(&diagnostics->program_resolve_ns,
                                  resolve_end_ns >= resolve_start_ns ?
                                    resolve_end_ns - resolve_start_ns : 0u);
        }
        (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: command resolution failed\r\n");
        free(resolved);
        return 1;
    }
    if (diagnostics != 0) {
        uint64_t resolve_end_ns = winxterm_dstcmd_shell_timestamp_ns();
        winxterm_diag_add_u64(&diagnostics->program_resolve_ns,
                              resolve_end_ns >= resolve_start_ns ?
                                resolve_end_ns - resolve_start_ns : 0u);
    }
    if (resolved->kind == WINXTERM_DSTCMD_EXEC_NOT_FOUND) {
        (void)winxterm_dstcmd_shell_write_widef(shell, L"%ls: command not found\r\n", argv->items[0]);
        free(resolved);
        return 127;
    }
    if (resolved->kind == WINXTERM_DSTCMD_EXEC_UNSUPPORTED) {
        (void)winxterm_dstcmd_shell_write_widef(shell, L"%ls: unsupported executable or script type\r\n", argv->items[0]);
        free(resolved);
        return 126;
    }
    if (!winxterm_dstcmd_path_to_native(resolved->path,
                                        resolved->path,
                                        WINXTERM_DSTCMD_PATH_CAPACITY) ||
        (resolved->interpreter[0] != L'\0' &&
         !winxterm_dstcmd_path_to_native(resolved->interpreter,
                                         resolved->interpreter,
                                         WINXTERM_DSTCMD_PATH_CAPACITY))) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: resolved path too long\r\n");
        free(resolved);
        return 1;
    }
    if (!winxterm_dstcmd_path_to_native(shell->cwd, native_cwd, WINXTERM_DSTCMD_PATH_CAPACITY)) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: current directory path too long\r\n");
        free(resolved);
        return 1;
    }
    int process_argc = 0;
    const wchar_t **process_argv = winxterm_dstcmd_build_process_argv(resolved, argv, &process_argc);
    if (process_argv == 0) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: out of memory\r\n");
        free(resolved);
        return 1;
    }
    wcscpy_s(prepared->native_cwd, WINXTERM_DSTCMD_PATH_CAPACITY, native_cwd);
    prepared->argv = process_argv;
    prepared->argc = process_argc;
    if (need_command_line) {
        prepared->command_line = winxterm_dstcmd_process_build_command_line(process_argv, process_argc);
        if (prepared->command_line == 0) {
            winxterm_dstcmd_prepared_exec_dispose(prepared);
            free(resolved);
            (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: out of memory\r\n");
            return 1;
        }
    }
    free(resolved);
    return 0;
}

typedef enum WinxtermDstcmdStageKind {
    WINXTERM_DSTCMD_STAGE_BUILTIN = 0,
    WINXTERM_DSTCMD_STAGE_EXTERNAL
} WinxtermDstcmdStageKind;

typedef struct WinxtermDstcmdPipelineEdge {
    HANDLE read_handle;
    HANDLE write_handle;
} WinxtermDstcmdPipelineEdge;

typedef struct WinxtermDstcmdStageRuntime {
    WinxtermDstcmdStageKind kind;
    PROCESS_INFORMATION process;
    HANDLE thread;
    HANDLE stdin_terminal_write;
    HANDLE stdout_terminal_read;
    HANDLE stderr_terminal_read;
    HANDLE builtin_input_handle;
    HANDLE builtin_output_handle;
    WinxtermDstcmdStreamEndpoint stdout_endpoint;
    WinxtermDstcmdRedirectSink stdout_redirect;
    WinxtermDstcmdShell builtin_shell;
    const WinxtermDstcmdArgv *argv;
    int status;
    bool started;
    bool exited;
} WinxtermDstcmdStageRuntime;

typedef struct WinxtermDstcmdBuiltinThreadContext {
    WinxtermDstcmdStageRuntime *runtime;
} WinxtermDstcmdBuiltinThreadContext;

static void winxterm_dstcmd_runtime_close_process(WinxtermDstcmdStageRuntime *runtime)
{
    if (runtime == 0) {
        return;
    }
    winxterm_dstcmd_close_handle(&runtime->process.hThread);
    winxterm_dstcmd_close_handle(&runtime->process.hProcess);
}

static void winxterm_dstcmd_stage_runtime_dispose(WinxtermDstcmdStageRuntime *runtime)
{
    if (runtime == 0) {
        return;
    }
    winxterm_dstcmd_runtime_close_process(runtime);
    if (runtime->thread != 0 && runtime->thread != INVALID_HANDLE_VALUE) {
        (void)WaitForSingleObject(runtime->thread, INFINITE);
    }
    winxterm_dstcmd_close_handle(&runtime->thread);
    winxterm_dstcmd_close_handle(&runtime->stdin_terminal_write);
    winxterm_dstcmd_close_handle(&runtime->stdout_terminal_read);
    winxterm_dstcmd_close_handle(&runtime->stderr_terminal_read);
    winxterm_dstcmd_close_handle(&runtime->builtin_input_handle);
    winxterm_dstcmd_close_handle(&runtime->builtin_output_handle);
    winxterm_dstcmd_close_handle(&runtime->stdout_redirect.file);
    winxterm_dstcmd_shell_dispose_aliases(&runtime->builtin_shell);
    winxterm_dstcmd_scratch_dispose(&runtime->builtin_shell.scratch);
}

static DWORD WINAPI winxterm_dstcmd_builtin_stage_thread(void *context)
{
    WinxtermDstcmdBuiltinThreadContext *thread_context = (WinxtermDstcmdBuiltinThreadContext *)context;
    if (thread_context == 0 || thread_context->runtime == 0) {
        free(thread_context);
        return 1;
    }
    WinxtermDstcmdStageRuntime *runtime = thread_context->runtime;
    runtime->status = winxterm_dstcmd_dispatch_builtin(&runtime->builtin_shell, runtime->argv);
    if (runtime->builtin_shell.stream_output_failed && runtime->status == 0) {
        runtime->status = 1;
    }
    winxterm_dstcmd_close_handle(&runtime->builtin_shell.stream_input_handle);
    runtime->builtin_input_handle = 0;
    winxterm_dstcmd_close_handle(&runtime->builtin_shell.stream_output_handle);
    runtime->builtin_output_handle = 0;
    free(thread_context);
    return (DWORD)runtime->status;
}

static void winxterm_dstcmd_init_isolated_shell(WinxtermDstcmdShell *stage_shell,
                                                WinxtermDstcmdShell *parent,
                                                HANDLE input_handle,
                                                HANDLE output_handle)
{
    memset(stage_shell, 0, sizeof(*stage_shell));
    winxterm_dstcmd_scratch_init(&stage_shell->scratch);
    if (parent == 0) {
        return;
    }
    stage_shell->shutdown_event = parent->shutdown_event;
    stage_shell->input_handle = parent->input_handle;
    stage_shell->output_handle = parent->output_handle;
    stage_shell->error_handle = parent->error_handle;
    stage_shell->last_status = parent->last_status;
    stage_shell->stream_input_handle = input_handle;
    stage_shell->stream_output_handle = output_handle;
    wcscpy_s(stage_shell->cwd, WINXTERM_DSTCMD_PATH_CAPACITY, parent->cwd);
    wcscpy_s(stage_shell->previous_cwd, WINXTERM_DSTCMD_PATH_CAPACITY, parent->previous_cwd);
    (void)winxterm_dstcmd_shell_clone_aliases(stage_shell, parent);
}

bool winxterm_dstcmd_exec_uses_interactive_client(const WinxtermDstcmdExecStage *stages,
                                                 size_t stage_count)
{
    if (stages == 0 || stage_count != 1u || stages[0].argv == 0 ||
        stages[0].argv->count <= 0 || stages[0].argv->items == 0 ||
        stages[0].argv->items[0] == 0) {
        return false;
    }
    if (stages[0].stdin_endpoint.kind != WINXTERM_DSTCMD_STREAM_TERMINAL ||
        stages[0].stdout_endpoint.kind != WINXTERM_DSTCMD_STREAM_TERMINAL ||
        stages[0].stderr_endpoint.kind != WINXTERM_DSTCMD_STREAM_TERMINAL) {
        return false;
    }
    return !winxterm_dstcmd_is_builtin(stages[0].argv->items[0]);
}

static bool winxterm_dstcmd_stage_can_run_direct_builtin(const WinxtermDstcmdExecStage *stage)
{
    return stage != 0 &&
           stage->argv != 0 &&
           stage->argv->count > 0 &&
           stage->argv->items != 0 &&
           stage->argv->items[0] != 0 &&
           winxterm_dstcmd_is_builtin(stage->argv->items[0]) &&
           stage->stdin_endpoint.kind == WINXTERM_DSTCMD_STREAM_TERMINAL &&
           stage->stdout_endpoint.kind == WINXTERM_DSTCMD_STREAM_TERMINAL &&
           stage->stderr_endpoint.kind == WINXTERM_DSTCMD_STREAM_TERMINAL;
}

static int winxterm_dstcmd_run_builtin_stage(WinxtermDstcmdShell *shell,
                                             const WinxtermDstcmdExecStage *stage)
{
    if (stage->isolate_shell_state) {
        WinxtermDstcmdShell *isolated = (WinxtermDstcmdShell *)calloc(1u, sizeof(*isolated));
        if (isolated == 0) {
            return 1;
        }
        winxterm_dstcmd_init_isolated_shell(isolated, shell, 0, 0);
        int status = winxterm_dstcmd_dispatch_builtin(isolated, stage->argv);
        winxterm_dstcmd_shell_dispose_aliases(isolated);
        winxterm_dstcmd_scratch_dispose(&isolated->scratch);
        free(isolated);
        return status;
    }
    return winxterm_dstcmd_dispatch_builtin(shell, stage->argv);
}

static int winxterm_dstcmd_run_standalone_external(WinxtermDstcmdShell *shell,
                                                  const WinxtermDstcmdExecStage *stage)
{
    int status = 1;
    bool child_mode_entered = false;
    bool title_changed = false;
    bool launch_failed = false;
    HANDLE child_stdin = 0;
    HANDLE child_stdout = 0;
    HANDLE child_stderr = 0;
    PROCESS_INFORMATION process;
    memset(&process, 0, sizeof(process));
    WinxtermDstcmdPreparedExec *prepared =
        (WinxtermDstcmdPreparedExec *)calloc(1u, sizeof(*prepared));
    if (prepared == 0) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: out of memory\r\n");
        return 1;
    }
    int prepare_status = winxterm_dstcmd_prepare_external(shell, stage->argv, true, prepared);
    if (prepare_status != 0) {
        status = prepare_status;
        goto cleanup;
    }

    bool handles_ok =
        winxterm_dstcmd_duplicate_inheritable_handle(shell->input_handle, &child_stdin) &&
        winxterm_dstcmd_duplicate_inheritable_handle(shell->output_handle, &child_stdout) &&
        winxterm_dstcmd_duplicate_inheritable_handle(shell->error_handle != 0 &&
                                                    shell->error_handle != INVALID_HANDLE_VALUE ?
                                                        shell->error_handle : shell->output_handle,
                                                    &child_stderr);
    if (!handles_ok) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: handle duplication failed\r\n");
        goto cleanup;
    }

    wchar_t client_title[WINXTERM_DSTCMD_LINE_CAPACITY];
    if (winxterm_dstcmd_exec_build_client_title(shell,
                                                stage->argv,
                                                client_title,
                                                WINXTERM_DSTCMD_LINE_CAPACITY)) {
        title_changed = winxterm_dstcmd_shell_set_title_wide(shell, client_title);
    }

    STARTUPINFOW startup;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_stdin;
    startup.hStdOutput = child_stdout;
    startup.hStdError = child_stderr;

    winxterm_dstcmd_shell_enter_foreground_child_mode(shell);
    child_mode_entered = true;
    BOOL created = CreateProcessW(0,
                                  prepared->command_line,
                                  0,
                                  0,
                                  TRUE,
                                  0,
                                  0,
                                  prepared->native_cwd,
                                  &startup,
                                  &process);
    if (!created) {
        launch_failed = true;
        goto cleanup;
    }

    DWORD wait_result = WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 1;
    if (wait_result == WAIT_OBJECT_0) {
        (void)GetExitCodeProcess(process.hProcess, &exit_code);
    }
    status = (int)exit_code;

cleanup:
    if (child_mode_entered) {
        winxterm_dstcmd_shell_enter_line_editor_mode(shell);
        child_mode_entered = false;
    }
    winxterm_dstcmd_close_handle(&child_stdin);
    winxterm_dstcmd_close_handle(&child_stdout);
    winxterm_dstcmd_close_handle(&child_stderr);
    winxterm_dstcmd_close_handle(&process.hThread);
    winxterm_dstcmd_close_handle(&process.hProcess);
    if (title_changed) {
        (void)winxterm_dstcmd_shell_update_cwd_title(shell);
    }
    if (launch_failed) {
        (void)winxterm_dstcmd_shell_write_widef(shell,
                                                L"%ls: launch failed\r\n",
                                                stage->argv->items[0]);
    }
    winxterm_dstcmd_prepared_exec_dispose(prepared);
    free(prepared);
    return status;
}

static bool winxterm_dstcmd_launch_stdio_external(WinxtermDstcmdShell *shell,
                                                 const WinxtermDstcmdExecStage *stage,
                                                 HANDLE child_stdin_source,
                                                 HANDLE child_stdout_target,
                                                 HANDLE child_stderr_target,
                                                 WinxtermDstcmdStageRuntime *runtime)
{
    WinxtermDstcmdPreparedExec *prepared =
        (WinxtermDstcmdPreparedExec *)calloc(1u, sizeof(*prepared));
    if (prepared == 0) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: out of memory\r\n");
        runtime->status = 1;
        return false;
    }
    int prepare_status = winxterm_dstcmd_prepare_external(shell, stage->argv, true, prepared);
    if (prepare_status != 0) {
        runtime->status = prepare_status;
        free(prepared);
        return false;
    }

    HANDLE child_stdin = 0;
    HANDLE child_stdout = 0;
    HANDLE child_stderr = 0;
    bool ok = winxterm_dstcmd_duplicate_inheritable_handle(child_stdin_source, &child_stdin) &&
              winxterm_dstcmd_duplicate_inheritable_handle(child_stdout_target, &child_stdout) &&
              winxterm_dstcmd_duplicate_inheritable_handle(child_stderr_target, &child_stderr);
    if (!ok) {
        winxterm_dstcmd_close_handle(&child_stdin);
        winxterm_dstcmd_close_handle(&child_stdout);
        winxterm_dstcmd_close_handle(&child_stderr);
        winxterm_dstcmd_prepared_exec_dispose(prepared);
        free(prepared);
        (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: handle duplication failed\r\n");
        runtime->status = 1;
        return false;
    }

    STARTUPINFOW startup;
    memset(&startup, 0, sizeof(startup));
    memset(&runtime->process, 0, sizeof(runtime->process));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = child_stdin;
    startup.hStdOutput = child_stdout;
    startup.hStdError = child_stderr;

    BOOL created = CreateProcessW(0,
                                  prepared->command_line,
                                  0,
                                  0,
                                  TRUE,
                                  CREATE_NO_WINDOW,
                                  0,
                                  prepared->native_cwd,
                                  &startup,
                                  &runtime->process);
    winxterm_dstcmd_close_handle(&child_stdin);
    winxterm_dstcmd_close_handle(&child_stdout);
    winxterm_dstcmd_close_handle(&child_stderr);
    winxterm_dstcmd_prepared_exec_dispose(prepared);
    free(prepared);
    if (!created) {
        (void)winxterm_dstcmd_shell_write_widef(shell,
                                                L"%ls: launch failed\r\n",
                                                stage->argv->items[0]);
        runtime->status = 1;
        return false;
    }
    runtime->kind = WINXTERM_DSTCMD_STAGE_EXTERNAL;
    runtime->started = true;
    return true;
}

static bool winxterm_dstcmd_prepare_builtin_stage(WinxtermDstcmdShell *shell,
                                                 const WinxtermDstcmdExecStage *stage,
                                                 HANDLE stdin_handle,
                                                 HANDLE stdout_handle,
                                                 WinxtermDstcmdStageRuntime *runtime)
{
    runtime->kind = WINXTERM_DSTCMD_STAGE_BUILTIN;
    runtime->argv = stage->argv;
    runtime->builtin_input_handle = stdin_handle;
    runtime->builtin_output_handle = stdout_handle;
    winxterm_dstcmd_init_isolated_shell(&runtime->builtin_shell,
                                        shell,
                                        stdin_handle,
                                        stdout_handle);
    return true;
}

static bool winxterm_dstcmd_start_prepared_builtin_stage(WinxtermDstcmdStageRuntime *runtime)
{
    WinxtermDstcmdBuiltinThreadContext *context =
        (WinxtermDstcmdBuiltinThreadContext *)calloc(1u, sizeof(*context));
    if (context == 0) {
        runtime->status = 1;
        return false;
    }
    context->runtime = runtime;
    runtime->thread = CreateThread(0, 0, winxterm_dstcmd_builtin_stage_thread, context, 0, 0);
    if (runtime->thread == 0) {
        free(context);
        runtime->status = 1;
        return false;
    }
    runtime->started = true;
    return true;
}

static bool winxterm_dstcmd_pipeline_all_exited(WinxtermDstcmdStageRuntime *runtimes,
                                                size_t count)
{
    for (size_t i = 0u; i < count; ++i) {
        if (runtimes[i].started && !runtimes[i].exited) {
            return false;
        }
    }
    return true;
}

static void winxterm_dstcmd_terminate_pipeline(WinxtermDstcmdStageRuntime *runtimes,
                                               size_t count)
{
    for (size_t i = 0u; i < count; ++i) {
        if (runtimes[i].process.hProcess != 0 &&
            runtimes[i].process.hProcess != INVALID_HANDLE_VALUE &&
            !runtimes[i].exited) {
            TerminateProcess(runtimes[i].process.hProcess, 1);
        }
        winxterm_dstcmd_close_handle(&runtimes[i].stdin_terminal_write);
    }
}

static bool winxterm_dstcmd_pump_terminal_input(WinxtermDstcmdShell *shell,
                                                WinxtermDstcmdStageRuntime *runtimes,
                                                size_t count,
                                                HANDLE input_write,
                                                bool *input_open,
                                                bool *cancelled)
{
    if (input_open == 0 || !*input_open || input_write == 0 || input_write == INVALID_HANDLE_VALUE) {
        return true;
    }
    uint8_t input[512];
    size_t input_count = 0u;
    while ((input_count = winxterm_dstcmd_shell_read_input(shell, input, sizeof(input), false)) != 0u) {
        size_t start = 0u;
        for (size_t i = 0u; i < input_count; ++i) {
            if (input[i] == 0x03u || input[i] == 0x04u) {
                if (i > start &&
                    !winxterm_dstcmd_write_process_stdin(input_write, input + start, i - start)) {
                    return false;
                }
                winxterm_dstcmd_close_handle(&input_write);
                *input_open = false;
                if (input[i] == 0x03u) {
                    *cancelled = true;
                    winxterm_dstcmd_terminate_pipeline(runtimes, count);
                }
                start = i + 1u;
                break;
            }
        }
        if (*input_open && input_count > start &&
            !winxterm_dstcmd_write_process_stdin(input_write, input + start, input_count - start)) {
            return false;
        }
        if (!*input_open) {
            break;
        }
    }
    return true;
}

static void winxterm_dstcmd_format_byte_quantity(uint64_t bytes, wchar_t *out, size_t out_count)
{
    if (out == 0 || out_count == 0u) {
        return;
    }
    if (bytes == 1u) {
        (void)wcscpy_s(out, out_count, L"1 byte");
        return;
    }
    if (bytes < 1024u) {
        (void)_snwprintf_s(out,
                           out_count,
                           _TRUNCATE,
                           L"%llu bytes",
                           (unsigned long long)bytes);
        return;
    }
    static const wchar_t *units[] = { L"KiB", L"MiB", L"GiB", L"TiB" };
    double value = (double)bytes / 1024.0;
    size_t unit_index = 0u;
    while (value >= 1024.0 && unit_index + 1u < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        ++unit_index;
    }
    (void)_snwprintf_s(out, out_count, _TRUNCATE, L"%.2f %ls", value, units[unit_index]);
}

static void winxterm_dstcmd_format_duration(uint64_t elapsed_ns, wchar_t *out, size_t out_count)
{
    if (out == 0 || out_count == 0u) {
        return;
    }
    if (elapsed_ns < 1000000000ull) {
        uint64_t milliseconds = (elapsed_ns + 500000ull) / 1000000ull;
        if (milliseconds == 0u && elapsed_ns != 0u) {
            milliseconds = 1u;
        }
        (void)_snwprintf_s(out,
                           out_count,
                           _TRUNCATE,
                           L"%llu ms",
                           (unsigned long long)milliseconds);
        return;
    }
    double seconds = (double)elapsed_ns / 1000000000.0;
    (void)_snwprintf_s(out,
                       out_count,
                       _TRUNCATE,
                       seconds < 10.0 ? L"%.2f s" : L"%.1f s",
                       seconds);
}

static void winxterm_dstcmd_format_speed(uint64_t bytes,
                                         uint64_t elapsed_ns,
                                         wchar_t *out,
                                         size_t out_count)
{
    if (out == 0 || out_count == 0u) {
        return;
    }
    if (elapsed_ns == 0u) {
        elapsed_ns = 1u;
    }
    double bytes_per_second = ((double)bytes * 1000000000.0) / (double)elapsed_ns;
    if (bytes_per_second < 1024.0) {
        (void)_snwprintf_s(out, out_count, _TRUNCATE, L"%.0f bytes/s", bytes_per_second);
        return;
    }
    static const wchar_t *units[] = { L"KiB/s", L"MiB/s", L"GiB/s", L"TiB/s" };
    double value = bytes_per_second / 1024.0;
    size_t unit_index = 0u;
    while (value >= 1024.0 && unit_index + 1u < sizeof(units) / sizeof(units[0])) {
        value /= 1024.0;
        ++unit_index;
    }
    (void)_snwprintf_s(out, out_count, _TRUNCATE, L"%.2f %ls", value, units[unit_index]);
}

static bool winxterm_dstcmd_write_redirect_summary(WinxtermDstcmdShell *shell,
                                                   const WinxtermDstcmdRedirectSink *sink)
{
    if (sink == 0 || sink->endpoint == 0 || sink->endpoint->kind != WINXTERM_DSTCMD_STREAM_REDIRECT) {
        return true;
    }
    uint64_t elapsed_ns = sink->finished_ns >= sink->started_ns ?
        sink->finished_ns - sink->started_ns : 0u;
    wchar_t bytes[64];
    wchar_t duration[64];
    wchar_t speed[64];
    winxterm_dstcmd_format_byte_quantity(sink->bytes_written, bytes, sizeof(bytes) / sizeof(bytes[0]));
    winxterm_dstcmd_format_duration(elapsed_ns, duration, sizeof(duration) / sizeof(duration[0]));
    winxterm_dstcmd_format_speed(sink->bytes_written, elapsed_ns, speed, sizeof(speed) / sizeof(speed[0]));
    return winxterm_dstcmd_shell_write_widef(shell,
                                            L"%ls written in %ls at %ls\r\n",
                                            bytes,
                                            duration,
                                            speed);
}

static int winxterm_dstcmd_run_stdio_job(WinxtermDstcmdShell *shell,
                                         const WinxtermDstcmdExecStage *stages,
                                         size_t stage_count)
{
    WinxtermDstcmdPipelineEdge *edges = 0;
    WinxtermDstcmdStageRuntime *runtimes = 0;
    int status = 0;
    bool cancelled = false;

    if (stage_count == 0u) {
        return 0;
    }
    runtimes = (WinxtermDstcmdStageRuntime *)calloc(stage_count, sizeof(*runtimes));
    if (runtimes == 0) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: out of memory\r\n");
        return 1;
    }
    if (stage_count > 1u) {
        edges = (WinxtermDstcmdPipelineEdge *)calloc(stage_count - 1u, sizeof(*edges));
        if (edges == 0) {
            free(runtimes);
            (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: out of memory\r\n");
            return 1;
        }
        for (size_t i = 0u; i + 1u < stage_count; ++i) {
            if (!winxterm_dstcmd_create_stdio_pipe(&edges[i].read_handle, &edges[i].write_handle)) {
                (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: pipe creation failed\r\n");
                status = 1;
                goto cleanup;
            }
        }
    }

    for (size_t i = 0u; i < stage_count; ++i) {
        HANDLE stdin_read = 0;
        HANDLE stdout_write = 0;
        HANDLE stderr_write = 0;
        bool stdin_from_terminal = stages[i].stdin_endpoint.kind == WINXTERM_DSTCMD_STREAM_TERMINAL;
        bool stdout_to_parent = stages[i].stdout_endpoint.kind == WINXTERM_DSTCMD_STREAM_TERMINAL ||
                                stages[i].stdout_endpoint.kind == WINXTERM_DSTCMD_STREAM_REDIRECT;
        bool builtin = winxterm_dstcmd_is_builtin(stages[i].argv->items[0]);
        runtimes[i].stdout_endpoint = stages[i].stdout_endpoint;
        if (runtimes[i].stdout_endpoint.kind == WINXTERM_DSTCMD_STREAM_REDIRECT) {
            runtimes[i].stdout_redirect.endpoint = &runtimes[i].stdout_endpoint;
            if (!winxterm_dstcmd_open_redirect_file(shell, &runtimes[i].stdout_redirect)) {
                status = 1;
                goto cleanup;
            }
        }

        if (stdin_from_terminal) {
            if (!winxterm_dstcmd_create_stdio_pipe(&stdin_read, &runtimes[i].stdin_terminal_write)) {
                (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: pipe creation failed\r\n");
                status = 1;
                goto cleanup;
            }
        } else {
            stdin_read = edges[i - 1u].read_handle;
        }

        if (stdout_to_parent) {
            if (!winxterm_dstcmd_create_stdio_pipe(&runtimes[i].stdout_terminal_read, &stdout_write)) {
                (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: pipe creation failed\r\n");
                status = 1;
                goto cleanup;
            }
        } else {
            stdout_write = edges[i].write_handle;
        }

        if (!builtin &&
            !winxterm_dstcmd_create_stdio_pipe(&runtimes[i].stderr_terminal_read, &stderr_write)) {
            (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: pipe creation failed\r\n");
            status = 1;
            goto cleanup;
        }

        if (builtin) {
            if (!winxterm_dstcmd_prepare_builtin_stage(shell,
                                                       stages + i,
                                                       stdin_read,
                                                       stdout_write,
                                                       runtimes + i)) {
                (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: builtin stage failed\r\n");
                status = 1;
                goto cleanup;
            }
            if (!stdin_from_terminal) {
                edges[i - 1u].read_handle = 0;
            }
            if (!stdout_to_parent) {
                edges[i].write_handle = 0;
            }
            continue;
        }

        if (!winxterm_dstcmd_launch_stdio_external(shell,
                                                  stages + i,
                                                  stdin_read,
                                                  stdout_write,
                                                  stderr_write,
                                                  runtimes + i)) {
            status = runtimes[i].status != 0 ? runtimes[i].status : 1;
            goto cleanup;
        }

        if (stdin_from_terminal) {
            winxterm_dstcmd_close_handle(&stdin_read);
        }
        if (stdout_to_parent) {
            winxterm_dstcmd_close_handle(&stdout_write);
        }
        winxterm_dstcmd_close_handle(&stderr_write);
    }

    if (edges != 0) {
        for (size_t i = 0u; i + 1u < stage_count; ++i) {
            bool keep_write = runtimes[i].kind == WINXTERM_DSTCMD_STAGE_BUILTIN &&
                              runtimes[i].builtin_output_handle == edges[i].write_handle;
            if (!keep_write) {
                winxterm_dstcmd_close_handle(&edges[i].write_handle);
            }
            winxterm_dstcmd_close_handle(&edges[i].read_handle);
        }
    }

    for (size_t i = 0u; i < stage_count; ++i) {
        if (runtimes[i].kind == WINXTERM_DSTCMD_STAGE_BUILTIN &&
            !winxterm_dstcmd_start_prepared_builtin_stage(runtimes + i)) {
            (void)winxterm_dstcmd_shell_write_wide(shell, L"dstcmd: builtin stage failed\r\n");
            status = 1;
            goto cleanup;
        }
    }

    while (!winxterm_dstcmd_pipeline_all_exited(runtimes, stage_count)) {
        if (shell->shutdown_event != 0 &&
            WaitForSingleObject(shell->shutdown_event, 0) == WAIT_OBJECT_0) {
            cancelled = true;
            winxterm_dstcmd_terminate_pipeline(runtimes, stage_count);
        }

        for (size_t i = 0u; i < stage_count; ++i) {
            bool read_any = false;
            WinxtermDstcmdRedirectSink *redirect =
                runtimes[i].stdout_endpoint.kind == WINXTERM_DSTCMD_STREAM_REDIRECT ?
                    &runtimes[i].stdout_redirect : 0;
            if (!winxterm_dstcmd_read_output_pipe_available(shell,
                                                            &runtimes[i].stdout_terminal_read,
                                                            false,
                                                            redirect,
                                                            &read_any) ||
                !winxterm_dstcmd_read_output_pipe_available(shell,
                                                            &runtimes[i].stderr_terminal_read,
                                                            false,
                                                            0,
                                                            &read_any)) {
                status = 1;
                goto cleanup;
            }
        }

        for (size_t i = 0u; i < stage_count; ++i) {
            if (!runtimes[i].started || runtimes[i].exited) {
                continue;
            }
            if (runtimes[i].kind == WINXTERM_DSTCMD_STAGE_EXTERNAL) {
                DWORD wait_result = WaitForSingleObject(runtimes[i].process.hProcess, 0);
                if (wait_result == WAIT_OBJECT_0) {
                    bool drain_any = false;
                    do {
                        drain_any = false;
                        WinxtermDstcmdRedirectSink *redirect =
                            runtimes[i].stdout_endpoint.kind == WINXTERM_DSTCMD_STREAM_REDIRECT ?
                                &runtimes[i].stdout_redirect : 0;
                        if (!winxterm_dstcmd_read_output_pipe_available(shell,
                                                                        &runtimes[i].stdout_terminal_read,
                                                                        true,
                                                                        redirect,
                                                                        &drain_any) ||
                            !winxterm_dstcmd_read_output_pipe_available(shell,
                                                                        &runtimes[i].stderr_terminal_read,
                                                                        true,
                                                                        0,
                                                                        &drain_any)) {
                            status = 1;
                            goto cleanup;
                        }
                    } while (drain_any);
                    DWORD exit_code = 1;
                    GetExitCodeProcess(runtimes[i].process.hProcess, &exit_code);
                    runtimes[i].status = (int)exit_code;
                    runtimes[i].exited = true;
                    winxterm_dstcmd_runtime_close_process(runtimes + i);
                    winxterm_dstcmd_close_handle(&runtimes[i].stdin_terminal_write);
                } else if (wait_result == WAIT_FAILED) {
                    runtimes[i].status = 1;
                    runtimes[i].exited = true;
                }
            } else {
                DWORD wait_result = WaitForSingleObject(runtimes[i].thread, 0);
                if (wait_result == WAIT_OBJECT_0) {
                    bool drain_any = false;
                    do {
                        drain_any = false;
                        WinxtermDstcmdRedirectSink *redirect =
                            runtimes[i].stdout_endpoint.kind == WINXTERM_DSTCMD_STREAM_REDIRECT ?
                                &runtimes[i].stdout_redirect : 0;
                        if (!winxterm_dstcmd_read_output_pipe_available(shell,
                                                                        &runtimes[i].stdout_terminal_read,
                                                                        true,
                                                                        redirect,
                                                                        &drain_any) ||
                            !winxterm_dstcmd_read_output_pipe_available(shell,
                                                                        &runtimes[i].stderr_terminal_read,
                                                                        true,
                                                                        0,
                                                                        &drain_any)) {
                            status = 1;
                            goto cleanup;
                        }
                    } while (drain_any);
                    DWORD thread_status = 1;
                    GetExitCodeThread(runtimes[i].thread, &thread_status);
                    runtimes[i].status = (int)thread_status;
                    runtimes[i].exited = true;
                    winxterm_dstcmd_close_handle(&runtimes[i].thread);
                    winxterm_dstcmd_close_handle(&runtimes[i].stdin_terminal_write);
                }
            }
        }

        if (stage_count != 0u &&
            runtimes[0].stdin_terminal_write != 0 &&
            runtimes[0].stdin_terminal_write != INVALID_HANDLE_VALUE) {
            bool input_open = true;
            if (!winxterm_dstcmd_pump_terminal_input(shell,
                                                     runtimes,
                                                     stage_count,
                                                     runtimes[0].stdin_terminal_write,
                                                     &input_open,
                                                     &cancelled)) {
                status = 1;
                goto cleanup;
            }
            if (!input_open) {
                runtimes[0].stdin_terminal_write = 0;
            }
        }

        HANDLE waits[2];
        DWORD wait_count = 0u;
        if (shell->shutdown_event != 0) {
            waits[wait_count++] = shell->shutdown_event;
        }
        if (wait_count != 0u) {
            (void)WaitForMultipleObjects(wait_count, waits, FALSE, 10);
        } else {
            Sleep(10);
        }
    }

    status = cancelled ? 1 : runtimes[stage_count - 1u].status;
    for (size_t i = 0u; i < stage_count; ++i) {
        WinxtermDstcmdRedirectSink *redirect = &runtimes[i].stdout_redirect;
        if (redirect->endpoint == 0 ||
            redirect->endpoint->kind != WINXTERM_DSTCMD_STREAM_REDIRECT) {
            continue;
        }
        redirect->finished_ns = winxterm_dstcmd_shell_timestamp_ns();
        winxterm_dstcmd_close_handle(&redirect->file);
        if (redirect->write_failed) {
            if (status == 0) {
                status = 1;
            }
            continue;
        }
        if (!cancelled && !winxterm_dstcmd_write_redirect_summary(shell, redirect)) {
            status = 1;
        }
    }

cleanup:
    if (status != 0) {
        winxterm_dstcmd_terminate_pipeline(runtimes, stage_count);
    }
    if (edges != 0) {
        for (size_t i = 0u; i + 1u < stage_count; ++i) {
            winxterm_dstcmd_close_handle(&edges[i].read_handle);
            winxterm_dstcmd_close_handle(&edges[i].write_handle);
        }
        free(edges);
    }
    if (runtimes != 0) {
        for (size_t i = 0u; i < stage_count; ++i) {
            winxterm_dstcmd_stage_runtime_dispose(runtimes + i);
        }
        free(runtimes);
    }
    return status;
}

int winxterm_dstcmd_exec_run(WinxtermDstcmdShell *shell,
                             const WinxtermDstcmdExecStage *stages,
                             size_t stage_count)
{
    if (shell == 0 || stages == 0 || stage_count == 0u) {
        return 0;
    }
    uint64_t start_ns = winxterm_dstcmd_shell_timestamp_ns();
    int status = 0;
    if (stage_count == 1u) {
        if (stages[0].argv == 0 || stages[0].argv->count <= 0 ||
            stages[0].argv->items == 0 || stages[0].argv->items[0] == 0) {
            status = 0;
        } else if (winxterm_dstcmd_exec_uses_interactive_client(stages, stage_count)) {
            status = winxterm_dstcmd_run_standalone_external(shell, stages);
        } else if (winxterm_dstcmd_stage_can_run_direct_builtin(stages)) {
            status = winxterm_dstcmd_run_builtin_stage(shell, stages);
        } else {
            status = winxterm_dstcmd_run_stdio_job(shell, stages, stage_count);
            winxterm_dstcmd_shell_enter_line_editor_mode(shell);
        }
    } else {
        status = winxterm_dstcmd_run_stdio_job(shell, stages, stage_count);
        winxterm_dstcmd_shell_enter_line_editor_mode(shell);
    }
    WinxtermCommandDiagnostics *diagnostics =
        shell->timing_verbose_active ? &shell->timing_diagnostics : 0;
    if (diagnostics != 0) {
        uint64_t end_ns = winxterm_dstcmd_shell_timestamp_ns();
        winxterm_diag_add_u64(&diagnostics->exec_run_ns,
                              end_ns >= start_ns ? end_ns - start_ns : 0u);
    }
    return status;
}
