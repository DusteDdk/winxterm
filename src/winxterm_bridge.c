#include "winxterm_bridge.h"

#include "winxterm_replies.h"
#include "winxterm_options.h"
#include "winxterm_scale.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

typedef struct WinxtermBridgeOutputCommit {
    WinxtermBridge *bridge;
    bool content_changed;
    bool presentation_changed;
} WinxtermBridgeOutputCommit;

static bool winxterm_bridge_field_equals(const char *field,
                                         size_t field_length,
                                         const char *name,
                                         const char *value)
{
    size_t name_length = name != 0 ? strlen(name) : 0u;
    size_t value_length = value != 0 ? strlen(value) : 0u;
    return field != 0 &&
           field_length == name_length + 1u + value_length &&
           strncmp(field, name, name_length) == 0 &&
           field[name_length] == '=' &&
           strncmp(field + name_length + 1u, value, value_length) == 0;
}

static bool winxterm_bridge_osc_field(const char *payload,
                                      const char *name,
                                      char *out,
                                      size_t out_count)
{
    if (payload == 0 || name == 0 || out == 0 || out_count == 0u) {
        return false;
    }
    size_t name_length = strlen(name);
    const char *p = payload;
    while (*p != '\0') {
        const char *end = strchr(p, ';');
        size_t field_length = end != 0 ? (size_t)(end - p) : strlen(p);
        if (field_length > name_length &&
            strncmp(p, name, name_length) == 0 &&
            p[name_length] == '=') {
            size_t value_length = field_length - name_length - 1u;
            if (value_length >= out_count) {
                value_length = out_count - 1u;
            }
            memcpy(out, p + name_length + 1u, value_length);
            out[value_length] = '\0';
            return true;
        }
        if (end == 0) {
            break;
        }
        p = end + 1;
    }
    return false;
}

static int winxterm_bridge_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool winxterm_bridge_percent_decode(const char *text, char *out, size_t out_count)
{
    if (text == 0 || out == 0 || out_count == 0u) {
        return false;
    }
    size_t offset = 0u;
    for (size_t i = 0u; text[i] != '\0'; ++i) {
        unsigned char value = (unsigned char)text[i];
        if (text[i] == '%') {
            int high = winxterm_bridge_hex_value(text[i + 1u]);
            int low = winxterm_bridge_hex_value(text[i + 2u]);
            if (high < 0 || low < 0) {
                return false;
            }
            value = (unsigned char)((high << 4) | low);
            i += 2u;
        }
        if (offset + 1u >= out_count) {
            return false;
        }
        out[offset++] = (char)value;
    }
    out[offset] = '\0';
    return true;
}

static bool winxterm_bridge_utf8_to_wide(const char *text, wchar_t *out, size_t out_count)
{
    if (text == 0 || out == 0 || out_count == 0u) {
        return false;
    }
    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, 0, 0);
    if (needed <= 0 || (size_t)needed > out_count) {
        return false;
    }
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1, out, (int)out_count) > 0;
}

static bool winxterm_bridge_queue_winxterm_reply(WinxtermBridge *bridge,
                                                 const char *id,
                                                 bool ok,
                                                 const char *message)
{
    char reply[512];
    int written = sprintf_s(reply,
                            sizeof(reply),
                            "\x1fwinxterm;v=1;id=%s;status=%s;message=%s\x1e",
                            id != 0 && id[0] != '\0' ? id : "0",
                            ok ? "ok" : "error",
                            message != 0 ? message : "");
    return written > 0 && winxterm_bridge_queue_reply(bridge, (const uint8_t *)reply, (size_t)written);
}

static bool winxterm_bridge_handle_winxterm_osc(WinxtermBridge *bridge, const char *payload)
{
    char id[32];
    char command[64];
    char value[1024];
    if (bridge == 0 || payload == 0 || strncmp(payload, "winxterm;", 9u) != 0 ||
        !winxterm_bridge_osc_field(payload, "id", id, sizeof(id)) ||
        !winxterm_bridge_osc_field(payload, "cmd", command, sizeof(command))) {
        return false;
    }

    const char *version_field = payload + 9u;
    if (!winxterm_bridge_field_equals(version_field, 3u, "v", "1") &&
        strstr(payload, ";v=1;") == 0) {
        return winxterm_bridge_queue_winxterm_reply(bridge, id, false, "unsupported-version");
    }

    if (strcmp(command, "query") == 0) {
        return winxterm_bridge_queue_winxterm_reply(bridge,
                                                    id,
                                                    true,
                                                    "caps=query,set-scale,set-bell,set-scrollbar,set-debuglog,playmacro");
    }
    if (strcmp(command, "set-scale") == 0) {
        if (!winxterm_bridge_osc_field(payload, "value", value, sizeof(value))) {
            return winxterm_bridge_queue_winxterm_reply(bridge, id, false, "missing-value");
        }
        unsigned long scale = strtoul(value, 0, 10);
        if (scale == 0ul || scale > WINXTERM_MAX_DISPLAY_SCALE) {
            return winxterm_bridge_queue_winxterm_reply(bridge, id, false, "invalid-scale");
        }
        winxterm_bridge_request_display_scale(bridge, (unsigned int)scale);
        return winxterm_bridge_queue_winxterm_reply(bridge, id, true, "ok");
    }
    if (strcmp(command, "set-bell") == 0) {
        if (!winxterm_bridge_osc_field(payload, "value", value, sizeof(value))) {
            return winxterm_bridge_queue_winxterm_reply(bridge, id, false, "missing-value");
        }
        if (strcmp(value, "on") == 0) {
            winxterm_bridge_set_bell_enabled(bridge, true);
        } else if (strcmp(value, "off") == 0) {
            winxterm_bridge_set_bell_enabled(bridge, false);
        } else {
            return winxterm_bridge_queue_winxterm_reply(bridge, id, false, "invalid-value");
        }
        return winxterm_bridge_queue_winxterm_reply(bridge, id, true, "ok");
    }
    if (strcmp(command, "set-scrollbar") == 0) {
        if (!winxterm_bridge_osc_field(payload, "value", value, sizeof(value))) {
            return winxterm_bridge_queue_winxterm_reply(bridge, id, false, "missing-value");
        }
        if (strcmp(value, "on") == 0) {
            winxterm_bridge_request_scrollbar(bridge, true);
        } else if (strcmp(value, "off") == 0) {
            winxterm_bridge_request_scrollbar(bridge, false);
        } else {
            return winxterm_bridge_queue_winxterm_reply(bridge, id, false, "invalid-value");
        }
        return winxterm_bridge_queue_winxterm_reply(bridge, id, true, "ok");
    }
    if (strcmp(command, "set-debuglog") == 0) {
        if (!winxterm_bridge_osc_field(payload, "value", value, sizeof(value))) {
            return winxterm_bridge_queue_winxterm_reply(bridge, id, false, "missing-value");
        }
        bool ok = true;
        if (strcmp(value, "on") == 0) {
            ok = bridge->log != 0 && winxterm_log_enable_for_process(bridge->log);
        } else if (strcmp(value, "off") == 0) {
            if (bridge->log != 0) {
                winxterm_log_disable(bridge->log);
            }
        } else {
            return winxterm_bridge_queue_winxterm_reply(bridge, id, false, "invalid-value");
        }
        return winxterm_bridge_queue_winxterm_reply(bridge, id, ok, ok ? "ok" : "debuglog-failed");
    }
    if (strcmp(command, "playmacro") == 0) {
        char decoded[WINXTERM_TERMINAL_OSC_PAYLOAD_CAPACITY];
        wchar_t path[32768];
        if (!winxterm_bridge_osc_field(payload, "path", value, sizeof(value)) ||
            !winxterm_bridge_percent_decode(value, decoded, sizeof(decoded)) ||
            !winxterm_bridge_utf8_to_wide(decoded, path, sizeof(path) / sizeof(path[0]))) {
            return winxterm_bridge_queue_winxterm_reply(bridge, id, false, "invalid-path");
        }
        DWORD attributes = GetFileAttributesW(path);
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0u) {
            return winxterm_bridge_queue_winxterm_reply(bridge, id, false, "macro-not-found");
        }
        bool ok = winxterm_bridge_request_macro(bridge, path);
        return winxterm_bridge_queue_winxterm_reply(bridge, id, ok, ok ? "ok" : "macro-queue-failed");
    }
    return winxterm_bridge_queue_winxterm_reply(bridge, id, false, "unknown-command");
}

