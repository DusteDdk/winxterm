#ifndef WINXTERM_INPUT_H
#define WINXTERM_INPUT_H

#include "winxterm_modes.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <windows.h>

#define WINXTERM_INPUT_MAX_SEQUENCE 64u

typedef struct WinxtermInputModifiers {
    bool shift;
    bool ctrl;
    bool alt;
} WinxtermInputModifiers;

size_t winxterm_input_encode_char(wchar_t ch,
                                  WinxtermInputModifiers modifiers,
                                  uint8_t *out,
                                  size_t out_capacity);
size_t winxterm_input_encode_virtual_key(WPARAM virtual_key,
                                         WinxtermInputModifiers modifiers,
                                         uint8_t *out,
                                         size_t out_capacity);
size_t winxterm_input_encode_virtual_key_with_modes(WPARAM virtual_key,
                                                    WinxtermInputModifiers modifiers,
                                                    const WinxtermModeState *modes,
                                                    uint8_t *out,
                                                    size_t out_capacity);

#endif
