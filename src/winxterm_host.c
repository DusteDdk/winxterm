#include "winxterm_host.h"
#include "winxterm_job_channel.h"
#include "winxterm_job_journal.h"
#include "winxterm_job_plan.h"
#include "winxterm_terminal_session.h"
#include "winxterm_host_dispatch.h"
#include "winxterm_transfer_format.h"
#include "winxterm_managed_runtime.h"

#include <stdbool.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const DWORD WINXTERM_HOST_TERMINATE_WAIT_MS = 1000u;
static const DWORD WINXTERM_HOST_EXIT_DRAIN_MS = 100u;
enum {
    WINXTERM_HOST_OUTPUT_READ_CHUNK_BYTES = 4096u,
    WINXTERM_HOST_OUTPUT_BATCH_MAX_BYTES = 64u * 1024u
};
static const uint64_t WINXTERM_HOST_OUTPUT_BATCH_MAX_NS = 500000ull;

struct WinxtermHostContext {
    WinxtermBridge *bridge;
    HPCON pseudo_console;
    PROCESS_INFORMATION process;
    HANDLE process_job;
    HANDLE input_write;
    HANDLE output_read;
    HANDLE job_request_read;
    HANDLE job_reply_write;
    HANDLE job_control_thread;
    HANDLE root_kill_thread;
    HANDLE root_kill_cancel_event;
    uint64_t pending_root_kill_request_id;
    WinxtermHostClient *pending_root_kill_client;
    HANDLE shutdown_event;
    DWORD exit_code;
    int last_resize_failure_columns;
    int last_resize_failure_rows;
    HRESULT last_resize_failure_result;
    DWORD last_resize_failure_log_tick;
    bool child_exited;
    bool force_terminated;
    uint64_t root_job_id;
    WinxtermManagedRuntimeRegistry runtimes;
    uint64_t active_session_id;
    WinxtermTerminalSession root_session;
    bool root_is_shell;
    bool root_headless_shutdown_requested;
    bool root_exit_notice_shown;
    wchar_t root_executable[WINXTERM_JOB_DISPLAY_NAME_CAPACITY];
};

static WinxtermHostManagedChild *winxterm_host_find_child_locked(
    WinxtermHostContext *host, uint64_t id);

WinxtermJobManager *winxterm_host_dispatch_manager(WinxtermHostContext *host)
{
    return host != 0 ? &host->bridge->job_manager : 0;
}

bool winxterm_host_dispatch_request_ui(WinxtermHostContext *host)
{
    return host != 0 && winxterm_bridge_request_job_ui(host->bridge);
}

static bool winxterm_host_switch_session(WinxtermHostContext *host, uint64_t target_id);
static WinxtermHostManagedChild *winxterm_host_find_child_locked(WinxtermHostContext *host,
                                                                 uint64_t id);
static void winxterm_host_prune_removed_children(WinxtermHostContext *host);
static void winxterm_host_emit_file_summary(WinxtermHostContext *host,
                                            uint64_t bytes, ULONGLONG start_ms);
static HANDLE winxterm_host_managed_child_final_process(WinxtermHostManagedChild *child);
static bool winxterm_host_managed_child_all_exited(WinxtermHostManagedChild *child);

typedef struct WinxtermHostEventAuthorization {
    WinxtermHostContext *host;
    uint64_t job_id;
} WinxtermHostEventAuthorization;

static bool winxterm_host_event_authorized(uint64_t requester_id, void *context)
{
    WinxtermHostEventAuthorization *authorization =
        (WinxtermHostEventAuthorization *)context;
    return authorization != 0 &&
        winxterm_job_manager_authorized(&authorization->host->bridge->job_manager,
                                        requester_id, authorization->job_id);
}

static void winxterm_host_send_event(WinxtermHostContext *host, WinxtermJobEventKind kind,
                                    uint64_t job_id)
{
    if (host == 0) return;
    WinxtermManagedJobSnapshot job;
    if (!winxterm_job_manager_snapshot_one(&host->bridge->job_manager, job_id, &job)) return;
    uint8_t payload[128];
    size_t length = 0u;
    bool ok = winxterm_job_tlv_append_u32(payload, sizeof(payload), &length,
                                         WINXTERM_JOB_TLV_EVENT_KIND, (uint32_t)kind) &&
              winxterm_job_tlv_append_u64(payload, sizeof(payload), &length,
                                         WINXTERM_JOB_TLV_JOB_ID, job.id) &&
              winxterm_job_tlv_append_u32(payload, sizeof(payload), &length,
                                         WINXTERM_JOB_TLV_STATE, (uint32_t)job.state) &&
              winxterm_job_tlv_append_u32(payload, sizeof(payload), &length,
                                         WINXTERM_JOB_TLV_EXIT_CODE,
                                         job.has_exit_code ? job.exit_code : 0u);
    WinxtermJobFrameHeader header = {WINXTERM_JOB_PROTOCOL_VERSION, WINXTERM_JOB_MESSAGE_EVENT,
                                    0u, 0u, (uint32_t)length};
    if (ok) {
        WinxtermHostEventAuthorization authorization = {host, job_id};
        winxterm_job_coordinator_broadcast(&host->bridge->job_coordinator,
                                           &header, payload,
                                           winxterm_host_event_authorized,
                                           &authorization);
    }
}

static void winxterm_host_send_resync_event(WinxtermHostContext *host)
{
    if (host == 0) return;
    uint8_t payload[96];
    size_t length = 0u;
    bool ok = winxterm_job_tlv_append_u32(payload, sizeof(payload), &length,
                                         WINXTERM_JOB_TLV_EVENT_KIND,
                                         WINXTERM_JOB_EVENT_RESYNC_REQUIRED) &&
              winxterm_job_tlv_append_u64(payload, sizeof(payload), &length,
                                         WINXTERM_JOB_TLV_JOB_ID, 0u) &&
              winxterm_job_tlv_append_u32(payload, sizeof(payload), &length,
                                         WINXTERM_JOB_TLV_STATE, 0u) &&
              winxterm_job_tlv_append_u32(payload, sizeof(payload), &length,
                                         WINXTERM_JOB_TLV_EXIT_CODE, 0u);
    WinxtermJobFrameHeader header = {WINXTERM_JOB_PROTOCOL_VERSION,
                                    WINXTERM_JOB_MESSAGE_EVENT, 0u, 0u,
                                    (uint32_t)length};
    if (!ok) return;
    winxterm_job_coordinator_broadcast(&host->bridge->job_coordinator,
                                       &header, payload, 0, 0);
}

static uint64_t winxterm_host_take_foreground_request(WinxtermHostContext *host, uint64_t job_id,
                                                       WinxtermHostClient **client_out)
{
    if (host == 0 || job_id == 0u) return 0u;
    EnterCriticalSection(&host->runtimes.lock);
    WinxtermHostManagedChild *child = winxterm_host_find_child_locked(host, job_id);
    uint64_t request_id = child != 0 ? child->pending_foreground_request_id : 0u;
    if (client_out != 0) *client_out = child != 0 ? child->pending_foreground_client : 0;
    if (child != 0) {
        child->pending_foreground_request_id = 0u;
        child->pending_foreground_client = 0;
    }
    LeaveCriticalSection(&host->runtimes.lock);
    return request_id;
}

static void winxterm_host_complete_foreground_request(WinxtermHostContext *host,
                                                       uint64_t job_id,
                                                       uint32_t status,
                                                       bool has_exit_code,
                                                       uint32_t exit_code)
{
    WinxtermHostClient *client = 0;
    uint64_t request_id = winxterm_host_take_foreground_request(host, job_id, &client);
    if (request_id == 0u || client == 0 || !client->reply_lock_initialized ||
        client->reply_write == 0) return;
    uint8_t payload[64];
    size_t length = 0u;
    bool ok = winxterm_job_tlv_append_u32(payload, sizeof(payload), &length,
                                         WINXTERM_JOB_TLV_STATUS, status) &&
              winxterm_job_tlv_append_u64(payload, sizeof(payload), &length,
                                         WINXTERM_JOB_TLV_JOB_ID, job_id);
    if (ok && has_exit_code) {
        ok = winxterm_job_tlv_append_u32(payload, sizeof(payload), &length,
                                        WINXTERM_JOB_TLV_EXIT_CODE, exit_code);
    }
    WinxtermJobFrameHeader reply = {WINXTERM_JOB_PROTOCOL_VERSION,
                                    WINXTERM_JOB_MESSAGE_REPLY, 0u, request_id,
                                    (uint32_t)length};
    if (ok) {
        EnterCriticalSection(&client->reply_lock);
        (void)winxterm_job_channel_write(client->reply_write, &reply, payload);
        LeaveCriticalSection(&client->reply_lock);
    }
}

bool winxterm_host_defer_foreground_request(WinxtermHostContext *host,
                                            WinxtermHostClient *client,
                                            uint64_t job_id,
                                            uint64_t request_id)
{
    if (host == 0 || client == 0 || job_id == 0u || request_id == 0u) return false;
    EnterCriticalSection(&host->runtimes.lock);
    WinxtermHostManagedChild *spawned = winxterm_host_find_child_locked(host, job_id);
    if (spawned != 0) {
        spawned->pending_foreground_request_id = request_id;
        spawned->pending_foreground_client = client;
    }
    LeaveCriticalSection(&host->runtimes.lock);
    if (spawned == 0) return false;
    WinxtermManagedJobSnapshot finished;
    if (winxterm_job_manager_snapshot_one(&host->bridge->job_manager, job_id, &finished) &&
        (finished.state == WINXTERM_JOB_EXITED || finished.state == WINXTERM_JOB_FAILED)) {
        winxterm_host_complete_foreground_request(host, job_id, ERROR_SUCCESS,
                                                  finished.has_exit_code,
                                                  finished.exit_code);
    }
    return true;
}

static void winxterm_host_send_status_reply(WinxtermHostClient *client,
                                            uint64_t request_id, uint32_t status)
{
    if (client == 0 || request_id == 0u || client->reply_write == 0) return;
    uint8_t payload[16];
    size_t length = 0u;
    if (!winxterm_job_tlv_append_u32(payload, sizeof(payload), &length,
                                    WINXTERM_JOB_TLV_STATUS, status)) return;
    WinxtermJobFrameHeader reply = {WINXTERM_JOB_PROTOCOL_VERSION,
                                    WINXTERM_JOB_MESSAGE_REPLY, 0u, request_id,
                                    (uint32_t)length};
    EnterCriticalSection(&client->reply_lock);
    (void)winxterm_job_channel_write(client->reply_write, &reply, payload);
    LeaveCriticalSection(&client->reply_lock);
}

uint32_t winxterm_host_view_job(WinxtermHostContext *host, uint64_t requester_id,
                                      uint64_t job_id,
                                      uint64_t cursor, uint64_t *snapshot_offset,
                                      uint8_t *bytes, size_t capacity, size_t *length,
                                      uint64_t *next_cursor, bool *more)
{
    if (!winxterm_job_manager_authorized(&host->bridge->job_manager,
                                         requester_id, job_id)) return ERROR_ACCESS_DENIED;
    if (job_id == host->root_job_id) {
        EnterCriticalSection(&host->root_session.output_lock);
        if (*snapshot_offset == 0u) {
            *snapshot_offset = host->root_session.transcript_produced_offset;
        }
        if (cursor == 0u) cursor = host->root_session.transcript_base_offset;
        LeaveCriticalSection(&host->root_session.output_lock);
        return winxterm_terminal_session_copy_transcript_page(
            &host->root_session, cursor, *snapshot_offset, bytes, capacity,
            length, next_cursor, more) ? ERROR_SUCCESS : ERROR_INVALID_DATA;
    }
    EnterCriticalSection(&host->runtimes.lock);
    WinxtermHostManagedChild *child = host->runtimes.head;
    while (child != 0 && child->id != job_id) child = child->next;
    uint32_t status = ERROR_NOT_FOUND;
    if (child != 0) {
        EnterCriticalSection(&child->session.output_lock);
        if (*snapshot_offset == 0u) {
            *snapshot_offset = child->session.transcript_produced_offset;
        }
        if (cursor == 0u) cursor = child->session.transcript_base_offset;
        LeaveCriticalSection(&child->session.output_lock);
        status = winxterm_terminal_session_copy_transcript_page(
            &child->session, cursor, *snapshot_offset, bytes, capacity,
            length, next_cursor, more) ? ERROR_SUCCESS : ERROR_INVALID_DATA;
    }
    LeaveCriticalSection(&host->runtimes.lock);
    return status;
}

static bool winxterm_host_copy_native_view(WinxtermHostContext *host, uint64_t job_id,
                                          uint8_t **bytes, size_t *byte_count)
{
    *bytes = 0;
    *byte_count = 0u;
    if (job_id == host->root_job_id) {
        return winxterm_bridge_copy_transcript(host->bridge, bytes, byte_count);
    }
    EnterCriticalSection(&host->runtimes.lock);
    WinxtermHostManagedChild *child = winxterm_host_find_child_locked(host, job_id);
    bool ok = child != 0;
    if (ok) {
        ok = winxterm_terminal_session_copy_transcript(&child->session, bytes, byte_count);
    }
    LeaveCriticalSection(&host->runtimes.lock);
    return ok;
}

uint32_t winxterm_host_signal_job(WinxtermHostContext *host, uint64_t requester_id,
                                        uint64_t job_id, bool force)
{
    if (!winxterm_job_manager_authorized(&host->bridge->job_manager,
                                         requester_id, job_id)) return ERROR_ACCESS_DENIED;
    if (job_id == host->root_job_id) {
        if (!force) return ERROR_NOT_SUPPORTED;
        if (host->process.hProcess == 0 ||
            WaitForSingleObject(host->process.hProcess, 0u) == WAIT_OBJECT_0) {
            return ERROR_INVALID_STATE;
        }
        (void)winxterm_job_manager_stopping(&host->bridge->job_manager, job_id);
        if (host->process_job != 0 && TerminateJobObject(host->process_job, 1u)) {
            return ERROR_SUCCESS;
        }
        uint32_t error = GetLastError();
        (void)winxterm_job_manager_cancel_stopping(&host->bridge->job_manager, job_id);
        return error;
    }
    EnterCriticalSection(&host->runtimes.lock);
    WinxtermHostManagedChild *child = host->runtimes.head;
    while (child != 0 && child->id != job_id) child = child->next;
    uint32_t status = ERROR_NOT_FOUND;
    HANDLE final_process = winxterm_host_managed_child_final_process(child);
    if (child != 0 && final_process != 0 && !winxterm_host_managed_child_all_exited(child)) {
        (void)winxterm_job_manager_stopping(&host->bridge->job_manager, job_id);
        if (!force) {
            static const uint8_t interrupt = 0x03u;
            DWORD written = 0u;
            (void)WriteFile(child->input_write, &interrupt, 1u, &written, 0);
        }
        DWORD wait = force ? WAIT_TIMEOUT :
            WaitForSingleObject(final_process, WINXTERM_JOB_TERMINATE_TIMEOUT_MS);
        status = ERROR_SUCCESS;
        if (wait == WAIT_TIMEOUT &&
            (child->process_job == 0 || !TerminateJobObject(child->process_job, 1u))) {
            status = GetLastError();
            (void)winxterm_job_manager_cancel_stopping(&host->bridge->job_manager, job_id);
        } else if (wait != WAIT_TIMEOUT && wait != WAIT_OBJECT_0) {
            status = ERROR_GEN_FAILURE;
            (void)winxterm_job_manager_cancel_stopping(&host->bridge->job_manager, job_id);
        }
    } else if (child != 0) status = ERROR_INVALID_STATE;
    LeaveCriticalSection(&host->runtimes.lock);
    return status;
}

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
           winxterm_host_env_name_equals(entry, L"TERM_PROGRAM_VERSION") ||
           winxterm_host_env_name_equals(entry, WINXTERM_JOB_ENV_PROTOCOL) ||
           winxterm_host_env_name_equals(entry, WINXTERM_JOB_ENV_REQUEST_HANDLE) ||
           winxterm_host_env_name_equals(entry, WINXTERM_JOB_ENV_REPLY_HANDLE) ||
           winxterm_host_env_name_equals(entry, WINXTERM_JOB_ENV_SELF_ID);
}