static bool winxterm_bridge_terminal_sink(void *context, const WinxtermTerminalOp *op)
{
    WinxtermBridgeOutputCommit *commit = (WinxtermBridgeOutputCommit *)context;
    WinxtermBridge *bridge = commit != 0 ? commit->bridge : 0;
    if (bridge == 0 || op == 0) {
        return false;
    }
    if (op->type == WINXTERM_TERMINAL_OP_TITLE) {
        winxterm_bridge_set_title_utf8(bridge, op->data.title.text, op->data.title.length);
        commit->presentation_changed = true;
        return true;
    }
    if (op->type == WINXTERM_TERMINAL_OP_BELL) {
        if (winxterm_bridge_note_bell(bridge)) {
            commit->presentation_changed = true;
        }
        return true;
    }
    if (op->type == WINXTERM_TERMINAL_OP_REPLY_BYTES) {
        if (!winxterm_bridge_queue_reply(bridge, op->data.reply.bytes, op->data.reply.length)) {
            winxterm_log_writef(bridge->log,
                                "terminal reply dropped: queue rejected %zu bytes",
                                op->data.reply.length);
            return false;
        }
        return true;
    }
    if (op->type == WINXTERM_TERMINAL_OP_QUERY) {
        uint8_t reply[WINXTERM_TERMINAL_REPLY_CAPACITY];
        size_t reply_length = 0u;
        switch (op->data.query.type) {
        case WINXTERM_TERMINAL_QUERY_PRIMARY_DA:
            reply_length = winxterm_reply_primary_da(reply, sizeof(reply));
            break;
        case WINXTERM_TERMINAL_QUERY_SECONDARY_DA:
            reply_length = winxterm_reply_secondary_da(reply, sizeof(reply));
            break;
        case WINXTERM_TERMINAL_QUERY_DSR_STATUS:
            reply_length = winxterm_reply_dsr_status(reply, sizeof(reply));
            break;
        case WINXTERM_TERMINAL_QUERY_CPR:
            reply_length = winxterm_reply_cpr(bridge->screen.cursor_row + 1,
                                              bridge->screen.cursor_col + 1,
                                              reply,
                                              sizeof(reply));
            break;
        case WINXTERM_TERMINAL_QUERY_DECRQM: {
            WinxtermTerminalMode mode;
            int status = 0;
            if (winxterm_mode_from_csi(op->data.query.private_marker, op->data.query.param, &mode)) {
                status = winxterm_mode_decrqm_status(&bridge->screen.mode_state, mode);
            }
            reply_length = winxterm_reply_decrqm(op->data.query.private_marker,
                                                 op->data.query.param,
                                                 status,
                                                 reply,
                                                 sizeof(reply));
            break;
        }
        case WINXTERM_TERMINAL_QUERY_DECRQSS:
            if (op->data.query.request_length == 1u && op->data.query.request[0] == 'm') {
                reply_length = winxterm_reply_decrqss("0m", 2u, true, reply, sizeof(reply));
            } else {
                reply_length = winxterm_reply_decrqss(op->data.query.request,
                                                      op->data.query.request_length,
                                                      false,
                                                      reply,
                                                      sizeof(reply));
            }
            break;
        case WINXTERM_TERMINAL_QUERY_XTVERSION:
            reply_length = winxterm_reply_xtversion(reply, sizeof(reply));
            break;
        case WINXTERM_TERMINAL_QUERY_XTQMODKEYS:
            reply_length = winxterm_reply_xtqmodkeys(reply, sizeof(reply));
            break;
        case WINXTERM_TERMINAL_QUERY_XTQFMTKEYS:
            reply_length = winxterm_reply_xtqfmtkeys(reply, sizeof(reply));
            break;
        default:
            break;
        }
        if (reply_length == 0u) {
            return true;
        }
        if (!winxterm_bridge_queue_reply(bridge, reply, reply_length)) {
            winxterm_log_writef(bridge->log,
                                "terminal query reply dropped: queue rejected %zu bytes",
                                reply_length);
            return false;
        }
        return true;
    }
    if (op->type == WINXTERM_TERMINAL_OP_OSC) {
        if (op->data.osc.command == WINXTERM_TERMINAL_OSC_WINXTERM_CONTROL &&
            op->data.osc.outcome == WINXTERM_TERMINAL_OSC_ACCEPTED) {
            return winxterm_bridge_handle_winxterm_osc(bridge, op->data.osc.payload);
        }
        return true;
    }
    bool ok = winxterm_screen_apply_op(&bridge->screen, op);
    if (ok) {
        commit->content_changed = true;
    }
    return ok;
}

uint64_t winxterm_bridge_timestamp_ns(void)
{
    static LARGE_INTEGER frequency;
    if (frequency.QuadPart == 0) {
        (void)QueryPerformanceFrequency(&frequency);
    }
    LARGE_INTEGER counter;
    (void)QueryPerformanceCounter(&counter);
    if (frequency.QuadPart <= 0) {
        return 0u;
    }
    uint64_t ticks = (uint64_t)counter.QuadPart;
    uint64_t ticks_per_second = (uint64_t)frequency.QuadPart;
    uint64_t seconds = ticks / ticks_per_second;
    uint64_t remainder = ticks % ticks_per_second;
    return seconds * 1000000000ull + (remainder * 1000000000ull) / ticks_per_second;
}

void winxterm_bridge_note_output_batch(WinxtermBridge *bridge, size_t byte_count)
{
    if (bridge == 0 || byte_count == 0u) {
        return;
    }
    EnterCriticalSection(&bridge->input_lock);
    ++bridge->output_batch_count;
    bridge->output_batch_bytes += (unsigned long long)byte_count;
    if ((unsigned long long)byte_count > bridge->output_batch_max_bytes) {
        bridge->output_batch_max_bytes = (unsigned long long)byte_count;
    }
    LeaveCriticalSection(&bridge->input_lock);
}

bool winxterm_bridge_init(WinxtermBridge *bridge, WinxtermLog *log, int columns, int rows)
{
    if (bridge == 0) {
        return false;
    }

    memset(bridge, 0, sizeof(*bridge));
    if (!winxterm_job_manager_init(&bridge->job_manager)) {
        return false;
    }
    bridge->log = log;
    bridge->backend = WINXTERM_DEFAULT_RENDER_BACKEND;
    bridge->unpainted_line_limit = WINXTERM_DEFAULT_UNPAINTED_LINE_LIMIT;
    bridge->fps_window_start_tick = GetTickCount();
    bridge->host_state = WINXTERM_HOST_STATE_NOT_STARTED;
    bridge->input_capacity = WINXTERM_BRIDGE_INPUT_INITIAL_CAPACITY;
    bridge->input_buffer = (uint8_t *)malloc(bridge->input_capacity);
    if (bridge->input_buffer == 0) {
        bridge->input_capacity = 0u;
        return false;
    }
    bridge->output_capacity = WINXTERM_BRIDGE_OUTPUT_INITIAL_CAPACITY;
    bridge->output_buffer = (uint8_t *)malloc(bridge->output_capacity);
    if (bridge->output_buffer == 0) {
        bridge->output_capacity = 0u;
        free(bridge->input_buffer);
        bridge->input_buffer = 0;
        bridge->input_capacity = 0u;
        return false;
    }
    bridge->transcript_capacity = WINXTERM_BRIDGE_TRANSCRIPT_CAPACITY;
    bridge->transcript_buffer = (uint8_t *)malloc(bridge->transcript_capacity);
    if (bridge->transcript_buffer == 0) {
        bridge->transcript_capacity = 0u;
    }
    winxterm_utf8_decoder_init(&bridge->output_decoder);
    InitializeCriticalSection(&bridge->screen_lock);
    bridge->screen_lock_initialized = true;
    InitializeCriticalSection(&bridge->input_lock);
    bridge->input_lock_initialized = true;
    bridge->hwnd_ready_event = CreateEventW(0, TRUE, FALSE, 0);
    if (bridge->hwnd_ready_event == 0) {
        free(bridge->output_buffer);
        bridge->output_buffer = 0;
        bridge->output_capacity = 0u;
        free(bridge->transcript_buffer);
        bridge->transcript_buffer = 0;
        bridge->transcript_capacity = 0u;
        free(bridge->input_buffer);
        bridge->input_buffer = 0;
        bridge->input_capacity = 0u;
        DeleteCriticalSection(&bridge->input_lock);
        bridge->input_lock_initialized = false;
        DeleteCriticalSection(&bridge->screen_lock);
        bridge->screen_lock_initialized = false;
        return false;
    }
    bridge->unpainted_below_limit_event = CreateEventW(0, TRUE, TRUE, 0);
    if (bridge->unpainted_below_limit_event == 0) {
        free(bridge->output_buffer);
        bridge->output_buffer = 0;
        bridge->output_capacity = 0u;
        free(bridge->transcript_buffer);
        bridge->transcript_buffer = 0;
        bridge->transcript_capacity = 0u;
        free(bridge->input_buffer);
        bridge->input_buffer = 0;
        bridge->input_capacity = 0u;
        CloseHandle(bridge->hwnd_ready_event);
        bridge->hwnd_ready_event = 0;
        DeleteCriticalSection(&bridge->input_lock);
        bridge->input_lock_initialized = false;
        DeleteCriticalSection(&bridge->screen_lock);
        bridge->screen_lock_initialized = false;
        return false;
    }
    bridge->output_room_event = CreateEventW(0, TRUE, TRUE, 0);
    if (bridge->output_room_event == 0) {
        free(bridge->output_buffer);
        bridge->output_buffer = 0;
        bridge->output_capacity = 0u;
        free(bridge->transcript_buffer);
        bridge->transcript_buffer = 0;
        bridge->transcript_capacity = 0u;
        free(bridge->input_buffer);
        bridge->input_buffer = 0;
        bridge->input_capacity = 0u;
        CloseHandle(bridge->hwnd_ready_event);
        bridge->hwnd_ready_event = 0;
        CloseHandle(bridge->unpainted_below_limit_event);
        bridge->unpainted_below_limit_event = 0;
        DeleteCriticalSection(&bridge->input_lock);
        bridge->input_lock_initialized = false;
        DeleteCriticalSection(&bridge->screen_lock);
        bridge->screen_lock_initialized = false;
        return false;
    }
    bridge->input_ready_event = CreateEventW(0, TRUE, FALSE, 0);
    if (bridge->input_ready_event == 0) {
        free(bridge->output_buffer);
        bridge->output_buffer = 0;
        bridge->output_capacity = 0u;
        free(bridge->transcript_buffer);
        bridge->transcript_buffer = 0;
        bridge->transcript_capacity = 0u;
        free(bridge->input_buffer);
        bridge->input_buffer = 0;
        bridge->input_capacity = 0u;
        CloseHandle(bridge->hwnd_ready_event);
        bridge->hwnd_ready_event = 0;
        CloseHandle(bridge->unpainted_below_limit_event);
        bridge->unpainted_below_limit_event = 0;
        CloseHandle(bridge->output_room_event);
        bridge->output_room_event = 0;
        DeleteCriticalSection(&bridge->input_lock);
        bridge->input_lock_initialized = false;
        DeleteCriticalSection(&bridge->screen_lock);
        bridge->screen_lock_initialized = false;
        return false;
    }
    if (!winxterm_screen_init(&bridge->screen, columns, rows)) {
        free(bridge->output_buffer);
        bridge->output_buffer = 0;
        bridge->output_capacity = 0u;
        free(bridge->transcript_buffer);
        bridge->transcript_buffer = 0;
        bridge->transcript_capacity = 0u;
        free(bridge->input_buffer);
        bridge->input_buffer = 0;
        bridge->input_capacity = 0u;
        CloseHandle(bridge->hwnd_ready_event);
        bridge->hwnd_ready_event = 0;
        CloseHandle(bridge->unpainted_below_limit_event);
        bridge->unpainted_below_limit_event = 0;
        CloseHandle(bridge->output_room_event);
        bridge->output_room_event = 0;
        CloseHandle(bridge->input_ready_event);
        bridge->input_ready_event = 0;
        DeleteCriticalSection(&bridge->input_lock);
        bridge->input_lock_initialized = false;
        DeleteCriticalSection(&bridge->screen_lock);
        bridge->screen_lock_initialized = false;
        return false;
    }
    winxterm_job_coordinator_init(&bridge->job_coordinator,
                                  WINXTERM_BRIDGE_JOB_ACTION_QUEUE_LIMIT);
    return true;
}

