#ifndef WINXTERM_HOST_H
#define WINXTERM_HOST_H

#include "winxterm_bridge.h"

#include <stdbool.h>
#include <wchar.h>
#include <windows.h>

DWORD winxterm_host_run_conpty(WinxtermBridge *bridge,
                               const wchar_t * const *argv,
                               int argc,
                               HANDLE shutdown_event);
DWORD winxterm_host_run_conpty_in_directory(WinxtermBridge *bridge,
                                            const wchar_t * const *argv,
                                            int argc,
                                            const wchar_t *current_directory,
                                            HANDLE shutdown_event);

#endif
