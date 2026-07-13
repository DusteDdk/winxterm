#include "dstcmd/winxterm_dstcmd_job_client.h"
#include "winxterm_job_manager.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct WinxtermDstcmdJobPending {
    uint64_t request_id;
    HANDLE completed;
    WinxtermJobFrame reply;
    bool has_reply;
    WinxtermDstcmdJobPending *next;
};

struct WinxtermDstcmdJobQueuedEvent {
    WinxtermDstcmdJobEvent value;
    WinxtermDstcmdJobQueuedEvent *next;
};

static bool winxterm_dstcmd_job_parse_event(const WinxtermJobFrame *frame,
                                            WinxtermDstcmdJobEvent *event)
{
    memset(event, 0, sizeof(*event));
    bool kind = false, id = false, state = false, exit_code = false;
    WinxtermJobTlvReader reader;
    WinxtermJobTlv tlv;
    winxterm_job_tlv_reader_init(&reader, frame->payload, frame->header.payload_length);
    while (reader.offset < reader.length && winxterm_job_tlv_next(&reader, &tlv)) {
        if (tlv.type == WINXTERM_JOB_TLV_EVENT_KIND && !kind) {
            kind = winxterm_job_tlv_read_u32(&tlv, &event->kind);
        } else if (tlv.type == WINXTERM_JOB_TLV_JOB_ID && !id) {
            id = winxterm_job_tlv_read_u64(&tlv, &event->job_id);
        } else if (tlv.type == WINXTERM_JOB_TLV_STATE && !state) {
            state = winxterm_job_tlv_read_u32(&tlv, &event->state);
        } else if (tlv.type == WINXTERM_JOB_TLV_EXIT_CODE && !exit_code) {
            exit_code = winxterm_job_tlv_read_u32(&tlv, &event->exit_code);
        } else return false;
    }
    return kind && id && state && exit_code &&
           (event->kind == WINXTERM_JOB_EVENT_EXITED ||
            event->kind == WINXTERM_JOB_EVENT_FOREGROUND_CHANGED ||
            event->kind == WINXTERM_JOB_EVENT_CONNECTED ||
            event->kind == WINXTERM_JOB_EVENT_DISCONNECTED ||
            event->kind == WINXTERM_JOB_EVENT_ADDED ||
            event->kind == WINXTERM_JOB_EVENT_RESYNC_REQUIRED);
}

static bool winxterm_dstcmd_job_env_u64(const wchar_t *name, uint64_t *value)
{
    wchar_t text[64];
    DWORD length = GetEnvironmentVariableW(name, text, (DWORD)(sizeof(text) / sizeof(text[0])));
    if (length == 0u || length >= sizeof(text) / sizeof(text[0])) return false;
    wchar_t *end = 0;
    errno = 0;
    unsigned long long parsed = wcstoull(text, &end, 0);
    if (errno != 0 || end == text || *end != L'\0') return false;
    *value = (uint64_t)parsed;
    return true;
}

static bool winxterm_dstcmd_job_pipe_handle(uint64_t encoded, HANDLE *handle)
{
    if (handle == 0 || encoded == 0u || encoded > (uint64_t)(uintptr_t)-1) return false;
    HANDLE candidate = (HANDLE)(uintptr_t)encoded;
    DWORD flags = 0u;
    if (!GetHandleInformation(candidate, &flags) || GetFileType(candidate) != FILE_TYPE_PIPE) return false;
    *handle = candidate;
    return true;
}

static DWORD WINAPI winxterm_dstcmd_job_reader(void *context)
{
    WinxtermDstcmdJobClient *client = (WinxtermDstcmdJobClient *)context;
    for (;;) {
        WinxtermJobFrame frame;
        if (!winxterm_job_channel_read(client->reply_handle, &frame)) break;
        if (frame.header.type == WINXTERM_JOB_MESSAGE_REPLY) {
            EnterCriticalSection(&client->lock);
            WinxtermDstcmdJobPending *pending = client->pending;
            while (pending != 0 && pending->request_id != frame.header.request_id) pending = pending->next;
            if (pending != 0 && !pending->has_reply) {
                pending->reply = frame;
                pending->has_reply = true;
                memset(&frame, 0, sizeof(frame));
                SetEvent(pending->completed);
            }
            LeaveCriticalSection(&client->lock);
        } else if (frame.header.type == WINXTERM_JOB_MESSAGE_EVENT) {
            WinxtermDstcmdJobQueuedEvent *queued =
                (WinxtermDstcmdJobQueuedEvent *)calloc(1u, sizeof(*queued));
            if (queued != 0 && winxterm_dstcmd_job_parse_event(&frame, &queued->value)) {
                EnterCriticalSection(&client->lock);
                if (client->event_count < 256u) {
                    if (client->event_tail != 0) client->event_tail->next = queued;
                    else client->event_head = queued;
                    client->event_tail = queued;
                    ++client->event_count;
                    if (client->event_ready != 0) SetEvent(client->event_ready);
                    queued = 0;
                } else {
                    client->event_overflowed = true;
                    if (client->event_ready != 0) SetEvent(client->event_ready);
                }
                LeaveCriticalSection(&client->lock);
            }
            free(queued);
        }
        /* Event dispatch is intentionally decoupled from request delivery. The
           lifecycle command layer can add listeners without changing this reader. */
        winxterm_job_frame_dispose(&frame);
    }
    EnterCriticalSection(&client->lock);
    client->available = false;
    for (WinxtermDstcmdJobPending *pending = client->pending; pending != 0; pending = pending->next) {
        SetEvent(pending->completed);
    }
    LeaveCriticalSection(&client->lock);
    return 0u;
}

