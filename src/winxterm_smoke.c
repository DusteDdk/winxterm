#include "winxterm_smoke.h"

#include "winxterm_font_6x13.h"
#include "winxterm_glyph_fallback.h"
#include "winxterm_bridge.h"
#include "winxterm_client.h"
#include "winxterm_clipboard.h"
#include "winxterm_frame_scheduler.h"
#include "winxterm_input.h"
#include "winxterm_job_channel.h"
#include "winxterm_job_journal.h"
#include "winxterm_job_plan.h"
#include "winxterm_job_manager.h"
#include "winxterm_job_protocol.h"
#include "winxterm_log.h"
#include "winxterm_macro.h"
#include "winxterm_modes.h"
#include "winxterm_mouse.h"
#include "winxterm_options.h"
#include "winxterm_pty.h"
#include "winxterm_replies.h"
#include "winxterm_render.h"
#include "winxterm_scale.h"
#include "winxterm_screen.h"
#include "winxterm_text.h"
#include "winxterm_terminal_session.h"
#include "winxterm_transfer_format.h"
#include "dstcmd/winxterm_dstcmd_job_client.h"
#include "winxterm_settings.h"
#include "winxterm_surface.h"
#include "winxterm_ux.h"
#include "winxterm_window_placement.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>


typedef struct WinxtermSmokeState {
    int failures;
} WinxtermSmokeState;

typedef struct WinxtermProtocolCapture {
    WinxtermScreen *screen;
    WinxtermTerminalOp ops[128];
    size_t op_count;
    uint8_t replies[1024];
    size_t reply_count;
} WinxtermProtocolCapture;

typedef struct WinxtermJobStressContext {
    WinxtermJobManager *manager;
    uint64_t requester_id;
    uint64_t job_id;
    volatile LONG failures;
} WinxtermJobStressContext;

typedef struct WinxtermOutputCommitStressContext {
    WinxtermBridge *bridge;
    HANDLE start_event;
    volatile LONG failures;
} WinxtermOutputCommitStressContext;

static DWORD WINAPI winxterm_smoke_output_commit_thread(void *context)
{
    WinxtermOutputCommitStressContext *stress =
        (WinxtermOutputCommitStressContext *)context;
    if (WaitForSingleObject(stress->start_event, 5000u) != WAIT_OBJECT_0) {
        InterlockedIncrement(&stress->failures);
        return 1u;
    }
    bool more = false;
    do {
        bool changed = false;
        bool presentation = false;
        if (!winxterm_bridge_commit_output(stress->bridge, 1u, &changed,
                                           &more, &presentation)) {
            InterlockedIncrement(&stress->failures);
            return 1u;
        }
    } while (more);
    return 0u;
}

static DWORD WINAPI winxterm_smoke_job_snapshot_thread(void *context)
{
    WinxtermJobStressContext *stress = (WinxtermJobStressContext *)context;
    for (size_t i = 0u; i < 2000u; ++i) {
        WinxtermManagedJobSnapshot *snapshot = 0;
        size_t count = 0u;
        if (!winxterm_job_manager_snapshot(stress->manager, stress->requester_id,
                                           &snapshot, &count) || count == 0u) {
            InterlockedIncrement(&stress->failures);
        }
        winxterm_job_manager_snapshot_dispose(snapshot);
    }
    return 0u;
}

static DWORD WINAPI winxterm_smoke_job_metadata_thread(void *context)
{
    WinxtermJobStressContext *stress = (WinxtermJobStressContext *)context;
    for (size_t i = 0u; i < 2000u; ++i) {
        if (!winxterm_job_manager_set_output(stress->manager, stress->job_id,
                                             i % 4096u, (i % 97u) == 0u)) {
            InterlockedIncrement(&stress->failures);
        }
    }
    return 0u;
}

typedef struct WinxtermJobMockHost {
    HANDLE request_read;
    HANDLE reply_write;
    volatile LONG failures;
} WinxtermJobMockHost;

static DWORD WINAPI winxterm_smoke_job_mock_host_thread(void *context)
{
    WinxtermJobMockHost *host = (WinxtermJobMockHost *)context;
    WinxtermJobFrame request;
    if (!winxterm_job_channel_read(host->request_read, &request) ||
        request.header.type != WINXTERM_JOB_MESSAGE_CAPABILITIES) {
        InterlockedIncrement(&host->failures);
        return 1u;
    }
    uint8_t event_payload[96];
    size_t event_length = 0u;
    bool ok = winxterm_job_tlv_append_u32(event_payload, sizeof(event_payload), &event_length,
                                         WINXTERM_JOB_TLV_EVENT_KIND,
                                         WINXTERM_JOB_EVENT_FOREGROUND_CHANGED) &&
              winxterm_job_tlv_append_u64(event_payload, sizeof(event_payload), &event_length,
                                         WINXTERM_JOB_TLV_JOB_ID, 7u) &&
              winxterm_job_tlv_append_u32(event_payload, sizeof(event_payload), &event_length,
                                         WINXTERM_JOB_TLV_STATE,
                                         WINXTERM_JOB_FOREGROUND) &&
              winxterm_job_tlv_append_u32(event_payload, sizeof(event_payload), &event_length,
                                         WINXTERM_JOB_TLV_EXIT_CODE, 0u);
    WinxtermJobFrameHeader event_header = {
        WINXTERM_JOB_PROTOCOL_VERSION, WINXTERM_JOB_MESSAGE_EVENT, 0u, 0u,
        (uint32_t)event_length
    };
    uint8_t reply_payload[32];
    size_t reply_length = 0u;
    ok = ok && winxterm_job_tlv_append_u32(reply_payload, sizeof(reply_payload), &reply_length,
                                          WINXTERM_JOB_TLV_STATUS, ERROR_SUCCESS) &&
         winxterm_job_tlv_append_u32(reply_payload, sizeof(reply_payload), &reply_length,
                                    WINXTERM_JOB_TLV_FLAGS,
                                    WINXTERM_JOB_CAPABILITY_LIST |
                                    WINXTERM_JOB_CAPABILITY_EVENTS);
    WinxtermJobFrameHeader reply_header = {
        WINXTERM_JOB_PROTOCOL_VERSION, WINXTERM_JOB_MESSAGE_REPLY, 0u,
        request.header.request_id, (uint32_t)reply_length
    };
    if (!ok || !winxterm_job_channel_write(host->reply_write, &event_header, event_payload) ||
        !winxterm_job_channel_write(host->reply_write, &reply_header, reply_payload)) {
        InterlockedIncrement(&host->failures);
    }
    winxterm_job_frame_dispose(&request);
    return 0u;
}

static void winxterm_smoke_expect(WinxtermSmokeState *state, bool condition, const char *message)
{
    if (condition) {
        return;
    }

    ++state->failures;
    OutputDebugStringA("winxterm smoke failure: ");
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
}

static void winxterm_smoke_test_job_control_contracts(WinxtermSmokeState *state)
{
    wchar_t transfer_bytes[64], transfer_duration[64], transfer_speed[64];
    winxterm_transfer_format_bytes(1536u, transfer_bytes, 64u);
    winxterm_transfer_format_duration(500000000ull, transfer_duration, 64u);
    winxterm_transfer_format_speed(1536u, 500000000ull, transfer_speed, 64u);
    winxterm_smoke_expect(state,
                          wcscmp(transfer_bytes, L"1.50 KiB") == 0 &&
                              wcscmp(transfer_duration, L"500 ms") == 0 &&
                              wcscmp(transfer_speed, L"3.00 KiB/s") == 0,
                          "managed and fallback redirects should share exact summary formatting");

    WinxtermJobCoordinator coordinator;
    winxterm_job_coordinator_init(&coordinator, 2u);
    uint32_t coordinator_action = 0u;
    uint64_t coordinator_job = 0u;
    uint8_t *coordinator_view = (uint8_t *)malloc(1u);
    uint8_t *taken_view = 0;
    size_t taken_view_count = 0u;
    winxterm_job_coordinator_set_active_session(&coordinator, 9u);
    winxterm_smoke_expect(state,
                          winxterm_job_coordinator_enqueue(&coordinator, 1u, 11u) &&
                              winxterm_job_coordinator_enqueue(&coordinator, 2u, 12u) &&
                              !winxterm_job_coordinator_enqueue(&coordinator, 3u, 13u) &&
                              winxterm_job_coordinator_take(&coordinator,
                                                            &coordinator_action,
                                                            &coordinator_job) &&
                              coordinator_action == 1u && coordinator_job == 11u &&
                              coordinator_view != 0 &&
                              winxterm_job_coordinator_publish_view(
                                  &coordinator, 11u, coordinator_view, 1u) &&
                              winxterm_job_coordinator_take_view(
                                  &coordinator, &coordinator_job, &taken_view,
                                  &taken_view_count) &&
                              coordinator_job == 11u && taken_view == coordinator_view &&
                              taken_view_count == 1u &&
                              winxterm_job_coordinator_active_session(&coordinator) == 9u,
                          "bridge-owned coordinator should bound and serialize native commands");
    WinxtermJobCoordinatorClient coordinator_client;
    memset(&coordinator_client, 0, sizeof(coordinator_client));
    coordinator_client.requester_id = 77u;
    InitializeCriticalSection(&coordinator_client.reply_lock);
    coordinator_client.reply_lock_initialized = true;
    HANDLE coordinator_read = 0;
    bool coordinator_pipe = CreatePipe(&coordinator_read,
                                       &coordinator_client.reply_write, 0, 0) != FALSE;
    WinxtermJobFrameHeader coordinator_event_header = {
        WINXTERM_JOB_PROTOCOL_VERSION, WINXTERM_JOB_MESSAGE_EVENT, 0u, 0u, 0u
    };
    if (coordinator_pipe &&
        winxterm_job_coordinator_add_client(&coordinator, &coordinator_client)) {
        winxterm_job_coordinator_broadcast(&coordinator, &coordinator_event_header,
                                           0, 0, 0);
    }
    WinxtermJobFrame coordinator_event;
    memset(&coordinator_event, 0, sizeof(coordinator_event));
    bool coordinator_broadcast_ok = coordinator_pipe &&
        winxterm_job_channel_read(coordinator_read, &coordinator_event) &&
        coordinator_event.header.type == WINXTERM_JOB_MESSAGE_EVENT;
    winxterm_job_frame_dispose(&coordinator_event);
    EnterCriticalSection(&coordinator_client.reply_lock);
    for (size_t i = 0u; coordinator_broadcast_ok && i < 300u; ++i) {
        winxterm_job_coordinator_broadcast(&coordinator, &coordinator_event_header,
                                           0, 0, 0);
    }
    LeaveCriticalSection(&coordinator_client.reply_lock);
    bool coordinator_resync = false;
    for (size_t i = 0u; coordinator_broadcast_ok && !coordinator_resync && i < 260u; ++i) {
        memset(&coordinator_event, 0, sizeof(coordinator_event));
        if (!winxterm_job_channel_read(coordinator_read, &coordinator_event)) {
            coordinator_broadcast_ok = false;
            break;
        }
        WinxtermJobTlvReader event_reader;
        WinxtermJobTlv event_tlv;
        winxterm_job_tlv_reader_init(&event_reader, coordinator_event.payload,
                                     coordinator_event.header.payload_length);
        while (winxterm_job_tlv_next(&event_reader, &event_tlv)) {
            uint32_t kind = 0u;
            if (event_tlv.type == WINXTERM_JOB_TLV_EVENT_KIND &&
                winxterm_job_tlv_read_u32(&event_tlv, &kind) &&
                kind == WINXTERM_JOB_EVENT_RESYNC_REQUIRED) coordinator_resync = true;
        }
        winxterm_job_frame_dispose(&coordinator_event);
    }
    WinxtermJobCoordinatorClient *detached_clients =
        winxterm_job_coordinator_detach_clients(&coordinator);
    winxterm_smoke_expect(state,
                          coordinator_broadcast_ok && coordinator_resync &&
                              detached_clients == &coordinator_client,
                          "coordinator should queue broadcasts and report event overflow by resync");
    winxterm_job_coordinator_stop_client(&coordinator_client);
    if (coordinator_read != 0) CloseHandle(coordinator_read);
    if (coordinator_client.reply_write != 0) CloseHandle(coordinator_client.reply_write);
    DeleteCriticalSection(&coordinator_client.reply_lock);
    if (taken_view != coordinator_view) free(coordinator_view);
    free(taken_view);
    winxterm_job_coordinator_dispose(&coordinator);

    WinxtermTerminalSession session;
    winxterm_smoke_expect(state,
                          winxterm_terminal_session_init(&session, 12, 4, true) &&
                              session.screen_stored && session.screen.columns == 12 &&
                              session.screen.rows == 4,
                          "managed terminal sessions should own initialized screen state");
    static const uint8_t session_output[] = "session transcript";
    uint8_t *session_copy = 0;
    size_t session_copy_count = 0u;
    winxterm_smoke_expect(state,
                          winxterm_terminal_session_record(
                              &session, session_output, sizeof(session_output) - 1u) &&
                              winxterm_terminal_session_copy_transcript(
                                  &session, &session_copy, &session_copy_count) &&
                              session_copy_count == sizeof(session_output) - 1u &&
                              memcmp(session_copy, session_output, session_copy_count) == 0,
                          "managed terminal sessions should retain independent transcript snapshots");
    free(session_copy);
    winxterm_terminal_session_dispose(&session);

    uint8_t payload[64];
    size_t payload_length = 0u;
    winxterm_smoke_expect(state,
                          winxterm_job_tlv_append_u64(payload, sizeof(payload), &payload_length,
                                                     WINXTERM_JOB_TLV_JOB_ID, 0x123456789abcdef0ull),
                          "job protocol should append an id TLV");
    WinxtermJobFrameHeader source = {WINXTERM_JOB_PROTOCOL_VERSION,
                                    WINXTERM_JOB_MESSAGE_FOREGROUND, 7u, 99u,
                                    (uint32_t)payload_length};
    uint8_t encoded[WINXTERM_JOB_PROTOCOL_HEADER_SIZE];
    WinxtermJobFrameHeader decoded;
    winxterm_smoke_expect(state,
                          winxterm_job_frame_encode_header(&source, encoded) &&
                              winxterm_job_frame_decode_header(encoded, sizeof(encoded), &decoded) &&
                              decoded.type == source.type && decoded.flags == source.flags &&
                              decoded.request_id == source.request_id &&
                              decoded.payload_length == source.payload_length,
                          "job protocol header should round trip");
    encoded[0] ^= 1u;
    winxterm_smoke_expect(state,
                          !winxterm_job_frame_decode_header(encoded, sizeof(encoded), &decoded),
                          "job protocol should reject a bad magic value");
    WinxtermJobTlvReader reader;
    WinxtermJobTlv tlv;
    uint64_t id = 0u;
    winxterm_job_tlv_reader_init(&reader, payload, payload_length);
    winxterm_smoke_expect(state,
                          winxterm_job_tlv_next(&reader, &tlv) &&
                              tlv.type == WINXTERM_JOB_TLV_JOB_ID &&
                              winxterm_job_tlv_read_u64(&tlv, &id) && id == 0x123456789abcdef0ull,
                          "job protocol TLV should round trip");

    SECURITY_ATTRIBUTES security;
    memset(&security, 0, sizeof(security));
    security.nLength = sizeof(security);
    HANDLE channel_read = 0;
    HANDLE channel_write = 0;
    bool channel_created = CreatePipe(&channel_read, &channel_write, &security, 0) != 0;
    WinxtermJobFrame channel_frame;
    memset(&channel_frame, 0, sizeof(channel_frame));
    winxterm_smoke_expect(state,
                          channel_created &&
                              winxterm_job_channel_write(channel_write, &source, payload) &&
                              winxterm_job_channel_read(channel_read, &channel_frame) &&
                              channel_frame.header.request_id == source.request_id &&
                              channel_frame.header.payload_length == payload_length &&
                              memcmp(channel_frame.payload, payload, payload_length) == 0,
                          "job channel should preserve a framed pipe message");
    winxterm_job_frame_dispose(&channel_frame);
    if (channel_read != 0) CloseHandle(channel_read);

    SECURITY_ATTRIBUTES mock_security;
    memset(&mock_security, 0, sizeof(mock_security));
    mock_security.nLength = sizeof(mock_security);
    mock_security.bInheritHandle = TRUE;
    WinxtermJobMockHost mock_host;
    memset(&mock_host, 0, sizeof(mock_host));
    HANDLE mock_client_request = 0;
    HANDLE mock_client_reply = 0;
    bool mock_pipes = CreatePipe(&mock_host.request_read, &mock_client_request,
                                 &mock_security, 0) != FALSE &&
                      CreatePipe(&mock_client_reply, &mock_host.reply_write,
                                 &mock_security, 0) != FALSE;
    wchar_t mock_protocol[32], mock_request[64], mock_reply[64], mock_self[32];
    bool mock_environment = mock_pipes &&
        swprintf_s(mock_protocol, 32, L"%u", WINXTERM_JOB_PROTOCOL_VERSION) > 0 &&
        swprintf_s(mock_request, 64, L"%llu",
                   (unsigned long long)(uintptr_t)mock_client_request) > 0 &&
        swprintf_s(mock_reply, 64, L"%llu",
                   (unsigned long long)(uintptr_t)mock_client_reply) > 0 &&
        swprintf_s(mock_self, 32, L"%u", 7u) > 0 &&
        SetEnvironmentVariableW(WINXTERM_JOB_ENV_PROTOCOL, mock_protocol) &&
        SetEnvironmentVariableW(WINXTERM_JOB_ENV_REQUEST_HANDLE, mock_request) &&
        SetEnvironmentVariableW(WINXTERM_JOB_ENV_REPLY_HANDLE, mock_reply) &&
        SetEnvironmentVariableW(WINXTERM_JOB_ENV_SELF_ID, mock_self);
    HANDLE mock_thread = mock_environment ?
        CreateThread(0, 0, winxterm_smoke_job_mock_host_thread, &mock_host, 0, 0) : 0;
    WinxtermDstcmdJobClient mock_client;
    memset(&mock_client, 0, sizeof(mock_client));
    if (mock_thread != 0) winxterm_dstcmd_job_client_init(&mock_client);
    (void)SetEnvironmentVariableW(WINXTERM_JOB_ENV_PROTOCOL, 0);
    (void)SetEnvironmentVariableW(WINXTERM_JOB_ENV_REQUEST_HANDLE, 0);
    (void)SetEnvironmentVariableW(WINXTERM_JOB_ENV_REPLY_HANDLE, 0);
    (void)SetEnvironmentVariableW(WINXTERM_JOB_ENV_SELF_ID, 0);
    WinxtermDstcmdJobEvent mock_event;
    bool mock_event_ready = mock_client.event_ready != 0 &&
        WaitForSingleObject(mock_client.event_ready, 1000u) == WAIT_OBJECT_0;
    bool mock_ok = mock_thread != 0 &&
        winxterm_dstcmd_job_client_available(&mock_client) &&
        (winxterm_dstcmd_job_client_capabilities(&mock_client) &
         (WINXTERM_JOB_CAPABILITY_LIST | WINXTERM_JOB_CAPABILITY_EVENTS)) ==
            (WINXTERM_JOB_CAPABILITY_LIST | WINXTERM_JOB_CAPABILITY_EVENTS) &&
        mock_event_ready &&
        winxterm_dstcmd_job_client_poll_event(&mock_client, &mock_event) &&
        mock_event.kind == WINXTERM_JOB_EVENT_FOREGROUND_CHANGED &&
        mock_event.job_id == 7u;
    if (mock_host.reply_write != 0) {
        CloseHandle(mock_host.reply_write);
        mock_host.reply_write = 0;
    }
    bool mock_channel_loss = mock_client.reader_thread != 0 &&
        WaitForSingleObject(mock_client.reader_thread, 1000u) == WAIT_OBJECT_0 &&
        !winxterm_dstcmd_job_client_available(&mock_client);
    bool mock_discovered = mock_client.lock_initialized;
    winxterm_dstcmd_job_client_dispose(&mock_client);
    if (mock_thread != 0) {
        (void)WaitForSingleObject(mock_thread, 1000u);
        CloseHandle(mock_thread);
    }
    if (mock_host.request_read != 0) CloseHandle(mock_host.request_read);
    if (mock_host.reply_write != 0) CloseHandle(mock_host.reply_write);
    /* The client owns and closes its pipe ends after successful discovery. */
    if (!mock_discovered) {
        if (mock_client_request != 0) CloseHandle(mock_client_request);
        if (mock_client_reply != 0) CloseHandle(mock_client_reply);
    }
    winxterm_smoke_expect(state, mock_ok && mock_channel_loss && mock_host.failures == 0,
                          "mock host should interleave events, correlate replies, and report channel loss");
    if (channel_write != 0) CloseHandle(channel_write);

    channel_read = 0;
    channel_write = 0;
    channel_created = CreatePipe(&channel_read, &channel_write, &security, 0) != 0;
    WinxtermJobFrameHeader truncated_header = source;
    truncated_header.payload_length = 8u;
    uint8_t truncated_encoded[WINXTERM_JOB_PROTOCOL_HEADER_SIZE];
    DWORD truncated_written = 0u;
    bool truncated_prepared = channel_created &&
        winxterm_job_frame_encode_header(&truncated_header, truncated_encoded) &&
        WriteFile(channel_write, truncated_encoded, sizeof(truncated_encoded),
                  &truncated_written, 0) &&
        WriteFile(channel_write, payload, 2u, &truncated_written, 0);
    if (channel_write != 0) { CloseHandle(channel_write); channel_write = 0; }
    memset(&channel_frame, 0, sizeof(channel_frame));
    winxterm_smoke_expect(state,
                          truncated_prepared &&
                              !winxterm_job_channel_read(channel_read, &channel_frame),
                          "job channel should reject a frame truncated by channel loss");
    winxterm_job_frame_dispose(&channel_frame);
    if (channel_read != 0) CloseHandle(channel_read);

    WinxtermJobManager manager;
    winxterm_smoke_expect(state, winxterm_job_manager_init(&manager),
                          "job manager should initialize");
    uint64_t root = winxterm_job_manager_add(&manager, 0u, WINXTERM_JOB_FOREGROUND);
    uint64_t child = winxterm_job_manager_add(&manager, root, WINXTERM_JOB_FOREGROUND);
    uint64_t grandchild = winxterm_job_manager_add(&manager, child, WINXTERM_JOB_FOREGROUND);
    WinxtermManagedJobSnapshot *job_snapshot = 0;
    size_t job_snapshot_count = 0u;
    winxterm_smoke_expect(state,
                          root == 1u && child == 2u && grandchild == 3u &&
                              winxterm_job_manager_foreground_id(&manager) == grandchild &&
                              winxterm_job_manager_authorized(&manager, root, grandchild) &&
                              !winxterm_job_manager_authorized(&manager, grandchild, root),
                          "job manager should assign ids, stack foreground jobs, and authorize ancestors");
    winxterm_smoke_expect(state,
                          winxterm_job_manager_set_process(&manager, child, 42u, L"child.exe") &&
                              winxterm_job_manager_snapshot(&manager, root, &job_snapshot,
                                                           &job_snapshot_count) &&
                              job_snapshot_count == 3u &&
                              job_snapshot[1].process_id == 42u &&
                              wcscmp(job_snapshot[1].display_name, L"child.exe") == 0,
                          "job manager snapshots should copy synchronized metadata");
    winxterm_job_manager_snapshot_dispose(job_snapshot);
    WinxtermJobStressContext stress = {&manager, root, child, 0};
    HANDLE stress_threads[2] = {
        CreateThread(0, 0, winxterm_smoke_job_snapshot_thread, &stress, 0, 0),
        CreateThread(0, 0, winxterm_smoke_job_metadata_thread, &stress, 0, 0)
    };
    bool stress_started = stress_threads[0] != 0 && stress_threads[1] != 0;
    if (stress_threads[0] != 0) (void)WaitForSingleObject(stress_threads[0], INFINITE);
    if (stress_threads[1] != 0) (void)WaitForSingleObject(stress_threads[1], INFINITE);
    if (stress_threads[0] != 0) CloseHandle(stress_threads[0]);
    if (stress_threads[1] != 0) CloseHandle(stress_threads[1]);
    winxterm_smoke_expect(state, stress_started && stress.failures == 0,
                          "job snapshots and metadata updates should remain synchronized under stress");
    winxterm_smoke_expect(state,
                          winxterm_job_manager_foreground(&manager, child) &&
                              winxterm_job_manager_foreground_id(&manager) == child,
                          "foregrounding an existing job should not duplicate its stack entry");
    winxterm_smoke_expect(state,
                          winxterm_job_manager_exit(&manager, child, 4u) &&
                              winxterm_job_manager_foreground_id(&manager) == grandchild,
                          "exiting the foreground job should restore the newest live job");
    WinxtermManagedJobSnapshot kill_snapshot;
    winxterm_smoke_expect(state,
                          winxterm_job_manager_stopping(&manager, grandchild) &&
                              winxterm_job_manager_cancel_stopping(&manager, grandchild) &&
                              winxterm_job_manager_snapshot_one(&manager, grandchild,
                                                                &kill_snapshot) &&
                              kill_snapshot.state == WINXTERM_JOB_FOREGROUND,
                          "cancelling kill escalation should restore foreground job state");
    winxterm_smoke_expect(state,
                          !winxterm_job_manager_remove(&manager, root, child),
                          "a job with an owned descendant should not be removable");
    winxterm_smoke_expect(state,
                          winxterm_job_manager_exit(&manager, grandchild, 0u) &&
                              winxterm_job_manager_remove(&manager, root, grandchild) &&
                              winxterm_job_manager_remove(&manager, root, child),
                          "an ancestor should remove exited leaf jobs in dependency order");
    winxterm_job_manager_dispose(&manager);

    WinxtermJobManager auto_remove_manager;
    bool auto_remove_ok = winxterm_job_manager_init(&auto_remove_manager);
    uint64_t auto_root = auto_remove_ok ?
        winxterm_job_manager_add(&auto_remove_manager, 0u, WINXTERM_JOB_FOREGROUND) : 0u;
    uint64_t auto_child = auto_root != 0u ?
        winxterm_job_manager_add(&auto_remove_manager, auto_root,
                                 WINXTERM_JOB_FOREGROUND) : 0u;
    uint64_t auto_descendant = auto_child != 0u ?
        winxterm_job_manager_add(&auto_remove_manager, auto_child,
                                 WINXTERM_JOB_BACKGROUND) : 0u;
    WinxtermManagedJobSnapshot auto_snapshot;
    auto_remove_ok = auto_descendant != 0u &&
        winxterm_job_manager_exit(&auto_remove_manager, auto_child, 7u) &&
        winxterm_job_manager_remove_finished_reparent(&auto_remove_manager, auto_child) &&
        !winxterm_job_manager_snapshot_one(&auto_remove_manager, auto_child,
                                           &auto_snapshot) &&
        winxterm_job_manager_snapshot_one(&auto_remove_manager, auto_descendant,
                                          &auto_snapshot) &&
        auto_snapshot.owner_id == auto_root &&
        winxterm_job_manager_authorized(&auto_remove_manager, auto_root,
                                        auto_descendant);
    winxterm_smoke_expect(state, auto_remove_ok,
                          "removing a completed foreground job should reparent its descendants");
    winxterm_job_manager_dispose(&auto_remove_manager);

    WinxtermJobManager large_manager;
    bool large_ok = winxterm_job_manager_init(&large_manager);
    uint64_t large_root = large_ok ?
        winxterm_job_manager_add(&large_manager, 0u, WINXTERM_JOB_FOREGROUND) : 0u;
    uint64_t last_large_id = large_root;
    for (size_t i = 0u; large_ok && i < 2048u; ++i) {
        last_large_id = winxterm_job_manager_add(&large_manager, large_root,
                                                 WINXTERM_JOB_BACKGROUND);
        large_ok = last_large_id == (uint64_t)i + 2u &&
                   winxterm_job_manager_exit(&large_manager, last_large_id,
                                             (uint32_t)(i & 0xffu));
    }
    WinxtermManagedJobSnapshot *large_snapshot = 0;
    size_t large_count = 0u;
    large_ok = large_ok &&
        winxterm_job_manager_snapshot(&large_manager, large_root,
                                      &large_snapshot, &large_count) &&
        large_count == 2049u && last_large_id == 2049u &&
        winxterm_job_manager_clean(&large_manager, large_root) == 2048u;
    winxterm_smoke_expect(state, large_ok,
                          "job manager should preserve monotonic ids and snapshots at high job counts");
    winxterm_job_manager_snapshot_dispose(large_snapshot);
    winxterm_job_manager_dispose(&large_manager);

    WinxtermJobJournal journal;
    uint8_t journal_input[] = {1u, 2u, 3u, 4u, 5u, 6u};
    uint8_t consumed[3];
    uint8_t *view = 0;
    size_t view_length = 0u;
    uint64_t view_next = 0u;
    winxterm_smoke_expect(state,
                          winxterm_job_journal_init(&journal, 6u) &&
                              winxterm_job_journal_append(&journal, journal_input, 6u) &&
                              winxterm_job_journal_backpressured(&journal) &&
                              winxterm_job_journal_snapshot(&journal, 0u, &view,
                                                            &view_length, &view_next) &&
                              view_length == 6u && view_next == 6u &&
                              memcmp(view, journal_input, 6u) == 0,
                          "job journal views should be non-destructive at the output limit");
    free(view);
    view = 0;
    winxterm_smoke_expect(state,
                          winxterm_job_journal_consume(&journal, consumed, sizeof(consumed)) == 3u &&
                              memcmp(consumed, journal_input, 3u) == 0 &&
                              !winxterm_job_journal_backpressured(&journal) &&
                              winxterm_job_journal_append(&journal, journal_input, 3u) &&
                              winxterm_job_journal_snapshot(&journal, 3u, &view,
                                                            &view_length, &view_next) &&
                              view_length == 6u &&
                              memcmp(view, journal_input + 3u, 3u) == 0 &&
                              memcmp(view + 3u, journal_input, 3u) == 0,
                          "job journal consumption should release capacity without replaying bytes");
    free(view);
    winxterm_job_journal_dispose(&journal);

    WinxtermJobJournal stable_journal;
    uint8_t stable_bytes[8];
    size_t stable_length = 0u;
    uint64_t stable_next = 0u;
    bool stable_more = false;
    winxterm_smoke_expect(state,
                          winxterm_job_journal_init(&stable_journal, 16u) &&
                              winxterm_job_journal_append(&stable_journal, journal_input, 3u) &&
                              winxterm_job_journal_append(&stable_journal, journal_input + 3u, 3u) &&
                              winxterm_job_journal_copy_snapshot(&stable_journal, 0u, 3u,
                                                                 stable_bytes, sizeof(stable_bytes),
                                                                 &stable_length, &stable_next,
                                                                 &stable_more) &&
                              stable_length == 3u && stable_next == 3u && !stable_more &&
                              memcmp(stable_bytes, journal_input, 3u) == 0,
                          "job journal pagination should preserve its captured end offset");
    winxterm_job_journal_dispose(&stable_journal);

    WinxtermJobJournal sparse_journal;
    winxterm_smoke_expect(state,
                          winxterm_job_journal_init(&sparse_journal,
                                                    WINXTERM_JOB_OUTPUT_LIMIT) &&
                              winxterm_job_journal_committed(&sparse_journal) == 0u &&
                              winxterm_job_journal_append(&sparse_journal,
                                                          journal_input, 1u) &&
                              winxterm_job_journal_committed(&sparse_journal) != 0u &&
                              winxterm_job_journal_committed(&sparse_journal) <
                                  WINXTERM_JOB_OUTPUT_LIMIT,
                          "large job journals should reserve their limit and commit incrementally");
    winxterm_job_journal_dispose(&sparse_journal);

    char *plan_arguments[] = {"tool.exe", "--value", "two words"};
    WinxtermJobPlanStage plan_stage;
    memset(&plan_stage, 0, sizeof(plan_stage));
    plan_stage.arguments = plan_arguments;
    plan_stage.argument_count = 3u;
    plan_stage.stdin_endpoint = WINXTERM_JOB_PLAN_ENDPOINT_TERMINAL;
    plan_stage.stdout_endpoint = WINXTERM_JOB_PLAN_ENDPOINT_TERMINAL;
    plan_stage.stderr_endpoint = WINXTERM_JOB_PLAN_ENDPOINT_TERMINAL;
    WinxtermJobExecutionPlan source_plan;
    memset(&source_plan, 0, sizeof(source_plan));
    source_plan.cwd = "C:\\work";
    source_plan.stages = &plan_stage;
    source_plan.stage_count = 1u;
    uint8_t *plan_payload = 0;
    uint32_t plan_payload_length = 0u;
    WinxtermJobExecutionPlan decoded_plan;
    memset(&decoded_plan, 0, sizeof(decoded_plan));
    winxterm_smoke_expect(state,
                          winxterm_job_plan_encode(&source_plan, &plan_payload,
                                                  &plan_payload_length) &&
                              winxterm_job_plan_decode(plan_payload, plan_payload_length,
                                                      &decoded_plan) &&
                              decoded_plan.stage_count == 1u &&
                              decoded_plan.stages[0].argument_count == 3u &&
                              strcmp(decoded_plan.stages[0].arguments[2], "two words") == 0,
                          "execution plans should round-trip typed stages and arguments");
    winxterm_job_plan_dispose(&decoded_plan);
    if (plan_payload_length >= 8u) {
        plan_payload[6] = 0xffu;
        plan_payload[7] = 0x7fu;
        winxterm_smoke_expect(state,
                              !winxterm_job_plan_decode(plan_payload, plan_payload_length,
                                                       &decoded_plan),
                              "execution-plan decoding should reject malformed bounded TLVs");
    }
    free(plan_payload);
}

