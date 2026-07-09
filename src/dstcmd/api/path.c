#include "dstcmd/api/path.h"

#include "dstcmd/winxterm_dstcmd.h"

#include <string.h>
#include <windows.h>

static bool winxterm_dstcmd_is_slash(wchar_t ch)
{
    return ch == L'\\' || ch == L'/';
}

static bool winxterm_dstcmd_copy_with_separator(const wchar_t *path,
                                                wchar_t *out,
                                                size_t out_count,
                                                wchar_t separator)
{
    if (out == 0 || out_count == 0u || path == 0) {
        return false;
    }
    size_t length = wcslen(path);
    if (length >= out_count) {
        return false;
    }
    for (size_t i = 0u; i <= length; ++i) {
        out[i] = winxterm_dstcmd_is_slash(path[i]) ? separator : path[i];
    }
    return true;
}

static bool winxterm_dstcmd_has_drive_root(const wchar_t *path)
{
    return path != 0 &&
           ((path[0] >= L'a' && path[0] <= L'z') || (path[0] >= L'A' && path[0] <= L'Z')) &&
           path[1] == L':' &&
           winxterm_dstcmd_is_slash(path[2]);
}

static bool winxterm_dstcmd_is_unc_rooted(const wchar_t *path)
{
    return path != 0 && winxterm_dstcmd_is_slash(path[0]) && winxterm_dstcmd_is_slash(path[1]);
}

static bool winxterm_dstcmd_starts_with(const wchar_t *text, const wchar_t *prefix)
{
    if (text == 0 || prefix == 0) {
        return false;
    }
    while (*prefix != L'\0') {
        if (*text++ != *prefix++) {
            return false;
        }
    }
    return true;
}

static bool winxterm_dstcmd_copy_normalized(wchar_t *out, size_t out_count, const wchar_t *path)
{
    return winxterm_dstcmd_path_to_display(path, out, out_count);
}

static bool winxterm_dstcmd_join_raw(const wchar_t *left,
                                     const wchar_t *right,
                                     wchar_t *out,
                                     size_t out_count)
{
    if (left == 0 || right == 0 || out == 0 || out_count == 0u) {
        return false;
    }
    size_t left_length = wcslen(left);
    size_t right_length = wcslen(right);
    bool need_slash = left_length != 0u && !winxterm_dstcmd_is_slash(left[left_length - 1u]);
    if (left_length + (need_slash ? 1u : 0u) + right_length >= out_count) {
        return false;
    }
    memcpy(out, left, left_length * sizeof(*out));
    size_t offset = left_length;
    if (need_slash) {
        out[offset++] = L'/';
    }
    memcpy(out + offset, right, (right_length + 1u) * sizeof(*out));
    for (size_t i = 0u; out[i] != L'\0'; ++i) {
        if (winxterm_dstcmd_is_slash(out[i])) {
            out[i] = L'/';
        }
    }
    return true;
}

bool winxterm_dstcmd_path_resolve_full_scratch(WinxtermDstcmdScratch *scratch,
                                               const wchar_t *cwd,
                                               const wchar_t *operand,
                                               wchar_t *out,
                                               size_t out_count)
{
    if (scratch == 0 || cwd == 0 || out == 0 || out_count == 0u) {
        return false;
    }

    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(scratch);
    wchar_t *combined = winxterm_dstcmd_scratch_alloc_path(scratch);
    wchar_t *native_combined = winxterm_dstcmd_scratch_alloc_path(scratch);
    if (combined == 0 || native_combined == 0) {
        winxterm_dstcmd_scratch_rewind(scratch, mark);
        return false;
    }

    bool ok = false;
    if (operand == 0 || operand[0] == L'\0') {
        if (!winxterm_dstcmd_copy_normalized(combined, WINXTERM_DSTCMD_PATH_CAPACITY, cwd)) {
            goto cleanup;
        }
    } else if (winxterm_dstcmd_has_drive_root(operand) || winxterm_dstcmd_is_unc_rooted(operand)) {
        if (!winxterm_dstcmd_copy_normalized(combined, WINXTERM_DSTCMD_PATH_CAPACITY, operand)) {
            goto cleanup;
        }
    } else if (winxterm_dstcmd_is_slash(operand[0]) && cwd[0] != L'\0' && cwd[1] == L':') {
        int written = _snwprintf_s(combined,
                                   WINXTERM_DSTCMD_PATH_CAPACITY,
                                   _TRUNCATE,
                                   L"%c:%ls",
                                   cwd[0],
                                   operand);
        if (written < 0) {
            goto cleanup;
        }
        for (size_t i = 0u; combined[i] != L'\0'; ++i) {
            if (winxterm_dstcmd_is_slash(combined[i])) {
                combined[i] = L'/';
            }
        }
    } else {
        if (!winxterm_dstcmd_join_raw(cwd, operand, combined, WINXTERM_DSTCMD_PATH_CAPACITY)) {
            goto cleanup;
        }
    }

    if (!winxterm_dstcmd_path_to_native(combined, native_combined, WINXTERM_DSTCMD_PATH_CAPACITY)) {
        goto cleanup;
    }
    DWORD needed = GetFullPathNameW(native_combined, 0u, 0, 0);
    if (needed == 0u || needed >= WINXTERM_DSTCMD_PATH_CAPACITY || needed >= out_count) {
        goto cleanup;
    }
    wchar_t *resolved = winxterm_dstcmd_scratch_alloc_wchars(scratch, (size_t)needed + 1u);
    if (resolved == 0) {
        goto cleanup;
    }
    DWORD length = GetFullPathNameW(native_combined, needed + 1u, resolved, 0);
    if (length == 0u || length >= needed + 1u || length >= out_count) {
        goto cleanup;
    }
    ok = winxterm_dstcmd_path_to_display(resolved, out, out_count);

cleanup:
    winxterm_dstcmd_scratch_rewind(scratch, mark);
    return ok;
}