static void winxterm_dstcmd_job_remove_pending(WinxtermDstcmdJobClient *client,
                                               WinxtermDstcmdJobPending *pending)
{
    WinxtermDstcmdJobPending **link = &client->pending;
    while (*link != 0 && *link != pending) link = &(*link)->next;
    if (*link == pending) *link = pending->next;
}

static bool winxterm_dstcmd_job_request(WinxtermDstcmdJobClient *client, uint16_t type,
                                       uint32_t flags, const uint8_t *payload,
                                       uint32_t payload_length, DWORD timeout,
                                       WinxtermJobFrame *reply)
{
    if (client == 0 || reply == 0 || !client->available) return false;
    memset(reply, 0, sizeof(*reply));
    WinxtermDstcmdJobPending pending;
    memset(&pending, 0, sizeof(pending));
    pending.completed = CreateEventW(0, TRUE, FALSE, 0);
    if (pending.completed == 0) return false;

    EnterCriticalSection(&client->lock);
    pending.request_id = client->next_request_id++;
    if (pending.request_id == 0u) pending.request_id = client->next_request_id++;
    pending.next = client->pending;
    client->pending = &pending;
    LeaveCriticalSection(&client->lock);

    WinxtermJobFrameHeader header = {WINXTERM_JOB_PROTOCOL_VERSION, type, flags,
                                    pending.request_id, payload_length};
    EnterCriticalSection(&client->write_lock);
    bool wrote = client->available &&
                 winxterm_job_channel_write(client->request_handle, &header, payload);
    LeaveCriticalSection(&client->write_lock);
    DWORD wait = wrote ? WaitForSingleObject(pending.completed, timeout) : WAIT_FAILED;

    EnterCriticalSection(&client->lock);
    winxterm_dstcmd_job_remove_pending(client, &pending);
    bool ok = wait == WAIT_OBJECT_0 && pending.has_reply;
    if (ok) {
        *reply = pending.reply;
        memset(&pending.reply, 0, sizeof(pending.reply));
    }
    LeaveCriticalSection(&client->lock);
    winxterm_job_frame_dispose(&pending.reply);
    CloseHandle(pending.completed);
    return ok;
}

static bool winxterm_dstcmd_job_reply_status(const WinxtermJobFrame *reply,
                                            uint32_t *status, uint32_t *flags)
{
    if (status != 0) *status = ERROR_INVALID_DATA;
    if (flags != 0) *flags = 0u;
    if (reply == 0 || reply->header.type != WINXTERM_JOB_MESSAGE_REPLY) return false;
    bool found = false;
    WinxtermJobTlvReader reader;
    WinxtermJobTlv tlv;
    winxterm_job_tlv_reader_init(&reader, reply->payload, reply->header.payload_length);
    while (reader.offset < reader.length) {
        if (!winxterm_job_tlv_next(&reader, &tlv)) return false;
        if (tlv.type == WINXTERM_JOB_TLV_STATUS) {
            if (found || status == 0 || !winxterm_job_tlv_read_u32(&tlv, status)) return false;
            found = true;
        } else if (tlv.type == WINXTERM_JOB_TLV_FLAGS && flags != 0) {
            if (!winxterm_job_tlv_read_u32(&tlv, flags)) return false;
        }
    }
    return found;
}

void winxterm_dstcmd_job_client_init(WinxtermDstcmdJobClient *client)
{
    if (client == 0) return;
    memset(client, 0, sizeof(*client));
    uint64_t protocol = 0u, request = 0u, reply = 0u, self_id = 0u;
    bool valid = winxterm_dstcmd_job_env_u64(WINXTERM_JOB_ENV_PROTOCOL, &protocol) &&
                 protocol == WINXTERM_JOB_PROTOCOL_VERSION &&
                 winxterm_dstcmd_job_env_u64(WINXTERM_JOB_ENV_REQUEST_HANDLE, &request) &&
                 winxterm_dstcmd_job_env_u64(WINXTERM_JOB_ENV_REPLY_HANDLE, &reply) &&
                 winxterm_dstcmd_job_env_u64(WINXTERM_JOB_ENV_SELF_ID, &self_id) && self_id != 0u &&
                 winxterm_dstcmd_job_pipe_handle(request, &client->request_handle) &&
                 winxterm_dstcmd_job_pipe_handle(reply, &client->reply_handle) &&
                 client->request_handle != client->reply_handle;
    (void)SetEnvironmentVariableW(WINXTERM_JOB_ENV_PROTOCOL, 0);
    (void)SetEnvironmentVariableW(WINXTERM_JOB_ENV_REQUEST_HANDLE, 0);
    (void)SetEnvironmentVariableW(WINXTERM_JOB_ENV_REPLY_HANDLE, 0);
    (void)SetEnvironmentVariableW(WINXTERM_JOB_ENV_SELF_ID, 0);
    if (!valid) { client->request_handle = 0; client->reply_handle = 0; return; }
    (void)SetHandleInformation(client->request_handle, HANDLE_FLAG_INHERIT, 0u);
    (void)SetHandleInformation(client->reply_handle, HANDLE_FLAG_INHERIT, 0u);
    InitializeCriticalSection(&client->lock);
    client->lock_initialized = true;
    InitializeCriticalSection(&client->write_lock);
    client->write_lock_initialized = true;
    client->self_id = self_id;
    client->event_ready = CreateEventW(0, TRUE, FALSE, 0);
    if (client->event_ready == 0) {
        client->available = false;
        return;
    }
    client->next_request_id = 1u;
    client->available = true;
    client->reader_thread = CreateThread(0, 0, winxterm_dstcmd_job_reader, client, 0, 0);
    if (client->reader_thread == 0 ||
        !winxterm_dstcmd_job_client_query_capabilities(client, &client->capabilities)) {
        client->available = false;
    }
}

