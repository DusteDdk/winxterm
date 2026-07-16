#include "dstcmd/commands/job.h"
#include "dstcmd/api/path.h"
#include "winxterm_job_manager.h"
#include "dstcmd/winxterm_dstcmd_exec.h"
#include "dstcmd/winxterm_dstcmd_selector.h"

#include <errno.h>
#include <stdlib.h>
#include <wchar.h>

static const wchar_t *winxterm_dstcmd_job_state_name(uint32_t state)
{
    switch ((WinxtermJobState)state) {
    case WINXTERM_JOB_STARTING: return L"starting";
    case WINXTERM_JOB_FOREGROUND: return L"foreground";
    case WINXTERM_JOB_BACKGROUND: return L"background";
    case WINXTERM_JOB_STOPPING: return L"stopping";
    case WINXTERM_JOB_EXITED: return L"exited";
    case WINXTERM_JOB_FAILED: return L"failed";
    default: return L"unknown";
    }
}

static bool winxterm_dstcmd_job_id(const wchar_t *text, uint64_t *id)
{
    if (text == 0 || text[0] == L'\0' || id == 0 || text[0] == L'-') return false;
    wchar_t *end = 0;
    errno = 0;
    unsigned long long parsed = wcstoull(text, &end, 10);
    if (errno != 0 || end == text || *end != L'\0' || parsed == 0u) return false;
    *id = (uint64_t)parsed;
    return true;
}

static uint8_t *winxterm_dstcmd_job_plain_output(const uint8_t *bytes, size_t byte_count,
                                                 size_t *plain_count)
{
    uint8_t *plain = (uint8_t *)malloc(byte_count + 1u);
    if (plain == 0) return 0;
    size_t out = 0u;
    bool escape = false, osc = false, osc_escape = false;
    for (size_t i = 0u; i < byte_count; ++i) {
        uint8_t ch = bytes[i];
        if (osc) {
            if (ch == 0x07u || (osc_escape && ch == '\\')) osc = false;
            osc_escape = ch == 0x1bu;
            continue;
        }
        if (escape) {
            if (ch == ']') { osc = true; escape = false; continue; }
            if (ch >= 0x40u && ch <= 0x7eu) escape = false;
            continue;
        }
        if (ch == 0x1bu) { escape = true; continue; }
        if (ch == '\r') {
            if (i + 1u < byte_count && bytes[i + 1u] == '\n') continue;
            ch = '\n';
        }
        if (ch < 0x20u && ch != '\n' && ch != '\t') continue;
        plain[out++] = ch;
    }
    plain[out] = '\0';
    *plain_count = out;
    return plain;
}