void winxterm_bridge_dispose(WinxtermBridge *bridge)
{
    if (bridge == 0) {
        return;
    }
    winxterm_job_manager_dispose(&bridge->job_manager);
    winxterm_job_coordinator_dispose(&bridge->job_coordinator);

    free(bridge->input_buffer);
    bridge->input_buffer = 0;
    bridge->input_capacity = 0u;
    bridge->input_count = 0u;
    bridge->input_head = 0u;
    bridge->input_tail = 0u;
    free(bridge->output_buffer);
    bridge->output_buffer = 0;
    bridge->output_capacity = 0u;
    bridge->output_count = 0u;
    bridge->output_high_water = 0u;
    free(bridge->transcript_buffer);
    bridge->transcript_buffer = 0;
    bridge->transcript_capacity = 0u;
    bridge->transcript_head = 0u;
    bridge->transcript_count = 0u;
    free(bridge->pending_macro_path);
    bridge->pending_macro_path = 0;
    bridge->macro_update_pending = false;
    winxterm_screen_dispose(&bridge->screen);
    if (bridge->hwnd_ready_event != 0) {
        CloseHandle(bridge->hwnd_ready_event);
        bridge->hwnd_ready_event = 0;
    }
    if (bridge->unpainted_below_limit_event != 0) {
        CloseHandle(bridge->unpainted_below_limit_event);
        bridge->unpainted_below_limit_event = 0;
    }
    if (bridge->output_room_event != 0) {
        CloseHandle(bridge->output_room_event);
        bridge->output_room_event = 0;
    }
    if (bridge->input_ready_event != 0) {
        CloseHandle(bridge->input_ready_event);
        bridge->input_ready_event = 0;
    }
    if (bridge->input_lock_initialized) {
        DeleteCriticalSection(&bridge->input_lock);
        bridge->input_lock_initialized = false;
    }
    if (bridge->screen_lock_initialized) {
        DeleteCriticalSection(&bridge->screen_lock);
        bridge->screen_lock_initialized = false;
    }
}

void winxterm_bridge_set_hwnd(WinxtermBridge *bridge, HWND hwnd)
{
    if (bridge == 0) {
        return;
    }

    bool post_macro = false;
    EnterCriticalSection(&bridge->input_lock);
    bridge->hwnd = hwnd;
    if (hwnd == 0) {
        bridge->render_update_pending = false;
        bridge->pending_frame_causes = WINXTERM_FRAME_CAUSE_NONE;
        bridge->macro_update_pending = false;
    } else if (bridge->pending_macro_path != 0 && !bridge->host_headless &&
               !bridge->macro_update_pending) {
        bridge->macro_update_pending = true;
        post_macro = true;
    }
    LeaveCriticalSection(&bridge->input_lock);
    if (hwnd != 0 && bridge->hwnd_ready_event != 0) {
        SetEvent(bridge->hwnd_ready_event);
    }
    if (post_macro) {
        PostMessageW(hwnd, WINXTERM_WM_MACRO_UPDATE, 0, 0);
    }
}

void winxterm_bridge_post_update(WinxtermBridge *bridge)
{
    winxterm_bridge_request_frame(bridge, WINXTERM_FRAME_CAUSE_CONTENT);
}

void winxterm_bridge_request_frame(WinxtermBridge *bridge, unsigned int causes)
{
    if (bridge != 0) {
        EnterCriticalSection(&bridge->input_lock);
        ++bridge->render_update_request_count;
        HWND hwnd = bridge->hwnd;
        bool headless = bridge->host_headless;
        bridge->pending_frame_causes |= causes != 0u ? causes : WINXTERM_FRAME_CAUSE_CONTENT;
        bool post = hwnd != 0 && !headless && !bridge->render_update_pending;
        if (post) {
            bridge->render_update_pending = true;
        } else if (hwnd != 0 && !headless) {
            ++bridge->render_update_coalesced_count;
        }
        LeaveCriticalSection(&bridge->input_lock);
        if (post) {
            PostMessageW(hwnd, WINXTERM_WM_RENDER_UPDATE, 0, 0);
        }
    }
}

bool winxterm_bridge_take_render_update(WinxtermBridge *bridge)
{
    if (bridge == 0) {
        return false;
    }

    bool pending = false;
    EnterCriticalSection(&bridge->input_lock);
    if (bridge->render_update_pending) {
        bridge->render_update_pending = false;
        ++bridge->render_update_taken_count;
        pending = true;
    }
    LeaveCriticalSection(&bridge->input_lock);
    return pending;
}

unsigned int winxterm_bridge_take_frame_request(WinxtermBridge *bridge)
{
    if (bridge == 0) {
        return WINXTERM_FRAME_CAUSE_NONE;
    }

    unsigned int causes = WINXTERM_FRAME_CAUSE_NONE;
    EnterCriticalSection(&bridge->input_lock);
    causes = bridge->pending_frame_causes;
    bridge->pending_frame_causes = WINXTERM_FRAME_CAUSE_NONE;
    LeaveCriticalSection(&bridge->input_lock);
    return causes;
}

void winxterm_bridge_resize_terminal(WinxtermBridge *bridge, int columns, int rows)
{
    if (bridge == 0 || columns <= 0 || rows <= 0) {
        return;
    }

    EnterCriticalSection(&bridge->screen_lock);
    winxterm_screen_resize(&bridge->screen, columns, rows);
    LeaveCriticalSection(&bridge->screen_lock);

    winxterm_bridge_queue_terminal_resize(bridge, columns, rows);
}

void winxterm_bridge_queue_terminal_resize(WinxtermBridge *bridge, int columns, int rows)
{
    if (bridge == 0 || columns <= 0 || rows <= 0) {
        return;
    }

    EnterCriticalSection(&bridge->input_lock);
    if (bridge->pending_resize) {
        ++bridge->resize_coalesced_count;
    }
    ++bridge->resize_request_count;
    bridge->pending_resize_columns = columns;
    bridge->pending_resize_rows = rows;
    bridge->pending_resize = true;
    if (bridge->input_ready_event != 0) {
        SetEvent(bridge->input_ready_event);
    }
    LeaveCriticalSection(&bridge->input_lock);
}

bool winxterm_bridge_peek_pending_resize(WinxtermBridge *bridge, int *columns, int *rows)
{
    if (bridge == 0 || columns == 0 || rows == 0) {
        return false;
    }
    bool has_resize = false;
    EnterCriticalSection(&bridge->input_lock);
    if (bridge->pending_resize) {
        *columns = bridge->pending_resize_columns;
        *rows = bridge->pending_resize_rows;
        ++bridge->resize_taken_count;
        has_resize = true;
    }
    if (bridge->input_ready_event != 0 && bridge->input_count == 0u) {
        ResetEvent(bridge->input_ready_event);
    }
    LeaveCriticalSection(&bridge->input_lock);
    return has_resize;
}