void winxterm_dstcmd_job_client_dispose(WinxtermDstcmdJobClient *client)
{
    if (client == 0) return;
    client->stopping = true;
    client->available = false;
    if (client->request_handle != 0 && client->request_handle != INVALID_HANDLE_VALUE) CloseHandle(client->request_handle);
    client->request_handle = 0;
    if (client->reader_thread != 0) (void)CancelSynchronousIo(client->reader_thread);
    if (client->reply_handle != 0 && client->reply_handle != INVALID_HANDLE_VALUE) CloseHandle(client->reply_handle);
    client->reply_handle = 0;
    if (client->reader_thread != 0) {
        (void)WaitForSingleObject(client->reader_thread, 1000u);
        CloseHandle(client->reader_thread);
    }
    if (client->event_ready != 0) CloseHandle(client->event_ready);
    WinxtermDstcmdJobQueuedEvent *event = client->event_head;
    while (event != 0) {
        WinxtermDstcmdJobQueuedEvent *next = event->next;
        free(event);
        event = next;
    }
    if (client->write_lock_initialized) DeleteCriticalSection(&client->write_lock);
    if (client->lock_initialized) DeleteCriticalSection(&client->lock);
    memset(client, 0, sizeof(*client));
}

bool winxterm_dstcmd_job_client_poll_event(WinxtermDstcmdJobClient *client,
                                          WinxtermDstcmdJobEvent *event)
{
    if (client == 0 || event == 0 || !client->lock_initialized) return false;
    EnterCriticalSection(&client->lock);
    WinxtermDstcmdJobQueuedEvent *queued = client->event_head;
    bool overflowed = queued == 0 && client->event_overflowed;
    if (overflowed) {
        memset(event, 0, sizeof(*event));
        event->kind = WINXTERM_JOB_EVENT_RESYNC_REQUIRED;
        client->event_overflowed = false;
    } else if (queued != 0) {
        client->event_head = queued->next;
        if (client->event_head == 0) client->event_tail = 0;
        --client->event_count;
        *event = queued->value;
    }
    if (client->event_head == 0 && !client->event_overflowed && client->event_ready != 0) {
        ResetEvent(client->event_ready);
    }
    LeaveCriticalSection(&client->lock);
    free(queued);
    return queued != 0 || overflowed;
}

HANDLE winxterm_dstcmd_job_client_event_handle(const WinxtermDstcmdJobClient *client)
{ return client != 0 ? client->event_ready : 0; }

bool winxterm_dstcmd_job_client_available(const WinxtermDstcmdJobClient *client)
{ return client != 0 && client->available; }

uint32_t winxterm_dstcmd_job_client_capabilities(const WinxtermDstcmdJobClient *client)
{ return client != 0 ? client->capabilities : 0u; }

bool winxterm_dstcmd_job_client_query_capabilities(WinxtermDstcmdJobClient *client,
                                                   uint32_t *capabilities)
{
    if (capabilities != 0) *capabilities = 0u;
    if (client == 0 || capabilities == 0) return false;
    WinxtermJobFrame reply;
    if (!winxterm_dstcmd_job_request(client, WINXTERM_JOB_MESSAGE_CAPABILITIES, 0u, 0, 0u,
                                     WINXTERM_DSTCMD_JOB_REQUEST_TIMEOUT_MS, &reply)) return false;
    uint32_t status = ERROR_INVALID_DATA;
    bool ok = winxterm_dstcmd_job_reply_status(&reply, &status, capabilities) && status == ERROR_SUCCESS;
    winxterm_job_frame_dispose(&reply);
    return ok;
}

