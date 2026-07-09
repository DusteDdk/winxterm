#include "dstcmd/commands/rm.h"

#include "dstcmd/api/path.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define WINXTERM_DSTCMD_RM_ASYNC_PREFIX L"._winxterm_rm_"
#define WINXTERM_DSTCMD_RM_ASYNC_RENAME_ATTEMPTS 64u

static const wchar_t *winxterm_dstcmd_display_path(const wchar_t *path,
                                                   wchar_t *buffer,
                                                   size_t buffer_count)
{
    return winxterm_dstcmd_path_to_display(path, buffer, buffer_count) ? buffer : path;
}

static void winxterm_dstcmd_rm_write_error(WinxtermDstcmdShell *shell,
                                           const wchar_t *operation,
                                           const wchar_t *path,
                                           DWORD error)
{
    wchar_t message[512];
    DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                  0,
                                  error,
                                  0,
                                  message,
                                  (DWORD)(sizeof(message) / sizeof(message[0])),
                                  0);
    while (length != 0u && (message[length - 1u] == L'\n' || message[length - 1u] == L'\r')) {
        message[--length] = L'\0';
    }
    if (length == 0u) {
        (void)winxterm_dstcmd_shell_write_widef(shell,
                                                L"rm: %ls '%ls' failed: Windows error %lu\r\n",
                                                operation,
                                                path,
                                                (unsigned long)error);
        return;
    }
    (void)winxterm_dstcmd_shell_write_widef(shell,
                                            L"rm: %ls '%ls' failed: %ls (error %lu)\r\n",
                                            operation,
                                            path,
                                            message,
                                            (unsigned long)error);
}

static bool winxterm_dstcmd_parse_rm_options(const WinxtermDstcmdArgv *argv,
                                             int *operand_start,
                                             bool *recursive,
                                             bool *force)
{
    *operand_start = 1;
    *recursive = false;
    *force = false;
    for (int i = 1; i < argv->count; ++i) {
        const wchar_t *arg = argv->items[i];
        if (arg == 0 || arg[0] != L'-' || arg[1] == L'\0') {
            *operand_start = i;
            return true;
        }
        if (wcscmp(arg, L"--") == 0) {
            *operand_start = i + 1;
            return true;
        }
        for (int j = 1; arg[j] != L'\0'; ++j) {
            if (arg[j] == L'f') {
                *force = true;
            } else if (arg[j] == L'r' || arg[j] == L'R') {
                *recursive = true;
            } else {
                return false;
            }
        }
    }
    *operand_start = argv->count;
    return true;
}

typedef struct WinxtermDstcmdRmEntry {
    wchar_t *path;
    DWORD attributes;
    bool visited;
} WinxtermDstcmdRmEntry;

typedef struct WinxtermDstcmdRmStack {
    WinxtermDstcmdRmEntry *items;
    size_t count;
    size_t capacity;
} WinxtermDstcmdRmStack;

typedef struct WinxtermDstcmdRmFailure {
    const wchar_t *operation;
    wchar_t *native_path;
    DWORD error;
} WinxtermDstcmdRmFailure;

typedef struct WinxtermDstcmdRmAsyncContext {
    WinxtermDstcmdShell *shell;
    wchar_t *hidden_native_path;
    wchar_t *hidden_display_path;
    DWORD attributes;
    bool force;
} WinxtermDstcmdRmAsyncContext;

static bool winxterm_dstcmd_rm_is_slash(wchar_t ch)
{
    return ch == L'\\' || ch == L'/';
}

static wchar_t *winxterm_dstcmd_rm_wcsdup(const wchar_t *text)
{
    if (text == 0) {
        return 0;
    }
    size_t length = wcslen(text);
    wchar_t *copy = (wchar_t *)malloc((length + 1u) * sizeof(*copy));
    if (copy == 0) {
        return 0;
    }
    memcpy(copy, text, (length + 1u) * sizeof(*copy));
    return copy;
}

static wchar_t *winxterm_dstcmd_rm_path_to_native_alloc(const wchar_t *path)
{
    size_t length = wcslen(path);
    if (length >= WINXTERM_DSTCMD_PATH_CAPACITY) {
        return 0;
    }
    wchar_t *native_path = (wchar_t *)malloc((length + 1u) * sizeof(*native_path));
    if (native_path == 0) {
        return 0;
    }
    if (!winxterm_dstcmd_path_to_native(path, native_path, length + 1u)) {
        free(native_path);
        return 0;
    }
    return native_path;
}

