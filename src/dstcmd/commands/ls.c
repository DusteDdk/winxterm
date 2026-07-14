#include "dstcmd/commands/ls.h"

#include "dstcmd/api/path.h"
#include "dstcmd/api/unicode.h"
#include "dstcmd/winxterm_dstcmd_parse.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

typedef WinxtermDstcmdDirEntry WinxtermDstcmdLsEntry;

static void winxterm_dstcmd_set_error(wchar_t *error, size_t error_count, const wchar_t *message)
{
    if (error == 0 || error_count == 0u) {
        return;
    }
    if (message == 0) {
        error[0] = L'\0';
        return;
    }
    wcsncpy_s(error, error_count, message, _TRUNCATE);
}

bool winxterm_dstcmd_parse_ls_options(const WinxtermDstcmdArgv *argv,
                                      int *operand_start,
                                      WinxtermDstcmdLsOptions *options,
                                      wchar_t *error,
                                      size_t error_count)
{
    if (argv == 0 || operand_start == 0 || options == 0) {
        return false;
    }
    memset(options, 0, sizeof(*options));
    *operand_start = 1;
    winxterm_dstcmd_set_error(error, error_count, L"");
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
            switch (arg[j]) {
            case L'l':
                options->long_format = true;
                break;
            case L't':
                options->sort_time = true;
                break;
            case L'a':
                options->all = true;
                break;
            case L'h':
                options->human_readable = true;
                break;
            default:
                winxterm_dstcmd_set_error(error, error_count, L"ls: unsupported option");
                return false;
            }
        }
    }
    *operand_start = argv->count;
    return true;
}

static int winxterm_dstcmd_compare_name(const void *left, const void *right)
{
    const WinxtermDstcmdLsEntry *a = (const WinxtermDstcmdLsEntry *)left;
    const WinxtermDstcmdLsEntry *b = (const WinxtermDstcmdLsEntry *)right;
    return _wcsicmp(a->name, b->name);
}

static int winxterm_dstcmd_compare_time(const void *left, const void *right)
{
    const WinxtermDstcmdLsEntry *a = (const WinxtermDstcmdLsEntry *)left;
    const WinxtermDstcmdLsEntry *b = (const WinxtermDstcmdLsEntry *)right;
    int time_compare = CompareFileTime(&b->write_time, &a->write_time);
    if (time_compare != 0) {
        return time_compare;
    }
    return _wcsicmp(a->name, b->name);
}

static void winxterm_dstcmd_ls_entries_dispose(WinxtermDstcmdLsEntry *entries, size_t count)
{
    if (entries == 0) {
        return;
    }
    for (size_t i = 0u; i < count; ++i) {
        free(entries[i].name);
    }
    free(entries);
}

static bool winxterm_dstcmd_ls_add_entry(WinxtermDstcmdLsEntry **entries,
                                         size_t *count,
                                         const WIN32_FIND_DATAW *data)
{
    if (entries == 0 || count == 0 || data == 0) {
        return false;
    }
    WinxtermDstcmdLsEntry *new_entries =
        (WinxtermDstcmdLsEntry *)realloc(*entries, (*count + 1u) * sizeof(*new_entries));
    if (new_entries == 0) {
        return false;
    }
    *entries = new_entries;
    WinxtermDstcmdLsEntry *entry = *entries + *count;
    memset(entry, 0, sizeof(*entry));
    entry->name = winxterm_dstcmd_wcsdup(data->cFileName);
    if (entry->name == 0) {
        return false;
    }
    entry->attributes = data->dwFileAttributes;
    entry->size = ((ULONGLONG)data->nFileSizeHigh << 32) | (ULONGLONG)data->nFileSizeLow;
    entry->write_time = data->ftLastWriteTime;
    ++(*count);
    return true;
}

static bool winxterm_dstcmd_ls_should_show(const WIN32_FIND_DATAW *data, bool all)
{
    if (data == 0) {
        return false;
    }
    if (all) {
        return true;
    }
    return data->cFileName[0] != L'.' && (data->dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) == 0u;
}

static bool winxterm_dstcmd_ls_entry_should_show(const WinxtermDstcmdLsEntry *entry, bool all)
{
    if (entry == 0 || entry->name == 0) {
        return false;
    }
    if (all) {
        return true;
    }
    return entry->name[0] != L'.' && (entry->attributes & FILE_ATTRIBUTE_HIDDEN) == 0u;
}

