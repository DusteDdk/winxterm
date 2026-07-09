#include "winxterm_render.h"

#include "winxterm_diagnostics.h"
#include "winxterm_font_6x13.h"
#include "winxterm_glyph_fallback.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#if defined(_M_IX86) || defined(_M_X64) || defined(__i386__) || defined(__x86_64__)
#include <immintrin.h>
#define WINXTERM_RENDER_HAS_AVX2 1
#else
#define WINXTERM_RENDER_HAS_AVX2 0
#endif

#if WINXTERM_RENDER_HAS_AVX2
static __m256i winxterm_row_masks_avx2[64];
#endif
static bool winxterm_row_mask_table_initialized;

const char *winxterm_render_backend_name(WinxtermRenderBackend backend)
{
    switch (backend) {
    case WINXTERM_RENDER_BACKEND_SPANS:
        return "spans";
    case WINXTERM_RENDER_BACKEND_ROW_MASKS:
        return "row-masks";
    case WINXTERM_RENDER_BACKEND_PRECLORED_CACHE:
        return "precolored-cache";
    default:
        return "unknown";
    }
}

uint32_t winxterm_render_bgra_from_rgb(uint32_t rgb)
{
    return rgb & 0x00ffffffu;
}

void winxterm_render_context_init(WinxtermRenderContext *context)
{
    if (context != 0) {
        memset(context, 0, sizeof(*context));
        context->glyph_fallback = winxterm_glyph_fallback_create();
    }
}

bool winxterm_render_context_load_fallback_fonts(WinxtermRenderContext *context,
                                                 void *module_instance,
                                                 WinxtermLog *log)
{
    if (context == 0) {
        return false;
    }
    if (context->glyph_fallback == 0) {
        context->glyph_fallback = winxterm_glyph_fallback_create();
    }
    return winxterm_glyph_fallback_load_resources(context->glyph_fallback, module_instance, log);
}

void winxterm_render_context_dispose(WinxtermRenderContext *context)
{
    if (context == 0) {
        return;
    }

    free(context->precolored_glyphs);
    winxterm_glyph_fallback_destroy(context->glyph_fallback);
    memset(context, 0, sizeof(*context));
}

bool winxterm_render_context_fallback_ready(const WinxtermRenderContext *context)
{
    return context != 0 && winxterm_glyph_fallback_ready(context->glyph_fallback);
}

size_t winxterm_render_context_fallback_cached_count(const WinxtermRenderContext *context)
{
    return context != 0 ? winxterm_glyph_fallback_cached_count(context->glyph_fallback) : 0u;
}

size_t winxterm_render_context_fallback_miss_count(const WinxtermRenderContext *context)
{
    return context != 0 ? winxterm_glyph_fallback_miss_count(context->glyph_fallback) : 0u;
}

void winxterm_render_clear(uint32_t *pixels, int width, int height, uint32_t rgb)
{
    if (pixels == 0 || width <= 0 || height <= 0) {
        return;
    }

    uint32_t color = winxterm_render_bgra_from_rgb(rgb);
    size_t count = (size_t)width * (size_t)height;
    for (size_t i = 0; i < count; ++i) {
        pixels[i] = color;
    }
}

void winxterm_render_clear_rect(uint32_t *pixels,
                                int bitmap_width,
                                int bitmap_height,
                                int x,
                                int y,
                                int width,
                                int height,
                                uint32_t rgb)
{
    if (pixels == 0 || bitmap_width <= 0 || bitmap_height <= 0 || width <= 0 || height <= 0) {
        return;
    }

    int left = x < 0 ? 0 : x;
    int top = y < 0 ? 0 : y;
    int right = x + width > bitmap_width ? bitmap_width : x + width;
    int bottom = y + height > bitmap_height ? bitmap_height : y + height;
    if (left >= right || top >= bottom) {
        return;
    }

    uint32_t color = winxterm_render_bgra_from_rgb(rgb);
    for (int row = top; row < bottom; ++row) {
        uint32_t *dst = pixels + (size_t)row * (size_t)bitmap_width + (size_t)left;
        for (int col = left; col < right; ++col) {
            *dst++ = color;
        }
    }
}