static wchar_t *winxterm_dstcmd_rm_path_to_display_alloc(const wchar_t *path)
{
    size_t length = wcslen(path);
    if (length >= WINXTERM_DSTCMD_PATH_CAPACITY) {
        return 0;
    }
    wchar_t *display_path = (wchar_t *)malloc((length + 1u) * sizeof(*display_path));
    if (display_path == 0) {
        return 0;
    }
    if (!winxterm_dstcmd_path_to_display(path, display_path, length + 1u)) {
        free(display_path);
        return 0;
    }
    return display_path;
}

static void winxterm_dstcmd_rm_failure_dispose(WinxtermDstcmdRmFailure *failure)
{
    if (failure == 0) {
        return;
    }
    free(failure->native_path);
    failure->native_path = 0;
    failure->operation = 0;
    failure->error = ERROR_SUCCESS;
}

static void winxterm_dstcmd_rm_failure_set(WinxtermDstcmdRmFailure *failure,
                                           const wchar_t *operation,
                                           const wchar_t *native_path,
                                           DWORD error)
{
    if (failure == 0 || failure->operation != 0) {
        return;
    }
    failure->operation = operation;
    failure->native_path = winxterm_dstcmd_rm_wcsdup(native_path);
    failure->error = error;
}

static void winxterm_dstcmd_rm_write_native_error(WinxtermDstcmdShell *shell,
                                                  const wchar_t *operation,
                                                  const wchar_t *native_path,
                                                  DWORD error)
{
    wchar_t *display_path = winxterm_dstcmd_rm_path_to_display_alloc(native_path);
    winxterm_dstcmd_rm_write_error(shell,
                                   operation,
                                   display_path != 0 ? display_path : native_path,
                                   error);
    free(display_path);
}

static void winxterm_dstcmd_rm_write_path_error(WinxtermDstcmdShell *shell,
                                                const wchar_t *operation,
                                                const wchar_t *native_path,
                                                const wchar_t *display_path,
                                                DWORD error)
{
    if (display_path != 0) {
        winxterm_dstcmd_rm_write_error(shell, operation, display_path, error);
    } else {
        winxterm_dstcmd_rm_write_native_error(shell, operation, native_path, error);
    }
}

static void winxterm_dstcmd_rm_write_native_child_too_long(WinxtermDstcmdShell *shell,
                                                           const wchar_t *native_path)
{
    wchar_t *display_path = winxterm_dstcmd_rm_path_to_display_alloc(native_path);
    (void)winxterm_dstcmd_shell_write_widef(shell,
                                            L"rm: enumerate directory '%ls' failed: child path too long\r\n",
                                            display_path != 0 ? display_path : native_path);
    free(display_path);
}

static wchar_t *winxterm_dstcmd_rm_join_native_child_alloc(const wchar_t *directory,
                                                           const wchar_t *name)
{
    size_t directory_length = wcslen(directory);
    size_t name_length = wcslen(name);
    bool need_slash = directory_length != 0u &&
                      !winxterm_dstcmd_rm_is_slash(directory[directory_length - 1u]);
    if (directory_length + (need_slash ? 1u : 0u) + name_length >= WINXTERM_DSTCMD_PATH_CAPACITY) {
        return 0;
    }
    wchar_t *path = (wchar_t *)malloc((directory_length + (need_slash ? 1u : 0u) + name_length + 1u) *
                                      sizeof(*path));
    if (path == 0) {
        return 0;
    }
    memcpy(path, directory, directory_length * sizeof(*path));
    size_t offset = directory_length;
    if (need_slash) {
        path[offset++] = L'\\';
    }
    memcpy(path + offset, name, (name_length + 1u) * sizeof(*path));
    return path;
}

