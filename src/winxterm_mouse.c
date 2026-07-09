#include "winxterm_mouse.h"

#include <stdio.h>

bool winxterm_mouse_reporting_enabled(const WinxtermModeState *modes)
{
    return modes != 0 &&
           (modes->mouse_x10 || modes->mouse_normal || modes->mouse_button_event ||
            modes->mouse_any_event);
}

bool winxterm_mouse_event_reportable(const WinxtermModeState *modes, const WinxtermMouseEvent *event)
{
    if (!winxterm_mouse_reporting_enabled(modes) || event == 0) {
        return false;
    }
    if (event->kind == WINXTERM_MOUSE_EVENT_WHEEL) {
        return true;
    }
    if (modes->mouse_any_event) {
        return true;
    }
    if (modes->mouse_button_event) {
        return event->kind == WINXTERM_MOUSE_EVENT_PRESS ||
               event->kind == WINXTERM_MOUSE_EVENT_RELEASE ||
               event->kind == WINXTERM_MOUSE_EVENT_DRAG;
    }
    if (modes->mouse_normal) {
        return event->kind == WINXTERM_MOUSE_EVENT_PRESS ||
               event->kind == WINXTERM_MOUSE_EVENT_RELEASE;
    }
    return modes->mouse_x10 && event->kind == WINXTERM_MOUSE_EVENT_PRESS;
}

static int winxterm_mouse_modifier_bits(WinxtermInputModifiers modifiers)
{
    int bits = 0;
    if (modifiers.shift) {
        bits += 4;
    }
    if (modifiers.alt) {
        bits += 8;
    }
    if (modifiers.ctrl) {
        bits += 16;
    }
    return bits;
}

static int winxterm_mouse_button_code(const WinxtermMouseEvent *event)
{
    int code = (int)event->button;
    if (event->kind == WINXTERM_MOUSE_EVENT_RELEASE) {
        code = 3;
    } else if (event->kind == WINXTERM_MOUSE_EVENT_DRAG || event->kind == WINXTERM_MOUSE_EVENT_MOVE) {
        code |= 32;
    }
    return code + winxterm_mouse_modifier_bits(event->modifiers);
}

size_t winxterm_mouse_encode_event(const WinxtermModeState *modes,
                                   const WinxtermMouseEvent *event,
                                   uint8_t *out,
                                   size_t out_capacity)
{
    if (!winxterm_mouse_event_reportable(modes, event) || out == 0 || out_capacity == 0u ||
        event->column < 0 || event->row < 0) {
        return 0u;
    }

    int code = winxterm_mouse_button_code(event);
    int x = event->column + 1;
    int y = event->row + 1;

    if (modes->mouse_sgr) {
        char suffix = event->kind == WINXTERM_MOUSE_EVENT_RELEASE ? 'm' : 'M';
        int written = sprintf_s((char *)out, out_capacity, "\x1b[<%d;%d;%d%c", code, x, y, suffix);
        return written > 0 ? (size_t)written : 0u;
    }

    if (x > 223 || y > 223 || out_capacity < 6u) {
        return 0u;
    }
    out[0] = 0x1bu;
    out[1] = '[';
    out[2] = 'M';
    out[3] = (uint8_t)(code + 32);
    out[4] = (uint8_t)(x + 32);
    out[5] = (uint8_t)(y + 32);
    return 6u;
}
