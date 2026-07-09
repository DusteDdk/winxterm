#include "winxterm_host.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const DWORD WINXTERM_HOST_SHUTDOWN_GRACE_MS = 1500u;
static const DWORD WINXTERM_HOST_TERMINATE_WAIT_MS = 1000u;
static const DWORD WINXTERM_HOST_EXIT_DRAIN_MS = 100u;
enum {
    WINXTERM_HOST_OUTPUT_READ_CHUNK_BYTES = 4096u,
    WINXTERM_HOST_OUTPUT_BATCH_MAX_BYTES = 64u * 1024u
};
static const uint64_t WINXTERM_HOST_OUTPUT_BATCH_MAX_NS = 500000ull;

typedef struct WinxtermHostContext {
    WinxtermBridge *bridge;
    HPCON pseudo_console;
    PROCESS_INFORMATION process;
    HANDLE input_write;
    HANDLE output_read;
    HANDLE shutdown_event;
    DWORD exit_code;
    int last_resize_failure_columns;
    int last_resize_failure_rows;
    HRESULT last_resize_failure_result;
    DWORD last_resize_failure_log_tick;
    bool child_exited;
    bool force_terminated;
} WinxtermHostContext;

static size_t winxterm_host_command_length(const wchar_t * const *argv, int argc)
{
    size_t total = 1u;
    for (int i = 0; i < argc; ++i) {
        if (argv[i] != 0) {
            total += wcslen(argv[i]) * 2u + 4u;
        }
    }
    return total;
}

