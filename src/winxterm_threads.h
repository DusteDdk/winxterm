#ifndef WINXTERM_THREADS_H
#define WINXTERM_THREADS_H

#include "winxterm_log.h"
#include "winxterm_options.h"

#include <windows.h>

int winxterm_threads_run(HINSTANCE instance,
                         WinxtermLog *log,
                         const WinxtermOptions *options,
                         const wchar_t * const *argv);

#endif