static const wchar_t *winxterm_dstcmd_display_path(const wchar_t *path,
                                                   wchar_t *buffer,
                                                   size_t buffer_count)
{
    return winxterm_dstcmd_path_to_display(path, buffer, buffer_count) ? buffer : path;
}

static void winxterm_dstcmd_format_file_time(FILETIME time, wchar_t *buffer, size_t buffer_count)
{
    if (buffer == 0 || buffer_count == 0u) {
        return;
    }
    FILETIME local_time;
    SYSTEMTIME system_time;
    if (!FileTimeToLocalFileTime(&time, &local_time) ||
        !FileTimeToSystemTime(&local_time, &system_time)) {
        buffer[0] = L'\0';
        return;
    }
    _snwprintf_s(buffer,
                 buffer_count,
                 _TRUNCATE,
                 L"%04u-%02u-%02u %02u:%02u",
                 (unsigned int)system_time.wYear,
                 (unsigned int)system_time.wMonth,
                 (unsigned int)system_time.wDay,
                 (unsigned int)system_time.wHour,
                 (unsigned int)system_time.wMinute);
}

static void winxterm_dstcmd_format_file_size(ULONGLONG size,
                                             bool human_readable,
                                             wchar_t *buffer,
                                             size_t buffer_count)
{
    if (buffer == 0 || buffer_count == 0u) {
        return;
    }
    if (!human_readable) {
        _snwprintf_s(buffer, buffer_count, _TRUNCATE, L"%llu", (unsigned long long)size);
        return;
    }

    static const wchar_t units[] = L"KMGTPE";
    double value = (double)size;
    size_t unit_index = 0u;
    while (value >= 1024.0 && unit_index < sizeof(units) / sizeof(units[0]) - 1u) {
        value /= 1024.0;
        ++unit_index;
    }

    if (unit_index == 0u) {
        _snwprintf_s(buffer, buffer_count, _TRUNCATE, L"%llu", (unsigned long long)size);
    } else if (value < 10.0) {
        _snwprintf_s(buffer, buffer_count, _TRUNCATE, L"%.1f%c", value, units[unit_index - 1u]);
    } else {
        _snwprintf_s(buffer, buffer_count, _TRUNCATE, L"%.0f%c", value, units[unit_index - 1u]);
    }
}

static bool winxterm_dstcmd_ls_uses_wide_format(const WinxtermDstcmdLsOptions *options)
{
    return options != 0 && !options->long_format;
}

static size_t winxterm_dstcmd_ls_terminal_columns(const WinxtermDstcmdShell *shell)
{
    HANDLE output = shell != 0 && shell->output_handle != 0 &&
        shell->output_handle != INVALID_HANDLE_VALUE ?
        shell->output_handle : GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (output != 0 && output != INVALID_HANDLE_VALUE &&
        GetConsoleScreenBufferInfo(output, &info)) {
        int columns = (int)(info.srWindow.Right - info.srWindow.Left + 1);
        if (columns > 0) {
            return (size_t)columns;
        }
        if (info.dwSize.X > 0) {
            return (size_t)info.dwSize.X;
        }
    }
    return 80u;
}

static size_t winxterm_dstcmd_ls_name_columns(const wchar_t *name)
{
    if (name == 0) {
        return 0u;
    }
    size_t length = wcslen(name);
    size_t offset = 0u;
    size_t columns = 0u;
    while (offset < length) {
        uint32_t codepoint = 0u;
        size_t before = offset;
        (void)winxterm_dstcmd_wide_decode_next(name, length, &offset, &codepoint);
        if (offset <= before) {
            offset = before + 1u;
        }
        int width = winxterm_dstcmd_codepoint_width(codepoint);
        if (width > 0) {
            columns += (size_t)width;
        }
    }
    return columns;
}

