#ifndef WINXTERM_SURFACE_H
#define WINXTERM_SURFACE_H

#include <stdbool.h>
#include <stdint.h>
#include <windows.h>

typedef struct WinxtermSurface {
    HDC memory_dc;
    HBITMAP dib;
    HBITMAP previous_bitmap;
    uint32_t *pixels;
    int width;
    int height;
    int stride_pixels;
    uint64_t generation;
    uint64_t painted_generation;
    bool gdi_access_pending;
} WinxtermSurface;

bool winxterm_surface_init(WinxtermSurface *surface, HDC reference_dc,
                           int width, int height);
bool winxterm_surface_resize(WinxtermSurface *surface, HDC reference_dc,
                             int width, int height);
void winxterm_surface_flush_before_cpu_write(WinxtermSurface *surface);
void winxterm_surface_note_gdi_access(WinxtermSurface *surface);
void winxterm_surface_dispose(WinxtermSurface *surface);

#endif
