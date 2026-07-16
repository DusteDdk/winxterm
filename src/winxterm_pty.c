#include "winxterm_pty.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

static void winxterm_pty_close_handle(HANDLE *handle)
{
    if (handle != 0 && *handle != 0 && *handle != INVALID_HANDLE_VALUE) {
        CloseHandle(*handle);
        *handle = 0;
    }
}

static bool winxterm_pty_create_pipes(HANDLE *input_read,
                                      HANDLE *input_write,
                                      HANDLE *output_read,
                                      HANDLE *output_write)
{
    SECURITY_ATTRIBUTES security;
    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    if (!CreatePipe(input_read, input_write, &security, 0)) return false;
    if (!CreatePipe(output_read, output_write, &security, 0)) {
        winxterm_pty_close_handle(input_read);
        winxterm_pty_close_handle(input_write);
        return false;
    }
    if (!SetHandleInformation(*input_write, HANDLE_FLAG_INHERIT, 0u) ||
        !SetHandleInformation(*output_read, HANDLE_FLAG_INHERIT, 0u)) {
        winxterm_pty_close_handle(input_read);
        winxterm_pty_close_handle(input_write);
        winxterm_pty_close_handle(output_read);
        winxterm_pty_close_handle(output_write);
        return false;
    }
    return true;
}

bool winxterm_pty_backend_from_environment(WinxtermPtyBackend *backend,
                                           wchar_t *error,
                                           size_t error_count)
{
    if (backend == 0) return false;
    *backend = WINXTERM_PTY_BACKEND_NATIVE;
    if (error != 0 && error_count != 0u) error[0] = L'\0';
    wchar_t value[16];
    SetLastError(ERROR_SUCCESS);
    DWORD length = GetEnvironmentVariableW(WINXTERM_PTY_SHIM_ENV, value,
                                            (DWORD)(sizeof(value) / sizeof(value[0])));
    DWORD environment_error = GetLastError();
    if (length == 0u && environment_error == ERROR_ENVVAR_NOT_FOUND) return true;
    if (length == 1u && value[0] == L'0') return true;
    if (length == 1u && value[0] == L'1') {
        *backend = WINXTERM_PTY_BACKEND_SHIM;
        return true;
    }
    if (error != 0 && error_count != 0u) {
        (void)swprintf_s(error, error_count,
                         L"%ls must be unset, 0, or 1.", WINXTERM_PTY_SHIM_ENV);
    }
    SetLastError(ERROR_INVALID_PARAMETER);
    return false;
}

const char *winxterm_pty_backend_name(WinxtermPtyBackend backend)
{
    return backend == WINXTERM_PTY_BACKEND_SHIM ? "stdio-shim" : "native-conpty";
}

HRESULT winxterm_pty_create(WinxtermPty *pty,
                            WinxtermPtyBackend backend,
                            COORD size,
                            HANDLE *input_write,
                            HANDLE *output_read)
{
    if (pty == 0 || input_write == 0 || output_read == 0) return E_INVALIDARG;
    memset(pty, 0, sizeof(*pty));
    *input_write = 0;
    *output_read = 0;
    pty->backend = backend;
    if (!winxterm_pty_create_pipes(&pty->child_input, input_write,
                                   output_read, &pty->child_output)) {
        DWORD error = GetLastError();
        winxterm_pty_dispose(pty);
        winxterm_pty_close_handle(input_write);
        winxterm_pty_close_handle(output_read);
        return HRESULT_FROM_WIN32(error);
    }
    if (backend == WINXTERM_PTY_BACKEND_NATIVE) {
        HRESULT result = CreatePseudoConsole(size, pty->child_input,
                                             pty->child_output, 0, &pty->pseudo_console);
        if (FAILED(result)) {
            winxterm_pty_dispose(pty);
            winxterm_pty_close_handle(input_write);
            winxterm_pty_close_handle(output_read);
            return result;
        }
    }
    return S_OK;
}

static bool winxterm_pty_prepare_handle_list(const HANDLE *handles,
                                             size_t handle_count,
                                             STARTUPINFOEXW *startup,
                                             LPPROC_THREAD_ATTRIBUTE_LIST *attribute_list,
                                             DWORD attribute_count)
{
    SIZE_T attribute_size = 0u;
    InitializeProcThreadAttributeList(0, attribute_count, 0, &attribute_size);
    *attribute_list = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0,
                                                              attribute_size);
    if (*attribute_list == 0 ||
        !InitializeProcThreadAttributeList(*attribute_list, attribute_count,
                                           0, &attribute_size)) {
        if (*attribute_list != 0) HeapFree(GetProcessHeap(), 0, *attribute_list);
        *attribute_list = 0;
        return false;
    }
    startup->lpAttributeList = *attribute_list;
    if (handle_count != 0u &&
        !UpdateProcThreadAttribute(*attribute_list, 0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   (void *)handles,
                                   handle_count * sizeof(*handles), 0, 0)) {
        winxterm_pty_destroy_startup(*attribute_list);
        *attribute_list = 0;
        startup->lpAttributeList = 0;
        return false;
    }
    return true;
}

