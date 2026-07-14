#ifndef WINXTERM_BRIDGE_H
#define WINXTERM_BRIDGE_H

#include "winxterm_diagnostics.h"
#include "winxterm_log.h"
#include "winxterm_job_manager.h"
#include "winxterm_job_coordinator.h"
#include "winxterm_screen.h"
#include "winxterm_text.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <windows.h>

#define WINXTERM_WM_RENDER_UPDATE (WM_APP + 1u)
#define WINXTERM_WM_SCALE_UPDATE (WM_APP + 2u)
#define WINXTERM_WM_MACRO_UPDATE (WM_APP + 3u)
#define WINXTERM_WM_JOB_VIEW (WM_APP + 4u)
#define WINXTERM_WM_JOB_UI (WM_APP + 5u)
#define WINXTERM_WM_SCROLLBAR_UPDATE (WM_APP + 6u)
#define WINXTERM_BRIDGE_INPUT_INITIAL_CAPACITY 16384u
#define WINXTERM_BRIDGE_INPUT_MAX_CAPACITY (16u * 1024u * 1024u)
#define WINXTERM_BRIDGE_OUTPUT_INITIAL_CAPACITY 65536u
#define WINXTERM_BRIDGE_OUTPUT_MAX_CAPACITY (16u * 1024u * 1024u)
#define WINXTERM_BRIDGE_TRANSCRIPT_CAPACITY (4u * 1024u * 1024u)
#define WINXTERM_BRIDGE_OUTPUT_COMMIT_MAX_BYTES 65536u
#define WINXTERM_BRIDGE_TITLE_CAPACITY 256u
#define WINXTERM_BRIDGE_CHILD_NAME_CAPACITY 512u
#define WINXTERM_BRIDGE_JOB_ACTION_QUEUE_LIMIT 1024u

typedef enum WinxtermFrameCause {
    WINXTERM_FRAME_CAUSE_NONE = 0u,
    WINXTERM_FRAME_CAUSE_CONTENT = 0x01u,
    WINXTERM_FRAME_CAUSE_RESIZE = 0x02u,
    WINXTERM_FRAME_CAUSE_PRESENTATION = 0x04u
} WinxtermFrameCause;

typedef enum WinxtermHostState {
    WINXTERM_HOST_STATE_NOT_STARTED = 0,
    WINXTERM_HOST_STATE_STARTING,
    WINXTERM_HOST_STATE_RUNNING_VISIBLE,
    WINXTERM_HOST_STATE_RUNNING_HEADLESS,
    WINXTERM_HOST_STATE_CHILD_EXITED,
    WINXTERM_HOST_STATE_SHUTDOWN_REQUESTED,
    WINXTERM_HOST_STATE_CLEANING_UP,
    WINXTERM_HOST_STATE_STOPPED,
    WINXTERM_HOST_STATE_FAILED
} WinxtermHostState;

typedef enum WinxtermBridgeJobAction {
    WINXTERM_BRIDGE_JOB_ACTION_NONE = 0,
    WINXTERM_BRIDGE_JOB_ACTION_FOREGROUND,
    WINXTERM_BRIDGE_JOB_ACTION_VIEW,
    WINXTERM_BRIDGE_JOB_ACTION_BACKGROUND,
    WINXTERM_BRIDGE_JOB_ACTION_CLOSE,
    WINXTERM_BRIDGE_JOB_ACTION_FORCE_EXIT
} WinxtermBridgeJobAction;

typedef struct WinxtermHostChildInfo {
    bool running;
    bool is_shell;
    WinxtermHostState state;
    DWORD process_id;
    wchar_t display_name[WINXTERM_BRIDGE_CHILD_NAME_CAPACITY];
} WinxtermHostChildInfo;

typedef struct WinxtermBridgeDiagnostics {
    size_t input_capacity;
    size_t input_count;
    size_t input_high_water;
    size_t input_grow_count;
    size_t input_enqueue_failures;
    size_t input_rejected_bytes;
    unsigned int resize_request_count;
    unsigned int resize_coalesced_count;
    unsigned int resize_taken_count;
    unsigned int resize_applied_count;
    unsigned int scale_request_count;
    unsigned int scale_coalesced_count;
    unsigned int scale_taken_count;
    unsigned long long render_update_request_count;
    unsigned long long render_update_coalesced_count;
    unsigned long long render_update_taken_count;
    unsigned long long output_batch_count;
    unsigned long long output_batch_bytes;
    unsigned long long output_batch_max_bytes;
    size_t output_queue_capacity;
    size_t output_queue_count;
    size_t output_queue_high_water;
    size_t output_queue_grow_count;
    size_t output_queue_enqueue_failures;
    unsigned int backpressure_wait_count;
    DWORD backpressure_total_wait_ms;
    DWORD backpressure_longest_wait_ms;
    unsigned long long headless_output_bytes;
    unsigned int headless_output_lines;
} WinxtermBridgeDiagnostics;

