#include "winxterm_surface.h"

#include <limits.h>
#include <string.h>

static bool winxterm_surface_create(WinxtermSurface *surface,
                                    HDC reference_dc,
                                    int width,
                                    int height)
{
    if (surface == 0 || width <= 0 || height <= 0 ||
        width > INT_MAX / (int)sizeof(uint32_t)) {
        return false;
    }

    WinxtermSurface created;
    memset(&created, 0, sizeof(created));
    created.memory_dc = CreateCompatibleDC(reference_dc);
    if (created.memory_dc == 0) {
        return false;
    }

    BITMAPINFO info;
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(info.bmiHeader);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    void *bits = 0;
    created.dib = CreateDIBSection(reference_dc, &info, DIB_RGB_COLORS,
                                   &bits, 0, 0u);
    if (created.dib == 0 || bits == 0) {
        if (created.dib != 0) {
            DeleteObject(created.dib);
        }
        DeleteDC(created.memory_dc);
        return false;
    }
    created.previous_bitmap =
        (HBITMAP)SelectObject(created.memory_dc, created.dib);
    if (created.previous_bitmap == 0 || created.previous_bitmap == HGDI_ERROR) {
        DeleteObject(created.dib);
        DeleteDC(created.memory_dc);
        return false;
    }
    created.pixels = (uint32_t *)bits;
    created.width = width;
    created.height = height;
    created.stride_pixels = width;
    *surface = created;
    return true;
}

bool winxterm_surface_init(WinxtermSurface *surface, HDC reference_dc,
                           int width, int height)
{
    if (surface == 0) {
        return false;
    }
    memset(surface, 0, sizeof(*surface));
    return winxterm_surface_create(surface, reference_dc, width, height);
}

bool winxterm_surface_resize(WinxtermSurface *surface, HDC reference_dc,
                             int width, int height)
{
    if (surface == 0 || width <= 0 || height <= 0) {
        return false;
    }
    if (surface->pixels != 0 && surface->width == width &&
        surface->height == height) {
        return true;
    }
    WinxtermSurface replacement;
    if (!winxterm_surface_create(&replacement, reference_dc, width, height)) {
        return false;
    }
    replacement.generation = surface->generation;
    replacement.painted_generation = surface->painted_generation;
    WinxtermSurface old = *surface;
    *surface = replacement;
    winxterm_surface_dispose(&old);
    return true;
}

void winxterm_surface_flush_before_cpu_write(WinxtermSurface *surface)
{
    if (surface != 0 && surface->gdi_access_pending) {
        (void)GdiFlush();
        surface->gdi_access_pending = false;
    }
}

void winxterm_surface_note_gdi_access(WinxtermSurface *surface)
{
    if (surface != 0) {
        surface->gdi_access_pending = true;
    }
}

void winxterm_surface_dispose(WinxtermSurface *surface)
{
    if (surface == 0) {
        return;
    }
    if (surface->memory_dc != 0 && surface->previous_bitmap != 0 &&
        surface->previous_bitmap != HGDI_ERROR) {
        (void)SelectObject(surface->memory_dc, surface->previous_bitmap);
    }
    if (surface->dib != 0) {
        DeleteObject(surface->dib);
    }
    if (surface->memory_dc != 0) {
        DeleteDC(surface->memory_dc);
    }
    memset(surface, 0, sizeof(*surface));
}
