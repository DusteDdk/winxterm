#ifndef WINXTERM_JOB_PROTOCOL_H
#define WINXTERM_JOB_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WINXTERM_JOB_PROTOCOL_MAGIC 0x424f4a57u /* "WJOB" little-endian */
#define WINXTERM_JOB_PROTOCOL_VERSION 1u
#define WINXTERM_JOB_PROTOCOL_HEADER_SIZE 24u
#define WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD (1024u * 1024u)
#define WINXTERM_JOB_ENV_PROTOCOL L"WINXTERM_JOB_PROTOCOL"
#define WINXTERM_JOB_ENV_REQUEST_HANDLE L"WINXTERM_JOB_REQUEST_HANDLE"
#define WINXTERM_JOB_ENV_REPLY_HANDLE L"WINXTERM_JOB_REPLY_HANDLE"
#define WINXTERM_JOB_ENV_SELF_ID L"WINXTERM_JOB_SELF_ID"

#define WINXTERM_JOB_CAPABILITY_LIST 0x00000001u
/* 0x00000002 was the never-correct per-job terminal-session capability and is
   intentionally left unassigned for protocol-version compatibility. */
#define WINXTERM_JOB_CAPABILITY_OUTPUT_JOURNAL 0x00000004u
#define WINXTERM_JOB_CAPABILITY_LIFECYCLE 0x00000008u
#define WINXTERM_JOB_CAPABILITY_EVENTS 0x00000010u
#define WINXTERM_JOB_CAPABILITY_SPAWN 0x00000020u
#define WINXTERM_JOB_CAPABILITY_CONNECTIONS 0x00000040u
#define WINXTERM_JOB_CAPABILITY_DISCONNECT 0x00000080u
#define WINXTERM_JOB_CAPABILITY_PIPELINES 0x00000100u
#define WINXTERM_JOB_CAPABILITY_FILE_ENDPOINTS 0x00000200u
#define WINXTERM_JOB_CAPABILITY_ATTACHMENTS 0x00000400u
#define WINXTERM_JOB_CAPABILITY_INTERACTIVE_UI 0x00000800u

#define WINXTERM_JOB_FLAG_FOREGROUND 0x00000001u
#define WINXTERM_JOB_FLAG_SELF 0x00000002u
#define WINXTERM_JOB_FLAG_CONNECTABLE 0x00000004u
#define WINXTERM_JOB_FLAG_BACKPRESSURED 0x00000008u
#define WINXTERM_JOB_FLAG_HAS_EXIT_CODE 0x00000010u
#define WINXTERM_JOB_FLAG_APPEND 0x00000020u
#define WINXTERM_JOB_FLAG_TEE 0x00000040u
#define WINXTERM_JOB_FLAG_MORE 0x80000000u
#define WINXTERM_JOB_SIGNAL_FORCE 0x00000001u

typedef enum WinxtermJobMessageType {
    WINXTERM_JOB_MESSAGE_CAPABILITIES = 1,
    WINXTERM_JOB_MESSAGE_SPAWN = 2,
    WINXTERM_JOB_MESSAGE_LIST = 3,
    WINXTERM_JOB_MESSAGE_FOREGROUND = 4,
    WINXTERM_JOB_MESSAGE_BACKGROUND = 5,
    WINXTERM_JOB_MESSAGE_VIEW = 6,
    WINXTERM_JOB_MESSAGE_SIGNAL = 7,
    WINXTERM_JOB_MESSAGE_REMOVE = 8,
    WINXTERM_JOB_MESSAGE_CLEAN = 9,
    WINXTERM_JOB_MESSAGE_CONNECT = 10,
    WINXTERM_JOB_MESSAGE_ATTACH_FILE = 11,
    WINXTERM_JOB_MESSAGE_DETACH = 12,
    WINXTERM_JOB_MESSAGE_INTERACTIVE = 13,
    WINXTERM_JOB_MESSAGE_DISCONNECT = 14,
    WINXTERM_JOB_MESSAGE_CANCEL = 15,
    WINXTERM_JOB_MESSAGE_REPLY = 0x4000,
    WINXTERM_JOB_MESSAGE_EVENT = 0x4001
} WinxtermJobMessageType;