static bool winxterm_smoke_bytes_contain(const uint8_t *haystack,
                                         size_t haystack_count,
                                         const char *needle)
{
    size_t needle_count = needle != 0 ? strlen(needle) : 0u;
    if (haystack == 0 || needle_count == 0u || haystack_count < needle_count) {
        return false;
    }
    for (size_t i = 0u; i + needle_count <= haystack_count; ++i) {
        if (memcmp(haystack + i, needle, needle_count) == 0) {
            return true;
        }
    }
    return false;
}

static WinxtermScreenCell winxterm_smoke_text_cell(uint32_t codepoint)
{
    WinxtermScreenCell cell;
    memset(&cell, 0, sizeof(cell));
    cell.codepoint = codepoint;
    cell.glyph_index = winxterm_screen_map_codepoint_to_glyph(codepoint);
    cell.foreground_rgb = WINXTERM_DEFAULT_FOREGROUND_RGB;
    cell.background_rgb = WINXTERM_DEFAULT_BACKGROUND_RGB;
    cell.foreground_palette_index = 7u;
    cell.background_palette_index = 0u;
    cell.color_flags = (uint8_t)(WINXTERM_SCREEN_COLOR_FOREGROUND_DEFAULT |
                                 WINXTERM_SCREEN_COLOR_BACKGROUND_DEFAULT);
    cell.width = 1u;
    cell.occupied = codepoint != (uint32_t)' ';
    return cell;
}

static void winxterm_smoke_fill_text_cells(WinxtermScreenCell *cells, int columns, const char *text)
{
    if (cells == 0 || columns <= 0) {
        return;
    }
    for (int column = 0; column < columns; ++column) {
        cells[column] = winxterm_smoke_text_cell((uint32_t)' ');
    }
    if (text == 0) {
        return;
    }
    for (int column = 0; column < columns && text[column] != '\0'; ++column) {
        cells[column] = winxterm_smoke_text_cell((uint32_t)(uint8_t)text[column]);
    }
}

static bool winxterm_smoke_set_scrollback_line(WinxtermScreen *screen,
                                               size_t index,
                                               const char *text,
                                               bool soft_wrapped)
{
    if (screen == 0 || index >= screen->scrollback_count) {
        return false;
    }
    WinxtermScreenLine *line = &screen->scrollback_lines[index];
    line->cells = (WinxtermScreenCell *)malloc((size_t)screen->columns * sizeof(*line->cells));
    if (line->cells == 0) {
        return false;
    }
    line->columns = screen->columns;
    line->soft_wrapped = soft_wrapped;
    line->content_columns = soft_wrapped ? screen->columns :
        (int)(text != 0 ? strlen(text) : 0u);
    winxterm_smoke_fill_text_cells(line->cells, screen->columns, text);
    return true;
}

static bool winxterm_smoke_protocol_append_reply(WinxtermProtocolCapture *capture,
                                                 const uint8_t *bytes,
                                                 size_t byte_count)
{
    if (capture == 0 || bytes == 0 || capture->reply_count + byte_count > sizeof(capture->replies)) {
        return false;
    }
    memcpy(capture->replies + capture->reply_count, bytes, byte_count);
    capture->reply_count += byte_count;
    return true;
}

static bool winxterm_smoke_protocol_sink(void *context, const WinxtermTerminalOp *op)
{
    WinxtermProtocolCapture *capture = (WinxtermProtocolCapture *)context;
    if (capture == 0 || op == 0) {
        return false;
    }
    if (capture->op_count < sizeof(capture->ops) / sizeof(capture->ops[0])) {
        capture->ops[capture->op_count++] = *op;
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
            reply_length = winxterm_reply_cpr(capture->screen != 0 ? capture->screen->cursor_row + 1 : 1,
                                              capture->screen != 0 ? capture->screen->cursor_col + 1 : 1,
                                              reply,
                                              sizeof(reply));
            break;
        case WINXTERM_TERMINAL_QUERY_DECRQM: {
            WinxtermTerminalMode mode;
            int status = 0;
            if (capture->screen != 0 &&
                winxterm_mode_from_csi(op->data.query.private_marker, op->data.query.param, &mode)) {
                status = winxterm_mode_decrqm_status(&capture->screen->mode_state, mode);
            }
            reply_length = winxterm_reply_decrqm(op->data.query.private_marker,
                                                 op->data.query.param,
                                                 status,
                                                 reply,
                                                 sizeof(reply));
            break;
        }
        case WINXTERM_TERMINAL_QUERY_DECRQSS:
            reply_length = winxterm_reply_decrqss(op->data.query.request,
                                                  op->data.query.request_length,
                                                  op->data.query.request_length == 1u &&
                                                      op->data.query.request[0] == 'm',
                                                  reply,
                                                  sizeof(reply));
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
        return reply_length == 0u || winxterm_smoke_protocol_append_reply(capture, reply, reply_length);
    }

    if (capture->screen != 0) {
        return winxterm_screen_apply_op(capture->screen, op);
    }
    return true;
}

static bool winxterm_smoke_replies_contain(const WinxtermProtocolCapture *capture,
                                           const char *text,
                                           size_t length)
{
    if (capture == 0 || text == 0 || length == 0u || length > capture->reply_count) {
        return false;
    }
    for (size_t i = 0u; i + length <= capture->reply_count; ++i) {
        if (memcmp(capture->replies + i, text, length) == 0) {
            return true;
        }
    }
    return false;
}

static bool winxterm_smoke_screen_contains_text(const WinxtermScreen *screen, const char *text)
{
    if (screen == 0 || text == 0 || text[0] == '\0') {
        return false;
    }
    size_t length = strlen(text);
    if (length == 0u || length > (size_t)screen->columns) {
        return false;
    }
    for (int row = 0; row < screen->rows; ++row) {
        for (int column = 0; column + (int)length <= screen->columns; ++column) {
            bool matched = true;
            for (size_t i = 0u; i < length; ++i) {
                const WinxtermScreenCell *cell = winxterm_screen_cell_at((WinxtermScreen *)screen,
                                                                         row,
                                                                         column + (int)i);
                if (cell == 0 || cell->codepoint != (uint32_t)(unsigned char)text[i]) {
                    matched = false;
                    break;
                }
            }
            if (matched) {
                return true;
            }
        }
    }
    return false;
}

static bool winxterm_smoke_row_view_contains_text(const WinxtermScreenRowView *row, const char *text)
{
    if (row == 0 || row->cells == 0 || text == 0 || text[0] == '\0') {
        return false;
    }
    size_t length = strlen(text);
    if (length == 0u || length > (size_t)row->columns) {
        return false;
    }
    for (int column = 0; column + (int)length <= row->columns; ++column) {
        bool matched = true;
        for (size_t i = 0u; i < length; ++i) {
            const WinxtermScreenCell *cell = row->cells + column + i;
            if (cell->codepoint != (uint32_t)(unsigned char)text[i]) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return true;
        }
    }
    return false;
}

static void winxterm_smoke_test_render_constants(WinxtermSmokeState *state)
{
    winxterm_smoke_expect(state, WINXTERM_TERMINAL_COLUMNS == 80, "terminal columns must be 80");
    winxterm_smoke_expect(state, WINXTERM_TERMINAL_ROWS == 24, "terminal rows must be 24");
    winxterm_smoke_expect(state, WINXTERM_CELL_WIDTH_PIXELS == 6, "cell width must be 6");
    winxterm_smoke_expect(state, WINXTERM_CELL_HEIGHT_PIXELS == 13, "cell height must be 13");
    winxterm_smoke_expect(state, WINXTERM_INITIAL_PIXEL_WIDTH == 480, "initial width must be 480");
    winxterm_smoke_expect(state, WINXTERM_INITIAL_PIXEL_HEIGHT == 312, "initial height must be 312");
    winxterm_smoke_expect(state, WINXTERM_BYTES_PER_PIXEL == 4, "pixel format must be 32-bit");
    winxterm_smoke_expect(state,
                          WINXTERM_DEFAULT_FOREGROUND_RGB == 0x00e3e3e3u,
                          "default foreground must match Phase 2 plan");
    winxterm_smoke_expect(state,
                          WINXTERM_DEFAULT_BACKGROUND_RGB == 0x00262626u,
                          "default background must match Phase 2 plan");

    WinxtermCellSize cells = winxterm_pixels_to_cells(479, 311);
    winxterm_smoke_expect(state, cells.columns == 79, "pixel-to-cell width must truncate");
    winxterm_smoke_expect(state, cells.rows == 23, "pixel-to-cell height must truncate");

    WinxtermCellSize scale_one_cells =
        winxterm_physical_pixels_to_cells(WINXTERM_INITIAL_PIXEL_WIDTH,
                                          WINXTERM_INITIAL_PIXEL_HEIGHT,
                                          1u);
    winxterm_smoke_expect(state,
                          scale_one_cells.columns == 80 && scale_one_cells.rows == 24,
                          "scale-1 capacity should preserve initial 80x24 grid");
    WinxtermCellSize scale_two_cells =
        winxterm_physical_pixels_to_cells(WINXTERM_INITIAL_PIXEL_WIDTH * 2,
                                          WINXTERM_INITIAL_PIXEL_HEIGHT * 2,
                                          2u);
    winxterm_smoke_expect(state,
                          scale_two_cells.columns == 80 && scale_two_cells.rows == 24,
                          "scale-2 capacity should preserve doubled 80x24 grid");
    WinxtermCellSize half_scale_two_cells =
        winxterm_physical_pixels_to_cells(WINXTERM_INITIAL_PIXEL_WIDTH,
                                          WINXTERM_INITIAL_PIXEL_HEIGHT,
                                          2u);
    winxterm_smoke_expect(state,
                          half_scale_two_cells.columns == 40 && half_scale_two_cells.rows == 12,
                          "scale-2 capacity should halve fixed physical client capacity");
}

