#include "winxterm_job_coordinator.h"
#include "winxterm_job_channel.h"

#include <stdlib.h>
#include <string.h>

struct WinxtermJobCoordinatorCommand {
    uint32_t action;
    uint64_t job_id;
    WinxtermJobCoordinatorCommand *next;
};

struct WinxtermJobCoordinatorView {
    uint64_t job_id;
    uint8_t *bytes;
    size_t byte_count;
    WinxtermJobCoordinatorView *next;
};

struct WinxtermJobCoordinatorEvent {
    WinxtermJobFrameHeader header;
    uint8_t *payload;
    WinxtermJobCoordinatorEvent *next;
};

static DWORD WINAPI winxterm_job_coordinator_event_thread(void *context)
{
    WinxtermJobCoordinatorClient *client = (WinxtermJobCoordinatorClient *)context;
    for (;;) {
        if (WaitForSingleObject(client->event_ready, INFINITE) != WAIT_OBJECT_0) break;
        EnterCriticalSection(&client->event_lock);
        bool stopping = client->event_stopping;
        WinxtermJobCoordinatorEvent *event = stopping ? 0 : client->event_head;
        bool resync = !stopping && event == 0 && client->event_overflowed;
        if (event != 0) {
            client->event_head = event->next;
            if (client->event_head == 0) client->event_tail = 0;
            --client->event_count;
        } else if (resync) {
            client->event_overflowed = false;
        }
        if ((client->event_head == 0 && !client->event_overflowed) || stopping) {
            ResetEvent(client->event_ready);
        }
        LeaveCriticalSection(&client->event_lock);
        if (stopping) break;

        uint8_t resync_payload[96];
        size_t resync_length = 0u;
        WinxtermJobFrameHeader resync_header = {0};
        if (resync) {
            bool ok = winxterm_job_tlv_append_u32(
                          resync_payload, sizeof(resync_payload), &resync_length,
                          WINXTERM_JOB_TLV_EVENT_KIND,
                          WINXTERM_JOB_EVENT_RESYNC_REQUIRED) &&
                      winxterm_job_tlv_append_u64(
                          resync_payload, sizeof(resync_payload), &resync_length,
                          WINXTERM_JOB_TLV_JOB_ID, 0u) &&
                      winxterm_job_tlv_append_u32(
                          resync_payload, sizeof(resync_payload), &resync_length,
                          WINXTERM_JOB_TLV_STATE, 0u) &&
                      winxterm_job_tlv_append_u32(
                          resync_payload, sizeof(resync_payload), &resync_length,
                          WINXTERM_JOB_TLV_EXIT_CODE, 0u);
            if (!ok) break;
            resync_header = (WinxtermJobFrameHeader){
                WINXTERM_JOB_PROTOCOL_VERSION, WINXTERM_JOB_MESSAGE_EVENT,
                0u, 0u, (uint32_t)resync_length
            };
        }
        const WinxtermJobFrameHeader *header = event != 0 ?
            &event->header : &resync_header;
        const uint8_t *payload = event != 0 ? event->payload : resync_payload;
        EnterCriticalSection(&client->reply_lock);
        bool written = client->reply_write != 0 &&
            winxterm_job_channel_write(client->reply_write, header, payload);
        LeaveCriticalSection(&client->reply_lock);
        if (event != 0) {
            free(event->payload);
            free(event);
        }
        if (!written) {
            EnterCriticalSection(&client->event_lock);
            client->event_stopping = true;
            LeaveCriticalSection(&client->event_lock);
            break;
        }
    }
    return 0u;
}

static bool winxterm_job_coordinator_start_client(WinxtermJobCoordinatorClient *client)
{
    InitializeCriticalSection(&client->event_lock);
    client->event_lock_initialized = true;
    client->event_ready = CreateEventW(0, TRUE, FALSE, 0);
    client->event_thread = client->event_ready != 0 ?
        CreateThread(0, 0, winxterm_job_coordinator_event_thread, client, 0, 0) : 0;
    if (client->event_thread != 0) return true;
    if (client->event_ready != 0) CloseHandle(client->event_ready);
    client->event_ready = 0;
    DeleteCriticalSection(&client->event_lock);
    client->event_lock_initialized = false;
    return false;
}

void winxterm_job_coordinator_init(WinxtermJobCoordinator *coordinator,
                                   size_t command_limit)
{
    if (coordinator == 0) return;
    memset(coordinator, 0, sizeof(*coordinator));
    InitializeCriticalSection(&coordinator->lock);
    coordinator->lock_initialized = true;
    InitializeCriticalSection(&coordinator->clients_lock);
    coordinator->clients_lock_initialized = true;
    coordinator->command_limit = command_limit;
}