void winxterm_bridge_request_display_scale(WinxtermBridge *bridge, unsigned int scale)
{
    if (bridge == 0 || !winxterm_display_scale_valid(scale)) {
        return;
    }

    HWND hwnd = 0;
    bool headless = false;
    EnterCriticalSection(&bridge->input_lock);
    if (bridge->pending_scale) {
        ++bridge->scale_coalesced_count;
    }
    ++bridge->scale_request_count;
    bridge->pending_display_scale = scale;
    bridge->pending_scale = true;
    hwnd = bridge->hwnd;
    headless = bridge->host_headless;
    LeaveCriticalSection(&bridge->input_lock);

    if (hwnd != 0 && !headless) {
        PostMessageW(hwnd, WINXTERM_WM_SCALE_UPDATE, 0, 0);
    }
}

bool winxterm_bridge_take_pending_display_scale(WinxtermBridge *bridge, unsigned int *scale)
{
    if (bridge == 0 || scale == 0) {
        return false;
    }

    bool has_scale = false;
    EnterCriticalSection(&bridge->input_lock);
    if (bridge->pending_scale) {
        *scale = bridge->pending_display_scale;
        bridge->pending_scale = false;
        ++bridge->scale_taken_count;
        has_scale = true;
    }
    LeaveCriticalSection(&bridge->input_lock);
    return has_scale;
}

void winxterm_bridge_request_scrollbar(WinxtermBridge *bridge, bool enabled)
{
    if (bridge == 0) {
        return;
    }

    HWND hwnd = 0;
    bool headless = false;
    EnterCriticalSection(&bridge->input_lock);
    bridge->pending_scrollbar_enabled = enabled;
    bridge->pending_scrollbar = true;
    hwnd = bridge->hwnd;
    headless = bridge->host_headless;
    LeaveCriticalSection(&bridge->input_lock);

    if (hwnd != 0 && !headless) {
        PostMessageW(hwnd, WINXTERM_WM_SCROLLBAR_UPDATE, 0, 0);
    }
}

bool winxterm_bridge_take_pending_scrollbar(WinxtermBridge *bridge, bool *enabled)
{
    if (bridge == 0 || enabled == 0) {
        return false;
    }

    bool has_scrollbar = false;
    EnterCriticalSection(&bridge->input_lock);
    if (bridge->pending_scrollbar) {
        *enabled = bridge->pending_scrollbar_enabled;
        bridge->pending_scrollbar = false;
        has_scrollbar = true;
    }
    LeaveCriticalSection(&bridge->input_lock);
    return has_scrollbar;
}

static wchar_t *winxterm_bridge_clone_wide(const wchar_t *text)
{
    if (text == 0) {
        return 0;
    }
    size_t length = wcslen(text);
    wchar_t *copy = (wchar_t *)calloc(length + 1u, sizeof(*copy));
    if (copy == 0) {
        return 0;
    }
    memcpy(copy, text, (length + 1u) * sizeof(*copy));
    return copy;
}

bool winxterm_bridge_request_macro(WinxtermBridge *bridge, const wchar_t *path)
{
    if (bridge == 0 || path == 0 || path[0] == L'\0') {
        return false;
    }
    wchar_t *path_copy = winxterm_bridge_clone_wide(path);
    if (path_copy == 0) {
        return false;
    }

    HWND hwnd = 0;
    bool post = false;
    EnterCriticalSection(&bridge->input_lock);
    free(bridge->pending_macro_path);
    bridge->pending_macro_path = path_copy;
    hwnd = bridge->hwnd;
    if (hwnd != 0 && !bridge->host_headless && !bridge->macro_update_pending) {
        bridge->macro_update_pending = true;
        post = true;
    }
    LeaveCriticalSection(&bridge->input_lock);

    if (post) {
        PostMessageW(hwnd, WINXTERM_WM_MACRO_UPDATE, 0, 0);
    }
    return true;
}

wchar_t *winxterm_bridge_take_macro_request(WinxtermBridge *bridge)
{
    if (bridge == 0) {
        return 0;
    }
    wchar_t *path = 0;
    EnterCriticalSection(&bridge->input_lock);
    path = bridge->pending_macro_path;
    bridge->pending_macro_path = 0;
    bridge->macro_update_pending = false;
    LeaveCriticalSection(&bridge->input_lock);
    return path;
}

bool winxterm_bridge_ack_pending_resize(WinxtermBridge *bridge, int columns, int rows)
{
    if (bridge == 0) {
        return false;
    }

    bool cleared = false;
    EnterCriticalSection(&bridge->input_lock);
    ++bridge->resize_applied_count;
    if (bridge->pending_resize &&
        bridge->pending_resize_columns == columns &&
        bridge->pending_resize_rows == rows) {
        bridge->pending_resize = false;
        cleared = true;
    }
    if (bridge->input_ready_event != 0 && bridge->input_count == 0u && !bridge->pending_resize) {
        ResetEvent(bridge->input_ready_event);
    }
    LeaveCriticalSection(&bridge->input_lock);
    return cleared;
}

static bool winxterm_bridge_grow_input_locked(WinxtermBridge *bridge, size_t required_capacity)
{
    if (bridge == 0 || required_capacity > WINXTERM_BRIDGE_INPUT_MAX_CAPACITY) {
        return false;
    }

    size_t new_capacity = bridge->input_capacity != 0u ?
        bridge->input_capacity : WINXTERM_BRIDGE_INPUT_INITIAL_CAPACITY;
    while (new_capacity < required_capacity) {
        if (new_capacity > WINXTERM_BRIDGE_INPUT_MAX_CAPACITY / 2u) {
            new_capacity = WINXTERM_BRIDGE_INPUT_MAX_CAPACITY;
        } else {
            new_capacity *= 2u;
        }
        if (new_capacity < required_capacity && new_capacity == WINXTERM_BRIDGE_INPUT_MAX_CAPACITY) {
            return false;
        }
    }

    uint8_t *new_buffer = (uint8_t *)malloc(new_capacity);
    if (new_buffer == 0) {
        return false;
    }

    for (size_t i = 0; i < bridge->input_count; ++i) {
        new_buffer[i] = bridge->input_buffer[(bridge->input_head + i) % bridge->input_capacity];
    }

    free(bridge->input_buffer);
    bridge->input_buffer = new_buffer;
    bridge->input_capacity = new_capacity;
    bridge->input_head = 0u;
    bridge->input_tail = bridge->input_count;
    ++bridge->input_grow_count;
    winxterm_log_writef(bridge->log, "input queue grown to %zu bytes", new_capacity);
    return true;
}

static bool winxterm_bridge_queue_input_internal(WinxtermBridge *bridge,
                                                 const uint8_t *bytes,
                                                 size_t byte_count)
{
    if (bridge == 0 || bytes == 0 || byte_count == 0u) {
        return false;
    }
    bool ok = true;
    EnterCriticalSection(&bridge->input_lock);
    if (bridge->host_headless || bridge->host_terminate_requested ||
        byte_count > WINXTERM_BRIDGE_INPUT_MAX_CAPACITY ||
        WINXTERM_BRIDGE_INPUT_MAX_CAPACITY - bridge->input_count < byte_count) {
        ok = false;
    } else if (bridge->input_capacity - bridge->input_count < byte_count) {
        ok = winxterm_bridge_grow_input_locked(bridge, bridge->input_count + byte_count);
    }

    if (!ok) {
        ++bridge->input_enqueue_failures;
        if (SIZE_MAX - bridge->input_rejected_bytes < byte_count) {
            bridge->input_rejected_bytes = SIZE_MAX;
        } else {
            bridge->input_rejected_bytes += byte_count;
        }
        LeaveCriticalSection(&bridge->input_lock);
        return false;
    }

    for (size_t i = 0; i < byte_count; ++i) {
        bridge->input_buffer[bridge->input_tail] = bytes[i];
        bridge->input_tail = (bridge->input_tail + 1u) % bridge->input_capacity;
        ++bridge->input_count;
    }
    if (bridge->input_count > bridge->input_high_water) {
        bridge->input_high_water = bridge->input_count;
    }
    if (bridge->input_ready_event != 0 && bridge->input_count != 0u) {
        SetEvent(bridge->input_ready_event);
    }
    LeaveCriticalSection(&bridge->input_lock);
    return ok;
}

bool winxterm_bridge_queue_input(WinxtermBridge *bridge, const uint8_t *bytes, size_t byte_count)
{
    return winxterm_bridge_queue_input_internal(bridge, bytes, byte_count);
}

bool winxterm_bridge_queue_reply(WinxtermBridge *bridge, const uint8_t *bytes, size_t byte_count)
{
    return winxterm_bridge_queue_input(bridge, bytes, byte_count);
}

