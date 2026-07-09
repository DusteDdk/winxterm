#ifndef WINXTERM_DSTCMD_JOBS_H
#define WINXTERM_DSTCMD_JOBS_H

#include <stdbool.h>
#include <stddef.h>
#include <windows.h>

#define WINXTERM_DSTCMD_JOB_CORE_WORKERS 2u

typedef void (*WinxtermDstcmdJobFn)(void *context);
typedef void (*WinxtermDstcmdJobCleanupFn)(void *context);

typedef struct WinxtermDstcmdJob WinxtermDstcmdJob;
typedef struct WinxtermDstcmdWorkerNode WinxtermDstcmdWorkerNode;

typedef struct WinxtermDstcmdJobPool {
    CRITICAL_SECTION lock;
    HANDLE work_semaphore;
    HANDLE idle_event;
    HANDLE core_ready_event;
    WinxtermDstcmdJob *head;
    WinxtermDstcmdJob *tail;
    WinxtermDstcmdWorkerNode *workers;
    size_t queued_count;
    size_t running_count;
    size_t idle_worker_count;
    size_t idle_core_worker_count;
    size_t total_worker_count;
    size_t core_worker_count;
    size_t overflow_worker_count;
    bool lock_initialized;
    bool accepting;
    bool shutting_down;
} WinxtermDstcmdJobPool;

bool winxterm_dstcmd_job_pool_init(WinxtermDstcmdJobPool *pool);
void winxterm_dstcmd_job_pool_dispose(WinxtermDstcmdJobPool *pool);
bool winxterm_dstcmd_job_pool_submit(WinxtermDstcmdJobPool *pool,
                                     WinxtermDstcmdJobFn run,
                                     WinxtermDstcmdJobCleanupFn cleanup,
                                     void *context);
bool winxterm_dstcmd_job_pool_wait_idle(WinxtermDstcmdJobPool *pool, DWORD timeout_ms);
size_t winxterm_dstcmd_job_pool_total_worker_count(WinxtermDstcmdJobPool *pool);
size_t winxterm_dstcmd_job_pool_core_worker_count(WinxtermDstcmdJobPool *pool);
size_t winxterm_dstcmd_job_pool_idle_worker_count(WinxtermDstcmdJobPool *pool);

#endif
