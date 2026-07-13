#include "winxterm_job_protocol.h"

#include <string.h>

static void winxterm_job_put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}

static void winxterm_job_put_u32(uint8_t *out, uint32_t value)
{
    for (unsigned int i = 0; i < 4u; ++i) out[i] = (uint8_t)(value >> (i * 8u));
}

static void winxterm_job_put_u64(uint8_t *out, uint64_t value)
{
    for (unsigned int i = 0; i < 8u; ++i) out[i] = (uint8_t)(value >> (i * 8u));
}

static uint16_t winxterm_job_get_u16(const uint8_t *in)
{
    return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
}

static uint32_t winxterm_job_get_u32(const uint8_t *in)
{
    uint32_t value = 0u;
    for (unsigned int i = 0; i < 4u; ++i) value |= (uint32_t)in[i] << (i * 8u);
    return value;
}

static uint64_t winxterm_job_get_u64(const uint8_t *in)
{
    uint64_t value = 0u;
    for (unsigned int i = 0; i < 8u; ++i) value |= (uint64_t)in[i] << (i * 8u);
    return value;
}

bool winxterm_job_frame_encode_header(const WinxtermJobFrameHeader *header,
                                      uint8_t out[WINXTERM_JOB_PROTOCOL_HEADER_SIZE])
{
    if (header == 0 || out == 0 || header->version != WINXTERM_JOB_PROTOCOL_VERSION ||
        header->type == 0u || header->payload_length > WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD) return false;
    winxterm_job_put_u32(out, WINXTERM_JOB_PROTOCOL_MAGIC);
    winxterm_job_put_u16(out + 4u, header->version);
    winxterm_job_put_u16(out + 6u, header->type);
    winxterm_job_put_u32(out + 8u, header->flags);
    winxterm_job_put_u64(out + 12u, header->request_id);
    winxterm_job_put_u32(out + 20u, header->payload_length);
    return true;
}

bool winxterm_job_frame_decode_header(const uint8_t *bytes, size_t length,
                                      WinxtermJobFrameHeader *header)
{
    if (bytes == 0 || header == 0 || length < WINXTERM_JOB_PROTOCOL_HEADER_SIZE ||
        winxterm_job_get_u32(bytes) != WINXTERM_JOB_PROTOCOL_MAGIC) return false;
    header->version = winxterm_job_get_u16(bytes + 4u);
    header->type = winxterm_job_get_u16(bytes + 6u);
    header->flags = winxterm_job_get_u32(bytes + 8u);
    header->request_id = winxterm_job_get_u64(bytes + 12u);
    header->payload_length = winxterm_job_get_u32(bytes + 20u);
    return header->version == WINXTERM_JOB_PROTOCOL_VERSION && header->type != 0u &&
           header->payload_length <= WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD;
}

bool winxterm_job_tlv_append(uint8_t *payload, size_t capacity, size_t *length,
                            uint16_t type, const void *value, uint32_t value_length)
{
    if (payload == 0 || length == 0 || type == 0u ||
        (value_length != 0u && value == 0) || *length > capacity ||
        value_length > WINXTERM_JOB_PROTOCOL_MAX_PAYLOAD ||
        capacity - *length < 8u || capacity - *length - 8u < value_length) return false;
    uint8_t *out = payload + *length;
    winxterm_job_put_u16(out, type);
    winxterm_job_put_u16(out + 2u, 0u);
    winxterm_job_put_u32(out + 4u, value_length);
    if (value_length != 0u) memcpy(out + 8u, value, value_length);
    *length += 8u + value_length;
    return true;
}

bool winxterm_job_tlv_append_u32(uint8_t *payload, size_t capacity, size_t *length,
                                uint16_t type, uint32_t value)
{
    uint8_t bytes[4]; winxterm_job_put_u32(bytes, value);
    return winxterm_job_tlv_append(payload, capacity, length, type, bytes, sizeof(bytes));
}

bool winxterm_job_tlv_append_u64(uint8_t *payload, size_t capacity, size_t *length,
                                uint16_t type, uint64_t value)
{
    uint8_t bytes[8]; winxterm_job_put_u64(bytes, value);
    return winxterm_job_tlv_append(payload, capacity, length, type, bytes, sizeof(bytes));
}

void winxterm_job_tlv_reader_init(WinxtermJobTlvReader *reader, const uint8_t *payload, size_t length)
{
    if (reader != 0) { reader->payload = payload; reader->length = length; reader->offset = 0u; }
}

bool winxterm_job_tlv_next(WinxtermJobTlvReader *reader, WinxtermJobTlv *tlv)
{
    if (reader == 0 || tlv == 0 || reader->payload == 0 || reader->offset > reader->length ||
        reader->length - reader->offset < 8u) return false;
    const uint8_t *in = reader->payload + reader->offset;
    uint32_t value_length = winxterm_job_get_u32(in + 4u);
    if (value_length > reader->length - reader->offset - 8u) return false;
    tlv->type = winxterm_job_get_u16(in);
    tlv->length = value_length;
    tlv->value = in + 8u;
    reader->offset += 8u + value_length;
    return tlv->type != 0u;
}

bool winxterm_job_tlv_read_u32(const WinxtermJobTlv *tlv, uint32_t *value)
{
    if (tlv == 0 || value == 0 || tlv->value == 0 || tlv->length != 4u) return false;
    *value = winxterm_job_get_u32(tlv->value); return true;
}

bool winxterm_job_tlv_read_u64(const WinxtermJobTlv *tlv, uint64_t *value)
{
    if (tlv == 0 || value == 0 || tlv->value == 0 || tlv->length != 8u) return false;
    *value = winxterm_job_get_u64(tlv->value); return true;
}
