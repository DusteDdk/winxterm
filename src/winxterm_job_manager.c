#include "winxterm_job_manager.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef struct WinxtermManagedJob {
    WinxtermManagedJobSnapshot value;
} WinxtermManagedJob;

typedef struct WinxtermJobManagerImpl {
    CRITICAL_SECTION lock;
    WinxtermManagedJob *jobs;
    size_t job_count;
    size_t job_capacity;
    uint64_t *foreground_stack;
    size_t foreground_count;
    size_t foreground_capacity;
    uint64_t next_id;
} WinxtermJobManagerImpl;

static WinxtermJobManagerImpl *winxterm_job_manager_impl(WinxtermJobManager *manager)
{
    return manager != 0 ? (WinxtermJobManagerImpl *)manager->implementation : 0;
}

static WinxtermManagedJob *winxterm_job_manager_find_locked(WinxtermJobManagerImpl *manager,
                                                            uint64_t id)
{
    if (manager == 0 || id == 0u) return 0;
    for (size_t i = 0u; i < manager->job_count; ++i) {
        if (manager->jobs[i].value.id == id) return manager->jobs + i;
    }
    return 0;
}

static bool winxterm_job_manager_grow_jobs_locked(WinxtermJobManagerImpl *manager)
{
    if (manager->job_count < manager->job_capacity) return true;
    size_t capacity = manager->job_capacity == 0u ? 8u : manager->job_capacity * 2u;
    if (capacity < manager->job_capacity || capacity > SIZE_MAX / sizeof(*manager->jobs)) return false;
    void *jobs = realloc(manager->jobs, capacity * sizeof(*manager->jobs));
    if (jobs == 0) return false;
    manager->jobs = (WinxtermManagedJob *)jobs;
    manager->job_capacity = capacity;
    return true;
}

static bool winxterm_job_manager_grow_stack_locked(WinxtermJobManagerImpl *manager)
{
    if (manager->foreground_count < manager->foreground_capacity) return true;
    size_t capacity = manager->foreground_capacity == 0u ? 8u : manager->foreground_capacity * 2u;
    if (capacity < manager->foreground_capacity ||
        capacity > SIZE_MAX / sizeof(*manager->foreground_stack)) return false;
    void *stack = realloc(manager->foreground_stack, capacity * sizeof(*manager->foreground_stack));
    if (stack == 0) return false;
    manager->foreground_stack = (uint64_t *)stack;
    manager->foreground_capacity = capacity;
    return true;
}

static bool winxterm_job_manager_authorized_locked(WinxtermJobManagerImpl *manager,
                                                   uint64_t requester_id, uint64_t target_id)
{
    if (manager == 0 || requester_id == 0u || target_id == 0u) return false;
    uint64_t cursor = target_id;
    for (size_t steps = 0u; steps <= manager->job_count; ++steps) {
        if (cursor == requester_id) return true;
        WinxtermManagedJob *job = winxterm_job_manager_find_locked(manager, cursor);
        if (job == 0 || job->value.owner_id == 0u) return false;
        cursor = job->value.owner_id;
    }
    return false;
}

static void winxterm_job_manager_remove_stack_locked(WinxtermJobManagerImpl *manager, uint64_t id)
{
    size_t out = 0u;
    for (size_t i = 0u; i < manager->foreground_count; ++i) {
        if (manager->foreground_stack[i] != id) manager->foreground_stack[out++] = manager->foreground_stack[i];
    }
    manager->foreground_count = out;
}

static uint64_t winxterm_job_manager_foreground_locked(WinxtermJobManagerImpl *manager)
{
    return manager->foreground_count != 0u ? manager->foreground_stack[manager->foreground_count - 1u] : 0u;
}

static void winxterm_job_manager_restore_locked(WinxtermJobManagerImpl *manager)
{
    while (manager->foreground_count != 0u) {
        uint64_t id = manager->foreground_stack[manager->foreground_count - 1u];
        WinxtermManagedJob *job = winxterm_job_manager_find_locked(manager, id);
        if (job != 0 && job->value.state != WINXTERM_JOB_EXITED && job->value.state != WINXTERM_JOB_FAILED) {
            job->value.state = WINXTERM_JOB_FOREGROUND;
            return;
        }
        --manager->foreground_count;
    }
}