static size_t winxterm_host_env_name_length(const wchar_t *entry)
{
    if (entry == 0) return 0u;
    const wchar_t *separator = wcschr(entry + (entry[0] == L'=' ? 1u : 0u), L'=');
    return separator != 0 ? (size_t)(separator - entry) : 0u;
}

static bool winxterm_host_env_same_name(const wchar_t *left, const wchar_t *right)
{
    size_t left_length = winxterm_host_env_name_length(left);
    size_t right_length = winxterm_host_env_name_length(right);
    return left_length != 0u && left_length == right_length &&
           _wcsnicmp(left, right, left_length) == 0;
}

static bool winxterm_host_env_overridden_by_extra(const wchar_t *entry,
                                                   const wchar_t * const *extra,
                                                   size_t extra_count)
{
    for (size_t i = 0u; i < extra_count; ++i) {
        if (winxterm_host_env_same_name(entry, extra[i])) return true;
    }
    return false;
}

static wchar_t *winxterm_host_build_environment(const wchar_t * const *extra, size_t extra_count)
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
        if (!winxterm_host_env_should_override(entry) &&
            !winxterm_host_env_overridden_by_extra(entry, extra, extra_count)) {
            total += wcslen(entry) + 1u;
        }
    }
    for (size_t i = 0u; i < sizeof(overrides) / sizeof(overrides[0]); ++i) {
        total += wcslen(overrides[i]) + 1u;
    }
    for (size_t i = 0u; i < extra_count; ++i) {
        if (extra[i] != 0 && !winxterm_host_env_name_equals(extra[i], L"TERM") &&
            !winxterm_host_env_name_equals(extra[i], L"COLORTERM") &&
            !winxterm_host_env_name_equals(extra[i], L"TERM_PROGRAM") &&
            !winxterm_host_env_name_equals(extra[i], L"TERM_PROGRAM_VERSION")) {
            total += wcslen(extra[i]) + 1u;
        }
    }

    wchar_t *block = (wchar_t *)calloc(total, sizeof(*block));
    if (block == 0) {
        FreeEnvironmentStringsW(environment);
        return 0;
    }

    size_t offset = 0u;
    for (const wchar_t *entry = environment; *entry != L'\0'; entry += wcslen(entry) + 1u) {
        if (!winxterm_host_env_should_override(entry) &&
            !winxterm_host_env_overridden_by_extra(entry, extra, extra_count)) {
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
    for (size_t i = 0u; i < extra_count; ++i) {
        if (extra[i] == 0 || winxterm_host_env_name_equals(extra[i], L"TERM") ||
            winxterm_host_env_name_equals(extra[i], L"COLORTERM") ||
            winxterm_host_env_name_equals(extra[i], L"TERM_PROGRAM") ||
            winxterm_host_env_name_equals(extra[i], L"TERM_PROGRAM_VERSION")) continue;
        size_t length = wcslen(extra[i]);
        memcpy(block + offset, extra[i], (length + 1u) * sizeof(*block));
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

static bool winxterm_host_create_job_pipes(HANDLE *request_read,
                                           HANDLE *request_write,
                                           HANDLE *reply_read,
                                           HANDLE *reply_write)
{
    SECURITY_ATTRIBUTES security;
    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    if (!CreatePipe(request_read, request_write, &security, 0)) return false;
    if (!CreatePipe(reply_read, reply_write, &security, 0)) {
        winxterm_host_close_handle(request_read);
        winxterm_host_close_handle(request_write);
        return false;
    }
    if (!SetHandleInformation(*request_read, HANDLE_FLAG_INHERIT, 0u) ||
        !SetHandleInformation(*reply_write, HANDLE_FLAG_INHERIT, 0u)) {
        winxterm_host_close_handle(request_read);
        winxterm_host_close_handle(request_write);
        winxterm_host_close_handle(reply_read);
        winxterm_host_close_handle(reply_write);
        return false;
    }
    return true;
}

static bool winxterm_host_is_dstshell(const wchar_t *path)
{
    if (path == 0) return false;
    const wchar_t *name = wcsrchr(path, L'\\');
    name = name != 0 ? name + 1 : path;
    return _wcsicmp(name, L"dstshell.exe") == 0 || _wcsicmp(name, L"dstshell") == 0;
}

static bool winxterm_host_is_shell_root(const wchar_t *path)
{
    if (path == 0) return false;
    const wchar_t *name = wcsrchr(path, L'\\');
    name = name != 0 ? name + 1 : path;
    return winxterm_host_is_dstshell(path) ||
           _wcsicmp(name, L"cmd.exe") == 0 || _wcsicmp(name, L"cmd") == 0 ||
           _wcsicmp(name, L"powershell.exe") == 0 || _wcsicmp(name, L"powershell") == 0 ||
           _wcsicmp(name, L"pwsh.exe") == 0 || _wcsicmp(name, L"pwsh") == 0 ||
           _wcsicmp(name, L"bash.exe") == 0 || _wcsicmp(name, L"bash") == 0;
}

static bool winxterm_host_show_root_exit_notice(WinxtermHostContext *host)
{
    wchar_t wide[512];
    const wchar_t *name = wcsrchr(host->root_executable, L'\\');
    name = name != 0 ? name + 1 : host->root_executable;
    int count = _snwprintf_s(wide, sizeof(wide) / sizeof(wide[0]), _TRUNCATE,
                             L"\r\n%ls exited with code %lu\r\nPress any key to close.\r\n",
                             name[0] != L'\0' ? name : L"process",
                             (unsigned long)host->exit_code);
    if (count <= 0) return false;
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, count,
                                    0, 0, 0, 0);
    char *utf8 = bytes > 0 ? (char *)malloc((size_t)bytes) : 0;
    bool ok = utf8 != 0 && WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                               wide, count, utf8, bytes, 0, 0) == bytes &&
              winxterm_host_queue_output(host->bridge, (const uint8_t *)utf8,
                                         (size_t)bytes, host->shutdown_event);
    free(utf8);
    return ok;
}

static bool winxterm_host_prepare_startup(HPCON pseudo_console,
                                          const HANDLE *inherited_handles,
                                          size_t inherited_handle_count,
                                          STARTUPINFOEXW *startup,
                                          LPPROC_THREAD_ATTRIBUTE_LIST *attribute_list)
{
    SIZE_T attribute_size = 0;
    memset(startup, 0, sizeof(*startup));
    startup->StartupInfo.cb = sizeof(*startup);

    DWORD attribute_count = inherited_handle_count != 0u ? 2u : 1u;
    InitializeProcThreadAttributeList(0, attribute_count, 0, &attribute_size);
    *attribute_list = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attribute_size);
    if (*attribute_list == 0) {
        return false;
    }
    if (!InitializeProcThreadAttributeList(*attribute_list, attribute_count, 0, &attribute_size)) {
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
    if (inherited_handle_count != 0u &&
        !UpdateProcThreadAttribute(*attribute_list,
                                   0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   (void *)inherited_handles,
                                   inherited_handle_count * sizeof(*inherited_handles),
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

static bool winxterm_host_prepare_stdio_startup(HANDLE stdin_handle,
                                                HANDLE stdout_handle,
                                                HANDLE stderr_handle,
                                                const HANDLE *extra_handles,
                                                size_t extra_handle_count,
                                                STARTUPINFOEXW *startup,
                                                LPPROC_THREAD_ATTRIBUTE_LIST *attribute_list)
{
    SIZE_T attribute_size = 0u;
    memset(startup, 0, sizeof(*startup));
    startup->StartupInfo.cb = sizeof(*startup);
    startup->StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup->StartupInfo.hStdInput = stdin_handle;
    startup->StartupInfo.hStdOutput = stdout_handle;
    startup->StartupInfo.hStdError = stderr_handle;
    HANDLE handles[8];
    size_t handle_count = 0u;
    HANDLE candidates[8] = {stdin_handle, stdout_handle, stderr_handle};
    if (extra_handle_count > 5u) {
        return false;
    }
    for (size_t i = 0u; i < extra_handle_count; ++i) {
        candidates[3u + i] = extra_handles[i];
    }
    for (size_t i = 0u; i < 3u + extra_handle_count; ++i) {
        bool duplicate = false;
        for (size_t j = 0u; j < handle_count; ++j) {
            if (handles[j] == candidates[i]) duplicate = true;
        }
        if (!duplicate) handles[handle_count++] = candidates[i];
    }
    InitializeProcThreadAttributeList(0, 1u, 0, &attribute_size);
    *attribute_list = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0,
                                                              attribute_size);
    if (*attribute_list == 0 ||
        !InitializeProcThreadAttributeList(*attribute_list, 1u, 0, &attribute_size)) {
        if (*attribute_list != 0) HeapFree(GetProcessHeap(), 0, *attribute_list);
        *attribute_list = 0;
        return false;
    }
    if (!UpdateProcThreadAttribute(*attribute_list, 0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                   handle_count * sizeof(*handles), 0, 0)) {
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

static wchar_t *winxterm_host_utf8_to_wide(const char *text)
{
    if (text == 0) return 0;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, 0, 0);
    if (count <= 0) return 0;
    wchar_t *wide = (wchar_t *)malloc((size_t)count * sizeof(*wide));
    if (wide == 0 || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         text, -1, wide, count) != count) {
        free(wide);
        return 0;
    }
    return wide;
}

static WinxtermHostManagedChild *winxterm_host_find_child_locked(WinxtermHostContext *host,
                                                                 uint64_t id)
{
    WinxtermHostManagedChild *child = host->runtimes.head;
    while (child != 0 && child->id != id) child = child->next;
    return child;
}

static bool winxterm_host_write_all(HANDLE handle, const uint8_t *bytes, size_t length)
{
    size_t offset = 0u;
    while (offset < length) {
        DWORD chunk = length - offset > 65536u ? 65536u : (DWORD)(length - offset);
        DWORD written = 0u;
        if (!WriteFile(handle, bytes + offset, chunk, &written, 0) || written == 0u) return false;
        offset += written;
    }
    return true;
}

static DWORD WINAPI winxterm_host_connection_thread(void *context)
{
    WinxtermHostManagedChild *source = (WinxtermHostManagedChild *)context;
    uint8_t bytes[WINXTERM_HOST_OUTPUT_READ_CHUNK_BYTES];
    bool write_failed = false;
    for (;;) {
        EnterCriticalSection(&source->host->runtimes.lock);
        bool connected = source->destination_id != 0u;
        HANDLE destination_input = source->connection_input;
        LeaveCriticalSection(&source->host->runtimes.lock);
        if (!connected || destination_input == 0) break;

        size_t length = 0u;
        uint64_t next = 0u;
        bool more = false;
        EnterCriticalSection(&source->session.output_lock);
        uint64_t offset = source->session.journal.consumed_offset;
        uint64_t end = source->session.journal.produced_offset;
        bool copied = winxterm_job_journal_copy_snapshot(
            &source->session.journal, offset, end, bytes, sizeof(bytes),
            &length, &next, &more);
        LeaveCriticalSection(&source->session.output_lock);
        (void)next;
        (void)more;
        if (!copied) { write_failed = true; break; }
        if (length == 0u) { Sleep(10u); continue; }

        if (!winxterm_host_write_all(destination_input, bytes, length)) {
            write_failed = true;
            break;
        }
        EnterCriticalSection(&source->session.output_lock);
        size_t consumed = winxterm_job_journal_consume(&source->session.journal,
                                                       bytes, length);
        size_t retained = winxterm_job_journal_retained(&source->session.journal);
        LeaveCriticalSection(&source->session.output_lock);
        if (consumed != length) { write_failed = true; break; }
        (void)winxterm_job_manager_set_output(&source->host->bridge->job_manager,
                                              source->id, retained, false);
    }
    if (write_failed) {
        EnterCriticalSection(&source->host->runtimes.lock);
        bool was_connected = source->destination_id != 0u;
        source->destination_id = 0u;
        LeaveCriticalSection(&source->host->runtimes.lock);
        if (was_connected) {
            winxterm_host_send_event(source->host, WINXTERM_JOB_EVENT_DISCONNECTED,
                                    source->id);
        }
    }
    return write_failed ? ERROR_WRITE_FAULT : ERROR_SUCCESS;
}

uint32_t winxterm_host_connect_jobs(WinxtermHostContext *host,
                                          uint64_t requester_id, uint64_t source_id,
                                          uint64_t destination_id)
{
    if (!winxterm_job_manager_authorized(&host->bridge->job_manager, requester_id, source_id) ||
        !winxterm_job_manager_authorized(&host->bridge->job_manager, requester_id,
                                         destination_id)) return ERROR_ACCESS_DENIED;
    WinxtermManagedJobSnapshot destination_snapshot;
    if (!winxterm_job_manager_snapshot_one(&host->bridge->job_manager, destination_id,
                                           &destination_snapshot)) return ERROR_NOT_FOUND;
    if (!destination_snapshot.input_connectable ||
        destination_snapshot.state == WINXTERM_JOB_EXITED ||
        destination_snapshot.state == WINXTERM_JOB_FAILED) return ERROR_INVALID_STATE;
    EnterCriticalSection(&host->runtimes.lock);
    WinxtermHostManagedChild *source = winxterm_host_find_child_locked(host, source_id);
    WinxtermHostManagedChild *destination = winxterm_host_find_child_locked(host, destination_id);
    if (source != 0 && source->connection_thread != 0 &&
        WaitForSingleObject(source->connection_thread, 0u) == WAIT_OBJECT_0) {
        CloseHandle(source->connection_thread);
        source->connection_thread = 0;
        winxterm_host_close_handle(&source->connection_input);
    }
    if (source == 0 || destination == 0 || source->destination_id != 0u) {
        LeaveCriticalSection(&host->runtimes.lock);
        return source != 0 && destination != 0 ? ERROR_ALREADY_ASSIGNED : ERROR_NOT_FOUND;
    }
    uint64_t cursor = destination_id;
    for (size_t steps = 0u; steps < 1024u && cursor != 0u; ++steps) {
        if (cursor == source_id) {
            LeaveCriticalSection(&host->runtimes.lock);
            return ERROR_CIRCULAR_DEPENDENCY;
        }
        WinxtermHostManagedChild *node = winxterm_host_find_child_locked(host, cursor);
        cursor = node != 0 ? node->destination_id : 0u;
    }
    HANDLE duplicate = 0;
    bool ok = DuplicateHandle(GetCurrentProcess(), destination->input_write,
                              GetCurrentProcess(), &duplicate, 0, FALSE,
                              DUPLICATE_SAME_ACCESS) != FALSE;
    if (ok) {
        source->connection_input = duplicate;
        source->destination_id = destination_id;
        source->connection_thread = CreateThread(
            0, 0, winxterm_host_connection_thread, source, 0, 0);
        ok = source->connection_thread != 0;
    }
    if (!ok) {
        source->destination_id = 0u;
        winxterm_host_close_handle(&source->connection_input);
    }
    LeaveCriticalSection(&host->runtimes.lock);
    if (ok) winxterm_host_send_event(host, WINXTERM_JOB_EVENT_CONNECTED, source_id);
    return ok ? ERROR_SUCCESS : GetLastError();
}

uint32_t winxterm_host_disconnect_job(WinxtermHostContext *host,
                                             uint64_t requester_id, uint64_t source_id)
{
    if (!winxterm_job_manager_authorized(&host->bridge->job_manager,
                                         requester_id, source_id)) return ERROR_ACCESS_DENIED;
    HANDLE thread = 0;
    EnterCriticalSection(&host->runtimes.lock);
    WinxtermHostManagedChild *source = winxterm_host_find_child_locked(host, source_id);
    uint32_t status = ERROR_NOT_FOUND;
    if (source != 0) {
        if (source->destination_id != 0u) {
            source->destination_id = 0u;
            thread = source->connection_thread;
            status = ERROR_SUCCESS;
        } else status = ERROR_INVALID_STATE;
    }
    LeaveCriticalSection(&host->runtimes.lock);
    if (thread != 0) {
        (void)CancelSynchronousIo(thread);
        (void)WaitForSingleObject(thread, 1000u);
        EnterCriticalSection(&host->runtimes.lock);
        if (source->connection_thread == thread) {
            CloseHandle(source->connection_thread);
            source->connection_thread = 0;
            winxterm_host_close_handle(&source->connection_input);
        }
        LeaveCriticalSection(&host->runtimes.lock);
    }
    if (status == ERROR_SUCCESS) {
        winxterm_host_send_event(host, WINXTERM_JOB_EVENT_DISCONNECTED, source_id);
    }
    return status;
}

uint32_t winxterm_host_attach_file(WinxtermHostContext *host, uint64_t job_id,
                                         uint64_t requester_id, const char *path,
                                         size_t path_length, uint32_t flags)
{
    if (host == 0 || path == 0 || path_length == 0u || path_length > INT_MAX ||
        (flags & ~(WINXTERM_JOB_FLAG_APPEND | WINXTERM_JOB_FLAG_TEE)) != 0u) {
        return ERROR_INVALID_PARAMETER;
    }
    if (!winxterm_job_manager_authorized(&host->bridge->job_manager,
                                         requester_id, job_id)) return ERROR_ACCESS_DENIED;
    int wide_count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path,
                                         (int)path_length, 0, 0);
    wchar_t *wide_path = wide_count > 0 ?
        (wchar_t *)calloc((size_t)wide_count + 1u, sizeof(*wide_path)) : 0;
    if (wide_path == 0 || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path,
                                              (int)path_length, wide_path,
                                              wide_count) != wide_count) {
        free(wide_path);
        return ERROR_NO_UNICODE_TRANSLATION;
    }
    HANDLE file = CreateFileW(wide_path, GENERIC_WRITE, FILE_SHARE_READ, 0,
                              (flags & WINXTERM_JOB_FLAG_APPEND) != 0u ?
                                  OPEN_ALWAYS : CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, 0);
    free(wide_path);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    if ((flags & WINXTERM_JOB_FLAG_APPEND) != 0u) {
        LARGE_INTEGER zero;
        zero.QuadPart = 0;
        if (!SetFilePointerEx(file, zero, 0, FILE_END)) {
            uint32_t error = GetLastError();
            CloseHandle(file);
            return error;
        }
    }
    EnterCriticalSection(&host->runtimes.lock);
    WinxtermHostManagedChild *child = winxterm_host_find_child_locked(host, job_id);
    uint32_t status = ERROR_NOT_FOUND;
    if (child != 0) {
        WinxtermManagedJobSnapshot snapshot;
        if (!winxterm_job_manager_snapshot_one(&host->bridge->job_manager, job_id, &snapshot) ||
            snapshot.state == WINXTERM_JOB_EXITED || snapshot.state == WINXTERM_JOB_FAILED) {
            status = ERROR_INVALID_STATE;
        } else {
            EnterCriticalSection(&child->session.output_lock);
            if (child->attachment_file != 0) {
                status = ERROR_ALREADY_ASSIGNED;
            } else {
                status = ERROR_SUCCESS;
                child->attachment_start_ms = GetTickCount64();
                child->attachment_bytes = 0u;
                child->attachment_file = file;
                child->attachment_tee = (flags & WINXTERM_JOB_FLAG_TEE) != 0u;
                file = 0;
            }
            LeaveCriticalSection(&child->session.output_lock);
        }
    }
    LeaveCriticalSection(&host->runtimes.lock);
    if (file != 0) CloseHandle(file);
    return status;
}

uint32_t winxterm_host_detach_file(WinxtermHostContext *host, uint64_t requester_id,
                                         uint64_t job_id)
{
    if (!winxterm_job_manager_authorized(&host->bridge->job_manager,
                                         requester_id, job_id)) return ERROR_ACCESS_DENIED;
    EnterCriticalSection(&host->runtimes.lock);
    WinxtermHostManagedChild *child = winxterm_host_find_child_locked(host, job_id);
    uint32_t status = ERROR_NOT_FOUND;
    uint64_t attached_bytes = 0u;
    ULONGLONG attachment_start_ms = 0u;
    if (child != 0) {
        EnterCriticalSection(&child->session.output_lock);
        if (child->attachment_file == 0) {
            status = ERROR_INVALID_STATE;
        } else {
            (void)FlushFileBuffers(child->attachment_file);
            winxterm_host_close_handle(&child->attachment_file);
            attached_bytes = child->attachment_bytes;
            attachment_start_ms = child->attachment_start_ms;
            child->attachment_bytes = 0u;
            child->attachment_start_ms = 0u;
            child->attachment_tee = false;
            status = ERROR_SUCCESS;
        }
        LeaveCriticalSection(&child->session.output_lock);
    }
    LeaveCriticalSection(&host->runtimes.lock);
    if (status == ERROR_SUCCESS) {
        winxterm_host_emit_file_summary(host, attached_bytes, attachment_start_ms);
    }
    return status;
}

static bool winxterm_host_route_output(WinxtermHostContext *host, uint64_t id,
                                      WinxtermJobJournal *journal, CRITICAL_SECTION *journal_lock,
                                      const uint8_t *bytes, size_t length)
{
    EnterCriticalSection(&host->runtimes.lock);
    bool active = host->active_session_id == id && !winxterm_bridge_is_headless(host->bridge);
    WinxtermHostManagedChild *runtime = id != host->root_job_id ?
        winxterm_host_find_child_locked(host, id) : 0;
    bool connected = runtime != 0 && runtime->destination_id != 0u;
    bool ok;
    size_t retained = 0u;
    bool backpressured = false;
    if (runtime != 0 && runtime->attachment_file != 0) {
        EnterCriticalSection(journal_lock);
        ok = runtime->attachment_file != 0 &&
             winxterm_host_write_all(runtime->attachment_file, bytes, length);
        if (ok) runtime->attachment_bytes += length;
        bool tee = runtime->attachment_tee;
        LeaveCriticalSection(journal_lock);
        if (ok && tee) {
            if (active && !connected) {
                ok = winxterm_host_queue_output(host->bridge, bytes, length,
                                                host->shutdown_event);
            } else {
                EnterCriticalSection(journal_lock);
                ok = winxterm_job_journal_append(journal, bytes, length);
                retained = winxterm_job_journal_retained(journal);
                backpressured = winxterm_job_journal_backpressured(journal);
                LeaveCriticalSection(journal_lock);
            }
        }
    } else if (active && !connected) {
        ok = winxterm_host_queue_output(host->bridge, bytes, length, host->shutdown_event);
    } else {
        EnterCriticalSection(journal_lock);
        ok = winxterm_job_journal_append(journal, bytes, length);
        retained = winxterm_job_journal_retained(journal);
        backpressured = winxterm_job_journal_backpressured(journal);
        LeaveCriticalSection(journal_lock);
    }
    LeaveCriticalSection(&host->runtimes.lock);
    if (!active || connected) {
        (void)winxterm_job_manager_set_output(&host->bridge->job_manager, id,
                                              retained, backpressured);
    }
    return ok;
}

static bool winxterm_host_drain_journal_locked(WinxtermHostContext *host, uint64_t id,
                                               WinxtermJobJournal *journal,
                                               CRITICAL_SECTION *journal_lock)
{
    uint8_t bytes[WINXTERM_HOST_OUTPUT_READ_CHUNK_BYTES];
    for (;;) {
        EnterCriticalSection(journal_lock);
        uint64_t offset = journal->consumed_offset;
        uint64_t end = journal->produced_offset;
        size_t length = 0u;
        uint64_t next = offset;
        bool more = false;
        bool copied = winxterm_job_journal_copy_snapshot(journal, offset, end, bytes,
                                                         sizeof(bytes), &length, &next, &more);
        LeaveCriticalSection(journal_lock);
        if (!copied) return false;
        if (length == 0u) return true;
        if (!winxterm_host_queue_output(host->bridge, bytes, length, host->shutdown_event)) return false;
        EnterCriticalSection(journal_lock);
        size_t consumed = winxterm_job_journal_consume(journal, bytes, length);
        size_t retained = winxterm_job_journal_retained(journal);
        LeaveCriticalSection(journal_lock);
        if (consumed != length) return false;
        (void)winxterm_job_manager_set_output(&host->bridge->job_manager, id, retained, false);
    }
}

static bool winxterm_host_switch_session(WinxtermHostContext *host, uint64_t target_id)
{
    if (host == 0) return false;
    EnterCriticalSection(&host->runtimes.lock);
    if (target_id == host->active_session_id) {
        LeaveCriticalSection(&host->runtimes.lock);
        return true;
    }
    WinxtermHostManagedChild *old_child = host->active_session_id != host->root_job_id ?
        winxterm_host_find_child_locked(host, host->active_session_id) : 0;
    WinxtermHostManagedChild *target_child = target_id != host->root_job_id ?
        winxterm_host_find_child_locked(host, target_id) : 0;
    if (target_id != 0u && target_id != host->root_job_id && target_child == 0) {
        LeaveCriticalSection(&host->runtimes.lock);
        return false;
    }
    WinxtermTerminalSession *old_session = host->active_session_id == host->root_job_id ?
        &host->root_session : old_child != 0 ? &old_child->session : 0;
    WinxtermTerminalSession *target_session = target_id == host->root_job_id ?
        &host->root_session : target_child != 0 ? &target_child->session : 0;
    bool more = false;
    do {
        bool changed = false, presentation = false;
        if (!winxterm_bridge_commit_output(host->bridge, 0u, &changed, &more, &presentation)) {
            LeaveCriticalSection(&host->runtimes.lock);
            return false;
        }
    } while (more);
    const uint8_t *target_input = 0;
    size_t target_input_count = 0u;
    if (target_session != 0) {
        EnterCriticalSection(&target_session->output_lock);
        target_input = target_session->pending_input;
        target_input_count = target_session->pending_input_count;
        LeaveCriticalSection(&target_session->output_lock);
    }
    uint8_t *previous_input = 0;
    size_t previous_input_count = 0u;
    if (!winxterm_bridge_switch_input_session(host->bridge, target_id,
                                              target_input, target_input_count,
                                              &previous_input,
                                              &previous_input_count)) {
        LeaveCriticalSection(&host->runtimes.lock);
        return false;
    }
    if (old_session != 0) {
        EnterCriticalSection(&old_session->output_lock);
        free(old_session->pending_input);
        old_session->pending_input = previous_input;
        old_session->pending_input_count = previous_input_count;
        LeaveCriticalSection(&old_session->output_lock);
        previous_input = 0;
    }
    free(previous_input);
    if (target_session != 0) {
        EnterCriticalSection(&target_session->output_lock);
        free(target_session->pending_input);
        target_session->pending_input = 0;
        target_session->pending_input_count = 0u;
        LeaveCriticalSection(&target_session->output_lock);
    }
    if (target_id != 0u) {
        EnterCriticalSection(&host->bridge->screen_lock);
        if (host->active_session_id == host->root_job_id) {
            host->root_session.screen = host->bridge->screen;
            host->root_session.decoder = host->bridge->output_decoder;
            memcpy(host->root_session.title, host->bridge->terminal_title,
                   sizeof(host->root_session.title));
            host->root_session.screen_stored = true;
        } else if (old_child != 0) {
            old_child->session.screen = host->bridge->screen;
            old_child->session.decoder = host->bridge->output_decoder;
            memcpy(old_child->session.title, host->bridge->terminal_title,
                   sizeof(old_child->session.title));
            old_child->session.screen_stored = true;
        }
        if (target_id == host->root_job_id) {
            host->bridge->screen = host->root_session.screen;
            host->bridge->output_decoder = host->root_session.decoder;
            memcpy(host->bridge->terminal_title, host->root_session.title,
                   sizeof(host->bridge->terminal_title));
            memset(&host->root_session.screen, 0, sizeof(host->root_session.screen));
            memset(&host->root_session.decoder, 0, sizeof(host->root_session.decoder));
            host->root_session.screen_stored = false;
        } else {
            host->bridge->screen = target_child->session.screen;
            host->bridge->output_decoder = target_child->session.decoder;
            memcpy(host->bridge->terminal_title, target_child->session.title,
                   sizeof(host->bridge->terminal_title));
            memset(&target_child->session.screen, 0, sizeof(target_child->session.screen));
            memset(&target_child->session.decoder, 0, sizeof(target_child->session.decoder));
            target_child->session.screen_stored = false;
        }
        LeaveCriticalSection(&host->bridge->screen_lock);
    }
    host->active_session_id = target_id;
    winxterm_bridge_set_active_session(host->bridge, target_id);
    bool ok = true;
    if (target_id == host->root_job_id) {
        ok = winxterm_host_drain_journal_locked(host, target_id, &host->root_session.journal,
                                                &host->root_session.output_lock);
    } else if (target_child != 0) {
        ok = winxterm_host_drain_journal_locked(host, target_id, &target_child->session.journal,
                                                &target_child->session.output_lock);
    }
    LeaveCriticalSection(&host->runtimes.lock);
    if (ok) winxterm_bridge_request_frame(host->bridge, WINXTERM_FRAME_CAUSE_CONTENT |
                                                        WINXTERM_FRAME_CAUSE_PRESENTATION);
    if (ok && target_id != 0u) {
        winxterm_host_send_event(host, WINXTERM_JOB_EVENT_FOREGROUND_CHANGED, target_id);
    }
    return ok;
}

static void winxterm_host_managed_child_close_runtime(WinxtermHostManagedChild *child)
{
    if (child == 0) return;
    if (child->kill_thread != 0) {
        (void)WaitForSingleObject(child->kill_thread, 1000u);
        winxterm_host_close_handle(&child->kill_thread);
    }
    winxterm_host_close_handle(&child->kill_cancel_event);
    winxterm_host_close_handle(&child->process.hThread);
    winxterm_host_close_handle(&child->process.hProcess);
    if (child->pseudo_console != 0) {
        ClosePseudoConsole(child->pseudo_console);
        child->pseudo_console = 0;
    }
    winxterm_host_close_handle(&child->input_write);
    winxterm_host_close_handle(&child->output_read);
    winxterm_host_close_handle(&child->redirected_output_read);
    winxterm_host_close_handle(&child->redirected_file);
    if (child->session.output_lock_initialized) EnterCriticalSection(&child->session.output_lock);
    if (child->attachment_file != 0) {
        (void)FlushFileBuffers(child->attachment_file);
        winxterm_host_close_handle(&child->attachment_file);
    }
    if (child->session.output_lock_initialized) LeaveCriticalSection(&child->session.output_lock);
    winxterm_host_close_handle(&child->process_job);
    if (child->stage_processes != 0) {
        for (size_t i = 0u; i < child->stage_count; ++i) {
            winxterm_host_close_handle(&child->stage_processes[i].hThread);
            winxterm_host_close_handle(&child->stage_processes[i].hProcess);
        }
        free(child->stage_processes);
        child->stage_processes = 0;
        child->stage_count = 0u;
    }
}

static HANDLE winxterm_host_managed_child_final_process(WinxtermHostManagedChild *child)
{
    if (child == 0) return 0;
    return child->stage_count != 0u ?
        child->stage_processes[child->stage_count - 1u].hProcess : child->process.hProcess;
}

static bool winxterm_host_managed_child_all_exited(WinxtermHostManagedChild *child)
{
    if (child == 0) return true;
    if (child->stage_count == 0u) {
        return child->process.hProcess == 0 ||
               WaitForSingleObject(child->process.hProcess, 0) == WAIT_OBJECT_0;
    }
    for (size_t i = 0u; i < child->stage_count; ++i) {
        if (child->stage_processes[i].hProcess != 0 &&
            WaitForSingleObject(child->stage_processes[i].hProcess, 0) != WAIT_OBJECT_0) return false;
    }
    return true;
}

static bool winxterm_host_has_live_children(WinxtermHostContext *host)
{
    bool live = false;
    EnterCriticalSection(&host->runtimes.lock);
    for (WinxtermHostManagedChild *child = host->runtimes.head; child != 0; child = child->next) {
        WinxtermManagedJobSnapshot snapshot;
        if (winxterm_job_manager_snapshot_one(&host->bridge->job_manager, child->id, &snapshot) &&
            snapshot.state != WINXTERM_JOB_EXITED && snapshot.state != WINXTERM_JOB_FAILED) {
            live = true;
            break;
        }
    }
    LeaveCriticalSection(&host->runtimes.lock);
    return live;
}

static void winxterm_host_record_root_exit(WinxtermHostContext *host)
{
    if (host == 0 || host->child_exited) return;
    host->child_exited = true;
    (void)GetExitCodeProcess(host->process.hProcess, &host->exit_code);
    (void)winxterm_job_manager_exit(&host->bridge->job_manager, host->root_job_id,
                                    (uint32_t)host->exit_code);
    uint64_t restored = winxterm_job_manager_foreground_id(&host->bridge->job_manager);
    (void)winxterm_host_switch_session(host, restored);
    winxterm_bridge_set_host_state(host->bridge, WINXTERM_HOST_STATE_CHILD_EXITED);
}

static DWORD WINAPI winxterm_host_root_kill_thread(void *context)
{
    WinxtermHostContext *host = (WinxtermHostContext *)context;
    static const uint8_t interrupt = 0x03u;
    DWORD written = 0u;
    (void)WriteFile(host->input_write, &interrupt, 1u, &written, 0);
    HANDLE waits[2] = {host->process.hProcess, host->root_kill_cancel_event};
    DWORD wait = host->process.hProcess != 0 && host->root_kill_cancel_event != 0 ?
        WaitForMultipleObjects(2u, waits, FALSE, WINXTERM_JOB_TERMINATE_TIMEOUT_MS) :
        WAIT_FAILED;
    uint32_t status = ERROR_SUCCESS;
    if (wait == WAIT_OBJECT_0 + 1u) {
        status = ERROR_CANCELLED;
        (void)winxterm_job_manager_cancel_stopping(&host->bridge->job_manager,
                                                   host->root_job_id);
    } else if (wait == WAIT_TIMEOUT) {
        if (host->process_job == 0 || !TerminateJobObject(host->process_job, 1u)) {
            status = GetLastError();
        }
    } else if (wait != WAIT_OBJECT_0) {
        status = ERROR_GEN_FAILURE;
    }
    EnterCriticalSection(&host->runtimes.lock);
    uint64_t request_id = host->pending_root_kill_request_id;
    WinxtermHostClient *client = host->pending_root_kill_client;
    host->pending_root_kill_request_id = 0u;
    host->pending_root_kill_client = 0;
    LeaveCriticalSection(&host->runtimes.lock);
    winxterm_host_send_status_reply(client, request_id, status);
    return status;
}

static DWORD WINAPI winxterm_host_kill_thread(void *context)
{
    WinxtermHostManagedChild *child = (WinxtermHostManagedChild *)context;
    HANDLE process = winxterm_host_managed_child_final_process(child);
    static const uint8_t interrupt = 0x03u;
    DWORD written = 0u;
    (void)WriteFile(child->input_write, &interrupt, 1u, &written, 0);
    HANDLE waits[2] = {process, child->kill_cancel_event};
    DWORD wait = process != 0 && child->kill_cancel_event != 0 ?
        WaitForMultipleObjects(2u, waits, FALSE, WINXTERM_JOB_TERMINATE_TIMEOUT_MS) : WAIT_FAILED;
    uint32_t status = ERROR_SUCCESS;
    if (wait == WAIT_OBJECT_0 + 1u) {
        status = ERROR_CANCELLED;
        (void)winxterm_job_manager_cancel_stopping(&child->host->bridge->job_manager,
                                                   child->id);
    } else if (wait == WAIT_TIMEOUT) {
        if (child->process_job == 0 || !TerminateJobObject(child->process_job, 1u)) {
            status = GetLastError();
        }
    } else if (wait != WAIT_OBJECT_0) status = ERROR_GEN_FAILURE;
    EnterCriticalSection(&child->host->runtimes.lock);
    uint64_t request_id = child->pending_kill_request_id;
    WinxtermHostClient *client = child->pending_kill_client;
    child->pending_kill_request_id = 0u;
    child->pending_kill_client = 0;
    LeaveCriticalSection(&child->host->runtimes.lock);
    winxterm_host_send_status_reply(client, request_id, status);
    return status;
}

uint32_t winxterm_host_start_kill(WinxtermHostClient *client, uint64_t job_id,
                                        uint64_t request_id)
{
    WinxtermHostContext *host = client != 0 ? (WinxtermHostContext *)client->context : 0;
    if (host == 0) return ERROR_INVALID_PARAMETER;
    if (!winxterm_job_manager_authorized(&host->bridge->job_manager,
                                         client->requester_id, job_id)) return ERROR_ACCESS_DENIED;
    EnterCriticalSection(&host->runtimes.lock);
    if (job_id == host->root_job_id) {
        uint32_t root_status = ERROR_INVALID_STATE;
        if (host->root_kill_thread != 0 &&
            WaitForSingleObject(host->root_kill_thread, 0u) == WAIT_OBJECT_0) {
            winxterm_host_close_handle(&host->root_kill_thread);
            winxterm_host_close_handle(&host->root_kill_cancel_event);
        }
        if (host->process.hProcess != 0 &&
            WaitForSingleObject(host->process.hProcess, 0u) != WAIT_OBJECT_0) {
            if (host->pending_root_kill_request_id != 0u) {
                root_status = ERROR_BUSY;
            } else {
                host->root_kill_cancel_event = CreateEventW(0, TRUE, FALSE, 0);
                host->pending_root_kill_request_id = request_id;
                host->pending_root_kill_client = client;
                (void)winxterm_job_manager_stopping(&host->bridge->job_manager, job_id);
                host->root_kill_thread = host->root_kill_cancel_event != 0 ?
                    CreateThread(0, 0, winxterm_host_root_kill_thread, host, 0, 0) : 0;
                if (host->root_kill_thread == 0) {
                    root_status = GetLastError();
                    host->pending_root_kill_request_id = 0u;
                    host->pending_root_kill_client = 0;
                    winxterm_host_close_handle(&host->root_kill_cancel_event);
                    (void)winxterm_job_manager_cancel_stopping(
                        &host->bridge->job_manager, job_id);
                } else {
                    root_status = ERROR_SUCCESS;
                }
            }
        }
        LeaveCriticalSection(&host->runtimes.lock);
        return root_status;
    }
    WinxtermHostManagedChild *child = winxterm_host_find_child_locked(host, job_id);
    uint32_t status = ERROR_NOT_FOUND;
    if (child != 0 && !winxterm_host_managed_child_all_exited(child)) {
        if (child->kill_thread != 0 &&
            WaitForSingleObject(child->kill_thread, 0u) == WAIT_OBJECT_0) {
            winxterm_host_close_handle(&child->kill_thread);
            winxterm_host_close_handle(&child->kill_cancel_event);
        }
        if (child->pending_kill_request_id != 0u) {
            status = ERROR_BUSY;
        } else {
            child->kill_cancel_event = CreateEventW(0, TRUE, FALSE, 0);
            child->pending_kill_request_id = request_id;
            child->pending_kill_client = client;
            (void)winxterm_job_manager_stopping(&host->bridge->job_manager, job_id);
            child->kill_thread = child->kill_cancel_event != 0 ?
                CreateThread(0, 0, winxterm_host_kill_thread, child, 0, 0) : 0;
            if (child->kill_thread == 0) {
                status = GetLastError();
                child->pending_kill_request_id = 0u;
                child->pending_kill_client = 0;
                winxterm_host_close_handle(&child->kill_cancel_event);
                (void)winxterm_job_manager_cancel_stopping(&host->bridge->job_manager, job_id);
            } else status = ERROR_SUCCESS;
        }
    } else if (child != 0) status = ERROR_INVALID_STATE;
    LeaveCriticalSection(&host->runtimes.lock);
    return status;
}

uint32_t winxterm_host_cancel_request(WinxtermHostClient *client, uint64_t request_id)
{
    WinxtermHostContext *host = client != 0 ? (WinxtermHostContext *)client->context : 0;
    if (host == 0) return ERROR_INVALID_PARAMETER;
    EnterCriticalSection(&host->runtimes.lock);
    uint32_t status = ERROR_NOT_FOUND;
    if (host->pending_root_kill_request_id == request_id &&
        host->pending_root_kill_client == client && host->root_kill_cancel_event != 0) {
        status = SetEvent(host->root_kill_cancel_event) ? ERROR_SUCCESS : GetLastError();
    }
    for (WinxtermHostManagedChild *child = host->runtimes.head; child != 0; child = child->next) {
        if (child->pending_kill_request_id == request_id &&
            child->pending_kill_client == client && child->kill_cancel_event != 0) {
            SetEvent(child->kill_cancel_event);
            status = ERROR_SUCCESS;
            break;
        }
    }
    LeaveCriticalSection(&host->runtimes.lock);
    return status;
}

uint32_t winxterm_host_lifecycle_request(WinxtermHostContext *host,
                                        WinxtermHostClient *client,
                                        uint16_t message_type,
                                        uint64_t target_id,
                                        uint32_t *removed_count)
{
    if (removed_count != 0) *removed_count = 0u;
    if (host == 0 || client == 0) return ERROR_INVALID_PARAMETER;
    if (message_type == WINXTERM_JOB_MESSAGE_CLEAN) {
        size_t removed = winxterm_job_manager_clean(&host->bridge->job_manager,
                                                    client->requester_id);
        if (removed_count != 0) {
            *removed_count = removed > UINT32_MAX ? UINT32_MAX : (uint32_t)removed;
        }
        winxterm_host_prune_removed_children(host);
        if (removed != 0u) winxterm_host_send_resync_event(host);
        return ERROR_SUCCESS;
    }
    if (!winxterm_job_manager_authorized(&host->bridge->job_manager,
                                         client->requester_id, target_id)) {
        return ERROR_ACCESS_DENIED;
    }
    bool changed = false;
    if (message_type == WINXTERM_JOB_MESSAGE_FOREGROUND) {
        uint64_t old_foreground =
            winxterm_job_manager_foreground_id(&host->bridge->job_manager);
        changed = winxterm_job_manager_foreground(&host->bridge->job_manager, target_id) &&
                  winxterm_host_switch_session(host, target_id);
        if (!changed && old_foreground != 0u) {
            (void)winxterm_job_manager_foreground(&host->bridge->job_manager,
                                                  old_foreground);
            (void)winxterm_host_switch_session(host, old_foreground);
        }
    } else if (message_type == WINXTERM_JOB_MESSAGE_BACKGROUND) {
        changed = winxterm_job_manager_background(&host->bridge->job_manager, target_id);
        if (changed) {
            winxterm_host_complete_foreground_request(host, target_id,
                                                      ERROR_SUCCESS, false, 0u);
            uint64_t restored =
                winxterm_job_manager_foreground_id(&host->bridge->job_manager);
            changed = winxterm_host_switch_session(host, restored);
        }
    } else if (message_type == WINXTERM_JOB_MESSAGE_REMOVE) {
        changed = winxterm_job_manager_remove(&host->bridge->job_manager,
                                              client->requester_id, target_id);
        if (changed) {
            winxterm_host_prune_removed_children(host);
            winxterm_host_send_resync_event(host);
        }
    } else {
        return ERROR_NOT_SUPPORTED;
    }
    return changed ? ERROR_SUCCESS : ERROR_INVALID_STATE;
}

static void winxterm_host_emit_file_summary(WinxtermHostContext *host,
                                            uint64_t bytes, ULONGLONG start_ms)
{
    if (host == 0 || start_ms == 0u) return;
    ULONGLONG elapsed_ms = GetTickCount64() - start_ms;
    uint64_t elapsed_ns = elapsed_ms > UINT64_MAX / 1000000ull ?
        UINT64_MAX : (uint64_t)elapsed_ms * 1000000ull;
    wchar_t byte_text[64], duration[64], speed[64], wide[256];
    winxterm_transfer_format_bytes(bytes, byte_text,
                                   sizeof(byte_text) / sizeof(byte_text[0]));
    winxterm_transfer_format_duration(elapsed_ns, duration,
                                      sizeof(duration) / sizeof(duration[0]));
    winxterm_transfer_format_speed(bytes, elapsed_ns, speed,
                                   sizeof(speed) / sizeof(speed[0]));
    int wide_length = _snwprintf_s(wide, sizeof(wide) / sizeof(wide[0]), _TRUNCATE,
                                   L"%ls written in %ls at %ls\r\n",
                                   byte_text, duration, speed);
    char summary[512];
    int length = wide_length > 0 ?
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, wide_length,
                            summary, sizeof(summary), 0, 0) : 0;
    if (length > 0) {
        (void)winxterm_host_queue_output(host->bridge,
                                         (const uint8_t *)summary, (size_t)length,
                                         host->shutdown_event);
    }
}

