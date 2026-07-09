#include "winxterm_log.h"

#include <stdarg.h>
#include <string.h>
#include <wchar.h>

static bool winxterm_log_get_home_directory(wchar_t *buffer, size_t buffer_count)
{
    if (buffer == 0 || buffer_count == 0u) {
        return false;
    }

    DWORD length = GetEnvironmentVariableW(L"USERPROFILE", buffer, (DWORD)buffer_count);
    if (length != 0u && length < buffer_count) {
        return true;
    }

    wchar_t drive[16];
    wchar_t path[WINXTERM_LOG_PATH_CAPACITY];
    DWORD drive_length = GetEnvironmentVariableW(L"HOMEDRIVE", drive, 16u);
    DWORD path_length = GetEnvironmentVariableW(L"HOMEPATH", path, WINXTERM_LOG_PATH_CAPACITY);
    if (drive_length == 0u || drive_length >= 16u || path_length == 0u ||
        path_length >= WINXTERM_LOG_PATH_CAPACITY) {
        buffer[0] = L'\0';
        return false;
    }

    return _snwprintf_s(buffer, buffer_count, _TRUNCATE, L"%ls%ls", drive, path) >= 0;
}

static bool winxterm_log_append_child(const wchar_t *directory,
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

static bool winxterm_log_format_logs_directory(wchar_t *buffer, size_t buffer_count)
{
    if (buffer == 0 || buffer_count == 0u) {
        return false;
    }

    wchar_t home[WINXTERM_LOG_PATH_CAPACITY];
    wchar_t state_directory[WINXTERM_LOG_PATH_CAPACITY];
    if (!winxterm_log_get_home_directory(home, WINXTERM_LOG_PATH_CAPACITY) ||
        !winxterm_log_append_child(home, L".winxterm", state_directory, WINXTERM_LOG_PATH_CAPACITY) ||
        !winxterm_log_append_child(state_directory, L"logs", buffer, buffer_count)) {
        buffer[0] = L'\0';
        return false;
    }

    return true;
}

static bool winxterm_log_prepare_logs_directory(wchar_t *buffer, size_t buffer_count)
{
    if (buffer == 0 || buffer_count == 0u) {
        return false;
    }

    wchar_t home[WINXTERM_LOG_PATH_CAPACITY];
    wchar_t state_directory[WINXTERM_LOG_PATH_CAPACITY];
    if (!winxterm_log_get_home_directory(home, WINXTERM_LOG_PATH_CAPACITY) ||
        !winxterm_log_append_child(home, L".winxterm", state_directory, WINXTERM_LOG_PATH_CAPACITY) ||
        !winxterm_log_append_child(state_directory, L"logs", buffer, buffer_count)) {
        buffer[0] = L'\0';
        return false;
    }

    if (!CreateDirectoryW(state_directory, 0) && GetLastError() != ERROR_ALREADY_EXISTS) {
        buffer[0] = L'\0';
        return false;
    }
    if (!CreateDirectoryW(buffer, 0) && GetLastError() != ERROR_ALREADY_EXISTS) {
        buffer[0] = L'\0';
        return false;
    }

    return true;
}

bool winxterm_log_format_filename(DWORD pid, wchar_t *buffer, size_t buffer_count)
{
    if (buffer == 0 || buffer_count == 0) {
        return false;
    }

    int required = _scwprintf(L"winxterm-debug-%lu.txt", (unsigned long)pid);
    if (required <= 0 || (size_t)required >= buffer_count) {
        buffer[0] = L'\0';
        return false;
    }

    int written = swprintf_s(buffer, buffer_count, L"winxterm-debug-%lu.txt", (unsigned long)pid);
    return written > 0 && (size_t)written < buffer_count;
}

bool winxterm_log_format_path(DWORD pid, wchar_t *buffer, size_t buffer_count)
{
    if (buffer == 0 || buffer_count == 0) {
        return false;
    }

    wchar_t logs_directory[WINXTERM_LOG_PATH_CAPACITY];
    wchar_t filename[64];
    if (!winxterm_log_format_logs_directory(logs_directory, WINXTERM_LOG_PATH_CAPACITY) ||
        !winxterm_log_format_filename(pid, filename, 64u) ||
        !winxterm_log_append_child(logs_directory, filename, buffer, buffer_count)) {
        buffer[0] = L'\0';
        return false;
    }

    return true;
}

bool winxterm_log_init(WinxtermLog *log)
{
    if (log == 0) {
        return false;
    }

    memset(log, 0, sizeof(*log));
    InitializeCriticalSection(&log->lock);
    log->lock_initialized = true;
    return true;
}

bool winxterm_log_enable_for_process(WinxtermLog *log)
{
    if (log == 0 || !log->lock_initialized) {
        return false;
    }

    if (log->lock_initialized) {
        EnterCriticalSection(&log->lock);
    }
    if (log->file != 0) {
        log->enabled = true;
        if (log->lock_initialized) {
            LeaveCriticalSection(&log->lock);
        }
        return true;
    }

    wchar_t logs_directory[WINXTERM_LOG_PATH_CAPACITY];
    wchar_t filename[64];
    wchar_t path[WINXTERM_LOG_PATH_CAPACITY];
    if (!winxterm_log_prepare_logs_directory(logs_directory, WINXTERM_LOG_PATH_CAPACITY) ||
        !winxterm_log_format_filename(GetCurrentProcessId(), filename, 64u) ||
        !winxterm_log_append_child(logs_directory, filename, path, WINXTERM_LOG_PATH_CAPACITY)) {
        if (log->lock_initialized) {
            LeaveCriticalSection(&log->lock);
        }
        return false;
    }

    FILE *file = 0;
    errno_t err = _wfopen_s(&file, path, L"a");
    if (err != 0 || file == 0) {
        log->file = 0;
        log->enabled = false;
        if (log->lock_initialized) {
            LeaveCriticalSection(&log->lock);
        }
        return false;
    }

    log->file = file;
    wcscpy_s(log->path, WINXTERM_LOG_PATH_CAPACITY, path);
    log->enabled = true;
    if (log->lock_initialized) {
        LeaveCriticalSection(&log->lock);
    }

    winxterm_log_writef(log, "log opened");
    return true;
}

void winxterm_log_disable(WinxtermLog *log)
{
    if (log == 0 || !log->lock_initialized) {
        return;
    }

    if (log->lock_initialized) {
        EnterCriticalSection(&log->lock);
    }
    FILE *file = log->file;
    if (file != 0) {
        SYSTEMTIME now;
        GetLocalTime(&now);
        fprintf(file,
                "%04u-%02u-%02u %02u:%02u:%02u.%03u [pid=%lu] log closed\n",
                (unsigned)now.wYear,
                (unsigned)now.wMonth,
                (unsigned)now.wDay,
                (unsigned)now.wHour,
                (unsigned)now.wMinute,
                (unsigned)now.wSecond,
                (unsigned)now.wMilliseconds,
                (unsigned long)GetCurrentProcessId());
        fflush(file);
        fclose(file);
    }
    log->file = 0;
    log->enabled = false;
    if (log->lock_initialized) {
        LeaveCriticalSection(&log->lock);
    }
}

void winxterm_log_dispose(WinxtermLog *log)
{
    if (log == 0) {
        return;
    }

    winxterm_log_disable(log);
    if (log->lock_initialized) {
        DeleteCriticalSection(&log->lock);
        log->lock_initialized = false;
    }
    log->path[0] = L'\0';
}

bool winxterm_log_enabled(const WinxtermLog *log)
{
    if (log == 0) {
        return false;
    }

    WinxtermLog *mutable_log = (WinxtermLog *)log;
    if (mutable_log->lock_initialized) {
        EnterCriticalSection(&mutable_log->lock);
    }
    bool enabled = mutable_log->enabled && mutable_log->file != 0;
    if (mutable_log->lock_initialized) {
        LeaveCriticalSection(&mutable_log->lock);
    }
    return enabled;
}

void winxterm_log_writef(WinxtermLog *log, const char *format, ...)
{
    if (log == 0 || format == 0) {
        return;
    }

    if (log->lock_initialized) {
        EnterCriticalSection(&log->lock);
    }
    if (!log->enabled || log->file == 0) {
        if (log->lock_initialized) {
            LeaveCriticalSection(&log->lock);
        }
        return;
    }

    SYSTEMTIME now;
    GetLocalTime(&now);

    fprintf(log->file,
            "%04u-%02u-%02u %02u:%02u:%02u.%03u [pid=%lu] ",
            (unsigned)now.wYear,
            (unsigned)now.wMonth,
            (unsigned)now.wDay,
            (unsigned)now.wHour,
            (unsigned)now.wMinute,
            (unsigned)now.wSecond,
            (unsigned)now.wMilliseconds,
            (unsigned long)GetCurrentProcessId());

    va_list args;
    va_start(args, format);
    vfprintf(log->file, format, args);
    va_end(args);

    fputc('\n', log->file);
    fflush(log->file);

    if (log->lock_initialized) {
        LeaveCriticalSection(&log->lock);
    }
}

const wchar_t *winxterm_log_path(const WinxtermLog *log)
{
    return log != 0 ? log->path : L"";
}