typedef struct WinxtermBridge {
    WinxtermLog *log;
    WinxtermJobManager job_manager;
    WinxtermScreen screen;
    CRITICAL_SECTION screen_lock;
    CRITICAL_SECTION input_lock;
    bool screen_lock_initialized;
    bool input_lock_initialized;
    HANDLE hwnd_ready_event;
    HANDLE unpainted_below_limit_event;
    HANDLE output_room_event;
    HANDLE input_ready_event;
    HWND hwnd;
    WinxtermRenderBackend backend;
    bool cycle_render_backends;
    bool show_render_stats_in_title;
    unsigned int unpainted_line_limit;
    unsigned int unpainted_lines;
    uint8_t *input_buffer;
    uint8_t *output_buffer;
    size_t input_capacity;
    size_t input_head;
    size_t input_tail;
    size_t input_count;
    uint64_t input_session_id;
    size_t output_capacity;
    size_t output_count;
    size_t output_high_water;
    size_t output_grow_count;
    size_t output_enqueue_failures;
    uint8_t *transcript_buffer;
    size_t transcript_capacity;
    size_t transcript_head;
    size_t transcript_count;
    WinxtermUtf8Decoder output_decoder;
    size_t input_high_water;
    size_t input_grow_count;
    size_t input_enqueue_failures;
    size_t input_rejected_bytes;
    int pending_resize_columns;
    int pending_resize_rows;
    bool pending_resize;
    unsigned int pending_display_scale;
    bool pending_scale;
    bool pending_scrollbar_enabled;
    bool pending_scrollbar;
    bool render_update_pending;
    unsigned int pending_frame_causes;
    bool output_paused;
    bool macro_update_pending;
    WinxtermJobCoordinator job_coordinator;
    wchar_t *pending_macro_path;
    unsigned int resize_request_count;
    unsigned int resize_coalesced_count;
    unsigned int resize_taken_count;
    unsigned int resize_applied_count;
    unsigned int scale_request_count;
    unsigned int scale_coalesced_count;
    unsigned int scale_taken_count;
    unsigned long long render_update_request_count;
    unsigned long long render_update_coalesced_count;
    unsigned long long render_update_taken_count;
    unsigned long long output_batch_count;
    unsigned long long output_batch_bytes;
    unsigned long long output_batch_max_bytes;
    char terminal_title[WINXTERM_BRIDGE_TITLE_CAPACITY];
    bool bell_pending;
    bool bell_enabled;
    uint64_t rendered_frames;
    DWORD fps_window_start_tick;
    uint32_t fps_window_frames;
    double last_fps;
    bool host_child_running;
    bool host_child_is_shell;
    bool host_headless;
    bool host_terminate_requested;
    WinxtermHostState host_state;
    DWORD host_child_process_id;
    wchar_t host_child_display_name[WINXTERM_BRIDGE_CHILD_NAME_CAPACITY];
    bool backpressure_logged;
    unsigned int backpressure_wait_count;
    DWORD backpressure_total_wait_ms;
    DWORD backpressure_longest_wait_ms;
    unsigned long long headless_output_bytes;
    unsigned int headless_output_lines;
} WinxtermBridge;

bool winxterm_bridge_init(WinxtermBridge *bridge, WinxtermLog *log, int columns, int rows);
void winxterm_bridge_dispose(WinxtermBridge *bridge);
void winxterm_bridge_set_hwnd(WinxtermBridge *bridge, HWND hwnd);
void winxterm_bridge_post_update(WinxtermBridge *bridge);
void winxterm_bridge_request_frame(WinxtermBridge *bridge, unsigned int causes);
bool winxterm_bridge_take_render_update(WinxtermBridge *bridge);
unsigned int winxterm_bridge_take_frame_request(WinxtermBridge *bridge);
void winxterm_bridge_resize_terminal(WinxtermBridge *bridge, int columns, int rows);
void winxterm_bridge_queue_terminal_resize(WinxtermBridge *bridge, int columns, int rows);
bool winxterm_bridge_peek_pending_resize(WinxtermBridge *bridge, int *columns, int *rows);
bool winxterm_bridge_ack_pending_resize(WinxtermBridge *bridge, int columns, int rows);
void winxterm_bridge_request_display_scale(WinxtermBridge *bridge, unsigned int scale);
bool winxterm_bridge_take_pending_display_scale(WinxtermBridge *bridge, unsigned int *scale);
void winxterm_bridge_request_scrollbar(WinxtermBridge *bridge, bool enabled);
bool winxterm_bridge_take_pending_scrollbar(WinxtermBridge *bridge, bool *enabled);
bool winxterm_bridge_request_macro(WinxtermBridge *bridge, const wchar_t *path);
wchar_t *winxterm_bridge_take_macro_request(WinxtermBridge *bridge);
bool winxterm_bridge_queue_input(WinxtermBridge *bridge, const uint8_t *bytes, size_t byte_count);
bool winxterm_bridge_queue_reply(WinxtermBridge *bridge, const uint8_t *bytes, size_t byte_count);
bool winxterm_bridge_switch_input_session(WinxtermBridge *bridge,
                                          uint64_t session_id,
                                          const uint8_t *pending_bytes,
                                          size_t pending_count,
                                          uint8_t **previous_bytes,
                                          size_t *previous_count);