bool winxterm_dstcmd_path_resolve_scratch(WinxtermDstcmdScratch *scratch,
                                          const wchar_t *cwd,
                                          const wchar_t *operand,
                                          wchar_t *out,
                                          size_t out_count)
{
    return winxterm_dstcmd_path_resolve_full_scratch(scratch, cwd, operand, out, out_count);
}

bool winxterm_dstcmd_path_resolve(const wchar_t *cwd,
                                  const wchar_t *operand,
                                  wchar_t *out,
                                  size_t out_count)
{
    WinxtermDstcmdScratch scratch;
    winxterm_dstcmd_scratch_init(&scratch);
    bool ok = winxterm_dstcmd_path_resolve_scratch(&scratch, cwd, operand, out, out_count);
    winxterm_dstcmd_scratch_dispose(&scratch);
    return ok;
}

bool winxterm_dstcmd_path_prepare_win32_scratch(WinxtermDstcmdScratch *scratch,
                                                const wchar_t *path,
                                                WinxtermDstcmdWin32Path *out)
{
    if (scratch == 0 || path == 0 || out == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    memset(out, 0, sizeof(*out));

    wchar_t *native_path = winxterm_dstcmd_scratch_alloc_path(scratch);
    wchar_t *syscall_path = winxterm_dstcmd_scratch_alloc_path(scratch);
    if (native_path == 0 || syscall_path == 0) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }
    if (!winxterm_dstcmd_path_to_native(path, native_path, WINXTERM_DSTCMD_PATH_CAPACITY)) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return false;
    }

    const wchar_t *prefix = 0;
    const wchar_t *suffix = native_path;
    bool extended = false;
    if (winxterm_dstcmd_starts_with(native_path, L"\\\\?\\") ||
        winxterm_dstcmd_starts_with(native_path, L"\\\\.\\")) {
        prefix = L"";
        extended = winxterm_dstcmd_starts_with(native_path, L"\\\\?\\");
    } else if (winxterm_dstcmd_has_drive_root(native_path)) {
        prefix = L"\\\\?\\";
        extended = true;
    } else if (winxterm_dstcmd_is_unc_rooted(native_path)) {
        prefix = L"\\\\?\\UNC\\";
        suffix = native_path + 2u;
        extended = true;
    } else {
        SetLastError(ERROR_PATH_NOT_FOUND);
        return false;
    }

    size_t prefix_length = wcslen(prefix);
    size_t suffix_length = wcslen(suffix);
    if (prefix_length + suffix_length >= WINXTERM_DSTCMD_PATH_CAPACITY) {
        SetLastError(ERROR_FILENAME_EXCED_RANGE);
        return false;
    }
    memcpy(syscall_path, prefix, prefix_length * sizeof(*syscall_path));
    memcpy(syscall_path + prefix_length, suffix, (suffix_length + 1u) * sizeof(*syscall_path));

    out->display = path;
    out->native = native_path;
    out->syscall = syscall_path;
    out->extended = extended;
    return true;
}

bool winxterm_dstcmd_path_from_win32_display(const wchar_t *path,
                                             wchar_t *out,
                                             size_t out_count)
{
    if (path == 0 || out == 0 || out_count == 0u) {
        return false;
    }
    wchar_t *prefixed = out;
    if (winxterm_dstcmd_starts_with(path, L"\\\\?\\UNC\\")) {
        const wchar_t *suffix = path + 8u;
        size_t suffix_length = wcslen(suffix);
        if (suffix_length + 2u >= out_count) {
            return false;
        }
        out[0] = L'\\';
        out[1] = L'\\';
        memcpy(out + 2u, suffix, (suffix_length + 1u) * sizeof(*out));
    } else if (winxterm_dstcmd_starts_with(path, L"\\\\?\\")) {
        prefixed = (wchar_t *)(path + 4u);
    } else {
        prefixed = (wchar_t *)path;
    }
    return winxterm_dstcmd_path_to_display(prefixed, out, out_count);
}