static bool winxterm_host_append_quoted_arg(wchar_t *buffer,
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

static wchar_t *winxterm_host_build_command_line(const wchar_t * const *argv, int argc)
{
    if (argv == 0 || argc <= 0) {
        return 0;
    }
    size_t capacity = winxterm_host_command_length(argv, argc);
    wchar_t *command_line = (wchar_t *)calloc(capacity, sizeof(*command_line));
    if (command_line == 0) {
        return 0;
    }
    size_t offset = 0u;
    for (int i = 0; i < argc; ++i) {
        if (!winxterm_host_append_quoted_arg(command_line, capacity, &offset, argv[i])) {
            free(command_line);
            return 0;
        }
    }
    return command_line;
}

static bool winxterm_host_env_name_equals(const wchar_t *entry, const wchar_t *name)
{
    size_t name_length = name != 0 ? wcslen(name) : 0u;
    if (entry == 0 || name == 0 || name_length == 0u) {
        return false;
    }
    return _wcsnicmp(entry, name, name_length) == 0 && entry[name_length] == L'=';
}

static bool winxterm_host_env_should_override(const wchar_t *entry)
{
    return winxterm_host_env_name_equals(entry, L"TERM") ||
           winxterm_host_env_name_equals(entry, L"COLORTERM") ||
           winxterm_host_env_name_equals(entry, L"TERM_PROGRAM") ||
           winxterm_host_env_name_equals(entry, L"TERM_PROGRAM_VERSION");
}

static wchar_t *winxterm_host_build_environment(void)
{
    static const wchar_t term[] = L"TERM=xterm-256color";
    static const wchar_t colorterm[] = L"COLORTERM=truecolor";
    static const wchar_t term_program[] = L"TERM_PROGRAM=winxterm";
    static const wchar_t term_program_version[] = L"TERM_PROGRAM_VERSION=0.1.0";
    const wchar_t *overrides[] = {
        term,
        colorterm,
        term_program,
        term_program_version,
    };

    wchar_t *environment = GetEnvironmentStringsW();
    if (environment == 0) {
        return 0;
    }

    size_t total = 1u;
    for (const wchar_t *entry = environment; *entry != L'\0'; entry += wcslen(entry) + 1u) {
        if (!winxterm_host_env_should_override(entry)) {
            total += wcslen(entry) + 1u;
        }
    }
    for (size_t i = 0u; i < sizeof(overrides) / sizeof(overrides[0]); ++i) {
        total += wcslen(overrides[i]) + 1u;
    }

    wchar_t *block = (wchar_t *)calloc(total, sizeof(*block));
    if (block == 0) {
        FreeEnvironmentStringsW(environment);
        return 0;
    }

    size_t offset = 0u;
    for (const wchar_t *entry = environment; *entry != L'\0'; entry += wcslen(entry) + 1u) {
        if (!winxterm_host_env_should_override(entry)) {
            size_t length = wcslen(entry);
            memcpy(block + offset, entry, (length + 1u) * sizeof(*block));
            offset += length + 1u;
        }
    }
    for (size_t i = 0u; i < sizeof(overrides) / sizeof(overrides[0]); ++i) {
        size_t length = wcslen(overrides[i]);
        memcpy(block + offset, overrides[i], (length + 1u) * sizeof(*block));
        offset += length + 1u;
    }
    block[offset] = L'\0';
    FreeEnvironmentStringsW(environment);
    return block;
}

static bool winxterm_host_queue_output(WinxtermBridge *bridge,
                                       const uint8_t *bytes,
                                       size_t byte_count,
                                       HANDLE shutdown_event)
{
    winxterm_bridge_note_output_batch(bridge, byte_count);
    return winxterm_bridge_enqueue_output_wait(bridge, bytes, byte_count, shutdown_event);
}

static void winxterm_host_close_handle(HANDLE *handle)
{
    if (handle != 0 && *handle != 0 && *handle != INVALID_HANDLE_VALUE) {
        CloseHandle(*handle);
        *handle = 0;
    }
}

static bool winxterm_host_create_pipes(HANDLE *input_read,
                                       HANDLE *input_write,
                                       HANDLE *output_read,
                                       HANDLE *output_write)
{
    SECURITY_ATTRIBUTES security;
    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    if (!CreatePipe(input_read, input_write, &security, 0)) {
        return false;
    }
    if (!CreatePipe(output_read, output_write, &security, 0)) {
        winxterm_host_close_handle(input_read);
        winxterm_host_close_handle(input_write);
        return false;
    }
    SetHandleInformation(*input_write, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(*output_read, HANDLE_FLAG_INHERIT, 0);
    return true;
}

static bool winxterm_host_prepare_startup(HPCON pseudo_console,
                                          STARTUPINFOEXW *startup,
                                          LPPROC_THREAD_ATTRIBUTE_LIST *attribute_list)
{
    SIZE_T attribute_size = 0;
    memset(startup, 0, sizeof(*startup));
    startup->StartupInfo.cb = sizeof(*startup);

    InitializeProcThreadAttributeList(0, 1, 0, &attribute_size);
    *attribute_list = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attribute_size);
    if (*attribute_list == 0) {
        return false;
    }
    if (!InitializeProcThreadAttributeList(*attribute_list, 1, 0, &attribute_size)) {
        HeapFree(GetProcessHeap(), 0, *attribute_list);
        *attribute_list = 0;
        return false;
    }
    if (!UpdateProcThreadAttribute(*attribute_list,
                                   0,
                                   PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   pseudo_console,
                                   sizeof(pseudo_console),
                                   0,
                                   0)) {
        DeleteProcThreadAttributeList(*attribute_list);
        HeapFree(GetProcessHeap(), 0, *attribute_list);
        *attribute_list = 0;
        return false;
    }
    startup->lpAttributeList = *attribute_list;
    return true;
}

static void winxterm_host_destroy_startup(LPPROC_THREAD_ATTRIBUTE_LIST attribute_list)
{
    if (attribute_list != 0) {
        DeleteProcThreadAttributeList(attribute_list);
        HeapFree(GetProcessHeap(), 0, attribute_list);
    }
}

typedef struct WinxtermHostStandardHandles {
    HANDLE input;
    HANDLE output;
    HANDLE error;
} WinxtermHostStandardHandles;

static WinxtermHostStandardHandles winxterm_host_clear_standard_handles(void)
{
    WinxtermHostStandardHandles handles;
    handles.input = GetStdHandle(STD_INPUT_HANDLE);
    handles.output = GetStdHandle(STD_OUTPUT_HANDLE);
    handles.error = GetStdHandle(STD_ERROR_HANDLE);
    /* Prevent children from inheriting the launching shell's stdio instead of
       the pseudoconsole handles assigned by PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE. */
    (void)SetStdHandle(STD_INPUT_HANDLE, 0);
    (void)SetStdHandle(STD_OUTPUT_HANDLE, 0);
    (void)SetStdHandle(STD_ERROR_HANDLE, 0);
    return handles;
}

static void winxterm_host_restore_standard_handles(WinxtermHostStandardHandles handles)
{
    (void)SetStdHandle(STD_INPUT_HANDLE, handles.input);
    (void)SetStdHandle(STD_OUTPUT_HANDLE, handles.output);
    (void)SetStdHandle(STD_ERROR_HANDLE, handles.error);
}

static bool winxterm_host_should_log_resize_failure(WinxtermHostContext *host,
                                                    int columns,
                                                    int rows,
                                                    HRESULT result)
{
    if (host == 0) {
        return false;
    }

    DWORD now = GetTickCount();
    bool same_failure = host->last_resize_failure_columns == columns &&
                        host->last_resize_failure_rows == rows &&
                        host->last_resize_failure_result == result;
    if (same_failure && now - host->last_resize_failure_log_tick < 1000u) {
        return false;
    }

    host->last_resize_failure_columns = columns;
    host->last_resize_failure_rows = rows;
    host->last_resize_failure_result = result;
    host->last_resize_failure_log_tick = now;
    return true;
}

static void winxterm_host_apply_pending_resize(WinxtermHostContext *host)
{
    if (host == 0 || host->bridge == 0) {
        return;
    }

    WinxtermBridge *bridge = host->bridge;
    int columns = 0;
    int rows = 0;
    if (winxterm_bridge_peek_pending_resize(bridge, &columns, &rows)) {
        COORD size;
        size.X = (SHORT)(columns > 0 ? columns : 1);
        size.Y = (SHORT)(rows > 0 ? rows : 1);
        HRESULT result = ResizePseudoConsole(host->pseudo_console, size);
        if (FAILED(result)) {
            if (winxterm_host_should_log_resize_failure(host, columns, rows, result)) {
                winxterm_log_writef(bridge->log,
                                    "ConPTY resize failed, size=%dx%d hr=0x%08lx",
                                    columns,
                                    rows,
                                    (unsigned long)result);
            }
        } else {
            host->last_resize_failure_columns = 0;
            host->last_resize_failure_rows = 0;
            host->last_resize_failure_result = S_OK;
            host->last_resize_failure_log_tick = 0u;
            (void)winxterm_bridge_ack_pending_resize(bridge, columns, rows);
        }
    }
}

static bool winxterm_host_write_pending_input(WinxtermBridge *bridge, HANDLE input_write)
{
    uint8_t input[512];
    size_t input_count = winxterm_bridge_read_input(bridge, input, sizeof(input));
    if (input_count == 0u) {
        return true;
    }
    DWORD written = 0;
    if (!WriteFile(input_write, input, (DWORD)input_count, &written, 0)) {
        winxterm_log_writef(bridge->log, "ConPTY input write failed, error=%lu", (unsigned long)GetLastError());
        return false;
    }
    return written == (DWORD)input_count;
}

static bool winxterm_host_read_available_output(WinxtermHostContext *host, bool *read_any)
{
    uint8_t stack_output[WINXTERM_HOST_OUTPUT_READ_CHUNK_BYTES];
    uint8_t *output = stack_output;
    size_t output_capacity = sizeof(stack_output);
    size_t output_count = 0u;
    uint64_t batch_start_ns = 0u;
    bool read_ok = true;
    DWORD available = 0;
    if (read_any != 0) {
        *read_any = false;
    }

    for (;;) {
        available = 0;
        if (!PeekNamedPipe(host->output_read, 0, 0, 0, &available, 0)) {
            DWORD error = GetLastError();
            if (error != ERROR_BROKEN_PIPE && error != ERROR_PIPE_NOT_CONNECTED) {
                winxterm_log_writef(host->bridge->log,
                                    "ConPTY output peek failed, error=%lu",
                                    (unsigned long)error);
                read_ok = false;
            }
            break;
        }
        if (available == 0u) {
            break;
        }

        if (output_count == output_capacity &&
            output_capacity < WINXTERM_HOST_OUTPUT_BATCH_MAX_BYTES) {
            size_t new_capacity = output_capacity * 2u;
            if (new_capacity > WINXTERM_HOST_OUTPUT_BATCH_MAX_BYTES) {
                new_capacity = WINXTERM_HOST_OUTPUT_BATCH_MAX_BYTES;
            }
            uint8_t *new_output = (uint8_t *)malloc(new_capacity);
            if (new_output == 0) {
                break;
            }
            memcpy(new_output, output, output_count);
            if (output != stack_output) {
                free(output);
            }
            output = new_output;
            output_capacity = new_capacity;
        }
        if (output_count == output_capacity) {
            break;
        }

        size_t remaining = output_capacity - output_count;
        DWORD to_read = available;
        if (to_read > WINXTERM_HOST_OUTPUT_READ_CHUNK_BYTES) {
            to_read = WINXTERM_HOST_OUTPUT_READ_CHUNK_BYTES;
        }
        if ((size_t)to_read > remaining) {
            to_read = (DWORD)remaining;
        }
        DWORD bytes_read = 0;
        if (!ReadFile(host->output_read, output + output_count, to_read, &bytes_read, 0)) {
            DWORD error = GetLastError();
            if (error != ERROR_BROKEN_PIPE && error != ERROR_PIPE_NOT_CONNECTED) {
                winxterm_log_writef(host->bridge->log,
                                    "ConPTY output read failed, error=%lu",
                                    (unsigned long)error);
                read_ok = false;
            }
            break;
        }
        if (bytes_read == 0u) {
            break;
        }

        if (output_count == 0u) {
            batch_start_ns = winxterm_bridge_timestamp_ns();
        }
        output_count += (size_t)bytes_read;
        if (read_any != 0) {
            *read_any = true;
        }
        if (output_count >= WINXTERM_HOST_OUTPUT_BATCH_MAX_BYTES) {
            break;
        }
        uint64_t now_ns = winxterm_bridge_timestamp_ns();
        if (now_ns >= batch_start_ns &&
            now_ns - batch_start_ns >= WINXTERM_HOST_OUTPUT_BATCH_MAX_NS) {
            break;
        }
    }

    if (output_count == 0u) {
        if (output != stack_output) {
            free(output);
        }
        return read_ok;
    }

    bool feed_ok = true;
    if (winxterm_bridge_is_headless(host->bridge)) {
        winxterm_bridge_note_headless_output(host->bridge, output, output_count);
    } else {
        feed_ok = winxterm_host_queue_output(host->bridge,
                                             output,
                                             output_count,
                                             host->shutdown_event);
    }
    if (output != stack_output) {
        free(output);
    }
    return read_ok && feed_ok;
}

static void winxterm_host_drain_available_output(WinxtermHostContext *host, DWORD drain_ms)
{
    DWORD start = GetTickCount();
    while (GetTickCount() - start < drain_ms) {
        bool read_any = false;
        if (!winxterm_host_read_available_output(host, &read_any)) {
            return;
        }
        if (!read_any) {
            Sleep(10);
        }
    }
}

static void winxterm_host_request_child_shutdown(WinxtermHostContext *host)
{
    if (host == 0 || host->process.hProcess == 0) {
        return;
    }

    winxterm_bridge_set_host_state(host->bridge, WINXTERM_HOST_STATE_CLEANING_UP);
    winxterm_host_close_handle(&host->input_write);
    DWORD wait_result = WaitForSingleObject(host->process.hProcess, WINXTERM_HOST_SHUTDOWN_GRACE_MS);
    if (wait_result == WAIT_TIMEOUT) {
        winxterm_log_writef(host->bridge->log,
                            "ConPTY child did not exit after %lu ms, terminating",
                            (unsigned long)WINXTERM_HOST_SHUTDOWN_GRACE_MS);
        TerminateProcess(host->process.hProcess, 1);
        host->force_terminated = true;
        (void)WaitForSingleObject(host->process.hProcess, WINXTERM_HOST_TERMINATE_WAIT_MS);
    }
}

static void winxterm_host_cleanup_context(WinxtermHostContext *host)
{
    if (host == 0) {
        return;
    }

    winxterm_host_close_handle(&host->process.hThread);
    winxterm_host_close_handle(&host->process.hProcess);
    if (host->pseudo_console != 0) {
        ClosePseudoConsole(host->pseudo_console);
        host->pseudo_console = 0;
    }
    winxterm_host_close_handle(&host->input_write);
    winxterm_host_close_handle(&host->output_read);
}

DWORD winxterm_host_run_conpty(WinxtermBridge *bridge,
                               const wchar_t * const *argv,
                               int argc,
                               HANDLE shutdown_event)
{
    return winxterm_host_run_conpty_in_directory(bridge, argv, argc, 0, shutdown_event);
}

DWORD winxterm_host_run_conpty_in_directory(WinxtermBridge *bridge,
                                            const wchar_t * const *argv,
                                            int argc,
                                            const wchar_t *current_directory,
                                            HANDLE shutdown_event)
{
    if (bridge == 0 || argv == 0 || argc <= 0) {
        return 1;
    }

    WinxtermHostContext host;
    memset(&host, 0, sizeof(host));
    host.bridge = bridge;
    host.shutdown_event = shutdown_event;
    host.exit_code = 1;
    winxterm_bridge_set_host_starting(bridge);

    HANDLE input_read = 0;
    HANDLE output_read = 0;
    HANDLE output_write = 0;
    if (!winxterm_host_create_pipes(&input_read, &host.input_write, &output_read, &output_write)) {
        winxterm_log_writef(bridge->log, "ConPTY pipe creation failed, error=%lu", (unsigned long)GetLastError());
        winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
        return 1;
    }
    host.output_read = output_read;

    COORD size;
    size.X = (SHORT)(bridge->screen.columns > 0 ? bridge->screen.columns : WINXTERM_TERMINAL_COLUMNS);
    size.Y = (SHORT)(bridge->screen.rows > 0 ? bridge->screen.rows : WINXTERM_TERMINAL_ROWS);
    HRESULT hr = CreatePseudoConsole(size, input_read, output_write, 0, &host.pseudo_console);
    winxterm_host_close_handle(&input_read);
    winxterm_host_close_handle(&output_write);
    if (FAILED(hr)) {
        winxterm_log_writef(bridge->log, "CreatePseudoConsole failed, hr=0x%08lx", (unsigned long)hr);
        winxterm_host_cleanup_context(&host);
        winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
        return 1;
    }

    wchar_t *command_line = winxterm_host_build_command_line(argv, argc);
    if (command_line == 0) {
        winxterm_host_cleanup_context(&host);
        winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
        return 1;
    }

    wchar_t *environment = winxterm_host_build_environment();
    if (environment == 0) {
        free(command_line);
        winxterm_host_cleanup_context(&host);
        winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
        return 1;
    }

    STARTUPINFOEXW startup;
    LPPROC_THREAD_ATTRIBUTE_LIST attribute_list = 0;
    if (!winxterm_host_prepare_startup(host.pseudo_console, &startup, &attribute_list)) {
        winxterm_log_writef(bridge->log, "ConPTY startup attribute setup failed, error=%lu", (unsigned long)GetLastError());
        free(command_line);
        free(environment);
        winxterm_host_cleanup_context(&host);
        winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
        return 1;
    }

    WinxtermHostStandardHandles standard_handles = winxterm_host_clear_standard_handles();
    BOOL created = CreateProcessW(0,
                                  command_line,
                                  0,
                                  0,
                                  FALSE,
                                  EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
                                  environment,
                                  current_directory != 0 && current_directory[0] != L'\0' ? current_directory : 0,
                                  &startup.StartupInfo,
                                  &host.process);
    winxterm_host_restore_standard_handles(standard_handles);
    free(command_line);
    free(environment);
    winxterm_host_destroy_startup(attribute_list);
    if (!created) {
        winxterm_log_writef(bridge->log, "ConPTY client launch failed, error=%lu", (unsigned long)GetLastError());
        winxterm_host_cleanup_context(&host);
        winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
        return 1;
    }
    winxterm_bridge_set_host_child(bridge, host.process.dwProcessId, argv[0]);
    winxterm_log_writef(bridge->log,
                        "ConPTY client started pid=%lu",
                        (unsigned long)host.process.dwProcessId);
    bool running = true;
    while (running) {
        bool headless = winxterm_bridge_is_headless(bridge);
        if (!headless) {
            winxterm_host_apply_pending_resize(&host);
            if (!winxterm_host_write_pending_input(bridge, host.input_write)) {
                if (WaitForSingleObject(shutdown_event, 0) != WAIT_OBJECT_0 &&
                    !winxterm_bridge_terminate_requested(bridge)) {
                    winxterm_bridge_set_host_state(bridge, WINXTERM_HOST_STATE_FAILED);
                }
                running = false;
                break;
            }
        }

        bool read_any = false;
        if (!winxterm_host_read_available_output(&host, &read_any)) {
            if (WaitForSingleObject(host.process.hProcess, 0) == WAIT_OBJECT_0) {
                host.child_exited = true;
                winxterm_bridge_set_host_state(bridge, WINXTERM_HOST_STATE_CHILD_EXITED);
            } else if (!winxterm_bridge_is_headless(bridge)) {
                winxterm_bridge_set_host_state(bridge, WINXTERM_HOST_STATE_FAILED);
            }
            running = false;
        } else if (read_any) {
            continue;
        } else {
            HANDLE waits[3] = {host.process.hProcess, shutdown_event, bridge->input_ready_event};
            DWORD wait_result = WaitForMultipleObjects(3, waits, FALSE, 10);
            if (wait_result == WAIT_OBJECT_0) {
                host.child_exited = true;
                winxterm_bridge_set_host_state(bridge, WINXTERM_HOST_STATE_CHILD_EXITED);
                running = false;
            } else if (wait_result == WAIT_OBJECT_0 + 1u) {
                winxterm_bridge_request_terminate(bridge);
                running = false;
            } else if (wait_result == WAIT_FAILED) {
                winxterm_log_writef(bridge->log,
                                    "ConPTY host wait failed, error=%lu",
                                    (unsigned long)GetLastError());
                winxterm_bridge_set_host_state(bridge, WINXTERM_HOST_STATE_FAILED);
                running = false;
            }
        }
    }
    bool shutdown_requested = WaitForSingleObject(shutdown_event, 0) == WAIT_OBJECT_0 ||
                              winxterm_bridge_terminate_requested(bridge);
    bool child_still_running = WaitForSingleObject(host.process.hProcess, 0) != WAIT_OBJECT_0;
    if ((shutdown_requested || (!host.child_exited && child_still_running &&
                                !winxterm_bridge_is_headless(bridge))) &&
        !host.child_exited) {
        winxterm_host_request_child_shutdown(&host);
    }

        winxterm_host_drain_available_output(&host, WINXTERM_HOST_EXIT_DRAIN_MS);
    GetExitCodeProcess(host.process.hProcess, &host.exit_code);
    winxterm_log_writef(bridge->log,
                        "ConPTY client exited code=%lu forced=%s",
                        (unsigned long)host.exit_code,
                        host.force_terminated ? "yes" : "no");
    if (winxterm_bridge_is_headless(bridge)) {
        WinxtermBridgeDiagnostics bridge_diagnostics;
        winxterm_bridge_copy_diagnostics(bridge, &bridge_diagnostics);
        winxterm_log_writef(bridge->log,
                            "headless output summary: bytes=%llu lines=%u",
                            bridge_diagnostics.headless_output_bytes,
                            bridge_diagnostics.headless_output_lines);
    }

    winxterm_host_cleanup_context(&host);
    winxterm_bridge_clear_host_child(bridge,
                                     shutdown_requested && host.force_terminated ?
                                         WINXTERM_HOST_STATE_STOPPED :
                                         WINXTERM_HOST_STATE_CHILD_EXITED);
    return host.exit_code;
}
