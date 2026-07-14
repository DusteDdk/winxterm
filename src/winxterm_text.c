#include "winxterm_text.h"

#include "winxterm_modes.h"

#include <stdbool.h>
#include <string.h>

#define WINXTERM_TEXT_ESCAPE_NONE 0u
#define WINXTERM_TEXT_ESCAPE_ESC 1u
#define WINXTERM_TEXT_ESCAPE_CSI 2u
#define WINXTERM_TEXT_ESCAPE_STRING 3u
#define WINXTERM_TEXT_ESCAPE_STRING_ESC 4u
#define WINXTERM_TEXT_ESCAPE_ESC_HASH 5u
#define WINXTERM_TEXT_ESCAPE_ESC_SPACE 6u
#define WINXTERM_TEXT_ESCAPE_ESC_CHARSET_G0 7u

#define WINXTERM_TEXT_STRING_OSC 1u
#define WINXTERM_TEXT_STRING_DCS 2u
#define WINXTERM_TEXT_STRING_APC 3u
#define WINXTERM_TEXT_STRING_PM 4u
#define WINXTERM_TEXT_STRING_SOS 5u

static void winxterm_text_csi_reset(WinxtermUtf8Decoder *decoder);

void winxterm_utf8_decoder_init(WinxtermUtf8Decoder *decoder)
{
    if (decoder != 0) {
        memset(decoder, 0, sizeof(*decoder));
        winxterm_text_csi_reset(decoder);
    }
}

const WinxtermParserDiagnostics *winxterm_text_diagnostics(const WinxtermUtf8Decoder *decoder)
{
    return decoder != 0 ? &decoder->diagnostics : 0;
}

void winxterm_text_reset_diagnostics(WinxtermUtf8Decoder *decoder)
{
    if (decoder != 0) {
        memset(&decoder->diagnostics, 0, sizeof(decoder->diagnostics));
    }
}

static bool winxterm_text_emit(WinxtermTerminalOpSink sink,
                               void *sink_context,
                               const WinxtermTerminalOp *op)
{
    return sink != 0 && op != 0 && sink(sink_context, op);
}

static bool winxterm_text_emit_control(WinxtermTerminalOpSink sink,
                                       void *sink_context,
                                       WinxtermTerminalControl control)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = WINXTERM_TERMINAL_OP_CONTROL;
    op.data.control = control;
    return winxterm_text_emit(sink, sink_context, &op);
}

static bool winxterm_text_emit_count(WinxtermTerminalOpSink sink,
                                     void *sink_context,
                                     WinxtermTerminalOpType type,
                                     int count)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = type;
    op.data.count = count <= 0 ? 1 : count;
    return winxterm_text_emit(sink, sink_context, &op);
}

static bool winxterm_text_emit_simple(WinxtermTerminalOpSink sink,
                                      void *sink_context,
                                      WinxtermTerminalOpType type)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = type;
    return winxterm_text_emit(sink, sink_context, &op);
}

static bool winxterm_text_emit_tab_clear(WinxtermTerminalOpSink sink,
                                         void *sink_context,
                                         int mode)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = WINXTERM_TERMINAL_OP_TAB_CLEAR;
    op.data.tab_clear.mode = (mode == 3) ?
        WINXTERM_TERMINAL_TAB_CLEAR_ALL : WINXTERM_TERMINAL_TAB_CLEAR_CURRENT;
    return winxterm_text_emit(sink, sink_context, &op);
}

static bool winxterm_text_emit_charset(WinxtermTerminalOpSink sink,
                                       void *sink_context,
                                       int slot,
                                       WinxtermTerminalCharset charset)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = WINXTERM_TERMINAL_OP_SET_CHARSET;
    op.data.charset.slot = slot;
    op.data.charset.charset = charset;
    return winxterm_text_emit(sink, sink_context, &op);
}

static bool winxterm_text_emit_rgb(WinxtermTerminalOpSink sink,
                                   void *sink_context,
                                   bool foreground,
                                   uint32_t rgb)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = foreground ? WINXTERM_TERMINAL_OP_SET_FOREGROUND : WINXTERM_TERMINAL_OP_SET_BACKGROUND;
    op.data.rgb = rgb & 0x00ffffffu;
    return winxterm_text_emit(sink, sink_context, &op);
}

static bool winxterm_text_emit_color_index(WinxtermTerminalOpSink sink,
                                           void *sink_context,
                                           bool foreground,
                                           int index)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = foreground ? WINXTERM_TERMINAL_OP_SET_FOREGROUND_INDEX :
                           WINXTERM_TERMINAL_OP_SET_BACKGROUND_INDEX;
    op.data.count = index;
    return winxterm_text_emit(sink, sink_context, &op);
}

static bool winxterm_text_emit_default_color(WinxtermTerminalOpSink sink,
                                             void *sink_context,
                                             bool foreground)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = foreground ? WINXTERM_TERMINAL_OP_RESET_FOREGROUND : WINXTERM_TERMINAL_OP_RESET_BACKGROUND;
    return winxterm_text_emit(sink, sink_context, &op);
}

static bool winxterm_text_emit_attribute_flags(WinxtermTerminalOpSink sink,
                                               void *sink_context,
                                               bool set_flags,
                                               uint32_t flags)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = set_flags ? WINXTERM_TERMINAL_OP_SET_TEXT_ATTRIBUTES :
                          WINXTERM_TERMINAL_OP_RESET_TEXT_ATTRIBUTES;
    op.data.flags = flags;
    return winxterm_text_emit(sink, sink_context, &op);
}

static int winxterm_text_param_or_default(const WinxtermUtf8Decoder *decoder,
                                          uint32_t index,
                                          int default_value)
{
    if (decoder == 0 || index >= decoder->csi_param_count || decoder->csi_params[index] < 0) {
        return default_value;
    }
    return decoder->csi_params[index];
}

static void winxterm_text_csi_reset(WinxtermUtf8Decoder *decoder)
{
    decoder->csi_param_count = 0u;
    decoder->csi_param = 0u;
    decoder->csi_has_param = false;
    decoder->csi_param_overflow = false;
    decoder->csi_private = '\0';
    decoder->csi_intermediate_count = 0u;
    memset(decoder->csi_intermediates, 0, sizeof(decoder->csi_intermediates));
    for (uint32_t i = 0; i < WINXTERM_TERMINAL_MAX_PARAMS; ++i) {
        decoder->csi_params[i] = -1;
    }
}

static void winxterm_text_parser_reset_transient(WinxtermUtf8Decoder *decoder)
{
    if (decoder == 0) {
        return;
    }
    decoder->codepoint = 0u;
    decoder->remaining = 0u;
    decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
    decoder->string_kind = 0u;
    decoder->string_length = 0u;
    decoder->string_overflow = false;
    decoder->string_buffer[0] = '\0';
    winxterm_text_csi_reset(decoder);
}

