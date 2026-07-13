#ifndef WINXTERM_JOB_COORDINATOR_H
#define WINXTERM_JOB_COORDINATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <windows.h>

#include "winxterm_job_manager.h"
#include "winxterm_job_protocol.h"

typedef struct WinxtermJobCoordinatorClient WinxtermJobCoordinatorClient;
typedef struct WinxtermJobCoordinatorEvent WinxtermJobCoordinatorEvent;
typedef bool (*WinxtermJobCoordinatorClientPredicate)(uint64_t requester_id,
                                                       void *context);

struct WinxtermJobCoordinatorClient {
    void *context;
    WinxtermJobCoordinatorClient *next;
    uint64_t requester_id;
    HANDLE request_read;
    HANDLE reply_write;
    HANDLE thread;
    CRITICAL_SECTION reply_lock;
    bool reply_lock_initialized;
    bool linked;
    WinxtermManagedJobSnapshot *list_snapshot;
    size_t list_snapshot_count;
    uint64_t list_snapshot_token;
    CRITICAL_SECTION event_lock;
    bool event_lock_initialized;
    HANDLE event_ready;
    HANDLE event_thread;
    WinxtermJobCoordinatorEvent *event_head;
    WinxtermJobCoordinatorEvent *event_tail;
    size_t event_count;
    bool event_overflowed;
    bool event_stopping;
};

typedef struct WinxtermJobCoordinatorCommand WinxtermJobCoordinatorCommand;
typedef struct WinxtermJobCoordinatorView WinxtermJobCoordinatorView;

typedef struct WinxtermJobCoordinator {
    CRITICAL_SECTION lock;
    CRITICAL_SECTION clients_lock;
    bool lock_initialized;
    bool clients_lock_initialized;
    WinxtermJobCoordinatorCommand *command_head;
    WinxtermJobCoordinatorCommand *command_tail;
    size_t command_count;
    size_t command_limit;
    WinxtermJobCoordinatorView *view_head;
    WinxtermJobCoordinatorView *view_tail;
    size_t view_count;
    uint64_t active_session_id;
    WinxtermJobCoordinatorClient *clients;
} WinxtermJobCoordinator;

void winxterm_job_coordinator_init(WinxtermJobCoordinator *coordinator,
                                   size_t command_limit);
void winxterm_job_coordinator_dispose(WinxtermJobCoordinator *coordinator);
bool winxterm_job_coordinator_enqueue(WinxtermJobCoordinator *coordinator,
                                     uint32_t action, uint64_t job_id);
bool winxterm_job_coordinator_take(WinxtermJobCoordinator *coordinator,
                                  uint32_t *action, uint64_t *job_id);
void winxterm_job_coordinator_set_active_session(WinxtermJobCoordinator *coordinator,
                                                 uint64_t session_id);
uint64_t winxterm_job_coordinator_active_session(WinxtermJobCoordinator *coordinator);
bool winxterm_job_coordinator_publish_view(WinxtermJobCoordinator *coordinator,
                                          uint64_t job_id, uint8_t *bytes,
                                          size_t byte_count);
bool winxterm_job_coordinator_take_view(WinxtermJobCoordinator *coordinator,
                                       uint64_t *job_id, uint8_t **bytes,
                                       size_t *byte_count);
bool winxterm_job_coordinator_add_client(WinxtermJobCoordinator *coordinator,
                                        WinxtermJobCoordinatorClient *client);
WinxtermJobCoordinatorClient *winxterm_job_coordinator_detach_clients(
    WinxtermJobCoordinator *coordinator);
void winxterm_job_coordinator_broadcast(WinxtermJobCoordinator *coordinator,
                                       const WinxtermJobFrameHeader *header,
                                       const uint8_t *payload,
                                       WinxtermJobCoordinatorClientPredicate predicate,
                                       void *predicate_context);
void winxterm_job_coordinator_stop_client(WinxtermJobCoordinatorClient *client);

#endif
