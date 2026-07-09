#ifndef WINXTERM_DSTCMD_UNICODE_H
#define WINXTERM_DSTCMD_UNICODE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

bool winxterm_dstcmd_utf8_decode_next(const char *text,
                                      size_t length,
                                      size_t *offset,
                                      uint32_t *codepoint);
size_t winxterm_dstcmd_utf8_prev_boundary(const char *text, size_t length, size_t offset);
size_t winxterm_dstcmd_utf8_next_boundary(const char *text, size_t length, size_t offset);

bool winxterm_dstcmd_codepoint_is_combining(uint32_t codepoint);
int winxterm_dstcmd_codepoint_width(uint32_t codepoint);

bool winxterm_dstcmd_wide_decode_next(const wchar_t *text,
                                      size_t length,
                                      size_t *offset,
                                      uint32_t *codepoint);
size_t winxterm_dstcmd_wide_next_boundary(const wchar_t *text, size_t length, size_t offset);
size_t winxterm_dstcmd_wide_prev_boundary(const wchar_t *text, size_t length, size_t offset);
size_t winxterm_dstcmd_wide_safe_truncate(const wchar_t *text, size_t length, size_t offset);

#endif