static bool winxterm_dstcmd_rm_stack_push(WinxtermDstcmdRmStack *stack,
                                          wchar_t *path,
                                          DWORD attributes,
                                          bool visited)
{
    if (stack->count == stack->capacity) {
        size_t new_capacity = stack->capacity == 0u ? 64u : stack->capacity * 2u;
        if (new_capacity <= stack->capacity ||
            new_capacity > ((size_t)-1) / sizeof(stack->items[0])) {
            return false;
        }
        WinxtermDstcmdRmEntry *items =
            (WinxtermDstcmdRmEntry *)realloc(stack->items, new_capacity * sizeof(stack->items[0]));
        if (items == 0) {
            return false;
        }
        stack->items = items;
        stack->capacity = new_capacity;
    }
    stack->items[stack->count].path = path;
    stack->items[stack->count].attributes = attributes;
    stack->items[stack->count].visited = visited;
    ++stack->count;
    return true;
}

static void winxterm_dstcmd_rm_stack_pop(WinxtermDstcmdRmStack *stack)
{
    if (stack->count == 0u) {
        return;
    }
    --stack->count;
    free(stack->items[stack->count].path);
}

static void winxterm_dstcmd_rm_stack_dispose(WinxtermDstcmdRmStack *stack)
{
    while (stack->count != 0u) {
        winxterm_dstcmd_rm_stack_pop(stack);
    }
    free(stack->items);
    stack->items = 0;
    stack->capacity = 0u;
}

static wchar_t *winxterm_dstcmd_rm_make_async_path_alloc(const wchar_t *native_path)
{
    const wchar_t *last_slash = 0;
    for (const wchar_t *p = native_path; p != 0 && *p != L'\0'; ++p) {
        if (winxterm_dstcmd_rm_is_slash(*p)) {
            last_slash = p;
        }
    }
    if (last_slash == 0 || last_slash[1] == L'\0') {
        return 0;
    }

    size_t parent_length = (size_t)(last_slash - native_path) + 1u;
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    DWORD seed = (DWORD)counter.LowPart ^ GetTickCount() ^ GetCurrentProcessId() ^ GetCurrentThreadId();
    WinxtermDstcmdScratch scratch;
    winxterm_dstcmd_scratch_init(&scratch);

    for (unsigned int attempt = 0u; attempt < WINXTERM_DSTCMD_RM_ASYNC_RENAME_ATTEMPTS; ++attempt) {
        wchar_t name[64];
        int name_length = _snwprintf_s(name,
                                       sizeof(name) / sizeof(name[0]),
                                       _TRUNCATE,
                                       L"%ls%lu",
                                       WINXTERM_DSTCMD_RM_ASYNC_PREFIX,
                                       (unsigned long)(seed + attempt * 2654435761u));
        if (name_length < 0) {
            winxterm_dstcmd_scratch_dispose(&scratch);
            return 0;
        }
        if (parent_length + (size_t)name_length >= WINXTERM_DSTCMD_PATH_CAPACITY) {
            winxterm_dstcmd_scratch_dispose(&scratch);
            return 0;
        }
        wchar_t *candidate = (wchar_t *)malloc((parent_length + (size_t)name_length + 1u) * sizeof(*candidate));
        if (candidate == 0) {
            winxterm_dstcmd_scratch_dispose(&scratch);
            return 0;
        }
        memcpy(candidate, native_path, parent_length * sizeof(*candidate));
        memcpy(candidate + parent_length, name, ((size_t)name_length + 1u) * sizeof(*candidate));
        WinxtermDstcmdPathInfo info;
        if (!winxterm_dstcmd_path_get_info_scratch(&scratch, candidate, &info) &&
            (info.error == ERROR_FILE_NOT_FOUND || info.error == ERROR_PATH_NOT_FOUND)) {
            winxterm_dstcmd_scratch_dispose(&scratch);
            return candidate;
        }
        free(candidate);
    }
    winxterm_dstcmd_scratch_dispose(&scratch);
    return 0;
}

static wchar_t *winxterm_dstcmd_rm_format_system_error_alloc(DWORD error)
{
    wchar_t message[512];
    DWORD length = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                                  0,
                                  error,
                                  0,
                                  message,
                                  (DWORD)(sizeof(message) / sizeof(message[0])),
                                  0);
    while (length != 0u && (message[length - 1u] == L'\n' || message[length - 1u] == L'\r')) {
        message[--length] = L'\0';
    }
    if (length == 0u) {
        int needed = _scwprintf(L"Windows error %lu", (unsigned long)error);
        if (needed < 0) {
            return 0;
        }
        wchar_t *fallback = (wchar_t *)malloc(((size_t)needed + 1u) * sizeof(*fallback));
        if (fallback == 0) {
            return 0;
        }
        if (swprintf_s(fallback, (size_t)needed + 1u, L"Windows error %lu", (unsigned long)error) < 0) {
            free(fallback);
            return 0;
        }
        return fallback;
    }
    return winxterm_dstcmd_rm_wcsdup(message);
}

