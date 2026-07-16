#ifndef WINXTERM_DSTCMD_JOB_CLIENT_H
#define WINXTERM_DSTCMD_JOB_CLIENT_H

#include "winxterm_job_channel.h"
#include "winxterm_job_plan.h"

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

#define WINXTERM_DSTCMD_JOB_REQUEST_TIMEOUT_MS 5000u
#define WINXTERM_DSTCMD_JOB_DISPLAY_NAME_CAPACITY 260u

typedef struct WinxtermDstcmdJobPending WinxtermDstcmdJobPending;
typedef struct WinxtermDstcmdJobQueuedEvent WinxtermDstcmdJobQueuedEvent;

typedef struct WinxtermDstcmdJobEvent {
    uint32_t kind;
    uint64_t job_id;
    uint32_t state;
    uint32_t exit_code;
} WinxtermDstcmdJobEvent;

typedef struct WinxtermDstcmdJobClient {
    HANDLE request_handle;
    HANDLE reply_handle;
    HANDLE reader_thread;
    HANDLE event_ready;
    CRITICAL_SECTION lock;
    CRITICAL_SECTION write_lock;
    bool lock_initialized;
    bool write_lock_initialized;
    WinxtermDstcmdJobPending *pending;
    WinxtermDstcmdJobQueuedEvent *event_head;
    WinxtermDstcmdJobQueuedEvent *event_tail;
    size_t event_count;
    bool event_overflowed;
    uint64_t self_id;
    uint64_t next_request_id;
    uint32_t capabilities;
    bool available;
    bool stopping;
} WinxtermDstcmdJobClient;

typedef struct WinxtermDstcmdJobInfo {
    uint64_t id;
    uint64_t owner_id;
    uint64_t buffered_output;
    uint32_t process_id;
    uint32_t state;
    uint32_t exit_code;
    bool has_exit_code;
    bool foreground;
    bool requester;
    bool input_connectable;
    bool backpressured;
    wchar_t display_name[WINXTERM_DSTCMD_JOB_DISPLAY_NAME_CAPACITY];
} WinxtermDstcmdJobInfo;

void winxterm_dstcmd_job_client_init(WinxtermDstcmdJobClient *client);
void winxterm_dstcmd_job_client_dispose(WinxtermDstcmdJobClient *client);
bool winxterm_dstcmd_job_client_available(const WinxtermDstcmdJobClient *client);
uint32_t winxterm_dstcmd_job_client_capabilities(const WinxtermDstcmdJobClient *client);
bool winxterm_dstcmd_job_client_query_capabilities(WinxtermDstcmdJobClient *client,
                                                   uint32_t *capabilities);
bool winxterm_dstcmd_job_client_list(WinxtermDstcmdJobClient *client,
                                    WinxtermDstcmdJobInfo **jobs,
                                    size_t *job_count);
bool winxterm_dstcmd_job_client_spawn(WinxtermDstcmdJobClient *client,
                                     const WinxtermJobExecutionPlan *plan,
                                     uint64_t *job_id, uint32_t *exit_code,
                                     bool *has_exit_code, uint32_t *status);
bool winxterm_dstcmd_job_client_view(WinxtermDstcmdJobClient *client, uint64_t job_id,
                                    uint8_t **bytes, size_t *byte_count,
                                    uint32_t *status);
bool winxterm_dstcmd_job_client_poll_event(WinxtermDstcmdJobClient *client,
                                          WinxtermDstcmdJobEvent *event);
HANDLE winxterm_dstcmd_job_client_event_handle(const WinxtermDstcmdJobClient *client);
bool winxterm_dstcmd_job_client_connect(WinxtermDstcmdJobClient *client,
                                       uint64_t source_id, uint64_t destination_id,
                                       uint32_t *status);
bool winxterm_dstcmd_job_client_disconnect(WinxtermDstcmdJobClient *client,
                                          uint64_t source_id, uint32_t *status);
bool winxterm_dstcmd_job_client_clean(WinxtermDstcmdJobClient *client,
                                     uint32_t *removed_count, uint32_t *status);
bool winxterm_dstcmd_job_client_interactive(WinxtermDstcmdJobClient *client,
                                           uint32_t *status);
bool winxterm_dstcmd_job_client_attach(WinxtermDstcmdJobClient *client, uint64_t job_id,
                                      const wchar_t *path, bool append, bool tee,
                                      uint32_t *status);
bool winxterm_dstcmd_job_client_kill(WinxtermDstcmdJobClient *client, uint64_t job_id,
                                    HANDLE input_handle, bool *cancelled,
                                    DWORD *elapsed_ms, uint32_t *status);
bool winxterm_dstcmd_job_client_simple(WinxtermDstcmdJobClient *client,
                                      uint16_t message_type, uint64_t target_id,
                                      uint32_t flags, uint32_t *status);
bool winxterm_dstcmd_job_client_foreground(WinxtermDstcmdJobClient *client,
                                          uint64_t target_id,
                                          uint32_t *exit_code,
                                          bool *has_exit_code,
                                          uint32_t *status);

#endif