static void winxterm_text_csi_push_param(WinxtermUtf8Decoder *decoder)
{
    if (decoder->csi_param_count >= WINXTERM_TERMINAL_MAX_PARAMS) {
        decoder->csi_param_overflow = true;
        ++decoder->diagnostics.csi_overflow;
        decoder->csi_has_param = false;
        decoder->csi_param = 0u;
        return;
    }
    decoder->csi_params[decoder->csi_param_count++] =
        decoder->csi_has_param ? (int)decoder->csi_param : -1;
    decoder->csi_has_param = false;
    decoder->csi_param = 0u;
}

static bool winxterm_text_intermediates_equal(const WinxtermUtf8Decoder *decoder, const char *text)
{
    size_t length = text != 0 ? strlen(text) : 0u;
    return decoder != 0 &&
           decoder->csi_intermediate_count == length &&
           memcmp(decoder->csi_intermediates, text, length) == 0;
}

static bool winxterm_text_emit_mode(WinxtermTerminalOpSink sink,
                                    void *sink_context,
                                    WinxtermTerminalMode mode,
                                    bool private_mode,
                                    bool enabled)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = enabled ? WINXTERM_TERMINAL_OP_SET_MODE : WINXTERM_TERMINAL_OP_RESET_MODE;
    op.private_mode = private_mode;
    op.data.mode.mode = mode;
    return winxterm_text_emit(sink, sink_context, &op);
}

static bool winxterm_text_emit_mode_action(WinxtermTerminalOpSink sink,
                                           void *sink_context,
                                           WinxtermTerminalMode mode,
                                           bool private_mode,
                                           WinxtermTerminalOpType type)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = type;
    op.private_mode = private_mode;
    op.data.mode.mode = mode;
    return winxterm_text_emit(sink, sink_context, &op);
}

static bool winxterm_text_emit_query(WinxtermUtf8Decoder *decoder,
                                     WinxtermTerminalOpSink sink,
                                     void *sink_context,
                                     WinxtermTerminalQueryType type,
                                     char private_marker,
                                     int param,
                                     const char *request,
                                     size_t request_length)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = WINXTERM_TERMINAL_OP_QUERY;
    op.data.query.type = type;
    op.data.query.private_marker = private_marker;
    op.data.query.param = param;
    if (request != 0 && request_length != 0u) {
        if (request_length >= WINXTERM_TERMINAL_REPLY_CAPACITY) {
            request_length = WINXTERM_TERMINAL_REPLY_CAPACITY - 1u;
        }
        memcpy(op.data.query.request, request, request_length);
        op.data.query.request[request_length] = '\0';
        op.data.query.request_length = request_length;
    }
    ++decoder->diagnostics.replies_emitted;
    return winxterm_text_emit(sink, sink_context, &op);
}

static bool winxterm_text_emit_reset(WinxtermUtf8Decoder *decoder,
                                     WinxtermTerminalOpSink sink,
                                     void *sink_context,
                                     WinxtermTerminalResetKind kind)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = WINXTERM_TERMINAL_OP_RESET;
    op.data.reset.kind = kind;
    bool ok = winxterm_text_emit(sink, sink_context, &op);
    if (kind == WINXTERM_TERMINAL_RESET_HARD) {
        WinxtermParserDiagnostics diagnostics = decoder->diagnostics;
        winxterm_text_parser_reset_transient(decoder);
        decoder->diagnostics = diagnostics;
    }
    return ok;
}

static bool winxterm_text_emit_osc_policy(WinxtermUtf8Decoder *decoder,
                                          WinxtermTerminalOpSink sink,
                                          void *sink_context,
                                          WinxtermTerminalOscCommand command,
                                          WinxtermTerminalOscOutcome outcome,
                                          const char *payload,
                                          size_t payload_length)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = WINXTERM_TERMINAL_OP_OSC;
    op.data.osc.command = command;
    op.data.osc.outcome = outcome;
    if (payload != 0 && payload_length != 0u) {
        if (payload_length >= WINXTERM_TERMINAL_OSC_PAYLOAD_CAPACITY) {
            payload_length = WINXTERM_TERMINAL_OSC_PAYLOAD_CAPACITY - 1u;
        }
        memcpy(op.data.osc.payload, payload, payload_length);
        op.data.osc.payload[payload_length] = '\0';
        op.data.osc.payload_length = payload_length;
    }
    if (outcome == WINXTERM_TERMINAL_OSC_DENIED_SENSITIVE) {
        ++decoder->diagnostics.denied_osc;
    } else if (outcome == WINXTERM_TERMINAL_OSC_UNSUPPORTED) {
        ++decoder->diagnostics.unsupported_osc;
    } else if (outcome == WINXTERM_TERMINAL_OSC_MALFORMED) {
        ++decoder->diagnostics.malformed_sequences;
    }
    return winxterm_text_emit(sink, sink_context, &op);
}