bool winxterm_dstcmd_job_client_interactive(WinxtermDstcmdJobClient *client,
                                           uint32_t *status)
{
    if (status != 0) *status = ERROR_INVALID_DATA;
    if (client == 0 || status == 0) return false;
    WinxtermJobFrame reply;
    if (!winxterm_dstcmd_job_request(client, WINXTERM_JOB_MESSAGE_INTERACTIVE,
                                     0u, 0, 0u,
                                     WINXTERM_DSTCMD_JOB_REQUEST_TIMEOUT_MS,
                                     &reply)) return false;
    bool ok = winxterm_dstcmd_job_reply_status(&reply, status, 0);
    winxterm_job_frame_dispose(&reply);
    return ok;
}

static bool winxterm_dstcmd_job_utf8(const WinxtermJobTlv *tlv, wchar_t *out, size_t capacity)
{
    if (tlv == 0 || out == 0 || capacity == 0u || tlv->length > INT_MAX) return false;
    int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, (const char *)tlv->value,
                                    (int)tlv->length, out, (int)(capacity - 1u));
    if (count < 0 || (tlv->length != 0u && count == 0)) return false;
    out[count] = L'\0';
    return true;
}

bool winxterm_dstcmd_job_client_list(WinxtermDstcmdJobClient *client,
                                    WinxtermDstcmdJobInfo **jobs, size_t *job_count)
{
    if (jobs != 0) *jobs = 0;
    if (job_count != 0) *job_count = 0u;
    if (!winxterm_dstcmd_job_client_available(client) || jobs == 0 || job_count == 0 ||
        (client->capabilities & WINXTERM_JOB_CAPABILITY_LIST) == 0u) return false;
    WinxtermDstcmdJobInfo *items = 0;
    size_t count = 0u;
    uint64_t cursor = 0u, snapshot_max_id = 0u;
    bool more;
    do {
        uint8_t request_payload[48];
        size_t request_length = 0u;
        bool encoded = winxterm_job_tlv_append_u64(request_payload, sizeof(request_payload), &request_length,
                                                   WINXTERM_JOB_TLV_CURSOR, cursor) &&
                       winxterm_job_tlv_append_u64(request_payload, sizeof(request_payload), &request_length,
                                                   WINXTERM_JOB_TLV_SNAPSHOT_MAX_ID, snapshot_max_id);
        WinxtermJobFrame reply;
        if (!encoded || !winxterm_dstcmd_job_request(client, WINXTERM_JOB_MESSAGE_LIST, 0u,
                                                     request_payload, (uint32_t)request_length,
                                                     WINXTERM_DSTCMD_JOB_REQUEST_TIMEOUT_MS,
                                                     &reply)) {
            free(items); return false;
        }
        bool ok = true, saw_status = false, after_cursor = false;
        uint32_t status = ERROR_INVALID_DATA, page_flags = 0u;
        WinxtermJobTlvReader reader;
        WinxtermJobTlv tlv;
        winxterm_job_tlv_reader_init(&reader, reply.payload, reply.header.payload_length);
        while (ok && reader.offset < reader.length && winxterm_job_tlv_next(&reader, &tlv)) {
            if (tlv.type == WINXTERM_JOB_TLV_STATUS) {
                ok = !saw_status && winxterm_job_tlv_read_u32(&tlv, &status); saw_status = true;
            } else if (tlv.type == WINXTERM_JOB_TLV_SNAPSHOT_MAX_ID) {
                ok = winxterm_job_tlv_read_u64(&tlv, &snapshot_max_id);
            } else if (tlv.type == WINXTERM_JOB_TLV_JOB_ID) {
                void *grown = realloc(items, (count + 1u) * sizeof(*items));
                if (grown == 0) { ok = false; break; }
                items = (WinxtermDstcmdJobInfo *)grown;
                memset(items + count, 0, sizeof(*items));
                ok = winxterm_job_tlv_read_u64(&tlv, &items[count].id);
                if (ok) ++count;
                after_cursor = false;
            } else if (tlv.type == WINXTERM_JOB_TLV_CURSOR) {
                ok = winxterm_job_tlv_read_u64(&tlv, &cursor); after_cursor = true;
            } else if (tlv.type == WINXTERM_JOB_TLV_FLAGS && after_cursor) {
                ok = winxterm_job_tlv_read_u32(&tlv, &page_flags);
            } else if (count == 0u) {
                ok = false;
            } else if (tlv.type == WINXTERM_JOB_TLV_OWNER_ID) {
                ok = winxterm_job_tlv_read_u64(&tlv, &items[count - 1u].owner_id);
            } else if (tlv.type == WINXTERM_JOB_TLV_STATE) {
                ok = winxterm_job_tlv_read_u32(&tlv, &items[count - 1u].state);
            } else if (tlv.type == WINXTERM_JOB_TLV_PROCESS_ID) {
                ok = winxterm_job_tlv_read_u32(&tlv, &items[count - 1u].process_id);
            } else if (tlv.type == WINXTERM_JOB_TLV_EXIT_CODE) {
                ok = winxterm_job_tlv_read_u32(&tlv, &items[count - 1u].exit_code);
            } else if (tlv.type == WINXTERM_JOB_TLV_BUFFERED_OUTPUT) {
                ok = winxterm_job_tlv_read_u64(&tlv, &items[count - 1u].buffered_output);
            } else if (tlv.type == WINXTERM_JOB_TLV_DISPLAY_NAME) {
                ok = winxterm_dstcmd_job_utf8(&tlv, items[count - 1u].display_name,
                                              WINXTERM_DSTCMD_JOB_DISPLAY_NAME_CAPACITY);
            } else if (tlv.type == WINXTERM_JOB_TLV_FLAGS) {
                uint32_t flags = 0u;
                ok = winxterm_job_tlv_read_u32(&tlv, &flags);
                items[count - 1u].foreground = (flags & WINXTERM_JOB_FLAG_FOREGROUND) != 0u;
                items[count - 1u].requester = (flags & WINXTERM_JOB_FLAG_SELF) != 0u;
                items[count - 1u].input_connectable = (flags & WINXTERM_JOB_FLAG_CONNECTABLE) != 0u;
                items[count - 1u].backpressured = (flags & WINXTERM_JOB_FLAG_BACKPRESSURED) != 0u;
                items[count - 1u].has_exit_code = (flags & WINXTERM_JOB_FLAG_HAS_EXIT_CODE) != 0u;
            } else ok = false;
        }
        ok = ok && reader.offset == reader.length;
        winxterm_job_frame_dispose(&reply);
        if (!ok || !saw_status || status != ERROR_SUCCESS) { free(items); return false; }
        more = (page_flags & WINXTERM_JOB_FLAG_MORE) != 0u;
    } while (more);
    *jobs = items;
    *job_count = count;
    return true;
}

