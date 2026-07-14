#include "winxterm_threads.h"

#include "winxterm_app.h"
#include "winxterm_bridge.h"
#include "winxterm_client.h"
#include "winxterm_scale.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define WINXTERM_PATH_BUFFER_COUNT 32768u

typedef struct WinxtermThreadRuntime {
    HINSTANCE instance;
    WinxtermLog *log;
    HANDLE shutdown_event;
    HANDLE input_thread;
    HANDLE client_thread;
    DWORD input_thread_id;
    DWORD client_thread_id;
    int input_exit_code;
    int client_exit_code;
    const WinxtermOptions *options;
    const wchar_t * const *argv;
    WinxtermBridge bridge;
} WinxtermThreadRuntime;

static bool winxterm_threads_file_exists(const wchar_t *path)
{
    if (path == 0 || path[0] == L'\0') {
        return false;
    }
    DWORD attributes = GetFileAttributesW(path);
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool winxterm_threads_path_is_executable(const wchar_t *path)
{
    if (!winxterm_threads_file_exists(path)) {
        return false;
    }
    const wchar_t *extension = wcsrchr(path, L'.');
    return extension != 0 &&
           (_wcsicmp(extension, L".exe") == 0 || _wcsicmp(extension, L".com") == 0);
}

static bool winxterm_threads_search_executable(const wchar_t *name, wchar_t *buffer, DWORD buffer_count)
{
    if (name == 0 || name[0] == L'\0' || buffer == 0 || buffer_count == 0u) {
        return false;
    }

    DWORD length = SearchPathW(0, name, 0, buffer_count, buffer, 0);
    if (length != 0u && length < buffer_count && winxterm_threads_path_is_executable(buffer)) {
        return true;
    }

    length = SearchPathW(0, name, L".exe", buffer_count, buffer, 0);
    if (length != 0u && length < buffer_count && winxterm_threads_path_is_executable(buffer)) {
        return true;
    }

    buffer[0] = L'\0';
    return false;
}

static bool winxterm_threads_own_directory_file(const wchar_t *name, wchar_t *buffer, DWORD buffer_count)
{
    if (name == 0 || buffer == 0 || buffer_count == 0u) {
        return false;
    }
    DWORD length = GetModuleFileNameW(0, buffer, buffer_count);
    if (length == 0u || length >= buffer_count) {
        buffer[0] = L'\0';
        return false;
    }
    wchar_t *slash = wcsrchr(buffer, L'\\');
    if (slash == 0) {
        buffer[0] = L'\0';
        return false;
    }
    size_t directory_length = (size_t)(slash - buffer + 1);
    size_t name_length = wcslen(name);
    if (directory_length + name_length >= buffer_count) {
        buffer[0] = L'\0';
        return false;
    }
    memcpy(buffer + directory_length, name, (name_length + 1u) * sizeof(*buffer));
    return true;
}

static bool winxterm_threads_write_notice(WinxtermBridge *bridge,
                                          HANDLE shutdown_event,
                                          const wchar_t *notice)
{
    if (bridge == 0 || notice == 0 || notice[0] == L'\0') {
        return true;
    }
    int byte_count = WideCharToMultiByte(CP_UTF8, 0, notice, -1, 0, 0, 0, 0);
    if (byte_count <= 0) {
        return false;
    }
    char *utf8 = (char *)calloc((size_t)byte_count + 3u, sizeof(*utf8));
    if (utf8 == 0) {
        return false;
    }
    bool ok = WideCharToMultiByte(CP_UTF8, 0, notice, -1, utf8, byte_count, 0, 0) > 0;
    if (ok) {
        size_t length = strlen(utf8);
        utf8[length++] = '\r';
        utf8[length++] = '\n';
        utf8[length] = '\0';
        ok = winxterm_client_write_bytes_with_policy(bridge,
                                                     0,
                                                     (const uint8_t *)utf8,
                                                     length,
                                                     shutdown_event,
                                                     true);
    }
    free(utf8);
    return ok;
}

static DWORD winxterm_threads_run_default_client(WinxtermThreadRuntime *runtime,
                                                 const wchar_t * const *extra_argv,
                                                 int extra_argc)
{
    winxterm_log_writef(runtime->log, "default client: external dstshell");
    const wchar_t *startup_notice =
        runtime != 0 && runtime->options != 0 ? runtime->options->startup_notice : 0;
    wchar_t notice[512];
    notice[0] = L'\0';
    if (extra_argv != 0 && extra_argc > 0) {
        if (startup_notice != 0 && startup_notice[0] != L'\0') {
            _snwprintf_s(notice,
                         512u,
                         _TRUNCATE,
                         L"%ls\r\n%ls: client executable not found; starting dstshell",
                         startup_notice,
                         extra_argv[0] != 0 ? extra_argv[0] : L"(null)");
        } else {
            _snwprintf_s(notice,
                         512u,
                         _TRUNCATE,
                         L"%ls: client executable not found; starting dstshell",
                         extra_argv[0] != 0 ? extra_argv[0] : L"(null)");
        }
        startup_notice = notice;
    }

    wchar_t dstshell_path[WINXTERM_PATH_BUFFER_COUNT];
    if (!winxterm_threads_own_directory_file(L"dstshell.exe",
                                             dstshell_path,
                                             WINXTERM_PATH_BUFFER_COUNT) ||
        !winxterm_threads_path_is_executable(dstshell_path)) {
        winxterm_log_writef(runtime->log, "default dstshell.exe not found beside winxterm");
        (void)winxterm_threads_write_notice(&runtime->bridge,
                                            runtime->shutdown_event,
                                            L"dstshell.exe was not found beside winxterm.exe");
        return 1;
    }
    (void)winxterm_threads_write_notice(&runtime->bridge, runtime->shutdown_event, startup_notice);
    const wchar_t *dstshell_argv[] = {dstshell_path};
    return winxterm_client_run_process(&runtime->bridge,
                                       dstshell_argv,
                                       1,
                                       runtime->shutdown_event);
}

static DWORD winxterm_threads_run_positional_client(WinxtermThreadRuntime *runtime)
{
    if (runtime == 0 || runtime->options == 0 || runtime->options->client_argc <= 0) {
        return winxterm_threads_run_default_client(runtime, 0, 0);
    }

    const wchar_t * const *client_argv = runtime->argv + runtime->options->client_arg_start;
    wchar_t resolved_client[WINXTERM_PATH_BUFFER_COUNT];
    if (winxterm_threads_search_executable(client_argv[0], resolved_client, WINXTERM_PATH_BUFFER_COUNT)) {
        const wchar_t **resolved_argv =
            (const wchar_t **)calloc((size_t)runtime->options->client_argc, sizeof(*resolved_argv));
        if (resolved_argv == 0) {
            return 1;
        }
        resolved_argv[0] = resolved_client;
        for (int i = 1; i < runtime->options->client_argc; ++i) {
            resolved_argv[i] = client_argv[i];
        }
        DWORD exit_code = winxterm_client_run_process(&runtime->bridge,
                                                      resolved_argv,
                                                      runtime->options->client_argc,
                                                      runtime->shutdown_event);
        free(resolved_argv);
        return exit_code;
    }

    winxterm_log_writef(runtime->log, "client not found, using default client fallback");
    return winxterm_threads_run_default_client(runtime, client_argv, runtime->options->client_argc);
}

static DWORD WINAPI winxterm_input_thread_proc(void *context)
{
    WinxtermThreadRuntime *runtime = (WinxtermThreadRuntime *)context;
    winxterm_log_writef(runtime->log, "input thread started id=%lu", (unsigned long)GetCurrentThreadId());

    WinxtermApp app;
    if (!winxterm_app_init(&app,
                           runtime->instance,
                           runtime->log,
                           &runtime->bridge,
                           runtime->shutdown_event,
                           runtime->options != 0 ?
                               runtime->options->display_scale : WINXTERM_DEFAULT_DISPLAY_SCALE)) {
        winxterm_log_writef(runtime->log, "input thread failed to initialize window");
        runtime->input_exit_code = 1;
        SetEvent(runtime->shutdown_event);
        return 1;
    }

    int exit_code = winxterm_app_run(&app);
    winxterm_app_dispose(&app);
    runtime->input_exit_code = exit_code;
    if (winxterm_bridge_is_headless(&runtime->bridge)) {
        winxterm_log_writef(runtime->log, "input thread exited while host remains headless");
    } else {
        SetEvent(runtime->shutdown_event);
    }
    winxterm_log_writef(runtime->log,
                        "input thread exiting id=%lu exit_code=%d",
                        (unsigned long)GetCurrentThreadId(),
                        exit_code);
    return (DWORD)exit_code;
}

static DWORD WINAPI winxterm_client_thread_proc(void *context)
{
    WinxtermThreadRuntime *runtime = (WinxtermThreadRuntime *)context;
    DWORD thread_id = GetCurrentThreadId();
    winxterm_log_writef(runtime->log, "client thread started id=%lu", (unsigned long)thread_id);

    if (runtime->bridge.hwnd_ready_event != 0) {
        HANDLE waits[2] = {runtime->bridge.hwnd_ready_event, runtime->shutdown_event};
        DWORD ready_result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (ready_result != WAIT_OBJECT_0) {
            runtime->client_exit_code = ready_result == WAIT_OBJECT_0 + 1u ? 0 : 1;
            winxterm_log_writef(runtime->log,
                                "client thread exiting before window ready id=%lu exit_code=%d",
                                (unsigned long)thread_id,
                                runtime->client_exit_code);
            return (DWORD)runtime->client_exit_code;
        }
    }

    if (runtime->options != 0 && runtime->options->demo) {
        runtime->client_exit_code = (int)winxterm_client_run_demo(&runtime->bridge, runtime->shutdown_event);
    } else if (runtime->options != 0 && runtime->options->client_argc > 0) {
        runtime->client_exit_code = (int)winxterm_threads_run_positional_client(runtime);
    } else {
        runtime->client_exit_code = (int)winxterm_threads_run_default_client(runtime, 0, 0);
    }

    winxterm_log_writef(runtime->log,
                        "client thread exiting id=%lu exit_code=%d",
                        (unsigned long)thread_id,
                        runtime->client_exit_code);
    SetEvent(runtime->shutdown_event);
    EnterCriticalSection(&runtime->bridge.input_lock);
    HWND hwnd = runtime->bridge.hwnd;
    bool headless = runtime->bridge.host_headless;
    LeaveCriticalSection(&runtime->bridge.input_lock);
    if (hwnd != 0 && !headless) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
    return (DWORD)runtime->client_exit_code;
}

static void winxterm_close_handle(HANDLE *handle)
{
    if (handle != 0 && *handle != 0) {
        CloseHandle(*handle);
        *handle = 0;
    }
}

int winxterm_threads_run(HINSTANCE instance,
                         WinxtermLog *log,
                         const WinxtermOptions *options,
                         const wchar_t * const *argv)
{
    WinxtermThreadRuntime runtime;
    memset(&runtime, 0, sizeof(runtime));
    runtime.instance = instance;
    runtime.log = log;
    runtime.options = options;
    runtime.argv = argv;
    runtime.input_exit_code = 1;
    runtime.client_exit_code = 1;

    if (!winxterm_bridge_init(&runtime.bridge,
                              log,
                              WINXTERM_TERMINAL_COLUMNS,
                              WINXTERM_TERMINAL_ROWS)) {
        winxterm_log_writef(log, "failed to initialize shared screen bridge");
        return 1;
    }
    if (options != 0) {
        runtime.bridge.show_render_stats_in_title = options->demo;
        winxterm_bridge_set_unpainted_line_limit(&runtime.bridge, options->unpainted_line_limit);
        if (options->macro_path != 0 &&
            !winxterm_bridge_request_macro(&runtime.bridge, options->macro_path)) {
            winxterm_log_writef(log, "failed to queue startup macro");
        }
    }

    runtime.shutdown_event = CreateEventW(0, TRUE, FALSE, 0);
    if (runtime.shutdown_event == 0) {
        winxterm_log_writef(log, "failed to create shutdown event, error=%lu", (unsigned long)GetLastError());
        winxterm_bridge_dispose(&runtime.bridge);
        return 1;
    }

    runtime.input_thread = CreateThread(0,
                                        0,
                                        winxterm_input_thread_proc,
                                        &runtime,
                                        0,
                                        &runtime.input_thread_id);
    if (runtime.input_thread == 0) {
        winxterm_log_writef(log, "failed to create input thread, error=%lu", (unsigned long)GetLastError());
        winxterm_close_handle(&runtime.shutdown_event);
        winxterm_bridge_dispose(&runtime.bridge);
        return 1;
    }

    runtime.client_thread = CreateThread(0,
                                         0,
                                         winxterm_client_thread_proc,
                                         &runtime,
                                         0,
                                         &runtime.client_thread_id);
    if (runtime.client_thread == 0) {
        winxterm_log_writef(log, "failed to create client thread, error=%lu", (unsigned long)GetLastError());
        SetEvent(runtime.shutdown_event);
        WaitForSingleObject(runtime.input_thread, INFINITE);
        winxterm_close_handle(&runtime.input_thread);
        winxterm_close_handle(&runtime.shutdown_event);
        winxterm_bridge_dispose(&runtime.bridge);
        return 1;
    }

    WaitForSingleObject(runtime.input_thread, INFINITE);
    if (!winxterm_bridge_is_headless(&runtime.bridge)) {
        SetEvent(runtime.shutdown_event);
    }
    WaitForSingleObject(runtime.client_thread, INFINITE);

    winxterm_close_handle(&runtime.client_thread);
    winxterm_close_handle(&runtime.input_thread);
    winxterm_close_handle(&runtime.shutdown_event);
    winxterm_bridge_dispose(&runtime.bridge);
    return runtime.input_exit_code != 0 ? runtime.input_exit_code : runtime.client_exit_code;
}
