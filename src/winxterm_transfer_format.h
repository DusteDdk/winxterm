#ifndef WINXTERM_TRANSFER_FORMAT_H
#define WINXTERM_TRANSFER_FORMAT_H

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>

void winxterm_transfer_format_bytes(uint64_t bytes, wchar_t *out, size_t out_count);
void winxterm_transfer_format_duration(uint64_t elapsed_ns, wchar_t *out, size_t out_count);
void winxterm_transfer_format_speed(uint64_t bytes, uint64_t elapsed_ns,
                                    wchar_t *out, size_t out_count);

#endif