bool winxterm_job_manager_init(WinxtermJobManager *manager)
{
    if (manager == 0) return false;
    manager->implementation = 0;
    WinxtermJobManagerImpl *impl = (WinxtermJobManagerImpl *)calloc(1u, sizeof(*impl));
    if (impl == 0) return false;
    InitializeCriticalSection(&impl->lock);
    impl->next_id = 1u;
    manager->implementation = impl;
    return true;
}

void winxterm_job_manager_dispose(WinxtermJobManager *manager)
{
    WinxtermJobManagerImpl *impl = winxterm_job_manager_impl(manager);
    if (impl == 0) return;
    free(impl->jobs);
    free(impl->foreground_stack);
    DeleteCriticalSection(&impl->lock);
    free(impl);
    manager->implementation = 0;
}

uint64_t winxterm_job_manager_add(WinxtermJobManager *manager, uint64_t owner_id, WinxtermJobState state)
{
    WinxtermJobManagerImpl *impl = winxterm_job_manager_impl(manager);
    if (impl == 0) return 0u;
    EnterCriticalSection(&impl->lock);
    if (impl->next_id == 0u ||
        (owner_id != 0u && winxterm_job_manager_find_locked(impl, owner_id) == 0) ||
        !winxterm_job_manager_grow_jobs_locked(impl)) {
        LeaveCriticalSection(&impl->lock);
        return 0u;
    }
    uint64_t id = impl->next_id++;
    WinxtermManagedJob *job = impl->jobs + impl->job_count++;
    memset(job, 0, sizeof(*job));
    job->value.id = id;
    job->value.owner_id = owner_id;
    job->value.state = state;
    if (state == WINXTERM_JOB_FOREGROUND) {
        uint64_t old_id = winxterm_job_manager_foreground_locked(impl);
        WinxtermManagedJob *old = winxterm_job_manager_find_locked(impl, old_id);
        if (!winxterm_job_manager_grow_stack_locked(impl)) {
            --impl->job_count;
            LeaveCriticalSection(&impl->lock);
            return 0u;
        }
        if (old != 0) old->value.state = WINXTERM_JOB_BACKGROUND;
        impl->foreground_stack[impl->foreground_count++] = id;
    }
    LeaveCriticalSection(&impl->lock);
    return id;
}

bool winxterm_job_manager_set_process(WinxtermJobManager *manager, uint64_t id,
                                     uint32_t process_id, const wchar_t *display_name)
{
    WinxtermJobManagerImpl *impl = winxterm_job_manager_impl(manager);
    if (impl == 0 || process_id == 0u) return false;
    EnterCriticalSection(&impl->lock);
    WinxtermManagedJob *job = winxterm_job_manager_find_locked(impl, id);
    if (job != 0) {
        job->value.process_id = process_id;
        if (display_name != 0) wcsncpy_s(job->value.display_name,
                                        WINXTERM_JOB_DISPLAY_NAME_CAPACITY,
                                        display_name, _TRUNCATE);
    }
    LeaveCriticalSection(&impl->lock);
    return job != 0;
}

bool winxterm_job_manager_set_output(WinxtermJobManager *manager, uint64_t id,
                                    uint64_t buffered_output, bool backpressured)
{
    WinxtermJobManagerImpl *impl = winxterm_job_manager_impl(manager);
    if (impl == 0 || buffered_output > WINXTERM_JOB_OUTPUT_LIMIT) return false;
    EnterCriticalSection(&impl->lock);
    WinxtermManagedJob *job = winxterm_job_manager_find_locked(impl, id);
    if (job != 0) { job->value.buffered_output = buffered_output; job->value.backpressured = backpressured; }
    LeaveCriticalSection(&impl->lock);
    return job != 0;
}

bool winxterm_job_manager_set_connectable(WinxtermJobManager *manager, uint64_t id, bool connectable)
{
    WinxtermJobManagerImpl *impl = winxterm_job_manager_impl(manager);
    if (impl == 0) return false;
    EnterCriticalSection(&impl->lock);
    WinxtermManagedJob *job = winxterm_job_manager_find_locked(impl, id);
    if (job != 0) job->value.input_connectable = connectable;
    LeaveCriticalSection(&impl->lock);
    return job != 0;
}

bool winxterm_job_manager_authorized(WinxtermJobManager *manager, uint64_t requester_id, uint64_t target_id)
{
    WinxtermJobManagerImpl *impl = winxterm_job_manager_impl(manager);
    if (impl == 0) return false;
    EnterCriticalSection(&impl->lock);
    bool result = winxterm_job_manager_authorized_locked(impl, requester_id, target_id);
    LeaveCriticalSection(&impl->lock);
    return result;
}

