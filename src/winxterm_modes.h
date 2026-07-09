#ifndef WINXTERM_MODES_H
#define WINXTERM_MODES_H

#include "winxterm_terminal_ops.h"

#include <stdbool.h>

typedef struct WinxtermModeState {
    bool application_cursor;
    bool application_keypad;
    bool origin;
    bool auto_wrap;
    bool cursor_visible;
    bool reverse_video;
    bool reverse_wrap;
    bool left_right_margin;
    bool alt_screen_47;
    bool alt_screen_1047;
    bool save_cursor_1048;
    bool alt_screen_1049;
    bool bracketed_paste;
    bool focus_report;
    bool mouse_x10;
    bool mouse_normal;
    bool mouse_button_event;
    bool mouse_any_event;
    bool mouse_sgr;
    bool synchronized_output;
    bool eight_bit_controls;
} WinxtermModeState;

void winxterm_mode_state_reset_hard(WinxtermModeState *state);
void winxterm_mode_state_reset_soft(WinxtermModeState *state);
bool winxterm_mode_state_set(WinxtermModeState *state, WinxtermTerminalMode mode, bool enabled);
bool winxterm_mode_state_enabled(const WinxtermModeState *state, WinxtermTerminalMode mode);
bool winxterm_mode_from_csi(char private_marker, int param, WinxtermTerminalMode *mode);
int winxterm_mode_decrqm_status(const WinxtermModeState *state, WinxtermTerminalMode mode);
const char *winxterm_mode_name(WinxtermTerminalMode mode);

#endif
