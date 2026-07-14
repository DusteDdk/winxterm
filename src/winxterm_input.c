#include "winxterm_input.h"

#include <stdio.h>
#include <string.h>

static size_t winxterm_input_append_byte(uint8_t *out, size_t out_capacity, size_t offset, uint8_t byte)
{
    if (offset >= out_capacity) {
        return 0u;
    }
    out[offset++] = byte;
    return offset;
}

static size_t winxterm_input_append_ascii(uint8_t *out,
                                          size_t out_capacity,
                                          size_t offset,
                                          const char *text)
{
    while (text != 0 && *text != '\0') {
        offset = winxterm_input_append_byte(out, out_capacity, offset, (uint8_t)*text++);
        if (offset == 0u) {
            return 0u;
        }
    }
    return offset;
}

static size_t winxterm_input_encode_utf8(uint32_t codepoint,
                                         uint8_t *out,
                                         size_t out_capacity,
                                         size_t offset)
{
    if (codepoint <= 0x7fu) {
        return winxterm_input_append_byte(out, out_capacity, offset, (uint8_t)codepoint);
    }
    if (codepoint <= 0x7ffu) {
        offset = winxterm_input_append_byte(out, out_capacity, offset, (uint8_t)(0xc0u | (codepoint >> 6)));
        return offset == 0u ? 0u :
            winxterm_input_append_byte(out, out_capacity, offset, (uint8_t)(0x80u | (codepoint & 0x3fu)));
    }
    if (codepoint <= 0xffffu) {
        offset = winxterm_input_append_byte(out, out_capacity, offset, (uint8_t)(0xe0u | (codepoint >> 12)));
        offset = offset == 0u ? 0u :
            winxterm_input_append_byte(out, out_capacity, offset, (uint8_t)(0x80u | ((codepoint >> 6) & 0x3fu)));
        return offset == 0u ? 0u :
            winxterm_input_append_byte(out, out_capacity, offset, (uint8_t)(0x80u | (codepoint & 0x3fu)));
    }
    if (codepoint <= 0x10ffffu) {
        offset = winxterm_input_append_byte(out, out_capacity, offset, (uint8_t)(0xf0u | (codepoint >> 18)));
        offset = offset == 0u ? 0u :
            winxterm_input_append_byte(out, out_capacity, offset, (uint8_t)(0x80u | ((codepoint >> 12) & 0x3fu)));
        offset = offset == 0u ? 0u :
            winxterm_input_append_byte(out, out_capacity, offset, (uint8_t)(0x80u | ((codepoint >> 6) & 0x3fu)));
        return offset == 0u ? 0u :
            winxterm_input_append_byte(out, out_capacity, offset, (uint8_t)(0x80u | (codepoint & 0x3fu)));
    }
    return 0u;
}

size_t winxterm_input_encode_char(wchar_t ch,
                                  WinxtermInputModifiers modifiers,
                                  uint8_t *out,
                                  size_t out_capacity)
{
    if (out == 0 || out_capacity == 0u) {
        return 0u;
    }

    size_t offset = 0u;
    if (modifiers.alt) {
        offset = winxterm_input_append_byte(out, out_capacity, offset, 0x1bu);
        if (offset == 0u) {
            return 0u;
        }
    }

    if (ch == L'\r') {
        return winxterm_input_append_byte(out, out_capacity, offset, '\r');
    }
    if (ch == L'\b') {
        return winxterm_input_append_byte(out, out_capacity, offset, 0x7fu);
    }
    if (ch == L'\t') {
        return winxterm_input_append_byte(out, out_capacity, offset, '\t');
    }
    if (ch == 0x1bu) {
        return winxterm_input_append_byte(out, out_capacity, offset, 0x1bu);
    }
    if (ch < 0x20) {
        return winxterm_input_append_byte(out, out_capacity, offset, (uint8_t)ch);
    }

    if (modifiers.ctrl && ch >= L'@' && ch <= L'_') {
        return winxterm_input_append_byte(out, out_capacity, offset, (uint8_t)(ch - L'@'));
    }
    if (modifiers.ctrl && ch >= L'a' && ch <= L'z') {
        return winxterm_input_append_byte(out, out_capacity, offset, (uint8_t)(ch - L'a' + 1));
    }

    return winxterm_input_encode_utf8((uint32_t)ch, out, out_capacity, offset);
}