bool winxterm_dstcmd_job_client_simple(WinxtermDstcmdJobClient *client,
                                      uint16_t message_type, uint64_t target_id,
                                      uint32_t flags, uint32_t *status)
{
    uint8_t payload[16];
    size_t length = 0u;
    if (status != 0) *status = ERROR_INVALID_DATA;
    if (message_type != WINXTERM_JOB_MESSAGE_CLEAN &&
        !winxterm_job_tlv_append_u64(payload, sizeof(payload), &length,
                                    WINXTERM_JOB_TLV_JOB_ID, target_id)) return false;
    WinxtermJobFrame reply;
    if (!winxterm_dstcmd_job_request(client, message_type, flags, payload,
                                     (uint32_t)length,
                                     WINXTERM_DSTCMD_JOB_REQUEST_TIMEOUT_MS, &reply)) return false;
    uint32_t local_status = ERROR_INVALID_DATA;
    bool ok = winxterm_dstcmd_job_reply_status(&reply, &local_status, 0);
    winxterm_job_frame_dispose(&reply);
    if (status != 0) *status = local_status;
    return ok;
}

bool winxterm_dstcmd_job_client_spawn(WinxtermDstcmdJobClient *client,
                                     const WinxtermJobExecutionPlan *plan,
                                     uint64_t *job_id, uint32_t *exit_code,
                                     bool *has_exit_code, uint32_t *status)
{
    if (job_id != 0) *job_id = 0u;
    if (exit_code != 0) *exit_code = 0u;
    if (has_exit_code != 0) *has_exit_code = false;
    if (status != 0) *status = ERROR_INVALID_DATA;
    if (!winxterm_dstcmd_job_client_available(client) || job_id == 0 || status == 0 ||
        (client->capabilities & WINXTERM_JOB_CAPABILITY_SPAWN) == 0u) return false;
    uint8_t *payload = 0;
    uint32_t payload_length = 0u;
    if (!winxterm_job_plan_encode(plan, &payload, &payload_length)) return false;
    WinxtermJobFrame reply;
    DWORD timeout = (plan->flags & WINXTERM_JOB_PLAN_FLAG_BACKGROUND) != 0u ?
        WINXTERM_DSTCMD_JOB_REQUEST_TIMEOUT_MS : INFINITE;
    bool requested = winxterm_dstcmd_job_request(client, WINXTERM_JOB_MESSAGE_SPAWN, 0u,
                                                 payload, payload_length, timeout, &reply);
    free(payload);
    if (!requested) return false;
    bool ok = false, saw_status = false, saw_id = false, saw_exit = false;
    WinxtermJobTlvReader reader;
    WinxtermJobTlv tlv;
    winxterm_job_tlv_reader_init(&reader, reply.payload, reply.header.payload_length);
    while (reader.offset < reader.length && winxterm_job_tlv_next(&reader, &tlv)) {
        if (tlv.type == WINXTERM_JOB_TLV_STATUS && !saw_status) {
            saw_status = winxterm_job_tlv_read_u32(&tlv, status);
            if (!saw_status) break;
        } else if (tlv.type == WINXTERM_JOB_TLV_JOB_ID && !saw_id) {
            saw_id = winxterm_job_tlv_read_u64(&tlv, job_id);
            if (!saw_id) break;
        } else if (tlv.type == WINXTERM_JOB_TLV_EXIT_CODE && !saw_exit && exit_code != 0) {
            saw_exit = winxterm_job_tlv_read_u32(&tlv, exit_code);
            if (!saw_exit) break;
        } else {
            saw_status = false;
            break;
        }
    }
    ok = saw_status && (*status != ERROR_SUCCESS || (saw_id && *job_id != 0u));
    if (has_exit_code != 0) *has_exit_code = saw_exit;
    winxterm_job_frame_dispose(&reply);
    return ok;
}

