#include "dstcmd/winxterm_dstcmd.h"

#include "dstcmd/api/path.h"
#include "dstcmd/winxterm_dstcmd_parse.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef struct WinxtermDstcmdDirRefreshJob {
    WinxtermDstcmdShell *shell;
    wchar_t path[WINXTERM_DSTCMD_PATH_CAPACITY];
    WinxtermDstcmdDirRefreshSource source;
    WinxtermDstcmdDirSnapshot baseline;
    WinxtermDstcmdScratch scratch;
} WinxtermDstcmdDirRefreshJob;

static int winxterm_dstcmd_dir_compare_name(const void *left, const void *right)
{
    const WinxtermDstcmdDirEntry *a = (const WinxtermDstcmdDirEntry *)left;
    const WinxtermDstcmdDirEntry *b = (const WinxtermDstcmdDirEntry *)right;
    return _wcsicmp(a->name != 0 ? a->name : L"", b->name != 0 ? b->name : L"");
}

static bool winxterm_dstcmd_dir_add_entry(WinxtermDstcmdDirSnapshot *snapshot,
                                          const WIN32_FIND_DATAW *data)
{
    if (snapshot == 0 || data == 0) {
        return false;
    }
    WinxtermDstcmdDirEntry *entries =
        (WinxtermDstcmdDirEntry *)realloc(snapshot->entries,
                                          (snapshot->count + 1u) * sizeof(*entries));
    if (entries == 0) {
        return false;
    }
    snapshot->entries = entries;
    WinxtermDstcmdDirEntry *entry = snapshot->entries + snapshot->count;
    memset(entry, 0, sizeof(*entry));
    entry->name = winxterm_dstcmd_wcsdup(data->cFileName);
    if (entry->name == 0) {
        return false;
    }
    entry->attributes = data->dwFileAttributes;
    entry->size = ((ULONGLONG)data->nFileSizeHigh << 32) | (ULONGLONG)data->nFileSizeLow;
    entry->write_time = data->ftLastWriteTime;
    ++snapshot->count;
    return true;
}

static bool winxterm_dstcmd_dir_read(WinxtermDstcmdScratch *scratch,
                                     const wchar_t *path,
                                     WinxtermDstcmdDirSnapshot *snapshot)
{
    if (scratch == 0 || path == 0 || snapshot == 0) {
        return false;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (wcsncpy_s(snapshot->path, WINXTERM_DSTCMD_PATH_CAPACITY, path, _TRUNCATE) != 0) {
        return false;
    }

    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(scratch);
    WinxtermDstcmdDirIter iter;
    if (!winxterm_dstcmd_dir_iter_open_scratch(scratch, path, &iter)) {
        snapshot->valid = true;
        winxterm_dstcmd_scratch_rewind(scratch, mark);
        return true;
    }

    bool ok = true;
    const WIN32_FIND_DATAW *data = 0;
    while (winxterm_dstcmd_dir_iter_next(&iter, &data)) {
        if (!winxterm_dstcmd_dir_add_entry(snapshot, data)) {
            ok = false;
            break;
        }
    }
    winxterm_dstcmd_dir_iter_close(&iter);
    if (!ok) {
        winxterm_dstcmd_dir_snapshot_dispose(snapshot);
        goto cleanup;
    }
    qsort(snapshot->entries, snapshot->count, sizeof(*snapshot->entries), winxterm_dstcmd_dir_compare_name);
    snapshot->valid = true;
    ok = true;

cleanup:
    winxterm_dstcmd_scratch_rewind(scratch, mark);
    return ok;
}

void winxterm_dstcmd_dir_snapshot_dispose(WinxtermDstcmdDirSnapshot *snapshot)
{
    if (snapshot == 0) {
        return;
    }
    for (size_t i = 0u; i < snapshot->count; ++i) {
        free(snapshot->entries[i].name);
    }
    free(snapshot->entries);
    memset(snapshot, 0, sizeof(*snapshot));
}

bool winxterm_dstcmd_dir_snapshot_clone(const WinxtermDstcmdDirSnapshot *snapshot,
                                        WinxtermDstcmdDirSnapshot *out)
{
    if (snapshot == 0 || out == 0) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!snapshot->valid) {
        return true;
    }
    if (wcsncpy_s(out->path, WINXTERM_DSTCMD_PATH_CAPACITY, snapshot->path, _TRUNCATE) != 0) {
        return false;
    }
    if (snapshot->count != 0u) {
        out->entries = (WinxtermDstcmdDirEntry *)calloc(snapshot->count, sizeof(*out->entries));
        if (out->entries == 0) {
            return false;
        }
        for (size_t i = 0u; i < snapshot->count; ++i) {
            out->entries[i] = snapshot->entries[i];
            out->entries[i].name = winxterm_dstcmd_wcsdup(snapshot->entries[i].name);
            if (out->entries[i].name == 0) {
                out->count = i;
                winxterm_dstcmd_dir_snapshot_dispose(out);
                return false;
            }
        }
    }
    out->count = snapshot->count;
    out->valid = snapshot->valid;
    return true;
}

