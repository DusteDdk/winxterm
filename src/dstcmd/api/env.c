#include "dstcmd/api/env.h"

#include "dstcmd/winxterm_dstcmd.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

static bool winxterm_dstcmd_env_assignment(const wchar_t *arg,
                                           const wchar_t **name,
                                           size_t *name_length,
                                           const wchar_t **value)
{
    if (arg == 0 || name == 0 || name_length == 0 || value == 0) {
        return false;
    }
    const wchar_t *equals = wcschr(arg, L'=');
    if (equals == 0 || equals == arg) {
        return false;
    }
    *name = arg;
    *name_length = (size_t)(equals - arg);
    *value = equals + 1;
    return true;
}

WinxtermDstcmdEnvApplyStatus winxterm_dstcmd_env_apply_assignment(const wchar_t *assignment,
                                                                  wchar_t *variable,
                                                                  size_t variable_count)
{
    const wchar_t *name = 0;
    size_t name_length = 0u;
    const wchar_t *value = 0;
    if (!winxterm_dstcmd_env_assignment(assignment, &name, &name_length, &value)) {
        return WINXTERM_DSTCMD_ENV_APPLY_INVALID;
    }
    if (variable == 0 || name_length >= variable_count) {
        return WINXTERM_DSTCMD_ENV_APPLY_NAME_TOO_LONG;
    }
    memcpy(variable, name, name_length * sizeof(*variable));
    variable[name_length] = L'\0';
    if (!SetEnvironmentVariableW(variable, value)) {
        return WINXTERM_DSTCMD_ENV_APPLY_SET_FAILED;
    }
    return WINXTERM_DSTCMD_ENV_APPLY_OK;
}

static bool winxterm_dstcmd_env_utf8_to_wide(const char *line, wchar_t **wide_line)
{
    if (line == 0 || wide_line == 0) {
        return false;
    }
    *wide_line = 0;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line, -1, 0, 0);
    if (count <= 0) {
        return false;
    }
    wchar_t *buffer = (wchar_t *)calloc((size_t)count, sizeof(*buffer));
    if (buffer == 0) {
        return false;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, line, -1, buffer, count) <= 0) {
        free(buffer);
        return false;
    }
    *wide_line = buffer;
    return true;
}

static char *winxterm_dstcmd_env_line_start(char *line, bool first_line)
{
    if (line == 0) {
        return 0;
    }
    unsigned char *bytes = (unsigned char *)line;
    if (first_line && bytes[0] == 0xefu && bytes[1] == 0xbbu && bytes[2] == 0xbfu) {
        line += 3;
    }
    while (*line == ' ' || *line == '\t') {
        ++line;
    }
    return line;
}

bool winxterm_dstcmd_env_load_file(const wchar_t *path)
{
    if (path == 0 || path[0] == L'\0') {
        return false;
    }
    FILE *file = 0;
    if (_wfopen_s(&file, path, L"rb") != 0 || file == 0) {
        return false;
    }

    char buffer[WINXTERM_DSTCMD_LINE_CAPACITY];
    bool first_line = true;
    while (fgets(buffer, sizeof(buffer), file) != 0) {
        size_t length = strlen(buffer);
        while (length != 0u && (buffer[length - 1u] == '\n' || buffer[length - 1u] == '\r')) {
            buffer[--length] = '\0';
        }
        char *line = winxterm_dstcmd_env_line_start(buffer, first_line);
        first_line = false;
        if (line == 0 || line[0] == '\0' || line[0] == '#') {
            continue;
        }

        wchar_t *wide_line = 0;
        if (winxterm_dstcmd_env_utf8_to_wide(line, &wide_line)) {
            wchar_t variable[32768];
            (void)winxterm_dstcmd_env_apply_assignment(wide_line,
                                                       variable,
                                                       sizeof(variable) / sizeof(variable[0]));
            free(wide_line);
        }
    }
    fclose(file);
    return true;
}

static void winxterm_dstcmd_env_set_error(wchar_t *error,
                                          size_t error_count,
                                          const wchar_t *format,
                                          ...)
{
    if (error == 0 || error_count == 0u) {
        return;
    }
    va_list args;
    va_start(args, format);
    if (_vsnwprintf_s(error, error_count, _TRUNCATE, format, args) < 0) {
        error[error_count - 1u] = L'\0';
    }
    va_end(args);
}