void winxterm_job_coordinator_dispose(WinxtermJobCoordinator *coordinator)
{
    if (coordinator == 0) return;
    WinxtermJobCoordinatorCommand *command = coordinator->command_head;
    while (command != 0) {
        WinxtermJobCoordinatorCommand *next = command->next;
        free(command);
        command = next;
    }
    WinxtermJobCoordinatorView *view = coordinator->view_head;
    while (view != 0) {
        WinxtermJobCoordinatorView *next = view->next;
        free(view->bytes);
        free(view);
        view = next;
    }
    if (coordinator->clients_lock_initialized) DeleteCriticalSection(&coordinator->clients_lock);
    if (coordinator->lock_initialized) DeleteCriticalSection(&coordinator->lock);
    memset(coordinator, 0, sizeof(*coordinator));
}

bool winxterm_job_coordinator_enqueue(WinxtermJobCoordinator *coordinator,
                                     uint32_t action, uint64_t job_id)
{
    if (coordinator == 0 || !coordinator->lock_initialized || action == 0u || job_id == 0u) {
        return false;
    }
    WinxtermJobCoordinatorCommand *command =
        (WinxtermJobCoordinatorCommand *)calloc(1u, sizeof(*command));
    if (command == 0) return false;
    command->action = action;
    command->job_id = job_id;
    EnterCriticalSection(&coordinator->lock);
    bool accepted = coordinator->command_count < coordinator->command_limit;
    if (accepted) {
        if (coordinator->command_tail != 0) coordinator->command_tail->next = command;
        else coordinator->command_head = command;
        coordinator->command_tail = command;
        ++coordinator->command_count;
    }
    LeaveCriticalSection(&coordinator->lock);
    if (!accepted) free(command);
    return accepted;
}

bool winxterm_job_coordinator_take(WinxtermJobCoordinator *coordinator,
                                  uint32_t *action, uint64_t *job_id)
{
    if (coordinator == 0 || !coordinator->lock_initialized || action == 0 || job_id == 0) {
        return false;
    }
    EnterCriticalSection(&coordinator->lock);
    WinxtermJobCoordinatorCommand *command = coordinator->command_head;
    if (command != 0) {
        coordinator->command_head = command->next;
        if (coordinator->command_head == 0) coordinator->command_tail = 0;
        --coordinator->command_count;
        *action = command->action;
        *job_id = command->job_id;
    }
    LeaveCriticalSection(&coordinator->lock);
    free(command);
    return command != 0;
}

void winxterm_job_coordinator_set_active_session(WinxtermJobCoordinator *coordinator,
                                                 uint64_t session_id)
{
    if (coordinator == 0 || !coordinator->lock_initialized) return;
    EnterCriticalSection(&coordinator->lock);
    coordinator->active_session_id = session_id;
    LeaveCriticalSection(&coordinator->lock);
}

uint64_t winxterm_job_coordinator_active_session(WinxtermJobCoordinator *coordinator)
{
    if (coordinator == 0 || !coordinator->lock_initialized) return 0u;
    EnterCriticalSection(&coordinator->lock);
    uint64_t session_id = coordinator->active_session_id;
    LeaveCriticalSection(&coordinator->lock);
    return session_id;
}

bool winxterm_job_coordinator_publish_view(WinxtermJobCoordinator *coordinator,
                                          uint64_t job_id, uint8_t *bytes,
                                          size_t byte_count)
{
    if (coordinator == 0 || !coordinator->lock_initialized || job_id == 0u ||
        (bytes == 0 && byte_count != 0u)) return false;
    WinxtermJobCoordinatorView *view =
        (WinxtermJobCoordinatorView *)calloc(1u, sizeof(*view));
    if (view == 0) return false;
    view->job_id = job_id;
    view->bytes = bytes;
    view->byte_count = byte_count;
    EnterCriticalSection(&coordinator->lock);
    bool accepted = coordinator->view_count < coordinator->command_limit;
    if (accepted) {
        if (coordinator->view_tail != 0) coordinator->view_tail->next = view;
        else coordinator->view_head = view;
        coordinator->view_tail = view;
        ++coordinator->view_count;
    }
    LeaveCriticalSection(&coordinator->lock);
    if (!accepted) free(view);
    return accepted;
}