static void winxterm_smoke_test_font_contract(WinxtermSmokeState *state)
{
    winxterm_smoke_expect(state, WINXTERM_FONT_6X13_WIDTH == 6u, "font width contract must be 6");
    winxterm_smoke_expect(state, WINXTERM_FONT_6X13_HEIGHT == 13u, "font height contract must be 13");
    winxterm_smoke_expect(state, WINXTERM_FONT_6X13_SOURCE_GLYPH_COUNT == 256u, "source font glyph slots must be 256");
    winxterm_smoke_expect(state, WINXTERM_FONT_6X13_TOTAL_GLYPH_COUNT == 257u,
                          "font glyph count must include fallback");
    winxterm_smoke_expect(state,
                          winxterm_font_6x13_rows[WINXTERM_FONT_6X13_FALLBACK_GLYPH_INDEX][0].foreground_mask == 0x3fu,
                          "fallback glyph top row must be a full rectangle edge");
    winxterm_smoke_expect(state,
                          winxterm_font_6x13_rows[WINXTERM_FONT_6X13_FALLBACK_GLYPH_INDEX][12].foreground_mask == 0x3fu,
                          "fallback glyph bottom row must be a full rectangle edge");
    winxterm_smoke_expect(state,
                          winxterm_font_6x13_rows[WINXTERM_FONT_6X13_FALLBACK_GLYPH_INDEX][1].foreground_mask == 0x21u,
                          "fallback glyph side row must include rectangle edges");
}

static void winxterm_smoke_test_log_names(WinxtermSmokeState *state)
{
    wchar_t filename[64];
    bool ok = winxterm_log_format_filename(1234u, filename, 64);
    winxterm_smoke_expect(state, ok, "log filename formatting should succeed");
    winxterm_smoke_expect(state,
                          wcscmp(filename, L"winxterm-debug-1234.txt") == 0,
                          "log filename must include PID");

    wchar_t tiny[4];
    winxterm_smoke_expect(state,
                          !winxterm_log_format_filename(1234u, tiny, 4),
                          "log filename formatting should reject tiny buffers");
}

static void winxterm_smoke_test_window_placement(WinxtermSmokeState *state)
{
    WinxtermWindowPlacementSaved saved;
    memset(&saved, 0, sizeof(saved));
    wcscpy_s(saved.monitor_name, CCHDEVICENAME, L"\\\\.\\DISPLAY1");
    saved.left = 40;
    saved.top = 50;

    char text[512];
    WinxtermWindowPlacementSaved parsed;
    winxterm_smoke_expect(state,
                          winxterm_window_placement_format_state(&saved, text, sizeof(text)),
                          "window placement state should format");
    winxterm_smoke_expect(state,
                          winxterm_window_placement_parse_state(text, &parsed),
                          "window placement state should parse");
    winxterm_smoke_expect(state,
                          wcscmp(parsed.monitor_name, saved.monitor_name) == 0 &&
                              parsed.left == saved.left &&
                              parsed.top == saved.top,
                          "window placement state should round trip position only");
    winxterm_smoke_expect(state,
                          winxterm_window_placement_parse_state(
                              "winxterm-window-placement-v2\r\n"
                              "monitor=\\\\.\\DISPLAY2\r\n"
                              "left=1\r\n"
                              "top=2\r\n",
                              &parsed),
                          "window placement parser should accept CRLF");
    winxterm_smoke_expect(state,
                          winxterm_window_placement_parse_state(
                              "winxterm-window-placement-v1\n"
                              "monitor=\\\\.\\DISPLAY1\n"
                              "left=0\n"
                              "top=0\n"
                              "width=0\n"
                              "height=480\n",
                              &parsed),
                          "window placement parser should ignore obsolete saved dimensions");

    WinxtermWindowPlacementMonitorScore score;
    memset(&score, 0, sizeof(score));
    winxterm_smoke_expect(state,
                          winxterm_window_placement_monitor_penalty(&score) == 0,
                          "empty monitor placement score should have no penalty");
    score.has_foreground_maximized_instance = true;
    winxterm_smoke_expect(state,
                          winxterm_window_placement_monitor_penalty(&score) > 0,
                          "foreground maximized window should penalize a monitor");
    int maximized_penalty = winxterm_window_placement_monitor_penalty(&score);
    score.has_fullscreen_instance = true;
    winxterm_smoke_expect(state,
                          winxterm_window_placement_monitor_penalty(&score) > maximized_penalty,
                          "fullscreen window should penalize a monitor more than maximized");

    RECT monitor = {100, 200, 2020, 1280};
    RECT existing = {500, 600, 900, 1000};
    RECT cascaded = winxterm_window_placement_cascade_rect(&monitor, &existing, 640, 480);
    winxterm_smoke_expect(state,
                          cascaded.left == 650 &&
                              cascaded.top == 675 &&
                              cascaded.right == 1290 &&
                              cascaded.bottom == 1155,
                          "window placement cascade should use screen-local offset");
    RECT edge_existing = {1600, 900, 1900, 1200};
    RECT edge_cascaded =
        winxterm_window_placement_cascade_rect(&monitor, &edge_existing, 640, 480);
    winxterm_smoke_expect(state,
                          edge_cascaded.left == monitor.right - 640 &&
                              edge_cascaded.top == monitor.bottom - 480 &&
                              edge_cascaded.right == monitor.right &&
                              edge_cascaded.bottom == monitor.bottom,
                          "window placement cascade should clamp to the visible edge");

    RECT from_local = winxterm_window_placement_rect_from_local(&monitor, 40, 50, 640, 480);
    winxterm_smoke_expect(state,
                          from_local.left == 140 &&
                              from_local.top == 250 &&
                              from_local.right == 780 &&
                              from_local.bottom == 730 &&
                              winxterm_window_placement_rect_fits_monitor(&from_local, &monitor),
                          "window placement local rect should convert to virtual coordinates");
    RECT offscreen = winxterm_window_placement_rect_from_local(&monitor, 1500, 50, 640, 480);
    winxterm_smoke_expect(state,
                          !winxterm_window_placement_rect_fits_monitor(&offscreen, &monitor),
                          "window placement saved rect should be rejected when outside monitor");
    RECT clamped =
        winxterm_window_placement_clamp_rect_to_visible_area(&monitor, &offscreen);
    winxterm_smoke_expect(state,
                          clamped.left == monitor.right - 640 &&
                              clamped.top == offscreen.top &&
                              clamped.right == monitor.right &&
                              clamped.bottom == offscreen.bottom &&
                              winxterm_window_placement_rect_fits_monitor(&clamped, &monitor),
                          "window placement saved rect should clamp inside visible area");
    RECT oversized = winxterm_window_placement_rect_from_local(&monitor, -20, -30, 3000, 2000);
    RECT clamped_oversized =
        winxterm_window_placement_clamp_rect_to_visible_area(&monitor, &oversized);
    winxterm_smoke_expect(state,
                          clamped_oversized.left == monitor.left &&
                              clamped_oversized.top == monitor.top &&
                              clamped_oversized.right == monitor.right &&
                              clamped_oversized.bottom == monitor.bottom,
                          "window placement oversized rect should shrink to visible area");

    RECT centered = winxterm_window_placement_center_rect(&monitor, 800, 600);
    winxterm_smoke_expect(state,
                          centered.left == 660 &&
                              centered.top == 440 &&
                              centered.right == 1460 &&
                              centered.bottom == 1040,
                          "window placement center fallback should center on the monitor");
}

static void winxterm_smoke_test_settings(WinxtermSmokeState *state)
{
    WinxtermSettings settings;
    winxterm_settings_init(&settings);
    winxterm_smoke_expect(state,
                          !settings.scrollbar,
                          "settings scrollbar should default off");

    settings.scrollbar = true;
    char text[512];
    winxterm_smoke_expect(state,
                          winxterm_settings_format(&settings, text, sizeof(text)),
                          "settings should format");

    WinxtermSettings parsed;
    winxterm_settings_init(&parsed);
    winxterm_smoke_expect(state,
                          winxterm_settings_parse(text, &parsed) && parsed.scrollbar,
                          "settings scrollbar on should round trip");

    winxterm_settings_init(&parsed);
    winxterm_smoke_expect(state,
                          winxterm_settings_parse("winxterm-settings-v1\r\n"
                                                  "future-key=value\r\n"
                                                  "scrollbar=on\r\n",
                                                  &parsed) &&
                              parsed.scrollbar,
                          "settings parser should accept CRLF and unknown keys");

    parsed.scrollbar = true;
    winxterm_smoke_expect(state,
                          winxterm_settings_parse("scrollbar=off\n", &parsed) &&
                              !parsed.scrollbar,
                          "settings parser should apply scrollbar off");

    parsed.scrollbar = false;
    winxterm_smoke_expect(state,
                          winxterm_settings_parse("scrollbar=sideways\n", &parsed) &&
                              !parsed.scrollbar,
                          "settings parser should keep prior value for invalid scrollbar values");

    char tiny[8];
    winxterm_smoke_expect(state,
                          !winxterm_settings_format(&settings, tiny, sizeof(tiny)),
                          "settings formatting should reject tiny buffers");
}

static void winxterm_smoke_test_grouped_u64_format(WinxtermSmokeState *state)
{
    wchar_t grouped[WINXTERM_DIAG_GROUPED_U64_CAPACITY];
    winxterm_smoke_expect(state,
                          winxterm_diag_format_grouped_u64(0u,
                                                            grouped,
                                                            WINXTERM_DIAG_GROUPED_U64_CAPACITY) &&
                              wcscmp(grouped, L"0") == 0,
                          "grouped uint64 formatter should format zero");
    winxterm_smoke_expect(state,
                          winxterm_diag_format_grouped_u64(100000000000ull,
                                                            grouped,
                                                            WINXTERM_DIAG_GROUPED_U64_CAPACITY) &&
                              wcscmp(grouped, L"100 000 000 000") == 0,
                          "grouped uint64 formatter should use space thousands separators");
    winxterm_smoke_expect(state,
                          winxterm_diag_format_grouped_u64(UINT64_MAX,
                                                            grouped,
                                                            WINXTERM_DIAG_GROUPED_U64_CAPACITY) &&
                              wcscmp(grouped, L"18 446 744 073 709 551 615") == 0,
                          "grouped uint64 formatter should handle uint64 max");
}

static void winxterm_smoke_test_options(WinxtermSmokeState *state)
{
    const wchar_t *smoke_argv[] = {L"winxterm.exe", L"--smoke"};
    WinxtermOptions options;
    winxterm_smoke_expect(state,
                          winxterm_options_parse(2, smoke_argv, &options) == 0,
                          "option parser should accept --smoke");
    winxterm_smoke_expect(state, options.smoke, "option parser should set smoke flag");
    winxterm_smoke_expect(state, !options.help, "option parser should leave help false");
    winxterm_smoke_expect(state,
                          options.unpainted_line_limit == WINXTERM_DEFAULT_UNPAINTED_LINE_LIMIT,
                          "option parser should set default unpainted line limit");
    winxterm_smoke_expect(state,
                          options.display_scale == WINXTERM_DEFAULT_DISPLAY_SCALE,
                          "option parser should set default display scale");

    const wchar_t *demo_argv[] = {L"winxterm.exe", L"--demo"};
    winxterm_smoke_expect(state,
                          winxterm_options_parse(2, demo_argv, &options) == 0,
                          "option parser should accept --demo");
    winxterm_smoke_expect(state, options.demo, "option parser should set demo flag");

    const wchar_t *bench_argv[] = {L"winxterm.exe", L"--glyphbench"};
    winxterm_smoke_expect(state,
                          winxterm_options_parse(2, bench_argv, &options) == 0,
                          "option parser should accept --glyphbench");
    winxterm_smoke_expect(state, options.glyphbench, "option parser should set glyphbench flag");

    const wchar_t *macro_argv[] = {L"winxterm.exe", L"--macro", L"boot.macro"};
    winxterm_smoke_expect(state,
                          winxterm_options_parse(3, macro_argv, &options) == 0 &&
                              options.macro_path != 0 &&
                              wcscmp(options.macro_path, L"boot.macro") == 0,
                          "option parser should accept startup macro path");

    const wchar_t *macro_help_argv[] = {L"winxterm.exe", L"--help", L"macro"};
    winxterm_smoke_expect(state,
                          winxterm_options_parse(3, macro_help_argv, &options) == 0 &&
                              options.help &&
                              options.help_topic != 0 &&
                              wcscmp(options.help_topic, L"macro") == 0 &&
                              options.client_argc == 0,
                          "option parser should accept macro help topic");

    const wchar_t *render_argv[] = {L"winxterm.exe", L"--unpaintedlines", L"42"};
    winxterm_smoke_expect(state,
                          winxterm_options_parse(3, render_argv, &options) == 0,
                          "option parser should accept backlog option");
    winxterm_smoke_expect(state,
                          options.unpainted_line_limit == 42u,
                          "option parser should set custom unpainted line limit");

    const wchar_t *thread_argv[] = {L"winxterm.exe", L"--ncputhreads", L"2"};
    winxterm_smoke_expect(state,
                          winxterm_options_parse(3, thread_argv, &options) != 0,
                          "removed render thread option should be rejected");

    const wchar_t *all_render_argv[] = {L"winxterm.exe", L"--rendermethod", L"all"};
    winxterm_smoke_expect(state,
                          winxterm_options_parse(3, all_render_argv, &options) != 0,
                          "removed renderer option should be rejected");

    const wchar_t *scale_argv[] = {L"winxterm.exe", L"-x", L"2"};
    winxterm_smoke_expect(state,
                          winxterm_options_parse(3, scale_argv, &options) == 0 &&
                              options.display_scale == 2u,
                          "option parser should accept display scale");

    const wchar_t *client_argv[] = {L"winxterm.exe", L"cmd.exe", L"/c", L"echo", L"hello"};
    winxterm_smoke_expect(state,
                          winxterm_options_parse(5, client_argv, &options) == 0,
                          "option parser should accept positional client commands");
    winxterm_smoke_expect(state, options.client_arg_start == 1, "client command should start at first positional arg");
    winxterm_smoke_expect(state, options.client_argc == 4, "client command should preserve remaining args");

    const wchar_t *bad_argv[] = {L"winxterm.exe", L"--unknown"};
    winxterm_smoke_expect(state,
                          winxterm_options_parse(2, bad_argv, &options) != 0,
                          "option parser should reject unknown options");

    const wchar_t *bad_integer_argv[] = {L"winxterm.exe", L"--unpaintedlines", L"0"};
    winxterm_smoke_expect(state,
                          winxterm_options_parse(3, bad_integer_argv, &options) != 0,
                          "option parser should reject non-positive unpainted line limits");

    const wchar_t *missing_scale_argv[] = {L"winxterm.exe", L"-x"};
    winxterm_smoke_expect(state,
                          winxterm_options_parse(2, missing_scale_argv, &options) != 0,
                          "option parser should reject missing display scale");

    const wchar_t *zero_scale_argv[] = {L"winxterm.exe", L"-x", L"0"};
    winxterm_smoke_expect(state,
                          winxterm_options_parse(3, zero_scale_argv, &options) != 0,
                          "option parser should reject zero display scale");

    const wchar_t *large_scale_argv[] = {L"winxterm.exe", L"-x", L"101"};
    winxterm_smoke_expect(state,
                          winxterm_options_parse(3, large_scale_argv, &options) != 0,
                          "option parser should reject too-large display scale");

    const wchar_t *bad_scale_argv[] = {L"winxterm.exe", L"-x", L"abc"};
    winxterm_smoke_expect(state,
                          winxterm_options_parse(3, bad_scale_argv, &options) != 0,
                          "option parser should reject non-integer display scale");
}

