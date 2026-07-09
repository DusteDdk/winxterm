#ifndef WINXTERM_GLYPH_FALLBACK_H
#define WINXTERM_GLYPH_FALLBACK_H

#include "winxterm_render.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WINXTERM_FALLBACK_MAX_COMBINING_CODEPOINTS 4u
#define WINXTERM_FALLBACK_MAX_CODEPOINTS (1u + WINXTERM_FALLBACK_MAX_COMBINING_CODEPOINTS)
#define WINXTERM_FALLBACK_MAX_WIDTH_CELLS 1u
#define WINXTERM_FALLBACK_MAX_WIDTH_PIXELS (WINXTERM_CELL_WIDTH_PIXELS * WINXTERM_FALLBACK_MAX_WIDTH_CELLS)
#define WINXTERM_FALLBACK_MAX_PIXELS (WINXTERM_FALLBACK_MAX_WIDTH_PIXELS * WINXTERM_CELL_HEIGHT_PIXELS)

typedef struct WinxtermLog WinxtermLog;
typedef struct WinxtermGlyphFallbackState WinxtermGlyphFallbackState;

typedef enum WinxtermGlyphFallbackFontKind {
    WINXTERM_GLYPH_FALLBACK_FONT_GENERAL = 0,
    WINXTERM_GLYPH_FALLBACK_FONT_EMOJI = 1,
    WINXTERM_GLYPH_FALLBACK_FONT_MATH = 2,
    WINXTERM_GLYPH_FALLBACK_FONT_COUNT = 3,
} WinxtermGlyphFallbackFontKind;

typedef struct WinxtermGlyphFallbackBitmap {
    int width_pixels;
    int height_pixels;
    bool color_pixels;
    uint8_t alpha[WINXTERM_FALLBACK_MAX_PIXELS];
    uint32_t pixels[WINXTERM_FALLBACK_MAX_PIXELS];
} WinxtermGlyphFallbackBitmap;

WinxtermGlyphFallbackState *winxterm_glyph_fallback_create(void);
void winxterm_glyph_fallback_destroy(WinxtermGlyphFallbackState *state);
bool winxterm_glyph_fallback_load_resources(WinxtermGlyphFallbackState *state,
                                            void *module_instance,
                                            WinxtermLog *log);
bool winxterm_glyph_fallback_ready(const WinxtermGlyphFallbackState *state);
size_t winxterm_glyph_fallback_cached_count(const WinxtermGlyphFallbackState *state);
size_t winxterm_glyph_fallback_miss_count(const WinxtermGlyphFallbackState *state);
WinxtermGlyphFallbackFontKind winxterm_glyph_fallback_select_font(uint32_t codepoint);
const char *winxterm_glyph_fallback_font_name(WinxtermGlyphFallbackFontKind kind);

const WinxtermGlyphFallbackBitmap *winxterm_glyph_fallback_get_glyph(
    WinxtermGlyphFallbackState *state,
    uint32_t codepoint,
    const uint32_t *combining_codepoints,
    uint8_t combining_count,
    uint8_t width_cells);

#ifdef __cplusplus
}
#endif

#endif
