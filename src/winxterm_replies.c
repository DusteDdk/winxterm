#include "winxterm_replies.h"

#include <stdio.h>
#include <string.h>

static size_t winxterm_reply_copy(const char *text, uint8_t *out, size_t capacity)
{
    size_t length = text != 0 ? strlen(text) : 0u;
    if (out == 0 || capacity < length) {
        return 0u;
    }
    memcpy(out, text, length);
    return length;
}

static size_t winxterm_reply_format(uint8_t *out, size_t capacity, const char *format, int a, int b, int c)
{
    char buffer[WINXTERM_TERMINAL_REPLY_CAPACITY];
    int written = sprintf_s(buffer, sizeof(buffer), format, a, b, c);
    if (written <= 0) {
        return 0u;
    }
    return winxterm_reply_copy(buffer, out, capacity);
}

size_t winxterm_reply_primary_da(uint8_t *out, size_t capacity)
{
    return winxterm_reply_copy("\x1b[?1;2c", out, capacity);
}

size_t winxterm_reply_secondary_da(uint8_t *out, size_t capacity)
{
    return winxterm_reply_copy("\x1b[>0;410;0c", out, capacity);
}

size_t winxterm_reply_dsr_status(uint8_t *out, size_t capacity)
{
    return winxterm_reply_copy("\x1b[0n", out, capacity);
}

size_t winxterm_reply_cpr(int row, int column, uint8_t *out, size_t capacity)
{
    if (row <= 0) {
        row = 1;
    }
    if (column <= 0) {
        column = 1;
    }
    return winxterm_reply_format(out, capacity, "\x1b[%d;%dR", row, column, 0);
}

size_t winxterm_reply_decrqm(char private_marker, int param, int status, uint8_t *out, size_t capacity)
{
    char buffer[WINXTERM_TERMINAL_REPLY_CAPACITY];
    int written = 0;
    if (private_marker == '?') {
        written = sprintf_s(buffer, sizeof(buffer), "\x1b[?%d;%d$y", param, status);
    } else {
        written = sprintf_s(buffer, sizeof(buffer), "\x1b[%d;%d$y", param, status);
    }
    if (written <= 0) {
        return 0u;
    }
    return winxterm_reply_copy(buffer, out, capacity);
}

size_t winxterm_reply_decrqss(const char *request,
                              size_t request_length,
                              bool success,
                              uint8_t *out,
                              size_t capacity)
{
    const char *prefix = success ? "\x1bP1$r" : "\x1bP0$r";
    const char *suffix = "\x1b\\";
    size_t prefix_length = strlen(prefix);
    size_t suffix_length = strlen(suffix);
    size_t total = prefix_length + request_length + suffix_length;
    if (out == 0 || request == 0 || capacity < total) {
        return 0u;
    }
    memcpy(out, prefix, prefix_length);
    memcpy(out + prefix_length, request, request_length);
    memcpy(out + prefix_length + request_length, suffix, suffix_length);
    return total;
}

size_t winxterm_reply_xtversion(uint8_t *out, size_t capacity)
{
    return winxterm_reply_copy("\x1bP>|Winxterm 0.1.0\x1b\\", out, capacity);
}

size_t winxterm_reply_xtqmodkeys(uint8_t *out, size_t capacity)
{
    return winxterm_reply_copy("\x1b[>4;0m", out, capacity);
}

size_t winxterm_reply_xtqfmtkeys(uint8_t *out, size_t capacity)
{
    return winxterm_reply_copy("\x1b[>1;0m", out, capacity);
}