static int winxterm_input_modifier_param(WinxtermInputModifiers modifiers)
{
    int value = 1;
    if (modifiers.shift) {
        value += 1;
    }
    if (modifiers.alt) {
        value += 2;
    }
    if (modifiers.ctrl) {
        value += 4;
    }
    return value;
}

static size_t winxterm_input_append_csi_modifier(uint8_t *out,
                                                 size_t out_capacity,
                                                 const char *prefix,
                                                 int key_code,
                                                 char suffix,
                                                 WinxtermInputModifiers modifiers)
{
    char buffer[WINXTERM_INPUT_MAX_SEQUENCE];
    int modifier = winxterm_input_modifier_param(modifiers);
    int written = 0;
    if (modifier == 1) {
        written = key_code == 0 ?
            sprintf_s(buffer, sizeof(buffer), "\x1b[%s%c", prefix, suffix) :
            sprintf_s(buffer, sizeof(buffer), "\x1b[%s%d%c", prefix, key_code, suffix);
    } else {
        written = key_code == 0 ?
            sprintf_s(buffer, sizeof(buffer), "\x1b[%s;%d%c", prefix, modifier, suffix) :
            sprintf_s(buffer, sizeof(buffer), "\x1b[%s%d;%d%c", prefix, key_code, modifier, suffix);
    }
    if (written <= 0) {
        return 0u;
    }
    return winxterm_input_append_ascii(out, out_capacity, 0u, buffer);
}

size_t winxterm_input_encode_virtual_key(WPARAM virtual_key,
                                         WinxtermInputModifiers modifiers,
                                         uint8_t *out,
                                         size_t out_capacity)
{
    return winxterm_input_encode_virtual_key_with_modes(virtual_key, modifiers, 0, out, out_capacity);
}

static const char *winxterm_input_application_keypad_sequence(WPARAM virtual_key)
{
    switch (virtual_key) {
    case VK_NUMPAD0:
        return "\x1bOp";
    case VK_NUMPAD1:
        return "\x1bOq";
    case VK_NUMPAD2:
        return "\x1bOr";
    case VK_NUMPAD3:
        return "\x1bOs";
    case VK_NUMPAD4:
        return "\x1bOt";
    case VK_NUMPAD5:
    case VK_CLEAR:
        return "\x1bOu";
    case VK_NUMPAD6:
        return "\x1bOv";
    case VK_NUMPAD7:
        return "\x1bOw";
    case VK_NUMPAD8:
        return "\x1bOx";
    case VK_NUMPAD9:
        return "\x1bOy";
    case VK_DECIMAL:
        return "\x1bOn";
    case VK_MULTIPLY:
        return "\x1bOj";
    case VK_ADD:
        return "\x1bOk";
    case VK_SUBTRACT:
        return "\x1bOm";
    case VK_DIVIDE:
        return "\x1bOo";
    default:
        return 0;
    }
}