static bool winxterm_text_apply_sgr(WinxtermUtf8Decoder *decoder,
                                    WinxtermTerminalOpSink sink,
                                    void *sink_context)
{
    if (decoder->csi_param_count == 0u) {
        WinxtermTerminalOp op;
        memset(&op, 0, sizeof(op));
        op.type = WINXTERM_TERMINAL_OP_RESET_ATTRIBUTES;
        return winxterm_text_emit(sink, sink_context, &op);
    }
    for (uint32_t i = 0; i < decoder->csi_param_count; ++i) {
        int param = winxterm_text_param_or_default(decoder, i, 0);
        if (param == 0) {
            WinxtermTerminalOp op;
            memset(&op, 0, sizeof(op));
            op.type = WINXTERM_TERMINAL_OP_RESET_ATTRIBUTES;
            if (!winxterm_text_emit(sink, sink_context, &op)) {
                return false;
            }
        } else if (param == 1) {
            if (!winxterm_text_emit_attribute_flags(sink, sink_context, true, WINXTERM_SCREEN_CELL_BOLD)) {
                return false;
            }
        } else if (param == 2) {
            if (!winxterm_text_emit_attribute_flags(sink, sink_context, true, WINXTERM_SCREEN_CELL_FAINT)) {
                return false;
            }
        } else if (param == 3) {
            if (!winxterm_text_emit_attribute_flags(sink, sink_context, true, WINXTERM_SCREEN_CELL_ITALIC)) {
                return false;
            }
        } else if (param == 4) {
            if (!winxterm_text_emit_attribute_flags(sink, sink_context, true, WINXTERM_SCREEN_CELL_UNDERLINE)) {
                return false;
            }
        } else if (param == 5 || param == 6) {
            if (!winxterm_text_emit_attribute_flags(sink, sink_context, true, WINXTERM_SCREEN_CELL_BLINK)) {
                return false;
            }
        } else if (param == 7) {
            if (!winxterm_text_emit_attribute_flags(sink, sink_context, true, WINXTERM_SCREEN_CELL_INVERSE)) {
                return false;
            }
        } else if (param == 8) {
            if (!winxterm_text_emit_attribute_flags(sink, sink_context, true, WINXTERM_SCREEN_CELL_INVISIBLE)) {
                return false;
            }
        } else if (param == 9) {
            if (!winxterm_text_emit_attribute_flags(sink, sink_context, true, WINXTERM_SCREEN_CELL_CROSSED_OUT)) {
                return false;
            }
        } else if (param == 21) {
            if (!winxterm_text_emit_attribute_flags(sink,
                                                   sink_context,
                                                   true,
                                                   WINXTERM_SCREEN_CELL_DOUBLE_UNDERLINE)) {
                return false;
            }
        } else if (param == 22) {
            if (!winxterm_text_emit_attribute_flags(sink,
                                                   sink_context,
                                                   false,
                                                   WINXTERM_SCREEN_CELL_BOLD | WINXTERM_SCREEN_CELL_FAINT)) {
                return false;
            }
        } else if (param == 23) {
            if (!winxterm_text_emit_attribute_flags(sink, sink_context, false, WINXTERM_SCREEN_CELL_ITALIC)) {
                return false;
            }
        } else if (param == 24) {
            if (!winxterm_text_emit_attribute_flags(sink,
                                                   sink_context,
                                                   false,
                                                   WINXTERM_SCREEN_CELL_UNDERLINE |
                                                       WINXTERM_SCREEN_CELL_DOUBLE_UNDERLINE)) {
                return false;
            }
        } else if (param == 25) {
            if (!winxterm_text_emit_attribute_flags(sink, sink_context, false, WINXTERM_SCREEN_CELL_BLINK)) {
                return false;
            }
        } else if (param == 27) {
            if (!winxterm_text_emit_attribute_flags(sink, sink_context, false, WINXTERM_SCREEN_CELL_INVERSE)) {
                return false;
            }
        } else if (param == 28) {
            if (!winxterm_text_emit_attribute_flags(sink, sink_context, false, WINXTERM_SCREEN_CELL_INVISIBLE)) {
                return false;
            }
        } else if (param == 29) {
            if (!winxterm_text_emit_attribute_flags(sink, sink_context, false, WINXTERM_SCREEN_CELL_CROSSED_OUT)) {
                return false;
            }
        } else if (param == 53) {
            if (!winxterm_text_emit_attribute_flags(sink, sink_context, true, WINXTERM_SCREEN_CELL_OVERLINE)) {
                return false;
            }
        } else if (param == 55) {
            if (!winxterm_text_emit_attribute_flags(sink, sink_context, false, WINXTERM_SCREEN_CELL_OVERLINE)) {
                return false;
            }
        } else if (param == 39) {
            if (!winxterm_text_emit_default_color(sink, sink_context, true)) {
                return false;
            }
        } else if (param == 49) {
            if (!winxterm_text_emit_default_color(sink, sink_context, false)) {
                return false;
            }
        } else if (param >= 30 && param <= 37) {
            if (!winxterm_text_emit_color_index(sink, sink_context, true, param - 30)) {
                return false;
            }
        } else if (param >= 40 && param <= 47) {
            if (!winxterm_text_emit_color_index(sink, sink_context, false, param - 40)) {
                return false;
            }
        } else if (param >= 90 && param <= 97) {
            if (!winxterm_text_emit_color_index(sink, sink_context, true, 8 + param - 90)) {
                return false;
            }
        } else if (param >= 100 && param <= 107) {
            if (!winxterm_text_emit_color_index(sink, sink_context, false, 8 + param - 100)) {
                return false;
            }
        } else if ((param == 38 || param == 48) && i + 1u < decoder->csi_param_count) {
            bool foreground = param == 38;
            int color_mode = winxterm_text_param_or_default(decoder, i + 1u, -1);
            if (color_mode == 5 && i + 2u < decoder->csi_param_count) {
                int index = winxterm_text_param_or_default(decoder, i + 2u, 0);
                if (!winxterm_text_emit_color_index(sink, sink_context, foreground, index)) {
                    return false;
                }
                i += 2u;
            } else if (color_mode == 2 && i + 4u < decoder->csi_param_count) {
                int r = winxterm_text_param_or_default(decoder, i + 2u, 0);
                int g = winxterm_text_param_or_default(decoder, i + 3u, 0);
                int b = winxterm_text_param_or_default(decoder, i + 4u, 0);
                if (r < 0) r = 0;
                if (g < 0) g = 0;
                if (b < 0) b = 0;
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
                if (!winxterm_text_emit_rgb(sink,
                                            sink_context,
                                            foreground,
                                            ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b)) {
                    return false;
                }
                i += 4u;
            }
        }
    }
    return true;
}

static bool winxterm_text_apply_mode(WinxtermUtf8Decoder *decoder,
                                     WinxtermTerminalOpSink sink,
                                     void *sink_context,
                                     bool enabled)
{
    for (uint32_t i = 0; i < decoder->csi_param_count; ++i) {
        int param = winxterm_text_param_or_default(decoder, i, 0);
        WinxtermTerminalMode mode;
        if (!winxterm_mode_from_csi(decoder->csi_private, param, &mode)) {
            ++decoder->diagnostics.unsupported_csi;
            continue;
        }
        if (!winxterm_text_emit_mode(sink, sink_context, mode, decoder->csi_private == '?', enabled)) {
            return false;
        }
    }
    return true;
}

static bool winxterm_text_apply_mode_action(WinxtermUtf8Decoder *decoder,
                                            WinxtermTerminalOpSink sink,
                                            void *sink_context,
                                            WinxtermTerminalOpType type)
{
    for (uint32_t i = 0; i < decoder->csi_param_count; ++i) {
        int param = winxterm_text_param_or_default(decoder, i, 0);
        WinxtermTerminalMode mode;
        if (!winxterm_mode_from_csi(decoder->csi_private, param, &mode)) {
            ++decoder->diagnostics.unsupported_csi;
            continue;
        }
        if (!winxterm_text_emit_mode_action(sink, sink_context, mode, decoder->csi_private == '?', type)) {
            return false;
        }
    }
    return true;
}