void winxterm_render_scroll_lines(uint32_t *front_pixels,
                                  uint32_t *back_pixels,
                                  int width,
                                  int height,
                                  int line_delta,
                                  uint32_t background_rgb)
{
    if (front_pixels == 0 || back_pixels == 0 || width <= 0 || height <= 0) {
        return;
    }

    int pixel_delta = line_delta * WINXTERM_CELL_HEIGHT_PIXELS;
    if (pixel_delta == 0) {
        memcpy(back_pixels, front_pixels, (size_t)width * (size_t)height * sizeof(uint32_t));
        return;
    }

    if (pixel_delta >= height || -pixel_delta >= height) {
        winxterm_render_clear(back_pixels, width, height, background_rgb);
        return;
    }

    uint32_t color = winxterm_render_bgra_from_rgb(background_rgb);
    if (pixel_delta > 0) {
        size_t copy_rows = (size_t)(height - pixel_delta);
        memcpy(back_pixels,
               front_pixels + (size_t)pixel_delta * (size_t)width,
               copy_rows * (size_t)width * sizeof(uint32_t));
        uint32_t *clear_start = back_pixels + copy_rows * (size_t)width;
        for (size_t i = 0; i < (size_t)pixel_delta * (size_t)width; ++i) {
            clear_start[i] = color;
        }
    } else {
        int down_pixels = -pixel_delta;
        size_t copy_rows = (size_t)(height - down_pixels);
        memcpy(back_pixels + (size_t)down_pixels * (size_t)width,
               front_pixels,
               copy_rows * (size_t)width * sizeof(uint32_t));
        for (size_t i = 0; i < (size_t)down_pixels * (size_t)width; ++i) {
            back_pixels[i] = color;
        }
    }
}

void winxterm_render_swap(uint32_t **front_pixels, uint32_t **back_pixels)
{
    if (front_pixels == 0 || back_pixels == 0) {
        return;
    }

    uint32_t *tmp = *front_pixels;
    *front_pixels = *back_pixels;
    *back_pixels = tmp;
}

static uint32_t winxterm_render_clamp_glyph(uint32_t glyph_index)
{
    return glyph_index < WINXTERM_FONT_6X13_TOTAL_GLYPH_COUNT ? glyph_index : WINXTERM_FONT_6X13_FALLBACK_GLYPH_INDEX;
}

static void winxterm_render_draw_spans(uint32_t *pixels,
                                       int bitmap_width,
                                       int origin_x,
                                       int origin_y,
                                       uint32_t glyph_index,
                                       uint32_t foreground,
                                       uint32_t background)
{
    bool draw_background = background != winxterm_render_bgra_from_rgb(WINXTERM_DEFAULT_BACKGROUND_RGB);
    for (int row = 0; row < WINXTERM_CELL_HEIGHT_PIXELS; ++row) {
        const WinxtermFont6x13Row *font_row = &winxterm_font_6x13_rows[glyph_index][row];

        if (draw_background) {
            for (uint8_t i = 0; i < font_row->background_span_count; ++i) {
                const WinxtermFont6x13Span *span = &font_row->background_spans[i];
                uint32_t *dst = pixels + (size_t)(origin_y + row) * (size_t)bitmap_width +
                                (size_t)(origin_x + span->x);
                for (uint8_t col = 0; col < span->length; ++col) {
                    dst[col] = background;
                }
            }
        }

        for (uint8_t i = 0; i < font_row->foreground_span_count; ++i) {
            const WinxtermFont6x13Span *span = &font_row->foreground_spans[i];
            uint32_t *dst = pixels + (size_t)(origin_y + row) * (size_t)bitmap_width +
                            (size_t)(origin_x + span->x);
            for (uint8_t col = 0; col < span->length; ++col) {
                dst[col] = foreground;
            }
        }
    }
}