size_t winxterm_input_encode_virtual_key_with_modes(WPARAM virtual_key,
                                                    WinxtermInputModifiers modifiers,
                                                    const WinxtermModeState *modes,
                                                    uint8_t *out,
                                                    size_t out_capacity)
{
    if (out == 0 || out_capacity == 0u) {
        return 0u;
    }

    bool application_cursor =
        modes != 0 && winxterm_mode_state_enabled(modes, WINXTERM_TERMINAL_MODE_APPLICATION_CURSOR);
    bool application_keypad =
        modes != 0 && winxterm_mode_state_enabled(modes, WINXTERM_TERMINAL_MODE_APPLICATION_KEYPAD);
    if (application_keypad && winxterm_input_modifier_param(modifiers) == 1) {
        const char *keypad = winxterm_input_application_keypad_sequence(virtual_key);
        if (keypad != 0) {
            return winxterm_input_append_ascii(out, out_capacity, 0u, keypad);
        }
    }
    if (application_cursor && winxterm_input_modifier_param(modifiers) == 1) {
        switch (virtual_key) {
        case VK_UP:
            return winxterm_input_append_ascii(out, out_capacity, 0u, "\x1bOA");
        case VK_DOWN:
            return winxterm_input_append_ascii(out, out_capacity, 0u, "\x1bOB");
        case VK_RIGHT:
            return winxterm_input_append_ascii(out, out_capacity, 0u, "\x1bOC");
        case VK_LEFT:
            return winxterm_input_append_ascii(out, out_capacity, 0u, "\x1bOD");
        default:
            break;
        }
    }

    switch (virtual_key) {
    case VK_BACK:
        if (modifiers.ctrl) {
            size_t offset = modifiers.alt ? winxterm_input_append_byte(out, out_capacity, 0u, 0x1bu) : 0u;
            return offset == 0u && modifiers.alt ?
                0u : winxterm_input_append_byte(out, out_capacity, offset, 0x17u);
        }
        break;
    case VK_UP:
        return winxterm_input_append_csi_modifier(out, out_capacity, "1", 0, 'A', modifiers);
    case VK_DOWN:
        return winxterm_input_append_csi_modifier(out, out_capacity, "1", 0, 'B', modifiers);
    case VK_RIGHT:
        return winxterm_input_append_csi_modifier(out, out_capacity, "1", 0, 'C', modifiers);
    case VK_LEFT:
        return winxterm_input_append_csi_modifier(out, out_capacity, "1", 0, 'D', modifiers);
    case VK_HOME:
        return winxterm_input_append_csi_modifier(out, out_capacity, "1", 0, 'H', modifiers);
    case VK_END:
        return winxterm_input_append_csi_modifier(out, out_capacity, "1", 0, 'F', modifiers);
    case VK_INSERT:
        return winxterm_input_append_csi_modifier(out, out_capacity, "", 2, '~', modifiers);
    case VK_DELETE:
        return winxterm_input_append_csi_modifier(out, out_capacity, "", 3, '~', modifiers);
    case VK_PRIOR:
        return winxterm_input_append_csi_modifier(out, out_capacity, "", 5, '~', modifiers);
    case VK_NEXT:
        return winxterm_input_append_csi_modifier(out, out_capacity, "", 6, '~', modifiers);
    case VK_F1:
        return winxterm_input_append_csi_modifier(out, out_capacity, "", 11, '~', modifiers);
    case VK_F2:
        return winxterm_input_append_csi_modifier(out, out_capacity, "", 12, '~', modifiers);
    case VK_F3:
        return winxterm_input_append_csi_modifier(out, out_capacity, "", 13, '~', modifiers);
    case VK_F4:
        return winxterm_input_append_csi_modifier(out, out_capacity, "", 14, '~', modifiers);
    case VK_F5:
        return winxterm_input_append_csi_modifier(out, out_capacity, "", 15, '~', modifiers);
    case VK_F6:
        return winxterm_input_append_csi_modifier(out, out_capacity, "", 17, '~', modifiers);
    case VK_F7:
        return winxterm_input_append_csi_modifier(out, out_capacity, "", 18, '~', modifiers);
    case VK_F8:
        return winxterm_input_append_csi_modifier(out, out_capacity, "", 19, '~', modifiers);
    case VK_F9:
        return winxterm_input_append_csi_modifier(out, out_capacity, "", 20, '~', modifiers);
    case VK_F10:
        return winxterm_input_append_csi_modifier(out, out_capacity, "", 21, '~', modifiers);
    case VK_F11:
        return winxterm_input_append_csi_modifier(out, out_capacity, "", 23, '~', modifiers);
    case VK_F12:
        return winxterm_input_append_csi_modifier(out, out_capacity, "", 24, '~', modifiers);
    default:
        break;
    }

    return 0u;
}