static DWORD WINAPI winxterm_host_managed_child_thread(void *context)
{
    WinxtermHostManagedChild *child = (WinxtermHostManagedChild *)context;
    uint8_t bytes[WINXTERM_HOST_OUTPUT_READ_CHUNK_BYTES];
    bool process_exited = false;
    for (;;) {
        if (!process_exited && winxterm_host_managed_child_all_exited(child)) {
            process_exited = true;
        }
        bool drained_attachment = false;
        bool attachment_failed = false;
        bool attachment_tee = false;
        size_t attachment_copied = 0u;
        EnterCriticalSection(&child->session.output_lock);
        if (child->attachment_file != 0 && child->session.journal.count != 0u) {
            size_t take = child->session.journal.count < sizeof(bytes) ?
                child->session.journal.count : sizeof(bytes);
            uint64_t next = child->session.journal.consumed_offset;
            bool more = false;
            bool copied = winxterm_job_journal_copy_snapshot(
                &child->session.journal,
                child->session.journal.consumed_offset,
                child->session.journal.produced_offset,
                bytes, take, &attachment_copied, &next, &more);
            attachment_tee = child->attachment_tee;
            attachment_failed = !copied || attachment_copied != take ||
                !winxterm_host_write_all(child->attachment_file,
                                         bytes, attachment_copied) ||
                winxterm_job_journal_consume(&child->session.journal,
                                             bytes, attachment_copied) !=
                    attachment_copied;
            if (!attachment_failed) {
                child->attachment_bytes += attachment_copied;
                drained_attachment = true;
            }
        }
        size_t attachment_retained =
            winxterm_job_journal_retained(&child->session.journal);
        LeaveCriticalSection(&child->session.output_lock);
        if (attachment_failed) {
            (void)winxterm_job_manager_fail(&child->host->bridge->job_manager,
                                            child->id, ERROR_WRITE_FAULT);
            break;
        }
        if (drained_attachment) {
            (void)winxterm_job_manager_set_output(&child->host->bridge->job_manager,
                                                  child->id,
                                                  attachment_retained, false);
            if (attachment_tee &&
                !winxterm_host_queue_output(child->host->bridge, bytes,
                                            attachment_copied,
                                            child->host->shutdown_event)) {
                (void)winxterm_job_manager_fail(&child->host->bridge->job_manager,
                                                child->id, ERROR_WRITE_FAULT);
                break;
            }
            continue;
        }
        DWORD available = 0u;
        if (!child->output_eof && !PeekNamedPipe(child->output_read, 0, 0, 0, &available, 0)) {
            DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
                child->output_eof = true;
            } else {
                (void)winxterm_job_manager_fail(&child->host->bridge->job_manager, child->id, error);
                break;
            }
        }
        DWORD redirected_available = 0u;
        if (child->redirected_output_read != 0 && !child->redirected_output_eof &&
            !PeekNamedPipe(child->redirected_output_read, 0, 0, 0,
                           &redirected_available, 0)) {
            DWORD error = GetLastError();
            if (error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED) {
                child->redirected_output_eof = true;
            } else {
                (void)winxterm_job_manager_fail(&child->host->bridge->job_manager, child->id, error);
                break;
            }
        }
        EnterCriticalSection(&child->host->runtimes.lock);
        bool active = child->host->active_session_id == child->id;
        bool connected = child->destination_id != 0u;
        if (active) {
            uint8_t input[512];
            size_t input_count = winxterm_bridge_read_session_input(
                child->host->bridge, child->id, input, sizeof(input));
            if (input_count != 0u) {
                DWORD written = 0u;
                (void)WriteFile(child->input_write, input, (DWORD)input_count, &written, 0);
            }
            int columns = 0, rows = 0;
            if (child->pseudo_console != 0 &&
                winxterm_bridge_peek_pending_resize(child->host->bridge, &columns, &rows)) {
                COORD size = {(SHORT)(columns > 0 ? columns : 1), (SHORT)(rows > 0 ? rows : 1)};
                if (SUCCEEDED(ResizePseudoConsole(child->pseudo_console, size))) {
                    (void)winxterm_bridge_ack_pending_resize(child->host->bridge, columns, rows);
                }
            }
        }
        LeaveCriticalSection(&child->host->runtimes.lock);
        EnterCriticalSection(&child->session.output_lock);
        size_t room = active && !connected ? sizeof(bytes) :
            child->session.journal.capacity - child->session.journal.count;
        bool full = (!active || connected) && room == 0u;
        LeaveCriticalSection(&child->session.output_lock);
        if (full) {
            (void)winxterm_job_manager_set_output(&child->host->bridge->job_manager,
                                                  child->id, WINXTERM_JOB_OUTPUT_LIMIT, true);
            if (process_exited && available == 0u && redirected_available == 0u) break;
            Sleep(10u);
            continue;
        }
        if (redirected_available != 0u) {
            DWORD take = redirected_available > sizeof(bytes) ? sizeof(bytes) : redirected_available;
            DWORD read = 0u;
            if (!ReadFile(child->redirected_output_read, bytes, take, &read, 0)) continue;
            bool written = winxterm_terminal_session_record(&child->session, bytes, read) &&
                           winxterm_host_write_all(child->redirected_file, bytes, read);
            if (written) child->redirected_bytes += read;
            if (written && child->redirected_tee) {
                written = winxterm_host_route_output(child->host, child->id,
                                                     &child->session.journal,
                                                     &child->session.output_lock,
                                                     bytes, read);
            }
            if (!written) {
                (void)winxterm_job_manager_fail(&child->host->bridge->job_manager,
                                                child->id, ERROR_WRITE_FAULT);
                break;
            }
            continue;
        }
        if (available != 0u) {
            DWORD take = available;
            if (take > sizeof(bytes)) take = sizeof(bytes);
            if ((size_t)take > room) take = (DWORD)room;
            DWORD read = 0u;
            if (!ReadFile(child->output_read, bytes, take, &read, 0)) continue;
            if (!winxterm_terminal_session_record(&child->session, bytes, read)) {
                (void)winxterm_job_manager_fail(&child->host->bridge->job_manager,
                                                child->id, ERROR_NOT_ENOUGH_MEMORY);
                break;
            }
            bool routed = winxterm_host_route_output(child->host, child->id,
                                                     &child->session.journal,
                                                     &child->session.output_lock,
                                                     bytes, read);
            if (!routed) {
                (void)winxterm_job_manager_fail(&child->host->bridge->job_manager,
                                                child->id, ERROR_BUFFER_OVERFLOW);
                break;
            }
            continue;
        }
        if (process_exited && available == 0u && redirected_available == 0u) break;
        Sleep(10u);
    }
    DWORD exit_code = 1u;
    HANDLE final_process = winxterm_host_managed_child_final_process(child);
    bool redirected = child->redirected_file != 0;
    if (redirected) {
        (void)FlushFileBuffers(child->redirected_file);
        winxterm_host_close_handle(&child->redirected_file);
    }
    bool attached = false;
    EnterCriticalSection(&child->session.output_lock);
    if (child->attachment_file != 0) {
        attached = true;
        (void)FlushFileBuffers(child->attachment_file);
        winxterm_host_close_handle(&child->attachment_file);
    }
    LeaveCriticalSection(&child->session.output_lock);
    if (final_process != 0 && GetExitCodeProcess(final_process, &exit_code) &&
        exit_code != STILL_ACTIVE) {
        (void)winxterm_job_manager_exit(&child->host->bridge->job_manager,
                                       child->id, exit_code);
        winxterm_host_send_event(child->host, WINXTERM_JOB_EVENT_EXITED, child->id);
        uint64_t restored = winxterm_job_manager_foreground_id(&child->host->bridge->job_manager);
        (void)winxterm_host_switch_session(child->host, restored);
        if (redirected) {
            winxterm_host_emit_file_summary(child->host, child->redirected_bytes,
                                            child->redirect_start_ms);
        }
        if (attached) {
            winxterm_host_emit_file_summary(child->host, child->attachment_bytes,
                                            child->attachment_start_ms);
        }
    }
    winxterm_host_complete_foreground_request(child->host, child->id,
                                              ERROR_SUCCESS, true, exit_code);
    winxterm_host_managed_child_close_runtime(child);
    return exit_code;
}