bool winxterm_job_coordinator_take_view(WinxtermJobCoordinator *coordinator,
                                       uint64_t *job_id, uint8_t **bytes,
                                       size_t *byte_count)
{
    if (coordinator == 0 || !coordinator->lock_initialized || job_id == 0 ||
        bytes == 0 || byte_count == 0) return false;
    EnterCriticalSection(&coordinator->lock);
    WinxtermJobCoordinatorView *view = coordinator->view_head;
    if (view != 0) {
        coordinator->view_head = view->next;
        if (coordinator->view_head == 0) coordinator->view_tail = 0;
        --coordinator->view_count;
        *job_id = view->job_id;
        *bytes = view->bytes;
        *byte_count = view->byte_count;
    }
    LeaveCriticalSection(&coordinator->lock);
    free(view);
    return view != 0;
}

bool winxterm_job_coordinator_add_client(WinxtermJobCoordinator *coordinator,
                                        WinxtermJobCoordinatorClient *client)
{
    if (coordinator == 0 || !coordinator->clients_lock_initialized || client == 0 ||
        client->requester_id == 0u || client->linked) return false;
    if (!winxterm_job_coordinator_start_client(client)) return false;
    EnterCriticalSection(&coordinator->clients_lock);
    client->next = coordinator->clients;
    coordinator->clients = client;
    client->linked = true;
    LeaveCriticalSection(&coordinator->clients_lock);
    return true;
}

WinxtermJobCoordinatorClient *winxterm_job_coordinator_detach_clients(
    WinxtermJobCoordinator *coordinator)
{
    if (coordinator == 0 || !coordinator->clients_lock_initialized) return 0;
    EnterCriticalSection(&coordinator->clients_lock);
    WinxtermJobCoordinatorClient *clients = coordinator->clients;
    coordinator->clients = 0;
    LeaveCriticalSection(&coordinator->clients_lock);
    return clients;
}

void winxterm_job_coordinator_broadcast(WinxtermJobCoordinator *coordinator,
                                       const WinxtermJobFrameHeader *header,
                                       const uint8_t *payload,
                                       WinxtermJobCoordinatorClientPredicate predicate,
                                       void *predicate_context)
{
    if (coordinator == 0 || !coordinator->clients_lock_initialized || header == 0 ||
        (payload == 0 && header->payload_length != 0u)) return;
    EnterCriticalSection(&coordinator->clients_lock);
    for (WinxtermJobCoordinatorClient *client = coordinator->clients;
         client != 0; client = client->next) {
        if (!client->reply_lock_initialized || client->reply_write == 0 ||
            (predicate != 0 && !predicate(client->requester_id, predicate_context))) continue;
        WinxtermJobCoordinatorEvent *event =
            (WinxtermJobCoordinatorEvent *)calloc(1u, sizeof(*event));
        if (event != 0 && header->payload_length != 0u) {
            event->payload = (uint8_t *)malloc(header->payload_length);
            if (event->payload != 0) {
                memcpy(event->payload, payload, header->payload_length);
            } else {
                free(event);
                event = 0;
            }
        }
        if (event != 0) event->header = *header;
        EnterCriticalSection(&client->event_lock);
        bool accepted = event != 0 && !client->event_stopping &&
                        client->event_count < 256u;
        if (accepted) {
            if (client->event_tail != 0) client->event_tail->next = event;
            else client->event_head = event;
            client->event_tail = event;
            ++client->event_count;
        } else {
            client->event_overflowed = !client->event_stopping;
        }
        if (!client->event_stopping) SetEvent(client->event_ready);
        LeaveCriticalSection(&client->event_lock);
        if (!accepted && event != 0) {
            free(event->payload);
            free(event);
        }
    }
    LeaveCriticalSection(&coordinator->clients_lock);
}

void winxterm_job_coordinator_stop_client(WinxtermJobCoordinatorClient *client)
{
    if (client == 0 || !client->event_lock_initialized) return;
    EnterCriticalSection(&client->event_lock);
    client->event_stopping = true;
    SetEvent(client->event_ready);
    LeaveCriticalSection(&client->event_lock);
    if (client->event_thread != 0) {
        (void)CancelSynchronousIo(client->event_thread);
        (void)WaitForSingleObject(client->event_thread, INFINITE);
        CloseHandle(client->event_thread);
        client->event_thread = 0;
    }
    if (client->event_ready != 0) CloseHandle(client->event_ready);
    client->event_ready = 0;
    WinxtermJobCoordinatorEvent *event = client->event_head;
    while (event != 0) {
        WinxtermJobCoordinatorEvent *next = event->next;
        free(event->payload);
        free(event);
        event = next;
    }
    client->event_head = client->event_tail = 0;
    client->event_count = 0u;
    DeleteCriticalSection(&client->event_lock);
    client->event_lock_initialized = false;
}
