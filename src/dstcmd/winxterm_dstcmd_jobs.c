#include "dstcmd/winxterm_dstcmd_jobs.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define WINXTERM_DSTCMD_OVERFLOW_IDLE_MS 1000u

struct WinxtermDstcmdJob {
    WinxtermDstcmdJobFn run;
    WinxtermDstcmdJobCleanupFn cleanup;
    void *context;
    WinxtermDstcmdJob *next;
};

struct WinxtermDstcmdWorkerNode {
    WinxtermDstcmdJobPool *pool;
    HANDLE thread;
    DWORD thread_id;
    bool core;
    WinxtermDstcmdWorkerNode *next;
};

static void winxterm_dstcmd_job_pool_remove_worker_locked(WinxtermDstcmdJobPool *pool,
                                                         WinxtermDstcmdWorkerNode *worker)
{
    WinxtermDstcmdWorkerNode **link = &pool->workers;
    while (*link != 0) {
        if (*link == worker) {
            *link = worker->next;
            return;
        }
        link = &(*link)->next;
    }
}

static void winxterm_dstcmd_job_pool_note_worker_exit_locked(WinxtermDstcmdJobPool *pool,
                                                            const WinxtermDstcmdWorkerNode *worker)
{
    if (pool->total_worker_count != 0u) {
        --pool->total_worker_count;
    }
    if (worker->core) {
        if (pool->core_worker_count != 0u) {
            --pool->core_worker_count;
        }
    } else if (pool->overflow_worker_count != 0u) {
        --pool->overflow_worker_count;
    }
}

static WinxtermDstcmdJob *winxterm_dstcmd_job_pool_pop_locked(WinxtermDstcmdJobPool *pool)
{
    WinxtermDstcmdJob *job = pool->head;
    if (job == 0) {
        return 0;
    }
    pool->head = job->next;
    if (pool->head == 0) {
        pool->tail = 0;
    }
    job->next = 0;
    if (pool->queued_count != 0u) {
        --pool->queued_count;
    }
    ++pool->running_count;
    return job;
}

static void winxterm_dstcmd_job_pool_run_job(WinxtermDstcmdJob *job)
{
    if (job == 0) {
        return;
    }
    if (job->run != 0) {
        job->run(job->context);
    }
    if (job->cleanup != 0) {
        job->cleanup(job->context);
    }
    free(job);
}

static DWORD WINAPI winxterm_dstcmd_job_worker_thread(void *context)
{
    WinxtermDstcmdWorkerNode *worker = (WinxtermDstcmdWorkerNode *)context;
    WinxtermDstcmdJobPool *pool = worker != 0 ? worker->pool : 0;
    if (pool == 0) {
        return 1;
    }

    for (;;) {
        EnterCriticalSection(&pool->lock);
        ++pool->idle_worker_count;
        if (worker->core) {
            ++pool->idle_core_worker_count;
            if (pool->idle_core_worker_count >= WINXTERM_DSTCMD_JOB_CORE_WORKERS) {
                SetEvent(pool->core_ready_event);
            }
        }
        LeaveCriticalSection(&pool->lock);

        DWORD wait_result = WaitForSingleObject(pool->work_semaphore,
                                                worker->core ?
                                                    INFINITE : WINXTERM_DSTCMD_OVERFLOW_IDLE_MS);

        EnterCriticalSection(&pool->lock);
        if (pool->idle_worker_count != 0u) {
            --pool->idle_worker_count;
        }
        if (worker->core && pool->idle_core_worker_count != 0u) {
            --pool->idle_core_worker_count;
        }
        WinxtermDstcmdJob *job = wait_result == WAIT_OBJECT_0 ?
            winxterm_dstcmd_job_pool_pop_locked(pool) : 0;
        bool exit_worker = job == 0 && (wait_result != WAIT_OBJECT_0 ||
                                        pool->shutting_down ||
                                        !worker->core);
        if (exit_worker) {
            winxterm_dstcmd_job_pool_note_worker_exit_locked(pool, worker);
        }
        LeaveCriticalSection(&pool->lock);

        if (job != 0) {
            winxterm_dstcmd_job_pool_run_job(job);
            EnterCriticalSection(&pool->lock);
            if (pool->running_count != 0u) {
                --pool->running_count;
            }
            if (pool->queued_count == 0u && pool->running_count == 0u) {
                SetEvent(pool->idle_event);
            }
            LeaveCriticalSection(&pool->lock);
            continue;
        }

        if (exit_worker) {
            return wait_result == WAIT_FAILED ? 1 : 0;
        }
    }
}

static bool winxterm_dstcmd_job_pool_start_worker(WinxtermDstcmdJobPool *pool, bool core)
{
    WinxtermDstcmdWorkerNode *worker =
        (WinxtermDstcmdWorkerNode *)calloc(1u, sizeof(*worker));
    if (worker == 0) {
        return false;
    }
    worker->pool = pool;
    worker->core = core;

    EnterCriticalSection(&pool->lock);
    worker->next = pool->workers;
    pool->workers = worker;
    ++pool->total_worker_count;
    if (core) {
        ++pool->core_worker_count;
    } else {
        ++pool->overflow_worker_count;
    }
    LeaveCriticalSection(&pool->lock);

    worker->thread = CreateThread(0,
                                  0,
                                  winxterm_dstcmd_job_worker_thread,
                                  worker,
                                  0,
                                  &worker->thread_id);
    if (worker->thread != 0) {
        return true;
    }

    EnterCriticalSection(&pool->lock);
    winxterm_dstcmd_job_pool_remove_worker_locked(pool, worker);
    winxterm_dstcmd_job_pool_note_worker_exit_locked(pool, worker);
    LeaveCriticalSection(&pool->lock);
    free(worker);
    return false;
}

