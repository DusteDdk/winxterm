#ifndef WINXTERM_LOG_H
#define WINXTERM_LOG_H

#include <stdbool.h>
#include <stdio.h>
#include <windows.h>

#define WINXTERM_LOG_PATH_CAPACITY 32768u

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WinxtermLog {
    FILE *file;
    wchar_t path[WINXTERM_LOG_PATH_CAPACITY];
    CRITICAL_SECTION lock;
    bool lock_initialized;
    bool enabled;
} WinxtermLog;

bool winxterm_log_format_filename(DWORD pid, wchar_t *buffer, size_t buffer_count);
bool winxterm_log_format_path(DWORD pid, wchar_t *buffer, size_t buffer_count);
bool winxterm_log_init(WinxtermLog *log);
bool winxterm_log_enable_for_process(WinxtermLog *log);
void winxterm_log_disable(WinxtermLog *log);
void winxterm_log_dispose(WinxtermLog *log);
bool winxterm_log_enabled(const WinxtermLog *log);
void winxterm_log_writef(WinxtermLog *log, const char *format, ...);
const wchar_t *winxterm_log_path(const WinxtermLog *log);

#ifdef __cplusplus
}
#endif

#endif