static void winxterm_render_draw_rowmask_scalar(uint32_t *pixels,
                                                int bitmap_width,
                                                int origin_x,
                                                int origin_y,
                                                uint32_t glyph_index,
                                                uint32_t foreground,
                                                uint32_t background)
{
    bool draw_background = background != winxterm_render_bgra_from_rgb(WINXTERM_DEFAULT_BACKGROUND_RGB);
    for (int row = 0; row < WINXTERM_CELL_HEIGHT_PIXELS; ++row) {
        uint8_t mask = winxterm_font_6x13_rows[glyph_index][row].foreground_mask;
        uint32_t *dst = pixels + (size_t)(origin_y + row) * (size_t)bitmap_width + (size_t)origin_x;
        for (int col = 0; col < WINXTERM_CELL_WIDTH_PIXELS; ++col) {
            uint8_t bit = (uint8_t)(1u << (WINXTERM_CELL_WIDTH_PIXELS - 1 - col));
            if ((mask & bit) != 0) {
                dst[col] = foreground;
            } else if (draw_background) {
                dst[col] = background;
            }
        }
    }
}

#if WINXTERM_RENDER_HAS_AVX2
static void winxterm_render_init_row_mask_table(void)
{
    if (winxterm_row_mask_table_initialized) {
        return;
    }

    for (int mask = 0; mask < 64; ++mask) {
        int lanes[8] = {0, 0, 0, 0, 0, 0, 0, 0};
        for (int col = 0; col < WINXTERM_CELL_WIDTH_PIXELS; ++col) {
            int bit = 1 << (WINXTERM_CELL_WIDTH_PIXELS - 1 - col);
            lanes[col] = (mask & bit) != 0 ? -1 : 0;
        }
        winxterm_row_masks_avx2[mask] =
            _mm256_set_epi32(lanes[7], lanes[6], lanes[5], lanes[4], lanes[3], lanes[2], lanes[1], lanes[0]);
    }

    winxterm_row_mask_table_initialized = true;
}
#endif

static void winxterm_render_draw_rowmask(uint32_t *pixels,
                                         int bitmap_width,
                                         int origin_x,
                                         int origin_y,
                                         uint32_t glyph_index,
                                         uint32_t foreground,
                                         uint32_t background)
{
#if WINXTERM_RENDER_HAS_AVX2
    if (origin_x + 8 <= bitmap_width && background == winxterm_render_bgra_from_rgb(WINXTERM_DEFAULT_BACKGROUND_RGB)) {
        winxterm_render_init_row_mask_table();
        __m256i color = _mm256_set1_epi32((int)foreground);
        for (int row = 0; row < WINXTERM_CELL_HEIGHT_PIXELS; ++row) {
            uint8_t mask = winxterm_font_6x13_rows[glyph_index][row].foreground_mask & 0x3fu;
            uint32_t *dst = pixels + (size_t)(origin_y + row) * (size_t)bitmap_width + (size_t)origin_x;
            _mm256_maskstore_epi32((int *)dst, winxterm_row_masks_avx2[mask], color);
        }
        return;
    }
#endif

    winxterm_render_draw_rowmask_scalar(pixels,
                                        bitmap_width,
                                        origin_x,
                                        origin_y,
                                        glyph_index,
                                        foreground,
                                        background);
}

