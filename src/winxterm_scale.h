#ifndef WINXTERM_SCALE_H
#define WINXTERM_SCALE_H

#include "winxterm_render.h"

#include <stdbool.h>
#include <limits.h>
#include <wchar.h>

#define WINXTERM_DEFAULT_DISPLAY_SCALE 1u
#define WINXTERM_MIN_DISPLAY_SCALE 1u
#define WINXTERM_MAX_DISPLAY_SCALE 100u

static inline bool winxterm_display_scale_valid(unsigned int scale)
{
    return scale >= WINXTERM_MIN_DISPLAY_SCALE && scale <= WINXTERM_MAX_DISPLAY_SCALE;
}

static inline bool winxterm_parse_display_scale_wide(const wchar_t *value, unsigned int *out)
{
    if (value == 0 || value[0] == L'\0' || out == 0) {
        return false;
    }

    unsigned int result = 0u;
    for (const wchar_t *p = value; *p != L'\0'; ++p) {
        if (*p < L'0' || *p > L'9') {
            return false;
        }
        unsigned int digit = (unsigned int)(*p - L'0');
        if (result > (UINT_MAX - digit) / 10u) {
            return false;
        }
        result = result * 10u + digit;
    }

    if (!winxterm_display_scale_valid(result)) {
        return false;
    }
    *out = result;
    return true;
}

static inline int winxterm_scale_multiply_int(int value, unsigned int scale)
{
    if (value <= 0 || scale == 0u) {
        return 0;
    }
    if ((unsigned int)value > (unsigned int)(INT_MAX / (int)scale)) {
        return INT_MAX;
    }
    return value * (int)scale;
}

static inline int winxterm_physical_to_logical_pixels(int pixels, unsigned int scale)
{
    if (pixels <= 0 || scale == 0u) {
        return 0;
    }
    return pixels / (int)scale;
}

static inline int winxterm_logical_to_physical_pixels(int pixels, unsigned int scale)
{
    return winxterm_scale_multiply_int(pixels, scale);
}

static inline int winxterm_columns_to_physical_pixels(int columns, unsigned int scale)
{
    if (columns <= 0 || columns > INT_MAX / WINXTERM_CELL_WIDTH_PIXELS) {
        return 0;
    }
    return winxterm_logical_to_physical_pixels(columns * WINXTERM_CELL_WIDTH_PIXELS, scale);
}

static inline int winxterm_rows_to_physical_pixels(int rows, unsigned int scale)
{
    if (rows <= 0 || rows > INT_MAX / WINXTERM_CELL_HEIGHT_PIXELS) {
        return 0;
    }
    return winxterm_logical_to_physical_pixels(rows * WINXTERM_CELL_HEIGHT_PIXELS, scale);
}

static inline WinxtermCellSize winxterm_physical_pixels_to_cells(int pixel_width,
                                                                 int pixel_height,
                                                                 unsigned int scale)
{
    return winxterm_pixels_to_cells(winxterm_physical_to_logical_pixels(pixel_width, scale),
                                    winxterm_physical_to_logical_pixels(pixel_height, scale));
}

#endif