bool winxterm_job_manager_foreground(WinxtermJobManager *manager, uint64_t id)
{
    WinxtermJobManagerImpl *impl = winxterm_job_manager_impl(manager);
    if (impl == 0) return false;
    EnterCriticalSection(&impl->lock);
    WinxtermManagedJob *target = winxterm_job_manager_find_locked(impl, id);
    bool ok = target != 0 && target->value.state != WINXTERM_JOB_EXITED &&
              target->value.state != WINXTERM_JOB_FAILED;
    if (ok) {
        uint64_t old_id = winxterm_job_manager_foreground_locked(impl);
        WinxtermManagedJob *old = winxterm_job_manager_find_locked(impl, old_id);
        winxterm_job_manager_remove_stack_locked(impl, id);
        ok = winxterm_job_manager_grow_stack_locked(impl);
        if (ok) {
            if (old != 0 && old != target) old->value.state = WINXTERM_JOB_BACKGROUND;
            impl->foreground_stack[impl->foreground_count++] = id;
            target->value.state = WINXTERM_JOB_FOREGROUND;
        }
    }
    LeaveCriticalSection(&impl->lock);
    return ok;
}

bool winxterm_job_manager_background(WinxtermJobManager *manager, uint64_t id)
{
    WinxtermJobManagerImpl *impl = winxterm_job_manager_impl(manager);
    if (impl == 0) return false;
    EnterCriticalSection(&impl->lock);
    WinxtermManagedJob *job = winxterm_job_manager_find_locked(impl, id);
    bool ok = job != 0 && job->value.state != WINXTERM_JOB_EXITED && job->value.state != WINXTERM_JOB_FAILED;
    if (ok) {
        bool was_foreground = winxterm_job_manager_foreground_locked(impl) == id;
        winxterm_job_manager_remove_stack_locked(impl, id);
        job->value.state = WINXTERM_JOB_BACKGROUND;
        job->value.input_connectable = true;
        if (was_foreground) winxterm_job_manager_restore_locked(impl);
    }
    LeaveCriticalSection(&impl->lock);
    return ok;
}

bool winxterm_job_manager_stopping(WinxtermJobManager *manager, uint64_t id)
{
    WinxtermJobManagerImpl *impl = winxterm_job_manager_impl(manager);
    if (impl == 0) return false;
    EnterCriticalSection(&impl->lock);
    WinxtermManagedJob *job = winxterm_job_manager_find_locked(impl, id);
    bool ok = job != 0 && job->value.state != WINXTERM_JOB_EXITED &&
              job->value.state != WINXTERM_JOB_FAILED;
    if (ok) job->value.state = WINXTERM_JOB_STOPPING;
    LeaveCriticalSection(&impl->lock);
    return ok;
}

bool winxterm_job_manager_cancel_stopping(WinxtermJobManager *manager, uint64_t id)
{
    WinxtermJobManagerImpl *impl = winxterm_job_manager_impl(manager);
    if (impl == 0) return false;
    EnterCriticalSection(&impl->lock);
    WinxtermManagedJob *job = winxterm_job_manager_find_locked(impl, id);
    bool ok = job != 0 && job->value.state == WINXTERM_JOB_STOPPING;
    if (ok) {
        job->value.state = winxterm_job_manager_foreground_locked(impl) == id ?
            WINXTERM_JOB_FOREGROUND : WINXTERM_JOB_BACKGROUND;
    }
    LeaveCriticalSection(&impl->lock);
    return ok;
}

static bool winxterm_job_manager_finish(WinxtermJobManager *manager, uint64_t id,
                                       uint32_t code, WinxtermJobState state)
{
    WinxtermJobManagerImpl *impl = winxterm_job_manager_impl(manager);
    if (impl == 0) return false;
    EnterCriticalSection(&impl->lock);
    WinxtermManagedJob *job = winxterm_job_manager_find_locked(impl, id);
    bool ok = job != 0;
    if (ok) {
        bool was_foreground = winxterm_job_manager_foreground_locked(impl) == id;
        job->value.state = state;
        job->value.exit_code = code;
        job->value.has_exit_code = true;
        winxterm_job_manager_remove_stack_locked(impl, id);
        if (was_foreground) winxterm_job_manager_restore_locked(impl);
    }
    LeaveCriticalSection(&impl->lock);
    return ok;
}

