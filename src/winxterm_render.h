#ifndef WINXTERM_RENDER_H
#define WINXTERM_RENDER_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#define WINXTERM_TERMINAL_COLUMNS 80
#define WINXTERM_TERMINAL_ROWS 24

#define WINXTERM_CELL_WIDTH_PIXELS 6
#define WINXTERM_CELL_HEIGHT_PIXELS 13

#define WINXTERM_INITIAL_PIXEL_WIDTH (WINXTERM_TERMINAL_COLUMNS * WINXTERM_CELL_WIDTH_PIXELS)
#define WINXTERM_INITIAL_PIXEL_HEIGHT (WINXTERM_TERMINAL_ROWS * WINXTERM_CELL_HEIGHT_PIXELS)

#define WINXTERM_BYTES_PER_PIXEL 4
#define WINXTERM_PIXEL_FORMAT_NAME "BGRA32"

#define WINXTERM_DEFAULT_FOREGROUND_RGB 0x00e3e3e3u
#define WINXTERM_DEFAULT_BACKGROUND_RGB 0x00262626u
#define WINXTERM_PHASE1_CLEAR_RGB 0x00000000u

#define WINXTERM_MISSING_GLYPH_INDEX 256u
#define WINXTERM_DYNAMIC_GLYPH_INDEX 0xffffffffu

typedef struct WinxtermGlyphFallbackState WinxtermGlyphFallbackState;
typedef struct WinxtermLog WinxtermLog;

typedef struct WinxtermCellSize {
    int columns;
    int rows;
} WinxtermCellSize;

typedef struct WinxtermRenderDamage {
    uint64_t *dirty_rows;
    size_t word_count;
    int row_count;
    bool full;
    bool scroll_valid;
    int scroll_rows;
} WinxtermRenderDamage;

bool winxterm_render_damage_resize(WinxtermRenderDamage *damage, int rows);
void winxterm_render_damage_dispose(WinxtermRenderDamage *damage);
void winxterm_render_damage_clear(WinxtermRenderDamage *damage);
void winxterm_render_damage_mark_row(WinxtermRenderDamage *damage, int row);
void winxterm_render_damage_mark_rows(WinxtermRenderDamage *damage, int first, int last);
void winxterm_render_damage_mark_all(WinxtermRenderDamage *damage);
void winxterm_render_damage_record_scroll(WinxtermRenderDamage *damage, int rows);
bool winxterm_render_damage_row(const WinxtermRenderDamage *damage, int row);
bool winxterm_render_damage_any(const WinxtermRenderDamage *damage);

typedef struct WinxtermRenderGlyphCell {
    uint32_t glyph_index;
    uint32_t foreground_rgb;
    uint32_t background_rgb;
} WinxtermRenderGlyphCell;

typedef struct WinxtermRenderContext {
    WinxtermGlyphFallbackState *glyph_fallback;
} WinxtermRenderContext;

static inline WinxtermCellSize winxterm_pixels_to_cells(int pixel_width, int pixel_height)
{
    WinxtermCellSize size;
    size.columns = pixel_width > 0 ? pixel_width / WINXTERM_CELL_WIDTH_PIXELS : 0;
    size.rows = pixel_height > 0 ? pixel_height / WINXTERM_CELL_HEIGHT_PIXELS : 0;
    return size;
}

uint32_t winxterm_render_bgra_from_rgb(uint32_t rgb);

void winxterm_render_context_init(WinxtermRenderContext *context);
bool winxterm_render_context_load_fallback_fonts(WinxtermRenderContext *context,
                                                 void *module_instance,
                                                 WinxtermLog *log);
void winxterm_render_context_dispose(WinxtermRenderContext *context);
bool winxterm_render_context_fallback_ready(const WinxtermRenderContext *context);
size_t winxterm_render_context_fallback_cached_count(const WinxtermRenderContext *context);
size_t winxterm_render_context_fallback_miss_count(const WinxtermRenderContext *context);

void winxterm_render_clear(uint32_t *pixels, int width, int height, uint32_t rgb);
void winxterm_render_clear_rect(uint32_t *pixels,
                                int bitmap_width,
                                int bitmap_height,
                                int x,
                                int y,
                                int width,
                                int height,
                                uint32_t rgb);
void winxterm_render_blend_rect_outline(uint32_t *pixels,
                                        int bitmap_width,
                                        int bitmap_height,
                                        int x,
                                        int y,
                                        int width,
                                        int height,
                                        uint32_t rgb,
                                        uint8_t alpha);
void winxterm_render_scroll_lines(uint32_t *pixels,
                                  int width,
                                  int height,
                                  int line_delta,
                                  uint32_t background_rgb);
void winxterm_render_draw_glyph(uint32_t *pixels,
                                int bitmap_width,
                                int bitmap_height,
                                int cell_x,
                                int cell_y,
                                uint32_t glyph_index,
                                uint32_t foreground_rgb,
                                uint32_t background_rgb);
void winxterm_render_draw_cell_glyph(WinxtermRenderContext *context,
                                     uint32_t *pixels,
                                     int bitmap_width,
                                     int bitmap_height,
                                     int cell_x,
                                     int cell_y,
                                     uint32_t glyph_index,
                                     uint32_t codepoint,
                                     const uint32_t *combining_codepoints,
                                     uint8_t combining_count,
                                     uint8_t width_cells,
                                     uint32_t foreground_rgb,
                                     uint32_t background_rgb);

#endif