static bool winxterm_dstcmd_ls_append_wide_entries(WinxtermDstcmdOutputBuilder *builder,
                                                   WinxtermDstcmdShell *shell,
                                                   const WinxtermDstcmdLsEntry *entries,
                                                   size_t count)
{
    if (entries == 0 || count == 0u) {
        return true;
    }

    if (count > ((size_t)-1) / sizeof(size_t)) {
        return false;
    }
    size_t *name_columns = (size_t *)calloc(count, sizeof(*name_columns));
    if (name_columns == 0) {
        return false;
    }
    size_t widest_name = 0u;
    for (size_t i = 0u; i < count; ++i) {
        name_columns[i] = winxterm_dstcmd_ls_name_columns(entries[i].name);
        if (name_columns[i] > widest_name) {
            widest_name = name_columns[i];
        }
    }

    size_t terminal_columns = winxterm_dstcmd_ls_terminal_columns(shell);
    if (widest_name > ((size_t)-1) - 2u) {
        free(name_columns);
        return false;
    }
    size_t column_width = widest_name + 2u;
    size_t column_count = terminal_columns + 2u >= column_width ?
        (terminal_columns + 2u) / column_width : 1u;
    if (column_count == 0u) {
        column_count = 1u;
    } else if (column_count > count) {
        column_count = count;
    }
    size_t row_count = (count + column_count - 1u) / column_count;

    bool appended = true;
    for (size_t row = 0u; appended && row < row_count; ++row) {
        for (size_t column = 0u; column < column_count; ++column) {
            size_t index = row + column * row_count;
            if (index >= count) {
                break;
            }
            const wchar_t *name = entries[index].name != 0 ? entries[index].name : L"";
            if (!winxterm_dstcmd_output_builder_append_wide(builder, name)) {
                appended = false;
                break;
            }
            size_t next_index = row + (column + 1u) * row_count;
            if (next_index < count) {
                size_t padding = widest_name - name_columns[index] + 2u;
                while (padding-- != 0u) {
                    if (!winxterm_dstcmd_output_builder_append_wide(builder, L" ")) {
                        appended = false;
                        break;
                    }
                }
            }
        }
        if (appended && !winxterm_dstcmd_output_builder_append_wide(builder, L"\r\n")) {
            appended = false;
        }
    }
    free(name_columns);
    return appended;
}