static void winxterm_dstcmd_env_trim_message(wchar_t *message)
{
    if (message == 0) {
        return;
    }
    size_t length = wcslen(message);
    while (length != 0u &&
           (message[length - 1u] == L'\r' || message[length - 1u] == L'\n' ||
            message[length - 1u] == L' ' || message[length - 1u] == L'\t')) {
        message[--length] = L'\0';
    }
}

static void winxterm_dstcmd_env_set_win32_error(wchar_t *error,
                                                size_t error_count,
                                                const wchar_t *context,
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
    winxterm_dstcmd_env_trim_message(message);
    if (path != 0 && path[0] != L'\0') {
        winxterm_dstcmd_env_set_error(error, error_count, L"%ls %ls: %ls", context, path, message);
    } else {
        winxterm_dstcmd_env_set_error(error, error_count, L"%ls: %ls", context, message);
    }
}

static void winxterm_dstcmd_env_set_errno_error(wchar_t *error,
                                                size_t error_count,
                                                const wchar_t *context,
                                                const wchar_t *path,
                                                int code)
{
    wchar_t message[512];
    if (_wcserror_s(message, sizeof(message) / sizeof(message[0]), code) != 0) {
        (void)_snwprintf_s(message,
                           sizeof(message) / sizeof(message[0]),
                           _TRUNCATE,
                           L"errno %d",
                           code);
    }
    winxterm_dstcmd_env_trim_message(message);
    if (path != 0 && path[0] != L'\0') {
        winxterm_dstcmd_env_set_error(error, error_count, L"%ls %ls: %ls", context, path, message);
    } else {
        winxterm_dstcmd_env_set_error(error, error_count, L"%ls: %ls", context, message);
    }
}

static bool winxterm_dstcmd_env_get_home_directory(wchar_t *out,
                                                   size_t out_count,
                                                   wchar_t *error,
                                                   size_t error_count)
{
    if (out == 0 || out_count == 0u) {
        winxterm_dstcmd_env_set_error(error, error_count, L"invalid home directory buffer");
        return false;
    }
    DWORD length = GetEnvironmentVariableW(L"USERPROFILE", out, (DWORD)out_count);
    if (length != 0u && length < out_count) {
        return true;
    }

    wchar_t drive[16];
    wchar_t path[WINXTERM_DSTCMD_PATH_CAPACITY];
    DWORD drive_length = GetEnvironmentVariableW(L"HOMEDRIVE", drive, 16u);
    DWORD path_length = GetEnvironmentVariableW(L"HOMEPATH", path, WINXTERM_DSTCMD_PATH_CAPACITY);
    if (drive_length == 0u || drive_length >= 16u || path_length == 0u ||
        path_length >= WINXTERM_DSTCMD_PATH_CAPACITY ||
        _snwprintf_s(out, out_count, _TRUNCATE, L"%ls%ls", drive, path) < 0) {
        winxterm_dstcmd_env_set_error(error,
                                      error_count,
                                      L"could not determine the user's home directory");
        return false;
    }
    return true;
}

static bool winxterm_dstcmd_env_state_directory(wchar_t *directory,
                                                size_t directory_count,
                                                wchar_t *error,
                                                size_t error_count)
{
    wchar_t home[WINXTERM_DSTCMD_PATH_CAPACITY];
    if (!winxterm_dstcmd_env_get_home_directory(home,
                                                sizeof(home) / sizeof(home[0]),
                                                error,
                                                error_count)) {
        return false;
    }
    if (_snwprintf_s(directory, directory_count, _TRUNCATE, L"%ls\\.winxterm", home) < 0) {
        winxterm_dstcmd_env_set_error(error, error_count, L"the .winxterm directory path is too long");
        return false;
    }
    if (!CreateDirectoryW(directory, 0)) {
        DWORD code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS) {
            winxterm_dstcmd_env_set_win32_error(error,
                                                error_count,
                                                L"could not create",
                                                directory,
                                                code);
            return false;
        }
    }
    DWORD attributes = GetFileAttributesW(directory);
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
        winxterm_dstcmd_env_set_error(error,
                                      error_count,
                                      L"%ls exists but is not a directory",
                                      directory);
        return false;
    }
    return true;
}

