#include "winxterm_modes.h"

#include <string.h>

void winxterm_mode_state_reset_hard(WinxtermModeState *state)
{
    if (state == 0) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->auto_wrap = true;
    state->cursor_visible = true;
}

void winxterm_mode_state_reset_soft(WinxtermModeState *state)
{
    winxterm_mode_state_reset_hard(state);
}

bool winxterm_mode_state_set(WinxtermModeState *state, WinxtermTerminalMode mode, bool enabled)
{
    if (state == 0) {
        return false;
    }
    switch (mode) {
    case WINXTERM_TERMINAL_MODE_APPLICATION_CURSOR:
        state->application_cursor = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_APPLICATION_KEYPAD:
        state->application_keypad = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_REVERSE_VIDEO:
        state->reverse_video = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_ORIGIN:
        state->origin = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_AUTOWRAP:
        state->auto_wrap = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_CURSOR_VISIBLE:
        state->cursor_visible = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_REVERSE_WRAP:
        state->reverse_wrap = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_LEFT_RIGHT_MARGIN:
        state->left_right_margin = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_ALT_SCREEN:
        state->alt_screen_47 = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_ALT_SCREEN_1047:
        state->alt_screen_1047 = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_SAVE_CURSOR_1048:
        state->save_cursor_1048 = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_ALT_SCREEN_1049:
        state->alt_screen_1049 = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_BRACKETED_PASTE:
        state->bracketed_paste = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_FOCUS_REPORT:
        state->focus_report = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_MOUSE_X10:
        state->mouse_x10 = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_MOUSE_NORMAL:
        state->mouse_normal = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_MOUSE_BUTTON_EVENT:
        state->mouse_button_event = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_MOUSE_ANY_EVENT:
        state->mouse_any_event = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_MOUSE_SGR:
        state->mouse_sgr = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_SYNCHRONIZED_OUTPUT:
        state->synchronized_output = enabled;
        return true;
    case WINXTERM_TERMINAL_MODE_EIGHT_BIT_CONTROLS:
        state->eight_bit_controls = enabled;
        return true;
    default:
        return false;
    }
}

bool winxterm_mode_state_enabled(const WinxtermModeState *state, WinxtermTerminalMode mode)
{
    if (state == 0) {
        return false;
    }
    switch (mode) {
    case WINXTERM_TERMINAL_MODE_APPLICATION_CURSOR:
        return state->application_cursor;
    case WINXTERM_TERMINAL_MODE_APPLICATION_KEYPAD:
        return state->application_keypad;
    case WINXTERM_TERMINAL_MODE_REVERSE_VIDEO:
        return state->reverse_video;
    case WINXTERM_TERMINAL_MODE_ORIGIN:
        return state->origin;
    case WINXTERM_TERMINAL_MODE_AUTOWRAP:
        return state->auto_wrap;
    case WINXTERM_TERMINAL_MODE_CURSOR_VISIBLE:
        return state->cursor_visible;
    case WINXTERM_TERMINAL_MODE_REVERSE_WRAP:
        return state->reverse_wrap;
    case WINXTERM_TERMINAL_MODE_LEFT_RIGHT_MARGIN:
        return state->left_right_margin;
    case WINXTERM_TERMINAL_MODE_ALT_SCREEN:
        return state->alt_screen_47;
    case WINXTERM_TERMINAL_MODE_ALT_SCREEN_1047:
        return state->alt_screen_1047;
    case WINXTERM_TERMINAL_MODE_SAVE_CURSOR_1048:
        return state->save_cursor_1048;
    case WINXTERM_TERMINAL_MODE_ALT_SCREEN_1049:
        return state->alt_screen_1049;
    case WINXTERM_TERMINAL_MODE_BRACKETED_PASTE:
        return state->bracketed_paste;
    case WINXTERM_TERMINAL_MODE_FOCUS_REPORT:
        return state->focus_report;
    case WINXTERM_TERMINAL_MODE_MOUSE_X10:
        return state->mouse_x10;
    case WINXTERM_TERMINAL_MODE_MOUSE_NORMAL:
        return state->mouse_normal;
    case WINXTERM_TERMINAL_MODE_MOUSE_BUTTON_EVENT:
        return state->mouse_button_event;
    case WINXTERM_TERMINAL_MODE_MOUSE_ANY_EVENT:
        return state->mouse_any_event;
    case WINXTERM_TERMINAL_MODE_MOUSE_SGR:
        return state->mouse_sgr;
    case WINXTERM_TERMINAL_MODE_SYNCHRONIZED_OUTPUT:
        return state->synchronized_output;
    case WINXTERM_TERMINAL_MODE_EIGHT_BIT_CONTROLS:
        return state->eight_bit_controls;
    default:
        return false;
    }
}

