#ifndef WINXTERM_DSTCMD_API_SCRATCH_H
#define WINXTERM_DSTCMD_API_SCRATCH_H

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

typedef struct WinxtermDstcmdScratchBlock WinxtermDstcmdScratchBlock;

typedef struct WinxtermDstcmdScratch {
    WinxtermDstcmdScratchBlock *first;
    WinxtermDstcmdScratchBlock *current;
    bool failed;
} WinxtermDstcmdScratch;

typedef struct WinxtermDstcmdScratchMark {
    WinxtermDstcmdScratchBlock *block;
    size_t used;
} WinxtermDstcmdScratchMark;

void winxterm_dstcmd_scratch_init(WinxtermDstcmdScratch *scratch);
void winxterm_dstcmd_scratch_dispose(WinxtermDstcmdScratch *scratch);
void winxterm_dstcmd_scratch_reset(WinxtermDstcmdScratch *scratch);
WinxtermDstcmdScratchMark winxterm_dstcmd_scratch_mark(WinxtermDstcmdScratch *scratch);
void winxterm_dstcmd_scratch_rewind(WinxtermDstcmdScratch *scratch,
                                    WinxtermDstcmdScratchMark mark);
void *winxterm_dstcmd_scratch_alloc(WinxtermDstcmdScratch *scratch,
                                    size_t byte_count,
                                    size_t alignment);
wchar_t *winxterm_dstcmd_scratch_alloc_wchars(WinxtermDstcmdScratch *scratch,
                                              size_t count);
wchar_t *winxterm_dstcmd_scratch_alloc_path(WinxtermDstcmdScratch *scratch);

#endif