static int winxterm_dstcmd_rm_delete_file_native(WinxtermDstcmdShell *shell,
                                                 WinxtermDstcmdScratch *scratch,
                                                 const wchar_t *native_path,
                                                 const wchar_t *display_path,
                                                 bool force,
                                                 WinxtermDstcmdRmFailure *failure)
{
    DWORD attribute_error = ERROR_SUCCESS;
    if (force) {
        if (!winxterm_dstcmd_path_set_attributes_scratch(scratch, native_path, FILE_ATTRIBUTE_NORMAL)) {
            attribute_error = GetLastError();
        }
    }
    if (winxterm_dstcmd_path_delete_file_scratch(scratch, native_path)) {
        return 0;
    }
    DWORD delete_error = GetLastError();
    if (failure != 0) {
        winxterm_dstcmd_rm_failure_set(failure,
                                       attribute_error != ERROR_SUCCESS ? L"clear attributes" : L"delete file",
                                       native_path,
                                       attribute_error != ERROR_SUCCESS ? attribute_error : delete_error);
        return 1;
    }
    if (attribute_error != ERROR_SUCCESS) {
        winxterm_dstcmd_rm_write_path_error(shell,
                                            L"clear attributes",
                                            native_path,
                                            display_path,
                                            attribute_error);
    }
    winxterm_dstcmd_rm_write_path_error(shell,
                                        L"delete file",
                                        native_path,
                                        display_path,
                                        delete_error);
    return 1;
}

static int winxterm_dstcmd_rm_remove_directory_native(WinxtermDstcmdShell *shell,
                                                      WinxtermDstcmdScratch *scratch,
                                                      const wchar_t *native_path,
                                                      const wchar_t *display_path,
                                                      bool force,
                                                      WinxtermDstcmdRmFailure *failure)
{
    DWORD attribute_error = ERROR_SUCCESS;
    if (force) {
        if (!winxterm_dstcmd_path_set_attributes_scratch(scratch, native_path, FILE_ATTRIBUTE_NORMAL)) {
            attribute_error = GetLastError();
        }
    }
    if (winxterm_dstcmd_path_remove_directory_scratch(scratch, native_path)) {
        return 0;
    }
    DWORD remove_error = GetLastError();
    if (failure != 0) {
        winxterm_dstcmd_rm_failure_set(failure,
                                       attribute_error != ERROR_SUCCESS ? L"clear attributes" : L"remove directory",
                                       native_path,
                                       attribute_error != ERROR_SUCCESS ? attribute_error : remove_error);
        return 1;
    }
    if (attribute_error != ERROR_SUCCESS) {
        winxterm_dstcmd_rm_write_path_error(shell,
                                            L"clear attributes",
                                            native_path,
                                            display_path,
                                            attribute_error);
    }
    winxterm_dstcmd_rm_write_path_error(shell,
                                        L"remove directory",
                                        native_path,
                                        display_path,
                                        remove_error);
    return 1;
}

