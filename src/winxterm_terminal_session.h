#ifndef WINXTERM_TERMINAL_SESSION_H
#define WINXTERM_TERMINAL_SESSION_H

#include "winxterm_job_journal.h"
#include "winxterm_screen.h"
#include "winxterm_text.h"

#include <stdbool.h>
#include <windows.h>

#define WINXTERM_TERMINAL_SESSION_TITLE_CAPACITY 256u
#define WINXTERM_TERMINAL_SESSION_TRANSCRIPT_CAPACITY (64u * 1024u * 1024u)

typedef struct WinxtermTerminalSession {
    CRITICAL_SECTION output_lock;
    bool output_lock_initialized;
    WinxtermJobJournal journal;
    WinxtermScreen screen;
    WinxtermUtf8Decoder decoder;
    bool screen_stored;
    char title[WINXTERM_TERMINAL_SESSION_TITLE_CAPACITY];
    uint8_t *transcript;
    size_t transcript_capacity;
    size_t transcript_head;
    size_t transcript_count;
    uint64_t transcript_base_offset;
    uint64_t transcript_produced_offset;
    uint8_t *pending_input;
    size_t pending_input_count;
} WinxtermTerminalSession;

bool winxterm_terminal_session_init(WinxtermTerminalSession *session,
                                    int columns, int rows, bool initialize_screen);
void winxterm_terminal_session_dispose(WinxtermTerminalSession *session);
bool winxterm_terminal_session_record(WinxtermTerminalSession *session,
                                      const uint8_t *bytes, size_t byte_count);
bool winxterm_terminal_session_copy_transcript(WinxtermTerminalSession *session,
                                               uint8_t **bytes, size_t *byte_count);
bool winxterm_terminal_session_copy_transcript_page(WinxtermTerminalSession *session,
                                                    uint64_t offset, uint64_t end_offset,
                                                    uint8_t *bytes, size_t capacity,
                                                    size_t *byte_count, uint64_t *next_offset,
                                                    bool *more);

#endif
