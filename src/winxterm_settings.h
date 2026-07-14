#ifndef WINXTERM_SETTINGS_H
#define WINXTERM_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#define WINXTERM_SETTINGS_FILENAME L"settings.rc"

typedef struct WinxtermSettings {
    bool scrollbar;
} WinxtermSettings;

void winxterm_settings_init(WinxtermSettings *settings);
bool winxterm_settings_parse(const char *text, WinxtermSettings *settings);
bool winxterm_settings_format(const WinxtermSettings *settings, char *buffer, size_t buffer_count);
bool winxterm_settings_load(WinxtermSettings *settings);
bool winxterm_settings_save(const WinxtermSettings *settings);

#endif