typedef enum WinxtermJobTlvType {
    WINXTERM_JOB_TLV_STATUS = 1,
    WINXTERM_JOB_TLV_ERROR = 2,
    WINXTERM_JOB_TLV_JOB_ID = 3,
    WINXTERM_JOB_TLV_OWNER_ID = 4,
    WINXTERM_JOB_TLV_STATE = 5,
    WINXTERM_JOB_TLV_EXIT_CODE = 6,
    WINXTERM_JOB_TLV_FLAGS = 7,
    WINXTERM_JOB_TLV_COMMAND = 8,
    WINXTERM_JOB_TLV_CWD = 9,
    WINXTERM_JOB_TLV_ENVIRONMENT = 10,
    WINXTERM_JOB_TLV_DISPLAY_NAME = 11,
    WINXTERM_JOB_TLV_OUTPUT = 12,
    WINXTERM_JOB_TLV_TIMEOUT_MS = 13,
    WINXTERM_JOB_TLV_SOURCE_ID = 14,
    WINXTERM_JOB_TLV_DESTINATION_ID = 15,
    WINXTERM_JOB_TLV_PATH = 16,
    WINXTERM_JOB_TLV_PROCESS_ID = 17,
    WINXTERM_JOB_TLV_BUFFERED_OUTPUT = 18,
    WINXTERM_JOB_TLV_CURSOR = 19,
    WINXTERM_JOB_TLV_SNAPSHOT_MAX_ID = 20,
    WINXTERM_JOB_TLV_EVENT_KIND = 21,
    WINXTERM_JOB_TLV_STAGE = 22,
    WINXTERM_JOB_TLV_ARGUMENT = 23,
    WINXTERM_JOB_TLV_ENDPOINT = 24,
    WINXTERM_JOB_TLV_SNAPSHOT_OFFSET = 25,
    WINXTERM_JOB_TLV_COUNT = 26,
    WINXTERM_JOB_TLV_REQUEST_ID = 27
} WinxtermJobTlvType;

typedef enum WinxtermJobEventKind {
    WINXTERM_JOB_EVENT_EXITED = 1,
    WINXTERM_JOB_EVENT_FOREGROUND_CHANGED = 2,
    WINXTERM_JOB_EVENT_RESYNC_REQUIRED = 3,
    WINXTERM_JOB_EVENT_CONNECTED = 4,
    WINXTERM_JOB_EVENT_DISCONNECTED = 5,
    WINXTERM_JOB_EVENT_ADDED = 6
} WinxtermJobEventKind;

typedef struct WinxtermJobFrameHeader {
    uint16_t version;
    uint16_t type;
    uint32_t flags;
    uint64_t request_id;
    uint32_t payload_length;
} WinxtermJobFrameHeader;

typedef struct WinxtermJobTlv {
    uint16_t type;
    const uint8_t *value;
    uint32_t length;
} WinxtermJobTlv;

typedef struct WinxtermJobTlvReader {
    const uint8_t *payload;
    size_t length;
    size_t offset;
} WinxtermJobTlvReader;

bool winxterm_job_frame_encode_header(const WinxtermJobFrameHeader *header,
                                      uint8_t out[WINXTERM_JOB_PROTOCOL_HEADER_SIZE]);
bool winxterm_job_frame_decode_header(const uint8_t *bytes,
                                      size_t length,
                                      WinxtermJobFrameHeader *header);
bool winxterm_job_tlv_append(uint8_t *payload,
                            size_t capacity,
                            size_t *length,
                            uint16_t type,
                            const void *value,
                            uint32_t value_length);
bool winxterm_job_tlv_append_u32(uint8_t *payload, size_t capacity, size_t *length,
                                uint16_t type, uint32_t value);
bool winxterm_job_tlv_append_u64(uint8_t *payload, size_t capacity, size_t *length,
                                uint16_t type, uint64_t value);
void winxterm_job_tlv_reader_init(WinxtermJobTlvReader *reader,
                                 const uint8_t *payload,
                                 size_t length);
bool winxterm_job_tlv_next(WinxtermJobTlvReader *reader, WinxtermJobTlv *tlv);
bool winxterm_job_tlv_read_u32(const WinxtermJobTlv *tlv, uint32_t *value);
bool winxterm_job_tlv_read_u64(const WinxtermJobTlv *tlv, uint64_t *value);

#endif