bool winxterm_bridge_switch_input_session(WinxtermBridge *bridge,
                                          uint64_t session_id,
                                          const uint8_t *pending_bytes,
                                          size_t pending_count,
                                          uint8_t **previous_bytes,
                                          size_t *previous_count)
{
    if (previous_bytes != 0) *previous_bytes = 0;
    if (previous_count != 0) *previous_count = 0u;
    if (bridge == 0 || previous_bytes == 0 || previous_count == 0 ||
        (pending_bytes == 0 && pending_count != 0u) ||
        pending_count > WINXTERM_BRIDGE_INPUT_MAX_CAPACITY) return false;

    EnterCriticalSection(&bridge->input_lock);
    if (bridge->input_session_id == 0u && pending_count == 0u) {
        bridge->input_session_id = session_id;
        LeaveCriticalSection(&bridge->input_lock);
        return true;
    }
    uint8_t *saved = bridge->input_count != 0u ?
        (uint8_t *)malloc(bridge->input_count) : 0;
    bool ok = bridge->input_count == 0u || saved != 0;
    if (ok && bridge->input_capacity < pending_count) {
        ok = winxterm_bridge_grow_input_locked(bridge, pending_count);
    }
    if (ok) {
        size_t saved_count = bridge->input_count;
        for (size_t i = 0u; i < saved_count; ++i) {
            saved[i] = bridge->input_buffer[(bridge->input_head + i) % bridge->input_capacity];
        }
        bridge->input_head = 0u;
        bridge->input_tail = 0u;
        bridge->input_count = 0u;
        for (size_t i = 0u; i < pending_count; ++i) {
            bridge->input_buffer[bridge->input_tail] = pending_bytes[i];
            bridge->input_tail = (bridge->input_tail + 1u) % bridge->input_capacity;
            ++bridge->input_count;
        }
        bridge->input_session_id = session_id;
        *previous_bytes = saved;
        *previous_count = saved_count;
        saved = 0;
        if (bridge->input_count > bridge->input_high_water) {
            bridge->input_high_water = bridge->input_count;
        }
        if (bridge->input_ready_event != 0) {
            if (bridge->input_count != 0u || bridge->pending_resize) {
                SetEvent(bridge->input_ready_event);
            } else {
                ResetEvent(bridge->input_ready_event);
            }
        }
    }
    LeaveCriticalSection(&bridge->input_lock);
    free(saved);
    return ok;
}

static bool winxterm_bridge_grow_output_locked(WinxtermBridge *bridge, size_t required_capacity)
{
    if (bridge == 0 || required_capacity > WINXTERM_BRIDGE_OUTPUT_MAX_CAPACITY) {
        return false;
    }
    size_t new_capacity = bridge->output_capacity != 0u ?
        bridge->output_capacity : WINXTERM_BRIDGE_OUTPUT_INITIAL_CAPACITY;
    while (new_capacity < required_capacity) {
        size_t doubled = new_capacity * 2u;
        if (doubled <= new_capacity || doubled > WINXTERM_BRIDGE_OUTPUT_MAX_CAPACITY) {
            new_capacity = WINXTERM_BRIDGE_OUTPUT_MAX_CAPACITY;
        } else {
            new_capacity = doubled;
        }
    }
    if (new_capacity < required_capacity) {
        return false;
    }
    uint8_t *new_buffer = (uint8_t *)malloc(new_capacity);
    if (new_buffer == 0) {
        return false;
    }
    if (bridge->output_buffer != 0 && bridge->output_count != 0u) {
        memcpy(new_buffer, bridge->output_buffer, bridge->output_count);
    }
    free(bridge->output_buffer);
    bridge->output_buffer = new_buffer;
    bridge->output_capacity = new_capacity;
    ++bridge->output_grow_count;
    return true;
}

static void winxterm_bridge_update_output_room_event_locked(WinxtermBridge *bridge)
{
    if (bridge == 0 || bridge->output_room_event == 0) {
        return;
    }
    if (bridge->host_headless ||
        bridge->host_terminate_requested ||
        bridge->output_count < WINXTERM_BRIDGE_OUTPUT_MAX_CAPACITY) {
        SetEvent(bridge->output_room_event);
    } else {
        ResetEvent(bridge->output_room_event);
    }
}

static void winxterm_bridge_record_transcript_locked(WinxtermBridge *bridge,
                                                     const uint8_t *bytes,
                                                     size_t byte_count)
{
    if (bridge == 0 || bytes == 0 || byte_count == 0u ||
        bridge->transcript_buffer == 0 || bridge->transcript_capacity == 0u) {
        return;
    }
    if (byte_count >= bridge->transcript_capacity) {
        const uint8_t *tail = bytes + byte_count - bridge->transcript_capacity;
        memcpy(bridge->transcript_buffer, tail, bridge->transcript_capacity);
        bridge->transcript_head = 0u;
        bridge->transcript_count = bridge->transcript_capacity;
        return;
    }

    if (bridge->transcript_count + byte_count > bridge->transcript_capacity) {
        size_t drop = bridge->transcript_count + byte_count - bridge->transcript_capacity;
        bridge->transcript_head = (bridge->transcript_head + drop) % bridge->transcript_capacity;
        bridge->transcript_count -= drop;
    }

    size_t tail = (bridge->transcript_head + bridge->transcript_count) % bridge->transcript_capacity;
    size_t first = bridge->transcript_capacity - tail;
    if (first > byte_count) {
        first = byte_count;
    }
    memcpy(bridge->transcript_buffer + tail, bytes, first);
    if (first < byte_count) {
        memcpy(bridge->transcript_buffer, bytes + first, byte_count - first);
    }
    bridge->transcript_count += byte_count;
}

bool winxterm_bridge_enqueue_output(WinxtermBridge *bridge,
                                    const uint8_t *bytes,
                                    size_t byte_count)
{
    if (bridge == 0 || bytes == 0 || byte_count == 0u) {
        return false;
    }

    bool ok = true;
    EnterCriticalSection(&bridge->input_lock);
    winxterm_bridge_record_transcript_locked(bridge, bytes, byte_count);
    if (bridge->host_headless || bridge->host_terminate_requested ||
        byte_count > WINXTERM_BRIDGE_OUTPUT_MAX_CAPACITY ||
        WINXTERM_BRIDGE_OUTPUT_MAX_CAPACITY - bridge->output_count < byte_count) {
        ok = false;
    } else if (bridge->output_capacity - bridge->output_count < byte_count) {
        ok = winxterm_bridge_grow_output_locked(bridge, bridge->output_count + byte_count);
    }
    if (ok) {
        memcpy(bridge->output_buffer + bridge->output_count, bytes, byte_count);
        bridge->output_count += byte_count;
        if (bridge->output_count > bridge->output_high_water) {
            bridge->output_high_water = bridge->output_count;
        }
    } else {
        ++bridge->output_enqueue_failures;
    }
    winxterm_bridge_update_output_room_event_locked(bridge);
    LeaveCriticalSection(&bridge->input_lock);
    if (ok) {
        winxterm_bridge_request_frame(bridge, WINXTERM_FRAME_CAUSE_CONTENT);
    }
    return ok;
}

void winxterm_bridge_set_active_session(WinxtermBridge *bridge, uint64_t session_id)
{
    if (bridge == 0) return;
    winxterm_job_coordinator_set_active_session(&bridge->job_coordinator, session_id);
}

uint64_t winxterm_bridge_active_session(WinxtermBridge *bridge)
{
    return bridge != 0 ?
        winxterm_job_coordinator_active_session(&bridge->job_coordinator) : 0u;
}

bool winxterm_bridge_request_job_action(WinxtermBridge *bridge,
                                       WinxtermBridgeJobAction action, uint64_t job_id)
{
    if (bridge == 0 || action == WINXTERM_BRIDGE_JOB_ACTION_NONE || job_id == 0u) return false;
    bool accepted = winxterm_job_coordinator_enqueue(
        &bridge->job_coordinator, (uint32_t)action, job_id);
    if (accepted && bridge->input_ready_event != 0) SetEvent(bridge->input_ready_event);
    return accepted;
}

bool winxterm_bridge_take_job_action(WinxtermBridge *bridge,
                                    WinxtermBridgeJobAction *action, uint64_t *job_id)
{
    if (bridge == 0 || action == 0 || job_id == 0) return false;
    uint32_t raw_action = 0u;
    bool available = winxterm_job_coordinator_take(
        &bridge->job_coordinator, &raw_action, job_id);
    if (available) *action = (WinxtermBridgeJobAction)raw_action;
    return available;
}

bool winxterm_bridge_publish_job_view(WinxtermBridge *bridge, uint64_t job_id,
                                      uint8_t *bytes, size_t byte_count)
{
    if (bridge == 0 || job_id == 0u || (bytes == 0 && byte_count != 0u)) return false;
    bool accepted = winxterm_job_coordinator_publish_view(
        &bridge->job_coordinator, job_id, bytes, byte_count);
    if (accepted && bridge->hwnd != 0) {
        PostMessageW(bridge->hwnd, WINXTERM_WM_JOB_VIEW, 0, 0);
    }
    return accepted;
}

bool winxterm_bridge_take_job_view(WinxtermBridge *bridge, uint64_t *job_id,
                                   uint8_t **bytes, size_t *byte_count)
{
    if (bridge == 0 || job_id == 0 || bytes == 0 || byte_count == 0) return false;
    return winxterm_job_coordinator_take_view(
        &bridge->job_coordinator, job_id, bytes, byte_count);
}

bool winxterm_bridge_request_job_ui(WinxtermBridge *bridge)
{
    if (bridge == 0) return false;
    EnterCriticalSection(&bridge->input_lock);
    HWND hwnd = bridge->hwnd;
    LeaveCriticalSection(&bridge->input_lock);
    return hwnd != 0 && PostMessageW(hwnd, WINXTERM_WM_JOB_UI, 0, 0) != FALSE;
}

