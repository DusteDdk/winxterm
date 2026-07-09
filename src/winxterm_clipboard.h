#ifndef WINXTERM_CLIPBOARD_H
#define WINXTERM_CLIPBOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <windows.h>

bool winxterm_clipboard_get_text_utf8(HWND owner, char **out_text, size_t *out_length);
bool winxterm_clipboard_set_text_utf8(HWND owner, const char *text, size_t length);
bool winxterm_clipboard_prepare_paste(const char *text,
                                      size_t length,
                                      bool bracketed,
                                      char **out_text,
                                      size_t *out_length);

#endif