static void winxterm_smoke_test_bridge_hardening(WinxtermSmokeState *state)
{
    WinxtermBridge bridge;
    winxterm_smoke_expect(state,
                          winxterm_bridge_init(&bridge, 0, WINXTERM_TERMINAL_COLUMNS, WINXTERM_TERMINAL_ROWS),
                          "bridge should initialize for hardening tests");
    if (bridge.input_buffer == 0) {
        return;
    }
    winxterm_bridge_set_active_session(&bridge, 42u);
    winxterm_smoke_expect(state, winxterm_bridge_active_session(&bridge) == 42u,
                          "bridge should publish active session identity atomically");
    static const uint8_t old_session_input[] = {'o', 'l', 'd'};
    static const uint8_t new_session_input[] = {'n', 'e', 'w'};
    uint8_t *saved_session_input = 0;
    size_t saved_session_input_count = 0u;
    uint8_t routed_session_input[4];
    bool input_switched = winxterm_bridge_queue_input(
        &bridge, old_session_input, sizeof(old_session_input)) &&
        winxterm_bridge_switch_input_session(&bridge, 42u, 0, 0u,
                                             &saved_session_input,
                                             &saved_session_input_count) &&
        saved_session_input_count == 0u &&
        winxterm_bridge_switch_input_session(&bridge, 43u,
                                             new_session_input,
                                             sizeof(new_session_input),
                                             &saved_session_input,
                                             &saved_session_input_count) &&
        saved_session_input_count == sizeof(old_session_input) &&
        memcmp(saved_session_input, old_session_input, sizeof(old_session_input)) == 0 &&
        winxterm_bridge_read_session_input(&bridge, 42u, routed_session_input,
                                           sizeof(routed_session_input)) == 0u &&
        winxterm_bridge_read_session_input(&bridge, 43u, routed_session_input,
                                           sizeof(routed_session_input)) ==
            sizeof(new_session_input) &&
        memcmp(routed_session_input, new_session_input, sizeof(new_session_input)) == 0;
    free(saved_session_input);
    winxterm_smoke_expect(state, input_switched,
                          "input switching should retain old bytes and route pending target bytes");

    static const uint8_t control_query[] = "\x1b]9001;winxterm;v=1;id=7;cmd=query\a";
    winxterm_smoke_expect(state,
                          winxterm_bridge_enqueue_output(&bridge,
                                                        control_query,
                                                        sizeof(control_query) - 1u),
                          "bridge should enqueue winxterm OSC control query");
    bool control_content_changed = false;
    bool control_more_pending = false;
    bool control_presentation_changed = false;
    winxterm_smoke_expect(state,
                          winxterm_bridge_commit_output(&bridge,
                                                       WINXTERM_BRIDGE_OUTPUT_COMMIT_MAX_BYTES,
                                                       &control_content_changed,
                                                       &control_more_pending,
                                                       &control_presentation_changed),
                          "bridge should dispatch winxterm OSC control query");
    uint8_t reply[256];
    size_t reply_count = winxterm_bridge_read_input(&bridge, reply, sizeof(reply));
    static const char expected_reply[] = "id=7;status=ok";
    winxterm_smoke_expect(state,
                          winxterm_smoke_bytes_contain(reply, reply_count, expected_reply),
                          "winxterm OSC query should queue an acknowledgement");
    winxterm_smoke_expect(state,
                          winxterm_smoke_bytes_contain(reply, reply_count, "set-bell") &&
                              !winxterm_bridge_bell_enabled(&bridge),
                          "winxterm OSC query should advertise bell control and default off");
    bool scrollbar_enabled = false;
    winxterm_smoke_expect(state,
                          winxterm_smoke_bytes_contain(reply, reply_count, "set-scrollbar") &&
                              !winxterm_bridge_take_pending_scrollbar(&bridge, &scrollbar_enabled),
                          "winxterm OSC query should advertise scrollbar control with none pending");
    winxterm_bridge_clear_input(&bridge);

    static const uint8_t inline_macro_control[] =
        "\x1b]9001;winxterm;v=1;id=71;cmd=playmacro;"
        "text=typestring%20one%0Awaitms%201\a";
    winxterm_smoke_expect(state,
                          winxterm_bridge_enqueue_output(&bridge,
                                                         inline_macro_control,
                                                         sizeof(inline_macro_control) - 1u) &&
                              winxterm_bridge_commit_output(&bridge,
                                                           WINXTERM_BRIDGE_OUTPUT_COMMIT_MAX_BYTES,
                                                           &control_content_changed,
                                                           &control_more_pending,
                                                           &control_presentation_changed),
                          "bridge should accept an inline macro control request");
    reply_count = winxterm_bridge_read_input(&bridge, reply, sizeof(reply));
    WinxtermMacroRequest macro_request;
    bool took_macro_request = winxterm_bridge_take_macro_request(&bridge, &macro_request);
    static const char expected_inline_macro[] = "typestring one\nwaitms 1";
    winxterm_smoke_expect(state,
                          winxterm_smoke_bytes_contain(reply, reply_count, "id=71;status=ok") &&
                              took_macro_request &&
                              macro_request.path == 0 &&
                              macro_request.text_utf8 != 0 &&
                              macro_request.text_length == sizeof(expected_inline_macro) - 1u &&
                              memcmp(macro_request.text_utf8,
                                     expected_inline_macro,
                                     sizeof(expected_inline_macro) - 1u) == 0,
                          "inline macro control should preserve decoded multiline text");
    free(macro_request.path);
    free(macro_request.text_utf8);
    winxterm_bridge_clear_input(&bridge);

    size_t large_count = WINXTERM_BRIDGE_INPUT_INITIAL_CAPACITY + 1024u;
    uint8_t *large = (uint8_t *)malloc(large_count);
    winxterm_smoke_expect(state, large != 0, "bridge hardening large input buffer should allocate");
    if (large != 0) {
        for (size_t i = 0; i < large_count; ++i) {
            large[i] = (uint8_t)(i & 0xffu);
        }
        winxterm_smoke_expect(state,
                              winxterm_bridge_queue_input(&bridge, large, large_count),
                              "elastic input queue should grow for large enqueue");
        WinxtermBridgeDiagnostics diagnostics;
        winxterm_bridge_copy_diagnostics(&bridge, &diagnostics);
        winxterm_smoke_expect(state,
                              diagnostics.input_capacity >= large_count &&
                                  diagnostics.input_grow_count >= 1u &&
                                  diagnostics.input_high_water == large_count,
                              "elastic input queue diagnostics should record growth and high water");

        uint8_t readback[64];
        size_t copied = winxterm_bridge_read_input(&bridge, readback, sizeof(readback));
        winxterm_smoke_expect(state, copied == sizeof(readback), "bridge input read should copy requested bytes");
        winxterm_smoke_expect(state,
                              memcmp(readback, large, sizeof(readback)) == 0,
                              "elastic input queue should preserve FIFO order after growth");
        winxterm_bridge_clear_input(&bridge);
        free(large);
    }

    uint8_t one = 0x5au;
    winxterm_smoke_expect(state,
                          !winxterm_bridge_queue_input(&bridge,
                                                       &one,
                                                       WINXTERM_BRIDGE_INPUT_MAX_CAPACITY + 1u),
                          "elastic input queue should reject over-cap enqueue atomically");
    WinxtermBridgeDiagnostics diagnostics;
    winxterm_bridge_copy_diagnostics(&bridge, &diagnostics);
    winxterm_smoke_expect(state,
                          diagnostics.input_count == 0u && diagnostics.input_enqueue_failures >= 1u,
                          "over-cap enqueue should leave queue empty and record failure");

    static const uint8_t queued_output[] = "queued output\n";
    winxterm_smoke_expect(state,
                          winxterm_bridge_enqueue_output(&bridge,
                                                         queued_output,
                                                         sizeof(queued_output) - 1u),
                          "bridge should enqueue terminal output without parsing it immediately");
    winxterm_smoke_expect(state,
                          !winxterm_smoke_screen_contains_text(&bridge.screen, "queued"),
                          "queued output should not mutate screen before commit");
    winxterm_smoke_expect(state,
                          (winxterm_bridge_take_frame_request(&bridge) & WINXTERM_FRAME_CAUSE_CONTENT) != 0u,
                          "queued output should request a content frame");
    bool content_changed = false;
    bool more_pending = false;
    bool presentation_changed = false;
    winxterm_smoke_expect(state,
                          winxterm_bridge_commit_output(&bridge,
                                                        WINXTERM_BRIDGE_OUTPUT_COMMIT_MAX_BYTES,
                                                        &content_changed,
                                                        &more_pending,
                                                        &presentation_changed),
                          "bridge should commit queued terminal output");
    winxterm_smoke_expect(state,
                          content_changed && !more_pending && !presentation_changed,
                          "plain queued output should commit as content only");
    winxterm_smoke_expect(state,
                          winxterm_smoke_screen_contains_text(&bridge.screen, "queued output"),
                          "committed queued output should mutate screen");
    winxterm_bridge_copy_diagnostics(&bridge, &diagnostics);
    winxterm_smoke_expect(state,
                          diagnostics.output_queue_high_water >= sizeof(queued_output) - 1u &&
                              diagnostics.output_queue_copy_latency.p50_ns != 0u &&
                              diagnostics.parser_apply_latency.p50_ns != 0u,
                          "output queue diagnostics should record high water and latency distributions");

    WinxtermBridge fast_bridge;
    memset(&fast_bridge, 0, sizeof(fast_bridge));
    winxterm_smoke_expect(state,
                          winxterm_bridge_init(&fast_bridge, 0, 12, 3),
                          "fast-output bridge should initialize");
    if (fast_bridge.output_buffer != 0) {
        char fast_output[128];
        size_t fast_offset = 0u;
        for (int line = 0; line < 8; ++line) {
            int written = sprintf_s(fast_output + fast_offset,
                                    sizeof(fast_output) - fast_offset,
                                    "fast%02d\n",
                                    line);
            if (written <= 0) {
                break;
            }
            fast_offset += (size_t)written;
        }
        winxterm_smoke_expect(state,
                              winxterm_bridge_enqueue_output(&fast_bridge,
                                                             (const uint8_t *)fast_output,
                                                             fast_offset),
                              "fast output should enqueue before model commit");
        content_changed = false;
        more_pending = true;
        presentation_changed = true;
        winxterm_smoke_expect(state,
                              winxterm_bridge_commit_output(&fast_bridge,
                                                            0u,
                                                            &content_changed,
                                                            &more_pending,
                                                            &presentation_changed),
                              "fast output should commit into the model without rendering");
        WinxtermScreenRowView fast_row;
        memset(&fast_row, 0, sizeof(fast_row));
        winxterm_smoke_expect(state,
                              content_changed &&
                                  !more_pending &&
                                  !presentation_changed &&
                                  fast_bridge.screen.scrollback_count >= 6u &&
                                  winxterm_screen_get_primary_view_row(&fast_bridge.screen, 0u, &fast_row) &&
                                  winxterm_smoke_row_view_contains_text(&fast_row, "fast00") &&
                                  winxterm_screen_get_primary_view_row(&fast_bridge.screen, 5u, &fast_row) &&
                                  winxterm_smoke_row_view_contains_text(&fast_row, "fast05"),
                              "fast committed output should preserve scrolled rows in scrollback before any render");
    }
    winxterm_bridge_dispose(&fast_bridge);

    WinxtermBridge wrap_bridge;
    memset(&wrap_bridge, 0, sizeof(wrap_bridge));
    winxterm_smoke_expect(state,
                          winxterm_bridge_init(&wrap_bridge, 0, 40, 2),
                          "output ring wrap bridge should initialize");
    if (wrap_bridge.output_buffer != 0) {
        static const uint8_t wrap_first[] = "abcdefghijklmnopqrstuvwx";
        static const uint8_t wrap_second[] = "YZ0123456789ABCD";
        static const uint8_t wrap_expected[] =
            "abcdefghijklmnopqrstuvwxYZ0123456789ABCD";
        wrap_bridge.output_capacity = 32u;
        bool wrap_changed = false;
        bool wrap_more = false;
        bool wrap_presentation = false;
        bool wrap_ok =
            winxterm_bridge_enqueue_output(&wrap_bridge, wrap_first,
                                           sizeof(wrap_first) - 1u) &&
            winxterm_bridge_commit_output(&wrap_bridge, 20u, &wrap_changed,
                                          &wrap_more, &wrap_presentation) &&
            wrap_more && wrap_bridge.output_head == 20u &&
            winxterm_bridge_enqueue_output(&wrap_bridge, wrap_second,
                                           sizeof(wrap_second) - 1u) &&
            winxterm_bridge_commit_output(&wrap_bridge, 0u, &wrap_changed,
                                          &wrap_more, &wrap_presentation) &&
            !wrap_more && wrap_bridge.output_count == 0u &&
            wrap_bridge.output_head == 0u;
        for (int column = 0; wrap_ok && column < 40; ++column) {
            const WinxtermScreenCell *cell =
                winxterm_screen_cell_at(&wrap_bridge.screen, 0, column);
            wrap_ok = cell != 0 &&
                cell->codepoint == (uint32_t)wrap_expected[column];
        }
        winxterm_smoke_expect(state, wrap_ok,
                              "output ring should preserve exact FIFO order across split wrap");
    }
    winxterm_bridge_dispose(&wrap_bridge);

    WinxtermBridge concurrent_bridge;
    memset(&concurrent_bridge, 0, sizeof(concurrent_bridge));
    winxterm_smoke_expect(state,
                          winxterm_bridge_init(&concurrent_bridge, 0, 200, 2),
                          "concurrent commit bridge should initialize");
    if (concurrent_bridge.output_buffer != 0) {
        uint8_t ordered_output[200];
        for (size_t i = 0u; i < sizeof(ordered_output); ++i) {
            ordered_output[i] = (uint8_t)('A' + (i % 26u));
        }
        WinxtermOutputCommitStressContext stress;
        memset(&stress, 0, sizeof(stress));
        stress.bridge = &concurrent_bridge;
        stress.start_event = CreateEventW(0, TRUE, FALSE, 0);
        HANDLE commit_threads[2] = {0, 0};
        bool concurrent_ok = stress.start_event != 0 &&
            winxterm_bridge_enqueue_output(&concurrent_bridge, ordered_output,
                                           sizeof(ordered_output));
        if (concurrent_ok) {
            commit_threads[0] = CreateThread(0, 0, winxterm_smoke_output_commit_thread,
                                             &stress, 0, 0);
            commit_threads[1] = CreateThread(0, 0, winxterm_smoke_output_commit_thread,
                                             &stress, 0, 0);
            concurrent_ok = commit_threads[0] != 0 && commit_threads[1] != 0;
        }
        if (concurrent_ok) {
            SetEvent(stress.start_event);
            concurrent_ok = WaitForMultipleObjects(2, commit_threads, TRUE, 10000u) ==
                                WAIT_OBJECT_0 &&
                            stress.failures == 0 && concurrent_bridge.output_count == 0u;
        } else if (stress.start_event != 0) {
            SetEvent(stress.start_event);
        }
        for (size_t i = 0u; concurrent_ok && i < sizeof(ordered_output); ++i) {
            const WinxtermScreenCell *cell =
                winxterm_screen_cell_at(&concurrent_bridge.screen, 0, (int)i);
            concurrent_ok = cell != 0 && cell->codepoint == ordered_output[i];
        }
        if (commit_threads[0] != 0) {
            (void)WaitForSingleObject(commit_threads[0], INFINITE);
            CloseHandle(commit_threads[0]);
        }
        if (commit_threads[1] != 0) {
            (void)WaitForSingleObject(commit_threads[1], INFINITE);
            CloseHandle(commit_threads[1]);
        }
        if (stress.start_event != 0) CloseHandle(stress.start_event);
        winxterm_smoke_expect(state, concurrent_ok,
                              "commit lock should preserve parser order across consumers");
    }
    winxterm_bridge_dispose(&concurrent_bridge);

    static const uint8_t queue_byte[] = "Q";
    winxterm_smoke_expect(state,
                          winxterm_bridge_enqueue_output(&bridge, queue_byte, sizeof(queue_byte) - 1u),
                          "output room test should enqueue one byte");
    HANDLE queue_shutdown_event = CreateEventW(0, TRUE, TRUE, 0);
    winxterm_smoke_expect(state,
                          queue_shutdown_event != 0,
                          "output room shutdown event should initialize");
    if (queue_shutdown_event != 0) {
        winxterm_smoke_expect(state,
                              !winxterm_bridge_wait_for_output_room(&bridge,
                                                                    WINXTERM_BRIDGE_OUTPUT_MAX_CAPACITY,
                                                                    queue_shutdown_event),
                              "output room wait should stop when shutdown is signaled");
        CloseHandle(queue_shutdown_event);
    }
    winxterm_smoke_expect(state,
                          winxterm_bridge_commit_output(&bridge,
                                                        1u,
                                                        &content_changed,
                                                        &more_pending,
                                                        &presentation_changed),
                          "committing output should release output queue room");
    winxterm_smoke_expect(state,
                          winxterm_bridge_wait_for_output_room(&bridge,
                                                              WINXTERM_BRIDGE_OUTPUT_MAX_CAPACITY,
                                                              0),
                          "drained output queue should have room for another full batch");
    winxterm_smoke_expect(state,
                          winxterm_bridge_enqueue_output_wait(&bridge,
                                                             queue_byte,
                                                             sizeof(queue_byte) - 1u,
                                                             0),
                          "shutdown-aware output enqueue should succeed when room is available");
    winxterm_smoke_expect(state,
                          winxterm_bridge_commit_output(&bridge,
                                                        WINXTERM_BRIDGE_OUTPUT_COMMIT_MAX_BYTES,
                                                        &content_changed,
                                                        &more_pending,
                                                        &presentation_changed),
                          "shutdown-aware output enqueue should remain commit-ready");

    winxterm_bridge_set_output_paused(&bridge, true);
    static const uint8_t paused_output[] = "paused output\n";
    winxterm_smoke_expect(state,
                          winxterm_bridge_enqueue_output(&bridge,
                                                         paused_output,
                                                         sizeof(paused_output) - 1u),
                          "bridge should accept output while output commit is paused");
    content_changed = true;
    more_pending = true;
    presentation_changed = true;
    winxterm_smoke_expect(state,
                          winxterm_bridge_commit_output(&bridge,
                                                        WINXTERM_BRIDGE_OUTPUT_COMMIT_MAX_BYTES,
                                                        &content_changed,
                                                        &more_pending,
                                                        &presentation_changed),
                          "paused output commit should be a no-op, not a failure");
    winxterm_smoke_expect(state,
                          !content_changed && !more_pending && !presentation_changed,
                          "paused output should not be reported as committed or repeatedly pending");
    winxterm_bridge_set_output_paused(&bridge, false);
    winxterm_smoke_expect(state,
                          (winxterm_bridge_take_frame_request(&bridge) & WINXTERM_FRAME_CAUSE_CONTENT) != 0u,
                          "unpausing queued output should request a content frame");
    winxterm_smoke_expect(state,
                          winxterm_bridge_commit_output(&bridge,
                                                        WINXTERM_BRIDGE_OUTPUT_COMMIT_MAX_BYTES,
                                                        &content_changed,
                                                        &more_pending,
                                                        &presentation_changed) &&
                              content_changed,
                          "unpaused queued output should commit");

    static const uint8_t title_bell[] = "\x1b]0;smoke-title\x07\x07";
    winxterm_smoke_expect(state,
                          winxterm_bridge_enqueue_output(&bridge, title_bell, sizeof(title_bell) - 1u),
                          "bridge should enqueue presentation output");
    winxterm_smoke_expect(state,
                          winxterm_bridge_commit_output(&bridge,
                                                        WINXTERM_BRIDGE_OUTPUT_COMMIT_MAX_BYTES,
                                                        &content_changed,
                                                        &more_pending,
                                                        &presentation_changed),
                          "bridge should commit presentation output");
    winxterm_smoke_expect(state,
                          presentation_changed &&
                              strcmp(bridge.terminal_title, "smoke-title") == 0 &&
                              !winxterm_bridge_take_bell(&bridge),
                          "title should update and bell should stay suppressed by default");

    static const uint8_t bell_on[] = "\x1b]9001;winxterm;v=1;id=8;cmd=set-bell;value=on\a\x07";
    winxterm_smoke_expect(state,
                          winxterm_bridge_enqueue_output(&bridge, bell_on, sizeof(bell_on) - 1u),
                          "bridge should enqueue set-bell on control");
    presentation_changed = false;
    winxterm_smoke_expect(state,
                          winxterm_bridge_commit_output(&bridge,
                                                        WINXTERM_BRIDGE_OUTPUT_COMMIT_MAX_BYTES,
                                                        &content_changed,
                                                        &more_pending,
                                                        &presentation_changed),
                          "bridge should commit set-bell on control");
    reply_count = winxterm_bridge_read_input(&bridge, reply, sizeof(reply));
    winxterm_smoke_expect(state,
                          winxterm_smoke_bytes_contain(reply, reply_count, "id=8;status=ok") &&
                              winxterm_bridge_bell_enabled(&bridge) &&
                              presentation_changed &&
                              winxterm_bridge_take_bell(&bridge),
                          "set-bell on should allow a following BEL to blink");
    winxterm_bridge_clear_input(&bridge);

    static const uint8_t bell_off[] = "\x1b]9001;winxterm;v=1;id=9;cmd=set-bell;value=off\a\x07";
    winxterm_smoke_expect(state,
                          winxterm_bridge_enqueue_output(&bridge, bell_off, sizeof(bell_off) - 1u),
                          "bridge should enqueue set-bell off control");
    presentation_changed = true;
    winxterm_smoke_expect(state,
                          winxterm_bridge_commit_output(&bridge,
                                                        WINXTERM_BRIDGE_OUTPUT_COMMIT_MAX_BYTES,
                                                        &content_changed,
                                                        &more_pending,
                                                        &presentation_changed),
                          "bridge should commit set-bell off control");
    reply_count = winxterm_bridge_read_input(&bridge, reply, sizeof(reply));
    winxterm_smoke_expect(state,
                          winxterm_smoke_bytes_contain(reply, reply_count, "id=9;status=ok") &&
                              !winxterm_bridge_bell_enabled(&bridge) &&
                              !winxterm_bridge_take_bell(&bridge),
                          "set-bell off should suppress a following BEL");
    winxterm_bridge_clear_input(&bridge);

    static const uint8_t scrollbar_on[] = "\x1b]9001;winxterm;v=1;id=10;cmd=set-scrollbar;value=on\a";
    winxterm_smoke_expect(state,
                          winxterm_bridge_enqueue_output(&bridge, scrollbar_on, sizeof(scrollbar_on) - 1u),
                          "bridge should enqueue set-scrollbar on control");
    winxterm_smoke_expect(state,
                          winxterm_bridge_commit_output(&bridge,
                                                        WINXTERM_BRIDGE_OUTPUT_COMMIT_MAX_BYTES,
                                                        &content_changed,
                                                        &more_pending,
                                                        &presentation_changed),
                          "bridge should commit set-scrollbar on control");
    reply_count = winxterm_bridge_read_input(&bridge, reply, sizeof(reply));
    bool pending_scrollbar_enabled = false;
    winxterm_smoke_expect(state,
                          winxterm_smoke_bytes_contain(reply, reply_count, "id=10;status=ok") &&
                              winxterm_bridge_take_pending_scrollbar(&bridge, &pending_scrollbar_enabled) &&
                              pending_scrollbar_enabled &&
                              !winxterm_bridge_take_pending_scrollbar(&bridge, &pending_scrollbar_enabled),
                          "set-scrollbar on should queue a single pending enable toggle");
    winxterm_bridge_clear_input(&bridge);

    static const uint8_t scrollbar_off[] = "\x1b]9001;winxterm;v=1;id=11;cmd=set-scrollbar;value=off\a";
    winxterm_smoke_expect(state,
                          winxterm_bridge_enqueue_output(&bridge, scrollbar_off, sizeof(scrollbar_off) - 1u),
                          "bridge should enqueue set-scrollbar off control");
    winxterm_smoke_expect(state,
                          winxterm_bridge_commit_output(&bridge,
                                                        WINXTERM_BRIDGE_OUTPUT_COMMIT_MAX_BYTES,
                                                        &content_changed,
                                                        &more_pending,
                                                        &presentation_changed),
                          "bridge should commit set-scrollbar off control");
    reply_count = winxterm_bridge_read_input(&bridge, reply, sizeof(reply));
    pending_scrollbar_enabled = true;
    winxterm_smoke_expect(state,
                          winxterm_smoke_bytes_contain(reply, reply_count, "id=11;status=ok") &&
                              winxterm_bridge_take_pending_scrollbar(&bridge, &pending_scrollbar_enabled) &&
                              !pending_scrollbar_enabled,
                          "set-scrollbar off should queue a pending disable toggle");
    winxterm_bridge_clear_input(&bridge);

    static const uint8_t scrollbar_invalid[] = "\x1b]9001;winxterm;v=1;id=12;cmd=set-scrollbar;value=maybe\a";
    winxterm_smoke_expect(state,
                          winxterm_bridge_enqueue_output(&bridge, scrollbar_invalid, sizeof(scrollbar_invalid) - 1u),
                          "bridge should enqueue invalid set-scrollbar control");
    winxterm_smoke_expect(state,
                          winxterm_bridge_commit_output(&bridge,
                                                        WINXTERM_BRIDGE_OUTPUT_COMMIT_MAX_BYTES,
                                                        &content_changed,
                                                        &more_pending,
                                                        &presentation_changed),
                          "bridge should commit invalid set-scrollbar control");
    reply_count = winxterm_bridge_read_input(&bridge, reply, sizeof(reply));
    winxterm_smoke_expect(state,
                          winxterm_smoke_bytes_contain(reply, reply_count, "id=12;status=error") &&
                              !winxterm_bridge_take_pending_scrollbar(&bridge, &pending_scrollbar_enabled),
                          "invalid set-scrollbar value should report an error and queue nothing");
    winxterm_bridge_clear_input(&bridge);

    winxterm_bridge_set_unpainted_line_limit(&bridge, 1u);
    winxterm_bridge_mark_painted(&bridge);
    WinxtermUtf8Decoder budget_decoder;
    winxterm_utf8_decoder_init(&budget_decoder);
    static const uint8_t budget_first[] = "budget one\n";
    winxterm_smoke_expect(state,
                          winxterm_client_write_bytes_with_policy(&bridge,
                                                                  &budget_decoder,
                                                                  budget_first,
                                                                  sizeof(budget_first) - 1u,
                                                                  0,
                                                                  true),
                          "client write should wait for available unpainted budget");
    winxterm_smoke_expect(state,
                          winxterm_bridge_commit_output(&bridge,
                                                        0u,
                                                        &content_changed,
                                                        &more_pending,
                                                        &presentation_changed),
                          "terminal should apply output before charging visual-line budget");
    winxterm_smoke_expect(state,
                          bridge.unpainted_lines == 1u,
                          "applied hard line advance should consume visual-line budget");
    HANDLE blocked_budget_event = CreateEventW(0, TRUE, TRUE, 0);
    winxterm_smoke_expect(state,
                          blocked_budget_event != 0,
                          "budget smoke shutdown event should initialize");
    static const uint8_t budget_blocked[] = "budget blocked\n";
    size_t output_before_blocked_write = bridge.output_count;
    if (blocked_budget_event != 0) {
        winxterm_smoke_expect(state,
                              !winxterm_client_write_bytes_with_policy(&bridge,
                                                                       &budget_decoder,
                                                                       budget_blocked,
                                                                       sizeof(budget_blocked) - 1u,
                                                                       blocked_budget_event,
                                                                       true),
                              "client write should stop waiting when shutdown is signaled");
        CloseHandle(blocked_budget_event);
    }
    winxterm_smoke_expect(state,
                          bridge.output_count == output_before_blocked_write,
                          "budget-blocked client write should not enqueue output");
    winxterm_bridge_mark_painted(&bridge);
    winxterm_smoke_expect(state,
                          bridge.unpainted_lines == 0u,
                          "mark painted should release unpainted budget");
    static const uint8_t budget_second[] = "budget two\n";
    winxterm_smoke_expect(state,
                          winxterm_client_write_bytes_with_policy(&bridge,
                                                                  &budget_decoder,
                                                                  budget_second,
                                                                  sizeof(budget_second) - 1u,
                                                                  0,
                                                                  true),
                          "client write should resume after painted budget release");
    winxterm_smoke_expect(state,
                          winxterm_bridge_commit_output(&bridge,
                                                        0u,
                                                        &content_changed,
                                                        &more_pending,
                                                        &presentation_changed) &&
                              content_changed &&
                              !more_pending,
                          "budget smoke output should drain after budget checks");
    winxterm_smoke_expect(state,
                          bridge.unpainted_lines == 1u,
                          "resumed applied output should consume visual-line budget");
    uint64_t covered_visual_lines = bridge.accepted_visual_lines;
    winxterm_bridge_add_unpainted_lines(&bridge, 1u);
    winxterm_bridge_mark_painted_through(&bridge, covered_visual_lines);
    winxterm_smoke_expect(state,
                          bridge.unpainted_lines == 1u,
                          "paint coverage should not release lines accepted after that surface");
    winxterm_bridge_mark_painted(&bridge);

    uint8_t soft_wrap_output[WINXTERM_TERMINAL_COLUMNS + 1u];
    memset(soft_wrap_output, 'x', sizeof(soft_wrap_output));
    winxterm_smoke_expect(state,
                          winxterm_bridge_enqueue_output(&bridge,
                                                         soft_wrap_output,
                                                         sizeof(soft_wrap_output)) &&
                              winxterm_bridge_commit_output(&bridge,
                                                            0u,
                                                            &content_changed,
                                                            &more_pending,
                                                            &presentation_changed) &&
                              bridge.unpainted_lines == 1u,
                          "soft wrap should consume visual-line budget without a newline");
    winxterm_bridge_mark_painted(&bridge);

    static const uint8_t split_output[] = "first line\nsecond line\n";
    winxterm_smoke_expect(state,
                          winxterm_bridge_enqueue_output(&bridge,
                                                         split_output,
                                                         sizeof(split_output) - 1u),
                          "bridge should enqueue split render output");
    content_changed = false;
    more_pending = false;
    presentation_changed = false;
    winxterm_smoke_expect(state,
                          winxterm_bridge_commit_output(&bridge,
                                                        6u,
                                                        &content_changed,
                                                        &more_pending,
                                                        &presentation_changed) &&
                              content_changed &&
                              more_pending,
                          "partial output commit should report pending text before render");
    winxterm_smoke_expect(state,
                          winxterm_bridge_commit_output(&bridge,
                                                        0u,
                                                        &content_changed,
                                                        &more_pending,
                                                        &presentation_changed) &&
                              content_changed &&
                              !more_pending &&
                              winxterm_smoke_screen_contains_text(&bridge.screen, "second line"),
                          "remaining split output should commit before final render");

    winxterm_bridge_resize_terminal(&bridge, 100, 30);
    winxterm_bridge_resize_terminal(&bridge, 101, 31);
    int columns = 0;
    int rows = 0;
    winxterm_smoke_expect(state,
                          winxterm_bridge_peek_pending_resize(&bridge, &columns, &rows) &&
                              columns == 101 &&
                              rows == 31,
                          "bridge should peek coalesced pending resize");
    winxterm_bridge_copy_diagnostics(&bridge, &diagnostics);
    winxterm_smoke_expect(state,
                          diagnostics.resize_request_count == 2u &&
                              diagnostics.resize_coalesced_count == 1u &&
                              diagnostics.resize_taken_count == 1u &&
                              diagnostics.resize_applied_count == 0u,
                          "resize diagnostics should record peeked but unapplied resize");
    columns = 0;
    rows = 0;
    winxterm_smoke_expect(state,
                          winxterm_bridge_peek_pending_resize(&bridge, &columns, &rows) &&
                              columns == 101 &&
                              rows == 31,
                          "unacknowledged pending resize should remain available");
    winxterm_bridge_resize_terminal(&bridge, 102, 32);
    winxterm_smoke_expect(state,
                          !winxterm_bridge_ack_pending_resize(&bridge, 101, 31),
                          "older resize ack should not clear newer pending resize");
    columns = 0;
    rows = 0;
    winxterm_smoke_expect(state,
                          winxterm_bridge_peek_pending_resize(&bridge, &columns, &rows) &&
                              columns == 102 &&
                              rows == 32,
                          "newer pending resize should survive older ack");
    winxterm_smoke_expect(state,
                          winxterm_bridge_ack_pending_resize(&bridge, 102, 32),
                          "matching resize ack should clear pending resize");
    winxterm_smoke_expect(state,
                          !winxterm_bridge_peek_pending_resize(&bridge, &columns, &rows),
                          "acked resize should exhaust pending resize");
    winxterm_bridge_copy_diagnostics(&bridge, &diagnostics);
    winxterm_smoke_expect(state,
                          diagnostics.resize_request_count == 3u &&
                              diagnostics.resize_coalesced_count == 2u &&
                              diagnostics.resize_taken_count == 3u &&
                              diagnostics.resize_applied_count == 2u,
                          "resize diagnostics should record ack-based coalescing");

    winxterm_bridge_request_display_scale(&bridge, 2u);
    winxterm_bridge_request_display_scale(&bridge, 3u);
    unsigned int display_scale = 0u;
    winxterm_smoke_expect(state,
                          winxterm_bridge_take_pending_display_scale(&bridge, &display_scale) &&
                              display_scale == 3u &&
                              !winxterm_bridge_take_pending_display_scale(&bridge, &display_scale),
                          "bridge should return coalesced pending display scale");
    winxterm_bridge_copy_diagnostics(&bridge, &diagnostics);
    winxterm_smoke_expect(state,
                          diagnostics.scale_request_count == 2u &&
                              diagnostics.scale_coalesced_count == 1u &&
                              diagnostics.scale_taken_count == 1u,
                          "scale diagnostics should record latest-scale coalescing");

    winxterm_bridge_set_host_starting(&bridge);
    winxterm_bridge_set_host_child(&bridge, 1234u, L"child.exe", true);
    WinxtermHostChildInfo child;
    memset(&child, 0, sizeof(child));
    winxterm_smoke_expect(state,
                          winxterm_bridge_copy_child_info(&bridge, &child) &&
                              child.running &&
                              child.is_shell &&
                              child.process_id == 1234u &&
                              wcscmp(child.display_name, L"child.exe") == 0,
                          "bridge should copy direct child close-dialog info");
    winxterm_bridge_request_headless(&bridge);
    winxterm_smoke_expect(state,
                          winxterm_bridge_is_headless(&bridge),
                          "bridge should enter headless mode");
    static const uint8_t headless_output[] = "one\ntwo\n";
    winxterm_bridge_note_headless_output(&bridge,
                                         headless_output,
                                         sizeof(headless_output) - 1u);
    winxterm_bridge_copy_diagnostics(&bridge, &diagnostics);
    winxterm_smoke_expect(state,
                          diagnostics.headless_output_bytes == sizeof(headless_output) - 1u &&
                              diagnostics.headless_output_lines == 2u,
                          "headless diagnostics should count drained output");
    uint8_t *transcript = 0;
    size_t transcript_count = 0u;
    winxterm_smoke_expect(state,
                          winxterm_bridge_copy_transcript(&bridge, &transcript, &transcript_count),
                          "bridge should copy raw output transcript");
    winxterm_smoke_expect(state,
                          transcript != 0 &&
                              transcript_count >= sizeof(headless_output) - 1u &&
                              memcmp(transcript + transcript_count - (sizeof(headless_output) - 1u),
                                     headless_output,
                                     sizeof(headless_output) - 1u) == 0,
                          "raw output transcript should preserve latest headless bytes");
    free(transcript);
    winxterm_bridge_request_terminate(&bridge);
    winxterm_smoke_expect(state,
                          winxterm_bridge_terminate_requested(&bridge),
                          "bridge should record terminate request");

    winxterm_smoke_expect(state,
                          winxterm_bridge_request_job_action(
                              &bridge, WINXTERM_BRIDGE_JOB_ACTION_FOREGROUND, 7u) &&
                              winxterm_bridge_request_job_action(
                                  &bridge, WINXTERM_BRIDGE_JOB_ACTION_BACKGROUND, 8u),
                          "bridge should queue multiple native job actions");
    WinxtermBridgeJobAction job_action = WINXTERM_BRIDGE_JOB_ACTION_NONE;
    uint64_t action_job_id = 0u;
    winxterm_smoke_expect(state,
                          winxterm_bridge_take_job_action(&bridge, &job_action, &action_job_id) &&
                              job_action == WINXTERM_BRIDGE_JOB_ACTION_FOREGROUND &&
                              action_job_id == 7u,
                          "bridge should preserve native job action FIFO order");
    winxterm_smoke_expect(state,
                          winxterm_bridge_take_job_action(&bridge, &job_action, &action_job_id) &&
                              job_action == WINXTERM_BRIDGE_JOB_ACTION_BACKGROUND &&
                              action_job_id == 8u &&
                              !winxterm_bridge_take_job_action(&bridge, &job_action, &action_job_id),
                          "bridge should drain every queued native job action exactly once");

    winxterm_bridge_dispose(&bridge);
}

