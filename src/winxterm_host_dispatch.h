#ifndef WINXTERM_HOST_DISPATCH_H
#define WINXTERM_HOST_DISPATCH_H

#include "winxterm_bridge.h"
#include "winxterm_job_coordinator.h"

typedef struct WinxtermHostContext WinxtermHostContext;
typedef WinxtermJobCoordinatorClient WinxtermHostClient;

DWORD WINAPI winxterm_job_dispatch_thread(void *context);

WinxtermJobManager *winxterm_host_dispatch_manager(WinxtermHostContext *host);
bool winxterm_host_dispatch_request_ui(WinxtermHostContext *host);
uint32_t winxterm_host_spawn_plan(WinxtermHostContext *host,
                                 const uint8_t *payload, size_t payload_length,
                                 uint64_t requester_id,
                                 uint64_t *job_id, bool *foreground);
bool winxterm_host_defer_foreground_request(WinxtermHostContext *host,
                                            WinxtermHostClient *client,
                                            uint64_t job_id,
                                            uint64_t request_id);
uint32_t winxterm_host_view_job(WinxtermHostContext *host, uint64_t requester_id,
                               uint64_t job_id, uint64_t cursor,
                               uint64_t *snapshot_offset, uint8_t *bytes,
                               size_t capacity, size_t *length,
                               uint64_t *next_cursor, bool *more);
uint32_t winxterm_host_signal_job(WinxtermHostContext *host, uint64_t requester_id,
                                 uint64_t job_id, bool force);
uint32_t winxterm_host_start_kill(WinxtermHostClient *client, uint64_t job_id,
                                 uint64_t request_id);
uint32_t winxterm_host_connect_jobs(WinxtermHostContext *host,
                                   uint64_t requester_id, uint64_t source_id,
                                   uint64_t destination_id);
uint32_t winxterm_host_disconnect_job(WinxtermHostContext *host,
                                     uint64_t requester_id, uint64_t source_id);
uint32_t winxterm_host_attach_file(WinxtermHostContext *host, uint64_t job_id,
                                  uint64_t requester_id, const char *path,
                                  size_t path_length, uint32_t flags);
uint32_t winxterm_host_detach_file(WinxtermHostContext *host,
                                  uint64_t requester_id, uint64_t job_id);
uint32_t winxterm_host_cancel_request(WinxtermHostClient *client,
                                     uint64_t request_id);
uint32_t winxterm_host_lifecycle_request(WinxtermHostContext *host,
                                        WinxtermHostClient *client,
                                        uint16_t message_type,
                                        uint64_t target_id,
                                        uint32_t *removed_count);

#endif