bool winxterm_dstcmd_job_client_view(WinxtermDstcmdJobClient *client, uint64_t job_id,
                                    uint8_t **bytes, size_t *byte_count,
                                    uint32_t *status)
{
    if (bytes != 0) *bytes = 0;
    if (byte_count != 0) *byte_count = 0u;
    if (status != 0) *status = ERROR_INVALID_DATA;
    if (!winxterm_dstcmd_job_client_available(client) || job_id == 0u || bytes == 0 ||
        byte_count == 0 || status == 0 ||
        (client->capabilities & WINXTERM_JOB_CAPABILITY_OUTPUT_JOURNAL) == 0u) return false;
    uint8_t *all = 0;
    size_t all_count = 0u;
    uint64_t cursor = 0u;
    uint64_t snapshot_offset = 0u;
    bool more;
    do {
        uint8_t request_payload[48];
        size_t request_length = 0u;
        if (!winxterm_job_tlv_append_u64(request_payload, sizeof(request_payload), &request_length,
                                        WINXTERM_JOB_TLV_JOB_ID, job_id) ||
            !winxterm_job_tlv_append_u64(request_payload, sizeof(request_payload), &request_length,
                                        WINXTERM_JOB_TLV_CURSOR, cursor) ||
            !winxterm_job_tlv_append_u64(request_payload, sizeof(request_payload), &request_length,
                                        WINXTERM_JOB_TLV_SNAPSHOT_OFFSET, snapshot_offset)) {
            free(all); return false;
        }
        WinxtermJobFrame reply;
        if (!winxterm_dstcmd_job_request(client, WINXTERM_JOB_MESSAGE_VIEW, 0u, request_payload,
                                         (uint32_t)request_length,
                                         WINXTERM_DSTCMD_JOB_REQUEST_TIMEOUT_MS, &reply)) {
            free(all); return false;
        }
        const uint8_t *page = 0;
        size_t page_length = 0u;
        uint64_t next_cursor = cursor;
        uint64_t reply_snapshot_offset = snapshot_offset;
        uint32_t flags = 0u;
        bool saw_status = false, saw_output = false, saw_cursor = false, saw_snapshot = false,
             saw_flags = false, ok = true;
        WinxtermJobTlvReader reader;
        WinxtermJobTlv tlv;
        winxterm_job_tlv_reader_init(&reader, reply.payload, reply.header.payload_length);
        while (ok && reader.offset < reader.length && winxterm_job_tlv_next(&reader, &tlv)) {
            if (tlv.type == WINXTERM_JOB_TLV_STATUS && !saw_status) {
                ok = winxterm_job_tlv_read_u32(&tlv, status); saw_status = ok;
            } else if (tlv.type == WINXTERM_JOB_TLV_OUTPUT && !saw_output) {
                page = tlv.value; page_length = tlv.length; saw_output = true;
            } else if (tlv.type == WINXTERM_JOB_TLV_CURSOR && !saw_cursor) {
                ok = winxterm_job_tlv_read_u64(&tlv, &next_cursor); saw_cursor = ok;
            } else if (tlv.type == WINXTERM_JOB_TLV_SNAPSHOT_OFFSET && !saw_snapshot) {
                ok = winxterm_job_tlv_read_u64(&tlv, &reply_snapshot_offset); saw_snapshot = ok;
            } else if (tlv.type == WINXTERM_JOB_TLV_FLAGS && !saw_flags) {
                ok = winxterm_job_tlv_read_u32(&tlv, &flags); saw_flags = ok;
            } else ok = false;
        }
        ok = ok && reader.offset == reader.length;
        if (!ok || !saw_status || *status != ERROR_SUCCESS || !saw_output || !saw_cursor || !saw_snapshot ||
            !saw_flags || next_cursor < cursor ||
            page_length > WINXTERM_JOB_OUTPUT_LIMIT - all_count) {
            winxterm_job_frame_dispose(&reply);
            free(all);
            return saw_status;
        }
        if (page_length != 0u) {
            void *grown = realloc(all, all_count + page_length);
            if (grown == 0) { winxterm_job_frame_dispose(&reply); free(all); return false; }
            all = (uint8_t *)grown;
            memcpy(all + all_count, page, page_length);
            all_count += page_length;
        }
        more = (flags & WINXTERM_JOB_FLAG_MORE) != 0u;
        bool advanced = next_cursor > cursor;
        cursor = next_cursor;
        snapshot_offset = reply_snapshot_offset;
        winxterm_job_frame_dispose(&reply);
        if (more && !advanced) { free(all); return false; }
    } while (more);
    *bytes = all;
    *byte_count = all_count;
    return true;
}

