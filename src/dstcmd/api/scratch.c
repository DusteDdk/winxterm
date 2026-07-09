#include "dstcmd/api/scratch.h"

#include "dstcmd/winxterm_dstcmd.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define WINXTERM_DSTCMD_SCRATCH_BLOCK_BYTES (WINXTERM_DSTCMD_PATH_CAPACITY * sizeof(wchar_t) * 4u)

struct WinxtermDstcmdScratchBlock {
    struct WinxtermDstcmdScratchBlock *next;
    size_t capacity;
    size_t used;
    unsigned char bytes[1];
};

static size_t winxterm_dstcmd_scratch_align(size_t value, size_t alignment)
{
    if (alignment <= 1u) {
        return value;
    }
    size_t remainder = value % alignment;
    return remainder == 0u ? value : value + (alignment - remainder);
}

static WinxtermDstcmdScratchBlock *winxterm_dstcmd_scratch_block_create(size_t capacity)
{
    if (capacity == 0u || capacity > ((size_t)-1) - sizeof(WinxtermDstcmdScratchBlock)) {
        return 0;
    }
    WinxtermDstcmdScratchBlock *block =
        (WinxtermDstcmdScratchBlock *)calloc(1u, sizeof(*block) + capacity - 1u);
    if (block == 0) {
        return 0;
    }
    block->capacity = capacity;
    return block;
}

void winxterm_dstcmd_scratch_init(WinxtermDstcmdScratch *scratch)
{
    if (scratch != 0) {
        memset(scratch, 0, sizeof(*scratch));
    }
}

void winxterm_dstcmd_scratch_dispose(WinxtermDstcmdScratch *scratch)
{
    if (scratch == 0) {
        return;
    }
    WinxtermDstcmdScratchBlock *block = scratch->first;
    while (block != 0) {
        WinxtermDstcmdScratchBlock *next = block->next;
        free(block);
        block = next;
    }
    memset(scratch, 0, sizeof(*scratch));
}

void winxterm_dstcmd_scratch_reset(WinxtermDstcmdScratch *scratch)
{
    if (scratch == 0) {
        return;
    }
    for (WinxtermDstcmdScratchBlock *block = scratch->first; block != 0; block = block->next) {
        block->used = 0u;
    }
    scratch->current = scratch->first;
    scratch->failed = false;
}

WinxtermDstcmdScratchMark winxterm_dstcmd_scratch_mark(WinxtermDstcmdScratch *scratch)
{
    WinxtermDstcmdScratchMark mark = {0};
    if (scratch != 0 && scratch->current != 0) {
        mark.block = scratch->current;
        mark.used = scratch->current->used;
    }
    return mark;
}

void winxterm_dstcmd_scratch_rewind(WinxtermDstcmdScratch *scratch,
                                    WinxtermDstcmdScratchMark mark)
{
    if (scratch == 0) {
        return;
    }
    if (mark.block == 0) {
        winxterm_dstcmd_scratch_reset(scratch);
        return;
    }
    bool found = false;
    for (WinxtermDstcmdScratchBlock *block = scratch->first; block != 0; block = block->next) {
        if (block == mark.block) {
            block->used = mark.used <= block->capacity ? mark.used : block->capacity;
            scratch->current = block;
            found = true;
        } else if (found) {
            block->used = 0u;
        }
    }
    if (!found) {
        winxterm_dstcmd_scratch_reset(scratch);
    }
    scratch->failed = false;
}

static bool winxterm_dstcmd_scratch_ensure_block(WinxtermDstcmdScratch *scratch,
                                                 size_t byte_count,
                                                 size_t alignment)
{
    if (scratch == 0 || scratch->failed) {
        return false;
    }
    if (alignment == 0u) {
        alignment = sizeof(void *);
    }
    size_t needed = byte_count + alignment;
    if (needed < byte_count) {
        scratch->failed = true;
        return false;
    }
    size_t capacity = WINXTERM_DSTCMD_SCRATCH_BLOCK_BYTES;
    if (capacity < needed) {
        capacity = needed;
    }
    if (scratch->first == 0) {
        scratch->first = winxterm_dstcmd_scratch_block_create(capacity);
        scratch->current = scratch->first;
        if (scratch->first == 0) {
            scratch->failed = true;
            return false;
        }
    }
    if (scratch->current == 0) {
        scratch->current = scratch->first;
    }
    return true;
}

void *winxterm_dstcmd_scratch_alloc(WinxtermDstcmdScratch *scratch,
                                    size_t byte_count,
                                    size_t alignment)
{
    if (byte_count == 0u) {
        byte_count = 1u;
    }
    if (alignment == 0u) {
        alignment = sizeof(void *);
    }
    if (!winxterm_dstcmd_scratch_ensure_block(scratch, byte_count, alignment)) {
        return 0;
    }

    for (;;) {
        WinxtermDstcmdScratchBlock *block = scratch->current;
        size_t aligned = winxterm_dstcmd_scratch_align(block->used, alignment);
        if (aligned >= block->used && byte_count <= block->capacity - aligned) {
            void *result = block->bytes + aligned;
            block->used = aligned + byte_count;
            memset(result, 0, byte_count);
            return result;
        }
        if (block->next != 0) {
            scratch->current = block->next;
            scratch->current->used = 0u;
            continue;
        }
        size_t needed = byte_count + alignment;
        size_t capacity = WINXTERM_DSTCMD_SCRATCH_BLOCK_BYTES;
        if (capacity < needed) {
            capacity = needed;
        }
        block->next = winxterm_dstcmd_scratch_block_create(capacity);
        if (block->next == 0) {
            scratch->failed = true;
            return 0;
        }
        scratch->current = block->next;
    }
}

wchar_t *winxterm_dstcmd_scratch_alloc_wchars(WinxtermDstcmdScratch *scratch,
                                              size_t count)
{
    if (count > ((size_t)-1) / sizeof(wchar_t)) {
        if (scratch != 0) {
            scratch->failed = true;
        }
        return 0;
    }
    return (wchar_t *)winxterm_dstcmd_scratch_alloc(scratch,
                                                    count * sizeof(wchar_t),
                                                    sizeof(wchar_t));
}

wchar_t *winxterm_dstcmd_scratch_alloc_path(WinxtermDstcmdScratch *scratch)
{
    return winxterm_dstcmd_scratch_alloc_wchars(scratch, WINXTERM_DSTCMD_PATH_CAPACITY);
}
