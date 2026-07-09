#ifndef WINXTERM_OPTIONS_H
#define WINXTERM_OPTIONS_H

#include "winxterm_render.h"

#include <stdbool.h>
#include <wchar.h>

#define WINXTERM_DEFAULT_UNPAINTED_LINE_LIMIT 20000u
#define WINXTERM_MAX_RENDER_THREADS 64u

typedef struct WinxtermOptions {
    bool smoke;
    bool help;
    const wchar_t *help_topic;
    bool demo;
    bool glyphbench;
    bool render_backend_set;
    bool render_backend_all;
    WinxtermRenderBackend render_backend;
    unsigned int unpainted_line_limit;
    unsigned int display_scale;
    unsigned int render_thread_count;
    const wchar_t *macro_path;
    const wchar_t *startup_notice;
    int client_arg_start;
    int client_argc;
} WinxtermOptions;

unsigned int winxterm_options_default_render_thread_count(void);
int winxterm_options_parse(int argc, const wchar_t * const *argv, WinxtermOptions *options);

#endif