static bool winxterm_dstcmd_job_view_pager(WinxtermDstcmdShell *shell,
                                           const uint8_t *bytes, size_t byte_count)
{
    size_t plain_count = 0u;
    uint8_t *plain = winxterm_dstcmd_job_plain_output(bytes, byte_count, &plain_count);
    if (plain == 0) return false;
    size_t line_count = 1u;
    for (size_t i = 0u; i < plain_count; ++i) if (plain[i] == '\n') ++line_count;
    size_t *lines = (size_t *)calloc(line_count + 1u, sizeof(*lines));
    if (lines == 0) { free(plain); return false; }
    size_t line = 1u;
    for (size_t i = 0u; i < plain_count; ++i) {
        if (plain[i] == '\n' && line < line_count) lines[line++] = i + 1u;
    }
    lines[line_count] = plain_count;
    size_t top = line_count > 22u ? line_count - 22u : 0u;
    bool done = false, ok = winxterm_dstcmd_shell_write_utf8(shell, "\x1b[?1049h\x1b[?25l");
    while (ok && !done) {
        ok = winxterm_dstcmd_shell_write_utf8(shell, "\x1b[H\x1b[2J");
        for (size_t row = 0u; ok && row < 22u && top + row < line_count; ++row) {
            size_t start = lines[top + row];
            size_t end = lines[top + row + 1u];
            if (end > start && plain[end - 1u] == '\n') --end;
            ok = winxterm_dstcmd_shell_write_bytes(shell, plain + start, end - start) &&
                 winxterm_dstcmd_shell_write_utf8(shell, "\r\n");
        }
        if (ok) ok = winxterm_dstcmd_shell_write_widef(
            shell, L"\x1b[7mView  lines %llu-%llu of %llu  (arrows/Page Up/Page Down; any other key exits)\x1b[0m",
            (unsigned long long)(line_count != 0u ? top + 1u : 0u),
            (unsigned long long)(top + 22u < line_count ? top + 22u : line_count),
            (unsigned long long)line_count);
        if (!ok) break;
        uint8_t input[32];
        size_t count = winxterm_dstcmd_shell_read_input(shell, input, sizeof(input), true);
        if (count == 0u) break;
        if (count >= 3u && input[0] == 0x1bu && input[1] == '[' && input[2] == 'A') {
            if (top != 0u) --top;
        } else if (count >= 3u && input[0] == 0x1bu && input[1] == '[' && input[2] == 'B') {
            if (top + 1u < line_count) ++top;
        } else if (count >= 4u && input[0] == 0x1bu && input[1] == '[' &&
                   input[2] == '5' && input[3] == '~') {
            top = top > 22u ? top - 22u : 0u;
        } else if (count >= 4u && input[0] == 0x1bu && input[1] == '[' &&
                   input[2] == '6' && input[3] == '~') {
            if (top + 22u < line_count) top += 22u;
            if (top >= line_count) top = line_count - 1u;
        } else done = true;
    }
    (void)winxterm_dstcmd_shell_write_utf8(shell, "\x1b[?25h\x1b[?1049l");
    free(lines);
    free(plain);
    return ok;
}

static int winxterm_dstcmd_job_request_simple(WinxtermDstcmdShell *shell,
                                              uint16_t type, uint64_t id)
{
    uint32_t status = ERROR_INVALID_DATA;
    if (!winxterm_dstcmd_job_client_simple(&shell->job_client, type, id, 0u, &status)) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"job: host request failed\r\n");
        return 1;
    }
    if (status != ERROR_SUCCESS) {
        (void)winxterm_dstcmd_shell_write_widef(shell, L"job: host rejected request (%lu)\r\n",
                                                (unsigned long)status);
        return 1;
    }
    return 0;
}

static int winxterm_dstcmd_job_list(WinxtermDstcmdShell *shell)
{
    WinxtermDstcmdJobInfo *jobs = 0;
    size_t count = 0u;
    if (!winxterm_dstcmd_job_client_list(&shell->job_client, &jobs, &count)) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"job: host request failed\r\n");
        free(jobs);
        return 1;
    }
    for (size_t i = 0u; i < count; ++i) {
        wchar_t exit_marker[64];
        exit_marker[0] = L'\0';
        if (jobs[i].has_exit_code) {
            (void)_snwprintf_s(exit_marker, sizeof(exit_marker) / sizeof(exit_marker[0]),
                              _TRUNCATE, L" exit=%lu", (unsigned long)jobs[i].exit_code);
        }
        (void)winxterm_dstcmd_shell_write_widef(shell,
            L"%llu - %ls (%ls) owner=%llu pid=%lu output=%llu%ls%ls%ls%ls\r\n",
            (unsigned long long)jobs[i].id,
            jobs[i].display_name[0] != L'\0' ? jobs[i].display_name : L"<unknown>",
            winxterm_dstcmd_job_state_name(jobs[i].state),
            (unsigned long long)jobs[i].owner_id,
            (unsigned long)jobs[i].process_id,
            (unsigned long long)jobs[i].buffered_output,
            jobs[i].foreground ? L" [current]" : L"",
            jobs[i].requester ? L" [self]" : L"",
            jobs[i].backpressured ? L" [backpressured]" : L"",
            exit_marker);
    }
    free(jobs);
    return 0;
}

typedef struct WinxtermDstcmdJobSelectorState {
    WinxtermDstcmdShell *shell;
    WinxtermDstcmdJobInfo *jobs;
    wchar_t **labels;
    size_t count;
} WinxtermDstcmdJobSelectorState;

