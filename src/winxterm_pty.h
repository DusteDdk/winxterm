#ifndef WINXTERM_PTY_H
#define WINXTERM_PTY_H

#include <stdbool.h>
#include <stddef.h>
#include <windows.h>

#define WINXTERM_PTY_SHIM_ENV L"WINXTERM_USE_CONPTY_SHIM"

typedef enum WinxtermPtyBackend {
    WINXTERM_PTY_BACKEND_NATIVE = 0,
    WINXTERM_PTY_BACKEND_SHIM = 1
} WinxtermPtyBackend;

typedef struct WinxtermPty {
    WinxtermPtyBackend backend;
    HPCON pseudo_console;
    HANDLE child_input;
    HANDLE child_output;
} WinxtermPty;

bool winxterm_pty_backend_from_environment(WinxtermPtyBackend *backend,
                                           wchar_t *error,
                                           size_t error_count);
const char *winxterm_pty_backend_name(WinxtermPtyBackend backend);

HRESULT winxterm_pty_create(WinxtermPty *pty,
                            WinxtermPtyBackend backend,
                            COORD size,
                            HANDLE *input_write,
                            HANDLE *output_read);
bool winxterm_pty_prepare_startup(WinxtermPty *pty,
                                  const HANDLE *extra_handles,
                                  size_t extra_handle_count,
                                  STARTUPINFOEXW *startup,
                                  LPPROC_THREAD_ATTRIBUTE_LIST *attribute_list,
                                  BOOL *inherit_handles,
                                  DWORD *creation_flags);
bool winxterm_pty_prepare_stdio_startup(HANDLE stdin_handle,
                                        HANDLE stdout_handle,
                                        HANDLE stderr_handle,
                                        const HANDLE *extra_handles,
                                        size_t extra_handle_count,
                                        STARTUPINFOEXW *startup,
                                        LPPROC_THREAD_ATTRIBUTE_LIST *attribute_list);
void winxterm_pty_destroy_startup(LPPROC_THREAD_ATTRIBUTE_LIST attribute_list);
void winxterm_pty_finish_launch(WinxtermPty *pty);
HRESULT winxterm_pty_resize(WinxtermPty *pty, COORD size);
void winxterm_pty_dispose(WinxtermPty *pty);

#endif
