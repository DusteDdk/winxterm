#ifndef WINXTERM_DSTCMD_PARSE_H
#define WINXTERM_DSTCMD_PARSE_H

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

typedef struct WinxtermDstcmdScratch WinxtermDstcmdScratch;

typedef struct WinxtermDstcmdArgv {
    wchar_t **items;
    int count;
} WinxtermDstcmdArgv;

bool winxterm_dstcmd_parse_line(const wchar_t *line,
                                WinxtermDstcmdArgv *argv,
                                wchar_t *error,
                                size_t error_count);
bool winxterm_dstcmd_parse_line_expanding_globs(const wchar_t *line,
                                                const wchar_t *cwd,
                                                WinxtermDstcmdArgv *argv,
                                                wchar_t *error,
                                                size_t error_count);
bool winxterm_dstcmd_parse_line_expanding_globs_and_status(const wchar_t *line,
                                                           const wchar_t *cwd,
                                                           int last_status,
                                                           WinxtermDstcmdArgv *argv,
                                                           wchar_t *error,
                                                           size_t error_count);
bool winxterm_dstcmd_parse_line_expanding_globs_and_status_scratch(WinxtermDstcmdScratch *scratch,
                                                                   const wchar_t *line,
                                                                   const wchar_t *cwd,
                                                                   int last_status,
                                                                   WinxtermDstcmdArgv *argv,
                                                                   wchar_t *error,
                                                                   size_t error_count);
bool winxterm_dstcmd_parse_single_token_expanding_status_scratch(WinxtermDstcmdScratch *scratch,
                                                                 const wchar_t *line,
                                                                 int last_status,
                                                                 wchar_t **out,
                                                                 wchar_t *error,
                                                                 size_t error_count);
void winxterm_dstcmd_argv_dispose(WinxtermDstcmdArgv *argv);
wchar_t *winxterm_dstcmd_wcsdup(const wchar_t *text);

#endif