static void winxterm_smoke_test_screen_and_text(WinxtermSmokeState *state)
{
    WinxtermScreen screen;
    winxterm_smoke_expect(state, winxterm_screen_init(&screen, 10, 3), "screen should initialize");
    winxterm_smoke_expect(state,
                          winxterm_screen_map_codepoint_to_glyph(0x41u) == 0x41u,
                          "Latin-1 codepoint should map directly");
    winxterm_smoke_expect(state,
                          winxterm_screen_map_codepoint_to_glyph(0x20acu) == WINXTERM_DYNAMIC_GLYPH_INDEX,
                          "non-Latin-1 codepoint should map to dynamic glyph rendering");

    WinxtermUtf8Decoder decoder;
    winxterm_utf8_decoder_init(&decoder);
    const uint8_t bytes[] = {'a', 'b', '\r', 'c', '\n', 0xe2u, 0x82u, 0xacu};
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen, &decoder, bytes, sizeof(bytes)),
                          "UTF-8 text should feed into screen");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 0, 0)->codepoint == (uint32_t)'c' &&
                              winxterm_screen_cell_at(&screen, 0, 1)->codepoint == (uint32_t)'b',
                          "carriage return should overwrite the current grid row");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 1, 0)->glyph_index == WINXTERM_DYNAMIC_GLYPH_INDEX,
                          "UTF-8 non-Latin-1 output should use dynamic glyph rendering");

    winxterm_screen_clear_current_session(&screen);
    winxterm_smoke_expect(state, screen.cursor_row == 0 && screen.cursor_col == 0, "clear should reset cursor");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 0, 0)->codepoint == (uint32_t)' ',
                          "clear should blank the active grid");

    winxterm_utf8_decoder_init(&decoder);
    static const char sgr_bytes[] = "\x1b[40mA\x1b[42mB\x1b[44mC\x1b[41mD\x1b[48;5;196mE\x1b[48;2;1;2;3mF";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)sgr_bytes,
                                                   sizeof(sgr_bytes) - 1u),
                          "SGR background colors should feed into screen");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 0, 0)->background_rgb == 0x00000000u &&
                              winxterm_screen_cell_at(&screen, 0, 1)->background_rgb == 0x00008000u &&
                              winxterm_screen_cell_at(&screen, 0, 2)->background_rgb == 0x00000080u &&
                              winxterm_screen_cell_at(&screen, 0, 3)->background_rgb == 0x00800000u &&
                              winxterm_screen_cell_at(&screen, 0, 4)->background_rgb == 0x00ff0000u &&
                              winxterm_screen_cell_at(&screen, 0, 5)->background_rgb == 0x00010203u,
                          "SGR background colors should become grid cell attributes");

    winxterm_screen_clear_current_session(&screen);
    winxterm_utf8_decoder_init(&decoder);
    static const char visual_sgr[] =
        "\x1b[1;3;4;7;9;53mA"
        "\x1b[22;23;24;27;29;55mB"
        "\x1b[31m\x1b[#{" "\x1b[32mC" "\x1b[#}D";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)visual_sgr,
                                                   sizeof(visual_sgr) - 1u),
                          "extended SGR attributes should feed into screen");
    uint32_t a_flags = winxterm_screen_cell_at(&screen, 0, 0)->attribute_flags;
    winxterm_smoke_expect(state,
                          (a_flags & (WINXTERM_SCREEN_CELL_BOLD |
                                      WINXTERM_SCREEN_CELL_ITALIC |
                                      WINXTERM_SCREEN_CELL_UNDERLINE |
                                      WINXTERM_SCREEN_CELL_INVERSE |
                                      WINXTERM_SCREEN_CELL_CROSSED_OUT |
                                      WINXTERM_SCREEN_CELL_OVERLINE)) ==
                              (WINXTERM_SCREEN_CELL_BOLD |
                               WINXTERM_SCREEN_CELL_ITALIC |
                               WINXTERM_SCREEN_CELL_UNDERLINE |
                               WINXTERM_SCREEN_CELL_INVERSE |
                               WINXTERM_SCREEN_CELL_CROSSED_OUT |
                               WINXTERM_SCREEN_CELL_OVERLINE),
                          "SGR visual flags should be stored on cells");
    winxterm_smoke_expect(state,
                          (winxterm_screen_cell_at(&screen, 0, 1)->attribute_flags &
                           WINXTERM_SCREEN_CELL_VISUAL_MASK) == 0u,
                          "SGR reset submodes should clear visual flags");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 0, 2)->foreground_palette_index == 2u &&
                              winxterm_screen_cell_at(&screen, 0, 3)->foreground_palette_index == 1u,
                          "SGR push/pop should restore indexed foreground state");

    winxterm_screen_clear_current_session(&screen);
    winxterm_utf8_decoder_init(&decoder);
    static const char palette_bytes[] =
        "\x1b]4;1;#112233\x07"
        "\x1b[31mA"
        "\x1b]10;#010203\x07"
        "\x1b[39mB";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)palette_bytes,
                                                   sizeof(palette_bytes) - 1u),
                          "OSC palette/default colors should feed into screen");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 0, 0)->foreground_rgb == 0x00112233u &&
                              (winxterm_screen_cell_at(&screen, 0, 0)->color_flags &
                               WINXTERM_SCREEN_COLOR_FOREGROUND_INDEXED) != 0u,
                          "OSC 4 should mutate indexed foreground colors for future cells");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 0, 1)->foreground_rgb == 0x00010203u &&
                              (winxterm_screen_cell_at(&screen, 0, 1)->color_flags &
                               WINXTERM_SCREEN_COLOR_FOREGROUND_DEFAULT) != 0u,
                          "OSC 10 should mutate default foreground for future default cells");

    winxterm_screen_clear_current_session(&screen);
    winxterm_utf8_decoder_init(&decoder);
    static const uint8_t unicode_bytes[] = {'A', 0xccu, 0x81u, 0xe4u, 0xb8u, 0xadu, 'B'};
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen, &decoder, unicode_bytes, sizeof(unicode_bytes)),
                          "combining and Unicode fallback should feed into screen");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 0, 0)->combining_count == 1u &&
                              winxterm_screen_cell_at(&screen, 0, 0)->combining_codepoints[0] == 0x0301u,
                          "combining marks should attach to the preceding base cell");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 0, 1)->width == 1u &&
                              !winxterm_screen_cell_at(&screen, 0, 2)->continuation &&
                              winxterm_screen_cell_at(&screen, 0, 2)->codepoint == (uint32_t)'B',
                          "Unicode fallback should occupy one cell");

    winxterm_screen_clear_current_session(&screen);
    winxterm_utf8_decoder_init(&decoder);
    static const char dec_graphics[] = "\x1b(0qx\x1b(B";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)dec_graphics,
                                                   sizeof(dec_graphics) - 1u),
                          "DEC special graphics should feed into screen");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 0, 0)->codepoint == 0x2500u &&
                              winxterm_screen_cell_at(&screen, 0, 0)->glyph_index == WINXTERM_DYNAMIC_GLYPH_INDEX &&
                              winxterm_screen_cell_at(&screen, 0, 1)->codepoint == 0x2502u &&
                              winxterm_screen_cell_at(&screen, 0, 1)->glyph_index == WINXTERM_DYNAMIC_GLYPH_INDEX,
                          "DEC graphics should keep Unicode codepoints for TTF fallback");

    winxterm_screen_clear_current_session(&screen);
    winxterm_utf8_decoder_init(&decoder);
    static const char csi_bytes[] = "abc\x1b[2;5HZ\x1b[1K\x1b[2;3r\x1b[?6h\x1b[Hq\x1b[?6l";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)csi_bytes,
                                                   sizeof(csi_bytes) - 1u),
                          "CSI cursor, erase, scroll region and origin mode should feed into screen");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 1, 4)->codepoint == (uint32_t)' ',
                          "erase-to-beginning should clear the addressed row");
    winxterm_smoke_expect(state,
                          screen.scroll_top == 1 && screen.scroll_bottom == 2 && !screen.origin_mode,
                          "scroll region and origin mode should update screen state");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 1, 0)->codepoint == (uint32_t)'q',
                          "origin-mode cursor addressing should be relative to scroll region");

    winxterm_screen_clear_current_session(&screen);
    winxterm_utf8_decoder_init(&decoder);
    static const char progress_rewrite[] =
        "before\r\n"
        "[1]\r\x1b[2K[2]\r\x1b[2K[3]";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)progress_rewrite,
                                                   sizeof(progress_rewrite) - 1u),
                          "progress-line rewrite should feed into screen");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 0, 0)->codepoint == (uint32_t)'b' &&
                              winxterm_screen_cell_at(&screen, 1, 0)->codepoint == (uint32_t)'[' &&
                              winxterm_screen_cell_at(&screen, 1, 1)->codepoint == (uint32_t)'3',
                          "progress-line rewrite should preserve previous active rows");

    winxterm_screen_clear_current_session(&screen);
    winxterm_utf8_decoder_init(&decoder);
    static const char visual_erase[] = "\x1b[34;44;4mX\x1b[2J";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)visual_erase,
                                                   sizeof(visual_erase) - 1u),
                          "erase display with visual attributes should feed into screen");
    const WinxtermScreenCell *erased = winxterm_screen_cell_at(&screen, 0, 0);
    winxterm_smoke_expect(state,
                          erased != 0 &&
                              erased->codepoint == (uint32_t)' ' &&
                              (erased->attribute_flags & WINXTERM_SCREEN_CELL_VISUAL_MASK) == 0u &&
                              (erased->color_flags & WINXTERM_SCREEN_COLOR_FOREGROUND_DEFAULT) != 0u &&
                              (erased->color_flags & WINXTERM_SCREEN_COLOR_BACKGROUND_INDEXED) != 0u &&
                              erased->background_palette_index == 4u,
                          "erase display should keep background color but drop underline/foreground visuals");

    winxterm_screen_clear_current_session(&screen);
    winxterm_utf8_decoder_init(&decoder);
    static const char alt_bytes[] = "P\x1b[?1049hA\x1b[?1049l";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)alt_bytes,
                                                   sizeof(alt_bytes) - 1u),
                          "alternate screen sequence should feed into screen");
    winxterm_smoke_expect(state,
                          !screen.alternate_active &&
                              winxterm_screen_cell_at(&screen, 0, 0)->codepoint == (uint32_t)'P',
                          "alternate screen should not overwrite primary grid");
    winxterm_screen_dispose(&screen);
}

static void winxterm_smoke_test_parser_strings(WinxtermSmokeState *state)
{
    WinxtermScreen screen;
    winxterm_smoke_expect(state, winxterm_screen_init(&screen, 10, 3), "string parser screen should initialize");

    WinxtermUtf8Decoder decoder;
    winxterm_utf8_decoder_init(&decoder);
    static const char dcs_ignore[] = "A\x1bPunsupported payload\x1b\\B";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)dcs_ignore,
                                                   sizeof(dcs_ignore) - 1u),
                          "unsupported DCS should be safely ignored");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 0, 0)->codepoint == (uint32_t)'A' &&
                              winxterm_screen_cell_at(&screen, 0, 1)->codepoint == (uint32_t)'B',
                          "unsupported DCS payload should not leak printable bytes");
    const WinxtermParserDiagnostics *diagnostics = winxterm_text_diagnostics(&decoder);
    winxterm_smoke_expect(state,
                          diagnostics != 0 && diagnostics->ignored_strings != 0u,
                          "unsupported DCS should increment ignored string diagnostics");

    winxterm_utf8_decoder_init(&decoder);
    WinxtermProtocolCapture capture = {0};
    capture.screen = &screen;
    static const char c1_osc[] = "\x9d" "0;c1-title" "\x9c";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes_to_sink(&decoder,
                                                           (const uint8_t *)c1_osc,
                                                           sizeof(c1_osc) - 1u,
                                                           winxterm_smoke_protocol_sink,
                                                           &capture),
                          "C1 OSC should parse and terminate on ST");
    winxterm_smoke_expect(state,
                          capture.op_count >= 2u &&
                              capture.ops[0].type == WINXTERM_TERMINAL_OP_TITLE &&
                              memcmp(capture.ops[0].data.title.text, "c1-title", 8u) == 0,
                          "C1 OSC title should emit title operation");

    winxterm_utf8_decoder_init(&decoder);
    char overflow[WINXTERM_TERMINAL_OSC_PAYLOAD_CAPACITY + 32u];
    memset(overflow, 'A', sizeof(overflow));
    overflow[0] = 0x1b;
    overflow[1] = ']';
    overflow[2] = '8';
    overflow[3] = ';';
    overflow[sizeof(overflow) - 2u] = 0x07;
    overflow[sizeof(overflow) - 1u] = '\0';
    capture.op_count = 0u;
    capture.reply_count = 0u;
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes_to_sink(&decoder,
                                                           (const uint8_t *)overflow,
                                                           sizeof(overflow) - 1u,
                                                           winxterm_smoke_protocol_sink,
                                                           &capture),
                          "overflowing OSC should finish safely");
    diagnostics = winxterm_text_diagnostics(&decoder);
    winxterm_smoke_expect(state,
                          diagnostics != 0 && diagnostics->string_overflow != 0u,
                          "overflowing OSC should increment string overflow diagnostics");
    winxterm_screen_dispose(&screen);
}