static bool winxterm_dstcmd_ls_append_entry(WinxtermDstcmdOutputBuilder *builder,
                                            const WinxtermDstcmdLsEntry *entry,
                                            const WinxtermDstcmdLsOptions *options)
{
    if (entry == 0 || options == 0) {
        return true;
    }
    if (options->long_format) {
        wchar_t time_buffer[32];
        wchar_t size_buffer[32];
        winxterm_dstcmd_format_file_time(entry->write_time, time_buffer, 32u);
        winxterm_dstcmd_format_file_size(entry->size, options->human_readable, size_buffer, 32u);
        wchar_t type = (entry->attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u ? L'd' : L'-';
        wchar_t readonly = (entry->attributes & FILE_ATTRIBUTE_READONLY) != 0u ? L'r' : L'w';
        wchar_t hidden = (entry->attributes & FILE_ATTRIBUTE_HIDDEN) != 0u ? L'h' : L'-';
        return winxterm_dstcmd_output_builder_append_widef(builder,
                                                           L"%c%c%c %10ls %ls %ls\r\n",
                                                           type,
                                                           readonly,
                                                           hidden,
                                                           size_buffer,
                                                           time_buffer,
                                                           entry->name);
    }
    return winxterm_dstcmd_output_builder_append_widef(builder, L"%ls\r\n", entry->name);
}

static int winxterm_dstcmd_ls_print_cached_current_directory(WinxtermDstcmdShell *shell,
                                                            WinxtermDstcmdOutputBuilder *builder,
                                                            const WinxtermDstcmdLsOptions *options,
                                                            bool print_header,
                                                            const wchar_t *header_operand)
{
    WinxtermDstcmdDirSnapshot *snapshot =
        (WinxtermDstcmdDirSnapshot *)calloc(1u, sizeof(*snapshot));
    if (snapshot == 0) {
        (void)winxterm_dstcmd_output_builder_append_wide(builder, L"ls: out of memory\r\n");
        return 1;
    }
    if (!winxterm_dstcmd_shell_copy_dir_cache(shell, snapshot)) {
        free(snapshot);
        (void)winxterm_dstcmd_output_builder_append_wide(builder,
                                                         L"ls: directory listing is still loading\r\n");
        return 0;
    }

    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    if (print_header) {
        wchar_t *display_operand = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
        if (display_operand == 0) {
            winxterm_dstcmd_dir_snapshot_dispose(snapshot);
            free(snapshot);
            return 1;
        }
        if (!winxterm_dstcmd_output_builder_append_widef(builder,
                                                         L"%ls:\r\n",
                                                         winxterm_dstcmd_display_path(header_operand != 0 ? header_operand : L".",
                                                                                     display_operand,
                                                                                     WINXTERM_DSTCMD_PATH_CAPACITY))) {
            winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
            winxterm_dstcmd_dir_snapshot_dispose(snapshot);
            free(snapshot);
            return 1;
        }
    }

    WinxtermDstcmdLsEntry *entries = 0;
    size_t count = 0u;
    for (size_t i = 0u; i < snapshot->count; ++i) {
        if (!winxterm_dstcmd_ls_entry_should_show(snapshot->entries + i, options->all)) {
            continue;
        }
        WinxtermDstcmdLsEntry *next =
            (WinxtermDstcmdLsEntry *)realloc(entries, (count + 1u) * sizeof(*next));
        if (next == 0) {
            winxterm_dstcmd_ls_entries_dispose(entries, count);
            winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
            winxterm_dstcmd_dir_snapshot_dispose(snapshot);
            free(snapshot);
            (void)winxterm_dstcmd_output_builder_append_wide(builder, L"ls: out of memory\r\n");
            return 1;
        }
        entries = next;
        entries[count] = snapshot->entries[i];
        entries[count].name = winxterm_dstcmd_wcsdup(snapshot->entries[i].name);
        if (entries[count].name == 0) {
            winxterm_dstcmd_ls_entries_dispose(entries, count);
            winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
            winxterm_dstcmd_dir_snapshot_dispose(snapshot);
            free(snapshot);
            (void)winxterm_dstcmd_output_builder_append_wide(builder, L"ls: out of memory\r\n");
            return 1;
        }
        ++count;
    }

    qsort(entries,
          count,
          sizeof(*entries),
          options->sort_time ? winxterm_dstcmd_compare_time : winxterm_dstcmd_compare_name);
    bool appended = true;
    if (winxterm_dstcmd_ls_uses_wide_format(options)) {
        appended = winxterm_dstcmd_ls_append_wide_entries(builder, shell, entries, count);
    } else {
        for (size_t i = 0u; i < count; ++i) {
            if (!winxterm_dstcmd_ls_append_entry(builder, entries + i, options)) {
                appended = false;
                break;
            }
        }
    }

    winxterm_dstcmd_ls_entries_dispose(entries, count);
    winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
    winxterm_dstcmd_dir_snapshot_dispose(snapshot);
    free(snapshot);
    return appended ? 0 : 1;
}

static bool winxterm_dstcmd_ls_refresh_current_directory(WinxtermDstcmdShell *shell)
{
    if (shell == 0) {
        return false;
    }
    winxterm_dstcmd_shell_invalidate_dir_cache(shell);
    return winxterm_dstcmd_shell_schedule_dir_cache_refresh(shell, WINXTERM_DSTCMD_DIR_REFRESH_CD) &&
           winxterm_dstcmd_job_pool_wait_idle(&shell->jobs, 5000u);
}

static int winxterm_dstcmd_ls_one(WinxtermDstcmdShell *shell,
                                  WinxtermDstcmdOutputBuilder *builder,
                                  const wchar_t *operand,
                                  const WinxtermDstcmdLsOptions *options,
                                  bool print_header)
{
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
    wchar_t *path = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    wchar_t *display_operand = winxterm_dstcmd_scratch_alloc_path(&shell->scratch);
    if (path == 0 || display_operand == 0) {
        return 1;
    }

    int status = 0;
    if (!winxterm_dstcmd_path_resolve_scratch(&shell->scratch,
                                             shell->cwd,
                                             operand,
                                             path,
                                             WINXTERM_DSTCMD_PATH_CAPACITY)) {
        (void)winxterm_dstcmd_output_builder_append_widef(builder,
                                                          L"ls: cannot access '%ls'\r\n",
                                                          winxterm_dstcmd_display_path(operand,
                                                                                      display_operand,
                                                                                      WINXTERM_DSTCMD_PATH_CAPACITY));
        status = 1;
        goto cleanup;
    }

    WinxtermDstcmdPathInfo info;
    if (!winxterm_dstcmd_path_get_info_scratch(&shell->scratch, path, &info)) {
        (void)winxterm_dstcmd_output_builder_append_widef(builder,
                                                          L"ls: cannot access '%ls'\r\n",
                                                          winxterm_dstcmd_display_path(operand,
                                                                                      display_operand,
                                                                                      WINXTERM_DSTCMD_PATH_CAPACITY));
        status = 1;
        goto cleanup;
    }

    if ((info.attributes & FILE_ATTRIBUTE_DIRECTORY) == 0u) {
        WinxtermDstcmdLsEntry entry;
        memset(&entry, 0, sizeof(entry));
        entry.name = (wchar_t *)winxterm_dstcmd_path_basename(path);
        entry.attributes = info.attributes;
        entry.size = info.size;
        entry.write_time = info.write_time;
        if (!winxterm_dstcmd_ls_append_entry(builder, &entry, options)) {
            status = 1;
            goto cleanup;
        }
        goto cleanup;
    }

    if (wcscmp(path, shell->cwd) == 0) {
        status = winxterm_dstcmd_ls_print_cached_current_directory(shell,
                                                                   builder,
                                                                   options,
                                                                   print_header,
                                                                   operand);
        goto cleanup;
    }

    if (print_header) {
        if (!winxterm_dstcmd_output_builder_append_widef(builder,
                                                         L"%ls:\r\n",
                                                         winxterm_dstcmd_display_path(operand != 0 ? operand : L".",
                                                                                     display_operand,
                                                                                     WINXTERM_DSTCMD_PATH_CAPACITY))) {
            status = 1;
            goto cleanup;
        }
    }

    WinxtermDstcmdDirIter iter;
    if (!winxterm_dstcmd_dir_iter_open_scratch(&shell->scratch, path, &iter)) {
        goto cleanup;
    }

    WinxtermDstcmdLsEntry *entries = 0;
    size_t count = 0u;
    const WIN32_FIND_DATAW *data = 0;
    while (winxterm_dstcmd_dir_iter_next(&iter, &data)) {
        if (winxterm_dstcmd_ls_should_show(data, options->all) &&
            !winxterm_dstcmd_ls_add_entry(&entries, &count, data)) {
            winxterm_dstcmd_dir_iter_close(&iter);
            winxterm_dstcmd_ls_entries_dispose(entries, count);
            (void)winxterm_dstcmd_output_builder_append_wide(builder, L"ls: out of memory\r\n");
            status = 1;
            goto cleanup;
        }
    }
    winxterm_dstcmd_dir_iter_close(&iter);

    qsort(entries,
          count,
          sizeof(*entries),
          options->sort_time ? winxterm_dstcmd_compare_time : winxterm_dstcmd_compare_name);
    bool appended = true;
    if (winxterm_dstcmd_ls_uses_wide_format(options)) {
        appended = winxterm_dstcmd_ls_append_wide_entries(builder, shell, entries, count);
    } else {
        for (size_t i = 0u; i < count; ++i) {
            if (!winxterm_dstcmd_ls_append_entry(builder, entries + i, options)) {
                appended = false;
                break;
            }
        }
    }
    winxterm_dstcmd_ls_entries_dispose(entries, count);
    status = appended ? 0 : 1;

cleanup:
    winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
    return status;
}

int winxterm_dstcmd_cmd_ls(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    WinxtermDstcmdOutputBuilder builder;
    winxterm_dstcmd_output_builder_init(&builder);

    WinxtermDstcmdLsOptions options;
    int operand_start = 1;
    wchar_t error[128];
    if (!winxterm_dstcmd_parse_ls_options(argv, &operand_start, &options, error, 128u)) {
        (void)winxterm_dstcmd_output_builder_append_widef(&builder, L"%ls\r\n", error);
        int status = builder.failed ? 1 : 2;
        bool flushed = !builder.failed && winxterm_dstcmd_output_builder_flush(&builder, shell);
        winxterm_dstcmd_output_builder_dispose(&builder);
        if (!flushed) {
            (void)winxterm_dstcmd_shell_write_wide(shell, L"ls: out of memory\r\n");
            return 1;
        }
        return status;
    }

    int status = 0;
    if (operand_start >= argv->count) {
        status = winxterm_dstcmd_ls_one(shell, &builder, L".", &options, false);
    } else {
        bool multiple = argv->count - operand_start > 1;
        bool explicit_current_directory =
            argv->count > operand_start && wcscmp(argv->items[argv->count - 1], L".") == 0;
        if (explicit_current_directory) {
            (void)winxterm_dstcmd_ls_refresh_current_directory(shell);
        }
        for (int i = operand_start; i < argv->count; ++i) {
            if (i != operand_start && multiple &&
                !winxterm_dstcmd_output_builder_append_wide(&builder, L"\r\n")) {
                status = 1;
                break;
            }
            if (winxterm_dstcmd_ls_one(shell, &builder, argv->items[i], &options, multiple) != 0) {
                status = 1;
            }
        }
    }

    bool schedule_refresh =
        winxterm_dstcmd_shell_schedule_dir_cache_refresh(shell, WINXTERM_DSTCMD_DIR_REFRESH_LS);
    (void)schedule_refresh;
    bool flushed = !builder.failed && winxterm_dstcmd_output_builder_flush(&builder, shell);
    winxterm_dstcmd_output_builder_dispose(&builder);
    if (!flushed) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"ls: out of memory\r\n");
        if (status == 0) {
            status = 1;
        }
    }
    return status;
}