static bool winxterm_dstcmd_shell_dir_cache_init(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    memset(&shell->dir_cache, 0, sizeof(shell->dir_cache));
    InitializeCriticalSection(&shell->dir_cache.lock);
    shell->dir_cache.lock_initialized = true;
    return true;
}

static void winxterm_dstcmd_shell_dir_cache_dispose(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || !shell->dir_cache.lock_initialized) {
        return;
    }
    EnterCriticalSection(&shell->dir_cache.lock);
    winxterm_dstcmd_dir_snapshot_dispose(&shell->dir_cache.snapshot);
    LeaveCriticalSection(&shell->dir_cache.lock);
    DeleteCriticalSection(&shell->dir_cache.lock);
    memset(&shell->dir_cache, 0, sizeof(shell->dir_cache));
}

bool winxterm_dstcmd_shell_copy_dir_cache(WinxtermDstcmdShell *shell,
                                          WinxtermDstcmdDirSnapshot *out)
{
    if (shell == 0 || out == 0 || !shell->dir_cache.lock_initialized) {
        return false;
    }
    EnterCriticalSection(&shell->dir_cache.lock);
    bool ok = shell->dir_cache.snapshot.valid &&
              wcscmp(shell->dir_cache.snapshot.path, shell->cwd) == 0 &&
              winxterm_dstcmd_dir_snapshot_clone(&shell->dir_cache.snapshot, out);
    LeaveCriticalSection(&shell->dir_cache.lock);
    return ok;
}

void winxterm_dstcmd_shell_invalidate_dir_cache(WinxtermDstcmdShell *shell)
{
    if (shell == 0 || !shell->dir_cache.lock_initialized) {
        return;
    }
    EnterCriticalSection(&shell->dir_cache.lock);
    winxterm_dstcmd_dir_snapshot_dispose(&shell->dir_cache.snapshot);
    LeaveCriticalSection(&shell->dir_cache.lock);
}

static bool winxterm_dstcmd_shell_publish_dir_cache(WinxtermDstcmdShell *shell,
                                                   WinxtermDstcmdDirSnapshot *snapshot)
{
    if (shell == 0 || snapshot == 0 || !snapshot->valid || !shell->dir_cache.lock_initialized) {
        return false;
    }
    EnterCriticalSection(&shell->dir_cache.lock);
    bool current = wcscmp(shell->cwd, snapshot->path) == 0;
    if (current) {
        winxterm_dstcmd_dir_snapshot_dispose(&shell->dir_cache.snapshot);
        shell->dir_cache.snapshot = *snapshot;
        memset(snapshot, 0, sizeof(*snapshot));
    }
    LeaveCriticalSection(&shell->dir_cache.lock);
    return current;
}