static void winxterm_host_managed_child_dispose(WinxtermHostManagedChild *child, bool terminate)
{
    if (child == 0) return;
    HANDLE final_process = winxterm_host_managed_child_final_process(child);
    if (terminate && child->process_job != 0 && final_process != 0 &&
        !winxterm_host_managed_child_all_exited(child)) {
        static const uint8_t interrupt = 0x03u;
        DWORD written = 0u;
        (void)WriteFile(child->input_write, &interrupt, 1u, &written, 0);
        if (WaitForSingleObject(final_process,
                                WINXTERM_JOB_TERMINATE_TIMEOUT_MS) == WAIT_TIMEOUT) {
            (void)TerminateJobObject(child->process_job, 1u);
        }
    }
    if (child->thread != 0) {
        (void)WaitForSingleObject(child->thread, 1000u);
        CloseHandle(child->thread);
    }
    if (child->connection_thread != 0) {
        EnterCriticalSection(&child->host->runtimes.lock);
        child->destination_id = 0u;
        LeaveCriticalSection(&child->host->runtimes.lock);
        (void)CancelSynchronousIo(child->connection_thread);
        (void)WaitForSingleObject(child->connection_thread, 1000u);
        winxterm_host_close_handle(&child->connection_thread);
    }
    winxterm_host_close_handle(&child->connection_input);
    if (child->client != 0 && !child->client_linked) {
        if (child->client->thread != 0) {
            (void)CancelSynchronousIo(child->client->thread);
            (void)WaitForSingleObject(child->client->thread, 1000u);
            winxterm_host_close_handle(&child->client->thread);
        }
        winxterm_host_close_handle(&child->client->request_read);
        winxterm_host_close_handle(&child->client->reply_write);
        winxterm_job_manager_snapshot_dispose(child->client->list_snapshot);
        if (child->client->reply_lock_initialized) {
            DeleteCriticalSection(&child->client->reply_lock);
        }
        free(child->client);
    }
    winxterm_host_managed_child_close_runtime(child);
    winxterm_terminal_session_dispose(&child->session);
    free(child);
}