static void winxterm_dstcmd_job_selector_dispose(WinxtermDstcmdJobSelectorState *state)
{
    if (state == 0) return;
    for (size_t i = 0u; i < state->count; ++i) free(state->labels != 0 ? state->labels[i] : 0);
    free(state->labels);
    free(state->jobs);
    state->labels = 0;
    state->jobs = 0;
    state->count = 0u;
}

static bool winxterm_dstcmd_job_selector_load(WinxtermDstcmdJobSelectorState *state)
{
    WinxtermDstcmdJobInfo *jobs = 0;
    size_t count = 0u;
    if (state == 0 || state->shell == 0 ||
        !winxterm_dstcmd_job_client_list(&state->shell->job_client, &jobs, &count)) {
        free(jobs);
        return false;
    }
    wchar_t **labels = count != 0u ? (wchar_t **)calloc(count, sizeof(*labels)) : 0;
    bool ok = count == 0u || labels != 0;
    for (size_t i = 0u; ok && i < count; ++i) {
        labels[i] = (wchar_t *)calloc(512u, sizeof(*labels[i]));
        ok = labels[i] != 0;
        if (ok) {
            (void)_snwprintf_s(labels[i], 512u, _TRUNCATE, L"%llu - %ls (%ls)%ls",
                               (unsigned long long)jobs[i].id,
                               jobs[i].display_name[0] != L'\0' ?
                                   jobs[i].display_name : L"<unknown>",
                               winxterm_dstcmd_job_state_name(jobs[i].state),
                               jobs[i].foreground ? L" [current]" : L"");
        }
    }
    if (!ok) {
        for (size_t i = 0u; i < count; ++i) free(labels != 0 ? labels[i] : 0);
        free(labels);
        free(jobs);
        return false;
    }
    winxterm_dstcmd_job_selector_dispose(state);
    state->jobs = jobs;
    state->labels = labels;
    state->count = count;
    return true;
}

static bool winxterm_dstcmd_job_selector_refresh(void *context,
                                                 const wchar_t *const **items,
                                                 size_t *item_count)
{
    WinxtermDstcmdJobSelectorState *state = (WinxtermDstcmdJobSelectorState *)context;
    WinxtermDstcmdJobEvent event;
    bool event_seen = false;
    while (winxterm_dstcmd_job_client_poll_event(&state->shell->job_client, &event)) {
        event_seen = true;
    }
    if (event_seen && !winxterm_dstcmd_job_selector_load(state)) return false;
    *items = (const wchar_t *const *)state->labels;
    *item_count = state->count;
    return state->count != 0u;
}