static bool winxterm_text_dispatch_csi(WinxtermUtf8Decoder *decoder,
                                       uint8_t final,
                                       WinxtermTerminalOpSink sink,
                                       void *sink_context)
{
    if (decoder->csi_has_param || decoder->csi_param_count == 0u) {
        winxterm_text_csi_push_param(decoder);
    }
    if (decoder->csi_param_overflow) {
        return true;
    }

    int p1 = winxterm_text_param_or_default(decoder, 0u, 1);
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));

    if (winxterm_text_intermediates_equal(decoder, " ")) {
        if (final == '@') {
            return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_SCROLL_LEFT, p1);
        }
        if (final == 'A') {
            return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_SCROLL_RIGHT, p1);
        }
        if (final == 'q') {
            op.type = WINXTERM_TERMINAL_OP_CURSOR_STYLE;
            op.data.cursor_style.style = (WinxtermTerminalCursorStyle)winxterm_text_param_or_default(decoder, 0u, 0);
            return winxterm_text_emit(sink, sink_context, &op);
        }
    }

    if (winxterm_text_intermediates_equal(decoder, "'")) {
        if (final == '}') {
            return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_INSERT_COLUMNS, p1);
        }
        if (final == '~') {
            return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_DELETE_COLUMNS, p1);
        }
    }

    if (winxterm_text_intermediates_equal(decoder, "$")) {
        if (final == 'x') {
            op.type = WINXTERM_TERMINAL_OP_RECT_FILL;
            op.data.rectangle.codepoint = (uint32_t)winxterm_text_param_or_default(decoder, 0u, (int)' ');
            op.data.rectangle.top = winxterm_text_param_or_default(decoder, 1u, 1);
            op.data.rectangle.left = winxterm_text_param_or_default(decoder, 2u, 1);
            op.data.rectangle.bottom = winxterm_text_param_or_default(decoder, 3u, 1);
            op.data.rectangle.right = winxterm_text_param_or_default(decoder, 4u, 1);
            return winxterm_text_emit(sink, sink_context, &op);
        }
        if (final == 'z' || final == '{') {
            op.type = WINXTERM_TERMINAL_OP_RECT_ERASE;
            op.private_mode = final == '{';
            op.data.rectangle.top = winxterm_text_param_or_default(decoder, 0u, 1);
            op.data.rectangle.left = winxterm_text_param_or_default(decoder, 1u, 1);
            op.data.rectangle.bottom = winxterm_text_param_or_default(decoder, 2u, 1);
            op.data.rectangle.right = winxterm_text_param_or_default(decoder, 3u, 1);
            return winxterm_text_emit(sink, sink_context, &op);
        }
        if (final == 'v') {
            op.type = WINXTERM_TERMINAL_OP_RECT_COPY;
            op.data.rectangle.top = winxterm_text_param_or_default(decoder, 0u, 1);
            op.data.rectangle.left = winxterm_text_param_or_default(decoder, 1u, 1);
            op.data.rectangle.bottom = winxterm_text_param_or_default(decoder, 2u, 1);
            op.data.rectangle.right = winxterm_text_param_or_default(decoder, 3u, 1);
            op.data.rectangle.dest_top = winxterm_text_param_or_default(decoder, 5u, 1);
            op.data.rectangle.dest_left = winxterm_text_param_or_default(decoder, 6u, 1);
            return winxterm_text_emit(sink, sink_context, &op);
        }
        if (final == 'r' || final == 't') {
            op.type = WINXTERM_TERMINAL_OP_RECT_ATTR;
            op.data.rectangle.top = winxterm_text_param_or_default(decoder, 0u, 1);
            op.data.rectangle.left = winxterm_text_param_or_default(decoder, 1u, 1);
            op.data.rectangle.bottom = winxterm_text_param_or_default(decoder, 2u, 1);
            op.data.rectangle.right = winxterm_text_param_or_default(decoder, 3u, 1);
            op.data.rectangle.flags = WINXTERM_SCREEN_CELL_PROTECTED;
            op.data.rectangle.set_flags = final == 'r';
            return winxterm_text_emit(sink, sink_context, &op);
        }
    }

    if (winxterm_text_intermediates_equal(decoder, "#")) {
        if (final == '{') {
            op.type = WINXTERM_TERMINAL_OP_PUSH_ATTRIBUTES;
            return winxterm_text_emit(sink, sink_context, &op);
        }
        if (final == '}') {
            op.type = WINXTERM_TERMINAL_OP_POP_ATTRIBUTES;
            return winxterm_text_emit(sink, sink_context, &op);
        }
    }

    switch (final) {
    case 'A':
        return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_CURSOR_UP, p1);
    case 'B':
        return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_CURSOR_DOWN, p1);
    case 'C':
        return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_CURSOR_FORWARD, p1);
    case 'D':
        return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_CURSOR_BACK, p1);
    case 'E':
        return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_CURSOR_NEXT_LINE, p1);
    case 'F':
        return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_CURSOR_PREVIOUS_LINE, p1);
    case 'G':
    case '`':
        return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_CURSOR_HORIZONTAL_ABSOLUTE, p1);
    case 'd':
        return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_CURSOR_VERTICAL_ABSOLUTE, p1);
    case 'I':
        return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_CURSOR_TAB_FORWARD, p1);
    case 'Z':
        return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_CURSOR_TAB_BACK, p1);
    case 'H':
    case 'f':
        op.type = WINXTERM_TERMINAL_OP_CURSOR_POSITION;
        op.data.cursor.row = winxterm_text_param_or_default(decoder, 0u, 1);
        op.data.cursor.column = winxterm_text_param_or_default(decoder, 1u, 1);
        return winxterm_text_emit(sink, sink_context, &op);
    case 'J':
        op.type = decoder->csi_private == '?' ?
            WINXTERM_TERMINAL_OP_SELECTIVE_ERASE_DISPLAY : WINXTERM_TERMINAL_OP_ERASE_DISPLAY;
        op.data.erase.mode = (WinxtermTerminalEraseMode)winxterm_text_param_or_default(decoder, 0u, 0);
        return winxterm_text_emit(sink, sink_context, &op);
    case 'K':
        op.type = decoder->csi_private == '?' ?
            WINXTERM_TERMINAL_OP_SELECTIVE_ERASE_LINE : WINXTERM_TERMINAL_OP_ERASE_LINE;
        op.data.erase.mode = (WinxtermTerminalEraseMode)winxterm_text_param_or_default(decoder, 0u, 0);
        return winxterm_text_emit(sink, sink_context, &op);
    case '@':
        return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_INSERT_CHARS, p1);
    case 'P':
        return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_DELETE_CHARS, p1);
    case 'L':
        return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_INSERT_LINES, p1);
    case 'M':
        return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_DELETE_LINES, p1);
    case 'S':
        return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_SCROLL_UP, p1);
    case 'T':
        return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_SCROLL_DOWN, p1);
    case 'X':
        return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_ERASE_CHARS, p1);
    case 'b':
        return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_REPEAT_CHAR, p1);
    case 'g':
        return winxterm_text_emit_tab_clear(sink, sink_context, winxterm_text_param_or_default(decoder, 0u, 0));
    case 'r':
        if (decoder->csi_private == '?') {
            return winxterm_text_apply_mode_action(decoder,
                                                   sink,
                                                   sink_context,
                                                   WINXTERM_TERMINAL_OP_RESTORE_MODE);
        }
        op.type = WINXTERM_TERMINAL_OP_SET_SCROLL_REGION;
        op.data.scroll_region.top = winxterm_text_param_or_default(decoder, 0u, 1);
        op.data.scroll_region.bottom = winxterm_text_param_or_default(decoder, 1u, 0);
        return winxterm_text_emit(sink, sink_context, &op);
    case 's':
        if (decoder->csi_private == '?') {
            return winxterm_text_apply_mode_action(decoder,
                                                   sink,
                                                   sink_context,
                                                   WINXTERM_TERMINAL_OP_SAVE_MODE);
        }
        if (decoder->csi_param_count >= 2u) {
            op.type = WINXTERM_TERMINAL_OP_SET_HORIZONTAL_MARGINS;
            op.data.horizontal_margins.left = winxterm_text_param_or_default(decoder, 0u, 1);
            op.data.horizontal_margins.right = winxterm_text_param_or_default(decoder, 1u, 0);
            return winxterm_text_emit(sink, sink_context, &op);
        }
        op.type = WINXTERM_TERMINAL_OP_SAVE_CURSOR;
        return winxterm_text_emit(sink, sink_context, &op);
    case 'u':
        op.type = WINXTERM_TERMINAL_OP_RESTORE_CURSOR;
        return winxterm_text_emit(sink, sink_context, &op);
    case 'h':
        return winxterm_text_apply_mode(decoder, sink, sink_context, true);
    case 'l':
        return winxterm_text_apply_mode(decoder, sink, sink_context, false);
    case 'm':
        return winxterm_text_apply_sgr(decoder, sink, sink_context);
    case 'c':
        if (decoder->csi_private == '>') {
            return winxterm_text_emit_query(decoder,
                                            sink,
                                            sink_context,
                                            WINXTERM_TERMINAL_QUERY_SECONDARY_DA,
                                            decoder->csi_private,
                                            p1,
                                            0,
                                            0u);
        }
        if (decoder->csi_private == '\0') {
            return winxterm_text_emit_query(decoder,
                                            sink,
                                            sink_context,
                                            WINXTERM_TERMINAL_QUERY_PRIMARY_DA,
                                            decoder->csi_private,
                                            p1,
                                            0,
                                            0u);
        }
        ++decoder->diagnostics.unsupported_csi;
        return true;
    case 'n':
        if (p1 == 5) {
            return winxterm_text_emit_query(decoder,
                                            sink,
                                            sink_context,
                                            WINXTERM_TERMINAL_QUERY_DSR_STATUS,
                                            decoder->csi_private,
                                            p1,
                                            0,
                                            0u);
        }
        if (p1 == 6) {
            return winxterm_text_emit_query(decoder,
                                            sink,
                                            sink_context,
                                            WINXTERM_TERMINAL_QUERY_CPR,
                                            decoder->csi_private,
                                            p1,
                                            0,
                                            0u);
        }
        ++decoder->diagnostics.unsupported_csi;
        return true;
    case 'p':
        if (winxterm_text_intermediates_equal(decoder, "!")) {
            return winxterm_text_emit_reset(decoder, sink, sink_context, WINXTERM_TERMINAL_RESET_SOFT);
        }
        if (winxterm_text_intermediates_equal(decoder, "$")) {
            return winxterm_text_emit_query(decoder,
                                            sink,
                                            sink_context,
                                            WINXTERM_TERMINAL_QUERY_DECRQM,
                                            decoder->csi_private,
                                            p1,
                                            0,
                                            0u);
        }
        ++decoder->diagnostics.unsupported_csi;
        return true;
    case 'q':
        if (decoder->csi_private == '>') {
            WinxtermTerminalQueryType type = WINXTERM_TERMINAL_QUERY_XTVERSION;
            if (p1 == 4) {
                type = WINXTERM_TERMINAL_QUERY_XTQMODKEYS;
            } else if (p1 == 1) {
                type = WINXTERM_TERMINAL_QUERY_XTQFMTKEYS;
            }
            return winxterm_text_emit_query(decoder, sink, sink_context, type, decoder->csi_private, p1, 0, 0u);
        }
        if (winxterm_text_intermediates_equal(decoder, "\"")) {
            int mode = winxterm_text_param_or_default(decoder, 0u, 0);
            op.type = WINXTERM_TERMINAL_OP_SET_PROTECTED;
            op.data.count = (mode == 1 || mode == 2) ? 1 : 0;
            return winxterm_text_emit(sink, sink_context, &op);
        }
        ++decoder->diagnostics.unsupported_csi;
        return true;
    default:
        ++decoder->diagnostics.unsupported_csi;
        return true;
    }
}