bool winxterm_dstcmd_job_client_connect(WinxtermDstcmdJobClient *client,
                                       uint64_t source_id, uint64_t destination_id,
                                       uint32_t *status)
{
    if (status != 0) *status = ERROR_INVALID_DATA;
    if (!winxterm_dstcmd_job_client_available(client) || source_id == 0u ||
        destination_id == 0u || status == 0 ||
        (client->capabilities & WINXTERM_JOB_CAPABILITY_CONNECTIONS) == 0u) return false;
    uint8_t payload[32];
    size_t length = 0u;
    if (!winxterm_job_tlv_append_u64(payload, sizeof(payload), &length,
                                    WINXTERM_JOB_TLV_SOURCE_ID, source_id) ||
        !winxterm_job_tlv_append_u64(payload, sizeof(payload), &length,
                                    WINXTERM_JOB_TLV_DESTINATION_ID, destination_id)) return false;
    WinxtermJobFrame reply;
    if (!winxterm_dstcmd_job_request(client, WINXTERM_JOB_MESSAGE_CONNECT, 0u, payload,
                                     (uint32_t)length,
                                     WINXTERM_DSTCMD_JOB_REQUEST_TIMEOUT_MS, &reply)) return false;
    bool ok = winxterm_dstcmd_job_reply_status(&reply, status, 0);
    winxterm_job_frame_dispose(&reply);
    return ok;
}

bool winxterm_dstcmd_job_client_disconnect(WinxtermDstcmdJobClient *client,
                                          uint64_t source_id, uint32_t *status)
{
    if (status != 0) *status = ERROR_INVALID_DATA;
    if (!winxterm_dstcmd_job_client_available(client) || source_id == 0u || status == 0 ||
        (client->capabilities & WINXTERM_JOB_CAPABILITY_DISCONNECT) == 0u) return false;
    uint8_t payload[16];
    size_t length = 0u;
    if (!winxterm_job_tlv_append_u64(payload, sizeof(payload), &length,
                                    WINXTERM_JOB_TLV_SOURCE_ID, source_id)) return false;
    WinxtermJobFrame reply;
    if (!winxterm_dstcmd_job_request(client, WINXTERM_JOB_MESSAGE_DISCONNECT, 0u, payload,
                                     (uint32_t)length,
                                     WINXTERM_DSTCMD_JOB_REQUEST_TIMEOUT_MS, &reply)) return false;
    bool ok = winxterm_dstcmd_job_reply_status(&reply, status, 0);
    winxterm_job_frame_dispose(&reply);
    return ok;
}

bool winxterm_dstcmd_job_client_clean(WinxtermDstcmdJobClient *client,
                                     uint32_t *removed_count, uint32_t *status)
{
    if (removed_count != 0) *removed_count = 0u;
    if (status != 0) *status = ERROR_INVALID_DATA;
    if (!winxterm_dstcmd_job_client_available(client) || removed_count == 0 || status == 0) {
        return false;
    }
    WinxtermJobFrame reply;
    if (!winxterm_dstcmd_job_request(client, WINXTERM_JOB_MESSAGE_CLEAN, 0u, 0, 0u,
                                     WINXTERM_DSTCMD_JOB_REQUEST_TIMEOUT_MS, &reply)) {
        return false;
    }
    bool saw_status = false, saw_count = false, ok = true;
    WinxtermJobTlvReader reader;
    WinxtermJobTlv tlv;
    winxterm_job_tlv_reader_init(&reader, reply.payload, reply.header.payload_length);
    while (ok && reader.offset < reader.length && winxterm_job_tlv_next(&reader, &tlv)) {
        if (tlv.type == WINXTERM_JOB_TLV_STATUS && !saw_status) {
            ok = winxterm_job_tlv_read_u32(&tlv, status);
            saw_status = ok;
        } else if (tlv.type == WINXTERM_JOB_TLV_COUNT && !saw_count) {
            ok = winxterm_job_tlv_read_u32(&tlv, removed_count);
            saw_count = ok;
        } else ok = false;
    }
    winxterm_job_frame_dispose(&reply);
    return ok && saw_status && saw_count;
}

bool winxterm_dstcmd_job_client_attach(WinxtermDstcmdJobClient *client, uint64_t job_id,
                                      const wchar_t *path, bool append, bool tee,
                                      uint32_t *status)
{
    if (status != 0) *status = ERROR_INVALID_DATA;
    if (!winxterm_dstcmd_job_client_available(client) || job_id == 0u || path == 0 ||
        path[0] == L'\0' || status == 0 ||
        (client->capabilities & WINXTERM_JOB_CAPABILITY_ATTACHMENTS) == 0u) return false;
    int path_bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
                                         0, 0, 0, 0);
    char *utf8 = path_bytes > 1 ? (char *)malloc((size_t)path_bytes) : 0;
    if (utf8 == 0 || WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, path, -1,
                                         utf8, path_bytes, 0, 0) != path_bytes) {
        free(utf8);
        return false;
    }
    size_t capacity = (size_t)path_bytes + 64u;
    uint8_t *payload = (uint8_t *)malloc(capacity);
    size_t length = 0u;
    uint32_t flags = (append ? WINXTERM_JOB_FLAG_APPEND : 0u) |
                     (tee ? WINXTERM_JOB_FLAG_TEE : 0u);
    bool encoded = payload != 0 &&
        winxterm_job_tlv_append_u64(payload, capacity, &length,
                                    WINXTERM_JOB_TLV_JOB_ID, job_id) &&
        winxterm_job_tlv_append_u32(payload, capacity, &length,
                                    WINXTERM_JOB_TLV_FLAGS, flags) &&
        winxterm_job_tlv_append(payload, capacity, &length,
                                WINXTERM_JOB_TLV_PATH, utf8,
                                (uint32_t)(path_bytes - 1));
    free(utf8);
    WinxtermJobFrame reply;
    bool requested = encoded && winxterm_dstcmd_job_request(
        client, WINXTERM_JOB_MESSAGE_ATTACH_FILE, 0u, payload, (uint32_t)length,
        WINXTERM_DSTCMD_JOB_REQUEST_TIMEOUT_MS, &reply);
    free(payload);
    if (!requested) return false;
    bool ok = winxterm_dstcmd_job_reply_status(&reply, status, 0);
    winxterm_job_frame_dispose(&reply);
    return ok;
}