static int winxterm_dstcmd_rm_remove_tree_native(WinxtermDstcmdShell *shell,
                                                 wchar_t *native_path,
                                                 DWORD attributes,
                                                 bool force,
                                                 WinxtermDstcmdRmFailure *failure)
{
    WinxtermDstcmdRmStack stack = {0};
    if (!winxterm_dstcmd_rm_stack_push(&stack, native_path, attributes, false)) {
        free(native_path);
        (void)winxterm_dstcmd_shell_write_wide(shell, L"rm: out of memory\r\n");
        return 1;
    }

    int status = 0;
    WinxtermDstcmdScratch scratch;
    winxterm_dstcmd_scratch_init(&scratch);
    while (stack.count != 0u) {
        WinxtermDstcmdRmEntry *entry = &stack.items[stack.count - 1u];
        if (entry->visited || (entry->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0u) {
            if (winxterm_dstcmd_rm_remove_directory_native(shell,
                                                           &scratch,
                                                           entry->path,
                                                           0,
                                                           force,
                                                           failure) != 0) {
                status = 1;
                if (failure != 0) {
                    break;
                }
            }
            winxterm_dstcmd_rm_stack_pop(&stack);
            continue;
        }

        entry->visited = true;
        const wchar_t *parent_path = entry->path;
        WinxtermDstcmdDirIter iter;
        if (!winxterm_dstcmd_dir_iter_open_scratch(&scratch, parent_path, &iter)) {
            DWORD error = iter.error;
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_NO_MORE_FILES) {
                winxterm_dstcmd_rm_failure_set(failure, L"enumerate directory", parent_path, error);
                winxterm_dstcmd_rm_write_native_error(shell, L"enumerate directory", parent_path, error);
                status = 1;
                if (failure != 0) {
                    break;
                }
            }
            continue;
        }

        bool stopped_early = false;
        const WIN32_FIND_DATAW *data = 0;
        while (winxterm_dstcmd_dir_iter_next(&iter, &data)) {
            if (wcscmp(data->cFileName, L".") == 0 || wcscmp(data->cFileName, L"..") == 0) {
                continue;
            }

            wchar_t *child = winxterm_dstcmd_rm_join_native_child_alloc(parent_path, data->cFileName);
            if (child == 0) {
                winxterm_dstcmd_rm_failure_set(failure,
                                               L"enumerate directory",
                                               parent_path,
                                               ERROR_FILENAME_EXCED_RANGE);
                winxterm_dstcmd_rm_write_native_child_too_long(shell, parent_path);
                status = 1;
                stopped_early = true;
                break;
            }

            if ((data->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
                if (!winxterm_dstcmd_rm_stack_push(&stack, child, data->dwFileAttributes, false)) {
                    winxterm_dstcmd_rm_failure_set(failure, L"queue directory", child, ERROR_NOT_ENOUGH_MEMORY);
                    free(child);
                    (void)winxterm_dstcmd_shell_write_wide(shell, L"rm: out of memory\r\n");
                    status = 1;
                    stopped_early = true;
                    break;
                }
            } else {
                if (winxterm_dstcmd_rm_delete_file_native(shell, &scratch, child, 0, force, failure) != 0) {
                    status = 1;
                    if (failure != 0) {
                        free(child);
                        stopped_early = true;
                        break;
                    }
                }
                free(child);
            }
        }

        DWORD find_error = iter.error;
        winxterm_dstcmd_dir_iter_close(&iter);
        if (failure != 0 && failure->operation != 0) {
            break;
        }
        if (!stopped_early && find_error != ERROR_NO_MORE_FILES) {
            winxterm_dstcmd_rm_failure_set(failure, L"enumerate directory", parent_path, find_error);
            winxterm_dstcmd_rm_write_native_error(shell, L"enumerate directory", parent_path, find_error);
            status = 1;
            if (failure != 0) {
                break;
            }
        }
    }

    winxterm_dstcmd_scratch_dispose(&scratch);
    winxterm_dstcmd_rm_stack_dispose(&stack);
    return status;
}

static void winxterm_dstcmd_rm_report_async_failure(WinxtermDstcmdRmAsyncContext *context,
                                                    WinxtermDstcmdRmFailure *failure)
{
    if (context == 0 || context->shell == 0 || failure == 0 || failure->operation == 0) {
        return;
    }

    WinxtermDstcmdScratch scratch;
    winxterm_dstcmd_scratch_init(&scratch);
    (void)winxterm_dstcmd_path_set_attributes_scratch(&scratch,
                                                      context->hidden_native_path,
                                                      FILE_ATTRIBUTE_NORMAL);
    winxterm_dstcmd_scratch_dispose(&scratch);

    wchar_t *system_error = winxterm_dstcmd_rm_format_system_error_alloc(failure->error);
    wchar_t *failure_display_path =
        failure->native_path != 0 ? winxterm_dstcmd_rm_path_to_display_alloc(failure->native_path) : 0;
    const wchar_t *failure_path = failure_display_path != 0 ? failure_display_path : context->hidden_display_path;
    const wchar_t *error_text = system_error != 0 ? system_error : L"unknown error";
    (void)winxterm_dstcmd_shell_notify_async_widef(
        context->shell,
        L"rm: background delete of '%ls' stopped while trying to %ls '%ls': %ls (error %lu). "
        L"Temporary directory '%ls' was marked visible for manual cleanup.",
        context->hidden_display_path,
        failure->operation,
        failure_path,
        error_text,
        (unsigned long)failure->error,
        context->hidden_display_path);
    free(failure_display_path);
    free(system_error);
}

static void winxterm_dstcmd_rm_async_job(void *context)
{
    WinxtermDstcmdRmAsyncContext *rm_context = (WinxtermDstcmdRmAsyncContext *)context;
    if (rm_context != 0) {
        WinxtermDstcmdRmFailure failure = {0};
        wchar_t *delete_path = winxterm_dstcmd_rm_wcsdup(rm_context->hidden_native_path);
        if (delete_path == 0) {
            winxterm_dstcmd_rm_failure_set(&failure,
                                           L"queue directory",
                                           rm_context->hidden_native_path,
                                           ERROR_NOT_ENOUGH_MEMORY);
        } else if (winxterm_dstcmd_rm_remove_tree_native(0,
                                                         delete_path,
                                                         rm_context->attributes,
                                                         rm_context->force,
                                                         &failure) != 0) {
            winxterm_dstcmd_rm_report_async_failure(rm_context, &failure);
        }
        if (failure.operation != 0 && delete_path == 0) {
            winxterm_dstcmd_rm_report_async_failure(rm_context, &failure);
        }
        winxterm_dstcmd_rm_failure_dispose(&failure);
    }
}

static void winxterm_dstcmd_rm_async_context_free(void *context)
{
    WinxtermDstcmdRmAsyncContext *rm_context = (WinxtermDstcmdRmAsyncContext *)context;
    if (rm_context == 0) {
        return;
    }
    free(rm_context->hidden_native_path);
    free(rm_context->hidden_display_path);
    free(rm_context);
}

static int winxterm_dstcmd_rm_start_async_directory(WinxtermDstcmdShell *shell,
                                                    wchar_t *native_path,
                                                    const wchar_t *display_path,
                                                    DWORD attributes,
                                                    bool force)
{
    wchar_t *hidden_native_path = winxterm_dstcmd_rm_make_async_path_alloc(native_path);
    if (hidden_native_path == 0) {
        (void)winxterm_dstcmd_shell_write_widef(shell,
                                                L"rm: rename '%ls' for background delete failed: path too long\r\n",
                                                display_path);
        free(native_path);
        return 1;
    }

    if (!winxterm_dstcmd_path_move_file_scratch(&shell->scratch, native_path, hidden_native_path, 0)) {
        winxterm_dstcmd_rm_write_error(shell, L"rename for background delete", display_path, GetLastError());
        free(hidden_native_path);
        free(native_path);
        return 1;
    }

    if (!winxterm_dstcmd_path_set_attributes_scratch(&shell->scratch,
                                                     hidden_native_path,
                                                     FILE_ATTRIBUTE_HIDDEN)) {
        DWORD error = GetLastError();
        (void)winxterm_dstcmd_path_move_file_scratch(&shell->scratch, hidden_native_path, native_path, 0);
        winxterm_dstcmd_rm_write_error(shell, L"hide background delete directory", display_path, error);
        free(hidden_native_path);
        free(native_path);
        return 1;
    }
    free(native_path);

    wchar_t *hidden_display_path = winxterm_dstcmd_rm_path_to_display_alloc(hidden_native_path);
    if (hidden_display_path == 0) {
        (void)winxterm_dstcmd_path_set_attributes_scratch(&shell->scratch,
                                                          hidden_native_path,
                                                          FILE_ATTRIBUTE_NORMAL);
        (void)winxterm_dstcmd_shell_write_wide(shell, L"rm: background delete directory path too long\r\n");
        free(hidden_native_path);
        return 1;
    }

    WinxtermDstcmdRmAsyncContext *context =
        (WinxtermDstcmdRmAsyncContext *)calloc(1u, sizeof(*context));
    if (context == 0) {
        (void)winxterm_dstcmd_path_set_attributes_scratch(&shell->scratch,
                                                          hidden_native_path,
                                                          FILE_ATTRIBUTE_NORMAL);
        (void)winxterm_dstcmd_shell_write_widef(shell,
                                                L"rm: failed to start background delete for '%ls'; temporary directory '%ls' was marked visible\r\n",
                                                display_path,
                                                hidden_display_path);
        free(context);
        free(hidden_display_path);
        free(hidden_native_path);
        return 1;
    }

    context->shell = shell;
    context->hidden_native_path = hidden_native_path;
    context->hidden_display_path = hidden_display_path;
    context->attributes = attributes;
    context->force = force;

    if (!winxterm_dstcmd_job_pool_submit(&shell->jobs,
                                         winxterm_dstcmd_rm_async_job,
                                         winxterm_dstcmd_rm_async_context_free,
                                         context)) {
        (void)winxterm_dstcmd_path_set_attributes_scratch(&shell->scratch,
                                                          hidden_native_path,
                                                          FILE_ATTRIBUTE_NORMAL);
        (void)winxterm_dstcmd_shell_write_widef(shell,
                                                L"rm: failed to start background delete for '%ls'; temporary directory '%ls' was marked visible\r\n",
                                                display_path,
                                                hidden_display_path);
        winxterm_dstcmd_rm_async_context_free(context);
        return 1;
    }
    return 0;
}

static int winxterm_dstcmd_remove_path(WinxtermDstcmdShell *shell,
                                       const wchar_t *path,
                                       bool recursive,
                                       bool force)
{
    wchar_t *native_path = winxterm_dstcmd_rm_path_to_native_alloc(path);
    if (native_path == 0) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"rm: path too long\r\n");
        return 1;
    }
    WinxtermDstcmdPathInfo info;
    if (!winxterm_dstcmd_path_get_info_scratch(&shell->scratch, native_path, &info)) {
        DWORD error = info.error;
        if (force && (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)) {
            free(native_path);
            return 0;
        }
        winxterm_dstcmd_rm_write_error(shell, L"stat", path, error);
        free(native_path);
        return 1;
    }
    if ((info.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
        WinxtermDstcmdScratch scratch;
        winxterm_dstcmd_scratch_init(&scratch);
        int status = winxterm_dstcmd_rm_delete_file_native(shell, &scratch, native_path, path, force, 0);
        winxterm_dstcmd_scratch_dispose(&scratch);
        free(native_path);
        return status;
    }
    if (!recursive) {
        (void)winxterm_dstcmd_shell_write_widef(shell, L"rm: cannot remove '%ls': is a directory\r\n", path);
        free(native_path);
        return 1;
    }
    if (shell != 0) {
        return winxterm_dstcmd_rm_start_async_directory(shell, native_path, path, info.attributes, force);
    }
    return winxterm_dstcmd_rm_remove_tree_native(shell, native_path, info.attributes, force, 0);
}

