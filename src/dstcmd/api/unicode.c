#include "dstcmd/api/unicode.h"

static bool winxterm_dstcmd_utf8_is_continuation(unsigned char byte)
{
    return (byte & 0xc0u) == 0x80u;
}

bool winxterm_dstcmd_utf8_decode_next(const char *text,
                                      size_t length,
                                      size_t *offset,
                                      uint32_t *codepoint)
{
    if (text == 0 || offset == 0 || codepoint == 0 || *offset >= length) {
        return false;
    }
    unsigned char first = (unsigned char)text[*offset];
    if (first < 0x80u) {
        *codepoint = first;
        ++(*offset);
        return true;
    }
    size_t needed = 0u;
    uint32_t value = 0u;
    if ((first & 0xe0u) == 0xc0u) {
        needed = 2u;
        value = first & 0x1fu;
    } else if ((first & 0xf0u) == 0xe0u) {
        needed = 3u;
        value = first & 0x0fu;
    } else if ((first & 0xf8u) == 0xf0u) {
        needed = 4u;
        value = first & 0x07u;
    } else {
        *codepoint = first;
        ++(*offset);
        return false;
    }
    if (*offset + needed > length) {
        *codepoint = first;
        ++(*offset);
        return false;
    }
    for (size_t i = 1u; i < needed; ++i) {
        unsigned char next = (unsigned char)text[*offset + i];
        if (!winxterm_dstcmd_utf8_is_continuation(next)) {
            *codepoint = first;
            ++(*offset);
            return false;
        }
        value = (value << 6) | (uint32_t)(next & 0x3fu);
    }
    *offset += needed;
    *codepoint = value;
    return true;
}

size_t winxterm_dstcmd_utf8_prev_boundary(const char *text, size_t length, size_t offset)
{
    if (text == 0 || offset == 0u) {
        return 0u;
    }
    if (offset > length) {
        offset = length;
    }
    do {
        --offset;
    } while (offset > 0u && winxterm_dstcmd_utf8_is_continuation((unsigned char)text[offset]));
    return offset;
}

size_t winxterm_dstcmd_utf8_next_boundary(const char *text, size_t length, size_t offset)
{
    if (text == 0 || offset >= length) {
        return length;
    }
    size_t next = offset;
    uint32_t codepoint = 0u;
    (void)winxterm_dstcmd_utf8_decode_next(text, length, &next, &codepoint);
    if (next <= offset) {
        next = offset + 1u;
    }
    return next;
}

bool winxterm_dstcmd_codepoint_is_combining(uint32_t codepoint)
{
    return (codepoint >= 0x0300u && codepoint <= 0x036fu) ||
           (codepoint >= 0x1ab0u && codepoint <= 0x1affu) ||
           (codepoint >= 0x1dc0u && codepoint <= 0x1dffu) ||
           (codepoint >= 0x20d0u && codepoint <= 0x20ffu) ||
           (codepoint >= 0xfe00u && codepoint <= 0xfe0fu) ||
           (codepoint >= 0xfe20u && codepoint <= 0xfe2fu) ||
           (codepoint >= 0x1f3fbu && codepoint <= 0x1f3ffu);
}

int winxterm_dstcmd_codepoint_width(uint32_t codepoint)
{
    if (winxterm_dstcmd_codepoint_is_combining(codepoint) ||
        codepoint < 0x20u ||
        (codepoint >= 0x7fu && codepoint < 0xa0u)) {
        return 0;
    }
    /* Match the terminal screen policy: dynamic fallback glyphs occupy one cell. */
    if (codepoint > 0xffu) {
        return 1;
    }
    return 1;
}

static bool winxterm_dstcmd_wide_is_high_surrogate(wchar_t ch)
{
    return ch >= (wchar_t)0xd800u && ch <= (wchar_t)0xdbffu;
}

static bool winxterm_dstcmd_wide_is_low_surrogate(wchar_t ch)
{
    return ch >= (wchar_t)0xdc00u && ch <= (wchar_t)0xdfffu;
}

bool winxterm_dstcmd_wide_decode_next(const wchar_t *text,
                                      size_t length,
                                      size_t *offset,
                                      uint32_t *codepoint)
{
    if (text == 0 || offset == 0 || codepoint == 0 || *offset >= length) {
        return false;
    }
    wchar_t first = text[*offset];
    if (winxterm_dstcmd_wide_is_high_surrogate(first) &&
        *offset + 1u < length &&
        winxterm_dstcmd_wide_is_low_surrogate(text[*offset + 1u])) {
        uint32_t high = (uint32_t)first - 0xd800u;
        uint32_t low = (uint32_t)text[*offset + 1u] - 0xdc00u;
        *codepoint = 0x10000u + ((high << 10) | low);
        *offset += 2u;
        return true;
    }
    *codepoint = (uint32_t)first;
    ++(*offset);
    return !winxterm_dstcmd_wide_is_low_surrogate(first);
}

size_t winxterm_dstcmd_wide_next_boundary(const wchar_t *text, size_t length, size_t offset)
{
    if (text == 0 || offset >= length) {
        return length;
    }
    if (winxterm_dstcmd_wide_is_high_surrogate(text[offset]) &&
        offset + 1u < length &&
        winxterm_dstcmd_wide_is_low_surrogate(text[offset + 1u])) {
        return offset + 2u;
    }
    return offset + 1u;
}

size_t winxterm_dstcmd_wide_prev_boundary(const wchar_t *text, size_t length, size_t offset)
{
    if (text == 0 || offset == 0u) {
        return 0u;
    }
    if (offset > length) {
        offset = length;
    }
    --offset;
    if (offset > 0u &&
        winxterm_dstcmd_wide_is_low_surrogate(text[offset]) &&
        winxterm_dstcmd_wide_is_high_surrogate(text[offset - 1u])) {
        --offset;
    }
    return offset;
}

size_t winxterm_dstcmd_wide_safe_truncate(const wchar_t *text, size_t length, size_t offset)
{
    if (text == 0 || offset == 0u) {
        return 0u;
    }
    if (offset >= length) {
        return length;
    }
    if (winxterm_dstcmd_wide_is_low_surrogate(text[offset]) &&
        offset > 0u &&
        winxterm_dstcmd_wide_is_high_surrogate(text[offset - 1u])) {
        return offset - 1u;
    }
    return offset;
}