bool winxterm_dstcmd_path_get_info_scratch(WinxtermDstcmdScratch *scratch,
                                           const wchar_t *path,
                                           WinxtermDstcmdPathInfo *info)
{
    if (info != 0) {
        memset(info, 0, sizeof(*info));
    }
    if (scratch == 0 || path == 0 || info == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(scratch);
    WinxtermDstcmdWin32Path win32_path;
    if (!winxterm_dstcmd_path_prepare_win32_scratch(scratch, path, &win32_path)) {
        info->error = GetLastError();
        winxterm_dstcmd_scratch_rewind(scratch, mark);
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(win32_path.syscall, GetFileExInfoStandard, &data)) {
        info->error = GetLastError();
        winxterm_dstcmd_scratch_rewind(scratch, mark);
        return false;
    }
    info->attributes = data.dwFileAttributes;
    info->size = ((ULONGLONG)data.nFileSizeHigh << 32) | (ULONGLONG)data.nFileSizeLow;
    info->write_time = data.ftLastWriteTime;
    info->exists = true;
    info->error = ERROR_SUCCESS;
    winxterm_dstcmd_scratch_rewind(scratch, mark);
    return true;
}

bool winxterm_dstcmd_path_is_directory_scratch(WinxtermDstcmdScratch *scratch,
                                               const wchar_t *path)
{
    WinxtermDstcmdPathInfo info;
    return winxterm_dstcmd_path_get_info_scratch(scratch, path, &info) &&
           (info.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
}

bool winxterm_dstcmd_path_is_directory(const wchar_t *path)
{
    WinxtermDstcmdScratch scratch;
    winxterm_dstcmd_scratch_init(&scratch);
    bool ok = winxterm_dstcmd_path_is_directory_scratch(&scratch, path);
    winxterm_dstcmd_scratch_dispose(&scratch);
    return ok;
}

bool winxterm_dstcmd_path_append_child(const wchar_t *directory,
                                       const wchar_t *name,
                                       wchar_t *out,
                                       size_t out_count)
{
    return winxterm_dstcmd_join_raw(directory, name, out, out_count);
}

bool winxterm_dstcmd_path_set_attributes_scratch(WinxtermDstcmdScratch *scratch,
                                                 const wchar_t *path,
                                                 DWORD attributes)
{
    if (scratch == 0 || path == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(scratch);
    WinxtermDstcmdWin32Path win32_path;
    bool ok = winxterm_dstcmd_path_prepare_win32_scratch(scratch, path, &win32_path) &&
              SetFileAttributesW(win32_path.syscall, attributes) != 0;
    winxterm_dstcmd_scratch_rewind(scratch, mark);
    return ok;
}

bool winxterm_dstcmd_path_create_directory_scratch(WinxtermDstcmdScratch *scratch,
                                                   const wchar_t *path)
{
    if (scratch == 0 || path == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(scratch);
    WinxtermDstcmdWin32Path win32_path;
    bool ok = winxterm_dstcmd_path_prepare_win32_scratch(scratch, path, &win32_path) &&
              CreateDirectoryW(win32_path.syscall, 0) != 0;
    winxterm_dstcmd_scratch_rewind(scratch, mark);
    return ok;
}

bool winxterm_dstcmd_path_copy_file_scratch(WinxtermDstcmdScratch *scratch,
                                            const wchar_t *source,
                                            const wchar_t *destination,
                                            bool fail_if_exists)
{
    if (scratch == 0 || source == 0 || destination == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(scratch);
    WinxtermDstcmdWin32Path win32_source;
    WinxtermDstcmdWin32Path win32_destination;
    bool ok = winxterm_dstcmd_path_prepare_win32_scratch(scratch, source, &win32_source) &&
              winxterm_dstcmd_path_prepare_win32_scratch(scratch, destination, &win32_destination) &&
              CopyFileW(win32_source.syscall, win32_destination.syscall, fail_if_exists ? TRUE : FALSE) != 0;
    winxterm_dstcmd_scratch_rewind(scratch, mark);
    return ok;
}

bool winxterm_dstcmd_path_move_file_scratch(WinxtermDstcmdScratch *scratch,
                                            const wchar_t *source,
                                            const wchar_t *destination,
                                            DWORD flags)
{
    if (scratch == 0 || source == 0 || destination == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(scratch);
    WinxtermDstcmdWin32Path win32_source;
    WinxtermDstcmdWin32Path win32_destination;
    bool ok = winxterm_dstcmd_path_prepare_win32_scratch(scratch, source, &win32_source) &&
              winxterm_dstcmd_path_prepare_win32_scratch(scratch, destination, &win32_destination) &&
              MoveFileExW(win32_source.syscall, win32_destination.syscall, flags) != 0;
    winxterm_dstcmd_scratch_rewind(scratch, mark);
    return ok;
}

bool winxterm_dstcmd_path_delete_file_scratch(WinxtermDstcmdScratch *scratch,
                                              const wchar_t *path)
{
    if (scratch == 0 || path == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(scratch);
    WinxtermDstcmdWin32Path win32_path;
    bool ok = winxterm_dstcmd_path_prepare_win32_scratch(scratch, path, &win32_path) &&
              DeleteFileW(win32_path.syscall) != 0;
    winxterm_dstcmd_scratch_rewind(scratch, mark);
    return ok;
}

bool winxterm_dstcmd_path_remove_directory_scratch(WinxtermDstcmdScratch *scratch,
                                                   const wchar_t *path)
{
    if (scratch == 0 || path == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(scratch);
    WinxtermDstcmdWin32Path win32_path;
    bool ok = winxterm_dstcmd_path_prepare_win32_scratch(scratch, path, &win32_path) &&
              RemoveDirectoryW(win32_path.syscall) != 0;
    winxterm_dstcmd_scratch_rewind(scratch, mark);
    return ok;
}

bool winxterm_dstcmd_dir_iter_open_scratch(WinxtermDstcmdScratch *scratch,
                                           const wchar_t *directory,
                                           WinxtermDstcmdDirIter *iter)
{
    if (iter != 0) {
        memset(iter, 0, sizeof(*iter));
        iter->handle = INVALID_HANDLE_VALUE;
    }
    if (scratch == 0 || directory == 0 || iter == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(scratch);
    wchar_t *pattern = winxterm_dstcmd_scratch_alloc_path(scratch);
    WinxtermDstcmdWin32Path win32_pattern;
    bool ok = false;
    if (pattern == 0 ||
        !winxterm_dstcmd_path_append_child(directory, L"*", pattern, WINXTERM_DSTCMD_PATH_CAPACITY) ||
        !winxterm_dstcmd_path_prepare_win32_scratch(scratch, pattern, &win32_pattern)) {
        iter->error = GetLastError();
        if (iter->error == ERROR_SUCCESS) {
            iter->error = ERROR_FILENAME_EXCED_RANGE;
        }
        goto cleanup;
    }

    iter->handle = FindFirstFileExW(win32_pattern.syscall,
                                    FindExInfoBasic,
                                    &iter->data,
                                    FindExSearchNameMatch,
                                    0,
                                    FIND_FIRST_EX_LARGE_FETCH);
    if (iter->handle == INVALID_HANDLE_VALUE) {
        iter->error = GetLastError();
        goto cleanup;
    }
    iter->first = true;
    iter->error = ERROR_SUCCESS;
    ok = true;

cleanup:
    winxterm_dstcmd_scratch_rewind(scratch, mark);
    return ok;
}

bool winxterm_dstcmd_dir_iter_next(WinxtermDstcmdDirIter *iter,
                                   const WIN32_FIND_DATAW **data)
{
    if (data != 0) {
        *data = 0;
    }
    if (iter == 0 || data == 0 || iter->handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    if (iter->first) {
        iter->first = false;
        *data = &iter->data;
        return true;
    }
    if (!FindNextFileW(iter->handle, &iter->data)) {
        iter->error = GetLastError();
        return false;
    }
    iter->error = ERROR_SUCCESS;
    *data = &iter->data;
    return true;
}

void winxterm_dstcmd_dir_iter_close(WinxtermDstcmdDirIter *iter)
{
    if (iter == 0) {
        return;
    }
    if (iter->handle != INVALID_HANDLE_VALUE) {
        FindClose(iter->handle);
    }
    iter->handle = INVALID_HANDLE_VALUE;
    iter->first = false;
}

const wchar_t *winxterm_dstcmd_path_basename(const wchar_t *path)
{
    if (path == 0) {
        return L"";
    }
    const wchar_t *base = path;
    for (const wchar_t *p = path; *p != L'\0'; ++p) {
        if (winxterm_dstcmd_is_slash(*p)) {
            base = p + 1;
        }
    }
    return *base != L'\0' ? base : path;
}

bool winxterm_dstcmd_path_to_display(const wchar_t *path, wchar_t *out, size_t out_count)
{
    return winxterm_dstcmd_copy_with_separator(path, out, out_count, L'/');
}

bool winxterm_dstcmd_path_to_native(const wchar_t *path, wchar_t *out, size_t out_count)
{
    return winxterm_dstcmd_copy_with_separator(path, out, out_count, L'\\');
}