int winxterm_dstcmd_cmd_rm(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    int operand_start = 1;
    bool recursive = false;
    bool force = false;
    if (!winxterm_dstcmd_parse_rm_options(argv, &operand_start, &recursive, &force)) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"rm: usage: rm [-rf] path...\r\n");
        return 2;
    }
    if (operand_start >= argv->count) {
        if (!force) {
            (void)winxterm_dstcmd_shell_write_wide(shell, L"rm: missing operand\r\n");
            return 2;
        }
        return 0;
    }
    int status = 0;
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    wchar_t *path = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *display_path = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    if (path == 0 || display_path == 0) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"rm: out of memory\r\n");
        return 1;
    }
    for (int i = operand_start; i < argv->count; ++i) {
        if (!winxterm_dstcmd_path_resolve_scratch(&shell->scratch,
                                                 shell->cwd,
                                                 argv->items[i],
                                                 path,
                                                 WINXTERM_DSTCMD_PATH_CAPACITY)) {
            (void)winxterm_dstcmd_shell_write_widef(shell,
                                                    L"rm: path too long: %ls\r\n",
                                                    winxterm_dstcmd_display_path(argv->items[i],
                                                                                display_path,
                                                                                WINXTERM_DSTCMD_PATH_CAPACITY));
            status = 1;
            continue;
        }
        if (winxterm_dstcmd_remove_path(shell, path, recursive, force) != 0) {
            status = 1;
        }
    }
    winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
    return status;
}
