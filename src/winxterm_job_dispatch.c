#include "winxterm_host_dispatch.h"
#include "winxterm_job_channel.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static bool winxterm_job_dispatch_append_utf8(uint8_t *payload, size_t capacity,
                                              size_t *length, uint16_t type,
                                              const wchar_t *text)
{
    if (text == 0 || text[0] == L'\0') return true;
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                    text, -1, 0, 0, 0, 0);
    if (bytes <= 1) return bytes == 1;
    char *utf8 = (char *)malloc((size_t)bytes);
    if (utf8 == 0) return false;
    bool ok = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                                  utf8, bytes, 0, 0) == bytes &&
              winxterm_job_tlv_append(payload, capacity, length, type,
                                      utf8, (uint32_t)(bytes - 1));
    free(utf8);
    return ok;
}

DWORD WINAPI winxterm_job_dispatch_thread(void *context)
{
    WinxtermHostClient *client = (WinxtermHostClient *)context;
    WinxtermHostContext *host = client != 0 ? (WinxtermHostContext *)client->context : 0;
    while (host != 0 && client->request_read != 0 && client->reply_write != 0) {
        WinxtermJobFrame request;
        if (!winxterm_job_channel_read(client->request_read, &request)) break;
        uint8_t *payload = (uint8_t *)malloc(WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD);
        if (payload == 0) { winxterm_job_frame_dispose(&request); break; }
        size_t payload_length = 0u;
        bool capabilities_request = request.header.type == WINXTERM_JOB_MESSAGE_CAPABILITIES;
        bool list_request = request.header.type == WINXTERM_JOB_MESSAGE_LIST;
        bool lifecycle_request = request.header.type == WINXTERM_JOB_MESSAGE_FOREGROUND ||
                                 request.header.type == WINXTERM_JOB_MESSAGE_BACKGROUND ||
                                 request.header.type == WINXTERM_JOB_MESSAGE_REMOVE ||
                                 request.header.type == WINXTERM_JOB_MESSAGE_CLEAN;
        bool spawn_request = request.header.type == WINXTERM_JOB_MESSAGE_SPAWN;
        bool view_request = request.header.type == WINXTERM_JOB_MESSAGE_VIEW;
        bool signal_request = request.header.type == WINXTERM_JOB_MESSAGE_SIGNAL;
        bool connect_request = request.header.type == WINXTERM_JOB_MESSAGE_CONNECT;
        bool disconnect_request = request.header.type == WINXTERM_JOB_MESSAGE_DISCONNECT;
        bool attach_request = request.header.type == WINXTERM_JOB_MESSAGE_ATTACH_FILE;
        bool detach_request = request.header.type == WINXTERM_JOB_MESSAGE_DETACH;
        bool cancel_request = request.header.type == WINXTERM_JOB_MESSAGE_CANCEL;
        bool interactive_request = request.header.type == WINXTERM_JOB_MESSAGE_INTERACTIVE;
        bool invalid_envelope = request.header.request_id == 0u ||
            (signal_request ? (request.header.flags & ~WINXTERM_JOB_SIGNAL_FORCE) != 0u :
                              request.header.flags != 0u);
        uint64_t response_job_id = 0u;
        uint32_t response_exit_code = 0u;
        bool response_has_exit_code = false;
        bool defer_reply = false;
        uint32_t response_count = 0u;
        uint8_t *view_bytes = 0;
        size_t view_length = 0u;
        uint64_t view_cursor = 0u;
        uint64_t view_snapshot_offset = 0u;
        bool view_more = false;
        uint32_t status = capabilities_request || list_request ? ERROR_SUCCESS : ERROR_NOT_SUPPORTED;
        if (invalid_envelope) status = ERROR_INVALID_DATA;
        if (interactive_request && !invalid_envelope) {
            status = request.header.payload_length != 0u ? ERROR_INVALID_DATA :
                winxterm_host_dispatch_request_ui(host) ? ERROR_SUCCESS :
                                                          ERROR_INVALID_WINDOW_HANDLE;
        }
        if (spawn_request && !invalid_envelope) {
            bool foreground_spawn = false;
            status = winxterm_host_spawn_plan(host, request.payload,
                                              request.header.payload_length,
                                              client->requester_id,
                                              &response_job_id, &foreground_spawn);
            if (status == ERROR_SUCCESS && foreground_spawn) {
                if (!winxterm_host_defer_foreground_request(
                        host, client, response_job_id, request.header.request_id)) {
                    status = ERROR_GEN_FAILURE;
                } else {
                    defer_reply = true;
                }
            }
        }
        if (view_request && !invalid_envelope) {
            uint64_t target_id = 0u, cursor = 0u, snapshot_offset = 0u;
            bool found_target = false, found_cursor = false, found_snapshot = false, valid = true;
            WinxtermJobTlvReader view_reader;
            WinxtermJobTlv view_tlv;
            winxterm_job_tlv_reader_init(&view_reader, request.payload, request.header.payload_length);
            while (valid && view_reader.offset < view_reader.length &&
                   winxterm_job_tlv_next(&view_reader, &view_tlv)) {
                if (view_tlv.type == WINXTERM_JOB_TLV_JOB_ID && !found_target) {
                    valid = winxterm_job_tlv_read_u64(&view_tlv, &target_id);
                    found_target = valid;
                } else if (view_tlv.type == WINXTERM_JOB_TLV_CURSOR && !found_cursor) {
                    valid = winxterm_job_tlv_read_u64(&view_tlv, &cursor);
                    found_cursor = valid;
                } else if (view_tlv.type == WINXTERM_JOB_TLV_SNAPSHOT_OFFSET && !found_snapshot) {
                    valid = winxterm_job_tlv_read_u64(&view_tlv, &snapshot_offset);
                    found_snapshot = valid;
                } else valid = false;
            }
            valid = valid && view_reader.offset == view_reader.length && found_target &&
                    found_cursor && found_snapshot && target_id != 0u;
            view_bytes = valid ? (uint8_t *)malloc(WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD / 2u) : 0;
            status = !valid ? ERROR_INVALID_DATA : view_bytes == 0 ? ERROR_NOT_ENOUGH_MEMORY :
                winxterm_host_view_job(host, client->requester_id, target_id, cursor,
                                      &snapshot_offset, view_bytes,
                                      WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD / 2u,
                                      &view_length, &view_cursor, &view_more);
            view_snapshot_offset = snapshot_offset;
        }
        if (signal_request && !invalid_envelope) {
            uint64_t target_id = 0u;
            WinxtermJobTlvReader signal_reader;
            WinxtermJobTlv signal_tlv;
            winxterm_job_tlv_reader_init(&signal_reader, request.payload,
                                         request.header.payload_length);
            bool valid = request.header.flags == 0u ||
                         request.header.flags == WINXTERM_JOB_SIGNAL_FORCE;
            valid = valid && winxterm_job_tlv_next(&signal_reader, &signal_tlv) &&
                    signal_tlv.type == WINXTERM_JOB_TLV_JOB_ID &&
                    winxterm_job_tlv_read_u64(&signal_tlv, &target_id) && target_id != 0u &&
                    signal_reader.offset == signal_reader.length;
            if (!valid) {
                status = ERROR_INVALID_DATA;
            } else if (request.header.flags == WINXTERM_JOB_SIGNAL_FORCE) {
                status = winxterm_host_signal_job(host, client->requester_id,
                                                  target_id, true);
            } else {
                status = winxterm_host_start_kill(client, target_id, request.header.request_id);
                defer_reply = status == ERROR_SUCCESS;
            }
        }
        if (connect_request && !invalid_envelope) {
            uint64_t source_id = 0u, destination_id = 0u;
            bool found_source = false, found_destination = false, valid = true;
            WinxtermJobTlvReader connect_reader;
            WinxtermJobTlv connect_tlv;
            winxterm_job_tlv_reader_init(&connect_reader, request.payload,
                                         request.header.payload_length);
            while (valid && connect_reader.offset < connect_reader.length &&
                   winxterm_job_tlv_next(&connect_reader, &connect_tlv)) {
                if (connect_tlv.type == WINXTERM_JOB_TLV_SOURCE_ID && !found_source) {
                    valid = winxterm_job_tlv_read_u64(&connect_tlv, &source_id);
                    found_source = valid;
                } else if (connect_tlv.type == WINXTERM_JOB_TLV_DESTINATION_ID && !found_destination) {
                    valid = winxterm_job_tlv_read_u64(&connect_tlv, &destination_id);
                    found_destination = valid;
                } else valid = false;
            }
            valid = valid && connect_reader.offset == connect_reader.length && found_source &&
                    found_destination && source_id != 0u &&
                    destination_id != 0u && source_id != destination_id;
            status = valid ? winxterm_host_connect_jobs(host, client->requester_id,
                                                        source_id, destination_id) :
                             ERROR_INVALID_DATA;
        }
        if (disconnect_request && !invalid_envelope) {
            uint64_t source_id = 0u;
            WinxtermJobTlvReader disconnect_reader;
            WinxtermJobTlv disconnect_tlv;
            winxterm_job_tlv_reader_init(&disconnect_reader, request.payload,
                                         request.header.payload_length);
            bool valid = winxterm_job_tlv_next(&disconnect_reader, &disconnect_tlv) &&
                disconnect_tlv.type == WINXTERM_JOB_TLV_SOURCE_ID &&
                winxterm_job_tlv_read_u64(&disconnect_tlv, &source_id) && source_id != 0u &&
                disconnect_reader.offset == disconnect_reader.length;
            status = valid ? winxterm_host_disconnect_job(host, client->requester_id,
                                                          source_id) : ERROR_INVALID_DATA;
        }
        if (attach_request && !invalid_envelope) {
            uint64_t target_id = 0u;
            uint32_t attach_flags = 0u;
            const char *path = 0;
            size_t path_length = 0u;
            bool found_target = false, found_flags = false, found_path = false, valid = true;
            WinxtermJobTlvReader attach_reader;
            WinxtermJobTlv attach_tlv;
            winxterm_job_tlv_reader_init(&attach_reader, request.payload,
                                         request.header.payload_length);
            while (valid && attach_reader.offset < attach_reader.length &&
                   winxterm_job_tlv_next(&attach_reader, &attach_tlv)) {
                if (attach_tlv.type == WINXTERM_JOB_TLV_JOB_ID && !found_target) {
                    valid = winxterm_job_tlv_read_u64(&attach_tlv, &target_id);
                    found_target = valid;
                } else if (attach_tlv.type == WINXTERM_JOB_TLV_FLAGS && !found_flags) {
                    valid = winxterm_job_tlv_read_u32(&attach_tlv, &attach_flags);
                    found_flags = valid;
                } else if (attach_tlv.type == WINXTERM_JOB_TLV_PATH && !found_path) {
                    path = (const char *)attach_tlv.value;
                    path_length = attach_tlv.length;
                    found_path = true;
                } else valid = false;
            }
            valid = valid && attach_reader.offset == attach_reader.length && found_target &&
                    found_flags && found_path && target_id != 0u;
            status = valid ? winxterm_host_attach_file(host, target_id, client->requester_id,
                                                       path, path_length,
                                                       attach_flags) : ERROR_INVALID_DATA;
        }
        if (detach_request && !invalid_envelope) {
            uint64_t target_id = 0u;
            WinxtermJobTlvReader detach_reader;
            WinxtermJobTlv detach_tlv;
            winxterm_job_tlv_reader_init(&detach_reader, request.payload,
                                         request.header.payload_length);
            bool valid = winxterm_job_tlv_next(&detach_reader, &detach_tlv) &&
                detach_tlv.type == WINXTERM_JOB_TLV_JOB_ID &&
                winxterm_job_tlv_read_u64(&detach_tlv, &target_id) && target_id != 0u &&
                detach_reader.offset == detach_reader.length;
            status = valid ? winxterm_host_detach_file(host, client->requester_id,
                                                       target_id) : ERROR_INVALID_DATA;
        }
        if (cancel_request && !invalid_envelope) {
            uint64_t cancel_id = 0u;
            WinxtermJobTlvReader cancel_reader;
            WinxtermJobTlv cancel_tlv;
            winxterm_job_tlv_reader_init(&cancel_reader, request.payload,
                                         request.header.payload_length);
            bool valid = winxterm_job_tlv_next(&cancel_reader, &cancel_tlv) &&
                cancel_tlv.type == WINXTERM_JOB_TLV_REQUEST_ID &&
                winxterm_job_tlv_read_u64(&cancel_tlv, &cancel_id) && cancel_id != 0u &&
                cancel_reader.offset == cancel_reader.length;
            status = valid ? winxterm_host_cancel_request(client, cancel_id) : ERROR_INVALID_DATA;
        }
        if (lifecycle_request && !invalid_envelope) {
            uint64_t target_id = 0u;
            bool valid = true;
            WinxtermJobTlvReader operation_reader;
            WinxtermJobTlv operation_tlv;
            winxterm_job_tlv_reader_init(&operation_reader, request.payload,
                                         request.header.payload_length);
            if (request.header.type == WINXTERM_JOB_MESSAGE_CLEAN) {
                valid = request.header.payload_length == 0u;
            } else {
                bool found_target = false;
                while (valid && operation_reader.offset < operation_reader.length &&
                       winxterm_job_tlv_next(&operation_reader, &operation_tlv)) {
                    if (operation_tlv.type != WINXTERM_JOB_TLV_JOB_ID || found_target) {
                        valid = false;
                    } else {
                        valid = winxterm_job_tlv_read_u64(&operation_tlv, &target_id) &&
                                target_id != 0u;
                        found_target = valid;
                    }
                }
                valid = valid && operation_reader.offset == operation_reader.length && found_target;
            }
            if (!valid) {
                status = ERROR_INVALID_DATA;
            } else {
                status = winxterm_host_lifecycle_request(
                    host, client, request.header.type, target_id, &response_count);
            }
        }
        bool ok = winxterm_job_tlv_append_u32(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD, &payload_length,
                                              WINXTERM_JOB_TLV_STATUS, status);
        if (ok && capabilities_request && !invalid_envelope) {
            uint32_t capabilities = WINXTERM_JOB_CAPABILITY_LIST |
                                    WINXTERM_JOB_CAPABILITY_LIFECYCLE;
            capabilities |= WINXTERM_JOB_CAPABILITY_SPAWN |
                            WINXTERM_JOB_CAPABILITY_OUTPUT_JOURNAL |
                            WINXTERM_JOB_CAPABILITY_EVENTS |
                            WINXTERM_JOB_CAPABILITY_MULTI_SESSION |
                            WINXTERM_JOB_CAPABILITY_CONNECTIONS |
                            WINXTERM_JOB_CAPABILITY_DISCONNECT |
                            WINXTERM_JOB_CAPABILITY_PIPELINES |
                            WINXTERM_JOB_CAPABILITY_FILE_ENDPOINTS |
                            WINXTERM_JOB_CAPABILITY_ATTACHMENTS |
                            WINXTERM_JOB_CAPABILITY_INTERACTIVE_UI;
            ok = winxterm_job_tlv_append_u32(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD, &payload_length,
                                             WINXTERM_JOB_TLV_FLAGS, capabilities);
        } else if (ok && list_request && !invalid_envelope) {
            WinxtermJobManager *manager = winxterm_host_dispatch_manager(host);
            uint64_t cursor = 0u, snapshot_max_id = 0u;
            WinxtermJobTlvReader request_reader;
            WinxtermJobTlv request_tlv;
            winxterm_job_tlv_reader_init(&request_reader, request.payload,
                                         request.header.payload_length);
            while (ok && request_reader.offset < request_reader.length &&
                   winxterm_job_tlv_next(&request_reader, &request_tlv)) {
                if (request_tlv.type == WINXTERM_JOB_TLV_CURSOR) {
                    ok = winxterm_job_tlv_read_u64(&request_tlv, &cursor);
                } else if (request_tlv.type == WINXTERM_JOB_TLV_SNAPSHOT_MAX_ID) {
                    ok = winxterm_job_tlv_read_u64(&request_tlv, &snapshot_max_id);
                } else {
                    ok = false;
                }
            }
            ok = ok && request_reader.offset == request_reader.length;
            WinxtermManagedJobSnapshot *jobs = client->list_snapshot;
            size_t job_count = client->list_snapshot_count;
            if (ok && snapshot_max_id == 0u) {
                winxterm_job_manager_snapshot_dispose(client->list_snapshot);
                client->list_snapshot = 0;
                client->list_snapshot_count = 0u;
                client->list_snapshot_token = 0u;
                ok = winxterm_job_manager_snapshot(manager, client->requester_id,
                                                   &client->list_snapshot,
                                                   &client->list_snapshot_count);
                jobs = client->list_snapshot;
                job_count = client->list_snapshot_count;
                for (size_t i = 0u; i < job_count; ++i) {
                    if (jobs[i].id > snapshot_max_id) snapshot_max_id = jobs[i].id;
                }
                client->list_snapshot_token = snapshot_max_id;
            } else if (ok && (client->list_snapshot == 0 ||
                              snapshot_max_id != client->list_snapshot_token)) ok = false;
            ok = ok && winxterm_job_tlv_append_u64(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                                   &payload_length,
                                                   WINXTERM_JOB_TLV_SNAPSHOT_MAX_ID,
                                                   snapshot_max_id);
            uint64_t next_cursor = cursor;
            bool more = false;
            for (size_t i = 0u; ok && i < job_count; ++i) {
                WinxtermManagedJobSnapshot *job = jobs + i;
                if (job->id <= cursor || job->id > snapshot_max_id) continue;
                /* Reserve enough room for the continuation fields rather than
                   emitting a partial job record. */
                if (WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD - payload_length < 2048u) {
                    more = true;
                    break;
                }
                uint32_t flags = (job->foreground ? WINXTERM_JOB_FLAG_FOREGROUND : 0u) |
                                 (job->id == client->requester_id ? WINXTERM_JOB_FLAG_SELF : 0u) |
                                 (job->input_connectable ? WINXTERM_JOB_FLAG_CONNECTABLE : 0u) |
                                 (job->backpressured ? WINXTERM_JOB_FLAG_BACKPRESSURED : 0u) |
                                 (job->has_exit_code ? WINXTERM_JOB_FLAG_HAS_EXIT_CODE : 0u);
                ok = winxterm_job_tlv_append_u64(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                                 &payload_length, WINXTERM_JOB_TLV_JOB_ID, job->id) &&
                     winxterm_job_tlv_append_u64(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                                 &payload_length, WINXTERM_JOB_TLV_OWNER_ID, job->owner_id) &&
                     winxterm_job_tlv_append_u32(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                                 &payload_length, WINXTERM_JOB_TLV_STATE,
                                                 (uint32_t)job->state) &&
                     winxterm_job_tlv_append_u32(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                                 &payload_length, WINXTERM_JOB_TLV_PROCESS_ID,
                                                 job->process_id) &&
                     winxterm_job_tlv_append_u32(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                                 &payload_length, WINXTERM_JOB_TLV_FLAGS, flags) &&
                     winxterm_job_tlv_append_u32(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                                 &payload_length, WINXTERM_JOB_TLV_EXIT_CODE,
                                                 job->has_exit_code ? job->exit_code : 0u) &&
                     winxterm_job_tlv_append_u64(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                                 &payload_length, WINXTERM_JOB_TLV_BUFFERED_OUTPUT,
                                                 job->buffered_output) &&
                     winxterm_job_dispatch_append_utf8(
                         payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                         &payload_length, WINXTERM_JOB_TLV_DISPLAY_NAME,
                         job->display_name);
                if (ok) next_cursor = job->id;
            }
            if (ok) ok = winxterm_job_tlv_append_u64(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                                     &payload_length, WINXTERM_JOB_TLV_CURSOR,
                                                     next_cursor) &&
                         winxterm_job_tlv_append_u32(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                                     &payload_length, WINXTERM_JOB_TLV_FLAGS,
                                                     more ? WINXTERM_JOB_FLAG_MORE : 0u);
            if (ok && !more) {
                winxterm_job_manager_snapshot_dispose(client->list_snapshot);
                client->list_snapshot = 0;
                client->list_snapshot_count = 0u;
                client->list_snapshot_token = 0u;
            }
            if (!ok) {
                payload_length = 0u;
                ok = winxterm_job_tlv_append_u32(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                                 &payload_length, WINXTERM_JOB_TLV_STATUS,
                                                 ERROR_INVALID_DATA);
            }
        } else if (ok && spawn_request && status == ERROR_SUCCESS) {
            ok = winxterm_job_tlv_append_u64(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                             &payload_length, WINXTERM_JOB_TLV_JOB_ID,
                                             response_job_id);
            if (ok && response_has_exit_code) {
                ok = winxterm_job_tlv_append_u32(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                                 &payload_length, WINXTERM_JOB_TLV_EXIT_CODE,
                                                 response_exit_code);
            }
        } else if (ok && view_request && status == ERROR_SUCCESS) {
            ok = winxterm_job_tlv_append(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                         &payload_length, WINXTERM_JOB_TLV_OUTPUT,
                                         view_bytes, (uint32_t)view_length) &&
                 winxterm_job_tlv_append_u64(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                             &payload_length, WINXTERM_JOB_TLV_CURSOR,
                                             view_cursor) &&
                 winxterm_job_tlv_append_u64(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                             &payload_length, WINXTERM_JOB_TLV_SNAPSHOT_OFFSET,
                                             view_snapshot_offset) &&
                 winxterm_job_tlv_append_u32(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                             &payload_length, WINXTERM_JOB_TLV_FLAGS,
                                             view_more ? WINXTERM_JOB_FLAG_MORE : 0u);
        } else if (ok && request.header.type == WINXTERM_JOB_MESSAGE_CLEAN &&
                   status == ERROR_SUCCESS) {
            ok = winxterm_job_tlv_append_u32(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD,
                                             &payload_length, WINXTERM_JOB_TLV_COUNT,
                                             response_count);
        } else if (ok && !lifecycle_request && !spawn_request && !view_request &&
                   !signal_request && !connect_request && !disconnect_request &&
                   !attach_request && !detach_request && !cancel_request &&
                   !interactive_request && !invalid_envelope) {
            static const char error[] = "job-control operation is not implemented";
            ok = winxterm_job_tlv_append(payload, WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD, &payload_length,
                                         WINXTERM_JOB_TLV_ERROR, error,
                                         (uint32_t)(sizeof(error) - 1u));
        }
        WinxtermJobFrameHeader reply = {WINXTERM_JOB_PROTOCOL_VERSION,
                                       WINXTERM_JOB_MESSAGE_REPLY,
                                       0u,
                                       request.header.request_id,
                                       (uint32_t)payload_length};
        winxterm_job_frame_dispose(&request);
        bool written = ok;
        if (!defer_reply) {
            EnterCriticalSection(&client->reply_lock);
            written = ok && winxterm_job_channel_write(client->reply_write, &reply, payload);
            LeaveCriticalSection(&client->reply_lock);
        }
        free(view_bytes);
        free(payload);
        if (!written) break;
    }
    return 0u;
}
