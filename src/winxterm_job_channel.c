#include "winxterm_job_channel.h"

#include <stdlib.h>
#include <string.h>

static bool winxterm_job_channel_write_exact(HANDLE handle, const uint8_t *bytes, size_t length)
{
    if (handle == 0 || handle == INVALID_HANDLE_VALUE || (length != 0u && bytes == 0)) {
        return false;
    }
    size_t offset = 0u;
    while (offset < length) {
        DWORD chunk = length - offset > (size_t)MAXDWORD ? MAXDWORD : (DWORD)(length - offset);
        DWORD written = 0u;
        if (!WriteFile(handle, bytes + offset, chunk, &written, 0) || written == 0u) {
            return false;
        }
        offset += (size_t)written;
    }
    return true;
}

static bool winxterm_job_channel_read_exact(HANDLE handle, uint8_t *bytes, size_t length)
{
    if (handle == 0 || handle == INVALID_HANDLE_VALUE || (length != 0u && bytes == 0)) {
        return false;
    }
    size_t offset = 0u;
    while (offset < length) {
        DWORD chunk = length - offset > (size_t)MAXDWORD ? MAXDWORD : (DWORD)(length - offset);
        DWORD read = 0u;
        if (!ReadFile(handle, bytes + offset, chunk, &read, 0) || read == 0u) {
            return false;
        }
        offset += (size_t)read;
    }
    return true;
}

bool winxterm_job_channel_write(HANDLE handle,
                                const WinxtermJobFrameHeader *header,
                                const uint8_t *payload)
{
    uint8_t encoded[WINXTERM_JOB_PROTOCOL_HEADER_SIZE];
    if (header == 0 ||
        (header->payload_length != 0u && payload == 0) ||
        !winxterm_job_frame_encode_header(header, encoded)) {
        return false;
    }
    return winxterm_job_channel_write_exact(handle, encoded, sizeof(encoded)) &&
           winxterm_job_channel_write_exact(handle, payload, header->payload_length);
}

bool winxterm_job_channel_read(HANDLE handle, WinxtermJobFrame *frame)
{
    if (frame == 0) {
        return false;
    }
    memset(frame, 0, sizeof(*frame));
    uint8_t encoded[WINXTERM_JOB_PROTOCOL_HEADER_SIZE];
    if (!winxterm_job_channel_read_exact(handle, encoded, sizeof(encoded)) ||
        !winxterm_job_frame_decode_header(encoded, sizeof(encoded), &frame->header)) {
        return false;
    }
    if (frame->header.payload_length == 0u) {
        return true;
    }
    frame->payload = (uint8_t *)malloc(frame->header.payload_length);
    if (frame->payload == 0 ||
        !winxterm_job_channel_read_exact(handle, frame->payload, frame->header.payload_length)) {
        winxterm_job_frame_dispose(frame);
        return false;
    }
    return true;
}

void winxterm_job_frame_dispose(WinxtermJobFrame *frame)
{
    if (frame == 0) {
        return;
    }
    free(frame->payload);
    memset(frame, 0, sizeof(*frame));
}