static bool winxterm_text_parse_osc_number(const char *text,
                                           size_t length,
                                           int *command,
                                           size_t *payload_offset)
{
    int value = 0;
    size_t i = 0u;
    if (text == 0 || command == 0 || payload_offset == 0 || length == 0u) {
        return false;
    }
    while (i < length && text[i] >= '0' && text[i] <= '9') {
        value = value * 10 + (int)(text[i] - '0');
        if (value > 9999) {
            return false;
        }
        ++i;
    }
    if (i == 0u || i >= length || text[i] != ';') {
        return false;
    }
    *command = value;
    *payload_offset = i + 1u;
    return true;
}

static bool winxterm_text_emit_title(WinxtermTerminalOpSink sink,
                                     void *sink_context,
                                     const char *text,
                                     size_t length)
{
    WinxtermTerminalOp op;
    memset(&op, 0, sizeof(op));
    op.type = WINXTERM_TERMINAL_OP_TITLE;
    if (length >= WINXTERM_TERMINAL_TITLE_CAPACITY) {
        length = WINXTERM_TERMINAL_TITLE_CAPACITY - 1u;
    }
    memcpy(op.data.title.text, text, length);
    op.data.title.text[length] = '\0';
    op.data.title.length = length;
    return winxterm_text_emit(sink, sink_context, &op);
}