bool winxterm_bridge_enqueue_output_wait(WinxtermBridge *bridge,
                                         const uint8_t *bytes,
                                         size_t byte_count,
                                         HANDLE shutdown_event)
{
    if (!winxterm_bridge_wait_for_output_room(bridge, byte_count, shutdown_event)) {
        return false;
    }
    return winxterm_bridge_enqueue_output(bridge, bytes, byte_count);
}

bool winxterm_bridge_commit_output(WinxtermBridge *bridge,
                                   size_t max_bytes,
                                   bool *content_changed,
                                   bool *more_pending,
                                   bool *presentation_changed)
{
    if (content_changed != 0) {
        *content_changed = false;
    }
    if (more_pending != 0) {
        *more_pending = false;
    }
    if (presentation_changed != 0) {
        *presentation_changed = false;
    }
    if (bridge == 0) {
        return false;
    }

    uint8_t *bytes = 0;
    size_t byte_count = 0u;
    EnterCriticalSection(&bridge->input_lock);
    if (!bridge->output_paused && bridge->output_count != 0u) {
        byte_count = bridge->output_count;
        if (max_bytes != 0u && byte_count > max_bytes) {
            byte_count = max_bytes;
        }
        bytes = (uint8_t *)malloc(byte_count);
        if (bytes != 0) {
            memcpy(bytes, bridge->output_buffer, byte_count);
            bridge->output_count -= byte_count;
            if (bridge->output_count != 0u) {
                memmove(bridge->output_buffer,
                        bridge->output_buffer + byte_count,
                        bridge->output_count);
            }
            winxterm_bridge_update_output_room_event_locked(bridge);
        } else {
            byte_count = 0u;
        }
    }
    bool still_pending = !bridge->output_paused && bridge->output_count != 0u;
    LeaveCriticalSection(&bridge->input_lock);

    if (bytes == 0 || byte_count == 0u) {
        if (more_pending != 0) {
            *more_pending = still_pending;
        }
        return bytes != 0 || byte_count == 0u;
    }

    WinxtermBridgeOutputCommit commit;
    memset(&commit, 0, sizeof(commit));
    commit.bridge = bridge;
    EnterCriticalSection(&bridge->screen_lock);
    uint64_t visual_lines_before = winxterm_screen_visual_line_advances(&bridge->screen);
    bool ok = winxterm_text_feed_bytes_to_sink(&bridge->output_decoder,
                                               bytes,
                                               byte_count,
                                               winxterm_bridge_terminal_sink,
                                               &commit);
    uint64_t visual_lines_after = winxterm_screen_visual_line_advances(&bridge->screen);
    uint64_t visual_delta = visual_lines_after - visual_lines_before;
    if (visual_delta > UINT_MAX) visual_delta = UINT_MAX;
    winxterm_bridge_add_unpainted_lines_locked(bridge, (unsigned int)visual_delta);
    LeaveCriticalSection(&bridge->screen_lock);
    free(bytes);

    if (content_changed != 0) {
        *content_changed = commit.content_changed;
    }
    if (presentation_changed != 0) {
        *presentation_changed = commit.presentation_changed;
    }
    if (more_pending != 0) {
        *more_pending = still_pending;
    }
    return ok;
}

size_t winxterm_bridge_read_input(WinxtermBridge *bridge, uint8_t *buffer, size_t buffer_capacity)
{
    if (bridge == 0 || buffer == 0 || buffer_capacity == 0u) {
        return 0u;
    }
    size_t copied = 0u;
    EnterCriticalSection(&bridge->input_lock);
    while (copied < buffer_capacity && bridge->input_count != 0u) {
        buffer[copied++] = bridge->input_buffer[bridge->input_head];
        bridge->input_head = (bridge->input_head + 1u) % bridge->input_capacity;
        --bridge->input_count;
    }
    if (bridge->input_ready_event != 0 && bridge->input_count == 0u && !bridge->pending_resize) {
        ResetEvent(bridge->input_ready_event);
    }
    LeaveCriticalSection(&bridge->input_lock);
    return copied;
}

size_t winxterm_bridge_read_session_input(WinxtermBridge *bridge, uint64_t session_id,
                                          uint8_t *buffer, size_t buffer_capacity)
{
    if (bridge == 0 || session_id == 0u || buffer == 0 || buffer_capacity == 0u) return 0u;
    size_t copied = 0u;
    EnterCriticalSection(&bridge->input_lock);
    if (bridge->input_session_id == session_id) {
        while (copied < buffer_capacity && bridge->input_count != 0u) {
            buffer[copied++] = bridge->input_buffer[bridge->input_head];
            bridge->input_head = (bridge->input_head + 1u) % bridge->input_capacity;
            --bridge->input_count;
        }
        if (bridge->input_ready_event != 0 && bridge->input_count == 0u &&
            !bridge->pending_resize) ResetEvent(bridge->input_ready_event);
    }
    LeaveCriticalSection(&bridge->input_lock);
    return copied;
}

void winxterm_bridge_clear_input(WinxtermBridge *bridge)
{
    if (bridge == 0) {
        return;
    }

    EnterCriticalSection(&bridge->input_lock);
    bridge->input_head = 0u;
    bridge->input_tail = 0u;
    bridge->input_count = 0u;
    if (bridge->input_ready_event != 0 && !bridge->pending_resize) {
        ResetEvent(bridge->input_ready_event);
    }
    LeaveCriticalSection(&bridge->input_lock);
}

void winxterm_bridge_set_title_utf8(WinxtermBridge *bridge, const char *title, size_t title_length)
{
    if (bridge == 0 || title == 0) {
        return;
    }
    EnterCriticalSection(&bridge->input_lock);
    if (title_length >= WINXTERM_BRIDGE_TITLE_CAPACITY) {
        title_length = WINXTERM_BRIDGE_TITLE_CAPACITY - 1u;
    }
    memcpy(bridge->terminal_title, title, title_length);
    bridge->terminal_title[title_length] = '\0';
    LeaveCriticalSection(&bridge->input_lock);
}

bool winxterm_bridge_note_bell(WinxtermBridge *bridge)
{
    if (bridge == 0) {
        return false;
    }
    EnterCriticalSection(&bridge->input_lock);
    if (!bridge->bell_enabled) {
        LeaveCriticalSection(&bridge->input_lock);
        return false;
    }
    bridge->bell_pending = true;
    LeaveCriticalSection(&bridge->input_lock);
    winxterm_bridge_request_frame(bridge, WINXTERM_FRAME_CAUSE_PRESENTATION);
    return true;
}

bool winxterm_bridge_take_bell(WinxtermBridge *bridge)
{
    if (bridge == 0) {
        return false;
    }
    bool pending = false;
    EnterCriticalSection(&bridge->input_lock);
    pending = bridge->bell_pending;
    bridge->bell_pending = false;
    LeaveCriticalSection(&bridge->input_lock);
    return pending;
}

void winxterm_bridge_set_bell_enabled(WinxtermBridge *bridge, bool enabled)
{
    if (bridge == 0) {
        return;
    }
    EnterCriticalSection(&bridge->input_lock);
    bridge->bell_enabled = enabled;
    if (!enabled) {
        bridge->bell_pending = false;
    }
    LeaveCriticalSection(&bridge->input_lock);
    winxterm_bridge_request_frame(bridge, WINXTERM_FRAME_CAUSE_PRESENTATION);
}

bool winxterm_bridge_bell_enabled(WinxtermBridge *bridge)
{
    if (bridge == 0) {
        return false;
    }
    bool enabled = false;
    EnterCriticalSection(&bridge->input_lock);
    enabled = bridge->bell_enabled;
    LeaveCriticalSection(&bridge->input_lock);
    return enabled;
}

bool winxterm_bridge_mode_enabled(WinxtermBridge *bridge, WinxtermTerminalMode mode)
{
    if (bridge == 0) {
        return false;
    }
    bool enabled = false;
    EnterCriticalSection(&bridge->screen_lock);
    enabled = winxterm_screen_mode_enabled(&bridge->screen, mode);
    LeaveCriticalSection(&bridge->screen_lock);
    return enabled;
}

bool winxterm_bridge_copy_mode_state(WinxtermBridge *bridge, WinxtermModeState *modes)
{
    if (bridge == 0 || modes == 0) {
        return false;
    }
    EnterCriticalSection(&bridge->screen_lock);
    *modes = bridge->screen.mode_state;
    LeaveCriticalSection(&bridge->screen_lock);
    return true;
}

void winxterm_bridge_set_backend(WinxtermBridge *bridge, WinxtermRenderBackend backend)
{
    if (bridge == 0 || backend < 0 || backend >= WINXTERM_RENDER_BACKEND_COUNT) {
        return;
    }

    bridge->backend = backend;
}

WinxtermRenderBackend winxterm_bridge_backend(WinxtermBridge *bridge)
{
    return bridge != 0 ? bridge->backend : WINXTERM_RENDER_BACKEND_SPANS;
}

