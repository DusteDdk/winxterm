#ifndef WINXTERM_JOB_CHANNEL_H
#define WINXTERM_JOB_CHANNEL_H

#include "winxterm_job_protocol.h"

#include <windows.h>

typedef struct WinxtermJobFrame {
    WinxtermJobFrameHeader header;
    uint8_t *payload;
} WinxtermJobFrame;

bool winxterm_job_channel_write(HANDLE handle,
                                const WinxtermJobFrameHeader *header,
                                const uint8_t *payload);
bool winxterm_job_channel_read(HANDLE handle, WinxtermJobFrame *frame);
void winxterm_job_frame_dispose(WinxtermJobFrame *frame);

#endif