static WinxtermPrecoloredGlyph *winxterm_render_find_precolored(WinxtermRenderContext *context,
                                                                uint32_t glyph_index,
                                                                uint32_t foreground,
                                                                uint32_t background)
{
    if (context == 0) {
        return 0;
    }

    for (size_t i = 0; i < context->precolored_count; ++i) {
        WinxtermPrecoloredGlyph *entry = &context->precolored_glyphs[i];
        if (entry->glyph_index == glyph_index && entry->foreground_rgb == foreground &&
            entry->background_rgb == background) {
            if (context->active_diagnostics != 0) {
                winxterm_diag_inc_u64(&context->active_diagnostics->precolored_cache_hits);
                winxterm_diag_inc_u64(&context->active_diagnostics->glyph_cache_hits);
            }
            return entry;
        }
    }
    if (context->active_diagnostics != 0) {
        winxterm_diag_inc_u64(&context->active_diagnostics->precolored_cache_misses);
        winxterm_diag_inc_u64(&context->active_diagnostics->glyph_cache_misses);
    }

    if (context->precolored_count == context->precolored_capacity) {
        size_t new_capacity = context->precolored_capacity == 0 ? 64u : context->precolored_capacity * 2u;
        WinxtermPrecoloredGlyph *new_entries =
            (WinxtermPrecoloredGlyph *)realloc(context->precolored_glyphs, new_capacity * sizeof(*new_entries));
        if (new_entries == 0) {
            return 0;
        }
        context->precolored_glyphs = new_entries;
        context->precolored_capacity = new_capacity;
    }

    WinxtermPrecoloredGlyph *entry = &context->precolored_glyphs[context->precolored_count++];
    entry->glyph_index = glyph_index;
    entry->foreground_rgb = foreground;
    entry->background_rgb = background;
    for (int row = 0; row < WINXTERM_CELL_HEIGHT_PIXELS; ++row) {
        uint8_t mask = winxterm_font_6x13_rows[glyph_index][row].foreground_mask;
        for (int col = 0; col < WINXTERM_CELL_WIDTH_PIXELS; ++col) {
            uint8_t bit = (uint8_t)(1u << (WINXTERM_CELL_WIDTH_PIXELS - 1 - col));
            entry->pixels[row * WINXTERM_CELL_WIDTH_PIXELS + col] = (mask & bit) != 0 ? foreground : background;
        }
    }
    return entry;
}

static void winxterm_render_draw_precolored(WinxtermRenderContext *context,
                                            uint32_t *pixels,
                                            int bitmap_width,
                                            int origin_x,
                                            int origin_y,
                                            uint32_t glyph_index,
                                            uint32_t foreground,
                                            uint32_t background)
{
    WinxtermPrecoloredGlyph *entry = winxterm_render_find_precolored(context, glyph_index, foreground, background);
    if (entry == 0) {
        winxterm_render_draw_spans(pixels, bitmap_width, origin_x, origin_y, glyph_index, foreground, background);
        return;
    }

    for (int row = 0; row < WINXTERM_CELL_HEIGHT_PIXELS; ++row) {
        uint32_t *dst = pixels + (size_t)(origin_y + row) * (size_t)bitmap_width + (size_t)origin_x;
        const uint32_t *src = entry->pixels + row * WINXTERM_CELL_WIDTH_PIXELS;
        memcpy(dst, src, WINXTERM_CELL_WIDTH_PIXELS * sizeof(uint32_t));
    }
}

static uint32_t winxterm_render_blend_channel(uint32_t foreground, uint32_t background, uint8_t alpha)
{
    uint32_t inverse = 255u - (uint32_t)alpha;
    return ((foreground * (uint32_t)alpha) + (background * inverse) + 127u) / 255u;
}

static uint32_t winxterm_render_blend_rgb(uint32_t foreground, uint32_t background, uint8_t alpha)
{
    if (alpha == 0u) {
        return background;
    }
    if (alpha == 255u) {
        return foreground;
    }
    uint32_t red = winxterm_render_blend_channel((foreground >> 16) & 0xffu,
                                                 (background >> 16) & 0xffu,
                                                 alpha);
    uint32_t green = winxterm_render_blend_channel((foreground >> 8) & 0xffu,
                                                   (background >> 8) & 0xffu,
                                                   alpha);
    uint32_t blue = winxterm_render_blend_channel(foreground & 0xffu, background & 0xffu, alpha);
    return (red << 16) | (green << 8) | blue;
}