void winxterm_bridge_set_unpainted_line_limit(WinxtermBridge *bridge, unsigned int line_limit)
{
    if (bridge == 0 || line_limit == 0u) {
        return;
    }

    EnterCriticalSection(&bridge->screen_lock);
    bridge->unpainted_line_limit = line_limit;
    if (!bridge->output_paused && bridge->unpainted_lines < bridge->unpainted_line_limit &&
        bridge->unpainted_below_limit_event != 0) {
        SetEvent(bridge->unpainted_below_limit_event);
    } else if (bridge->unpainted_below_limit_event != 0) {
        ResetEvent(bridge->unpainted_below_limit_event);
    }
    LeaveCriticalSection(&bridge->screen_lock);
}

void winxterm_bridge_set_output_paused(WinxtermBridge *bridge, bool paused)
{
    if (bridge == 0) {
        return;
    }

    bool request_content_frame = false;
    EnterCriticalSection(&bridge->input_lock);
    bridge->output_paused = paused;
    request_content_frame = !paused && bridge->output_count != 0u;
    LeaveCriticalSection(&bridge->input_lock);

    EnterCriticalSection(&bridge->screen_lock);
    if (bridge->unpainted_below_limit_event != 0) {
        if (paused || bridge->unpainted_lines >= bridge->unpainted_line_limit) {
            ResetEvent(bridge->unpainted_below_limit_event);
        } else {
            SetEvent(bridge->unpainted_below_limit_event);
        }
    }
    LeaveCriticalSection(&bridge->screen_lock);
    if (request_content_frame) {
        winxterm_bridge_request_frame(bridge, WINXTERM_FRAME_CAUSE_CONTENT);
    }
}

bool winxterm_bridge_wait_for_unpainted_budget(WinxtermBridge *bridge, HANDLE shutdown_event)
{
    if (bridge == 0 || bridge->unpainted_below_limit_event == 0) {
        return true;
    }

    if (WaitForSingleObject(bridge->unpainted_below_limit_event, 0) == WAIT_OBJECT_0) {
        bool available = false;
        EnterCriticalSection(&bridge->screen_lock);
        available = !bridge->output_paused && bridge->unpainted_lines < bridge->unpainted_line_limit;
        if (!available) {
            ResetEvent(bridge->unpainted_below_limit_event);
        }
        LeaveCriticalSection(&bridge->screen_lock);
        if (!available) {
            return winxterm_bridge_wait_for_unpainted_budget(bridge, shutdown_event);
        }
        return true;
    }

    DWORD wait_start = GetTickCount();
    EnterCriticalSection(&bridge->screen_lock);
    if (!bridge->backpressure_logged) {
        bridge->backpressure_logged = true;
        winxterm_log_writef(bridge->log,
                            "output backpressure active: unpainted=%u limit=%u",
                            bridge->unpainted_lines,
                            bridge->unpainted_line_limit);
    }
    LeaveCriticalSection(&bridge->screen_lock);

    DWORD wait_result = WAIT_FAILED;
    if (shutdown_event == 0) {
        wait_result = WaitForSingleObject(bridge->unpainted_below_limit_event, INFINITE);
    } else {
        HANDLE waits[2] = {bridge->unpainted_below_limit_event, shutdown_event};
        wait_result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
    }

    DWORD elapsed = GetTickCount() - wait_start;
    EnterCriticalSection(&bridge->screen_lock);
    ++bridge->backpressure_wait_count;
    if (ULONG_MAX - bridge->backpressure_total_wait_ms < elapsed) {
        bridge->backpressure_total_wait_ms = ULONG_MAX;
    } else {
        bridge->backpressure_total_wait_ms += elapsed;
    }
    if (elapsed > bridge->backpressure_longest_wait_ms) {
        bridge->backpressure_longest_wait_ms = elapsed;
    }
    LeaveCriticalSection(&bridge->screen_lock);
    return wait_result == WAIT_OBJECT_0;
}

bool winxterm_bridge_wait_for_output_room(WinxtermBridge *bridge,
                                          size_t byte_count,
                                          HANDLE shutdown_event)
{
    if (bridge == 0 || bridge->output_room_event == 0 || byte_count == 0u ||
        byte_count > WINXTERM_BRIDGE_OUTPUT_MAX_CAPACITY) {
        return false;
    }

    for (;;) {
        bool stopped = false;
        bool available = false;
        EnterCriticalSection(&bridge->input_lock);
        stopped = bridge->host_headless || bridge->host_terminate_requested;
        available = !stopped &&
                    bridge->output_count <= WINXTERM_BRIDGE_OUTPUT_MAX_CAPACITY &&
                    WINXTERM_BRIDGE_OUTPUT_MAX_CAPACITY - bridge->output_count >= byte_count;
        if (available) {
            SetEvent(bridge->output_room_event);
        } else if (!stopped) {
            ResetEvent(bridge->output_room_event);
        }
        LeaveCriticalSection(&bridge->input_lock);

        if (available) {
            return true;
        }
        if (stopped) {
            return false;
        }

        DWORD wait_result = WAIT_FAILED;
        if (shutdown_event == 0) {
            wait_result = WaitForSingleObject(bridge->output_room_event, INFINITE);
        } else {
            HANDLE waits[2] = {bridge->output_room_event, shutdown_event};
            wait_result = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        }
        if (wait_result != WAIT_OBJECT_0) {
            return false;
        }
    }
}

void winxterm_bridge_add_unpainted_lines(WinxtermBridge *bridge, unsigned int line_count)
{
    if (bridge == 0 || line_count == 0u) {
        return;
    }

    EnterCriticalSection(&bridge->screen_lock);
    winxterm_bridge_add_unpainted_lines_locked(bridge, line_count);
    LeaveCriticalSection(&bridge->screen_lock);
}

void winxterm_bridge_add_unpainted_lines_locked(WinxtermBridge *bridge, unsigned int line_count)
{
    if (bridge == 0 || line_count == 0u) {
        return;
    }

    if (UINT_MAX - bridge->unpainted_lines < line_count) {
        bridge->unpainted_lines = UINT_MAX;
    } else {
        bridge->unpainted_lines += line_count;
    }
    if ((bridge->output_paused || bridge->unpainted_lines >= bridge->unpainted_line_limit) &&
        bridge->unpainted_below_limit_event != 0) {
        ResetEvent(bridge->unpainted_below_limit_event);
    }
}

void winxterm_bridge_mark_painted(WinxtermBridge *bridge)
{
    if (bridge == 0) {
        return;
    }

    EnterCriticalSection(&bridge->screen_lock);
    winxterm_bridge_mark_painted_locked(bridge);
    LeaveCriticalSection(&bridge->screen_lock);
}

void winxterm_bridge_mark_painted_locked(WinxtermBridge *bridge)
{
    if (bridge == 0) {
        return;
    }

    bridge->unpainted_lines = 0;
    if (bridge->unpainted_below_limit_event != 0 && !bridge->output_paused) {
        SetEvent(bridge->unpainted_below_limit_event);
    }
}

void winxterm_bridge_note_frame(WinxtermBridge *bridge)
{
    if (bridge == 0) {
        return;
    }

    ++bridge->rendered_frames;
    ++bridge->fps_window_frames;
    DWORD now = GetTickCount();
    DWORD elapsed = now - bridge->fps_window_start_tick;
    if (elapsed >= 1000u) {
        bridge->last_fps = (double)bridge->fps_window_frames * 1000.0 / (double)elapsed;
        bridge->fps_window_frames = 0;
        bridge->fps_window_start_tick = now;
    }
}

void winxterm_bridge_set_host_starting(WinxtermBridge *bridge)
{
    if (bridge == 0) {
        return;
    }

    EnterCriticalSection(&bridge->input_lock);
    bridge->host_state = WINXTERM_HOST_STATE_STARTING;
    bridge->host_child_running = false;
    bridge->host_child_is_shell = false;
    bridge->host_headless = false;
    bridge->host_terminate_requested = false;
    bridge->host_child_process_id = 0;
    bridge->host_child_display_name[0] = L'\0';
    LeaveCriticalSection(&bridge->input_lock);
    winxterm_log_writef(bridge->log, "host state: starting");
}

void winxterm_bridge_set_host_child(WinxtermBridge *bridge, DWORD process_id,
                                    const wchar_t *display_name, bool is_shell)
{
    if (bridge == 0) {
        return;
    }

    EnterCriticalSection(&bridge->input_lock);
    bridge->host_state = bridge->host_headless ?
        WINXTERM_HOST_STATE_RUNNING_HEADLESS : WINXTERM_HOST_STATE_RUNNING_VISIBLE;
    bridge->host_child_running = true;
    bridge->host_child_is_shell = is_shell;
    bridge->host_child_process_id = process_id;
    if (display_name != 0 && display_name[0] != L'\0') {
        wcsncpy_s(bridge->host_child_display_name,
                  WINXTERM_BRIDGE_CHILD_NAME_CAPACITY,
                  display_name,
                  _TRUNCATE);
    } else {
        wcsncpy_s(bridge->host_child_display_name,
                  WINXTERM_BRIDGE_CHILD_NAME_CAPACITY,
                  L"(unknown)",
                  _TRUNCATE);
    }
    LeaveCriticalSection(&bridge->input_lock);
    winxterm_log_writef(bridge->log,
                        "host state: running direct_child_pid=%lu",
                        (unsigned long)process_id);
}

