#ifndef WINXTERM_JOB_MANAGER_H
#define WINXTERM_JOB_MANAGER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

#define WINXTERM_JOB_OUTPUT_LIMIT (64u * 1024u * 1024u)
#define WINXTERM_JOB_TERMINATE_TIMEOUT_MS 300u
#define WINXTERM_JOB_DISPLAY_NAME_CAPACITY 260u

typedef enum WinxtermJobState {
    WINXTERM_JOB_STARTING = 0,
    WINXTERM_JOB_FOREGROUND,
    WINXTERM_JOB_BACKGROUND,
    WINXTERM_JOB_STOPPING,
    WINXTERM_JOB_EXITED,
    WINXTERM_JOB_FAILED
} WinxtermJobState;

typedef struct WinxtermManagedJobSnapshot {
    uint64_t id;
    uint64_t owner_id;
    WinxtermJobState state;
    uint32_t process_id;
    uint32_t exit_code;
    uint64_t buffered_output;
    bool has_exit_code;
    bool input_connectable;
    bool backpressured;
    bool foreground;
    wchar_t display_name[WINXTERM_JOB_DISPLAY_NAME_CAPACITY];
} WinxtermManagedJobSnapshot;

/* The implementation is deliberately opaque. Registry storage and its lock may
   only be reached through the functions below. */
typedef struct WinxtermJobManager {
    void *implementation;
} WinxtermJobManager;

bool winxterm_job_manager_init(WinxtermJobManager *manager);
void winxterm_job_manager_dispose(WinxtermJobManager *manager);
uint64_t winxterm_job_manager_add(WinxtermJobManager *manager, uint64_t owner_id,
                                 WinxtermJobState state);
bool winxterm_job_manager_set_process(WinxtermJobManager *manager, uint64_t id,
                                     uint32_t process_id, const wchar_t *display_name);
bool winxterm_job_manager_set_output(WinxtermJobManager *manager, uint64_t id,
                                    uint64_t buffered_output, bool backpressured);
bool winxterm_job_manager_set_connectable(WinxtermJobManager *manager, uint64_t id,
                                         bool connectable);
bool winxterm_job_manager_authorized(WinxtermJobManager *manager,
                                     uint64_t requester_id, uint64_t target_id);
bool winxterm_job_manager_foreground(WinxtermJobManager *manager, uint64_t id);
bool winxterm_job_manager_background(WinxtermJobManager *manager, uint64_t id);
bool winxterm_job_manager_stopping(WinxtermJobManager *manager, uint64_t id);
bool winxterm_job_manager_cancel_stopping(WinxtermJobManager *manager, uint64_t id);
bool winxterm_job_manager_exit(WinxtermJobManager *manager, uint64_t id, uint32_t exit_code);
bool winxterm_job_manager_fail(WinxtermJobManager *manager, uint64_t id, uint32_t error_code);
uint64_t winxterm_job_manager_foreground_id(WinxtermJobManager *manager);
bool winxterm_job_manager_remove(WinxtermJobManager *manager,
                                uint64_t requester_id, uint64_t target_id);
bool winxterm_job_manager_remove_finished_reparent(WinxtermJobManager *manager,
                                                   uint64_t target_id);
size_t winxterm_job_manager_clean(WinxtermJobManager *manager, uint64_t requester_id);
bool winxterm_job_manager_snapshot(WinxtermJobManager *manager,
                                  uint64_t requester_id,
                                  WinxtermManagedJobSnapshot **jobs,
                                  size_t *job_count);
bool winxterm_job_manager_snapshot_one(WinxtermJobManager *manager, uint64_t id,
                                      WinxtermManagedJobSnapshot *snapshot);
void winxterm_job_manager_snapshot_dispose(WinxtermManagedJobSnapshot *jobs);

#endif
