#ifndef WINXTERM_JOB_JOURNAL_H
#define WINXTERM_JOB_JOURNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct WinxtermJobJournal {
    uint8_t *bytes;
    size_t capacity;
    size_t committed;
    size_t head;
    size_t count;
    uint64_t consumed_offset;
    uint64_t produced_offset;
} WinxtermJobJournal;

bool winxterm_job_journal_init(WinxtermJobJournal *journal, size_t capacity);
void winxterm_job_journal_dispose(WinxtermJobJournal *journal);
bool winxterm_job_journal_append(WinxtermJobJournal *journal,
                                const uint8_t *bytes, size_t length);
size_t winxterm_job_journal_consume(WinxtermJobJournal *journal,
                                    uint8_t *bytes, size_t capacity);
bool winxterm_job_journal_snapshot(const WinxtermJobJournal *journal,
                                  uint64_t offset, uint8_t **bytes,
                                  size_t *length, uint64_t *next_offset);
bool winxterm_job_journal_copy_snapshot(const WinxtermJobJournal *journal,
                                       uint64_t offset, uint64_t end_offset,
                                       uint8_t *bytes,
                                       size_t capacity, size_t *length,
                                       uint64_t *next_offset, bool *more);
size_t winxterm_job_journal_retained(const WinxtermJobJournal *journal);
bool winxterm_job_journal_backpressured(const WinxtermJobJournal *journal);
size_t winxterm_job_journal_committed(const WinxtermJobJournal *journal);

#endif