bool winxterm_dstcmd_job_pool_init(WinxtermDstcmdJobPool *pool)
{
    if (pool == 0) {
        return false;
    }
    memset(pool, 0, sizeof(*pool));
    InitializeCriticalSection(&pool->lock);
    pool->lock_initialized = true;
    pool->work_semaphore = CreateSemaphoreW(0, 0, LONG_MAX, 0);
    pool->idle_event = CreateEventW(0, TRUE, TRUE, 0);
    pool->core_ready_event = CreateEventW(0, TRUE, FALSE, 0);
    pool->accepting = true;
    if (pool->work_semaphore == 0 || pool->idle_event == 0 || pool->core_ready_event == 0) {
        winxterm_dstcmd_job_pool_dispose(pool);
        return false;
    }

    for (size_t i = 0u; i < WINXTERM_DSTCMD_JOB_CORE_WORKERS; ++i) {
        if (!winxterm_dstcmd_job_pool_start_worker(pool, true)) {
            winxterm_dstcmd_job_pool_dispose(pool);
            return false;
        }
    }

    if (WaitForSingleObject(pool->core_ready_event, 5000u) != WAIT_OBJECT_0) {
        winxterm_dstcmd_job_pool_dispose(pool);
        return false;
    }
    return true;
}

void winxterm_dstcmd_job_pool_dispose(WinxtermDstcmdJobPool *pool)
{
    if (pool == 0) {
        return;
    }

    size_t wake_count = 0u;
    if (pool->lock_initialized) {
        EnterCriticalSection(&pool->lock);
        pool->accepting = false;
        pool->shutting_down = true;
        wake_count = pool->total_worker_count;
        LeaveCriticalSection(&pool->lock);
    }
    for (size_t i = 0u; i < wake_count; ++i) {
        if (pool->work_semaphore != 0) {
            ReleaseSemaphore(pool->work_semaphore, 1, 0);
        }
    }

    WinxtermDstcmdWorkerNode *worker = pool->workers;
    while (worker != 0) {
        WinxtermDstcmdWorkerNode *next = worker->next;
        if (worker->thread != 0) {
            WaitForSingleObject(worker->thread, INFINITE);
            CloseHandle(worker->thread);
        }
        free(worker);
        worker = next;
    }

    WinxtermDstcmdJob *job = pool->head;
    while (job != 0) {
        WinxtermDstcmdJob *next = job->next;
        if (job->cleanup != 0) {
            job->cleanup(job->context);
        }
        free(job);
        job = next;
    }

    if (pool->work_semaphore != 0) {
        CloseHandle(pool->work_semaphore);
    }
    if (pool->idle_event != 0) {
        CloseHandle(pool->idle_event);
    }
    if (pool->core_ready_event != 0) {
        CloseHandle(pool->core_ready_event);
    }
    if (pool->lock_initialized) {
        DeleteCriticalSection(&pool->lock);
    }
    memset(pool, 0, sizeof(*pool));
}

bool winxterm_dstcmd_job_pool_submit(WinxtermDstcmdJobPool *pool,
                                     WinxtermDstcmdJobFn run,
                                     WinxtermDstcmdJobCleanupFn cleanup,
                                     void *context)
{
    if (pool == 0 || run == 0 || !pool->lock_initialized || pool->work_semaphore == 0) {
        return false;
    }
    WinxtermDstcmdJob *job = (WinxtermDstcmdJob *)calloc(1u, sizeof(*job));
    if (job == 0) {
        return false;
    }
    job->run = run;
    job->cleanup = cleanup;
    job->context = context;

    bool spawn_overflow = false;
    EnterCriticalSection(&pool->lock);
    if (!pool->accepting || pool->shutting_down) {
        LeaveCriticalSection(&pool->lock);
        free(job);
        return false;
    }
    ResetEvent(pool->idle_event);
    if (pool->tail != 0) {
        pool->tail->next = job;
    } else {
        pool->head = job;
    }
    pool->tail = job;
    ++pool->queued_count;
    spawn_overflow = pool->idle_worker_count == 0u;
    LeaveCriticalSection(&pool->lock);

    ReleaseSemaphore(pool->work_semaphore, 1, 0);
    if (spawn_overflow) {
        (void)winxterm_dstcmd_job_pool_start_worker(pool, false);
    }
    return true;
}

bool winxterm_dstcmd_job_pool_wait_idle(WinxtermDstcmdJobPool *pool, DWORD timeout_ms)
{
    if (pool == 0 || pool->idle_event == 0) {
        return false;
    }
    return WaitForSingleObject(pool->idle_event, timeout_ms) == WAIT_OBJECT_0;
}

size_t winxterm_dstcmd_job_pool_total_worker_count(WinxtermDstcmdJobPool *pool)
{
    if (pool == 0 || !pool->lock_initialized) {
        return 0u;
    }
    EnterCriticalSection(&pool->lock);
    size_t count = pool->total_worker_count;
    LeaveCriticalSection(&pool->lock);
    return count;
}

size_t winxterm_dstcmd_job_pool_core_worker_count(WinxtermDstcmdJobPool *pool)
{
    if (pool == 0 || !pool->lock_initialized) {
        return 0u;
    }
    EnterCriticalSection(&pool->lock);
    size_t count = pool->core_worker_count;
    LeaveCriticalSection(&pool->lock);
    return count;
}

size_t winxterm_dstcmd_job_pool_idle_worker_count(WinxtermDstcmdJobPool *pool)
{
    if (pool == 0 || !pool->lock_initialized) {
        return 0u;
    }
    EnterCriticalSection(&pool->lock);
    size_t count = pool->idle_worker_count;
    LeaveCriticalSection(&pool->lock);
    return count;
}