static bool winxterm_dstcmd_dir_entry_same_metadata(const WinxtermDstcmdDirEntry *a,
                                                    const WinxtermDstcmdDirEntry *b)
{
    return a != 0 &&
           b != 0 &&
           a->attributes == b->attributes &&
           a->size == b->size &&
           CompareFileTime(&a->write_time, &b->write_time) == 0;
}

static bool winxterm_dstcmd_dir_snapshots_equal(const WinxtermDstcmdDirSnapshot *baseline,
                                                const WinxtermDstcmdDirSnapshot *fresh)
{
    if (baseline == 0 || fresh == 0 || !baseline->valid || !fresh->valid ||
        baseline->count != fresh->count) {
        return false;
    }
    for (size_t i = 0u; i < baseline->count; ++i) {
        const wchar_t *left_name = baseline->entries[i].name != 0 ? baseline->entries[i].name : L"";
        const wchar_t *right_name = fresh->entries[i].name != 0 ? fresh->entries[i].name : L"";
        if (_wcsicmp(left_name, right_name) != 0 ||
            !winxterm_dstcmd_dir_entry_same_metadata(&baseline->entries[i], &fresh->entries[i])) {
            return false;
        }
    }
    return true;
}

static bool winxterm_dstcmd_dir_append_change(wchar_t **message,
                                              size_t *length,
                                              const wchar_t *kind,
                                              const wchar_t *name)
{
    if (message == 0 || length == 0 || kind == 0 || name == 0) {
        return false;
    }
    int needed = _scwprintf(L"%ls%ls: %ls,\r\n", *message != 0 ? *message : L"", kind, name);
    if (needed < 0) {
        return false;
    }
    wchar_t *next = (wchar_t *)malloc(((size_t)needed + 1u) * sizeof(*next));
    if (next == 0) {
        return false;
    }
    if (swprintf_s(next,
                   (size_t)needed + 1u,
                   L"%ls%ls: %ls,\r\n",
                   *message != 0 ? *message : L"",
                   kind,
                   name) < 0) {
        free(next);
        return false;
    }
    free(*message);
    *message = next;
    *length = (size_t)needed;
    return true;
}

static wchar_t *winxterm_dstcmd_dir_format_changes(const WinxtermDstcmdDirSnapshot *baseline,
                                                   const WinxtermDstcmdDirSnapshot *fresh)
{
    wchar_t *changes = 0;
    size_t length = 0u;
    size_t left = 0u;
    size_t right = 0u;
    while (baseline != 0 && fresh != 0 && (left < baseline->count || right < fresh->count)) {
        const wchar_t *left_name =
            left < baseline->count && baseline->entries[left].name != 0 ? baseline->entries[left].name : L"";
        const wchar_t *right_name =
            right < fresh->count && fresh->entries[right].name != 0 ? fresh->entries[right].name : L"";
        int compare = 0;
        if (left >= baseline->count) {
            compare = 1;
        } else if (right >= fresh->count) {
            compare = -1;
        } else {
            compare = _wcsicmp(left_name, right_name);
        }

        if (compare < 0) {
            if (!winxterm_dstcmd_dir_append_change(&changes, &length, L"removed", left_name)) {
                break;
            }
            ++left;
        } else if (compare > 0) {
            if (!winxterm_dstcmd_dir_append_change(&changes, &length, L"added", right_name)) {
                break;
            }
            ++right;
        } else {
            if (!winxterm_dstcmd_dir_entry_same_metadata(&baseline->entries[left], &fresh->entries[right]) &&
                !winxterm_dstcmd_dir_append_change(&changes, &length, L"modified", right_name)) {
                break;
            }
            ++left;
            ++right;
        }
    }
    (void)length;
    return changes;
}

