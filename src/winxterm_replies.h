#ifndef WINXTERM_REPLIES_H
#define WINXTERM_REPLIES_H

#include "winxterm_terminal_ops.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

size_t winxterm_reply_primary_da(uint8_t *out, size_t capacity);
size_t winxterm_reply_secondary_da(uint8_t *out, size_t capacity);
size_t winxterm_reply_dsr_status(uint8_t *out, size_t capacity);
size_t winxterm_reply_cpr(int row, int column, uint8_t *out, size_t capacity);
size_t winxterm_reply_decrqm(char private_marker, int param, int status, uint8_t *out, size_t capacity);
size_t winxterm_reply_decrqss(const char *request,
                              size_t request_length,
                              bool success,
                              uint8_t *out,
                              size_t capacity);
size_t winxterm_reply_xtversion(uint8_t *out, size_t capacity);
size_t winxterm_reply_xtqmodkeys(uint8_t *out, size_t capacity);
size_t winxterm_reply_xtqfmtkeys(uint8_t *out, size_t capacity);

#endif