static void winxterm_host_prune_removed_children(WinxtermHostContext *host)
{
    WinxtermHostManagedChild *removed = 0;
    EnterCriticalSection(&host->runtimes.lock);
    WinxtermHostManagedChild **link = &host->runtimes.head;
    while (*link != 0) {
        WinxtermManagedJobSnapshot snapshot;
        if (!winxterm_job_manager_snapshot_one(&host->bridge->job_manager, (*link)->id, &snapshot)) {
            WinxtermHostManagedChild *child = *link;
            *link = child->next;
            child->next = removed;
            removed = child;
        } else {
            link = &(*link)->next;
        }
    }
    LeaveCriticalSection(&host->runtimes.lock);
    while (removed != 0) {
        WinxtermHostManagedChild *next = removed->next;
        winxterm_host_managed_child_dispose(removed, false);
        removed = next;
    }
}

typedef struct WinxtermHostPipelineEdge {
    HANDLE read_handle;
    HANDLE write_handle;
} WinxtermHostPipelineEdge;

static bool winxterm_host_pipeline_plan_supported(const WinxtermJobExecutionPlan *plan)
{
    if (plan == 0 || plan->stage_count == 0u) return false;
    for (size_t i = 0u; i < plan->stage_count; ++i) {
        const WinxtermJobPlanStage *stage = &plan->stages[i];
        if (stage->stderr_endpoint != WINXTERM_JOB_PLAN_ENDPOINT_TERMINAL ||
            stage->stdin_endpoint != (i == 0u ? WINXTERM_JOB_PLAN_ENDPOINT_TERMINAL :
                                                WINXTERM_JOB_PLAN_ENDPOINT_PIPE) ||
            (i + 1u != plan->stage_count &&
             stage->stdout_endpoint != WINXTERM_JOB_PLAN_ENDPOINT_PIPE) ||
            (i + 1u == plan->stage_count &&
             stage->stdout_endpoint != WINXTERM_JOB_PLAN_ENDPOINT_TERMINAL &&
             stage->stdout_endpoint != WINXTERM_JOB_PLAN_ENDPOINT_FILE)) return false;
    }
    return plan->stage_count > 1u ||
           plan->stages[0].stdout_endpoint == WINXTERM_JOB_PLAN_ENDPOINT_FILE ||
           (plan->stages[0].flags & WINXTERM_JOB_STAGE_FLAG_ISOLATED_BUILTIN) != 0u;
}