static void winxterm_smoke_test_mode_registry(WinxtermSmokeState *state)
{
    WinxtermScreen screen;
    winxterm_smoke_expect(state, winxterm_screen_init(&screen, 10, 3), "mode registry screen should initialize");
    WinxtermUtf8Decoder decoder;
    winxterm_utf8_decoder_init(&decoder);
    static const char modes[] = "\x1b[?1h\x1b[?2004h\x1b[?1s\x1b[?1l\x1b[?1r\x1b[?2004l";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)modes,
                                                   sizeof(modes) - 1u),
                          "mode set reset save restore should feed");
    winxterm_smoke_expect(state,
                          winxterm_screen_mode_enabled(&screen, WINXTERM_TERMINAL_MODE_APPLICATION_CURSOR),
                          "private mode restore should restore application cursor mode");
    winxterm_smoke_expect(state,
                          !winxterm_screen_mode_enabled(&screen, WINXTERM_TERMINAL_MODE_BRACKETED_PASTE),
                          "bracketed paste should reset through shared mode registry");
    winxterm_screen_dispose(&screen);
}

static void winxterm_smoke_test_query_replies(WinxtermSmokeState *state)
{
    WinxtermScreen screen;
    winxterm_smoke_expect(state, winxterm_screen_init(&screen, 10, 3), "query reply screen should initialize");
    WinxtermUtf8Decoder decoder;
    winxterm_utf8_decoder_init(&decoder);
    WinxtermProtocolCapture capture = {0};
    capture.screen = &screen;
    static const char queries[] =
        "\x1b[2;3H"
        "\x1b[c"
        "\x1b[>c"
        "\x1b[5n"
        "\x1b[6n"
        "\x1b[?2004$p"
        "\x1b[>0q"
        "\x1b[>4q"
        "\x1b[>1q"
        "\x1bP$qm\x1b\\";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes_to_sink(&decoder,
                                                           (const uint8_t *)queries,
                                                           sizeof(queries) - 1u,
                                                           winxterm_smoke_protocol_sink,
                                                           &capture),
                          "query sequences should feed into protocol sink");
    winxterm_smoke_expect(state,
                          winxterm_smoke_replies_contain(&capture, "\x1b[?1;2c", 7u),
                          "primary DA reply should be emitted");
    winxterm_smoke_expect(state,
                          winxterm_smoke_replies_contain(&capture, "\x1b[>0;410;0c", 11u),
                          "secondary DA reply should be emitted");
    winxterm_smoke_expect(state,
                          winxterm_smoke_replies_contain(&capture, "\x1b[0n", 4u),
                          "DSR ready reply should be emitted");
    winxterm_smoke_expect(state,
                          winxterm_smoke_replies_contain(&capture, "\x1b[2;3R", 6u),
                          "CPR reply should use current cursor position");
    winxterm_smoke_expect(state,
                          winxterm_smoke_replies_contain(&capture, "\x1b[?2004;2$y", 11u),
                          "DECRQM should report known reset private modes");
    winxterm_smoke_expect(state,
                          winxterm_smoke_replies_contain(&capture, "Winxterm 0.1.0", 14u),
                          "XTVERSION should report winxterm identity");
    winxterm_smoke_expect(state,
                          winxterm_smoke_replies_contain(&capture, "\x1bP1$rm\x1b\\", 7u),
                          "DECRQSS SGR query should get a success reply");
    winxterm_screen_dispose(&screen);
}

static void winxterm_smoke_test_osc_policy(WinxtermSmokeState *state)
{
    WinxtermScreen screen;
    winxterm_smoke_expect(state, winxterm_screen_init(&screen, 10, 3), "OSC policy screen should initialize");
    WinxtermUtf8Decoder decoder;
    winxterm_utf8_decoder_init(&decoder);
    WinxtermProtocolCapture capture = {0};
    capture.screen = &screen;
    static const char osc_bytes[] =
        "\x1b]0;main-title\x07"
        "\x1b]1;icon-title\x07"
        "\x1b]52;c;AAAA\x07"
        "\x1b]8;;https://example.invalid\x1b\\"
        "\x1b]bad\x07";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes_to_sink(&decoder,
                                                           (const uint8_t *)osc_bytes,
                                                           sizeof(osc_bytes) - 1u,
                                                           winxterm_smoke_protocol_sink,
                                                           &capture),
                          "OSC policy fixtures should feed");
    bool saw_title = false;
    bool saw_icon = false;
    bool saw_denied = false;
    bool saw_unsupported = false;
    bool saw_malformed = false;
    for (size_t i = 0u; i < capture.op_count; ++i) {
        if (capture.ops[i].type == WINXTERM_TERMINAL_OP_TITLE) {
            saw_title = true;
        } else if (capture.ops[i].type == WINXTERM_TERMINAL_OP_OSC) {
            saw_icon = saw_icon ||
                (capture.ops[i].data.osc.command == WINXTERM_TERMINAL_OSC_ICON_TITLE &&
                 capture.ops[i].data.osc.outcome == WINXTERM_TERMINAL_OSC_ACCEPTED_NO_CONSUMER);
            saw_denied = saw_denied ||
                capture.ops[i].data.osc.outcome == WINXTERM_TERMINAL_OSC_DENIED_SENSITIVE;
            saw_unsupported = saw_unsupported ||
                capture.ops[i].data.osc.outcome == WINXTERM_TERMINAL_OSC_UNSUPPORTED;
            saw_malformed = saw_malformed ||
                capture.ops[i].data.osc.outcome == WINXTERM_TERMINAL_OSC_MALFORMED;
        }
    }
    winxterm_smoke_expect(state, saw_title, "OSC 0 should still emit title");
    winxterm_smoke_expect(state, saw_icon, "OSC 1 should be accepted without a local consumer");
    winxterm_smoke_expect(state, saw_denied, "OSC 52 should be denied by policy");
    winxterm_smoke_expect(state, saw_unsupported, "OSC 8 should be unsupported until hyperlink metadata exists");
    winxterm_smoke_expect(state, saw_malformed, "malformed OSC should produce a policy diagnostic op");
    const WinxtermParserDiagnostics *diagnostics = winxterm_text_diagnostics(&decoder);
    winxterm_smoke_expect(state,
                          diagnostics != 0 &&
                              diagnostics->denied_osc != 0u &&
                              diagnostics->unsupported_osc != 0u &&
                              diagnostics->malformed_sequences != 0u,
                          "OSC policy should update parser diagnostics");
    winxterm_screen_dispose(&screen);
}

static void winxterm_smoke_test_reset_sequences(WinxtermSmokeState *state)
{
    WinxtermScreen screen;
    winxterm_smoke_expect(state, winxterm_screen_init(&screen, 10, 3), "reset screen should initialize");
    WinxtermUtf8Decoder decoder;
    winxterm_utf8_decoder_init(&decoder);
    static const char alignment[] = "\x1b#8";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)alignment,
                                                   sizeof(alignment) - 1u),
                          "DECALN should feed");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 2, 9)->codepoint == (uint32_t)'E',
                          "DECALN should fill the visible grid with E");

    static const char soft_reset[] = "\x1b[?2004h\x1b[!p";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)soft_reset,
                                                   sizeof(soft_reset) - 1u),
                          "DECSTR should feed");
    winxterm_smoke_expect(state,
                          !winxterm_screen_mode_enabled(&screen, WINXTERM_TERMINAL_MODE_BRACKETED_PASTE) &&
                              winxterm_screen_cell_at(&screen, 2, 9)->codepoint == (uint32_t)'E',
                          "DECSTR should reset modes without clearing visible contents");

    static const char controls_and_hard_reset[] = "\x1b G\x1b F\x1b" "c";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)controls_and_hard_reset,
                                                   sizeof(controls_and_hard_reset) - 1u),
                          "S8C1T S7C1T and RIS should feed");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 0, 0)->codepoint == (uint32_t)' ' &&
                              !screen.alternate_active &&
                              screen.auto_wrap,
                          "RIS should clear screen state and restore defaults");
    winxterm_screen_dispose(&screen);
}

static void winxterm_smoke_test_screen_layout_extensions(WinxtermSmokeState *state)
{
    WinxtermScreen screen;
    winxterm_smoke_expect(state, winxterm_screen_init(&screen, 10, 4), "layout extension screen should initialize");

    WinxtermUtf8Decoder decoder;
    winxterm_utf8_decoder_init(&decoder);
    static const char cursor_tabs[] = "\x1b[3G\x1bH\x1b[1G\x1b[IZ";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)cursor_tabs,
                                                   sizeof(cursor_tabs) - 1u),
                          "HPA, HTS and CHT should feed");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 0, 2)->codepoint == (uint32_t)'Z',
                          "custom tab stop should move the cursor to the set column");

    winxterm_screen_clear_current_session(&screen);
    winxterm_utf8_decoder_init(&decoder);
    static const char reverse_wrap[] = "\x1b[?45h\x1b[2;1H\bR";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)reverse_wrap,
                                                   sizeof(reverse_wrap) - 1u),
                          "reverse-wrap backspace should feed");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 0, 9)->codepoint == (uint32_t)'R',
                          "reverse-wrap should move from the left edge to the previous row");

    winxterm_screen_clear_current_session(&screen);
    winxterm_utf8_decoder_init(&decoder);
    static const char margins[] = "\x1b[2;5sabcdef";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)margins,
                                                   sizeof(margins) - 1u),
                          "horizontal margins should feed");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 0, 0)->codepoint == (uint32_t)' ' &&
                              winxterm_screen_cell_at(&screen, 0, 1)->codepoint == (uint32_t)'a' &&
                              winxterm_screen_cell_at(&screen, 0, 4)->codepoint == (uint32_t)'d' &&
                              winxterm_screen_cell_at(&screen, 1, 1)->codepoint == (uint32_t)'e',
                          "printing should clip and wrap within left-right margins");

    winxterm_screen_clear_current_session(&screen);
    winxterm_utf8_decoder_init(&decoder);
    static const char alt_modes[] = "P\x1b[?47hA\x1b[?47l\x1b[2;3H\x1b[?1048h\x1b[1;1H\x1b[?1048l";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)alt_modes,
                                                   sizeof(alt_modes) - 1u),
                          "alternate and 1048 cursor modes should feed");
    winxterm_smoke_expect(state,
                          !screen.alternate_active &&
                              winxterm_screen_cell_at(&screen, 0, 0)->codepoint == (uint32_t)'P' &&
                              screen.cursor_row == 1 && screen.cursor_col == 2,
                          "47 should preserve primary contents and 1048 should restore cursor");

    winxterm_screen_clear_current_session(&screen);
    winxterm_utf8_decoder_init(&decoder);
    static const char protected_rects[] =
        "\x1b[1\"qP\x1b[0\"qN\x1b[?2J"
        "\x1b[88;2;2;2;4$x"
        "\x1b[2;2;2;4;1;3;1$v";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)protected_rects,
                                                   sizeof(protected_rects) - 1u),
                          "protected text and rectangle operations should feed");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 0, 0)->codepoint == (uint32_t)'P' &&
                              winxterm_screen_cell_at(&screen, 0, 1)->codepoint == (uint32_t)' ' &&
                              winxterm_screen_cell_at(&screen, 1, 1)->codepoint == (uint32_t)'X' &&
                              winxterm_screen_cell_at(&screen, 2, 0)->codepoint == (uint32_t)'X',
                          "selective erase should preserve protected cells and rectangles should mutate cells");

    winxterm_screen_clear_current_session(&screen);
    winxterm_utf8_decoder_init(&decoder);
    static const char dec_graphics[] = "\x1b(0q\x1b(B";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)dec_graphics,
                                                   sizeof(dec_graphics) - 1u),
                          "DEC special graphics charset should feed");
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 0, 0)->codepoint == 0x2500u,
                          "DEC special graphics q should map to a stable line-drawing codepoint");
    winxterm_screen_dispose(&screen);

    winxterm_smoke_expect(state, winxterm_screen_init(&screen, 4, 3),
                          "full-erase wrap metadata screen should initialize");
    winxterm_utf8_decoder_init(&decoder);
    static const char erase_wrapped_screen[] = "abcdefgh\x1b[H\x1b[2J";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)erase_wrapped_screen,
                                                   sizeof(erase_wrapped_screen) - 1u),
                          "wrapped screen clear should feed");
    winxterm_smoke_expect(state,
                          !screen.primary_line_meta[0].soft_wrapped &&
                              !screen.primary_line_meta[1].soft_wrapped &&
                              screen.primary_line_meta[0].content_columns == 0 &&
                              screen.primary_line_meta[1].content_columns == 0,
                          "full display erase should discard stale soft-wrap boundaries");
    winxterm_screen_dispose(&screen);

    winxterm_smoke_expect(state, winxterm_screen_init(&screen, 4, 2),
                          "scrolling saved-cursor screen should initialize");
    winxterm_utf8_decoder_init(&decoder);
    static const char saved_cursor_scroll[] = "\x1b[2;1H\x1b[sabcde\x1b[uZ";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)saved_cursor_scroll,
                                                   sizeof(saved_cursor_scroll) - 1u),
                          "saved cursor should survive a bottom-row soft wrap");
    winxterm_smoke_expect(state,
                          screen.cursor_row == 0 && screen.cursor_col == 1 &&
                              winxterm_screen_cell_at(&screen, 0, 0)->codepoint == (uint32_t)'Z',
                          "saved cursor should follow terminal scrolling");
    winxterm_screen_dispose(&screen);

    winxterm_smoke_expect(state, winxterm_screen_init(&screen, 6, 3),
                          "cell-accurate extent screen should initialize");
    winxterm_utf8_decoder_init(&decoder);
    static const char styled_utf8[] = "a\x1b[31mb\x1b[0m\a\xf0\x9f\x98\x80";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)styled_utf8,
                                                   sizeof(styled_utf8) - 1u),
                          "styled multibyte text should feed");
    winxterm_smoke_expect(state,
                          screen.cursor_col == 3 &&
                              screen.primary_line_meta[0].content_columns == 3 &&
                              winxterm_screen_visual_line_advances(&screen) == 0u,
                          "UTF-8 glyphs should occupy one cell and non-rendering sequences none");
    winxterm_screen_dispose(&screen);

    winxterm_smoke_expect(state, winxterm_screen_init(&screen, 4, 3),
                          "printed-space reflow screen should initialize");
    winxterm_utf8_decoder_init(&decoder);
    static const char printed_spaces[] = "ab  ";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)printed_spaces,
                                                   sizeof(printed_spaces) - 1u),
                          "printed trailing spaces should feed");
    winxterm_screen_resize(&screen, 2, 3);
    winxterm_smoke_expect(state,
                          screen.scrollback_count >= 1u &&
                              screen.scrollback_lines[screen.scrollback_count - 1u].soft_wrapped &&
                              screen.primary_line_meta[0].content_columns == 2 &&
                              winxterm_screen_cell_at(&screen, 0, 0)->occupied &&
                              winxterm_screen_cell_at(&screen, 0, 1)->occupied,
                          "printed trailing spaces should survive cell-accurate reflow");
    winxterm_screen_dispose(&screen);

    winxterm_smoke_expect(state, winxterm_screen_init(&screen, 4, 2), "resize reflow screen should initialize");
    winxterm_utf8_decoder_init(&decoder);
    static const char wrapped[] = "abcdefghijkl";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)wrapped,
                                                   sizeof(wrapped) - 1u),
                          "wrapped text should feed before resize");
    winxterm_screen_resize(&screen, 2, 2);
    winxterm_smoke_expect(state,
                          screen.scrollback_count >= 2u &&
                              screen.scrollback_lines[0].columns == 2 &&
                              screen.scrollback_lines[0].soft_wrapped &&
                              screen.scrollback_lines[0].cells[0].codepoint == (uint32_t)'a' &&
                              screen.scrollback_lines[1].cells[0].codepoint == (uint32_t)'c',
                          "soft-wrapped scrollback should reflow to the new width");
    winxterm_screen_dispose(&screen);

    winxterm_smoke_expect(state, winxterm_screen_init(&screen, 4, 4), "visible reflow screen should initialize");
    winxterm_utf8_decoder_init(&decoder);
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)wrapped,
                                                   8u),
                          "wrapped visible text should feed before resize");
    winxterm_screen_resize(&screen, 8, 4);
    winxterm_smoke_expect(state,
                          winxterm_screen_cell_at(&screen, 0, 0)->codepoint == (uint32_t)'a' &&
                              winxterm_screen_cell_at(&screen, 0, 7)->codepoint == (uint32_t)'h' &&
                              winxterm_screen_cell_at(&screen, 1, 0)->codepoint == (uint32_t)' ',
                          "soft-wrapped visible rows should reflow to the new width");
    winxterm_smoke_expect(state,
                          screen.cursor_row == 0 && screen.cursor_col == 7,
                          "visible-row reflow should keep the cursor with the logical line");
    WinxtermUxState hit_ux;
    WinxtermUxPosition hit_position;
    winxterm_ux_init(&hit_ux);
    winxterm_smoke_expect(state,
                          winxterm_ux_hit_test(&hit_ux,
                                               &screen,
                                               2 * WINXTERM_CELL_WIDTH_PIXELS * 2 + 1,
                                               WINXTERM_CELL_HEIGHT_PIXELS * 2 + 1,
                                               2u,
                                               &hit_position) &&
                              hit_position.kind == WINXTERM_UX_ROW_PRIMARY_VISIBLE &&
                              hit_position.row == 1u &&
                              hit_position.column == 2,
                          "scale-aware hit test should map physical pixels to logical cells");
    winxterm_screen_dispose(&screen);
}

static void winxterm_smoke_test_keyboard_encoding(WinxtermSmokeState *state)
{
    uint8_t sequence[WINXTERM_INPUT_MAX_SEQUENCE];
    WinxtermInputModifiers none = {0};
    size_t length = winxterm_input_encode_char(L'A', none, sequence, sizeof(sequence));
    winxterm_smoke_expect(state, length == 1u && sequence[0] == 'A', "printable input should encode as UTF-8");

    WinxtermInputModifiers alt = {0};
    alt.alt = true;
    length = winxterm_input_encode_char(L'x', alt, sequence, sizeof(sequence));
    winxterm_smoke_expect(state,
                          length == 2u && sequence[0] == 0x1bu && sequence[1] == 'x',
                          "Alt printable input should use ESC prefix");

    WinxtermInputModifiers ctrl = {0};
    ctrl.ctrl = true;
    length = winxterm_input_encode_char(L'l', ctrl, sequence, sizeof(sequence));
    winxterm_smoke_expect(state,
                          length == 1u && sequence[0] == 0x0cu,
                          "Ctrl plus an ASCII letter should encode its control character");

    length = winxterm_input_encode_virtual_key(VK_RIGHT, ctrl, sequence, sizeof(sequence));
    winxterm_smoke_expect(state,
                          length == 6u && memcmp(sequence, "\x1b[1;5C", 6u) == 0,
                          "Ctrl+Right should encode xterm modifier sequence");

    length = winxterm_input_encode_virtual_key(VK_BACK, ctrl, sequence, sizeof(sequence));
    winxterm_smoke_expect(state,
                          length == 1u && sequence[0] == 0x17u,
                          "Ctrl+Backspace should encode word erase control");

    length = winxterm_input_encode_virtual_key(VK_F5, none, sequence, sizeof(sequence));
    winxterm_smoke_expect(state,
                          length == 5u && memcmp(sequence, "\x1b[15~", 5u) == 0,
                          "function keys should encode CSI tilde sequences");

    WinxtermModeState modes;
    winxterm_mode_state_reset_hard(&modes);
    modes.application_cursor = true;
    length = winxterm_input_encode_virtual_key_with_modes(VK_UP, none, &modes, sequence, sizeof(sequence));
    winxterm_smoke_expect(state,
                          length == 3u && memcmp(sequence, "\x1bOA", 3u) == 0,
                          "application cursor mode should encode cursor keys as SS3");

    winxterm_mode_state_reset_hard(&modes);
    modes.application_keypad = true;
    length = winxterm_input_encode_virtual_key_with_modes(VK_NUMPAD1, none, &modes, sequence, sizeof(sequence));
    winxterm_smoke_expect(state,
                          length == 3u && memcmp(sequence, "\x1bOq", 3u) == 0,
                          "application keypad mode should encode numpad keys as SS3");
}