static bool winxterm_text_dispatch_osc(WinxtermUtf8Decoder *decoder,
                                       WinxtermTerminalOpSink sink,
                                       void *sink_context)
{
    const char *text = decoder->string_buffer;
    size_t length = decoder->string_length;
    int command_value = 0;
    size_t payload_offset = 0u;
    if (decoder->string_overflow) {
        return winxterm_text_emit_osc_policy(decoder,
                                             sink,
                                             sink_context,
                                             WINXTERM_TERMINAL_OSC_UNKNOWN,
                                             WINXTERM_TERMINAL_OSC_OVERFLOW,
                                             0,
                                             0u);
    }
    if (!winxterm_text_parse_osc_number(text, length, &command_value, &payload_offset)) {
        return winxterm_text_emit_osc_policy(decoder,
                                             sink,
                                             sink_context,
                                             WINXTERM_TERMINAL_OSC_UNKNOWN,
                                             WINXTERM_TERMINAL_OSC_MALFORMED,
                                             text,
                                             length);
    }

    const char *payload = text + payload_offset;
    size_t payload_length = length - payload_offset;
    WinxtermTerminalOscCommand command = (WinxtermTerminalOscCommand)command_value;
    if (command_value == 0 || command_value == 2) {
        if (!winxterm_text_emit_title(sink, sink_context, payload, payload_length)) {
            return false;
        }
        return winxterm_text_emit_osc_policy(decoder,
                                             sink,
                                             sink_context,
                                             command,
                                             WINXTERM_TERMINAL_OSC_ACCEPTED,
                                             payload,
                                             payload_length);
    }
    if (command_value == 1) {
        return winxterm_text_emit_osc_policy(decoder,
                                             sink,
                                             sink_context,
                                             command,
                                             WINXTERM_TERMINAL_OSC_ACCEPTED_NO_CONSUMER,
                                             payload,
                                             payload_length);
    }
    if (command_value == 52) {
        return winxterm_text_emit_osc_policy(decoder,
                                             sink,
                                             sink_context,
                                             command,
                                             WINXTERM_TERMINAL_OSC_DENIED_SENSITIVE,
                                             0,
                                             0u);
    }
    if (command_value == 8) {
        return winxterm_text_emit_osc_policy(decoder,
                                             sink,
                                             sink_context,
                                             command,
                                             WINXTERM_TERMINAL_OSC_UNSUPPORTED,
                                             payload,
                                             payload_length);
    }
    if (command_value == 4 || command_value == 10 || command_value == 11) {
        return winxterm_text_emit_osc_policy(decoder,
                                             sink,
                                             sink_context,
                                             command,
                                             WINXTERM_TERMINAL_OSC_ACCEPTED_NO_CONSUMER,
                                             payload,
                                             payload_length);
    }
    if (command_value == WINXTERM_TERMINAL_OSC_WINXTERM_CONTROL) {
        return winxterm_text_emit_osc_policy(decoder,
                                             sink,
                                             sink_context,
                                             command,
                                             WINXTERM_TERMINAL_OSC_ACCEPTED,
                                             payload,
                                             payload_length);
    }
    return winxterm_text_emit_osc_policy(decoder,
                                         sink,
                                         sink_context,
                                         WINXTERM_TERMINAL_OSC_UNKNOWN,
                                         WINXTERM_TERMINAL_OSC_UNSUPPORTED,
                                         payload,
                                         payload_length);
}

static bool winxterm_text_dispatch_string(WinxtermUtf8Decoder *decoder,
                                          WinxtermTerminalOpSink sink,
                                          void *sink_context)
{
    if (decoder->string_kind == WINXTERM_TEXT_STRING_OSC) {
        return winxterm_text_dispatch_osc(decoder, sink, sink_context);
    }
    if (decoder->string_kind == WINXTERM_TEXT_STRING_DCS) {
        if (!decoder->string_overflow &&
            decoder->string_length >= 2u &&
            decoder->string_buffer[0] == '$' &&
            decoder->string_buffer[1] == 'q') {
            return winxterm_text_emit_query(decoder,
                                            sink,
                                            sink_context,
                                            WINXTERM_TERMINAL_QUERY_DECRQSS,
                                            '\0',
                                            0,
                                            decoder->string_buffer + 2,
                                            decoder->string_length - 2u);
        }
    }
    ++decoder->diagnostics.ignored_strings;
    return true;
}

static void winxterm_text_begin_string(WinxtermUtf8Decoder *decoder, uint8_t string_kind)
{
    decoder->escape_state = WINXTERM_TEXT_ESCAPE_STRING;
    decoder->string_kind = string_kind;
    decoder->string_length = 0u;
    decoder->string_overflow = false;
    decoder->string_buffer[0] = '\0';
}

static bool winxterm_text_append_string_byte(WinxtermUtf8Decoder *decoder, uint8_t byte)
{
    if (decoder->string_length + 1u >= WINXTERM_TERMINAL_OSC_PAYLOAD_CAPACITY) {
        decoder->string_overflow = true;
        ++decoder->diagnostics.string_overflow;
        return true;
    }
    decoder->string_buffer[decoder->string_length++] = (char)byte;
    decoder->string_buffer[decoder->string_length] = '\0';
    return true;
}