static uint32_t winxterm_host_spawn_pipeline_plan(WinxtermHostContext *host,
                                                  const WinxtermJobExecutionPlan *plan,
                                                  uint64_t requester_id,
                                                  uint64_t *job_id, bool *foreground)
{
    uint32_t status = ERROR_NOT_ENOUGH_MEMORY;
    bool background = (plan->flags & WINXTERM_JOB_PLAN_FLAG_BACKGROUND) != 0u;
    WinxtermHostManagedChild *child =
        (WinxtermHostManagedChild *)calloc(1u, sizeof(*child));
    WinxtermHostPipelineEdge *edges = 0;
    HANDLE input_read = 0, output_write = 0, redirected_output_write = 0;
    wchar_t **environment = 0;
    wchar_t *environment_block = 0, *cwd = 0, *display_name = 0;
    bool registered = false;
    if (child == 0) goto cleanup;
    child->host = host;
    child->stage_count = plan->stage_count;
    child->stage_processes =
        (PROCESS_INFORMATION *)calloc(child->stage_count, sizeof(*child->stage_processes));
    edges = child->stage_count > 1u ?
        (WinxtermHostPipelineEdge *)calloc(child->stage_count - 1u, sizeof(*edges)) : 0;
    if (child->stage_processes == 0 || (child->stage_count > 1u && edges == 0) ||
        !winxterm_terminal_session_init(
            &child->session,
            host->bridge->screen.columns > 0 ? host->bridge->screen.columns : 80,
            host->bridge->screen.rows > 0 ? host->bridge->screen.rows : 24, true)) goto cleanup;
    cwd = winxterm_host_utf8_to_wide(plan->cwd);
    if (cwd == 0) { status = ERROR_NO_UNICODE_TRANSLATION; goto cleanup; }
    if (plan->environment_count != 0u) {
        environment = (wchar_t **)calloc(plan->environment_count, sizeof(*environment));
        if (environment == 0) goto cleanup;
        for (size_t i = 0u; i < plan->environment_count; ++i) {
            environment[i] = winxterm_host_utf8_to_wide(plan->environment[i]);
            if (environment[i] == 0) { status = ERROR_NO_UNICODE_TRANSLATION; goto cleanup; }
        }
    }
    environment_block = winxterm_host_build_environment((const wchar_t * const *)environment,
                                                         plan->environment_count);
    display_name = winxterm_host_utf8_to_wide(plan->stages[0].arguments[0]);
    if (environment_block == 0 || display_name == 0) goto cleanup;
    if (!winxterm_host_create_pipes(&input_read, &child->input_write,
                                    &child->output_read, &output_write)) {
        status = GetLastError();
        goto cleanup;
    }
    const WinxtermJobPlanStage *final_plan_stage = &plan->stages[plan->stage_count - 1u];
    if (final_plan_stage->stdout_endpoint == WINXTERM_JOB_PLAN_ENDPOINT_FILE) {
        SECURITY_ATTRIBUTES redirect_security = {sizeof(redirect_security), 0, TRUE};
        if (!CreatePipe(&child->redirected_output_read, &redirected_output_write,
                        &redirect_security, 0) ||
            !SetHandleInformation(child->redirected_output_read, HANDLE_FLAG_INHERIT, 0u)) {
            status = GetLastError();
            goto cleanup;
        }
        wchar_t *redirect_path = winxterm_host_utf8_to_wide(final_plan_stage->path);
        if (redirect_path == 0) { status = ERROR_NO_UNICODE_TRANSLATION; goto cleanup; }
        child->redirected_file = CreateFileW(
            redirect_path, GENERIC_WRITE, FILE_SHARE_READ, 0,
            (final_plan_stage->flags & WINXTERM_JOB_STAGE_FLAG_APPEND) != 0u ?
                OPEN_ALWAYS : CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, 0);
        free(redirect_path);
        if (child->redirected_file == INVALID_HANDLE_VALUE) {
            child->redirected_file = 0;
            status = GetLastError();
            goto cleanup;
        }
        if ((final_plan_stage->flags & WINXTERM_JOB_STAGE_FLAG_APPEND) != 0u) {
            LARGE_INTEGER zero;
            zero.QuadPart = 0;
            if (!SetFilePointerEx(child->redirected_file, zero, 0, FILE_END)) {
                status = GetLastError();
                goto cleanup;
            }
        }
        child->redirected_tee =
            (final_plan_stage->flags & WINXTERM_JOB_STAGE_FLAG_TEE) != 0u;
        child->redirect_start_ms = GetTickCount64();
    }
    SECURITY_ATTRIBUTES security = {sizeof(security), 0, TRUE};
    for (size_t i = 0u; i + 1u < child->stage_count; ++i) {
        if (!CreatePipe(&edges[i].read_handle, &edges[i].write_handle, &security, 0)) {
            status = GetLastError();
            goto cleanup;
        }
    }
    child->process_job = CreateJobObjectW(0, 0);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    memset(&limits, 0, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (child->process_job == 0 ||
        !SetInformationJobObject(child->process_job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        status = GetLastError();
        goto cleanup;
    }
    for (size_t stage_index = 0u; stage_index < child->stage_count; ++stage_index) {
        status = ERROR_NOT_ENOUGH_MEMORY;
        const WinxtermJobPlanStage *stage = &plan->stages[stage_index];
        wchar_t **arguments =
            (wchar_t **)calloc(stage->argument_count, sizeof(*arguments));
        wchar_t *command_line = 0;
        LPPROC_THREAD_ATTRIBUTE_LIST attributes = 0;
        if (arguments == 0) goto stage_cleanup;
        for (size_t i = 0u; i < stage->argument_count; ++i) {
            arguments[i] = winxterm_host_utf8_to_wide(stage->arguments[i]);
            if (arguments[i] == 0) { status = ERROR_NO_UNICODE_TRANSLATION; goto stage_cleanup; }
        }
        command_line = winxterm_host_build_command_line((const wchar_t * const *)arguments,
                                                         (int)stage->argument_count);
        HANDLE stage_input = stage_index == 0u ? input_read : edges[stage_index - 1u].read_handle;
        HANDLE stage_output = stage_index + 1u == child->stage_count ?
            (redirected_output_write != 0 ? redirected_output_write : output_write) :
            edges[stage_index].write_handle;
        STARTUPINFOEXW startup;
        if (command_line == 0 ||
            !winxterm_host_prepare_stdio_startup(stage_input, stage_output, output_write,
                                                 0, 0u,
                                                 &startup, &attributes)) {
            if (status == ERROR_NOT_ENOUGH_MEMORY) status = GetLastError();
            goto stage_cleanup;
        }
        WinxtermHostStandardHandles standard_handles = winxterm_host_clear_standard_handles();
        BOOL created = CreateProcessW(0, command_line, 0, 0, TRUE,
                                      EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                                          CREATE_SUSPENDED | CREATE_NO_WINDOW,
                                      environment_block, cwd, &startup.StartupInfo,
                                      &child->stage_processes[stage_index]);
        winxterm_host_restore_standard_handles(standard_handles);
        if (!created || !AssignProcessToJobObject(
                child->process_job, child->stage_processes[stage_index].hProcess)) {
            status = GetLastError();
            if (created) {
                (void)TerminateProcess(child->stage_processes[stage_index].hProcess, status);
            }
            goto stage_cleanup;
        }
        status = ERROR_SUCCESS;
stage_cleanup:
        if (attributes != 0) winxterm_host_destroy_startup(attributes);
        free(command_line);
        if (arguments != 0) {
            for (size_t i = 0u; i < stage->argument_count; ++i) free(arguments[i]);
        }
        free(arguments);
        if (status != ERROR_SUCCESS) goto cleanup;
    }
    winxterm_host_close_handle(&input_read);
    winxterm_host_close_handle(&output_write);
    winxterm_host_close_handle(&redirected_output_write);
    for (size_t i = 0u; i + 1u < child->stage_count; ++i) {
        winxterm_host_close_handle(&edges[i].read_handle);
        winxterm_host_close_handle(&edges[i].write_handle);
    }
    child->id = winxterm_job_manager_add(&host->bridge->job_manager, requester_id,
                                         background ? WINXTERM_JOB_BACKGROUND :
                                                      WINXTERM_JOB_FOREGROUND);
    if (child->id == 0u) { status = ERROR_NOT_ENOUGH_MEMORY; goto cleanup; }
    registered = true;
    PROCESS_INFORMATION *final_stage = &child->stage_processes[child->stage_count - 1u];
    (void)winxterm_job_manager_set_process(&host->bridge->job_manager, child->id,
                                          final_stage->dwProcessId, display_name);
    if ((plan->flags & WINXTERM_JOB_PLAN_FLAG_CONNECTABLE_STDIN) != 0u) {
        (void)winxterm_job_manager_set_connectable(&host->bridge->job_manager,
                                                   child->id, true);
    }
    EnterCriticalSection(&host->runtimes.lock);
    child->next = host->runtimes.head;
    host->runtimes.head = child;
    LeaveCriticalSection(&host->runtimes.lock);
    if (!background && !winxterm_host_switch_session(host, child->id)) {
        status = ERROR_INVALID_STATE;
        (void)winxterm_job_manager_fail(&host->bridge->job_manager, child->id, status);
        (void)TerminateJobObject(child->process_job, status);
        child = 0;
        goto cleanup;
    }
    child->thread = CreateThread(0, 0, winxterm_host_managed_child_thread, child, 0, 0);
    if (child->thread == 0) { status = GetLastError(); goto registered_failure; }
    for (size_t i = 0u; i < child->stage_count; ++i) {
        if (ResumeThread(child->stage_processes[i].hThread) == (DWORD)-1) {
            status = GetLastError();
            goto registered_failure;
        }
    }
    *job_id = child->id;
    *foreground = !background;
    winxterm_host_send_event(host, WINXTERM_JOB_EVENT_ADDED, child->id);
    child = 0;
    status = ERROR_SUCCESS;
    goto cleanup;

registered_failure:
    (void)winxterm_job_manager_fail(&host->bridge->job_manager, child->id, status);
    if (child->process_job != 0) (void)TerminateJobObject(child->process_job, status);
    (void)winxterm_host_switch_session(
        host, winxterm_job_manager_foreground_id(&host->bridge->job_manager));
    child = 0;
cleanup:
    winxterm_host_close_handle(&input_read);
    winxterm_host_close_handle(&output_write);
    winxterm_host_close_handle(&redirected_output_write);
    if (edges != 0) {
        for (size_t i = 0u; i + 1u < plan->stage_count; ++i) {
            winxterm_host_close_handle(&edges[i].read_handle);
            winxterm_host_close_handle(&edges[i].write_handle);
        }
    }
    free(edges);
    if (child != 0) winxterm_host_managed_child_dispose(child, true);
    if (environment != 0) {
        for (size_t i = 0u; i < plan->environment_count; ++i) free(environment[i]);
    }
    free(environment);
    free(environment_block);
    free(cwd);
    free(display_name);
    (void)registered;
    return status;
}

uint32_t winxterm_host_spawn_plan(WinxtermHostContext *host,
                                        const uint8_t *payload, size_t payload_length,
                                        uint64_t requester_id,
                                        uint64_t *job_id, bool *foreground)
{
    if (job_id != 0) *job_id = 0u;
    if (foreground != 0) *foreground = false;
    if (host == 0 || job_id == 0 || foreground == 0) return ERROR_INVALID_PARAMETER;
    WinxtermJobExecutionPlan plan;
    if (!winxterm_job_plan_decode(payload, payload_length, &plan)) return ERROR_INVALID_DATA;
    if (winxterm_host_pipeline_plan_supported(&plan)) {
        uint32_t pipeline_status =
            winxterm_host_spawn_pipeline_plan(host, &plan, requester_id,
                                              job_id, foreground);
        winxterm_job_plan_dispose(&plan);
        return pipeline_status;
    }
    bool background = (plan.flags & WINXTERM_JOB_PLAN_FLAG_BACKGROUND) != 0u;
    bool supported = plan.stage_count == 1u &&
                     (plan.stages[0].flags & WINXTERM_JOB_STAGE_FLAG_ISOLATED_BUILTIN) == 0u &&
                     plan.stages[0].stdin_endpoint != WINXTERM_JOB_PLAN_ENDPOINT_PIPE &&
                     plan.stages[0].stdout_endpoint == WINXTERM_JOB_PLAN_ENDPOINT_TERMINAL &&
                     plan.stages[0].stderr_endpoint == WINXTERM_JOB_PLAN_ENDPOINT_TERMINAL;
    if (!supported) { winxterm_job_plan_dispose(&plan); return ERROR_NOT_SUPPORTED; }

    uint32_t status = ERROR_NOT_ENOUGH_MEMORY;
    WinxtermHostManagedChild *child = (WinxtermHostManagedChild *)calloc(1u, sizeof(*child));
    wchar_t **arguments = 0;
    wchar_t **environment = 0;
    wchar_t *cwd = 0;
    wchar_t *command_line = 0;
    wchar_t *environment_block = 0;
    size_t environment_count = plan.environment_count;
    HANDLE input_read = 0, output_write = 0;
    HANDLE job_request_write = 0, job_reply_read = 0;
    HANDLE inherited_handles[2];
    size_t inherited_handle_count = 0u;
    LPPROC_THREAD_ATTRIBUTE_LIST attributes = 0;
    bool registered = false;
    if (child == 0) goto cleanup;
    child->host = host;
    if (!winxterm_terminal_session_init(
            &child->session,
            host->bridge->screen.columns > 0 ? host->bridge->screen.columns : 80,
            host->bridge->screen.rows > 0 ? host->bridge->screen.rows : 24,
            true)) goto cleanup;

    arguments = (wchar_t **)calloc(plan.stages[0].argument_count, sizeof(*arguments));
    if (arguments == 0) goto cleanup;
    for (size_t i = 0u; i < plan.stages[0].argument_count; ++i) {
        arguments[i] = winxterm_host_utf8_to_wide(plan.stages[0].arguments[i]);
        if (arguments[i] == 0) { status = ERROR_NO_UNICODE_TRANSLATION; goto cleanup; }
    }
    cwd = winxterm_host_utf8_to_wide(plan.cwd);
    if (cwd == 0) { status = ERROR_NO_UNICODE_TRANSLATION; goto cleanup; }
    bool nested_client = winxterm_host_is_dstshell(arguments[0]);
    if (nested_client) environment_count += 4u;
    if (environment_count != 0u) {
        environment = (wchar_t **)calloc(environment_count, sizeof(*environment));
        if (environment == 0) goto cleanup;
        for (size_t i = 0u; i < plan.environment_count; ++i) {
            environment[i] = winxterm_host_utf8_to_wide(plan.environment[i]);
            if (environment[i] == 0) { status = ERROR_NO_UNICODE_TRANSLATION; goto cleanup; }
        }
    }
    child->id = winxterm_job_manager_add(&host->bridge->job_manager, requester_id,
                                         background ? WINXTERM_JOB_BACKGROUND :
                                                      WINXTERM_JOB_FOREGROUND);
    if (child->id == 0u) goto cleanup;
    registered = true;
    if (nested_client) {
        child->client = (WinxtermHostClient *)calloc(1u, sizeof(*child->client));
        if (child->client == 0 ||
            !winxterm_host_create_job_pipes(&child->client->request_read,
                                            &job_request_write, &job_reply_read,
                                            &child->client->reply_write)) goto cleanup;
        child->client->context = host;
        child->client->requester_id = child->id;
        wchar_t values[4][96];
        int counts[4];
        counts[0] = swprintf_s(values[0], 96, L"%ls=%u", WINXTERM_JOB_ENV_PROTOCOL,
                               WINXTERM_JOB_PROTOCOL_VERSION);
        counts[1] = swprintf_s(values[1], 96, L"%ls=%llu", WINXTERM_JOB_ENV_REQUEST_HANDLE,
                               (unsigned long long)(uintptr_t)job_request_write);
        counts[2] = swprintf_s(values[2], 96, L"%ls=%llu", WINXTERM_JOB_ENV_REPLY_HANDLE,
                               (unsigned long long)(uintptr_t)job_reply_read);
        counts[3] = swprintf_s(values[3], 96, L"%ls=%llu", WINXTERM_JOB_ENV_SELF_ID,
                               (unsigned long long)child->id);
        for (size_t i = 0u; i < 4u; ++i) {
            if (counts[i] <= 0) goto cleanup;
            environment[plan.environment_count + i] = _wcsdup(values[i]);
            if (environment[plan.environment_count + i] == 0) goto cleanup;
        }
        inherited_handles[0] = job_request_write;
        inherited_handles[1] = job_reply_read;
        inherited_handle_count = 2u;
    }
    command_line = winxterm_host_build_command_line((const wchar_t * const *)arguments,
                                                     (int)plan.stages[0].argument_count);
    environment_block = winxterm_host_build_environment(
        (const wchar_t * const *)environment, environment_count);
    if (command_line == 0 || environment_block == 0) goto cleanup;
    if (!winxterm_host_create_pipes(&input_read, &child->input_write,
                                    &child->output_read, &output_write)) {
        status = GetLastError(); goto cleanup;
    }
    COORD size = {(SHORT)(host->bridge->screen.columns > 0 ? host->bridge->screen.columns : 80),
                  (SHORT)(host->bridge->screen.rows > 0 ? host->bridge->screen.rows : 24)};
    HRESULT hr = CreatePseudoConsole(size, input_read, output_write, 0, &child->pseudo_console);
    winxterm_host_close_handle(&input_read);
    winxterm_host_close_handle(&output_write);
    if (FAILED(hr)) { status = (uint32_t)hr; goto cleanup; }
    STARTUPINFOEXW startup;
    if (!winxterm_host_prepare_startup(child->pseudo_console, inherited_handles,
                                      inherited_handle_count, &startup, &attributes)) {
        status = GetLastError(); goto cleanup;
    }
    child->process_job = CreateJobObjectW(0, 0);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
    memset(&limits, 0, sizeof(limits));
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (child->process_job == 0 ||
        !SetInformationJobObject(child->process_job, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits))) {
        status = GetLastError(); goto cleanup;
    }
    WinxtermHostStandardHandles standard_handles = winxterm_host_clear_standard_handles();
    BOOL created = CreateProcessW(0, command_line, 0, 0,
                                  inherited_handle_count != 0u,
                                  EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                                      CREATE_SUSPENDED,
                                  environment_block, cwd, &startup.StartupInfo, &child->process);
    winxterm_host_restore_standard_handles(standard_handles);
    winxterm_host_close_handle(&job_request_write);
    winxterm_host_close_handle(&job_reply_read);
    free(environment_block);
    environment_block = 0;
    if (!created) { status = GetLastError(); goto cleanup; }
    if (!AssignProcessToJobObject(child->process_job, child->process.hProcess)) {
        status = GetLastError(); goto cleanup;
    }
    (void)winxterm_job_manager_set_process(&host->bridge->job_manager, child->id,
                                          child->process.dwProcessId, arguments[0]);
    if ((plan.flags & WINXTERM_JOB_PLAN_FLAG_CONNECTABLE_STDIN) != 0u) {
        (void)winxterm_job_manager_set_connectable(&host->bridge->job_manager, child->id, true);
    }
    EnterCriticalSection(&host->runtimes.lock);
    child->next = host->runtimes.head;
    host->runtimes.head = child;
    LeaveCriticalSection(&host->runtimes.lock);
    if (child->client != 0) {
        InitializeCriticalSection(&child->client->reply_lock);
        child->client->reply_lock_initialized = true;
        child->client->thread = CreateThread(0, 0, winxterm_job_dispatch_thread,
                                             child->client, 0, 0);
        if (child->client->thread == 0) {
            status = GetLastError();
            goto registered_failure;
        }
        child->client_linked = winxterm_job_coordinator_add_client(
            &host->bridge->job_coordinator, child->client);
        if (!child->client_linked) {
            status = ERROR_NOT_ENOUGH_MEMORY;
            goto registered_failure;
        }
    }
    if (!background && !winxterm_host_switch_session(host, child->id)) {
        status = ERROR_INVALID_STATE;
        (void)winxterm_job_manager_fail(&host->bridge->job_manager, child->id, status);
        goto registered_failure;
    }
    child->thread = CreateThread(0, 0, winxterm_host_managed_child_thread, child, 0, 0);
    if (child->thread == 0 || ResumeThread(child->process.hThread) == (DWORD)-1) {
        status = GetLastError();
        (void)winxterm_job_manager_fail(&host->bridge->job_manager, child->id, status);
        (void)winxterm_host_switch_session(host,
            winxterm_job_manager_foreground_id(&host->bridge->job_manager));
        /* It remains registered and will be cleaned with the host. */
        goto registered_failure;
    }
    *job_id = child->id;
    *foreground = !background;
    winxterm_host_send_event(host, WINXTERM_JOB_EVENT_ADDED, child->id);
    child = 0;
    status = ERROR_SUCCESS;
    goto cleanup;

registered_failure:
    (void)winxterm_job_manager_fail(&host->bridge->job_manager, child->id, status);
    if (child->process_job != 0) (void)TerminateJobObject(child->process_job, status);
    (void)winxterm_host_switch_session(
        host, winxterm_job_manager_foreground_id(&host->bridge->job_manager));
    child = 0;
cleanup:
    if (attributes != 0) winxterm_host_destroy_startup(attributes);
    winxterm_host_close_handle(&input_read);
    winxterm_host_close_handle(&output_write);
    winxterm_host_close_handle(&job_request_write);
    winxterm_host_close_handle(&job_reply_read);
    free(environment_block);
    if (registered && child != 0) {
        (void)winxterm_job_manager_fail(&host->bridge->job_manager, child->id, status);
        (void)winxterm_host_switch_session(
            host, winxterm_job_manager_foreground_id(&host->bridge->job_manager));
    }
    if (child != 0) winxterm_host_managed_child_dispose(child, true);
    if (arguments != 0) {
        for (size_t i = 0u; i < plan.stages[0].argument_count; ++i) free(arguments[i]);
    }
    free(arguments);
    if (environment != 0) {
        for (size_t i = 0u; i < environment_count; ++i) free(environment[i]);
    }
    free(environment);
    free(cwd);
    free(command_line);
    winxterm_job_plan_dispose(&plan);
    return status;
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
        if (host->pseudo_console == 0) {
            (void)winxterm_bridge_ack_pending_resize(bridge, columns, rows);
            return;
        }
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

static bool winxterm_host_write_pending_input(WinxtermBridge *bridge, uint64_t session_id,
                                              HANDLE input_write)
{
    uint8_t input[512];
    size_t input_count = winxterm_bridge_read_session_input(
        bridge, session_id, input, sizeof(input));
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
    feed_ok = winxterm_terminal_session_record(&host->root_session,
                                               output, output_count) &&
              winxterm_host_route_output(host, host->root_job_id,
                                        &host->root_session.journal,
                                        &host->root_session.output_lock,
                                        output, output_count);
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
    DWORD written = 0u;
    static const uint8_t interrupt = 0x03u;
    (void)WriteFile(host->input_write, &interrupt, 1u, &written, 0);
    DWORD wait_result = WaitForSingleObject(host->process.hProcess,
                                            WINXTERM_JOB_TERMINATE_TIMEOUT_MS);
    if (wait_result == WAIT_TIMEOUT) {
        winxterm_log_writef(host->bridge->log,
                            "ConPTY child did not exit after %lu ms, terminating",
                            (unsigned long)WINXTERM_JOB_TERMINATE_TIMEOUT_MS);
        if (host->process_job != 0) {
            (void)TerminateJobObject(host->process_job, 1u);
        } else {
            (void)TerminateProcess(host->process.hProcess, 1u);
        }
        host->force_terminated = true;
        (void)WaitForSingleObject(host->process.hProcess, WINXTERM_HOST_TERMINATE_WAIT_MS);
    }
}

static void winxterm_host_cleanup_context(WinxtermHostContext *host)
{
    if (host == 0) {
        return;
    }

    WinxtermHostClient *clients = winxterm_job_coordinator_detach_clients(
        &host->bridge->job_coordinator);
    for (WinxtermHostClient *client = clients; client != 0; client = client->next) {
        if (client->thread != 0) (void)CancelSynchronousIo(client->thread);
        winxterm_host_close_handle(&client->request_read);
        if (client->thread != 0) (void)WaitForSingleObject(client->thread, 1000u);
        winxterm_job_coordinator_stop_client(client);
        winxterm_host_close_handle(&client->reply_write);
    }
    host->job_request_read = 0;
    host->job_reply_write = 0;
    host->job_control_thread = 0;
    if (host->root_kill_cancel_event != 0) SetEvent(host->root_kill_cancel_event);
    if (host->root_kill_thread != 0) {
        (void)WaitForSingleObject(host->root_kill_thread, 1000u);
    }
    winxterm_host_close_handle(&host->root_kill_thread);
    winxterm_host_close_handle(&host->root_kill_cancel_event);
    WinxtermHostManagedChild *children = 0;
    if (host->runtimes.lock_initialized) {
        EnterCriticalSection(&host->runtimes.lock);
        children = host->runtimes.head;
        host->runtimes.head = 0;
        LeaveCriticalSection(&host->runtimes.lock);
    }
    while (children != 0) {
        WinxtermHostManagedChild *next = children->next;
        winxterm_host_managed_child_dispose(children, true);
        children = next;
    }
    while (clients != 0) {
        WinxtermHostClient *next = clients->next;
        winxterm_host_close_handle(&clients->thread);
        winxterm_job_manager_snapshot_dispose(clients->list_snapshot);
        if (clients->reply_lock_initialized) DeleteCriticalSection(&clients->reply_lock);
        free(clients);
        clients = next;
    }
    winxterm_host_close_handle(&host->process.hThread);
    winxterm_host_close_handle(&host->process.hProcess);
    winxterm_host_close_handle(&host->process_job);
    if (host->pseudo_console != 0) {
        ClosePseudoConsole(host->pseudo_console);
        host->pseudo_console = 0;
    }
    winxterm_host_close_handle(&host->input_write);
    winxterm_host_close_handle(&host->output_read);
    winxterm_terminal_session_dispose(&host->root_session);
    winxterm_managed_runtime_registry_dispose(&host->runtimes);
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
    winxterm_managed_runtime_registry_init(&host.runtimes);
    host.bridge = bridge;
    host.shutdown_event = shutdown_event;
    host.exit_code = 1;
    host.root_is_shell = winxterm_host_is_shell_root(argv[0]);
    (void)wcsncpy_s(host.root_executable,
                    sizeof(host.root_executable) / sizeof(host.root_executable[0]),
                    argv[0], _TRUNCATE);
    winxterm_bridge_set_host_starting(bridge);
    uint64_t root_job_id = winxterm_job_manager_add(&bridge->job_manager,
                                                    0u,
                                                    WINXTERM_JOB_FOREGROUND);
    if (root_job_id == 0u) {
        winxterm_host_cleanup_context(&host);
        winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
        return 1;
    }
    host.root_job_id = root_job_id;
    host.active_session_id = root_job_id;
    winxterm_bridge_set_active_session(bridge, root_job_id);
    uint8_t *startup_input = 0;
    size_t startup_input_count = 0u;
    if (!winxterm_bridge_switch_input_session(bridge, root_job_id, 0, 0u,
                                              &startup_input,
                                              &startup_input_count)) {
        winxterm_host_cleanup_context(&host);
        (void)winxterm_job_manager_fail(&bridge->job_manager, root_job_id,
                                        ERROR_NOT_ENOUGH_MEMORY);
        winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
        return 1;
    }
    free(startup_input);
    if (!winxterm_terminal_session_init(&host.root_session, 0, 0, false)) {
        winxterm_host_cleanup_context(&host);
        (void)winxterm_job_manager_fail(&bridge->job_manager, root_job_id,
                                        ERROR_NOT_ENOUGH_MEMORY);
        winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
        return 1;
    }
    host.process_job = CreateJobObjectW(0, 0);
    if (host.process_job == 0) {
        DWORD error = GetLastError();
        winxterm_host_cleanup_context(&host);
        (void)winxterm_job_manager_fail(&bridge->job_manager, root_job_id, error);
        winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
        return 1;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_limits;
    memset(&job_limits, 0, sizeof(job_limits));
    job_limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(host.process_job, JobObjectExtendedLimitInformation,
                                 &job_limits, sizeof(job_limits))) {
        DWORD error = GetLastError();
        winxterm_host_cleanup_context(&host);
        (void)winxterm_job_manager_fail(&bridge->job_manager, root_job_id, error);
        winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
        return 1;
    }

    HANDLE input_read = 0;
    HANDLE output_read = 0;
    HANDLE output_write = 0;
    wchar_t host_transport[16];
    DWORD host_transport_length = GetEnvironmentVariableW(L"WINXTERM_HOST_TRANSPORT",
                                                           host_transport,
                                                           16u);
    bool raw_stdio = host_transport_length == 5u &&
                     _wcsicmp(host_transport, L"stdio") == 0;
    if (!winxterm_host_create_pipes(&input_read, &host.input_write, &output_read, &output_write)) {
        DWORD error = GetLastError();
        winxterm_log_writef(bridge->log, "ConPTY pipe creation failed, error=%lu", (unsigned long)error);
        winxterm_host_cleanup_context(&host);
        (void)winxterm_job_manager_fail(&bridge->job_manager, root_job_id, error);
        winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
        return 1;
    }
    host.output_read = output_read;

    if (!raw_stdio) {
        COORD size;
        size.X = (SHORT)(bridge->screen.columns > 0 ?
            bridge->screen.columns : WINXTERM_TERMINAL_COLUMNS);
        size.Y = (SHORT)(bridge->screen.rows > 0 ?
            bridge->screen.rows : WINXTERM_TERMINAL_ROWS);
        HRESULT hr = CreatePseudoConsole(size, input_read, output_write, 0, &host.pseudo_console);
        winxterm_host_close_handle(&input_read);
        winxterm_host_close_handle(&output_write);
        if (FAILED(hr)) {
            winxterm_log_writef(bridge->log, "CreatePseudoConsole failed, hr=0x%08lx", (unsigned long)hr);
            winxterm_host_cleanup_context(&host);
            (void)winxterm_job_manager_fail(&bridge->job_manager, root_job_id, (uint32_t)hr);
            winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
            return 1;
        }
    }

    wchar_t *command_line = winxterm_host_build_command_line(argv, argc);
    if (command_line == 0) {
        winxterm_host_close_handle(&input_read);
        winxterm_host_close_handle(&output_write);
        winxterm_host_cleanup_context(&host);
        (void)winxterm_job_manager_fail(&bridge->job_manager, root_job_id, ERROR_NOT_ENOUGH_MEMORY);
        winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
        return 1;
    }

    HANDLE job_request_write = 0;
    HANDLE job_reply_read = 0;
    HANDLE inherited_handles[2];
    size_t inherited_handle_count = 0u;
    wchar_t protocol_env[64];
    wchar_t request_env[96];
    wchar_t reply_env[96];
    wchar_t self_env[64];
    const wchar_t *job_environment[4];
    size_t job_environment_count = 0u;
    bool job_channel = winxterm_host_is_dstshell(argv[0]) &&
        winxterm_host_create_job_pipes(&host.job_request_read,
                                       &job_request_write,
                                       &job_reply_read,
                                       &host.job_reply_write);
    if (job_channel) {
        int protocol_length = swprintf_s(protocol_env, 64, L"%ls=%u",
                                         WINXTERM_JOB_ENV_PROTOCOL,
                                         WINXTERM_JOB_PROTOCOL_VERSION);
        int request_length = swprintf_s(request_env, 96, L"%ls=%llu",
                                        WINXTERM_JOB_ENV_REQUEST_HANDLE,
                                        (unsigned long long)(uintptr_t)job_request_write);
        int reply_length = swprintf_s(reply_env, 96, L"%ls=%llu",
                                      WINXTERM_JOB_ENV_REPLY_HANDLE,
                                      (unsigned long long)(uintptr_t)job_reply_read);
        int self_length = swprintf_s(self_env, 64, L"%ls=%llu",
                                     WINXTERM_JOB_ENV_SELF_ID,
                                     (unsigned long long)root_job_id);
        job_channel = protocol_length > 0 && request_length > 0 && reply_length > 0 && self_length > 0;
    }
    if (job_channel) {
        inherited_handles[0] = job_request_write;
        inherited_handles[1] = job_reply_read;
        inherited_handle_count = 2u;
        job_environment[0] = protocol_env;
        job_environment[1] = request_env;
        job_environment[2] = reply_env;
        job_environment[3] = self_env;
        job_environment_count = 4u;
    } else {
        winxterm_host_close_handle(&host.job_request_read);
        winxterm_host_close_handle(&job_request_write);
        winxterm_host_close_handle(&job_reply_read);
        winxterm_host_close_handle(&host.job_reply_write);
    }

    wchar_t *environment = winxterm_host_build_environment(job_environment,
                                                           job_environment_count);
    if (environment == 0) {
        free(command_line);
        winxterm_host_close_handle(&input_read);
        winxterm_host_close_handle(&output_write);
        winxterm_host_close_handle(&job_request_write);
        winxterm_host_close_handle(&job_reply_read);
        winxterm_host_cleanup_context(&host);
        (void)winxterm_job_manager_fail(&bridge->job_manager, root_job_id, ERROR_NOT_ENOUGH_MEMORY);
        winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
        return 1;
    }

    STARTUPINFOEXW startup;
    LPPROC_THREAD_ATTRIBUTE_LIST attribute_list = 0;
    bool startup_prepared = raw_stdio ?
        winxterm_host_prepare_stdio_startup(input_read,
                                            output_write,
                                            output_write,
                                            inherited_handles,
                                            inherited_handle_count,
                                            &startup,
                                            &attribute_list) :
        winxterm_host_prepare_startup(host.pseudo_console,
                                     inherited_handles,
                                     inherited_handle_count,
                                     &startup,
                                     &attribute_list);
    if (!startup_prepared) {
        DWORD error = GetLastError();
        winxterm_log_writef(bridge->log, "host startup attribute setup failed, error=%lu", (unsigned long)error);
        free(command_line);
        free(environment);
        winxterm_host_close_handle(&input_read);
        winxterm_host_close_handle(&output_write);
        winxterm_host_close_handle(&job_request_write);
        winxterm_host_close_handle(&job_reply_read);
        winxterm_host_cleanup_context(&host);
        (void)winxterm_job_manager_fail(&bridge->job_manager, root_job_id, error);
        winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
        return 1;
    }

    WinxtermHostStandardHandles standard_handles = winxterm_host_clear_standard_handles();
    BOOL created = CreateProcessW(0,
                                  command_line,
                                  0,
                                  0,
                                  raw_stdio || inherited_handle_count != 0u,
                                  EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                                      CREATE_SUSPENDED | (raw_stdio ? CREATE_NO_WINDOW : 0u),
                                  environment,
                                  current_directory != 0 && current_directory[0] != L'\0' ? current_directory : 0,
                                  &startup.StartupInfo,
                                  &host.process);
    winxterm_host_restore_standard_handles(standard_handles);
    winxterm_host_close_handle(&input_read);
    winxterm_host_close_handle(&output_write);
    winxterm_host_close_handle(&job_request_write);
    winxterm_host_close_handle(&job_reply_read);
    free(command_line);
    free(environment);
    winxterm_host_destroy_startup(attribute_list);
    if (!created) {
        DWORD error = GetLastError();
        winxterm_log_writef(bridge->log, "ConPTY client launch failed, error=%lu", (unsigned long)error);
        winxterm_host_cleanup_context(&host);
        (void)winxterm_job_manager_fail(&bridge->job_manager, root_job_id, error);
        winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
        return 1;
    }
    if (!AssignProcessToJobObject(host.process_job, host.process.hProcess)) {
        DWORD error = GetLastError();
        (void)TerminateProcess(host.process.hProcess, error);
        winxterm_host_cleanup_context(&host);
        (void)winxterm_job_manager_fail(&bridge->job_manager, root_job_id, error);
        winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
        return 1;
    }
    if (ResumeThread(host.process.hThread) == (DWORD)-1) {
        DWORD error = GetLastError();
        (void)TerminateJobObject(host.process_job, error);
        winxterm_host_cleanup_context(&host);
        (void)winxterm_job_manager_fail(&bridge->job_manager, root_job_id, error);
        winxterm_bridge_clear_host_child(bridge, WINXTERM_HOST_STATE_FAILED);
        return 1;
    }
    (void)winxterm_job_manager_set_process(&bridge->job_manager, root_job_id,
                                          host.process.dwProcessId, argv[0]);
    if (host.job_request_read != 0 && host.job_reply_write != 0) {
        WinxtermHostClient *client = (WinxtermHostClient *)calloc(1u, sizeof(*client));
        if (client != 0) {
            client->context = &host;
            client->requester_id = root_job_id;
            client->request_read = host.job_request_read;
            client->reply_write = host.job_reply_write;
            InitializeCriticalSection(&client->reply_lock);
            client->reply_lock_initialized = true;
            client->thread = CreateThread(0, 0, winxterm_job_dispatch_thread,
                                          client, 0, 0);
        }
        if (client == 0 || client->thread == 0) {
            winxterm_log_writef(bridge->log, "job-control thread creation failed, error=%lu",
                                (unsigned long)GetLastError());
            if (client != 0) {
                if (client->reply_lock_initialized) DeleteCriticalSection(&client->reply_lock);
                free(client);
            }
            winxterm_host_close_handle(&host.job_request_read);
            winxterm_host_close_handle(&host.job_reply_write);
        } else if (winxterm_job_coordinator_add_client(&bridge->job_coordinator, client)) {
            host.job_control_thread = client->thread;
        } else {
            winxterm_log_writef(bridge->log,
                                "job-control event queue creation failed, error=%lu",
                                (unsigned long)GetLastError());
            (void)CancelSynchronousIo(client->thread);
            winxterm_host_close_handle(&client->request_read);
            (void)WaitForSingleObject(client->thread, 1000u);
            winxterm_host_close_handle(&client->thread);
            winxterm_host_close_handle(&client->reply_write);
            if (client->reply_lock_initialized) DeleteCriticalSection(&client->reply_lock);
            free(client);
            host.job_request_read = 0;
            host.job_reply_write = 0;
        }
    }
    winxterm_bridge_set_host_child(bridge, host.process.dwProcessId, argv[0],
                                   host.root_is_shell);
    winxterm_log_writef(bridge->log,
                        "ConPTY client started pid=%lu",
                        (unsigned long)host.process.dwProcessId);
    bool running = true;
    while (running) {
        WinxtermBridgeJobAction job_action;
        uint64_t action_job_id = 0u;
        while (winxterm_bridge_take_job_action(bridge, &job_action, &action_job_id)) {
            if (job_action == WINXTERM_BRIDGE_JOB_ACTION_FOREGROUND &&
                winxterm_job_manager_foreground(&bridge->job_manager, action_job_id)) {
                (void)winxterm_host_switch_session(&host, action_job_id);
            } else if (job_action == WINXTERM_BRIDGE_JOB_ACTION_VIEW) {
                uint8_t *view_bytes = 0;
                size_t view_count = 0u;
                if (winxterm_host_copy_native_view(&host, action_job_id,
                                                   &view_bytes, &view_count)) {
                    if (!winxterm_bridge_publish_job_view(bridge, action_job_id,
                                                          view_bytes, view_count)) {
                        free(view_bytes);
                    }
                }
            } else if (job_action == WINXTERM_BRIDGE_JOB_ACTION_BACKGROUND &&
                       winxterm_job_manager_background(&bridge->job_manager, action_job_id)) {
                winxterm_host_complete_foreground_request(&host, action_job_id,
                                                          ERROR_SUCCESS, false, 0u);
                (void)winxterm_host_switch_session(&host,
                    winxterm_job_manager_foreground_id(&bridge->job_manager));
            } else if (job_action == WINXTERM_BRIDGE_JOB_ACTION_CLOSE ||
                       job_action == WINXTERM_BRIDGE_JOB_ACTION_FORCE_EXIT) {
                if (action_job_id == root_job_id) {
                    winxterm_bridge_request_terminate(bridge);
                } else {
                    (void)winxterm_host_signal_job(&host, root_job_id, action_job_id,
                        job_action == WINXTERM_BRIDGE_JOB_ACTION_FORCE_EXIT);
                }
            }
        }
        bool headless = winxterm_bridge_is_headless(bridge);
        if (headless && host.root_is_shell && !host.child_exited &&
            !host.root_headless_shutdown_requested) {
            host.root_headless_shutdown_requested = true;
            winxterm_log_writef(bridge->log,
                                "window closed; stopping root shell while managed jobs continue");
            winxterm_host_request_child_shutdown(&host);
            if (WaitForSingleObject(host.process.hProcess, 0) == WAIT_OBJECT_0) {
                winxterm_host_record_root_exit(&host);
            }
        }
        EnterCriticalSection(&host.runtimes.lock);
        bool root_active = host.active_session_id == root_job_id;
        if (!headless && root_active) {
            winxterm_host_apply_pending_resize(&host);
            if (!winxterm_host_write_pending_input(bridge, root_job_id,
                                                   host.input_write)) {
                if (WaitForSingleObject(shutdown_event, 0) != WAIT_OBJECT_0 &&
                    !winxterm_bridge_terminate_requested(bridge)) {
                    winxterm_bridge_set_host_state(bridge, WINXTERM_HOST_STATE_FAILED);
                }
                running = false;
                LeaveCriticalSection(&host.runtimes.lock);
                break;
            }
        }
        LeaveCriticalSection(&host.runtimes.lock);

        if (host.child_exited) {
            if (!winxterm_host_has_live_children(&host)) {
                if (host.root_is_shell || headless) {
                    running = false;
                } else if (!host.root_exit_notice_shown) {
                    winxterm_bridge_clear_input(bridge);
                    host.root_exit_notice_shown = winxterm_host_show_root_exit_notice(&host);
                    if (!host.root_exit_notice_shown) running = false;
                } else {
                    uint8_t key[16];
                    if (winxterm_bridge_read_input(bridge, key, sizeof(key)) != 0u) running = false;
                }
                if (running) Sleep(10u);
                continue;
            }
            HANDLE waits[2] = {shutdown_event, bridge->input_ready_event};
            DWORD wait_result = WaitForMultipleObjects(2u, waits, FALSE, 10u);
            if (wait_result == WAIT_OBJECT_0) {
                winxterm_bridge_request_terminate(bridge);
                running = false;
            }
            continue;
        }

        bool read_any = false;
        EnterCriticalSection(&host.root_session.output_lock);
        bool root_backpressured = !root_active &&
            winxterm_job_journal_backpressured(&host.root_session.journal);
        LeaveCriticalSection(&host.root_session.output_lock);
        if (root_backpressured) {
            Sleep(10u);
        } else if (!winxterm_host_read_available_output(&host, &read_any)) {
            if (WaitForSingleObject(host.process.hProcess, 0) == WAIT_OBJECT_0) {
                winxterm_host_record_root_exit(&host);
            } else {
                if (!winxterm_bridge_is_headless(bridge)) {
                    winxterm_bridge_set_host_state(bridge, WINXTERM_HOST_STATE_FAILED);
                }
                running = false;
            }
        } else if (read_any) {
            continue;
        } else {
            HANDLE waits[3] = {host.process.hProcess, shutdown_event, bridge->input_ready_event};
            DWORD wait_result = WaitForMultipleObjects(3, waits, FALSE, 10);
            if (wait_result == WAIT_OBJECT_0) {
                winxterm_host_record_root_exit(&host);
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

    if (!host.child_exited && WaitForSingleObject(host.process.hProcess, 0) == WAIT_OBJECT_0) {
        winxterm_host_record_root_exit(&host);
    }
    winxterm_host_cleanup_context(&host);
    if (!host.child_exited) {
        (void)winxterm_job_manager_exit(&bridge->job_manager, root_job_id,
                                        (uint32_t)host.exit_code);
    }
    winxterm_bridge_clear_host_child(bridge,
                                     shutdown_requested && host.force_terminated ?
                                         WINXTERM_HOST_STATE_STOPPED :
                                         WINXTERM_HOST_STATE_CHILD_EXITED);
    return host.exit_code;
}