typedef struct WinxtermSmokeMacroCaptureState {
    unsigned int screenshot_count;
    unsigned int screendump_count;
    unsigned int celldump_count;
    unsigned int render_barrier_count;
    unsigned int wait_redraw_count;
    unsigned int wait_redraw_process_count;
    unsigned int wait_redraw_ready_after;
    const wchar_t *screenshot_path;
    const wchar_t *screendump_path;
    const wchar_t *celldump_path;
} WinxtermSmokeMacroCaptureState;

static bool winxterm_smoke_macro_write_screenshot(void *context, const wchar_t *path)
{
    WinxtermSmokeMacroCaptureState *state = (WinxtermSmokeMacroCaptureState *)context;
    if (state == 0) {
        return false;
    }
    ++state->screenshot_count;
    state->screenshot_path = path;
    return true;
}

static bool winxterm_smoke_macro_write_screendump(void *context, const wchar_t *path)
{
    WinxtermSmokeMacroCaptureState *state = (WinxtermSmokeMacroCaptureState *)context;
    if (state == 0) {
        return false;
    }
    ++state->screendump_count;
    state->screendump_path = path;
    return true;
}

static bool winxterm_smoke_macro_write_celldump(void *context, const wchar_t *path)
{
    WinxtermSmokeMacroCaptureState *state = (WinxtermSmokeMacroCaptureState *)context;
    if (state == 0) return false;
    ++state->celldump_count;
    state->celldump_path = path;
    return true;
}

static bool winxterm_smoke_macro_render_barrier(void *context)
{
    WinxtermSmokeMacroCaptureState *state = (WinxtermSmokeMacroCaptureState *)context;
    if (state == 0) {
        return false;
    }
    ++state->render_barrier_count;
    return true;
}

static bool winxterm_smoke_macro_wait_redraw(void *context, bool process, bool *ready)
{
    WinxtermSmokeMacroCaptureState *state = (WinxtermSmokeMacroCaptureState *)context;
    if (state == 0 || ready == 0) {
        return false;
    }
    ++state->wait_redraw_count;
    if (process) {
        ++state->wait_redraw_process_count;
    }
    *ready = state->wait_redraw_count >= state->wait_redraw_ready_after;
    return true;
}

static void winxterm_smoke_test_macro_parser(WinxtermSmokeState *state)
{
    WinxtermMacro *macro = 0;
    winxterm_smoke_expect(state, winxterm_macro_create(&macro), "macro runtime should allocate");
    if (macro == 0) {
        return;
    }

    static const char script[] =
        "typestring echo \";\"; comment without leading space\n"
        "enterstring echo  \\; ; comment after escaped semicolon\n"
        "typestring sometext   ; comment with consumed delimiter space\n"
        "typestring hello \\\n"
        "world; continuation should remove newline\n"
        "set typedelayms 7\n"
        "keydown KEY_LEFT_ALT\n"
        "keypress KEY_ENTER 7 9\n"
        "keyup KEY_LEFT_ALT 3\n"
        "waitms 11\n"
        "waitredraw\n"
        "waitredraw -w\n"
        "waithost 123\n"
        "screenshot first.png\n"
        "screendump grid.txt\n"
        "celldump cells.txt\n"
        "histdump hist.log\n"
        "maximize\n"
        "minimize\n"
        "restore\n"
        "exit\n";
    wchar_t error[256];
    error[0] = L'\0';
    winxterm_smoke_expect(state,
                          winxterm_macro_parse_text_utf8(macro,
                                                         script,
                                                         sizeof(script) - 1u,
                                                         error,
                                                         sizeof(error) / sizeof(error[0])),
                          "macro parser should accept representative script");
    winxterm_smoke_expect(state,
                          winxterm_macro_command_count(macro) == 20u,
                          "macro parser should produce expected command count");

    const WinxtermMacroCommand *command = winxterm_macro_command_at(macro, 0u);
    winxterm_smoke_expect(state,
                          command != 0 &&
                              command->kind == WINXTERM_MACRO_COMMAND_TYPE_STRING &&
                              wcscmp(command->text, L"echo \";\"") == 0,
                          "macro parser should ignore quoted semicolon terminators");
    command = winxterm_macro_command_at(macro, 1u);
    winxterm_smoke_expect(state,
                          command != 0 &&
                              command->kind == WINXTERM_MACRO_COMMAND_ENTER_STRING &&
                              wcscmp(command->text, L"echo  \\;") == 0,
                          "macro parser should preserve escaped semicolons in payload");
    command = winxterm_macro_command_at(macro, 2u);
    winxterm_smoke_expect(state,
                          command != 0 &&
                              wcscmp(command->text, L"sometext  ") == 0,
                          "macro parser should consume only delimiter space before semicolon");
    command = winxterm_macro_command_at(macro, 3u);
    winxterm_smoke_expect(state,
                          command != 0 &&
                              wcscmp(command->text, L"hello world") == 0,
                          "macro parser should join continued lines without newline");
    command = winxterm_macro_command_at(macro, 4u);
    winxterm_smoke_expect(state,
                          command != 0 &&
                              command->kind == WINXTERM_MACRO_COMMAND_SET_TYPE_DELAY &&
                              command->first_ms == 7u,
                          "macro parser should parse typedelayms");
    command = winxterm_macro_command_at(macro, 7u);
    winxterm_smoke_expect(state,
                          command != 0 &&
                              command->kind == WINXTERM_MACRO_COMMAND_KEY_UP &&
                              command->virtual_key == VK_LMENU &&
                              command->first_ms == 3u,
                          "macro parser should parse keyup waitms");
    command = winxterm_macro_command_at(macro, 9u);
    winxterm_smoke_expect(state,
                          command != 0 &&
                              command->kind == WINXTERM_MACRO_COMMAND_WAIT_REDRAW &&
                              command->first_ms == 0u,
                          "macro parser should parse passive waitredraw");
    command = winxterm_macro_command_at(macro, 10u);
    winxterm_smoke_expect(state,
                          command != 0 &&
                              command->kind == WINXTERM_MACRO_COMMAND_WAIT_REDRAW &&
                              command->first_ms == 1u,
                          "macro parser should parse pumping waitredraw");
    command = winxterm_macro_command_at(macro, 11u);
    winxterm_smoke_expect(state,
                          command != 0 &&
                              command->kind == WINXTERM_MACRO_COMMAND_WAIT_HOST &&
                              command->first_ms == 123u,
                          "macro parser should parse waithost timeout");
    command = winxterm_macro_command_at(macro, 12u);
    winxterm_smoke_expect(state,
                          command != 0 &&
                              command->kind == WINXTERM_MACRO_COMMAND_SCREENSHOT &&
                              wcscmp(command->text, L"first.png") == 0,
                          "macro parser should preserve png screenshot filenames");

    WPARAM key = 0;
    winxterm_smoke_expect(state,
                          winxterm_macro_key_name_to_virtual_key(L"KEY_ARROW_UP", &key) &&
                              key == VK_UP,
                          "macro key map should include arrow keys");
    winxterm_smoke_expect(state,
                          winxterm_macro_key_name_to_virtual_key(L"KEY_RETURN", &key) &&
                              key == VK_RETURN,
                          "macro key map should include return aliases");
    wchar_t not_found[WINXTERM_LOG_PATH_CAPACITY + 64u];
    winxterm_smoke_expect(state,
                          winxterm_macro_format_not_found_message(L"missing-macro-file.macro",
                                                                  not_found,
                                                                  sizeof(not_found) / sizeof(not_found[0])) &&
                              wcsstr(not_found, L"\"Macro: ") == not_found &&
                              wcsstr(not_found, L"missing-macro-file.macro") != 0 &&
                              wcsstr(not_found, L"\" was not found.") != 0,
                          "macro not-found diagnostic should include canonical macro filename");
    static const char bad_script[] = "dumpscreen C:\\Users\\example\\first.txt\n";
    wchar_t parse_error[256];
    parse_error[0] = L'\0';
    winxterm_smoke_expect(state,
                          !winxterm_macro_parse_text_utf8(macro,
                                                          bad_script,
                                                          sizeof(bad_script) - 1u,
                                                          parse_error,
                                                          sizeof(parse_error) / sizeof(parse_error[0])) &&
                              wcscmp(parse_error,
                                     L"Macro error on line 1: dumpscreen C:\\Users\\example\\first.txt") == 0,
                          "macro parser errors should report the full unparsed line");

    winxterm_macro_destroy(macro);
}

static void winxterm_smoke_test_macro_capture_commands(WinxtermSmokeState *state)
{
    WinxtermMacro *macro = 0;
    winxterm_smoke_expect(state, winxterm_macro_create(&macro), "macro capture runtime should allocate");
    if (macro == 0) {
        return;
    }

    static const char script[] =
        "waitredraw\n"
        "waitredraw -w\n"
        "screenshot first.bmp\n"
        "screendump first.txt\n"
        "celldump cells.txt\n";
    wchar_t error[256];
    error[0] = L'\0';
    winxterm_smoke_expect(state,
                          winxterm_macro_parse_text_utf8(macro,
                                                         script,
                                                         sizeof(script) - 1u,
                                                         error,
                                                         sizeof(error) / sizeof(error[0])),
                          "macro capture commands should parse");

    WinxtermSmokeMacroCaptureState capture;
    memset(&capture, 0, sizeof(capture));
    capture.wait_redraw_ready_after = 2u;
    WinxtermMacroCallbacks callbacks;
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.context = &capture;
    callbacks.write_screenshot = winxterm_smoke_macro_write_screenshot;
    callbacks.write_screendump = winxterm_smoke_macro_write_screendump;
    callbacks.write_celldump = winxterm_smoke_macro_write_celldump;
    callbacks.wait_redraw = winxterm_smoke_macro_wait_redraw;
    callbacks.render_barrier = winxterm_smoke_macro_render_barrier;

    DWORD delay = winxterm_macro_step(macro, &callbacks);
    winxterm_smoke_expect(state,
                          delay != WINXTERM_MACRO_DONE_DELAY &&
                              capture.wait_redraw_count == 1u &&
                              capture.wait_redraw_process_count == 0u &&
                              capture.screenshot_count == 0u &&
                              capture.screendump_count == 0u &&
                              capture.celldump_count == 0u,
                          "macro waitredraw should pause playback while unsettled");

    delay = winxterm_macro_step(macro, &callbacks);
    winxterm_smoke_expect(state,
                          delay == WINXTERM_MACRO_DONE_DELAY &&
                              capture.wait_redraw_count == 3u &&
                              capture.wait_redraw_process_count == 1u &&
                              capture.screenshot_count == 1u &&
                              capture.screendump_count == 1u &&
                              capture.celldump_count == 1u &&
                              capture.render_barrier_count == 0u &&
                              capture.screenshot_path != 0 &&
                              wcscmp(capture.screenshot_path, L"first.bmp") == 0 &&
                              capture.screendump_path != 0 &&
                              wcscmp(capture.screendump_path, L"first.txt") == 0 &&
                              capture.celldump_path != 0 &&
                              wcscmp(capture.celldump_path, L"cells.txt") == 0,
                          "macro capture commands should not force render barriers");

    winxterm_macro_destroy(macro);
}

static void winxterm_smoke_test_daily_ux_helpers(WinxtermSmokeState *state)
{
    WinxtermScreen screen;
    WinxtermUtf8Decoder decoder;
    winxterm_smoke_expect(state, winxterm_screen_init(&screen, 4, 2), "UX screen should initialize");
    winxterm_utf8_decoder_init(&decoder);
    static const char history_text[] = "abcd\nefgh\nijkl";
    winxterm_smoke_expect(state,
                          winxterm_text_feed_bytes(&screen,
                                                   &decoder,
                                                   (const uint8_t *)history_text,
                                                   sizeof(history_text) - 1u),
                          "UX history text should feed");

    WinxtermUxState ux;
    winxterm_ux_init(&ux);
    winxterm_ux_note_screen_changed(&ux, &screen);
    winxterm_ux_scroll_to_top(&ux, &screen);
    winxterm_smoke_expect(state,
                          ux.viewport.line_offset_from_bottom == winxterm_screen_scrollback_count(&screen),
                          "viewport should scroll to oldest available history");
    size_t first_row = winxterm_ux_primary_first_row(&ux, &screen);
    WinxtermScreenRowView row;
    winxterm_smoke_expect(state,
                          winxterm_screen_get_primary_view_row(&screen, first_row, &row) &&
                              row.cells[0].codepoint == (uint32_t)'a',
                          "viewport first row should resolve into scrollback");
    winxterm_ux_scroll_to_bottom(&ux);
    winxterm_smoke_expect(state,
                          winxterm_ux_primary_first_row(&ux, &screen) ==
                              winxterm_screen_default_primary_first_row(&screen, 0u),
                          "viewport bottom should use live screen rows");
    winxterm_ux_scroll_to_offset_for_rows(&ux, &screen, screen.rows, 1u);
    winxterm_smoke_expect(state,
                          ux.viewport.line_offset_from_bottom == 1u &&
                              !ux.viewport.follow_output,
                          "viewport absolute offset should stop following output");
    winxterm_ux_scroll_to_offset_for_rows(&ux, &screen, screen.rows, 1000u);
    winxterm_smoke_expect(state,
                          ux.viewport.line_offset_from_bottom ==
                              winxterm_screen_scrollback_count(&screen),
                          "viewport absolute offset should clamp to available history");
    winxterm_ux_scroll_to_offset_for_rows(&ux, &screen, screen.rows, 0u);
    winxterm_smoke_expect(state,
                          ux.viewport.line_offset_from_bottom == 0u &&
                              ux.viewport.follow_output,
                          "viewport zero offset should resume following output");

    WinxtermUxPosition start = {WINXTERM_UX_ROW_PRIMARY_VISIBLE, 0u, 0};
    WinxtermUxPosition end = {WINXTERM_UX_ROW_PRIMARY_VISIBLE, 0u, 1};
    winxterm_ux_begin_selection(&ux, start);
    winxterm_ux_update_selection(&ux, end);
    winxterm_ux_finish_selection(&ux);
    char *selection = 0;
    size_t selection_length = 0u;
    winxterm_smoke_expect(state,
                          winxterm_ux_extract_selection_utf8(&ux, &screen, &selection, &selection_length) &&
                              selection_length == 2u,
                          "selection extraction should produce UTF-8 bytes");
    free(selection);
    winxterm_screen_dispose(&screen);

    winxterm_smoke_expect(state,
                          winxterm_screen_init(&screen, 8, 3),
                          "mouse UX helper screen should initialize");
    if (screen.primary_cells != 0 && screen.primary_line_meta != 0) {
        winxterm_smoke_fill_text_cells(&screen.primary_cells[0], screen.columns, "ab cd");
        winxterm_smoke_fill_text_cells(&screen.primary_cells[(size_t)screen.columns],
                                       screen.columns,
                                       "ef g");
        winxterm_smoke_fill_text_cells(&screen.primary_cells[(size_t)screen.columns * 2u],
                                       screen.columns,
                                       "hijk");
        screen.primary_line_meta[0].soft_wrapped = true;
        screen.primary_line_meta[1].soft_wrapped = false;
        screen.primary_line_meta[2].soft_wrapped = false;

        winxterm_ux_init(&ux);
        WinxtermUxPosition word_position = {WINXTERM_UX_ROW_PRIMARY_VISIBLE, 0u, 4};
        selection = 0;
        selection_length = 0u;
        winxterm_smoke_expect(state,
                              winxterm_ux_select_non_space_run_at(&ux, &screen, word_position) &&
                                  winxterm_ux_extract_selection_utf8(&ux, &screen, &selection, &selection_length) &&
                                  selection_length == 2u &&
                                  memcmp(selection, "cd", 2u) == 0,
                              "double-click selection should copy a non-space run");
        free(selection);

        winxterm_ux_clear_selection(&ux);
        WinxtermUxPosition space_position = {WINXTERM_UX_ROW_PRIMARY_VISIBLE, 0u, 2};
        winxterm_smoke_expect(state,
                              !winxterm_ux_select_non_space_run_at(&ux, &screen, space_position) &&
                                  !winxterm_ux_has_selection(&ux),
                              "double-click selection should ignore spaces");

        winxterm_smoke_fill_text_cells(&screen.primary_cells[0], screen.columns, "abcdefgh");
        winxterm_smoke_fill_text_cells(&screen.primary_cells[(size_t)screen.columns],
                                       screen.columns,
                                       "ij kl");
        screen.primary_line_meta[0].soft_wrapped = true;
        screen.primary_line_meta[1].soft_wrapped = false;
        WinxtermUxPosition wrapped_word_position = {WINXTERM_UX_ROW_PRIMARY_VISIBLE, 1u, 1};
        selection = 0;
        selection_length = 0u;
        winxterm_smoke_expect(state,
                              winxterm_ux_select_non_space_run_at(&ux, &screen, wrapped_word_position) &&
                                  winxterm_ux_extract_selection_utf8(&ux, &screen, &selection, &selection_length) &&
                                  selection_length == 10u &&
                                  memcmp(selection, "abcdefghij", 10u) == 0,
                              "double-click selection should follow soft-wrapped real lines");
        free(selection);

        selection = 0;
        selection_length = 0u;
        winxterm_smoke_expect(state,
                              winxterm_ux_select_real_line_at(&ux, &screen, wrapped_word_position) &&
                                  winxterm_ux_extract_selection_utf8(&ux, &screen, &selection, &selection_length) &&
                                  selection_length == 13u &&
                                  memcmp(selection, "abcdefghij kl", 13u) == 0,
                              "triple-click selection should copy the full real line without newline");
        free(selection);

        winxterm_smoke_fill_text_cells(&screen.primary_cells[0], screen.columns, "a cd");
        winxterm_smoke_fill_text_cells(&screen.primary_cells[(size_t)screen.columns],
                                       screen.columns,
                                       "e gh");
        winxterm_smoke_fill_text_cells(&screen.primary_cells[(size_t)screen.columns * 2u],
                                       screen.columns,
                                       "i kl");
        screen.primary_line_meta[0].soft_wrapped = false;
        screen.primary_line_meta[1].soft_wrapped = false;
        screen.primary_line_meta[2].soft_wrapped = false;
        WinxtermUxPosition box_start = {WINXTERM_UX_ROW_PRIMARY_VISIBLE, 0u, 1};
        WinxtermUxPosition box_end = {WINXTERM_UX_ROW_PRIMARY_VISIBLE, 2u, 3};
        winxterm_ux_begin_selection_mode(&ux, box_start, WINXTERM_SELECTION_RECTANGULAR);
        winxterm_ux_update_selection(&ux, box_end);
        winxterm_ux_finish_selection(&ux);
        WinxtermScreenSelectionRange box_range = winxterm_ux_render_selection(&ux, &screen);
        selection = 0;
        selection_length = 0u;
        winxterm_smoke_expect(state,
                              box_range.enabled && box_range.rectangular &&
                                  box_range.start_column == 1 && box_range.end_column == 3 &&
                                  winxterm_ux_extract_selection_utf8(&ux, &screen, &selection, &selection_length) &&
                                  selection_length == 9u &&
                                  memcmp(selection, "cd gh kl ", 9u) == 0,
                              "rectangular selection should trim rows and join them with spaces");
        free(selection);
        selection = 0;
        selection_length = 0u;
        winxterm_smoke_expect(state,
                              winxterm_ux_extract_selection_utf8_format(
                                  &ux,
                                  &screen,
                                  WINXTERM_SELECTION_COPY_RECTANGULAR_PRESERVE_ROWS,
                                  &selection,
                                  &selection_length) &&
                                  selection_length == 11u &&
                                  memcmp(selection, " cd\n gh\n kl", 11u) == 0,
                              "legacy rectangular selection should preserve selected spaces and row breaks");
        free(selection);
    }
    winxterm_screen_dispose(&screen);

    winxterm_smoke_expect(state,
                          winxterm_screen_init(&screen, 4, 4),
                          "partial viewport screen should initialize");
    if (screen.primary_cells != 0 && screen.primary_line_meta != 0) {
        screen.scrollback_count = 2u;
        screen.scrollback_capacity = 2u;
        screen.scrollback_lines = (WinxtermScreenLine *)calloc(screen.scrollback_capacity,
                                                              sizeof(*screen.scrollback_lines));
        bool seeded = screen.scrollback_lines != 0 &&
            winxterm_smoke_set_scrollback_line(&screen, 0u, "1111", false) &&
            winxterm_smoke_set_scrollback_line(&screen, 1u, "2222", false);
        winxterm_smoke_fill_text_cells(&screen.primary_cells[0], screen.columns, "aaaa");
        winxterm_smoke_fill_text_cells(&screen.primary_cells[(size_t)screen.columns],
                                       screen.columns,
                                       "bbbb");
        winxterm_smoke_fill_text_cells(&screen.primary_cells[(size_t)screen.columns * 2u],
                                       screen.columns,
                                       "cccc");
        winxterm_smoke_fill_text_cells(&screen.primary_cells[(size_t)screen.columns * 3u],
                                       screen.columns,
                                       "dddd");
        winxterm_smoke_expect(state, seeded, "partial viewport history should seed");

        WinxtermScreenRenderState snapshot;
        winxterm_smoke_expect(state,
                              winxterm_screen_render_state_init(
                                  &snapshot, &screen,
                                  screen.columns * WINXTERM_CELL_WIDTH_PIXELS,
                                  2 * WINXTERM_CELL_HEIGHT_PIXELS, false, 0) &&
                                  snapshot.rows == 2 && snapshot.first_row == 4u,
                              "partial bottom render state should select bottom live rows");

        winxterm_ux_init(&ux);
        winxterm_ux_note_screen_changed_for_rows(&ux, &screen, 2);
        winxterm_smoke_expect(state,
                              winxterm_ux_primary_first_row_for_rows(&ux, &screen, 2) == 4u,
                              "partial viewport bottom should start at bottom visible rows");
        winxterm_ux_scroll_lines_for_rows(&ux, &screen, 2, 1);
        winxterm_smoke_expect(state,
                              winxterm_ux_primary_first_row_for_rows(&ux, &screen, 2) == 3u,
                              "partial viewport one-line scroll should move one row up");
        winxterm_ux_scroll_to_top_for_rows(&ux, &screen, 2);
        winxterm_smoke_expect(state,
                              ux.viewport.line_offset_from_bottom == 4u &&
                                  winxterm_ux_primary_first_row_for_rows(&ux, &screen, 2) == 0u,
                              "partial viewport top should use total rows minus visible rows");
        winxterm_ux_scroll_to_bottom(&ux);
        WinxtermUxPosition partial_hit;
        winxterm_smoke_expect(state,
                              winxterm_ux_hit_test_cells(&ux,
                                                         &screen,
                                                         WINXTERM_CELL_WIDTH_PIXELS + 1,
                                                         WINXTERM_CELL_HEIGHT_PIXELS + 1,
                                                         1u,
                                                         4,
                                                         2,
                                                         &partial_hit) &&
                                  partial_hit.kind == WINXTERM_UX_ROW_PRIMARY_VISIBLE &&
                                  partial_hit.row == 3u &&
                                  partial_hit.column == 1,
                              "partial viewport hit test should map to rendered bottom rows");
        winxterm_smoke_expect(state,
                              !winxterm_ux_hit_test_cells(&ux,
                                                          &screen,
                                                          0,
                                                          2 * WINXTERM_CELL_HEIGHT_PIXELS,
                                                          1u,
                                                          4,
                                                          2,
                                                          &partial_hit),
                              "partial viewport hit test should reject clipped rows");
    }
    winxterm_screen_dispose(&screen);

    winxterm_smoke_expect(state,
                          winxterm_screen_init(&screen, 6, 2),
                          "bottom-anchor resize screen should initialize");
    if (screen.primary_cells != 0 && screen.primary_line_meta != 0) {
        screen.scrollback_count = 2u;
        screen.scrollback_capacity = 2u;
        screen.scrollback_lines = (WinxtermScreenLine *)calloc(screen.scrollback_capacity,
                                                              sizeof(*screen.scrollback_lines));
        bool seeded = screen.scrollback_lines != 0 &&
            winxterm_smoke_set_scrollback_line(&screen, 0u, "abcdef", true) &&
            winxterm_smoke_set_scrollback_line(&screen, 1u, "ghijkl", false);
        winxterm_smoke_fill_text_cells(&screen.primary_cells[0], screen.columns, "mnopqr");
        winxterm_smoke_fill_text_cells(&screen.primary_cells[(size_t)screen.columns],
                                       screen.columns,
                                       "stuvwx");
        screen.primary_line_meta[0].soft_wrapped = false;
        screen.primary_line_meta[1].soft_wrapped = false;
        winxterm_smoke_expect(state, seeded, "bottom-anchor resize history should seed");

        winxterm_ux_init(&ux);
        winxterm_ux_note_screen_changed(&ux, &screen);
        winxterm_ux_scroll_to_top(&ux, &screen);
        WinxtermScreenPrimaryAnchor anchor = ux.viewport.bottom_anchor;
        winxterm_smoke_expect(state,
                              ux.viewport.bottom_anchor_valid,
                              "scrolled viewport should capture a bottom anchor");

        winxterm_ux_capture_resize_anchor(&ux, &screen);
        winxterm_screen_resize(&screen, 3, 2);
        winxterm_ux_restore_resize_anchor(&ux, &screen);

        size_t anchor_row = 0u;
        size_t first_anchor_row = winxterm_ux_primary_first_row(&ux, &screen);
        WinxtermScreenRowView top_row;
        WinxtermScreenRowView bottom_row;
        bool anchor_resolved =
            winxterm_screen_primary_global_row_from_anchor(&screen, &anchor, &anchor_row);
        winxterm_smoke_expect(state,
                              anchor_resolved &&
                                  first_anchor_row + (size_t)(screen.rows - 1) == anchor_row,
                              "narrow resize should keep the anchor on the last visible row");
        winxterm_smoke_expect(state,
                              winxterm_screen_get_primary_view_row(&screen,
                                                                   first_anchor_row,
                                                                   &top_row) &&
                                  winxterm_screen_get_primary_view_row(&screen,
                                                                       first_anchor_row + 1u,
                                                                       &bottom_row) &&
                                  top_row.cells[0].codepoint == (uint32_t)'g' &&
                                  bottom_row.cells[0].codepoint == (uint32_t)'j',
                              "narrow resize should push wrapped text above the anchored row");

        winxterm_ux_scroll_to_top_for_rows(&ux, &screen, 1);
        anchor = ux.viewport.bottom_anchor;
        winxterm_smoke_expect(state,
                              ux.viewport.bottom_anchor_valid,
                              "partial scrolled viewport should capture a one-row bottom anchor");
        winxterm_ux_capture_resize_anchor_for_rows(&ux, &screen, 1);
        winxterm_screen_resize(&screen, 4, 2);
        winxterm_ux_restore_resize_anchor_for_rows(&ux, &screen, 1);
        anchor_resolved =
            winxterm_screen_primary_global_row_from_anchor(&screen, &anchor, &anchor_row);
        first_anchor_row = winxterm_ux_primary_first_row_for_rows(&ux, &screen, 1);
        winxterm_smoke_expect(state,
                              anchor_resolved && first_anchor_row == anchor_row,
                              "partial resize should keep the anchor on the only visible row");
    }
    winxterm_screen_dispose(&screen);

    char *paste = 0;
    size_t paste_length = 0u;
    static const char paste_input[] = "a\r\nb\rc";
    winxterm_smoke_expect(state,
                          winxterm_clipboard_prepare_paste(paste_input,
                                                           sizeof(paste_input) - 1u,
                                                           true,
                                                           &paste,
                                                           &paste_length) &&
                              paste_length == 17u &&
                              memcmp(paste, "\x1b[200~a\nb\nc\x1b[201~", 17u) == 0,
                          "bracketed paste helper should normalize line endings and wrap");
    free(paste);
    paste = 0;
    paste_length = 0u;

    static const char prompt_paste_input[] = "echo 1\r\necho 2\r\necho 3";
    winxterm_smoke_expect(state,
                          winxterm_clipboard_prepare_paste(prompt_paste_input,
                                                           sizeof(prompt_paste_input) - 1u,
                                                           false,
                                                           &paste,
                                                           &paste_length) &&
                              paste_length == 20u &&
                              memcmp(paste, "echo 1\necho 2\necho 3", 20u) == 0,
                          "paste helper should preserve unterminated final input line");
    free(paste);

    WinxtermModeState modes;
    winxterm_mode_state_reset_hard(&modes);
    modes.mouse_normal = true;
    modes.mouse_sgr = true;
    WinxtermMouseEvent event;
    memset(&event, 0, sizeof(event));
    event.kind = WINXTERM_MOUSE_EVENT_PRESS;
    event.button = WINXTERM_MOUSE_BUTTON_LEFT;
    event.column = 2;
    event.row = 3;
    event.modifiers.ctrl = true;
    uint8_t sequence[WINXTERM_INPUT_MAX_SEQUENCE];
    size_t length = winxterm_mouse_encode_event(&modes, &event, sequence, sizeof(sequence));
    winxterm_smoke_expect(state,
                          length == 10u && memcmp(sequence, "\x1b[<16;3;4M", 10u) == 0,
                          "SGR mouse helper should encode 1-based coordinates and modifiers");

    winxterm_ux_init(&ux);
    winxterm_ux_start_bell(&ux, 1000u);
    winxterm_smoke_expect(state,
                          winxterm_ux_bell_active(&ux, 1000u) &&
                              winxterm_ux_bell_title_prefix(&ux, 1250u)[0] == L' ',
                          "bell helper should blink title prefix on 250ms phases");
    winxterm_smoke_expect(state,
                          !winxterm_ux_bell_active(&ux, 3000u),
                          "bell helper should stop after two seconds");
}