static int winxterm_dstcmd_job_selector(WinxtermDstcmdShell *shell)
{
    WinxtermDstcmdJobSelectorState selector = {shell, 0, 0, 0u};
    bool listed = winxterm_dstcmd_job_selector_load(&selector);
    if (!listed || selector.count == 0u) {
        winxterm_dstcmd_job_selector_dispose(&selector);
        (void)winxterm_dstcmd_shell_write_wide(shell, L"job: no visible jobs\r\n");
        return listed ? 0 : 1;
    }
    size_t selected = 0u;
    bool accepted = winxterm_dstcmd_selector_select_dynamic(
        shell, L"Jobs", (const wchar_t *const *)selector.labels, selector.count, 10u,
        winxterm_dstcmd_job_client_event_handle(&shell->job_client),
        winxterm_dstcmd_job_selector_refresh, &selector, &selected);
    if (!accepted || selected >= selector.count) {
        winxterm_dstcmd_job_selector_dispose(&selector);
        return 0;
    }
    WinxtermDstcmdJobInfo job = selector.jobs[selected];
    winxterm_dstcmd_job_selector_dispose(&selector);
    bool live = job.state != WINXTERM_JOB_EXITED && job.state != WINXTERM_JOB_FAILED;
    const wchar_t *actions[5] = {
        L"Foreground", L"Background", L"Close", L"────────", L"Force exit"
    };
    const uint16_t messages[5] = {
        WINXTERM_JOB_MESSAGE_FOREGROUND,
        WINXTERM_JOB_MESSAGE_BACKGROUND,
        WINXTERM_JOB_MESSAGE_SIGNAL,
        0u,
        WINXTERM_JOB_MESSAGE_SIGNAL
    };
    const uint32_t flags[5] = {0u, 0u, 0u, 0u, WINXTERM_JOB_SIGNAL_FORCE};
    bool enabled[5] = {
        live && !job.foreground,
        live && job.foreground,
        live,
        false,
        live
    };
    size_t action = 0u;
    if (!winxterm_dstcmd_selector_select_menu(shell, L"Action", actions, enabled,
                                              5u, &action)) return 0;
    uint32_t status = ERROR_INVALID_DATA;
    if (messages[action] == WINXTERM_JOB_MESSAGE_SIGNAL && flags[action] == 0u) {
        bool cancelled = false;
        DWORD elapsed = 0u;
        HANDLE input = shell->stream_input_handle != 0 ? shell->stream_input_handle :
                       shell->input_handle;
        bool ok = winxterm_dstcmd_job_client_kill(&shell->job_client, job.id, input,
                                             &cancelled, &elapsed, &status);
        return ok && status == ERROR_SUCCESS ? 0 : 1;
    } else {
        if (messages[action] == WINXTERM_JOB_MESSAGE_FOREGROUND) {
            uint32_t exit_code = 0u;
            bool has_exit_code = false;
            bool ok = winxterm_dstcmd_job_client_foreground(
                &shell->job_client, job.id, &exit_code, &has_exit_code, &status);
            return ok && status == ERROR_SUCCESS ?
                (has_exit_code ? (int)exit_code : 0) : 1;
        }
        bool ok = winxterm_dstcmd_job_client_simple(
            &shell->job_client, messages[action], job.id, flags[action], &status);
        return ok && status == ERROR_SUCCESS ? 0 : 1;
    }
}

static wchar_t *winxterm_dstcmd_job_history_wide(const char *utf8)
{
    int count = utf8 != 0 ? MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                utf8, -1, 0, 0) : 0;
    wchar_t *wide = count > 0 ? (wchar_t *)malloc((size_t)count * sizeof(*wide)) : 0;
    if (wide == 0 || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                         utf8, -1, wide, count) != count) {
        free(wide);
        return 0;
    }
    return wide;
}

static bool winxterm_dstcmd_job_select_history(WinxtermDstcmdShell *shell, wchar_t **line)
{
    *line = 0;
    size_t capacity = shell->history_count + shell->persisted_history_count;
    if (capacity == 0u) return false;
    wchar_t **items = (wchar_t **)calloc(capacity, sizeof(*items));
    if (items == 0) return false;
    size_t count = 0u;
    for (size_t i = shell->history_count; i-- > 0u;) {
        items[count] = winxterm_dstcmd_job_history_wide(shell->history[i]);
        if (items[count] != 0) ++count;
    }
    for (size_t i = 0u; i < shell->persisted_history_count; ++i) {
        bool duplicate = false;
        for (size_t j = 0u; j < shell->history_count; ++j) {
            if (shell->history[j] != 0 && shell->persisted_history[i] != 0 &&
                strcmp(shell->history[j], shell->persisted_history[i]) == 0) duplicate = true;
        }
        if (!duplicate) {
            items[count] = winxterm_dstcmd_job_history_wide(shell->persisted_history[i]);
            if (items[count] != 0) ++count;
        }
    }
    size_t selected = 0u;
    bool accepted = count != 0u && winxterm_dstcmd_selector_select(
        shell, L"History", (const wchar_t *const *)items, count, 5u, &selected);
    if (accepted) *line = winxterm_dstcmd_wcsdup(items[selected]);
    for (size_t i = 0u; i < count; ++i) free(items[i]);
    free(items);
    return accepted && *line != 0;
}

