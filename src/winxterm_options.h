#ifndef WINXTERM_OPTIONS_H
#define WINXTERM_OPTIONS_H

#include <stdbool.h>
#include <wchar.h>

#define WINXTERM_DEFAULT_UNPAINTED_LINE_LIMIT 20000u

typedef struct WinxtermOptions {
    bool smoke;
    bool help;
    const wchar_t *help_topic;
    bool demo;
    bool glyphbench;
    unsigned int unpainted_line_limit;
    unsigned int display_scale;
    const wchar_t *macro_path;
    const wchar_t *startup_notice;
    int client_arg_start;
    int client_argc;
} WinxtermOptions;

int winxterm_options_parse(int argc, const wchar_t * const *argv, WinxtermOptions *options);

#endif