void winxterm_render_blend_rect_outline(uint32_t *pixels,
                                        int bitmap_width,
                                        int bitmap_height,
                                        int x,
                                        int y,
                                        int width,
                                        int height,
                                        uint32_t rgb,
                                        uint8_t alpha)
{
    if (pixels == 0 || bitmap_width <= 0 || bitmap_height <= 0 || width <= 0 || height <= 0 || alpha == 0u) {
        return;
    }

    int left = x < 0 ? 0 : x;
    int top = y < 0 ? 0 : y;
    int right = x + width > bitmap_width ? bitmap_width : x + width;
    int bottom = y + height > bitmap_height ? bitmap_height : y + height;
    if (left >= right || top >= bottom) {
        return;
    }

    uint32_t foreground = winxterm_render_bgra_from_rgb(rgb);
    for (int col = left; col < right; ++col) {
        uint32_t *top_pixel = pixels + (size_t)top * (size_t)bitmap_width + (size_t)col;
        *top_pixel = winxterm_render_blend_rgb(foreground, *top_pixel, alpha);
        if (bottom - 1 != top) {
            uint32_t *bottom_pixel = pixels + (size_t)(bottom - 1) * (size_t)bitmap_width + (size_t)col;
            *bottom_pixel = winxterm_render_blend_rgb(foreground, *bottom_pixel, alpha);
        }
    }
    for (int row = top + 1; row < bottom - 1; ++row) {
        uint32_t *left_pixel = pixels + (size_t)row * (size_t)bitmap_width + (size_t)left;
        *left_pixel = winxterm_render_blend_rgb(foreground, *left_pixel, alpha);
        if (right - 1 != left) {
            uint32_t *right_pixel = pixels + (size_t)row * (size_t)bitmap_width + (size_t)(right - 1);
            *right_pixel = winxterm_render_blend_rgb(foreground, *right_pixel, alpha);
        }
    }
}

static void winxterm_render_draw_fallback_bitmap(uint32_t *pixels,
                                                 int bitmap_width,
                                                 int origin_x,
                                                 int origin_y,
                                                 const WinxtermGlyphFallbackBitmap *bitmap,
                                                 uint32_t foreground,
                                                 uint32_t background)
{
    if (bitmap == 0) {
        return;
    }
    bool draw_background = background != winxterm_render_bgra_from_rgb(WINXTERM_DEFAULT_BACKGROUND_RGB);
    if (draw_background) {
        for (int row = 0; row < bitmap->height_pixels; ++row) {
            uint32_t *dst = pixels + (size_t)(origin_y + row) * (size_t)bitmap_width + (size_t)origin_x;
            for (int col = 0; col < bitmap->width_pixels; ++col) {
                dst[col] = background;
            }
        }
    }

    for (int row = 0; row < bitmap->height_pixels; ++row) {
        uint32_t *dst = pixels + (size_t)(origin_y + row) * (size_t)bitmap_width + (size_t)origin_x;
        const uint8_t *alpha = bitmap->alpha + (size_t)row * (size_t)WINXTERM_FALLBACK_MAX_WIDTH_PIXELS;
        const uint32_t *src = bitmap->pixels + (size_t)row * (size_t)WINXTERM_FALLBACK_MAX_WIDTH_PIXELS;
        for (int col = 0; col < bitmap->width_pixels; ++col) {
            if (bitmap->color_pixels) {
                dst[col] = winxterm_render_blend_rgb(src[col], dst[col], alpha[col]);
            } else if (alpha[col] != 0u) {
                dst[col] = winxterm_render_blend_rgb(foreground, dst[col], alpha[col]);
            }
        }
    }
}

