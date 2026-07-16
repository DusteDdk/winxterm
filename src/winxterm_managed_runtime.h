#ifndef WINXTERM_MANAGED_RUNTIME_H
#define WINXTERM_MANAGED_RUNTIME_H

#include "winxterm_job_coordinator.h"
#include "winxterm_pty.h"
#include "winxterm_terminal_session.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <windows.h>

typedef struct WinxtermHostContext WinxtermHostContext;

typedef struct WinxtermHostManagedChild {
    WinxtermHostContext *host;
    struct WinxtermHostManagedChild *next;
    uint64_t id;
    WinxtermPty pty;
    PROCESS_INFORMATION process;
    PROCESS_INFORMATION *stage_processes;
    size_t stage_count;
    HANDLE process_job;
    HANDLE input_write;
    HANDLE output_read;
    HANDLE redirected_output_read;
    HANDLE redirected_file;
    HANDLE attachment_file;
    HANDLE thread;
    HANDLE kill_thread;
    HANDLE kill_cancel_event;
    uint64_t pending_foreground_request_id;
    WinxtermJobCoordinatorClient *pending_foreground_client;
    uint64_t pending_kill_request_id;
    WinxtermJobCoordinatorClient *pending_kill_client;
    WinxtermJobCoordinatorClient *client;
    bool client_linked;
    bool pty_created;
    WinxtermTerminalSession session;
    uint64_t destination_id;
    HANDLE connection_thread;
    HANDLE connection_input;
    uint64_t redirected_bytes;
    uint64_t attachment_bytes;
    ULONGLONG redirect_start_ms;
    ULONGLONG attachment_start_ms;
    bool redirected_tee;
    bool attachment_tee;
    bool output_eof;
    bool redirected_output_eof;
    bool foreground_request_registration_pending;
    bool process_completed;
    bool completion_has_exit_code;
    uint32_t completion_exit_code;
} WinxtermHostManagedChild;

typedef struct WinxtermManagedRuntimeRegistry {
    CRITICAL_SECTION lock;
    bool lock_initialized;
    WinxtermHostManagedChild *head;
} WinxtermManagedRuntimeRegistry;

void winxterm_managed_runtime_registry_init(WinxtermManagedRuntimeRegistry *registry);
void winxterm_managed_runtime_registry_dispose(WinxtermManagedRuntimeRegistry *registry);

#endif
