#ifndef WINXTERM_FONT_6X13_COMPAT_H
#define WINXTERM_FONT_6X13_COMPAT_H

#include "../resources/winxterm_font_6x13.h"

#define WINXTERM_FONT_6X13_GLYPH_COUNT WINXTERM_FONT_6X13_TOTAL_GLYPH_COUNT
#define WINXTERM_FONT_6X13_GLYPH_DATA_AVAILABLE 1
#define WINXTERM_FONT_6X13_SOURCE_PATH "resources/6x13-ISO8859-1.pcf"

typedef struct WinxtermFont6x13 {
    int width;
    int height;
    int glyph_count;
} WinxtermFont6x13;

static const WinxtermFont6x13 winxterm_font_6x13 = {
    (int)WINXTERM_FONT_6X13_WIDTH,
    (int)WINXTERM_FONT_6X13_HEIGHT,
    (int)WINXTERM_FONT_6X13_TOTAL_GLYPH_COUNT,
};

#endif