static void winxterm_smoke_test_renderers(WinxtermSmokeState *state)
{
    const int columns = 12;
    const int rows = 4;
    const int width = columns * WINXTERM_CELL_WIDTH_PIXELS;
    const int height = rows * WINXTERM_CELL_HEIGHT_PIXELS;
    size_t pixel_count = (size_t)width * (size_t)height;
    uint32_t *incremental = (uint32_t *)malloc(pixel_count * sizeof(*incremental));
    uint32_t *reference = (uint32_t *)malloc(pixel_count * sizeof(*reference));
    WinxtermScreen screen;
    WinxtermRenderContext context;
    memset(&screen, 0, sizeof(screen));
    winxterm_render_context_init(&context);
    (void)winxterm_render_context_load_fallback_fonts(&context, GetModuleHandleW(0), 0);

    bool ready = incremental != 0 && reference != 0 &&
        winxterm_screen_init(&screen, columns, rows);
    winxterm_smoke_expect(state, ready, "incremental renderer test should initialize");
    if (ready) {
        static const char initial[] = "alpha\nbeta\ngamma";
        for (size_t i = 0u; i < sizeof(initial) - 1u; ++i) {
            (void)winxterm_screen_append_codepoint(&screen, (uint8_t)initial[i]);
        }
        WinxtermScreenRenderState render_state;
        winxterm_smoke_expect(state,
                              winxterm_screen_render_state_init(&render_state, &screen,
                                                               width, height, true, 0),
                              "direct render state should initialize");
        winxterm_screen_render_state_rows(&render_state, &screen, &context,
                                          incremental, 0, rows);
        winxterm_render_damage_clear(&screen.damage);

        int old_cursor_row = screen.cursor_row;
        screen.cursor_row = 1;
        screen.cursor_col = 2;
        winxterm_render_damage_mark_row(&screen.damage, old_cursor_row);
        winxterm_render_damage_mark_row(&screen.damage, screen.cursor_row);
        (void)winxterm_screen_append_codepoint(&screen, (uint32_t)'Z');
        (void)winxterm_screen_render_state_init(&render_state, &screen,
                                                width, height, true, 0);
        memcpy(reference, incremental, pixel_count * sizeof(*reference));
        for (int row = 0; row < rows;) {
            while (row < rows && !winxterm_render_damage_row(&screen.damage, row)) ++row;
            int first = row;
            while (row < rows && winxterm_render_damage_row(&screen.damage, row)) ++row;
            if (first < row) {
                winxterm_screen_render_state_rows(&render_state, &screen, &context,
                                                  incremental, first, row - first);
            }
        }
        winxterm_screen_render_state_rows(&render_state, &screen, &context,
                                          reference, 0, rows);
        winxterm_smoke_expect(state,
                              memcmp(incremental, reference,
                                     pixel_count * sizeof(*incremental)) == 0,
                              "incremental rows should match a forced full redraw");

        uint32_t random = 0x5eed1234u;
        bool randomized_match = true;
        winxterm_render_damage_clear(&screen.damage);
        for (int step = 0; step < 128 && randomized_match; ++step) {
            int previous_cursor_row = screen.cursor_row;
            random = random * 1664525u + 1013904223u;
            screen.cursor_row = (int)(random % (uint32_t)rows);
            random = random * 1664525u + 1013904223u;
            screen.cursor_col = (int)(random % (uint32_t)(columns - 1));
            screen.pending_wrap = false;
            winxterm_render_damage_mark_row(&screen.damage, previous_cursor_row);
            winxterm_render_damage_mark_row(&screen.damage, screen.cursor_row);
            if ((step % 3) == 0) {
                winxterm_screen_set_foreground_index(&screen, step % 16);
            }
            (void)winxterm_screen_append_codepoint(
                &screen, (uint32_t)('!' + (step % 90)));
            (void)winxterm_screen_render_state_init(&render_state, &screen,
                                                    width, height, true, 0);
            for (int row = 0; row < rows;) {
                while (row < rows &&
                       !winxterm_render_damage_row(&screen.damage, row)) ++row;
                int first = row;
                while (row < rows &&
                       winxterm_render_damage_row(&screen.damage, row)) ++row;
                if (first < row) {
                    winxterm_screen_render_state_rows(&render_state, &screen, &context,
                                                      incremental, first, row - first);
                }
            }
            winxterm_screen_render_state_rows(&render_state, &screen, &context,
                                              reference, 0, rows);
            randomized_match = memcmp(incremental, reference,
                                      pixel_count * sizeof(*incremental)) == 0;
            winxterm_render_damage_clear(&screen.damage);
        }
        winxterm_smoke_expect(state, randomized_match,
                              "randomized incremental updates should match forced full redraws");

        winxterm_render_damage_mark_row(&screen.damage, 0);
        winxterm_render_damage_mark_row(&screen.damage, 3);
        winxterm_smoke_expect(state,
                              winxterm_render_damage_row(&screen.damage, 0) &&
                                  !winxterm_render_damage_row(&screen.damage, 1) &&
                                  winxterm_render_damage_row(&screen.damage, 3),
                              "row damage should preserve disjoint updates");
        winxterm_render_damage_record_scroll(&screen.damage, 1);
        winxterm_smoke_expect(state,
                              screen.damage.scroll_valid &&
                                  screen.damage.scroll_rows == 1 &&
                                  winxterm_render_damage_row(&screen.damage, rows - 1),
                              "scroll damage should mark the exposed row");
    }

    winxterm_screen_dispose(&screen);
    winxterm_render_context_dispose(&context);
    free(incremental);
    free(reference);
}

static void winxterm_smoke_test_frame_scheduler(WinxtermSmokeState *state)
{
    WinxtermFrameScheduler scheduler;
    winxterm_frame_scheduler_init(&scheduler);
    uint64_t now = 1000000000ull;
    winxterm_frame_scheduler_note_visible_update(&scheduler, now);
    winxterm_smoke_expect(state,
                          scheduler.deadline_armed &&
                              scheduler.mode == WINXTERM_FRAME_PACING_DEBOUNCED &&
                              scheduler.deadline_ns == now + WINXTERM_FRAME_INTERVAL_NS,
                          "a first visible update should arm a trailing deadline");

    now += 4000000ull;
    winxterm_frame_scheduler_note_visible_update(&scheduler, now);
    winxterm_smoke_expect(state,
                          scheduler.deadline_ns == now + WINXTERM_FRAME_INTERVAL_NS &&
                              !winxterm_frame_scheduler_due(&scheduler, now + 15000000ull),
                          "low-rate visible updates should rearm the trailing deadline");

    for (unsigned int i = 2u; i < WINXTERM_VISIBLE_RATE_SAMPLE_CAPACITY; ++i) {
        now += 1000000ull;
        winxterm_frame_scheduler_note_visible_update(&scheduler, now);
    }
    winxterm_smoke_expect(state,
                          scheduler.mode == WINXTERM_FRAME_PACING_SUSTAINED &&
                              scheduler.deadline_armed,
                          "the seventeenth visible update in one second should enter sustained mode");
    uint64_t sustained_deadline = scheduler.deadline_ns;
    now += 1000000ull;
    winxterm_frame_scheduler_note_visible_update(&scheduler, now);
    winxterm_smoke_expect(state,
                          scheduler.deadline_ns == sustained_deadline,
                          "sustained updates should not postpone an armed cadence deadline");

    uint64_t sampled_updates = scheduler.visible_updates;
    winxterm_frame_scheduler_schedule(&scheduler, now + 1000000ull);
    winxterm_smoke_expect(state,
                          scheduler.visible_updates == sampled_updates &&
                              scheduler.deadline_ns == sustained_deadline,
                          "local visual scheduling should not affect the parsed-update rate");

    now = 2005000001ull;
    winxterm_frame_scheduler_note_visible_update(&scheduler, now);
    winxterm_smoke_expect(state,
                          scheduler.mode == WINXTERM_FRAME_PACING_SUSTAINED &&
                              winxterm_frame_scheduler_recent_visible_updates(&scheduler, now) == 16u,
                          "exactly sixteen visible updates should retain sustained mode");

    now = 3000000000ull;
    winxterm_frame_scheduler_note_visible_update(&scheduler, now);
    winxterm_smoke_expect(state,
                          scheduler.mode == WINXTERM_FRAME_PACING_DEBOUNCED &&
                              scheduler.deadline_ns == now + WINXTERM_FRAME_INTERVAL_NS,
                          "an expired visible-update window should return to debounce mode");
    winxterm_frame_scheduler_note_presented(&scheduler, now + WINXTERM_FRAME_INTERVAL_NS);
    winxterm_smoke_expect(state,
                          !scheduler.deadline_armed &&
                              scheduler.last_present_ns == now + WINXTERM_FRAME_INTERVAL_NS,
                          "presentation should consume the active deadline");
}

static void winxterm_smoke_test_surface(WinxtermSmokeState *state)
{
    WinxtermSurface surface;
    memset(&surface, 0, sizeof(surface));
    bool initialized = winxterm_surface_init(&surface, 0, 12, 13);
    winxterm_smoke_expect(state,
                          initialized && surface.memory_dc != 0 && surface.dib != 0 &&
                              surface.pixels != 0 && surface.width == 12 &&
                              surface.height == 13 && surface.stride_pixels == 12,
                          "persistent DIB surface should initialize");
    if (initialized) {
        uint32_t *old_pixels = surface.pixels;
        winxterm_smoke_expect(state,
                              !winxterm_surface_resize(&surface, 0, 0, 26) &&
                                  surface.pixels == old_pixels && surface.width == 12 &&
                                  surface.height == 13,
                              "invalid surface replacement should preserve the live DIB");
        winxterm_smoke_expect(state,
                              winxterm_surface_resize(&surface, 0, 18, 26) &&
                                  surface.pixels != 0 && surface.width == 18 &&
                                  surface.height == 26 && surface.stride_pixels == 18,
                              "valid surface replacement should commit transactionally");
    }
    winxterm_surface_dispose(&surface);
    winxterm_smoke_expect(state,
                          surface.memory_dc == 0 && surface.dib == 0 && surface.pixels == 0,
                          "surface disposal should release every GDI resource");
}

static void winxterm_smoke_test_pty_backend_environment(WinxtermSmokeState *state)
{
    wchar_t saved[16];
    SetLastError(ERROR_SUCCESS);
    DWORD saved_length = GetEnvironmentVariableW(WINXTERM_PTY_SHIM_ENV, saved,
                                                  (DWORD)(sizeof(saved) / sizeof(saved[0])));
    bool had_saved = saved_length != 0u || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
    WinxtermPtyBackend backend = WINXTERM_PTY_BACKEND_SHIM;
    wchar_t error[160];

    (void)SetEnvironmentVariableW(WINXTERM_PTY_SHIM_ENV, 0);
    winxterm_smoke_expect(state,
                          winxterm_pty_backend_from_environment(&backend, error,
                              sizeof(error) / sizeof(error[0])) &&
                              backend == WINXTERM_PTY_BACKEND_NATIVE,
                          "unset PTY shim environment should select native ConPTY");
    (void)SetEnvironmentVariableW(WINXTERM_PTY_SHIM_ENV, L"0");
    winxterm_smoke_expect(state,
                          winxterm_pty_backend_from_environment(&backend, error,
                              sizeof(error) / sizeof(error[0])) &&
                              backend == WINXTERM_PTY_BACKEND_NATIVE,
                          "PTY shim environment value 0 should select native ConPTY");
    (void)SetEnvironmentVariableW(WINXTERM_PTY_SHIM_ENV, L"1");
    winxterm_smoke_expect(state,
                          winxterm_pty_backend_from_environment(&backend, error,
                              sizeof(error) / sizeof(error[0])) &&
                              backend == WINXTERM_PTY_BACKEND_SHIM,
                          "PTY shim environment value 1 should select the stdio shim");
    (void)SetEnvironmentVariableW(WINXTERM_PTY_SHIM_ENV, L"stdio");
    winxterm_smoke_expect(state,
                          !winxterm_pty_backend_from_environment(&backend, error,
                              sizeof(error) / sizeof(error[0])) &&
                              wcsstr(error, WINXTERM_PTY_SHIM_ENV) != 0,
                          "invalid PTY shim environment should be rejected with its name");

    if (had_saved && saved_length < sizeof(saved) / sizeof(saved[0])) {
        (void)SetEnvironmentVariableW(WINXTERM_PTY_SHIM_ENV, saved);
    } else {
        (void)SetEnvironmentVariableW(WINXTERM_PTY_SHIM_ENV, 0);
    }
}

int winxterm_smoke_run(void)
{
    WinxtermSmokeState state = {0};

    winxterm_smoke_test_render_constants(&state);
    winxterm_smoke_test_font_contract(&state);
    winxterm_smoke_test_log_names(&state);
    winxterm_smoke_test_window_placement(&state);
    winxterm_smoke_test_settings(&state);
    winxterm_smoke_test_grouped_u64_format(&state);
    winxterm_smoke_test_options(&state);
    winxterm_smoke_test_pty_backend_environment(&state);
    winxterm_smoke_test_job_control_contracts(&state);
    winxterm_smoke_test_bridge_hardening(&state);
    winxterm_smoke_test_screen_and_text(&state);
    winxterm_smoke_test_parser_strings(&state);
    winxterm_smoke_test_mode_registry(&state);
    winxterm_smoke_test_query_replies(&state);
    winxterm_smoke_test_osc_policy(&state);
    winxterm_smoke_test_reset_sequences(&state);
    winxterm_smoke_test_screen_layout_extensions(&state);
    winxterm_smoke_test_keyboard_encoding(&state);
    winxterm_smoke_test_macro_parser(&state);
    winxterm_smoke_test_macro_capture_commands(&state);
    winxterm_smoke_test_daily_ux_helpers(&state);
    winxterm_smoke_test_renderers(&state);
    winxterm_smoke_test_frame_scheduler(&state);
    winxterm_smoke_test_surface(&state);

    return state.failures == 0 ? 0 : 1;
}