bool winxterm_dstcmd_job_client_kill(WinxtermDstcmdJobClient *client, uint64_t job_id,
                                    HANDLE input_handle, bool *cancelled,
                                    DWORD *elapsed_ms, uint32_t *status)
{
    if (cancelled != 0) *cancelled = false;
    if (elapsed_ms != 0) *elapsed_ms = 0u;
    if (status != 0) *status = ERROR_INVALID_DATA;
    if (!winxterm_dstcmd_job_client_available(client) || job_id == 0u || status == 0) return false;
    uint8_t payload[16];
    size_t payload_length = 0u;
    if (!winxterm_job_tlv_append_u64(payload, sizeof(payload), &payload_length,
                                     WINXTERM_JOB_TLV_JOB_ID, job_id)) return false;
    WinxtermDstcmdJobPending pending;
    memset(&pending, 0, sizeof(pending));
    pending.completed = CreateEventW(0, TRUE, FALSE, 0);
    if (pending.completed == 0) return false;
    EnterCriticalSection(&client->lock);
    pending.request_id = client->next_request_id++;
    if (pending.request_id == 0u) pending.request_id = client->next_request_id++;
    pending.next = client->pending;
    client->pending = &pending;
    LeaveCriticalSection(&client->lock);
    WinxtermJobFrameHeader header = {WINXTERM_JOB_PROTOCOL_VERSION,
                                    WINXTERM_JOB_MESSAGE_SIGNAL, 0u,
                                    pending.request_id, (uint32_t)payload_length};
    EnterCriticalSection(&client->write_lock);
    bool wrote = client->available &&
        winxterm_job_channel_write(client->request_handle, &header, payload);
    LeaveCriticalSection(&client->write_lock);
    DWORD started = GetTickCount();
    bool cancel_sent = false;
    DWORD wait = WAIT_FAILED;
    while (wrote) {
        HANDLE waits[2] = {pending.completed, input_handle};
        DWORD count = input_handle != 0 && input_handle != INVALID_HANDLE_VALUE ? 2u : 1u;
        wait = WaitForMultipleObjects(count, waits, FALSE,
                                      WINXTERM_DSTCMD_JOB_REQUEST_TIMEOUT_MS);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_OBJECT_0 + 1u || cancel_sent) break;
        uint8_t input[64];
        DWORD available = 1u, read = 0u;
        if (GetFileType(input_handle) == FILE_TYPE_PIPE) {
            if (!PeekNamedPipe(input_handle, 0, 0, 0, &available, 0) || available == 0u) continue;
            if (available > sizeof(input)) available = sizeof(input);
        }
        if (!ReadFile(input_handle, input, available, &read, 0)) continue;
        for (DWORD i = 0u; i < read; ++i) {
            if (input[i] != 0x03u) continue;
            uint8_t cancel_payload[16];
            size_t cancel_length = 0u;
            WinxtermJobFrame cancel_reply;
            uint32_t cancel_status = ERROR_INVALID_DATA;
            if (winxterm_job_tlv_append_u64(cancel_payload, sizeof(cancel_payload),
                                            &cancel_length, WINXTERM_JOB_TLV_REQUEST_ID,
                                            pending.request_id) &&
                winxterm_dstcmd_job_request(client, WINXTERM_JOB_MESSAGE_CANCEL, 0u,
                                            cancel_payload, (uint32_t)cancel_length,
                                            WINXTERM_DSTCMD_JOB_REQUEST_TIMEOUT_MS,
                                            &cancel_reply)) {
                (void)winxterm_dstcmd_job_reply_status(&cancel_reply, &cancel_status, 0);
                winxterm_job_frame_dispose(&cancel_reply);
            }
            cancel_sent = cancel_status == ERROR_SUCCESS;
            break;
        }
    }
    EnterCriticalSection(&client->lock);
    winxterm_dstcmd_job_remove_pending(client, &pending);
    bool ok = wait == WAIT_OBJECT_0 && pending.has_reply &&
              winxterm_dstcmd_job_reply_status(&pending.reply, status, 0);
    LeaveCriticalSection(&client->lock);
    if (elapsed_ms != 0) *elapsed_ms = GetTickCount() - started;
    if (cancelled != 0) *cancelled = cancel_sent && *status == ERROR_CANCELLED;
    winxterm_job_frame_dispose(&pending.reply);
    CloseHandle(pending.completed);
    return ok;
}
