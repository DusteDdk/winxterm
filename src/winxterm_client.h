#ifndef WINXTERM_CLIENT_H
#define WINXTERM_CLIENT_H

#include "winxterm_bridge.h"
#include "winxterm_text.h"

#include <stddef.h>
#include <stdint.h>
#include <wchar.h>
#include <windows.h>

bool winxterm_client_write_bytes(WinxtermBridge *bridge,
                                 WinxtermUtf8Decoder *decoder,
                                 const uint8_t *bytes,
                                 size_t byte_count);
bool winxterm_client_write_bytes_with_policy(WinxtermBridge *bridge,
                                             WinxtermUtf8Decoder *decoder,
                                             const uint8_t *bytes,
                                             size_t byte_count,
                                             HANDLE shutdown_event,
                                             bool wait_for_unpainted_budget);
DWORD winxterm_client_run_demo(WinxtermBridge *bridge, HANDLE shutdown_event);
DWORD winxterm_client_run_process(WinxtermBridge *bridge,
                                  const wchar_t * const *argv,
                                  int argc,
                                  HANDLE shutdown_event);
DWORD winxterm_client_run_process_in_directory(WinxtermBridge *bridge,
                                               const wchar_t * const *argv,
                                               int argc,
                                               const wchar_t *current_directory,
                                               HANDLE shutdown_event);
int winxterm_glyphbench_run(WinxtermLog *log);

#endif