static bool winxterm_text_feed_escape_byte(WinxtermUtf8Decoder *decoder,
                                           uint8_t byte,
                                           WinxtermTerminalOpSink sink,
                                           void *sink_context)
{
    if (decoder->escape_state == WINXTERM_TEXT_ESCAPE_ESC) {
        if (byte == '[') {
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_CSI;
            winxterm_text_csi_reset(decoder);
        } else if (byte == ']') {
            winxterm_text_begin_string(decoder, WINXTERM_TEXT_STRING_OSC);
        } else if (byte == 'P') {
            winxterm_text_begin_string(decoder, WINXTERM_TEXT_STRING_DCS);
        } else if (byte == '_') {
            winxterm_text_begin_string(decoder, WINXTERM_TEXT_STRING_APC);
        } else if (byte == '^') {
            winxterm_text_begin_string(decoder, WINXTERM_TEXT_STRING_PM);
        } else if (byte == 'X') {
            winxterm_text_begin_string(decoder, WINXTERM_TEXT_STRING_SOS);
        } else if (byte == '#') {
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_ESC_HASH;
        } else if (byte == ' ') {
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_ESC_SPACE;
        } else if (byte == '(') {
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_ESC_CHARSET_G0;
        } else if (byte == 'D') {
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
            return winxterm_text_emit_simple(sink, sink_context, WINXTERM_TERMINAL_OP_INDEX);
        } else if (byte == 'E') {
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
            return winxterm_text_emit_simple(sink, sink_context, WINXTERM_TERMINAL_OP_NEXT_LINE);
        } else if (byte == 'M') {
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
            return winxterm_text_emit_simple(sink, sink_context, WINXTERM_TERMINAL_OP_REVERSE_INDEX);
        } else if (byte == 'H') {
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
            return winxterm_text_emit_simple(sink, sink_context, WINXTERM_TERMINAL_OP_TAB_SET);
        } else if (byte == '6') {
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
            return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_SCROLL_RIGHT, 1);
        } else if (byte == '9') {
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
            return winxterm_text_emit_count(sink, sink_context, WINXTERM_TERMINAL_OP_SCROLL_LEFT, 1);
        } else if (byte == 'V') {
            WinxtermTerminalOp op;
            memset(&op, 0, sizeof(op));
            op.type = WINXTERM_TERMINAL_OP_SET_PROTECTED;
            op.data.count = 1;
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
            return winxterm_text_emit(sink, sink_context, &op);
        } else if (byte == 'W') {
            WinxtermTerminalOp op;
            memset(&op, 0, sizeof(op));
            op.type = WINXTERM_TERMINAL_OP_SET_PROTECTED;
            op.data.count = 0;
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
            return winxterm_text_emit(sink, sink_context, &op);
        } else if (byte == '7') {
            WinxtermTerminalOp op;
            memset(&op, 0, sizeof(op));
            op.type = WINXTERM_TERMINAL_OP_SAVE_CURSOR;
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
            return winxterm_text_emit(sink, sink_context, &op);
        } else if (byte == '8') {
            WinxtermTerminalOp op;
            memset(&op, 0, sizeof(op));
            op.type = WINXTERM_TERMINAL_OP_RESTORE_CURSOR;
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
            return winxterm_text_emit(sink, sink_context, &op);
        } else if (byte == '=') {
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
            return winxterm_text_emit_mode(sink,
                                           sink_context,
                                           WINXTERM_TERMINAL_MODE_APPLICATION_KEYPAD,
                                           false,
                                           true);
        } else if (byte == '>') {
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
            return winxterm_text_emit_mode(sink,
                                           sink_context,
                                           WINXTERM_TERMINAL_MODE_APPLICATION_KEYPAD,
                                           false,
                                           false);
        } else if (byte == 'c') {
            return winxterm_text_emit_reset(decoder, sink, sink_context, WINXTERM_TERMINAL_RESET_HARD);
        } else {
            ++decoder->diagnostics.unsupported_esc;
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
        }
        return true;
    }

    if (decoder->escape_state == WINXTERM_TEXT_ESCAPE_ESC_CHARSET_G0) {
        decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
        if (byte == '0') {
            return winxterm_text_emit_charset(sink,
                                              sink_context,
                                              0,
                                              WINXTERM_TERMINAL_CHARSET_DEC_SPECIAL_GRAPHICS);
        }
        if (byte == 'B') {
            return winxterm_text_emit_charset(sink,
                                              sink_context,
                                              0,
                                              WINXTERM_TERMINAL_CHARSET_ASCII);
        }
        ++decoder->diagnostics.unsupported_esc;
        return true;
    }

    if (decoder->escape_state == WINXTERM_TEXT_ESCAPE_ESC_HASH) {
        decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
        if (byte == '8') {
            WinxtermTerminalOp op;
            memset(&op, 0, sizeof(op));
            op.type = WINXTERM_TERMINAL_OP_DECALN;
            return winxterm_text_emit(sink, sink_context, &op);
        }
        ++decoder->diagnostics.unsupported_esc;
        return true;
    }

    if (decoder->escape_state == WINXTERM_TEXT_ESCAPE_ESC_SPACE) {
        decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
        if (byte == 'F') {
            return winxterm_text_emit_mode(sink,
                                           sink_context,
                                           WINXTERM_TERMINAL_MODE_EIGHT_BIT_CONTROLS,
                                           false,
                                           false);
        }
        if (byte == 'G') {
            return winxterm_text_emit_mode(sink,
                                           sink_context,
                                           WINXTERM_TERMINAL_MODE_EIGHT_BIT_CONTROLS,
                                           false,
                                           true);
        }
        ++decoder->diagnostics.unsupported_esc;
        return true;
    }

    if (decoder->escape_state == WINXTERM_TEXT_ESCAPE_STRING ||
        decoder->escape_state == WINXTERM_TEXT_ESCAPE_STRING_ESC) {
        if (decoder->escape_state == WINXTERM_TEXT_ESCAPE_STRING_ESC) {
            if (byte == '\\') {
                decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
                return winxterm_text_dispatch_string(decoder, sink, sink_context);
            }
            (void)winxterm_text_append_string_byte(decoder, 0x1bu);
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_STRING;
        }
        if (byte == 0x07u) {
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
            return winxterm_text_dispatch_string(decoder, sink, sink_context);
        }
        if (byte == 0x1bu) {
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_STRING_ESC;
            return true;
        }
        return winxterm_text_append_string_byte(decoder, byte);
    }

    if (decoder->escape_state == WINXTERM_TEXT_ESCAPE_CSI) {
        if ((byte == '?' || byte == '>') && decoder->csi_param_count == 0u &&
            !decoder->csi_has_param) {
            decoder->csi_private = (char)byte;
            return true;
        }
        if (byte >= '0' && byte <= '9') {
            decoder->csi_param = decoder->csi_param * 10u + (uint32_t)(byte - '0');
            if (decoder->csi_param > 9999u) {
                decoder->csi_param = 9999u;
            }
            decoder->csi_has_param = true;
            return true;
        }
        if (byte == ';' || byte == ':') {
            winxterm_text_csi_push_param(decoder);
            return true;
        }
        if (byte >= 0x20u && byte <= 0x2fu) {
            if (decoder->csi_intermediate_count + 1u < sizeof(decoder->csi_intermediates)) {
                decoder->csi_intermediates[decoder->csi_intermediate_count++] = (char)byte;
                decoder->csi_intermediates[decoder->csi_intermediate_count] = '\0';
            } else {
                decoder->csi_param_overflow = true;
                ++decoder->diagnostics.csi_overflow;
            }
            return true;
        }
        if (byte >= 0x40u && byte <= 0x7eu) {
            bool ok = winxterm_text_dispatch_csi(decoder, byte, sink, sink_context);
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
            return ok;
        }
        ++decoder->diagnostics.malformed_sequences;
        return true;
    }

    return true;
}

static bool winxterm_text_emit_codepoint(WinxtermTerminalOpSink sink,
                                         void *sink_context,
                                         uint32_t codepoint)
{
    if (codepoint == 0x08u) {
        return winxterm_text_emit_control(sink, sink_context, WINXTERM_TERMINAL_CONTROL_BS);
    }
    if (codepoint == 0x09u) {
        return winxterm_text_emit_control(sink, sink_context, WINXTERM_TERMINAL_CONTROL_TAB);
    }
    if (codepoint == 0x0au) {
        return winxterm_text_emit_control(sink, sink_context, WINXTERM_TERMINAL_CONTROL_LF);
    }
    if (codepoint == 0x0du) {
        return winxterm_text_emit_control(sink, sink_context, WINXTERM_TERMINAL_CONTROL_CR);
    }
    if (codepoint == 0x07u) {
        WinxtermTerminalOp op;
        memset(&op, 0, sizeof(op));
        op.type = WINXTERM_TERMINAL_OP_BELL;
        return winxterm_text_emit(sink, sink_context, &op);
    }
    if (codepoint < 0x20u) {
        return true;
    }
    {
        WinxtermTerminalOp op;
        memset(&op, 0, sizeof(op));
        op.type = WINXTERM_TERMINAL_OP_PRINT;
        op.data.codepoint = codepoint;
        return winxterm_text_emit(sink, sink_context, &op);
    }
}

