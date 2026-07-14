#include "winxterm_settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#define WINXTERM_SETTINGS_PATH_CAPACITY 32768u
#define WINXTERM_SETTINGS_MAX_FILE_BYTES 8192u

static bool winxterm_settings_append_child(const wchar_t *directory,
                                           const wchar_t *name,
                                           wchar_t *out,
                                           size_t out_count)
{
    if (directory == 0 || directory[0] == L'\0' || name == 0 || name[0] == L'\0' ||
        out == 0 || out_count == 0u) {
        return false;
    }

    const wchar_t *separator = L"\\";
    size_t length = wcslen(directory);
    if (length != 0u && (directory[length - 1u] == L'\\' || directory[length - 1u] == L'/')) {
        separator = L"";
    }

    return _snwprintf_s(out, out_count, _TRUNCATE, L"%ls%ls%ls", directory, separator, name) >= 0;
}

static bool winxterm_settings_get_home_directory(wchar_t *buffer, size_t buffer_count)
{
    if (buffer == 0 || buffer_count == 0u) {
        return false;
    }

    DWORD length = GetEnvironmentVariableW(L"USERPROFILE", buffer, (DWORD)buffer_count);
    if (length != 0u && length < buffer_count) {
        return true;
    }

    wchar_t drive[16];
    wchar_t path[WINXTERM_SETTINGS_PATH_CAPACITY];
    DWORD drive_length = GetEnvironmentVariableW(L"HOMEDRIVE", drive, 16u);
    DWORD path_length =
        GetEnvironmentVariableW(L"HOMEPATH", path, WINXTERM_SETTINGS_PATH_CAPACITY);
    if (drive_length == 0u || drive_length >= 16u || path_length == 0u ||
        path_length >= WINXTERM_SETTINGS_PATH_CAPACITY) {
        buffer[0] = L'\0';
        return false;
    }

    return _snwprintf_s(buffer, buffer_count, _TRUNCATE, L"%ls%ls", drive, path) >= 0;
}

static bool winxterm_settings_format_state_directory(wchar_t *buffer, size_t buffer_count)
{
    wchar_t home[WINXTERM_SETTINGS_PATH_CAPACITY];
    if (!winxterm_settings_get_home_directory(home, WINXTERM_SETTINGS_PATH_CAPACITY) ||
        !winxterm_settings_append_child(home, L".winxterm", buffer, buffer_count)) {
        if (buffer != 0 && buffer_count != 0u) {
            buffer[0] = L'\0';
        }
        return false;
    }
    return true;
}

static bool winxterm_settings_format_state_path(wchar_t *buffer, size_t buffer_count)
{
    wchar_t directory[WINXTERM_SETTINGS_PATH_CAPACITY];
    return winxterm_settings_format_state_directory(directory,
                                                    WINXTERM_SETTINGS_PATH_CAPACITY) &&
           winxterm_settings_append_child(directory,
                                          WINXTERM_SETTINGS_FILENAME,
                                          buffer,
                                          buffer_count);
}

static bool winxterm_settings_prepare_state_path(wchar_t *buffer,
                                                 size_t buffer_count,
                                                 wchar_t *directory,
                                                 size_t directory_count)
{
    if (!winxterm_settings_format_state_directory(directory, directory_count)) {
        return false;
    }
    if (!CreateDirectoryW(directory, 0) && GetLastError() != ERROR_ALREADY_EXISTS) {
        directory[0] = L'\0';
        return false;
    }
    return winxterm_settings_append_child(directory,
                                          WINXTERM_SETTINGS_FILENAME,
                                          buffer,
                                          buffer_count);
}

void winxterm_settings_init(WinxtermSettings *settings)
{
    if (settings == 0) {
        return;
    }
    memset(settings, 0, sizeof(*settings));
    settings->scrollbar = false;
}

static bool winxterm_settings_value_is(const char *value, size_t value_length, const char *expected)
{
    return value_length == strlen(expected) && memcmp(value, expected, value_length) == 0;
}