static void winxterm_dstcmd_dir_refresh_job_run(void *context)
{
    WinxtermDstcmdDirRefreshJob *job = (WinxtermDstcmdDirRefreshJob *)context;
    if (job == 0 || job->shell == 0) {
        return;
    }

    WinxtermDstcmdDirSnapshot *fresh =
        (WinxtermDstcmdDirSnapshot *)calloc(1u, sizeof(*fresh));
    if (fresh == 0 || !winxterm_dstcmd_dir_read(&job->scratch, job->path, fresh)) {
        if (job->source == WINXTERM_DSTCMD_DIR_REFRESH_LS) {
            (void)winxterm_dstcmd_shell_notify_async_widef(job->shell,
                                                           L"ls: background directory read failed: %ls",
                                                           job->path);
        }
        free(fresh);
        return;
    }

    bool changed = job->source == WINXTERM_DSTCMD_DIR_REFRESH_LS &&
                   job->baseline.valid &&
                   wcscmp(job->baseline.path, fresh->path) == 0 &&
                   !winxterm_dstcmd_dir_snapshots_equal(&job->baseline, fresh);
    wchar_t *changes = changed ? winxterm_dstcmd_dir_format_changes(&job->baseline, fresh) : 0;
    bool published = winxterm_dstcmd_shell_publish_dir_cache(job->shell, fresh);
    if (published && changes != 0) {
        (void)winxterm_dstcmd_shell_notify_async_output_widef(job->shell,
                                                              L"directory content changed:\r\n%ls",
                                                              changes);
    }
    free(changes);
    winxterm_dstcmd_dir_snapshot_dispose(fresh);
    free(fresh);
}

static void winxterm_dstcmd_dir_refresh_job_cleanup(void *context)
{
    WinxtermDstcmdDirRefreshJob *job = (WinxtermDstcmdDirRefreshJob *)context;
    if (job == 0) {
        return;
    }
    winxterm_dstcmd_dir_snapshot_dispose(&job->baseline);
    winxterm_dstcmd_scratch_dispose(&job->scratch);
    free(job);
}

bool winxterm_dstcmd_shell_schedule_dir_cache_refresh(WinxtermDstcmdShell *shell,
                                                      WinxtermDstcmdDirRefreshSource source)
{
    if (shell == 0) {
        return false;
    }
    WinxtermDstcmdDirRefreshJob *job =
        (WinxtermDstcmdDirRefreshJob *)calloc(1u, sizeof(*job));
    if (job == 0) {
        return false;
    }
    job->shell = shell;
    job->source = source;
    winxterm_dstcmd_scratch_init(&job->scratch);
    if (shell->dir_cache.lock_initialized) {
        EnterCriticalSection(&shell->dir_cache.lock);
        wcsncpy_s(job->path, WINXTERM_DSTCMD_PATH_CAPACITY, shell->cwd, _TRUNCATE);
        if (source == WINXTERM_DSTCMD_DIR_REFRESH_LS &&
            shell->dir_cache.snapshot.valid &&
            wcscmp(shell->dir_cache.snapshot.path, shell->cwd) == 0) {
            (void)winxterm_dstcmd_dir_snapshot_clone(&shell->dir_cache.snapshot, &job->baseline);
        }
        LeaveCriticalSection(&shell->dir_cache.lock);
    } else {
        wcsncpy_s(job->path, WINXTERM_DSTCMD_PATH_CAPACITY, shell->cwd, _TRUNCATE);
    }
    if (!winxterm_dstcmd_job_pool_submit(&shell->jobs,
                                         winxterm_dstcmd_dir_refresh_job_run,
                                         winxterm_dstcmd_dir_refresh_job_cleanup,
                                         job)) {
        winxterm_dstcmd_dir_refresh_job_cleanup(job);
        return false;
    }
    return true;
}

bool winxterm_dstcmd_shell_init_dir_cache(WinxtermDstcmdShell *shell)
{
    return winxterm_dstcmd_shell_dir_cache_init(shell);
}

void winxterm_dstcmd_shell_dispose_dir_cache(WinxtermDstcmdShell *shell)
{
    winxterm_dstcmd_shell_dir_cache_dispose(shell);
}