int winxterm_dstcmd_cmd_job(WinxtermDstcmdShell *shell, const WinxtermDstcmdArgv *argv)
{
    if (shell == 0 || argv == 0) return 1;
    if (!winxterm_dstcmd_job_client_available(&shell->job_client)) {
        (void)winxterm_dstcmd_shell_write_wide(shell,
                                               L"job: winxterm job control is unavailable\r\n");
        return 1;
    }
    uint64_t id = 0u;
    if (argv->count == 1) return winxterm_dstcmd_job_selector(shell);
    if (argv->count == 2 && _wcsicmp(argv->items[1], L"ls") == 0) {
        return winxterm_dstcmd_job_list(shell);
    }
    if (argv->count == 2 && _wcsicmp(argv->items[1], L"clean") == 0) {
        uint32_t removed = 0u, status = ERROR_INVALID_DATA;
        if (!winxterm_dstcmd_job_client_clean(&shell->job_client, &removed, &status) ||
            status != ERROR_SUCCESS) {
            (void)winxterm_dstcmd_shell_write_widef(shell, L"job: clean failed (%lu)\r\n",
                                                    (unsigned long)status);
            return 1;
        }
        (void)winxterm_dstcmd_shell_write_widef(shell, L"Removed %lu job%ls.\r\n",
                                                (unsigned long)removed,
                                                removed == 1u ? L"" : L"s");
        return 0;
    }
    if (argv->count == 3 && _wcsicmp(argv->items[1], L"remove") == 0 &&
        winxterm_dstcmd_job_id(argv->items[2], &id)) {
        return winxterm_dstcmd_job_request_simple(shell, WINXTERM_JOB_MESSAGE_REMOVE, id);
    }
    bool run = argv->count >= 2 && _wcsicmp(argv->items[1], L"run") == 0;
    bool open = argv->count >= 2 && _wcsicmp(argv->items[1], L"open") == 0;
    if (run || open) {
        WinxtermDstcmdArgv command;
        memset(&command, 0, sizeof(command));
        wchar_t *selected_line = 0;
        bool owns_command = false;
        wchar_t parse_error[256];
        parse_error[0] = L'\0';
        if (argv->count == 2) {
            if (!winxterm_dstcmd_job_select_history(shell, &selected_line)) return 0;
            uint64_t new_id = 0u;
            int selected_status = winxterm_dstcmd_shell_run_background_line(
                shell, selected_line, open, &new_id);
            if (selected_status != 0) {
                (void)winxterm_dstcmd_shell_write_widef(
                    shell, L"job: background launch failed (%d)\r\n", selected_status);
                free(selected_line);
                return selected_status;
            }
            (void)winxterm_dstcmd_shell_write_widef(
                shell, L"Running in background: %ls [job %llu]\r\n",
                selected_line, (unsigned long long)new_id);
            free(selected_line);
            return 0;
        } else if (argv->count == 3 &&
                   (wcschr(argv->items[2], L'|') != 0 ||
                    wcschr(argv->items[2], L'>') != 0)) {
            uint64_t new_id = 0u;
            int line_status = winxterm_dstcmd_shell_run_background_line(
                shell, argv->items[2], open, &new_id);
            if (line_status != 0) return line_status;
            (void)winxterm_dstcmd_shell_write_widef(
                shell, L"Running in background: %ls [job %llu]\r\n",
                argv->items[2], (unsigned long long)new_id);
            return 0;
        } else {
            command.count = argv->count - 2;
            command.items = argv->items + 2;
        }
        if (!winxterm_dstcmd_shell_expand_alias(shell, &command, parse_error,
                                                sizeof(parse_error) / sizeof(parse_error[0]))) {
            if (owns_command) winxterm_dstcmd_argv_dispose(&command);
            free(selected_line);
            return 1;
        }
        uint64_t new_id = 0u;
        WinxtermDstcmdExecStage background_stage;
        memset(&background_stage, 0, sizeof(background_stage));
        background_stage.argv = &command;
        background_stage.stdin_endpoint.kind = WINXTERM_DSTCMD_STREAM_TERMINAL;
        background_stage.stdout_endpoint.kind = WINXTERM_DSTCMD_STREAM_TERMINAL;
        background_stage.stderr_endpoint.kind = WINXTERM_DSTCMD_STREAM_TERMINAL;
        background_stage.isolate_shell_state = true;
        int status = winxterm_dstcmd_exec_run_managed_stages_background(
            shell, &background_stage, 1u, open, &new_id);
        if (status != 0) {
            (void)winxterm_dstcmd_shell_write_widef(shell,
                                                    L"job: background launch failed (%d)\r\n",
                                                    status);
            if (owns_command) winxterm_dstcmd_argv_dispose(&command);
            free(selected_line);
            return status;
        }
        (void)winxterm_dstcmd_shell_write_wide(shell, L"Running in background: ");
        if (selected_line != 0) {
            (void)winxterm_dstcmd_shell_write_wide(shell, selected_line);
        } else {
            for (int i = 0; i < command.count; ++i) {
                if (i != 0) (void)winxterm_dstcmd_shell_write_wide(shell, L" ");
                (void)winxterm_dstcmd_shell_write_wide(shell, command.items[i]);
            }
        }
        (void)winxterm_dstcmd_shell_write_widef(shell, L" [job %llu]\r\n",
                                                (unsigned long long)new_id);
        if (owns_command) winxterm_dstcmd_argv_dispose(&command);
        free(selected_line);
        return 0;
    }
    if (argv->count == 3 && _wcsicmp(argv->items[1], L"view") == 0 &&
        winxterm_dstcmd_job_id(argv->items[2], &id)) {
        uint8_t *bytes = 0;
        size_t byte_count = 0u;
        uint32_t status = ERROR_INVALID_DATA;
        if (!winxterm_dstcmd_job_client_view(&shell->job_client, id, &bytes,
                                             &byte_count, &status)) {
            (void)winxterm_dstcmd_shell_write_widef(shell, L"job: view failed (%lu)\r\n",
                                                    (unsigned long)status);
            free(bytes);
            return 1;
        }
        bool written = winxterm_dstcmd_job_view_pager(shell, bytes, byte_count);
        free(bytes);
        return written ? 0 : 1;
    }
    bool force_kill = argv->count == 4 && _wcsicmp(argv->items[1], L"kill") == 0 &&
                      _wcsicmp(argv->items[2], L"-f") == 0;
    bool normal_kill = argv->count == 3 && _wcsicmp(argv->items[1], L"kill") == 0;
    const wchar_t *kill_id = force_kill ? argv->items[3] : normal_kill ? argv->items[2] : 0;
    if (kill_id != 0 && winxterm_dstcmd_job_id(kill_id, &id)) {
        uint32_t status = ERROR_INVALID_DATA;
        bool requested = false;
        bool cancelled = false;
        DWORD elapsed_ms = 0u;
        if (force_kill) {
            requested = winxterm_dstcmd_job_client_simple(
                &shell->job_client, WINXTERM_JOB_MESSAGE_SIGNAL, id,
                WINXTERM_JOB_SIGNAL_FORCE, &status);
        } else {
            HANDLE input = shell->stream_input_handle != 0 ? shell->stream_input_handle :
                           shell->input_handle;
            requested = winxterm_dstcmd_job_client_kill(&shell->job_client, id, input,
                                                        &cancelled, &elapsed_ms, &status);
        }
        if (cancelled) {
            (void)winxterm_dstcmd_shell_write_widef(
                shell, L"job: kill cancelled after %lu ms; job left running\r\n",
                (unsigned long)elapsed_ms);
            return 1;
        }
        if (!requested || status != ERROR_SUCCESS) {
            (void)winxterm_dstcmd_shell_write_widef(shell, L"job: kill failed (%lu)\r\n",
                                                    (unsigned long)status);
            return 1;
        }
        return 0;
    }
    if (argv->count == 4 && _wcsicmp(argv->items[1], L"connect") == 0) {
        uint64_t destination_id = 0u;
        uint32_t status = ERROR_INVALID_DATA;
        if (!winxterm_dstcmd_job_id(argv->items[2], &id) ||
            !winxterm_dstcmd_job_id(argv->items[3], &destination_id)) {
            (void)winxterm_dstcmd_shell_write_wide(shell, L"job: invalid job id\r\n");
            return 2;
        }
        if (!winxterm_dstcmd_job_client_connect(&shell->job_client, id, destination_id,
                                                &status) || status != ERROR_SUCCESS) {
            (void)winxterm_dstcmd_shell_write_widef(shell, L"job: connect failed (%lu)\r\n",
                                                    (unsigned long)status);
            return 1;
        }
        return 0;
    }
    if (argv->count == 3 && _wcsicmp(argv->items[1], L"disconnect") == 0) {
        uint32_t status = ERROR_INVALID_DATA;
        if (!winxterm_dstcmd_job_id(argv->items[2], &id)) {
            (void)winxterm_dstcmd_shell_write_wide(shell, L"job: invalid job id\r\n");
            return 2;
        }
        if (!winxterm_dstcmd_job_client_disconnect(&shell->job_client, id, &status) ||
            status != ERROR_SUCCESS) {
            (void)winxterm_dstcmd_shell_write_widef(shell, L"job: disconnect failed (%lu)\r\n",
                                                    (unsigned long)status);
            return 1;
        }
        return 0;
    }
    if (argv->count >= 4 && argv->count <= 6 &&
        _wcsicmp(argv->items[1], L"attach") == 0) {
        bool append = false, tee = false;
        if (!winxterm_dstcmd_job_id(argv->items[2], &id)) {
            (void)winxterm_dstcmd_shell_write_wide(shell, L"job: invalid job id\r\n");
            return 2;
        }
        for (int i = 4; i < argv->count; ++i) {
            if (_wcsicmp(argv->items[i], L"append") == 0) append = true;
            else if (_wcsicmp(argv->items[i], L"tee") == 0) tee = true;
            else {
                (void)winxterm_dstcmd_shell_write_wide(shell,
                                                       L"job: expected append or tee\r\n");
                return 2;
            }
        }
        uint32_t status = ERROR_INVALID_DATA;
        WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(&shell->scratch);
        WinxtermDstcmdWin32Path path;
        bool resolved = winxterm_dstcmd_path_prepare_win32_scratch(&shell->scratch,
                                                                   argv->items[3], &path);
        if (!resolved ||
            !winxterm_dstcmd_job_client_attach(&shell->job_client, id, path.syscall,
                                               append, tee, &status) ||
            status != ERROR_SUCCESS) {
            winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
            (void)winxterm_dstcmd_shell_write_widef(shell, L"job: attach failed (%lu)\r\n",
                                                    (unsigned long)status);
            return 1;
        }
        winxterm_dstcmd_scratch_rewind(&shell->scratch, mark);
        return 0;
    }
    if (argv->count == 3 && _wcsicmp(argv->items[1], L"detach") == 0 &&
        winxterm_dstcmd_job_id(argv->items[2], &id)) {
        return winxterm_dstcmd_job_request_simple(shell, WINXTERM_JOB_MESSAGE_DETACH, id);
    }
    if (argv->count == 2 && winxterm_dstcmd_job_id(argv->items[1], &id)) {
        uint32_t exit_code = 0u;
        uint32_t status = ERROR_INVALID_DATA;
        bool has_exit_code = false;
        if (!winxterm_dstcmd_job_client_foreground(&shell->job_client, id,
                                                   &exit_code, &has_exit_code,
                                                   &status) ||
            status != ERROR_SUCCESS) {
            (void)winxterm_dstcmd_shell_write_widef(
                shell, L"job: foreground failed (%lu)\r\n",
                (unsigned long)status);
            return 1;
        }
        return has_exit_code ? (int)exit_code : 0;
    }
    (void)winxterm_dstcmd_shell_write_wide(shell,
        L"usage: job [ls|clean|remove ID|ID|view ID|run COMMAND...|open COMMAND...|connect SOURCE DEST|disconnect SOURCE|attach ID FILE [append] [tee]|detach ID|kill [-f] ID]\r\n");
    return 2;
}