static bool winxterm_dstcmd_env_join_state_file(const wchar_t *directory,
                                                const wchar_t *name,
                                                wchar_t *out,
                                                size_t out_count,
                                                wchar_t *error,
                                                size_t error_count)
{
    if (_snwprintf_s(out, out_count, _TRUNCATE, L"%ls\\%ls", directory, name) < 0) {
        winxterm_dstcmd_env_set_error(error, error_count, L"the env.rc path is too long");
        return false;
    }
    return true;
}

static bool winxterm_dstcmd_env_backup_name(wchar_t *out,
                                            size_t out_count,
                                            wchar_t *error,
                                            size_t error_count)
{
    SYSTEMTIME time;
    GetLocalTime(&time);
    DWORD pid = GetCurrentProcessId();
    if (_snwprintf_s(out,
                     out_count,
                     _TRUNCATE,
                     L"env.backup_%lu_%04u-%02u-%02u_%u_%u_%u.rc.txt",
                     (unsigned long)pid,
                     (unsigned int)time.wYear,
                     (unsigned int)time.wMonth,
                     (unsigned int)time.wDay,
                     (unsigned int)time.wHour,
                     (unsigned int)time.wMinute,
                     (unsigned int)time.wSecond) < 0) {
        winxterm_dstcmd_env_set_error(error, error_count, L"the env.rc backup path is too long");
        return false;
    }
    return true;
}

static bool winxterm_dstcmd_env_temp_name(wchar_t *out,
                                          size_t out_count,
                                          wchar_t *error,
                                          size_t error_count)
{
    if (_snwprintf_s(out,
                     out_count,
                     _TRUNCATE,
                     L"env.tmp_%lu_%lu.rc.tmp",
                     (unsigned long)GetCurrentProcessId(),
                     (unsigned long)GetTickCount()) < 0) {
        winxterm_dstcmd_env_set_error(error, error_count, L"the temporary env.rc path is too long");
        return false;
    }
    return true;
}

static bool winxterm_dstcmd_env_entry_is_representable(const wchar_t *entry,
                                                       wchar_t *error,
                                                       size_t error_count)
{
    for (const wchar_t *p = entry; p != 0 && *p != L'\0'; ++p) {
        if (*p == L'\r' || *p == L'\n') {
            winxterm_dstcmd_env_set_error(error,
                                          error_count,
                                          L"the environment contains a variable with a newline, which env.rc cannot represent");
            return false;
        }
    }
    return true;
}

static bool winxterm_dstcmd_env_write_utf8_line(FILE *file,
                                                const wchar_t *entry,
                                                wchar_t *error,
                                                size_t error_count)
{
    int byte_count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry, -1, 0, 0, 0, 0);
    if (byte_count <= 0) {
        winxterm_dstcmd_env_set_win32_error(error,
                                            error_count,
                                            L"could not encode environment variable",
                                            0,
                                            GetLastError());
        return false;
    }
    char *bytes = (char *)malloc((size_t)byte_count);
    if (bytes == 0) {
        winxterm_dstcmd_env_set_error(error, error_count, L"out of memory");
        return false;
    }
    bool ok = false;
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, entry, -1, bytes, byte_count, 0, 0) <=
        0) {
        winxterm_dstcmd_env_set_win32_error(error,
                                            error_count,
                                            L"could not encode environment variable",
                                            0,
                                            GetLastError());
        goto cleanup;
    }
    if (fwrite(bytes, 1u, (size_t)byte_count - 1u, file) != (size_t)byte_count - 1u ||
        fwrite("\r\n", 1u, 2u, file) != 2u) {
        winxterm_dstcmd_env_set_errno_error(error, error_count, L"could not write", 0, errno);
        goto cleanup;
    }
    ok = true;

cleanup:
    free(bytes);
    return ok;
}

static bool winxterm_dstcmd_env_write_raw(FILE *file,
                                          const char *bytes,
                                          size_t byte_count,
                                          const wchar_t *path,
                                          wchar_t *error,
                                          size_t error_count)
{
    if (byte_count == 0u) {
        return true;
    }
    if (fwrite(bytes, 1u, byte_count, file) != byte_count) {
        winxterm_dstcmd_env_set_errno_error(error, error_count, L"could not write", path, errno);
        return false;
    }
    return true;
}