bool winxterm_bridge_enqueue_output(WinxtermBridge *bridge, const uint8_t *bytes, size_t byte_count);
bool winxterm_bridge_request_job_action(WinxtermBridge *bridge,
                                       WinxtermBridgeJobAction action, uint64_t job_id);
bool winxterm_bridge_take_job_action(WinxtermBridge *bridge,
                                    WinxtermBridgeJobAction *action, uint64_t *job_id);
bool winxterm_bridge_publish_job_view(WinxtermBridge *bridge, uint64_t job_id,
                                      uint8_t *bytes, size_t byte_count);
bool winxterm_bridge_take_job_view(WinxtermBridge *bridge, uint64_t *job_id,
                                   uint8_t **bytes, size_t *byte_count);
bool winxterm_bridge_request_job_ui(WinxtermBridge *bridge);
bool winxterm_bridge_commit_output(WinxtermBridge *bridge,
                                   size_t max_bytes,
                                   bool *content_changed,
                                   bool *more_pending,
                                   bool *presentation_changed);
size_t winxterm_bridge_read_input(WinxtermBridge *bridge, uint8_t *buffer, size_t buffer_capacity);
size_t winxterm_bridge_read_session_input(WinxtermBridge *bridge, uint64_t session_id,
                                          uint8_t *buffer, size_t buffer_capacity);
void winxterm_bridge_clear_input(WinxtermBridge *bridge);
void winxterm_bridge_set_title_utf8(WinxtermBridge *bridge, const char *title, size_t title_length);
void winxterm_bridge_set_active_session(WinxtermBridge *bridge, uint64_t session_id);
uint64_t winxterm_bridge_active_session(WinxtermBridge *bridge);
bool winxterm_bridge_note_bell(WinxtermBridge *bridge);
bool winxterm_bridge_take_bell(WinxtermBridge *bridge);
void winxterm_bridge_set_bell_enabled(WinxtermBridge *bridge, bool enabled);
bool winxterm_bridge_bell_enabled(WinxtermBridge *bridge);
bool winxterm_bridge_mode_enabled(WinxtermBridge *bridge, WinxtermTerminalMode mode);
bool winxterm_bridge_copy_mode_state(WinxtermBridge *bridge, WinxtermModeState *modes);
void winxterm_bridge_set_backend(WinxtermBridge *bridge, WinxtermRenderBackend backend);
WinxtermRenderBackend winxterm_bridge_backend(WinxtermBridge *bridge);
void winxterm_bridge_set_unpainted_line_limit(WinxtermBridge *bridge, unsigned int line_limit);
void winxterm_bridge_set_output_paused(WinxtermBridge *bridge, bool paused);
bool winxterm_bridge_wait_for_unpainted_budget(WinxtermBridge *bridge, HANDLE shutdown_event);
bool winxterm_bridge_wait_for_output_room(WinxtermBridge *bridge,
                                          size_t byte_count,
                                          HANDLE shutdown_event);
bool winxterm_bridge_enqueue_output_wait(WinxtermBridge *bridge,
                                         const uint8_t *bytes,
                                         size_t byte_count,
                                         HANDLE shutdown_event);
void winxterm_bridge_add_unpainted_lines(WinxtermBridge *bridge, unsigned int line_count);
void winxterm_bridge_add_unpainted_lines_locked(WinxtermBridge *bridge, unsigned int line_count);
void winxterm_bridge_mark_painted(WinxtermBridge *bridge);
void winxterm_bridge_mark_painted_locked(WinxtermBridge *bridge);
void winxterm_bridge_note_frame(WinxtermBridge *bridge);
void winxterm_bridge_set_host_starting(WinxtermBridge *bridge);
void winxterm_bridge_set_host_child(WinxtermBridge *bridge, DWORD process_id,
                                    const wchar_t *display_name, bool is_shell);
void winxterm_bridge_set_host_state(WinxtermBridge *bridge, WinxtermHostState state);
void winxterm_bridge_clear_host_child(WinxtermBridge *bridge, WinxtermHostState final_state);
bool winxterm_bridge_copy_child_info(WinxtermBridge *bridge, WinxtermHostChildInfo *info);
void winxterm_bridge_request_headless(WinxtermBridge *bridge);
void winxterm_bridge_request_terminate(WinxtermBridge *bridge);
bool winxterm_bridge_is_headless(WinxtermBridge *bridge);
bool winxterm_bridge_terminate_requested(WinxtermBridge *bridge);
void winxterm_bridge_note_headless_output(WinxtermBridge *bridge, const uint8_t *bytes, size_t byte_count);
bool winxterm_bridge_copy_transcript(WinxtermBridge *bridge, uint8_t **bytes, size_t *byte_count);
void winxterm_bridge_copy_diagnostics(WinxtermBridge *bridge, WinxtermBridgeDiagnostics *diagnostics);
uint64_t winxterm_bridge_timestamp_ns(void);
void winxterm_bridge_note_output_batch(WinxtermBridge *bridge, size_t byte_count);

#endif
