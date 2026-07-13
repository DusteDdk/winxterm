#include "winxterm_job_journal.h"
#include "winxterm_job_manager.h"

#include <stdlib.h>
#include <string.h>
#include <windows.h>

static bool winxterm_job_journal_commit_through(WinxtermJobJournal *journal, size_t end)
{
    if (end <= journal->committed) return true;
    SYSTEM_INFO system_info;
    GetSystemInfo(&system_info);
    size_t page = system_info.dwPageSize != 0u ? (size_t)system_info.dwPageSize : 4096u;
    if (end > SIZE_MAX - (page - 1u)) return false;
    size_t committed = (end + page - 1u) / page * page;
    if (committed > journal->capacity) committed = journal->capacity;
    size_t length = committed - journal->committed;
    if (length != 0u &&
        VirtualAlloc(journal->bytes + journal->committed, length,
                     MEM_COMMIT, PAGE_READWRITE) == 0) return false;
    journal->committed = committed;
    return true;
}

bool winxterm_job_journal_init(WinxtermJobJournal *journal, size_t capacity)
{
    if (journal == 0 || capacity == 0u || capacity > WINXTERM_JOB_OUTPUT_LIMIT) return false;
    memset(journal, 0, sizeof(*journal));
    journal->bytes = (uint8_t *)VirtualAlloc(0, capacity, MEM_RESERVE, PAGE_READWRITE);
    if (journal->bytes == 0) return false;
    journal->capacity = capacity;
    return true;
}

void winxterm_job_journal_dispose(WinxtermJobJournal *journal)
{
    if (journal == 0) return;
    if (journal->bytes != 0) (void)VirtualFree(journal->bytes, 0u, MEM_RELEASE);
    memset(journal, 0, sizeof(*journal));
}

bool winxterm_job_journal_append(WinxtermJobJournal *journal,
                                const uint8_t *bytes, size_t length)
{
    if (journal == 0 || (bytes == 0 && length != 0u) ||
        length > journal->capacity - journal->count ||
        UINT64_MAX - journal->produced_offset < length) return false;
    if (length == 0u) return true;
    size_t tail = (journal->head + journal->count) % journal->capacity;
    size_t first = length < journal->capacity - tail ? length : journal->capacity - tail;
    if (!winxterm_job_journal_commit_through(journal, tail + first) ||
        (length != first && !winxterm_job_journal_commit_through(journal, length - first))) {
        return false;
    }
    if (first != 0u) memcpy(journal->bytes + tail, bytes, first);
    if (length != first) memcpy(journal->bytes, bytes + first, length - first);
    journal->count += length;
    journal->produced_offset += length;
    return true;
}

size_t winxterm_job_journal_consume(WinxtermJobJournal *journal,
                                    uint8_t *bytes, size_t capacity)
{
    if (journal == 0 || (bytes == 0 && capacity != 0u)) return 0u;
    size_t length = capacity < journal->count ? capacity : journal->count;
    size_t first = length < journal->capacity - journal->head ? length : journal->capacity - journal->head;
    if (first != 0u) memcpy(bytes, journal->bytes + journal->head, first);
    if (length != first) memcpy(bytes + first, journal->bytes, length - first);
    journal->head = (journal->head + length) % journal->capacity;
    journal->count -= length;
    journal->consumed_offset += length;
    return length;
}

bool winxterm_job_journal_snapshot(const WinxtermJobJournal *journal,
                                  uint64_t offset, uint8_t **bytes,
                                  size_t *length, uint64_t *next_offset)
{
    if (bytes != 0) *bytes = 0;
    if (length != 0) *length = 0u;
    if (next_offset != 0) *next_offset = 0u;
    if (journal == 0 || bytes == 0 || length == 0 || next_offset == 0 ||
        offset < journal->consumed_offset || offset > journal->produced_offset) return false;
    size_t skip = (size_t)(offset - journal->consumed_offset);
    if (skip > journal->count) return false;
    size_t count = journal->count - skip;
    uint8_t *copy = count != 0u ? (uint8_t *)malloc(count) : 0;
    if (count != 0u && copy == 0) return false;
    size_t start = (journal->head + skip) % journal->capacity;
    size_t first = count < journal->capacity - start ? count : journal->capacity - start;
    if (first != 0u) memcpy(copy, journal->bytes + start, first);
    if (count != first) memcpy(copy + first, journal->bytes, count - first);
    *bytes = copy;
    *length = count;
    *next_offset = journal->produced_offset;
    return true;
}

bool winxterm_job_journal_copy_snapshot(const WinxtermJobJournal *journal,
                                       uint64_t offset, uint64_t end_offset,
                                       uint8_t *bytes,
                                       size_t capacity, size_t *length,
                                       uint64_t *next_offset, bool *more)
{
    if (length != 0) *length = 0u;
    if (next_offset != 0) *next_offset = offset;
    if (more != 0) *more = false;
    if (journal == 0 || (bytes == 0 && capacity != 0u) || length == 0 ||
        next_offset == 0 || more == 0 || offset < journal->consumed_offset ||
        offset > journal->produced_offset || end_offset < offset ||
        end_offset > journal->produced_offset) return false;
    size_t skip = (size_t)(offset - journal->consumed_offset);
    if (skip > journal->count) return false;
    size_t available = (size_t)(end_offset - offset);
    size_t count = capacity < available ? capacity : available;
    size_t start = (journal->head + skip) % journal->capacity;
    size_t first = count < journal->capacity - start ? count : journal->capacity - start;
    if (first != 0u) memcpy(bytes, journal->bytes + start, first);
    if (count != first) memcpy(bytes + first, journal->bytes, count - first);
    *length = count;
    *next_offset = offset + count;
    *more = count < available;
    return true;
}

size_t winxterm_job_journal_retained(const WinxtermJobJournal *journal)
{ return journal != 0 ? journal->count : 0u; }

bool winxterm_job_journal_backpressured(const WinxtermJobJournal *journal)
{ return journal != 0 && journal->count == journal->capacity; }

size_t winxterm_job_journal_committed(const WinxtermJobJournal *journal)
{ return journal != 0 ? journal->committed : 0u; }
