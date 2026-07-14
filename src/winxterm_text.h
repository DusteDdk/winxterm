#ifndef WINXTERM_TEXT_H
#define WINXTERM_TEXT_H

#include "winxterm_screen.h"
#include "winxterm_terminal_ops.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct WinxtermParserDiagnostics {
    uint32_t unsupported_esc;
    uint32_t unsupported_csi;
    uint32_t unsupported_osc;
    uint32_t denied_osc;
    uint32_t malformed_sequences;
    uint32_t csi_overflow;
    uint32_t string_overflow;
    uint32_t ignored_strings;
    uint32_t replies_emitted;
} WinxtermParserDiagnostics;

typedef struct WinxtermUtf8Decoder {
    uint32_t codepoint;
    int csi_params[WINXTERM_TERMINAL_MAX_PARAMS];
    uint32_t csi_param_count;
    uint32_t csi_param;
    char csi_intermediates[4];
    size_t csi_intermediate_count;
    char string_buffer[WINXTERM_TERMINAL_OSC_PAYLOAD_CAPACITY];
    size_t string_length;
    uint8_t string_kind;
    bool string_overflow;
    uint8_t remaining;
    uint8_t escape_state;
    char csi_private;
    bool csi_has_param;
    bool csi_param_overflow;
    WinxtermParserDiagnostics diagnostics;
} WinxtermUtf8Decoder;

void winxterm_utf8_decoder_init(WinxtermUtf8Decoder *decoder);
const WinxtermParserDiagnostics *winxterm_text_diagnostics(const WinxtermUtf8Decoder *decoder);
void winxterm_text_reset_diagnostics(WinxtermUtf8Decoder *decoder);
bool winxterm_text_feed_bytes(WinxtermScreen *screen,
                              WinxtermUtf8Decoder *decoder,
                              const uint8_t *bytes,
                              size_t byte_count);
bool winxterm_text_feed_bytes_to_sink(WinxtermUtf8Decoder *decoder,
                                      const uint8_t *bytes,
                                      size_t byte_count,
                                      WinxtermTerminalOpSink sink,
                                      void *sink_context);

#endif