void winxterm_bridge_set_host_state(WinxtermBridge *bridge, WinxtermHostState state)
{
    if (bridge == 0) {
        return;
    }

    EnterCriticalSection(&bridge->input_lock);
    bridge->host_state = state;
    LeaveCriticalSection(&bridge->input_lock);
    winxterm_log_writef(bridge->log, "host state: %d", (int)state);
}

void winxterm_bridge_clear_host_child(WinxtermBridge *bridge, WinxtermHostState final_state)
{
    if (bridge == 0) {
        return;
    }

    EnterCriticalSection(&bridge->input_lock);
    bridge->host_state = final_state;
    bridge->host_child_running = false;
    bridge->host_child_is_shell = false;
    bridge->host_headless = false;
    bridge->host_terminate_requested = false;
    bridge->host_child_process_id = 0;
    bridge->host_child_display_name[0] = L'\0';
    LeaveCriticalSection(&bridge->input_lock);
    winxterm_log_writef(bridge->log, "host state: %d", (int)final_state);
}

bool winxterm_bridge_copy_child_info(WinxtermBridge *bridge, WinxtermHostChildInfo *info)
{
    if (bridge == 0 || info == 0) {
        return false;
    }

    EnterCriticalSection(&bridge->input_lock);
    info->running = bridge->host_child_running;
    info->is_shell = bridge->host_child_is_shell;
    info->state = bridge->host_state;
    info->process_id = bridge->host_child_process_id;
    wcsncpy_s(info->display_name,
              WINXTERM_BRIDGE_CHILD_NAME_CAPACITY,
              bridge->host_child_display_name,
              _TRUNCATE);
    LeaveCriticalSection(&bridge->input_lock);
    return info->running;
}

void winxterm_bridge_request_headless(WinxtermBridge *bridge)
{
    if (bridge == 0) {
        return;
    }

    EnterCriticalSection(&bridge->input_lock);
    bridge->host_headless = true;
    bridge->host_state = WINXTERM_HOST_STATE_RUNNING_HEADLESS;
    bridge->input_head = 0u;
    bridge->input_tail = 0u;
    bridge->input_count = 0u;
    bridge->output_count = 0u;
    bridge->pending_frame_causes = WINXTERM_FRAME_CAUSE_NONE;
    HWND hwnd = bridge->hwnd;
    bridge->hwnd = 0;
    winxterm_bridge_update_output_room_event_locked(bridge);
    if (bridge->input_ready_event != 0 && !bridge->pending_resize) {
        ResetEvent(bridge->input_ready_event);
    }
    LeaveCriticalSection(&bridge->input_lock);

    EnterCriticalSection(&bridge->screen_lock);
    bridge->unpainted_lines = 0u;
    if (bridge->unpainted_below_limit_event != 0) {
        SetEvent(bridge->unpainted_below_limit_event);
    }
    LeaveCriticalSection(&bridge->screen_lock);

    winxterm_log_writef(bridge->log,
                        "host state: running headless hwnd=%p",
                        (void *)hwnd);
}

void winxterm_bridge_request_terminate(WinxtermBridge *bridge)
{
    if (bridge == 0) {
        return;
    }

    EnterCriticalSection(&bridge->input_lock);
    bridge->host_terminate_requested = true;
    bridge->host_state = WINXTERM_HOST_STATE_SHUTDOWN_REQUESTED;
    bridge->input_head = 0u;
    bridge->input_tail = 0u;
    bridge->input_count = 0u;
    bridge->output_count = 0u;
    bridge->pending_frame_causes = WINXTERM_FRAME_CAUSE_NONE;
    winxterm_bridge_update_output_room_event_locked(bridge);
    if (bridge->input_ready_event != 0 && !bridge->pending_resize) {
        ResetEvent(bridge->input_ready_event);
    }
    LeaveCriticalSection(&bridge->input_lock);
    winxterm_log_writef(bridge->log, "host state: shutdown requested");
}

bool winxterm_bridge_is_headless(WinxtermBridge *bridge)
{
    if (bridge == 0) {
        return false;
    }

    EnterCriticalSection(&bridge->input_lock);
    bool headless = bridge->host_headless;
    LeaveCriticalSection(&bridge->input_lock);
    return headless;
}

bool winxterm_bridge_terminate_requested(WinxtermBridge *bridge)
{
    if (bridge == 0) {
        return false;
    }

    EnterCriticalSection(&bridge->input_lock);
    bool requested = bridge->host_terminate_requested;
    LeaveCriticalSection(&bridge->input_lock);
    return requested;
}

void winxterm_bridge_note_headless_output(WinxtermBridge *bridge, const uint8_t *bytes, size_t byte_count)
{
    if (bridge == 0 || byte_count == 0u) {
        return;
    }

    unsigned int lines = 0u;
    if (bytes != 0) {
        for (size_t i = 0; i < byte_count; ++i) {
            if (bytes[i] == '\n' && lines != UINT_MAX) {
                ++lines;
            }
        }
    }

    EnterCriticalSection(&bridge->input_lock);
    winxterm_bridge_record_transcript_locked(bridge, bytes, byte_count);
    if (ULLONG_MAX - bridge->headless_output_bytes < (unsigned long long)byte_count) {
        bridge->headless_output_bytes = ULLONG_MAX;
    } else {
        bridge->headless_output_bytes += (unsigned long long)byte_count;
    }
    if (UINT_MAX - bridge->headless_output_lines < lines) {
        bridge->headless_output_lines = UINT_MAX;
    } else {
        bridge->headless_output_lines += lines;
    }
    LeaveCriticalSection(&bridge->input_lock);
}

bool winxterm_bridge_copy_transcript(WinxtermBridge *bridge, uint8_t **bytes, size_t *byte_count)
{
    if (bytes != 0) {
        *bytes = 0;
    }
    if (byte_count != 0) {
        *byte_count = 0u;
    }
    if (bridge == 0 || bytes == 0 || byte_count == 0) {
        return false;
    }

    EnterCriticalSection(&bridge->input_lock);
    size_t count = bridge->transcript_count;
    uint8_t *copy = (uint8_t *)malloc(count != 0u ? count : 1u);
    if (copy != 0 && count != 0u) {
        size_t first = bridge->transcript_capacity - bridge->transcript_head;
        if (first > count) {
            first = count;
        }
        memcpy(copy, bridge->transcript_buffer + bridge->transcript_head, first);
        if (first < count) {
            memcpy(copy + first, bridge->transcript_buffer, count - first);
        }
    }
    LeaveCriticalSection(&bridge->input_lock);

    if (copy == 0) {
        return false;
    }
    *bytes = copy;
    *byte_count = count;
    return true;
}

void winxterm_bridge_copy_diagnostics(WinxtermBridge *bridge, WinxtermBridgeDiagnostics *diagnostics)
{
    if (bridge == 0 || diagnostics == 0) {
        return;
    }

    memset(diagnostics, 0, sizeof(*diagnostics));
    EnterCriticalSection(&bridge->input_lock);
    diagnostics->input_capacity = bridge->input_capacity;
    diagnostics->input_count = bridge->input_count;
    diagnostics->input_high_water = bridge->input_high_water;
    diagnostics->input_grow_count = bridge->input_grow_count;
    diagnostics->input_enqueue_failures = bridge->input_enqueue_failures;
    diagnostics->input_rejected_bytes = bridge->input_rejected_bytes;
    diagnostics->resize_request_count = bridge->resize_request_count;
    diagnostics->resize_coalesced_count = bridge->resize_coalesced_count;
    diagnostics->resize_taken_count = bridge->resize_taken_count;
    diagnostics->resize_applied_count = bridge->resize_applied_count;
    diagnostics->scale_request_count = bridge->scale_request_count;
    diagnostics->scale_coalesced_count = bridge->scale_coalesced_count;
    diagnostics->scale_taken_count = bridge->scale_taken_count;
    diagnostics->render_update_request_count = bridge->render_update_request_count;
    diagnostics->render_update_coalesced_count = bridge->render_update_coalesced_count;
    diagnostics->render_update_taken_count = bridge->render_update_taken_count;
    diagnostics->output_batch_count = bridge->output_batch_count;
    diagnostics->output_batch_bytes = bridge->output_batch_bytes;
    diagnostics->output_batch_max_bytes = bridge->output_batch_max_bytes;
    diagnostics->output_queue_capacity = bridge->output_capacity;
    diagnostics->output_queue_count = bridge->output_count;
    diagnostics->output_queue_high_water = bridge->output_high_water;
    diagnostics->output_queue_grow_count = bridge->output_grow_count;
    diagnostics->output_queue_enqueue_failures = bridge->output_enqueue_failures;
    diagnostics->headless_output_bytes = bridge->headless_output_bytes;
    diagnostics->headless_output_lines = bridge->headless_output_lines;
    LeaveCriticalSection(&bridge->input_lock);

    EnterCriticalSection(&bridge->screen_lock);
    diagnostics->backpressure_wait_count = bridge->backpressure_wait_count;
    diagnostics->backpressure_total_wait_ms = bridge->backpressure_total_wait_ms;
    diagnostics->backpressure_longest_wait_ms = bridge->backpressure_longest_wait_ms;
    LeaveCriticalSection(&bridge->screen_lock);
}