static bool winxterm_dstcmd_env_write_newline(FILE *file,
                                              const wchar_t *path,
                                              wchar_t *error,
                                              size_t error_count)
{
    return winxterm_dstcmd_env_write_raw(file, "\r\n", 2u, path, error, error_count);
}

static bool winxterm_dstcmd_env_ascii_equal_ci(const char *left, const char *right, size_t count)
{
    if (left == 0 || right == 0) {
        return false;
    }
    for (size_t i = 0u; i < count; ++i) {
        char a = left[i];
        char b = right[i];
        if (a >= 'a' && a <= 'z') {
            a = (char)(a - ('a' - 'A'));
        }
        if (b >= 'a' && b <= 'z') {
            b = (char)(b - ('a' - 'A'));
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

static bool winxterm_dstcmd_env_line_is_cwd_assignment(const char *line, bool first_line)
{
    if (line == 0) {
        return false;
    }
    const char *start = line;
    const unsigned char *bytes = (const unsigned char *)start;
    if (first_line && bytes[0] == 0xefu && bytes[1] == 0xbbu && bytes[2] == 0xbfu) {
        start += 3;
    }
    while (*start == ' ' || *start == '\t') {
        ++start;
    }
    if (*start == '\0' || *start == '#' || *start == '\r' || *start == '\n') {
        return false;
    }
    const char *equals = strchr(start, '=');
    return equals != 0 && (size_t)(equals - start) == 3u &&
           winxterm_dstcmd_env_ascii_equal_ci(start, "CWD", 3u);
}

static bool winxterm_dstcmd_env_write_cwd_line(FILE *file,
                                               const wchar_t *cwd,
                                               const wchar_t *path,
                                               wchar_t *error,
                                               size_t error_count)
{
    if (cwd == 0 || cwd[0] == L'\0') {
        winxterm_dstcmd_env_set_error(error, error_count, L"CWD is not set");
        return false;
    }
    if (wcslen(cwd) > WINXTERM_DSTCMD_PATH_CAPACITY - 5u) {
        winxterm_dstcmd_env_set_error(error, error_count, L"CWD is too long");
        return false;
    }
    wchar_t entry[WINXTERM_DSTCMD_PATH_CAPACITY + 5u];
    if (_snwprintf_s(entry,
                     sizeof(entry) / sizeof(entry[0]),
                     _TRUNCATE,
                     L"CWD=%ls",
                     cwd) < 0) {
        winxterm_dstcmd_env_set_error(error, error_count, L"CWD is too long");
        return false;
    }
    if (!winxterm_dstcmd_env_entry_is_representable(entry, error, error_count)) {
        return false;
    }
    (void)path;
    return winxterm_dstcmd_env_write_utf8_line(file, entry, error, error_count);
}

static WinxtermDstcmdEnvCwdStatus winxterm_dstcmd_env_edit_cwd_rc(const wchar_t *cwd,
                                                                  bool save,
                                                                  wchar_t *env_path,
                                                                  size_t env_path_count,
                                                                  wchar_t *error,
                                                                  size_t error_count)
{
    if (env_path != 0 && env_path_count != 0u) {
        env_path[0] = L'\0';
    }
    if (error != 0 && error_count != 0u) {
        error[0] = L'\0';
    }
    if (env_path == 0 || env_path_count == 0u) {
        winxterm_dstcmd_env_set_error(error, error_count, L"invalid CWD environment arguments");
        return WINXTERM_DSTCMD_ENV_CWD_FAILED;
    }
    if (save && (cwd == 0 || cwd[0] == L'\0')) {
        winxterm_dstcmd_env_set_error(error, error_count, L"CWD is not set");
        return WINXTERM_DSTCMD_ENV_CWD_FAILED;
    }

    wchar_t directory[WINXTERM_DSTCMD_PATH_CAPACITY];
    if (!winxterm_dstcmd_env_state_directory(directory,
                                             sizeof(directory) / sizeof(directory[0]),
                                             error,
                                             error_count) ||
        !winxterm_dstcmd_env_join_state_file(directory,
                                             L"env.rc",
                                             env_path,
                                             env_path_count,
                                             error,
                                             error_count)) {
        return WINXTERM_DSTCMD_ENV_CWD_FAILED;
    }

    DWORD attributes = GetFileAttributesW(env_path);
    bool replacing = attributes != INVALID_FILE_ATTRIBUTES;
    if (replacing && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
        winxterm_dstcmd_env_set_error(error, error_count, L"%ls exists but is a directory", env_path);
        return WINXTERM_DSTCMD_ENV_CWD_FAILED;
    }
    if (!replacing && !save) {
        return WINXTERM_DSTCMD_ENV_CWD_UNCHANGED;
    }

    wchar_t temp_name[64];
    wchar_t temp_path[WINXTERM_DSTCMD_PATH_CAPACITY];
    if (!winxterm_dstcmd_env_temp_name(temp_name,
                                       sizeof(temp_name) / sizeof(temp_name[0]),
                                       error,
                                       error_count) ||
        !winxterm_dstcmd_env_join_state_file(directory,
                                             temp_name,
                                             temp_path,
                                             sizeof(temp_path) / sizeof(temp_path[0]),
                                             error,
                                             error_count)) {
        return WINXTERM_DSTCMD_ENV_CWD_FAILED;
    }

    FILE *input = 0;
    if (replacing) {
        errno_t open_error = _wfopen_s(&input, env_path, L"rb");
        if (open_error != 0 || input == 0) {
            winxterm_dstcmd_env_set_errno_error(error,
                                                error_count,
                                                L"could not open",
                                                env_path,
                                                open_error != 0 ? open_error : errno);
            return WINXTERM_DSTCMD_ENV_CWD_FAILED;
        }
    }

    FILE *output = 0;
    errno_t temp_open_error = _wfopen_s(&output, temp_path, L"wb");
    if (temp_open_error != 0 || output == 0) {
        if (input != 0) {
            fclose(input);
        }
        winxterm_dstcmd_env_set_errno_error(error,
                                            error_count,
                                            L"could not open",
                                            temp_path,
                                            temp_open_error != 0 ? temp_open_error : errno);
        return WINXTERM_DSTCMD_ENV_CWD_FAILED;
    }

    bool ok = true;
    bool found = false;
    bool wrote_cwd = false;
    bool wrote_any = false;
    bool last_ended_newline = true;
    bool first_line = true;
    bool skipping_line_remainder = false;
    char buffer[WINXTERM_DSTCMD_LINE_CAPACITY];
    while (input != 0 && fgets(buffer, sizeof(buffer), input) != 0) {
        size_t length = strlen(buffer);
        bool ended_newline = length != 0u && buffer[length - 1u] == '\n';
        if (skipping_line_remainder) {
            if (ended_newline) {
                skipping_line_remainder = false;
            }
            first_line = false;
            continue;
        }
        if (winxterm_dstcmd_env_line_is_cwd_assignment(buffer, first_line)) {
            found = true;
            if (save && !wrote_cwd) {
                ok = winxterm_dstcmd_env_write_cwd_line(output,
                                                        cwd,
                                                        temp_path,
                                                        error,
                                                        error_count);
                wrote_cwd = true;
                wrote_any = true;
                last_ended_newline = true;
            }
            if (!ended_newline) {
                skipping_line_remainder = true;
            }
            first_line = false;
            if (!ok) {
                break;
            }
            continue;
        }
        ok = winxterm_dstcmd_env_write_raw(output, buffer, length, temp_path, error, error_count);
        wrote_any = wrote_any || length != 0u;
        last_ended_newline = ended_newline;
        first_line = false;
        if (!ok) {
            break;
        }
    }
    if (input != 0 && ferror(input) && ok) {
        winxterm_dstcmd_env_set_errno_error(error, error_count, L"could not read", env_path, errno);
        ok = false;
    }
    if (save && ok && !wrote_cwd) {
        if (wrote_any && !last_ended_newline) {
            ok = winxterm_dstcmd_env_write_newline(output, temp_path, error, error_count);
        }
        if (ok) {
            ok = winxterm_dstcmd_env_write_cwd_line(output, cwd, temp_path, error, error_count);
        }
    }
    if (input != 0 && fclose(input) != 0 && ok) {
        winxterm_dstcmd_env_set_errno_error(error, error_count, L"could not close", env_path, errno);
        ok = false;
    }
    if (fclose(output) != 0 && ok) {
        winxterm_dstcmd_env_set_errno_error(error, error_count, L"could not close", temp_path, errno);
        ok = false;
    }
    if (!ok) {
        (void)DeleteFileW(temp_path);
        return WINXTERM_DSTCMD_ENV_CWD_FAILED;
    }
    if (!save && !found) {
        (void)DeleteFileW(temp_path);
        return WINXTERM_DSTCMD_ENV_CWD_UNCHANGED;
    }

    if (!MoveFileExW(temp_path,
                     env_path,
                     MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        winxterm_dstcmd_env_set_win32_error(error,
                                            error_count,
                                            replacing ? L"could not replace" : L"could not create",
                                            env_path,
                                            GetLastError());
        (void)DeleteFileW(temp_path);
        return WINXTERM_DSTCMD_ENV_CWD_FAILED;
    }
    return WINXTERM_DSTCMD_ENV_CWD_UPDATED;
}

WinxtermDstcmdEnvCwdStatus winxterm_dstcmd_env_save_cwd_rc(const wchar_t *cwd,
                                                           wchar_t *env_path,
                                                           size_t env_path_count,
                                                           wchar_t *error,
                                                           size_t error_count)
{
    return winxterm_dstcmd_env_edit_cwd_rc(cwd, true, env_path, env_path_count, error, error_count);
}

WinxtermDstcmdEnvCwdStatus winxterm_dstcmd_env_clear_cwd_rc(wchar_t *env_path,
                                                            size_t env_path_count,
                                                            wchar_t *error,
                                                            size_t error_count)
{
    return winxterm_dstcmd_env_edit_cwd_rc(0, false, env_path, env_path_count, error, error_count);
}

static bool winxterm_dstcmd_env_write_file(const wchar_t *path,
                                           size_t *variable_count,
                                           wchar_t *error,
                                           size_t error_count)
{
    if (variable_count != 0) {
        *variable_count = 0u;
    }
    LPWCH environment = GetEnvironmentStringsW();
    if (environment == 0) {
        winxterm_dstcmd_env_set_win32_error(error,
                                            error_count,
                                            L"could not read the environment",
                                            0,
                                            GetLastError());
        return false;
    }

    FILE *file = 0;
    errno_t open_error = _wfopen_s(&file, path, L"wb");
    if (open_error != 0 || file == 0) {
        FreeEnvironmentStringsW(environment);
        winxterm_dstcmd_env_set_errno_error(error,
                                            error_count,
                                            L"could not open",
                                            path,
                                            open_error != 0 ? open_error : errno);
        return false;
    }

    bool ok = true;
    size_t count = 0u;
    for (const wchar_t *entry = environment; *entry != L'\0'; entry += wcslen(entry) + 1u) {
        if (entry[0] == L'=') {
            continue;
        }
        if (!winxterm_dstcmd_env_entry_is_representable(entry, error, error_count) ||
            !winxterm_dstcmd_env_write_utf8_line(file, entry, error, error_count)) {
            ok = false;
            break;
        }
        ++count;
    }
    if (fclose(file) != 0 && ok) {
        winxterm_dstcmd_env_set_errno_error(error, error_count, L"could not close", path, errno);
        ok = false;
    }
    FreeEnvironmentStringsW(environment);
    if (variable_count != 0) {
        *variable_count = count;
    }
    return ok;
}

WinxtermDstcmdEnvSaveStatus winxterm_dstcmd_env_save_rc(wchar_t *env_path,
                                                        size_t env_path_count,
                                                        wchar_t *backup_path,
                                                        size_t backup_path_count,
                                                        size_t *variable_count,
                                                        wchar_t *error,
                                                        size_t error_count)
{
    if (env_path != 0 && env_path_count != 0u) {
        env_path[0] = L'\0';
    }
    if (backup_path != 0 && backup_path_count != 0u) {
        backup_path[0] = L'\0';
    }
    if (variable_count != 0) {
        *variable_count = 0u;
    }
    if (error != 0 && error_count != 0u) {
        error[0] = L'\0';
    }
    if (env_path == 0 || env_path_count == 0u || backup_path == 0 || backup_path_count == 0u ||
        variable_count == 0) {
        winxterm_dstcmd_env_set_error(error, error_count, L"invalid save environment arguments");
        return WINXTERM_DSTCMD_ENV_SAVE_FAILED;
    }

    wchar_t directory[WINXTERM_DSTCMD_PATH_CAPACITY];
    if (!winxterm_dstcmd_env_state_directory(directory,
                                             sizeof(directory) / sizeof(directory[0]),
                                             error,
                                             error_count) ||
        !winxterm_dstcmd_env_join_state_file(directory,
                                             L"env.rc",
                                             env_path,
                                             env_path_count,
                                             error,
                                             error_count)) {
        return WINXTERM_DSTCMD_ENV_SAVE_FAILED;
    }

    wchar_t temp_name[64];
    wchar_t temp_path[WINXTERM_DSTCMD_PATH_CAPACITY];
    if (!winxterm_dstcmd_env_temp_name(temp_name,
                                       sizeof(temp_name) / sizeof(temp_name[0]),
                                       error,
                                       error_count) ||
        !winxterm_dstcmd_env_join_state_file(directory,
                                             temp_name,
                                             temp_path,
                                             sizeof(temp_path) / sizeof(temp_path[0]),
                                             error,
                                             error_count)) {
        return WINXTERM_DSTCMD_ENV_SAVE_FAILED;
    }

    if (!winxterm_dstcmd_env_write_file(temp_path, variable_count, error, error_count)) {
        (void)DeleteFileW(temp_path);
        return WINXTERM_DSTCMD_ENV_SAVE_FAILED;
    }

    DWORD attributes = GetFileAttributesW(env_path);
    bool replacing = attributes != INVALID_FILE_ATTRIBUTES;
    if (replacing && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
        winxterm_dstcmd_env_set_error(error, error_count, L"%ls exists but is a directory", env_path);
        (void)DeleteFileW(temp_path);
        return WINXTERM_DSTCMD_ENV_SAVE_FAILED;
    }

    if (!replacing) {
        if (!MoveFileExW(temp_path, env_path, MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
            winxterm_dstcmd_env_set_win32_error(error,
                                                error_count,
                                                L"could not create",
                                                env_path,
                                                GetLastError());
            (void)DeleteFileW(temp_path);
            return WINXTERM_DSTCMD_ENV_SAVE_FAILED;
        }
        return WINXTERM_DSTCMD_ENV_SAVE_CREATED;
    }

    wchar_t backup_name[128];
    if (!winxterm_dstcmd_env_backup_name(backup_name,
                                         sizeof(backup_name) / sizeof(backup_name[0]),
                                         error,
                                         error_count) ||
        !winxterm_dstcmd_env_join_state_file(directory,
                                             backup_name,
                                             backup_path,
                                             backup_path_count,
                                             error,
                                             error_count)) {
        (void)DeleteFileW(temp_path);
        return WINXTERM_DSTCMD_ENV_SAVE_FAILED;
    }
    if (!MoveFileExW(env_path, backup_path, MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
        winxterm_dstcmd_env_set_win32_error(error,
                                            error_count,
                                            L"could not back up",
                                            env_path,
                                            GetLastError());
        (void)DeleteFileW(temp_path);
        return WINXTERM_DSTCMD_ENV_SAVE_FAILED;
    }
    if (!MoveFileExW(temp_path, env_path, MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
        DWORD replace_error = GetLastError();
        bool restored = MoveFileExW(backup_path,
                                    env_path,
                                    MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING |
                                        MOVEFILE_WRITE_THROUGH) != 0;
        if (restored) {
            winxterm_dstcmd_env_set_win32_error(error,
                                                error_count,
                                                L"could not replace",
                                                env_path,
                                                replace_error);
        } else {
            winxterm_dstcmd_env_set_error(error,
                                          error_count,
                                          L"could not replace %ls and could not restore the previous file from %ls",
                                          env_path,
                                          backup_path);
        }
        (void)DeleteFileW(temp_path);
        return WINXTERM_DSTCMD_ENV_SAVE_FAILED;
    }
    return WINXTERM_DSTCMD_ENV_SAVE_REPLACED;
}