bool winxterm_job_manager_exit(WinxtermJobManager *manager, uint64_t id, uint32_t exit_code)
{ return winxterm_job_manager_finish(manager, id, exit_code, WINXTERM_JOB_EXITED); }

bool winxterm_job_manager_complete(
    WinxtermJobManager *manager, uint64_t id, uint32_t exit_code,
    WinxtermManagedJobSnapshot *finished_snapshot, bool *removed_foreground)
{
    WinxtermJobManagerImpl *impl = winxterm_job_manager_impl(manager);
    if (removed_foreground != 0) *removed_foreground = false;
    if (impl == 0 || id == 0u || finished_snapshot == 0 ||
        removed_foreground == 0) return false;
    EnterCriticalSection(&impl->lock);
    bool completed = false;
    for (size_t i = 0u; i < impl->job_count; ++i) {
        WinxtermManagedJob *target = impl->jobs + i;
        if (target->value.id != id) continue;
        bool was_foreground = winxterm_job_manager_foreground_locked(impl) == id;
        target->value.state = WINXTERM_JOB_EXITED;
        target->value.exit_code = exit_code;
        target->value.has_exit_code = true;
        *finished_snapshot = target->value;
        finished_snapshot->foreground = was_foreground;
        winxterm_job_manager_remove_stack_locked(impl, id);
        if (was_foreground) {
            uint64_t owner_id = target->value.owner_id;
            for (size_t child = 0u; child < impl->job_count; ++child) {
                if (impl->jobs[child].value.owner_id == id) {
                    impl->jobs[child].value.owner_id = owner_id;
                }
            }
            winxterm_job_manager_restore_locked(impl);
            memmove(impl->jobs + i, impl->jobs + i + 1u,
                    (impl->job_count - i - 1u) * sizeof(*impl->jobs));
            --impl->job_count;
            *removed_foreground = true;
        }
        completed = true;
        break;
    }
    LeaveCriticalSection(&impl->lock);
    return completed;
}

bool winxterm_job_manager_fail(WinxtermJobManager *manager, uint64_t id, uint32_t error_code)
{ return winxterm_job_manager_finish(manager, id, error_code, WINXTERM_JOB_FAILED); }

uint64_t winxterm_job_manager_foreground_id(WinxtermJobManager *manager)
{
    WinxtermJobManagerImpl *impl = winxterm_job_manager_impl(manager);
    if (impl == 0) return 0u;
    EnterCriticalSection(&impl->lock);
    uint64_t id = winxterm_job_manager_foreground_locked(impl);
    LeaveCriticalSection(&impl->lock);
    return id;
}

static bool winxterm_job_manager_removable_locked(WinxtermJobManagerImpl *impl,
                                                  uint64_t requester_id, size_t index)
{
    WinxtermManagedJob *target = impl->jobs + index;
    if ((target->value.state != WINXTERM_JOB_EXITED && target->value.state != WINXTERM_JOB_FAILED) ||
        !winxterm_job_manager_authorized_locked(impl, requester_id, target->value.id)) return false;
    for (size_t i = 0u; i < impl->job_count; ++i) {
        if (impl->jobs[i].value.owner_id == target->value.id) return false;
    }
    return true;
}

bool winxterm_job_manager_remove(WinxtermJobManager *manager, uint64_t requester_id, uint64_t target_id)
{
    WinxtermJobManagerImpl *impl = winxterm_job_manager_impl(manager);
    if (impl == 0) return false;
    EnterCriticalSection(&impl->lock);
    bool removed = false;
    for (size_t i = 0u; i < impl->job_count; ++i) {
        if (impl->jobs[i].value.id == target_id && winxterm_job_manager_removable_locked(impl, requester_id, i)) {
            winxterm_job_manager_remove_stack_locked(impl, target_id);
            memmove(impl->jobs + i, impl->jobs + i + 1u,
                    (impl->job_count - i - 1u) * sizeof(*impl->jobs));
            --impl->job_count;
            removed = true;
            break;
        }
    }
    LeaveCriticalSection(&impl->lock);
    return removed;
}