bool winxterm_settings_parse(const char *text, WinxtermSettings *settings)
{
    if (text == 0 || settings == 0) {
        return false;
    }

    const char *line = text;
    while (*line != '\0') {
        const char *line_end = line;
        while (*line_end != '\0' && *line_end != '\r' && *line_end != '\n') {
            ++line_end;
        }
        const char *equals = memchr(line, '=', (size_t)(line_end - line));
        if (equals != 0) {
            size_t key_length = (size_t)(equals - line);
            const char *value = equals + 1;
            size_t value_length = (size_t)(line_end - value);
            if (key_length == strlen("scrollbar") &&
                memcmp(line, "scrollbar", key_length) == 0) {
                if (winxterm_settings_value_is(value, value_length, "on")) {
                    settings->scrollbar = true;
                } else if (winxterm_settings_value_is(value, value_length, "off")) {
                    settings->scrollbar = false;
                }
            }
        }
        while (*line_end == '\r' || *line_end == '\n') {
            ++line_end;
        }
        line = line_end;
    }
    return true;
}

bool winxterm_settings_format(const WinxtermSettings *settings,
                              char *buffer,
                              size_t buffer_count)
{
    if (settings == 0 || buffer == 0 || buffer_count == 0u) {
        return false;
    }

    int written = _snprintf_s(buffer,
                              buffer_count,
                              _TRUNCATE,
                              "winxterm-settings-v1\n"
                              "scrollbar=%s\n",
                              settings->scrollbar ? "on" : "off");
    return written > 0 && (size_t)written < buffer_count;
}

bool winxterm_settings_load(WinxtermSettings *settings)
{
    if (settings == 0) {
        return false;
    }

    wchar_t path[WINXTERM_SETTINGS_PATH_CAPACITY];
    if (!winxterm_settings_format_state_path(path, WINXTERM_SETTINGS_PATH_CAPACITY)) {
        return false;
    }

    HANDLE file = CreateFileW(path,
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              0,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              0);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        size.QuadPart > (LONGLONG)WINXTERM_SETTINGS_MAX_FILE_BYTES) {
        CloseHandle(file);
        return false;
    }

    char *buffer = (char *)calloc((size_t)size.QuadPart + 1u, sizeof(*buffer));
    if (buffer == 0) {
        CloseHandle(file);
        return false;
    }

    DWORD bytes_read = 0;
    bool ok = ReadFile(file, buffer, (DWORD)size.QuadPart, &bytes_read, 0) &&
              bytes_read == (DWORD)size.QuadPart &&
              winxterm_settings_parse(buffer, settings);
    free(buffer);
    CloseHandle(file);
    return ok;
}

bool winxterm_settings_save(const WinxtermSettings *settings)
{
    if (settings == 0) {
        return false;
    }

    char text[1024];
    if (!winxterm_settings_format(settings, text, sizeof(text))) {
        return false;
    }

    wchar_t directory[WINXTERM_SETTINGS_PATH_CAPACITY];
    wchar_t path[WINXTERM_SETTINGS_PATH_CAPACITY];
    if (!winxterm_settings_prepare_state_path(path,
                                              WINXTERM_SETTINGS_PATH_CAPACITY,
                                              directory,
                                              WINXTERM_SETTINGS_PATH_CAPACITY)) {
        return false;
    }

    wchar_t temp_path[WINXTERM_SETTINGS_PATH_CAPACITY];
    if (_snwprintf_s(temp_path,
                     WINXTERM_SETTINGS_PATH_CAPACITY,
                     _TRUNCATE,
                     L"%ls\\settings-%lu-%lu.tmp",
                     directory,
                     (unsigned long)GetCurrentProcessId(),
                     (unsigned long)GetTickCount()) < 0) {
        return false;
    }

    HANDLE file = CreateFileW(temp_path,
                              GENERIC_WRITE,
                              0,
                              0,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL,
                              0);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    DWORD byte_count = (DWORD)strlen(text);
    DWORD written = 0;
    bool ok = WriteFile(file, text, byte_count, &written, 0) && written == byte_count;
    ok = CloseHandle(file) && ok;
    if (ok) {
        ok = MoveFileExW(temp_path, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    }
    if (!ok) {
        DeleteFileW(temp_path);
    }
    return ok;
}
