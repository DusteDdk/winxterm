#include "winxterm_terminal_session.h"

#include "winxterm_job_manager.h"

#include <string.h>
#include <stdlib.h>

bool winxterm_terminal_session_init(WinxtermTerminalSession *session,
                                    int columns, int rows, bool initialize_screen)
{
    if (session == 0) return false;
    memset(session, 0, sizeof(*session));
    InitializeCriticalSection(&session->output_lock);
    session->output_lock_initialized = true;
    if (!winxterm_job_journal_init(&session->journal, WINXTERM_JOB_OUTPUT_LIMIT)) {
        winxterm_terminal_session_dispose(session);
        return false;
    }
    winxterm_utf8_decoder_init(&session->decoder);
    if (initialize_screen) {
        if (!winxterm_screen_init(&session->screen, columns, rows)) {
            winxterm_terminal_session_dispose(session);
            return false;
        }
        session->screen_stored = true;
    }
    return true;
}

void winxterm_terminal_session_dispose(WinxtermTerminalSession *session)
{
    if (session == 0) return;
    winxterm_job_journal_dispose(&session->journal);
    if (session->screen_stored) winxterm_screen_dispose(&session->screen);
    free(session->transcript);
    free(session->pending_input);
    if (session->output_lock_initialized) DeleteCriticalSection(&session->output_lock);
    memset(session, 0, sizeof(*session));
}

bool winxterm_terminal_session_record(WinxtermTerminalSession *session,
                                      const uint8_t *bytes, size_t byte_count)
{
    if (session == 0 || (bytes == 0 && byte_count != 0u)) return false;
    EnterCriticalSection(&session->output_lock);
    size_t original_byte_count = byte_count;
    if (UINT64_MAX - session->transcript_produced_offset < original_byte_count) {
        LeaveCriticalSection(&session->output_lock);
        return false;
    }
    if (byte_count >= WINXTERM_TERMINAL_SESSION_TRANSCRIPT_CAPACITY) {
        bytes += byte_count - WINXTERM_TERMINAL_SESSION_TRANSCRIPT_CAPACITY;
        byte_count = WINXTERM_TERMINAL_SESSION_TRANSCRIPT_CAPACITY;
        session->transcript_head = 0u;
        session->transcript_count = 0u;
    }
    size_t needed = session->transcript_count + byte_count;
    if (needed > session->transcript_capacity) {
        size_t capacity = session->transcript_capacity != 0u ? session->transcript_capacity : 65536u;
        while (capacity < needed && capacity < WINXTERM_TERMINAL_SESSION_TRANSCRIPT_CAPACITY) {
            capacity *= 2u;
        }
        if (capacity > WINXTERM_TERMINAL_SESSION_TRANSCRIPT_CAPACITY) {
            capacity = WINXTERM_TERMINAL_SESSION_TRANSCRIPT_CAPACITY;
        }
        uint8_t *grown = (uint8_t *)malloc(capacity);
        if (grown != 0) {
            size_t first = session->transcript_count <
                    session->transcript_capacity - session->transcript_head ?
                session->transcript_count : session->transcript_capacity - session->transcript_head;
            if (first != 0u) memcpy(grown, session->transcript + session->transcript_head, first);
            if (session->transcript_count != first) {
                memcpy(grown + first, session->transcript,
                       session->transcript_count - first);
            }
            free(session->transcript);
            session->transcript = grown;
            session->transcript_capacity = capacity;
            session->transcript_head = 0u;
        }
    }
    bool ok = byte_count == 0u || session->transcript_capacity != 0u;
    if (ok && byte_count != 0u) {
        if (byte_count >= session->transcript_capacity) {
            bytes += byte_count - session->transcript_capacity;
            byte_count = session->transcript_capacity;
            session->transcript_head = 0u;
            session->transcript_count = 0u;
            needed = byte_count;
        }
        size_t overflow = needed > session->transcript_capacity ?
            needed - session->transcript_capacity : 0u;
        session->transcript_head = (session->transcript_head + overflow) %
                                   session->transcript_capacity;
        session->transcript_count -= overflow;
        size_t tail = (session->transcript_head + session->transcript_count) %
                      session->transcript_capacity;
        size_t first = byte_count < session->transcript_capacity - tail ?
            byte_count : session->transcript_capacity - tail;
        memcpy(session->transcript + tail, bytes, first);
        if (byte_count != first) memcpy(session->transcript, bytes + first, byte_count - first);
        session->transcript_count += byte_count;
    }
    if (ok) {
        session->transcript_produced_offset += original_byte_count;
        session->transcript_base_offset =
            session->transcript_produced_offset - session->transcript_count;
    }
    LeaveCriticalSection(&session->output_lock);
    return ok;
}

bool winxterm_terminal_session_copy_transcript_page(WinxtermTerminalSession *session,
                                                    uint64_t offset, uint64_t end_offset,
                                                    uint8_t *bytes, size_t capacity,
                                                    size_t *byte_count, uint64_t *next_offset,
                                                    bool *more)
{
    if (byte_count != 0) *byte_count = 0u;
    if (next_offset != 0) *next_offset = offset;
    if (more != 0) *more = false;
    if (session == 0 || (bytes == 0 && capacity != 0u) || byte_count == 0 ||
        next_offset == 0 || more == 0) return false;
    EnterCriticalSection(&session->output_lock);
    bool ok = offset >= session->transcript_base_offset &&
              offset <= session->transcript_produced_offset &&
              end_offset >= offset && end_offset <= session->transcript_produced_offset;
    if (ok) {
        size_t skip = (size_t)(offset - session->transcript_base_offset);
        size_t available = (size_t)(end_offset - offset);
        size_t count = capacity < available ? capacity : available;
        if (count != 0u) {
            size_t start = (session->transcript_head + skip) % session->transcript_capacity;
            size_t first = count < session->transcript_capacity - start ?
                count : session->transcript_capacity - start;
            if (first != 0u) memcpy(bytes, session->transcript + start, first);
            if (count != first) memcpy(bytes + first, session->transcript, count - first);
        }
        *byte_count = count;
        *next_offset = offset + count;
        *more = count < available;
    }
    LeaveCriticalSection(&session->output_lock);
    return ok;
}

bool winxterm_terminal_session_copy_transcript(WinxtermTerminalSession *session,
                                               uint8_t **bytes, size_t *byte_count)
{
    if (bytes != 0) *bytes = 0;
    if (byte_count != 0) *byte_count = 0u;
    if (session == 0 || bytes == 0 || byte_count == 0) return false;
    EnterCriticalSection(&session->output_lock);
    size_t count = session->transcript_count;
    uint8_t *copy = count != 0u ? (uint8_t *)malloc(count) : 0;
    bool ok = count == 0u || copy != 0;
    if (ok && count != 0u) {
        size_t first = count < session->transcript_capacity - session->transcript_head ?
            count : session->transcript_capacity - session->transcript_head;
        memcpy(copy, session->transcript + session->transcript_head, first);
        if (count != first) memcpy(copy + first, session->transcript, count - first);
    }
    if (ok) {
        *bytes = copy;
        *byte_count = count;
    }
    LeaveCriticalSection(&session->output_lock);
    return ok;
}