bool winxterm_pty_prepare_stdio_startup(HANDLE stdin_handle,
                                        HANDLE stdout_handle,
                                        HANDLE stderr_handle,
                                        const HANDLE *extra_handles,
                                        size_t extra_handle_count,
                                        STARTUPINFOEXW *startup,
                                        LPPROC_THREAD_ATTRIBUTE_LIST *attribute_list)
{
    if (startup == 0 || attribute_list == 0 || extra_handle_count > 5u) return false;
    memset(startup, 0, sizeof(*startup));
    startup->StartupInfo.cb = sizeof(*startup);
    startup->StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup->StartupInfo.hStdInput = stdin_handle;
    startup->StartupInfo.hStdOutput = stdout_handle;
    startup->StartupInfo.hStdError = stderr_handle;
    HANDLE candidates[8] = {stdin_handle, stdout_handle, stderr_handle};
    HANDLE handles[8];
    size_t handle_count = 0u;
    for (size_t i = 0u; i < extra_handle_count; ++i) candidates[3u + i] = extra_handles[i];
    for (size_t i = 0u; i < 3u + extra_handle_count; ++i) {
        bool duplicate = false;
        for (size_t j = 0u; j < handle_count; ++j) {
            if (handles[j] == candidates[i]) duplicate = true;
        }
        if (!duplicate) handles[handle_count++] = candidates[i];
    }
    return winxterm_pty_prepare_handle_list(handles, handle_count, startup,
                                            attribute_list, 1u);
}

bool winxterm_pty_prepare_startup(WinxtermPty *pty,
                                  const HANDLE *extra_handles,
                                  size_t extra_handle_count,
                                  STARTUPINFOEXW *startup,
                                  LPPROC_THREAD_ATTRIBUTE_LIST *attribute_list,
                                  BOOL *inherit_handles,
                                  DWORD *creation_flags)
{
    if (pty == 0 || startup == 0 || attribute_list == 0 ||
        inherit_handles == 0 || creation_flags == 0) return false;
    *attribute_list = 0;
    *inherit_handles = FALSE;
    *creation_flags = 0u;
    if (pty->backend == WINXTERM_PTY_BACKEND_SHIM) {
        if (!winxterm_pty_prepare_stdio_startup(pty->child_input,
                                                pty->child_output,
                                                pty->child_output,
                                                extra_handles,
                                                extra_handle_count,
                                                startup,
                                                attribute_list)) return false;
        *inherit_handles = TRUE;
        *creation_flags = CREATE_NO_WINDOW;
        return true;
    }
    memset(startup, 0, sizeof(*startup));
    startup->StartupInfo.cb = sizeof(*startup);
    DWORD attribute_count = extra_handle_count != 0u ? 2u : 1u;
    if (!winxterm_pty_prepare_handle_list(extra_handles, extra_handle_count,
                                          startup, attribute_list,
                                          attribute_count)) return false;
    if (!UpdateProcThreadAttribute(*attribute_list, 0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   pty->pseudo_console,
                                   sizeof(pty->pseudo_console), 0, 0)) {
        winxterm_pty_destroy_startup(*attribute_list);
        *attribute_list = 0;
        startup->lpAttributeList = 0;
        return false;
    }
    *inherit_handles = extra_handle_count != 0u;
    return true;
}

void winxterm_pty_destroy_startup(LPPROC_THREAD_ATTRIBUTE_LIST attribute_list)
{
    if (attribute_list != 0) {
        DeleteProcThreadAttributeList(attribute_list);
        HeapFree(GetProcessHeap(), 0, attribute_list);
    }
}

void winxterm_pty_finish_launch(WinxtermPty *pty)
{
    if (pty == 0) return;
    winxterm_pty_close_handle(&pty->child_input);
    winxterm_pty_close_handle(&pty->child_output);
}

HRESULT winxterm_pty_resize(WinxtermPty *pty, COORD size)
{
    if (pty == 0) return E_INVALIDARG;
    if (pty->backend == WINXTERM_PTY_BACKEND_SHIM) return S_FALSE;
    if (pty->pseudo_console == 0) return E_HANDLE;
    return ResizePseudoConsole(pty->pseudo_console, size);
}

void winxterm_pty_dispose(WinxtermPty *pty)
{
    if (pty == 0) return;
    winxterm_pty_finish_launch(pty);
    if (pty->pseudo_console != 0) ClosePseudoConsole(pty->pseudo_console);
    memset(pty, 0, sizeof(*pty));
}