static bool winxterm_text_emit_replacement(WinxtermTerminalOpSink sink, void *sink_context)
{
    return winxterm_text_emit_codepoint(sink, sink_context, 0xfffdu);
}

static bool winxterm_text_feed_codepoint(WinxtermUtf8Decoder *decoder,
                                         uint32_t codepoint,
                                         WinxtermTerminalOpSink sink,
                                         void *sink_context)
{
    if (codepoint == 0x1bu) {
        decoder->escape_state = WINXTERM_TEXT_ESCAPE_ESC;
        return true;
    }
    return winxterm_text_emit_codepoint(sink, sink_context, codepoint);
}

static bool winxterm_text_feed_c1_control(WinxtermUtf8Decoder *decoder,
                                          uint8_t byte,
                                          WinxtermTerminalOpSink sink,
                                          void *sink_context)
{
    if (byte == 0x84u) {
        return winxterm_text_emit_simple(sink, sink_context, WINXTERM_TERMINAL_OP_INDEX);
    }
    if (byte == 0x85u) {
        return winxterm_text_emit_simple(sink, sink_context, WINXTERM_TERMINAL_OP_NEXT_LINE);
    }
    if (byte == 0x8du) {
        return winxterm_text_emit_simple(sink, sink_context, WINXTERM_TERMINAL_OP_REVERSE_INDEX);
    }
    if (byte == 0x90u) {
        winxterm_text_begin_string(decoder, WINXTERM_TEXT_STRING_DCS);
        return true;
    }
    if (byte == 0x98u) {
        winxterm_text_begin_string(decoder, WINXTERM_TEXT_STRING_SOS);
        return true;
    }
    if (byte == 0x9bu) {
        decoder->escape_state = WINXTERM_TEXT_ESCAPE_CSI;
        winxterm_text_csi_reset(decoder);
        return true;
    }
    if (byte == 0x9cu) {
        if (decoder->escape_state == WINXTERM_TEXT_ESCAPE_STRING ||
            decoder->escape_state == WINXTERM_TEXT_ESCAPE_STRING_ESC) {
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_NONE;
            return winxterm_text_dispatch_string(decoder, sink, sink_context);
        }
        return true;
    }
    if (byte == 0x9du) {
        winxterm_text_begin_string(decoder, WINXTERM_TEXT_STRING_OSC);
        return true;
    }
    if (byte == 0x9eu) {
        winxterm_text_begin_string(decoder, WINXTERM_TEXT_STRING_PM);
        return true;
    }
    if (byte == 0x9fu) {
        winxterm_text_begin_string(decoder, WINXTERM_TEXT_STRING_APC);
        return true;
    }
    return true;
}

bool winxterm_text_feed_bytes_to_sink(WinxtermUtf8Decoder *decoder,
                                      const uint8_t *bytes,
                                      size_t byte_count,
                                      WinxtermTerminalOpSink sink,
                                      void *sink_context)
{
    if (decoder == 0 || (bytes == 0 && byte_count != 0u) || sink == 0) {
        return false;
    }

    for (size_t i = 0; i < byte_count; ++i) {
        uint8_t byte = bytes[i];
        if (decoder->escape_state != WINXTERM_TEXT_ESCAPE_NONE) {
            if (byte == 0x9cu &&
                (decoder->escape_state == WINXTERM_TEXT_ESCAPE_STRING ||
                 decoder->escape_state == WINXTERM_TEXT_ESCAPE_STRING_ESC)) {
                if (!winxterm_text_feed_c1_control(decoder, byte, sink, sink_context)) {
                    return false;
                }
                continue;
            }
            if (!winxterm_text_feed_escape_byte(decoder, byte, sink, sink_context)) {
                return false;
            }
            continue;
        }
        if (byte == 0x1bu) {
            decoder->escape_state = WINXTERM_TEXT_ESCAPE_ESC;
            decoder->remaining = 0u;
            continue;
        }
        if (decoder->remaining == 0u && byte >= 0x80u && byte <= 0x9fu) {
            if (!winxterm_text_feed_c1_control(decoder, byte, sink, sink_context)) {
                return false;
            }
            continue;
        }
        if (byte < 0x20u) {
            if (!winxterm_text_emit_codepoint(sink, sink_context, byte)) {
                return false;
            }
            continue;
        }
        if (decoder->remaining == 0u) {
            if (byte < 0x80u) {
                if (!winxterm_text_emit_codepoint(sink, sink_context, byte)) {
                    return false;
                }
            } else if ((byte & 0xe0u) == 0xc0u) {
                decoder->codepoint = byte & 0x1fu;
                decoder->remaining = 1u;
            } else if ((byte & 0xf0u) == 0xe0u) {
                decoder->codepoint = byte & 0x0fu;
                decoder->remaining = 2u;
            } else if ((byte & 0xf8u) == 0xf0u) {
                decoder->codepoint = byte & 0x07u;
                decoder->remaining = 3u;
            } else if (!winxterm_text_emit_replacement(sink, sink_context)) {
                return false;
            }
        } else if ((byte & 0xc0u) == 0x80u) {
            decoder->codepoint = (decoder->codepoint << 6) | (uint32_t)(byte & 0x3fu);
            --decoder->remaining;
            if (decoder->remaining == 0u) {
                uint32_t codepoint = decoder->codepoint;
                if (!winxterm_text_feed_codepoint(decoder,
                                                  codepoint,
                                                  sink,
                                                  sink_context)) {
                    return false;
                }
            }
        } else {
            decoder->remaining = 0u;
            if (!winxterm_text_emit_replacement(sink, sink_context)) {
                return false;
            }
            --i;
        }
    }
    return true;
}

static bool winxterm_text_screen_sink(void *context, const WinxtermTerminalOp *op)
{
    return winxterm_screen_apply_op((WinxtermScreen *)context, op);
}

bool winxterm_text_feed_bytes(WinxtermScreen *screen,
                              WinxtermUtf8Decoder *decoder,
                              const uint8_t *bytes,
                              size_t byte_count)
{
    if (screen == 0) {
        return false;
    }
    return winxterm_text_feed_bytes_to_sink(decoder, bytes, byte_count, winxterm_text_screen_sink, screen);
}