bool winxterm_job_manager_remove_finished_reparent(WinxtermJobManager *manager,
                                                   uint64_t target_id)
{
    WinxtermJobManagerImpl *impl = winxterm_job_manager_impl(manager);
    if (impl == 0 || target_id == 0u) return false;
    EnterCriticalSection(&impl->lock);
    bool removed = false;
    for (size_t i = 0u; i < impl->job_count; ++i) {
        WinxtermManagedJob *target = impl->jobs + i;
        if (target->value.id != target_id ||
            (target->value.state != WINXTERM_JOB_EXITED &&
             target->value.state != WINXTERM_JOB_FAILED)) continue;
        uint64_t owner_id = target->value.owner_id;
        for (size_t child = 0u; child < impl->job_count; ++child) {
            if (impl->jobs[child].value.owner_id == target_id) {
                impl->jobs[child].value.owner_id = owner_id;
            }
        }
        winxterm_job_manager_remove_stack_locked(impl, target_id);
        memmove(impl->jobs + i, impl->jobs + i + 1u,
                (impl->job_count - i - 1u) * sizeof(*impl->jobs));
        --impl->job_count;
        removed = true;
        break;
    }
    LeaveCriticalSection(&impl->lock);
    return removed;
}

size_t winxterm_job_manager_clean(WinxtermJobManager *manager, uint64_t requester_id)
{
    WinxtermJobManagerImpl *impl = winxterm_job_manager_impl(manager);
    if (impl == 0) return 0u;
    EnterCriticalSection(&impl->lock);
    size_t removed = 0u;
    bool progress;
    do {
        progress = false;
        for (size_t i = impl->job_count; i-- > 0u;) {
            if (winxterm_job_manager_removable_locked(impl, requester_id, i)) {
                uint64_t id = impl->jobs[i].value.id;
                winxterm_job_manager_remove_stack_locked(impl, id);
                memmove(impl->jobs + i, impl->jobs + i + 1u,
                        (impl->job_count - i - 1u) * sizeof(*impl->jobs));
                --impl->job_count;
                ++removed;
                progress = true;
            }
        }
    } while (progress);
    LeaveCriticalSection(&impl->lock);
    return removed;
}

bool winxterm_job_manager_snapshot(WinxtermJobManager *manager, uint64_t requester_id,
                                  WinxtermManagedJobSnapshot **jobs, size_t *job_count)
{
    if (jobs != 0) *jobs = 0;
    if (job_count != 0) *job_count = 0u;
    WinxtermJobManagerImpl *impl = winxterm_job_manager_impl(manager);
    if (impl == 0 || jobs == 0 || job_count == 0) return false;
    EnterCriticalSection(&impl->lock);
    size_t count = 0u;
    for (size_t i = 0u; i < impl->job_count; ++i) {
        if (requester_id == 0u || winxterm_job_manager_authorized_locked(impl, requester_id,
                                                                        impl->jobs[i].value.id)) ++count;
    }
    WinxtermManagedJobSnapshot *copy = count != 0u ?
        (WinxtermManagedJobSnapshot *)calloc(count, sizeof(*copy)) : 0;
    bool ok = count == 0u || copy != 0;
    if (ok) {
        size_t out = 0u;
        uint64_t foreground = winxterm_job_manager_foreground_locked(impl);
        for (size_t i = 0u; i < impl->job_count; ++i) {
            if (requester_id != 0u && !winxterm_job_manager_authorized_locked(impl, requester_id,
                                                                              impl->jobs[i].value.id)) continue;
            copy[out] = impl->jobs[i].value;
            copy[out].foreground = copy[out].id == foreground;
            ++out;
        }
        *jobs = copy;
        *job_count = count;
    }
    LeaveCriticalSection(&impl->lock);
    return ok;
}

bool winxterm_job_manager_snapshot_one(WinxtermJobManager *manager, uint64_t id,
                                      WinxtermManagedJobSnapshot *snapshot)
{
    WinxtermJobManagerImpl *impl = winxterm_job_manager_impl(manager);
    if (impl == 0 || snapshot == 0) return false;
    EnterCriticalSection(&impl->lock);
    WinxtermManagedJob *job = winxterm_job_manager_find_locked(impl, id);
    if (job != 0) {
        *snapshot = job->value;
        snapshot->foreground = id == winxterm_job_manager_foreground_locked(impl);
    }
    LeaveCriticalSection(&impl->lock);
    return job != 0;
}

void winxterm_job_manager_snapshot_dispose(WinxtermManagedJobSnapshot *jobs)
{ free(jobs); }