bool winxterm_mode_from_csi(char private_marker, int param, WinxtermTerminalMode *mode)
{
    if (mode == 0 || private_marker != '?') {
        return false;
    }
    switch (param) {
    case 1:
        *mode = WINXTERM_TERMINAL_MODE_APPLICATION_CURSOR;
        return true;
    case 5:
        *mode = WINXTERM_TERMINAL_MODE_REVERSE_VIDEO;
        return true;
    case 6:
        *mode = WINXTERM_TERMINAL_MODE_ORIGIN;
        return true;
    case 7:
        *mode = WINXTERM_TERMINAL_MODE_AUTOWRAP;
        return true;
    case 9:
        *mode = WINXTERM_TERMINAL_MODE_MOUSE_X10;
        return true;
    case 25:
        *mode = WINXTERM_TERMINAL_MODE_CURSOR_VISIBLE;
        return true;
    case 45:
        *mode = WINXTERM_TERMINAL_MODE_REVERSE_WRAP;
        return true;
    case 69:
        *mode = WINXTERM_TERMINAL_MODE_LEFT_RIGHT_MARGIN;
        return true;
    case 47:
        *mode = WINXTERM_TERMINAL_MODE_ALT_SCREEN;
        return true;
    case 1000:
        *mode = WINXTERM_TERMINAL_MODE_MOUSE_NORMAL;
        return true;
    case 1002:
        *mode = WINXTERM_TERMINAL_MODE_MOUSE_BUTTON_EVENT;
        return true;
    case 1003:
        *mode = WINXTERM_TERMINAL_MODE_MOUSE_ANY_EVENT;
        return true;
    case 1004:
        *mode = WINXTERM_TERMINAL_MODE_FOCUS_REPORT;
        return true;
    case 1006:
        *mode = WINXTERM_TERMINAL_MODE_MOUSE_SGR;
        return true;
    case 1047:
        *mode = WINXTERM_TERMINAL_MODE_ALT_SCREEN_1047;
        return true;
    case 1048:
        *mode = WINXTERM_TERMINAL_MODE_SAVE_CURSOR_1048;
        return true;
    case 1049:
        *mode = WINXTERM_TERMINAL_MODE_ALT_SCREEN_1049;
        return true;
    case 2004:
        *mode = WINXTERM_TERMINAL_MODE_BRACKETED_PASTE;
        return true;
    case 2026:
        *mode = WINXTERM_TERMINAL_MODE_SYNCHRONIZED_OUTPUT;
        return true;
    default:
        return false;
    }
}

int winxterm_mode_decrqm_status(const WinxtermModeState *state, WinxtermTerminalMode mode)
{
    if (state == 0) {
        return 0;
    }
    return winxterm_mode_state_enabled(state, mode) ? 1 : 2;
}

const char *winxterm_mode_name(WinxtermTerminalMode mode)
{
    switch (mode) {
    case WINXTERM_TERMINAL_MODE_APPLICATION_CURSOR: return "application-cursor";
    case WINXTERM_TERMINAL_MODE_APPLICATION_KEYPAD: return "application-keypad";
    case WINXTERM_TERMINAL_MODE_REVERSE_VIDEO: return "reverse-video";
    case WINXTERM_TERMINAL_MODE_ORIGIN: return "origin";
    case WINXTERM_TERMINAL_MODE_AUTOWRAP: return "autowrap";
    case WINXTERM_TERMINAL_MODE_CURSOR_VISIBLE: return "cursor-visible";
    case WINXTERM_TERMINAL_MODE_REVERSE_WRAP: return "reverse-wrap";
    case WINXTERM_TERMINAL_MODE_LEFT_RIGHT_MARGIN: return "left-right-margin";
    case WINXTERM_TERMINAL_MODE_ALT_SCREEN: return "alternate-screen-47";
    case WINXTERM_TERMINAL_MODE_ALT_SCREEN_1047: return "alternate-screen-1047";
    case WINXTERM_TERMINAL_MODE_SAVE_CURSOR_1048: return "save-cursor-1048";
    case WINXTERM_TERMINAL_MODE_ALT_SCREEN_1049: return "alternate-screen-1049";
    case WINXTERM_TERMINAL_MODE_BRACKETED_PASTE: return "bracketed-paste";
    case WINXTERM_TERMINAL_MODE_FOCUS_REPORT: return "focus-report";
    case WINXTERM_TERMINAL_MODE_MOUSE_X10: return "mouse-x10";
    case WINXTERM_TERMINAL_MODE_MOUSE_NORMAL: return "mouse-normal";
    case WINXTERM_TERMINAL_MODE_MOUSE_BUTTON_EVENT: return "mouse-button-event";
    case WINXTERM_TERMINAL_MODE_MOUSE_ANY_EVENT: return "mouse-any-event";
    case WINXTERM_TERMINAL_MODE_MOUSE_SGR: return "mouse-sgr";
    case WINXTERM_TERMINAL_MODE_SYNCHRONIZED_OUTPUT: return "synchronized-output";
    case WINXTERM_TERMINAL_MODE_EIGHT_BIT_CONTROLS: return "eight-bit-controls";
    default: return "unknown";
    }
}
