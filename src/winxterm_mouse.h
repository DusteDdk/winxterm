#ifndef WINXTERM_MOUSE_H
#define WINXTERM_MOUSE_H

#include "winxterm_input.h"
#include "winxterm_modes.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum WinxtermMouseButton {
    WINXTERM_MOUSE_BUTTON_LEFT = 0,
    WINXTERM_MOUSE_BUTTON_MIDDLE = 1,
    WINXTERM_MOUSE_BUTTON_RIGHT = 2,
    WINXTERM_MOUSE_BUTTON_RELEASE = 3,
    WINXTERM_MOUSE_WHEEL_UP = 64,
    WINXTERM_MOUSE_WHEEL_DOWN = 65
} WinxtermMouseButton;

typedef enum WinxtermMouseEventKind {
    WINXTERM_MOUSE_EVENT_PRESS,
    WINXTERM_MOUSE_EVENT_RELEASE,
    WINXTERM_MOUSE_EVENT_DRAG,
    WINXTERM_MOUSE_EVENT_MOVE,
    WINXTERM_MOUSE_EVENT_WHEEL
} WinxtermMouseEventKind;

typedef struct WinxtermMouseEvent {
    WinxtermMouseEventKind kind;
    WinxtermMouseButton button;
    int column;
    int row;
    WinxtermInputModifiers modifiers;
} WinxtermMouseEvent;

bool winxterm_mouse_reporting_enabled(const WinxtermModeState *modes);
bool winxterm_mouse_event_reportable(const WinxtermModeState *modes, const WinxtermMouseEvent *event);
size_t winxterm_mouse_encode_event(const WinxtermModeState *modes,
                                   const WinxtermMouseEvent *event,
                                   uint8_t *out,
                                   size_t out_capacity);

#endif