void winxterm_render_draw_glyph(WinxtermRenderContext *context,
                                uint32_t *pixels,
                                int bitmap_width,
                                int bitmap_height,
                                int cell_x,
                                int cell_y,
                                uint32_t glyph_index,
                                uint32_t foreground_rgb,
                                uint32_t background_rgb,
                                WinxtermRenderBackend backend)
{
    if (pixels == 0 || bitmap_width <= 0 || bitmap_height <= 0 || cell_x < 0 || cell_y < 0) {
        return;
    }

    int origin_x = cell_x * WINXTERM_CELL_WIDTH_PIXELS;
    int origin_y = cell_y * WINXTERM_CELL_HEIGHT_PIXELS;
    if (origin_x + WINXTERM_CELL_WIDTH_PIXELS > bitmap_width ||
        origin_y + WINXTERM_CELL_HEIGHT_PIXELS > bitmap_height) {
        return;
    }

    uint32_t glyph = winxterm_render_clamp_glyph(glyph_index);
    uint32_t foreground = winxterm_render_bgra_from_rgb(foreground_rgb);
    uint32_t background = winxterm_render_bgra_from_rgb(background_rgb);
    if (context != 0 && context->active_diagnostics != 0) {
        winxterm_diag_inc_u64(&context->active_diagnostics->glyph_draw_calls);
        winxterm_diag_inc_u64(&context->active_diagnostics->glyph_rendered_count);
    }

    switch (backend) {
    case WINXTERM_RENDER_BACKEND_ROW_MASKS:
        winxterm_render_draw_rowmask(pixels, bitmap_width, origin_x, origin_y, glyph, foreground, background);
        break;
    case WINXTERM_RENDER_BACKEND_PRECLORED_CACHE:
        winxterm_render_draw_precolored(context,
                                        pixels,
                                        bitmap_width,
                                        origin_x,
                                        origin_y,
                                        glyph,
                                        foreground,
                                        background);
        break;
    case WINXTERM_RENDER_BACKEND_SPANS:
    default:
        winxterm_render_draw_spans(pixels, bitmap_width, origin_x, origin_y, glyph, foreground, background);
        break;
    }
}

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
                                     uint32_t background_rgb,
                                     WinxtermRenderBackend backend)
{
    if (pixels == 0 || bitmap_width <= 0 || bitmap_height <= 0 || cell_x < 0 || cell_y < 0) {
        return;
    }
    if (width_cells == 0u) {
        width_cells = 1u;
    } else if (width_cells > WINXTERM_FALLBACK_MAX_WIDTH_CELLS) {
        width_cells = WINXTERM_FALLBACK_MAX_WIDTH_CELLS;
    }

    bool dynamic_glyph = glyph_index == WINXTERM_DYNAMIC_GLYPH_INDEX || codepoint > 0xffu;
    if (dynamic_glyph) {
        width_cells = 1u;
    }

    int origin_x = cell_x * WINXTERM_CELL_WIDTH_PIXELS;
    int origin_y = cell_y * WINXTERM_CELL_HEIGHT_PIXELS;
    int fallback_width_pixels = (int)width_cells * WINXTERM_CELL_WIDTH_PIXELS;
    if (origin_x + WINXTERM_CELL_WIDTH_PIXELS > bitmap_width ||
        origin_y + WINXTERM_CELL_HEIGHT_PIXELS > bitmap_height) {
        return;
    }

    if (dynamic_glyph) {
        if (context != 0 && origin_x + fallback_width_pixels <= bitmap_width) {
            size_t misses_before = winxterm_render_context_fallback_miss_count(context);
            const WinxtermGlyphFallbackBitmap *fallback =
                winxterm_glyph_fallback_get_glyph(context->glyph_fallback,
                                                  codepoint,
                                                  combining_codepoints,
                                                  combining_count,
                                                  width_cells);
            if (context->active_diagnostics != 0) {
                size_t misses_after = winxterm_render_context_fallback_miss_count(context);
                if (misses_after > misses_before) {
                    winxterm_diag_inc_u64(&context->active_diagnostics->fallback_cache_misses);
                    winxterm_diag_inc_u64(&context->active_diagnostics->glyph_cache_misses);
                } else if (fallback != 0) {
                    winxterm_diag_inc_u64(&context->active_diagnostics->fallback_cache_hits);
                    winxterm_diag_inc_u64(&context->active_diagnostics->glyph_cache_hits);
                }
            }
            if (fallback != 0) {
                if (context->active_diagnostics != 0) {
                    winxterm_diag_inc_u64(&context->active_diagnostics->glyph_draw_calls);
                    winxterm_diag_inc_u64(&context->active_diagnostics->glyph_rendered_count);
                }
                winxterm_render_draw_fallback_bitmap(pixels,
                                                     bitmap_width,
                                                     origin_x,
                                                     origin_y,
                                                     fallback,
                                                     winxterm_render_bgra_from_rgb(foreground_rgb),
                                                     winxterm_render_bgra_from_rgb(background_rgb));
            }
        }
        return;
    }

    uint32_t glyph = winxterm_render_clamp_glyph(glyph_index);
    winxterm_render_draw_glyph(context,
                               pixels,
                               bitmap_width,
                               bitmap_height,
                               cell_x,
                               cell_y,
                               glyph,
                               foreground_rgb,
                               background_rgb,
                               backend);
}
