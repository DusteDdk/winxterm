#include "winxterm_glyph_fallback.h"

#include "../resources/resource.h"
#include "winxterm_log.h"

#include <d2d1_3.h>
#include <d3d11.h>
#include <dwrite_3.h>
#include <dxgi.h>
#include <math.h>
#include <new>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define WINXTERM_FALLBACK_CACHE_LIMIT 1024u
#define WINXTERM_FALLBACK_SCRATCH_WIDTH_PIXELS 128
#define WINXTERM_FALLBACK_SCRATCH_HEIGHT_PIXELS 128

class WinxtermMemoryFontStream final : public IDWriteFontFileStream {
public:
    WinxtermMemoryFontStream(const uint8_t *data, UINT64 size) : ref_count_(1), data_(data), size_(size) {}

    IFACEMETHOD(QueryInterface)(REFIID riid, void **object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteFontFileStream)) {
            *object = static_cast<IDWriteFontFileStream *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHOD_(ULONG, AddRef)() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
    }

    IFACEMETHOD_(ULONG, Release)() override
    {
        ULONG count = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
        if (count == 0u) {
            delete this;
        }
        return count;
    }

    IFACEMETHOD(ReadFileFragment)(const void **fragment_start,
                                  UINT64 file_offset,
                                  UINT64 fragment_size,
                                  void **fragment_context) override
    {
        if (fragment_start == nullptr || fragment_context == nullptr) {
            return E_POINTER;
        }
        if (file_offset > size_ || fragment_size > size_ - file_offset) {
            *fragment_start = nullptr;
            *fragment_context = nullptr;
            return E_FAIL;
        }
        *fragment_start = data_ + file_offset;
        *fragment_context = nullptr;
        return S_OK;
    }

    IFACEMETHOD_(void, ReleaseFileFragment)(void *fragment_context) override
    {
        (void)fragment_context;
    }

    IFACEMETHOD(GetFileSize)(UINT64 *file_size) override
    {
        if (file_size == nullptr) {
            return E_POINTER;
        }
        *file_size = size_;
        return S_OK;
    }

    IFACEMETHOD(GetLastWriteTime)(UINT64 *last_write_time) override
    {
        if (last_write_time == nullptr) {
            return E_POINTER;
        }
        *last_write_time = 0u;
        return E_NOTIMPL;
    }

private:
    volatile LONG ref_count_;
    const uint8_t *data_;
    UINT64 size_;
};

class WinxtermMemoryFontLoader final : public IDWriteFontFileLoader {
public:
    WinxtermMemoryFontLoader(const uint8_t *data, UINT32 size) : ref_count_(1), data_(data), size_(size) {}

    IFACEMETHOD(QueryInterface)(REFIID riid, void **object) override
    {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteFontFileLoader)) {
            *object = static_cast<IDWriteFontFileLoader *>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    IFACEMETHOD_(ULONG, AddRef)() override
    {
        return static_cast<ULONG>(InterlockedIncrement(&ref_count_));
    }

    IFACEMETHOD_(ULONG, Release)() override
    {
        ULONG count = static_cast<ULONG>(InterlockedDecrement(&ref_count_));
        if (count == 0u) {
            delete this;
        }
        return count;
    }

    IFACEMETHOD(CreateStreamFromKey)(const void *font_file_reference_key,
                                     UINT32 font_file_reference_key_size,
                                     IDWriteFontFileStream **font_file_stream) override
    {
        (void)font_file_reference_key;
        (void)font_file_reference_key_size;
        if (font_file_stream == nullptr) {
            return E_POINTER;
        }
        WinxtermMemoryFontStream *stream = new (std::nothrow) WinxtermMemoryFontStream(data_, size_);
        if (stream == nullptr) {
            *font_file_stream = nullptr;
            return E_OUTOFMEMORY;
        }
        *font_file_stream = stream;
        return S_OK;
    }

private:
    volatile LONG ref_count_;
    const uint8_t *data_;
    UINT32 size_;
};

typedef struct WinxtermFallbackFont {
    WinxtermGlyphFallbackFontKind kind;
    int resource_id;
    const char *name;
    WinxtermMemoryFontLoader *loader;
    IDWriteFontFile *file;
    IDWriteFontFace *face;
    bool available;
} WinxtermFallbackFont;

typedef struct WinxtermFallbackCacheEntry {
    uint32_t codepoints[WINXTERM_FALLBACK_MAX_CODEPOINTS];
    uint8_t codepoint_count;
    uint8_t width_cells;
    WinxtermGlyphFallbackFontKind font_kind;
    uint64_t last_used;
    WinxtermGlyphFallbackBitmap bitmap;
} WinxtermFallbackCacheEntry;

typedef struct WinxtermFallbackScratchBitmap {
    int width_pixels;
    int height_pixels;
    bool color_pixels;
    uint8_t *alpha;
    uint32_t *pixels;
} WinxtermFallbackScratchBitmap;

struct WinxtermGlyphFallbackState {
    IDWriteFactory *factory;
    IDWriteFactory2 *factory2;
    IDWriteFactory4 *factory4;
    ID2D1Factory8 *d2d_factory;
    ID3D11Device *d3d_device;
    IDXGIDevice *dxgi_device;
    ID2D1Device7 *d2d_device;
    ID2D1DeviceContext7 *d2d_context;
    ID2D1SolidColorBrush *d2d_foreground_brush;
    WinxtermFallbackFont fonts[WINXTERM_GLYPH_FALLBACK_FONT_COUNT];
    WinxtermFallbackCacheEntry *cache;
    size_t cache_count;
    size_t cache_capacity;
    uint64_t use_clock;
    size_t miss_count;
    bool resources_loaded;
};

static void winxterm_glyph_fallback_init_font(WinxtermFallbackFont *font,
                                              WinxtermGlyphFallbackFontKind kind,
                                              int resource_id,
                                              const char *name)
{
    memset(font, 0, sizeof(*font));
    font->kind = kind;
    font->resource_id = resource_id;
    font->name = name;
}

WinxtermGlyphFallbackState *winxterm_glyph_fallback_create(void)
{
    WinxtermGlyphFallbackState *state =
        static_cast<WinxtermGlyphFallbackState *>(calloc(1u, sizeof(*state)));
    if (state == nullptr) {
        return nullptr;
    }
    winxterm_glyph_fallback_init_font(&state->fonts[WINXTERM_GLYPH_FALLBACK_FONT_GENERAL],
                                      WINXTERM_GLYPH_FALLBACK_FONT_GENERAL,
                                      IDR_WINXTERM_FONT_GENERAL,
                                      "general");
    winxterm_glyph_fallback_init_font(&state->fonts[WINXTERM_GLYPH_FALLBACK_FONT_EMOJI],
                                      WINXTERM_GLYPH_FALLBACK_FONT_EMOJI,
                                      IDR_WINXTERM_FONT_EMOJI,
                                      "emoji");
    winxterm_glyph_fallback_init_font(&state->fonts[WINXTERM_GLYPH_FALLBACK_FONT_MATH],
                                      WINXTERM_GLYPH_FALLBACK_FONT_MATH,
                                      IDR_WINXTERM_FONT_MATH,
                                      "math");
    return state;
}

static void winxterm_glyph_fallback_dispose_font(WinxtermGlyphFallbackState *state, WinxtermFallbackFont *font)
{
    if (font->face != nullptr) {
        font->face->Release();
        font->face = nullptr;
    }
    if (font->file != nullptr) {
        font->file->Release();
        font->file = nullptr;
    }
    if (state != nullptr && state->factory != nullptr && font->loader != nullptr) {
        (void)state->factory->UnregisterFontFileLoader(font->loader);
    }
    if (font->loader != nullptr) {
        font->loader->Release();
        font->loader = nullptr;
    }
    font->available = false;
}

void winxterm_glyph_fallback_destroy(WinxtermGlyphFallbackState *state)
{
    if (state == nullptr) {
        return;
    }
    for (int i = 0; i < WINXTERM_GLYPH_FALLBACK_FONT_COUNT; ++i) {
        winxterm_glyph_fallback_dispose_font(state, &state->fonts[i]);
    }
    if (state->d2d_foreground_brush != nullptr) {
        state->d2d_foreground_brush->Release();
    }
    if (state->d2d_context != nullptr) {
        state->d2d_context->Release();
    }
    if (state->d2d_device != nullptr) {
        state->d2d_device->Release();
    }
    if (state->dxgi_device != nullptr) {
        state->dxgi_device->Release();
    }
    if (state->d3d_device != nullptr) {
        state->d3d_device->Release();
    }
    if (state->d2d_factory != nullptr) {
        state->d2d_factory->Release();
    }
    if (state->factory4 != nullptr) {
        state->factory4->Release();
    }
    if (state->factory2 != nullptr) {
        state->factory2->Release();
    }
    if (state->factory != nullptr) {
        state->factory->Release();
    }
    free(state->cache);
    free(state);
}

static bool winxterm_glyph_fallback_resource_bytes(void *module_instance,
                                                   int resource_id,
                                                   const uint8_t **bytes,
                                                   UINT32 *byte_count)
{
    HMODULE module = module_instance != nullptr ? static_cast<HMODULE>(module_instance) : GetModuleHandleW(nullptr);
    LPCWSTR resource_name = MAKEINTRESOURCEW(static_cast<WORD>(resource_id));
    LPCWSTR resource_type = MAKEINTRESOURCEW(10);
    HRSRC resource = FindResourceW(module, resource_name, resource_type);
    if (resource == nullptr) {
        return false;
    }
    DWORD size = SizeofResource(module, resource);
    HGLOBAL loaded = LoadResource(module, resource);
    const void *data = loaded != nullptr ? LockResource(loaded) : nullptr;
    if (size == 0u || data == nullptr) {
        return false;
    }
    *bytes = static_cast<const uint8_t *>(data);
    *byte_count = size;
    return true;
}

static bool winxterm_glyph_fallback_load_font(WinxtermGlyphFallbackState *state,
                                              void *module_instance,
                                              WinxtermFallbackFont *font,
                                              WinxtermLog *log)
{
    const uint8_t *bytes = nullptr;
    UINT32 byte_count = 0u;
    if (!winxterm_glyph_fallback_resource_bytes(module_instance, font->resource_id, &bytes, &byte_count)) {
        winxterm_log_writef(log, "glyph fallback font %s resource unavailable", font->name);
        return false;
    }

    font->loader = new (std::nothrow) WinxtermMemoryFontLoader(bytes, byte_count);
    if (font->loader == nullptr) {
        return false;
    }

    HRESULT hr = state->factory->RegisterFontFileLoader(font->loader);
    if (FAILED(hr)) {
        winxterm_log_writef(log, "glyph fallback font %s loader registration failed: 0x%08lx",
                            font->name,
                            static_cast<unsigned long>(hr));
        winxterm_glyph_fallback_dispose_font(state, font);
        return false;
    }

    UINT32 key = static_cast<UINT32>(font->kind);
    hr = state->factory->CreateCustomFontFileReference(&key, sizeof(key), font->loader, &font->file);
    if (FAILED(hr)) {
        winxterm_log_writef(log, "glyph fallback font %s file reference failed: 0x%08lx",
                            font->name,
                            static_cast<unsigned long>(hr));
        winxterm_glyph_fallback_dispose_font(state, font);
        return false;
    }

    BOOL supported = FALSE;
    DWRITE_FONT_FILE_TYPE file_type = DWRITE_FONT_FILE_TYPE_UNKNOWN;
    DWRITE_FONT_FACE_TYPE face_type = DWRITE_FONT_FACE_TYPE_UNKNOWN;
    UINT32 face_count = 0u;
    hr = font->file->Analyze(&supported, &file_type, &face_type, &face_count);
    if (FAILED(hr) || !supported || face_count == 0u) {
        winxterm_log_writef(log, "glyph fallback font %s unsupported: 0x%08lx",
                            font->name,
                            static_cast<unsigned long>(hr));
        winxterm_glyph_fallback_dispose_font(state, font);
        return false;
    }

    hr = state->factory->CreateFontFace(face_type,
                                        1u,
                                        &font->file,
                                        0u,
                                        DWRITE_FONT_SIMULATIONS_NONE,
                                        &font->face);
    if (FAILED(hr)) {
        winxterm_log_writef(log, "glyph fallback font %s face creation failed: 0x%08lx",
                            font->name,
                            static_cast<unsigned long>(hr));
        winxterm_glyph_fallback_dispose_font(state, font);
        return false;
    }

    font->available = true;
    winxterm_log_writef(log, "glyph fallback font %s loaded, bytes=%lu", font->name, static_cast<unsigned long>(byte_count));
    return true;
}

bool winxterm_glyph_fallback_load_resources(WinxtermGlyphFallbackState *state,
                                            void *module_instance,
                                            WinxtermLog *log)
{
    if (state == nullptr) {
        return false;
    }
    if (state->resources_loaded) {
        return winxterm_glyph_fallback_ready(state);
    }

    HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                     __uuidof(IDWriteFactory),
                                     reinterpret_cast<IUnknown **>(&state->factory));
    if (FAILED(hr) || state->factory == nullptr) {
        winxterm_log_writef(log, "DirectWrite factory creation failed: 0x%08lx", static_cast<unsigned long>(hr));
        state->resources_loaded = true;
        return false;
    }
    hr = state->factory->QueryInterface(__uuidof(IDWriteFactory2), reinterpret_cast<void **>(&state->factory2));
    if (FAILED(hr)) {
        state->factory2 = nullptr;
        winxterm_log_writef(log,
                            "DirectWrite color glyph factory unavailable: 0x%08lx; emoji fallback is monochrome",
                            static_cast<unsigned long>(hr));
    }
    hr = state->factory->QueryInterface(__uuidof(IDWriteFactory4), reinterpret_cast<void **>(&state->factory4));
    if (FAILED(hr)) {
        state->factory4 = nullptr;
    }

    bool any_loaded = false;
    for (int i = 0; i < WINXTERM_GLYPH_FALLBACK_FONT_COUNT; ++i) {
        any_loaded = winxterm_glyph_fallback_load_font(state, module_instance, &state->fonts[i], log) || any_loaded;
    }
    state->resources_loaded = true;
    if (!any_loaded) {
        winxterm_log_writef(log, "no TTF glyph fallback fonts loaded; bitmap missing glyph remains active");
    }
    return any_loaded;
}

bool winxterm_glyph_fallback_ready(const WinxtermGlyphFallbackState *state)
{
    if (state == nullptr) {
        return false;
    }
    for (int i = 0; i < WINXTERM_GLYPH_FALLBACK_FONT_COUNT; ++i) {
        if (state->fonts[i].available) {
            return true;
        }
    }
    return false;
}

size_t winxterm_glyph_fallback_cached_count(const WinxtermGlyphFallbackState *state)
{
    return state != nullptr ? state->cache_count : 0u;
}

size_t winxterm_glyph_fallback_miss_count(const WinxtermGlyphFallbackState *state)
{
    return state != nullptr ? state->miss_count : 0u;
}

static bool winxterm_glyph_fallback_is_emoji(uint32_t codepoint)
{
    return (codepoint >= 0x1f000u && codepoint <= 0x1faffu) ||
           (codepoint >= 0x2600u && codepoint <= 0x27bfu) ||
           (codepoint >= 0xfe00u && codepoint <= 0xfe0fu);
}

static bool winxterm_glyph_fallback_is_math(uint32_t codepoint)
{
    return (codepoint >= 0x2190u && codepoint <= 0x22ffu) ||
           (codepoint >= 0x27c0u && codepoint <= 0x27efu) ||
           (codepoint >= 0x2980u && codepoint <= 0x29ffu) ||
           (codepoint >= 0x2a00u && codepoint <= 0x2affu) ||
           (codepoint >= 0x1d400u && codepoint <= 0x1d7ffu);
}

WinxtermGlyphFallbackFontKind winxterm_glyph_fallback_select_font(uint32_t codepoint)
{
    if (winxterm_glyph_fallback_is_emoji(codepoint)) {
        return WINXTERM_GLYPH_FALLBACK_FONT_EMOJI;
    }
    if (winxterm_glyph_fallback_is_math(codepoint)) {
        return WINXTERM_GLYPH_FALLBACK_FONT_MATH;
    }
    return WINXTERM_GLYPH_FALLBACK_FONT_GENERAL;
}

const char *winxterm_glyph_fallback_font_name(WinxtermGlyphFallbackFontKind kind)
{
    switch (kind) {
    case WINXTERM_GLYPH_FALLBACK_FONT_EMOJI:
        return "emoji";
    case WINXTERM_GLYPH_FALLBACK_FONT_MATH:
        return "math";
    case WINXTERM_GLYPH_FALLBACK_FONT_GENERAL:
    default:
        return "general";
    }
}

static bool winxterm_glyph_fallback_font_has_glyph(const WinxtermFallbackFont *font, uint32_t codepoint)
{
    if (font == nullptr || !font->available || font->face == nullptr) {
        return false;
    }
    UINT16 glyph = 0u;
    HRESULT hr = font->face->GetGlyphIndices(&codepoint, 1u, &glyph);
    return SUCCEEDED(hr) && glyph != 0u;
}

static WinxtermFallbackFont *winxterm_glyph_fallback_choose_font(WinxtermGlyphFallbackState *state,
                                                                 uint32_t codepoint,
                                                                 WinxtermGlyphFallbackFontKind preferred)
{
    static const WinxtermGlyphFallbackFontKind emoji_order[] = {
        WINXTERM_GLYPH_FALLBACK_FONT_EMOJI,
        WINXTERM_GLYPH_FALLBACK_FONT_GENERAL,
        WINXTERM_GLYPH_FALLBACK_FONT_MATH,
    };
    static const WinxtermGlyphFallbackFontKind math_order[] = {
        WINXTERM_GLYPH_FALLBACK_FONT_MATH,
        WINXTERM_GLYPH_FALLBACK_FONT_GENERAL,
        WINXTERM_GLYPH_FALLBACK_FONT_EMOJI,
    };
    static const WinxtermGlyphFallbackFontKind general_order[] = {
        WINXTERM_GLYPH_FALLBACK_FONT_GENERAL,
        WINXTERM_GLYPH_FALLBACK_FONT_MATH,
        WINXTERM_GLYPH_FALLBACK_FONT_EMOJI,
    };

    const WinxtermGlyphFallbackFontKind *order = general_order;
    if (preferred == WINXTERM_GLYPH_FALLBACK_FONT_EMOJI) {
        order = emoji_order;
    } else if (preferred == WINXTERM_GLYPH_FALLBACK_FONT_MATH) {
        order = math_order;
    }
    for (int i = 0; i < WINXTERM_GLYPH_FALLBACK_FONT_COUNT; ++i) {
        WinxtermFallbackFont *font = &state->fonts[order[i]];
        if (winxterm_glyph_fallback_font_has_glyph(font, codepoint)) {
            return font;
        }
    }
    return nullptr;
}

static bool winxterm_glyph_fallback_key_matches(const WinxtermFallbackCacheEntry *entry,
                                                const uint32_t *codepoints,
                                                uint8_t codepoint_count,
                                                uint8_t width_cells,
                                                WinxtermGlyphFallbackFontKind font_kind)
{
    return entry->codepoint_count == codepoint_count &&
           entry->width_cells == width_cells &&
           entry->font_kind == font_kind &&
           memcmp(entry->codepoints, codepoints, static_cast<size_t>(codepoint_count) * sizeof(codepoints[0])) == 0;
}

static WinxtermFallbackCacheEntry *winxterm_glyph_fallback_find_cache(WinxtermGlyphFallbackState *state,
                                                                      const uint32_t *codepoints,
                                                                      uint8_t codepoint_count,
                                                                      uint8_t width_cells,
                                                                      WinxtermGlyphFallbackFontKind font_kind)
{
    for (size_t i = 0; i < state->cache_count; ++i) {
        WinxtermFallbackCacheEntry *entry = &state->cache[i];
        if (winxterm_glyph_fallback_key_matches(entry, codepoints, codepoint_count, width_cells, font_kind)) {
            entry->last_used = ++state->use_clock;
            return entry;
        }
    }
    return nullptr;
}

static WinxtermFallbackCacheEntry *winxterm_glyph_fallback_alloc_cache_entry(WinxtermGlyphFallbackState *state)
{
    if (state->cache_count < state->cache_capacity) {
        WinxtermFallbackCacheEntry *entry = &state->cache[state->cache_count++];
        memset(entry, 0, sizeof(*entry));
        return entry;
    }
    if (state->cache_capacity < WINXTERM_FALLBACK_CACHE_LIMIT) {
        size_t new_capacity = state->cache_capacity == 0u ? 64u : state->cache_capacity * 2u;
        if (new_capacity > WINXTERM_FALLBACK_CACHE_LIMIT) {
            new_capacity = WINXTERM_FALLBACK_CACHE_LIMIT;
        }
        WinxtermFallbackCacheEntry *entries =
            static_cast<WinxtermFallbackCacheEntry *>(realloc(state->cache, new_capacity * sizeof(*entries)));
        if (entries == nullptr) {
            return nullptr;
        }
        state->cache = entries;
        state->cache_capacity = new_capacity;
        WinxtermFallbackCacheEntry *entry = &state->cache[state->cache_count++];
        memset(entry, 0, sizeof(*entry));
        return entry;
    }

    size_t evict_index = 0u;
    uint64_t oldest = state->cache[0].last_used;
    for (size_t i = 1u; i < state->cache_count; ++i) {
        if (state->cache[i].last_used < oldest) {
            oldest = state->cache[i].last_used;
            evict_index = i;
        }
    }
    WinxtermFallbackCacheEntry *entry = &state->cache[evict_index];
    memset(entry, 0, sizeof(*entry));
    return entry;
}

static bool winxterm_glyph_fallback_scratch_init(WinxtermFallbackScratchBitmap *bitmap,
                                                 int width,
                                                 int height,
                                                 bool color_pixels)
{
    if (bitmap == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    memset(bitmap, 0, sizeof(*bitmap));
    size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixel_count == 0u || pixel_count > UINT32_MAX) {
        return false;
    }
    bitmap->alpha = static_cast<uint8_t *>(calloc(pixel_count, sizeof(*bitmap->alpha)));
    bitmap->pixels = static_cast<uint32_t *>(calloc(pixel_count, sizeof(*bitmap->pixels)));
    if (bitmap->alpha == nullptr || bitmap->pixels == nullptr) {
        free(bitmap->alpha);
        free(bitmap->pixels);
        memset(bitmap, 0, sizeof(*bitmap));
        return false;
    }
    bitmap->width_pixels = width;
    bitmap->height_pixels = height;
    bitmap->color_pixels = color_pixels;
    return true;
}

static void winxterm_glyph_fallback_scratch_dispose(WinxtermFallbackScratchBitmap *bitmap)
{
    if (bitmap == nullptr) {
        return;
    }
    free(bitmap->alpha);
    free(bitmap->pixels);
    memset(bitmap, 0, sizeof(*bitmap));
}

static bool winxterm_glyph_fallback_scratch_find_bounds(const WinxtermFallbackScratchBitmap *scratch, RECT *bounds)
{
    if (scratch == nullptr || bounds == nullptr || scratch->alpha == nullptr) {
        return false;
    }

    int left = scratch->width_pixels;
    int top = scratch->height_pixels;
    int right = 0;
    int bottom = 0;
    for (int y = 0; y < scratch->height_pixels; ++y) {
        const uint8_t *row = scratch->alpha + static_cast<size_t>(y) * static_cast<size_t>(scratch->width_pixels);
        for (int x = 0; x < scratch->width_pixels; ++x) {
            if (row[x] == 0u) {
                continue;
            }
            if (x < left) {
                left = x;
            }
            if (x + 1 > right) {
                right = x + 1;
            }
            if (y < top) {
                top = y;
            }
            if (y + 1 > bottom) {
                bottom = y + 1;
            }
        }
    }
    if (left >= right || top >= bottom) {
        return false;
    }
    bounds->left = left;
    bounds->top = top;
    bounds->right = right;
    bounds->bottom = bottom;
    return true;
}

static double winxterm_glyph_fallback_min_double(double a, double b)
{
    return a < b ? a : b;
}

static double winxterm_glyph_fallback_max_double(double a, double b)
{
    return a > b ? a : b;
}

static uint8_t winxterm_glyph_fallback_byte_from_double(double value)
{
    if (value <= 0.0) {
        return 0u;
    }
    if (value >= 255.0) {
        return 255u;
    }
    return static_cast<uint8_t>(value + 0.5);
}

static bool winxterm_glyph_fallback_normalize_scratch(const WinxtermFallbackScratchBitmap *scratch,
                                                      WinxtermGlyphFallbackBitmap *bitmap)
{
    if (scratch == nullptr || bitmap == nullptr || scratch->alpha == nullptr || scratch->pixels == nullptr) {
        return false;
    }

    RECT bounds = {};
    if (!winxterm_glyph_fallback_scratch_find_bounds(scratch, &bounds)) {
        return false;
    }

    memset(bitmap, 0, sizeof(*bitmap));
    bitmap->width_pixels = WINXTERM_CELL_WIDTH_PIXELS;
    bitmap->height_pixels = WINXTERM_CELL_HEIGHT_PIXELS;
    bitmap->color_pixels = scratch->color_pixels;

    int source_width = bounds.right - bounds.left;
    int source_height = bounds.bottom - bounds.top;
    double scale = 1.0;
    if (source_width > WINXTERM_CELL_WIDTH_PIXELS || source_height > WINXTERM_CELL_HEIGHT_PIXELS) {
        double scale_x = static_cast<double>(WINXTERM_CELL_WIDTH_PIXELS) / static_cast<double>(source_width);
        double scale_y = static_cast<double>(WINXTERM_CELL_HEIGHT_PIXELS) / static_cast<double>(source_height);
        scale = winxterm_glyph_fallback_min_double(scale_x, scale_y);
    }
    if (scale <= 0.0) {
        return false;
    }

    int scaled_width = static_cast<int>(static_cast<double>(source_width) * scale + 0.5);
    int scaled_height = static_cast<int>(static_cast<double>(source_height) * scale + 0.5);
    if (scaled_width < 1) {
        scaled_width = 1;
    } else if (scaled_width > WINXTERM_CELL_WIDTH_PIXELS) {
        scaled_width = WINXTERM_CELL_WIDTH_PIXELS;
    }
    if (scaled_height < 1) {
        scaled_height = 1;
    } else if (scaled_height > WINXTERM_CELL_HEIGHT_PIXELS) {
        scaled_height = WINXTERM_CELL_HEIGHT_PIXELS;
    }

    int dst_left = (WINXTERM_CELL_WIDTH_PIXELS - scaled_width) / 2;
    int dst_top = (WINXTERM_CELL_HEIGHT_PIXELS - scaled_height) / 2;
    for (int y = 0; y < scaled_height; ++y) {
        double src_y0 = static_cast<double>(bounds.top) + static_cast<double>(y) / scale;
        double src_y1 = static_cast<double>(bounds.top) + static_cast<double>(y + 1) / scale;
        int sy0 = static_cast<int>(floor(src_y0));
        int sy1 = static_cast<int>(ceil(src_y1));
        for (int x = 0; x < scaled_width; ++x) {
            double src_x0 = static_cast<double>(bounds.left) + static_cast<double>(x) / scale;
            double src_x1 = static_cast<double>(bounds.left) + static_cast<double>(x + 1) / scale;
            int sx0 = static_cast<int>(floor(src_x0));
            int sx1 = static_cast<int>(ceil(src_x1));

            double coverage_total = 0.0;
            double alpha_total = 0.0;
            double red_total = 0.0;
            double green_total = 0.0;
            double blue_total = 0.0;
            for (int sy = sy0; sy < sy1; ++sy) {
                if (sy < bounds.top || sy >= bounds.bottom) {
                    continue;
                }
                double y_overlap = winxterm_glyph_fallback_min_double(static_cast<double>(sy + 1), src_y1) -
                                   winxterm_glyph_fallback_max_double(static_cast<double>(sy), src_y0);
                if (y_overlap <= 0.0) {
                    continue;
                }
                for (int sx = sx0; sx < sx1; ++sx) {
                    if (sx < bounds.left || sx >= bounds.right) {
                        continue;
                    }
                    double x_overlap = winxterm_glyph_fallback_min_double(static_cast<double>(sx + 1), src_x1) -
                                       winxterm_glyph_fallback_max_double(static_cast<double>(sx), src_x0);
                    if (x_overlap <= 0.0) {
                        continue;
                    }
                    double coverage = x_overlap * y_overlap;
                    size_t src_index = static_cast<size_t>(sy) * static_cast<size_t>(scratch->width_pixels) +
                                       static_cast<size_t>(sx);
                    double alpha = static_cast<double>(scratch->alpha[src_index]);
                    coverage_total += coverage;
                    alpha_total += alpha * coverage;
                    if (scratch->color_pixels && alpha > 0.0) {
                        uint32_t rgb = scratch->pixels[src_index];
                        red_total += static_cast<double>((rgb >> 16) & 0xffu) * alpha * coverage;
                        green_total += static_cast<double>((rgb >> 8) & 0xffu) * alpha * coverage;
                        blue_total += static_cast<double>(rgb & 0xffu) * alpha * coverage;
                    }
                }
            }

            if (coverage_total <= 0.0) {
                continue;
            }
            uint8_t alpha = winxterm_glyph_fallback_byte_from_double(alpha_total / coverage_total);
            if (alpha == 0u) {
                continue;
            }

            int dst_x = dst_left + x;
            int dst_y = dst_top + y;
            size_t dst_index = static_cast<size_t>(dst_y) * WINXTERM_FALLBACK_MAX_WIDTH_PIXELS +
                               static_cast<size_t>(dst_x);
            bitmap->alpha[dst_index] = alpha;
            if (scratch->color_pixels && alpha_total > 0.0) {
                uint32_t red = winxterm_glyph_fallback_byte_from_double(red_total / alpha_total);
                uint32_t green = winxterm_glyph_fallback_byte_from_double(green_total / alpha_total);
                uint32_t blue = winxterm_glyph_fallback_byte_from_double(blue_total / alpha_total);
                bitmap->pixels[dst_index] = (red << 16) | (green << 8) | blue;
            }
        }
    }

    return true;
}

static bool winxterm_glyph_fallback_create_analysis(WinxtermGlyphFallbackState *state,
                                                    const WinxtermFallbackFont *font,
                                                    UINT16 glyph,
                                                    FLOAT advance,
                                                    FLOAT baseline_x,
                                                    FLOAT baseline_y,
                                                    IDWriteGlyphRunAnalysis **analysis)
{
    DWRITE_GLYPH_OFFSET offset = {};
    DWRITE_GLYPH_RUN run = {};
    run.fontFace = font->face;
    run.fontEmSize = static_cast<FLOAT>(WINXTERM_CELL_HEIGHT_PIXELS);
    run.glyphCount = 1u;
    run.glyphIndices = &glyph;
    run.glyphAdvances = &advance;
    run.glyphOffsets = &offset;
    run.isSideways = FALSE;
    run.bidiLevel = 0u;
    return SUCCEEDED(state->factory->CreateGlyphRunAnalysis(&run,
                                                            1.0f,
                                                            nullptr,
                                                            DWRITE_RENDERING_MODE_ALIASED,
                                                            DWRITE_MEASURING_MODE_NATURAL,
                                                            baseline_x,
                                                            baseline_y,
                                                            analysis));
}

static uint8_t winxterm_glyph_fallback_color_component(FLOAT value)
{
    if (value <= 0.0f) {
        return 0u;
    }
    if (value >= 1.0f) {
        return 255u;
    }
    return static_cast<uint8_t>(value * 255.0f + 0.5f);
}

static uint32_t winxterm_glyph_fallback_rgb_from_color(DWRITE_COLOR_F color)
{
    uint32_t red = winxterm_glyph_fallback_color_component(color.r);
    uint32_t green = winxterm_glyph_fallback_color_component(color.g);
    uint32_t blue = winxterm_glyph_fallback_color_component(color.b);
    return (red << 16) | (green << 8) | blue;
}

static void winxterm_glyph_fallback_scratch_compose_color_pixel(WinxtermFallbackScratchBitmap *bitmap,
                                                                int x,
                                                                int y,
                                                                uint32_t rgb,
                                                                uint8_t alpha)
{
    if (alpha == 0u || x < 0 || y < 0 || x >= bitmap->width_pixels || y >= bitmap->height_pixels) {
        return;
    }

    size_t index = static_cast<size_t>(y) * static_cast<size_t>(bitmap->width_pixels) + static_cast<size_t>(x);
    uint8_t dst_alpha = bitmap->alpha[index];
    uint32_t dst_rgb = bitmap->pixels[index];
    uint32_t inv_alpha = 255u - static_cast<uint32_t>(alpha);
    uint32_t out_alpha = static_cast<uint32_t>(alpha) + (static_cast<uint32_t>(dst_alpha) * inv_alpha + 127u) / 255u;
    if (out_alpha == 0u) {
        bitmap->alpha[index] = 0u;
        bitmap->pixels[index] = 0u;
        return;
    }

    uint32_t denom = out_alpha * 255u;
    uint32_t src_red = (rgb >> 16) & 0xffu;
    uint32_t src_green = (rgb >> 8) & 0xffu;
    uint32_t src_blue = rgb & 0xffu;
    uint32_t dst_red = (dst_rgb >> 16) & 0xffu;
    uint32_t dst_green = (dst_rgb >> 8) & 0xffu;
    uint32_t dst_blue = dst_rgb & 0xffu;
    uint32_t red = (src_red * static_cast<uint32_t>(alpha) * 255u +
                    dst_red * static_cast<uint32_t>(dst_alpha) * inv_alpha + denom / 2u) / denom;
    uint32_t green = (src_green * static_cast<uint32_t>(alpha) * 255u +
                      dst_green * static_cast<uint32_t>(dst_alpha) * inv_alpha + denom / 2u) / denom;
    uint32_t blue = (src_blue * static_cast<uint32_t>(alpha) * 255u +
                     dst_blue * static_cast<uint32_t>(dst_alpha) * inv_alpha + denom / 2u) / denom;

    bitmap->alpha[index] = static_cast<uint8_t>(out_alpha > 255u ? 255u : out_alpha);
    bitmap->pixels[index] = (red << 16) | (green << 8) | blue;
}

static bool winxterm_glyph_fallback_raster_color_layer(WinxtermGlyphFallbackState *state,
                                                       const DWRITE_GLYPH_RUN *run,
                                                       FLOAT baseline_x,
                                                       FLOAT baseline_y,
                                                       WinxtermFallbackScratchBitmap *bitmap,
                                                       uint32_t rgb,
                                                       uint8_t layer_alpha)
{
    IDWriteGlyphRunAnalysis *analysis = nullptr;
    HRESULT hr = state->factory->CreateGlyphRunAnalysis(run,
                                                        1.0f,
                                                        nullptr,
                                                        DWRITE_RENDERING_MODE_ALIASED,
                                                        DWRITE_MEASURING_MODE_NATURAL,
                                                        baseline_x,
                                                        baseline_y,
                                                        &analysis);
    if (FAILED(hr) || analysis == nullptr) {
        return false;
    }

    RECT bounds = {};
    hr = analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &bounds);
    if (FAILED(hr) || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        analysis->Release();
        return false;
    }

    int width = bounds.right - bounds.left;
    int height = bounds.bottom - bounds.top;
    size_t buffer_size = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (buffer_size == 0u || buffer_size > UINT32_MAX) {
        analysis->Release();
        return false;
    }
    uint8_t *alpha = static_cast<uint8_t *>(malloc(buffer_size));
    if (alpha == nullptr) {
        analysis->Release();
        return false;
    }
    hr = analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &bounds, alpha, static_cast<UINT32>(buffer_size));
    analysis->Release();
    if (FAILED(hr)) {
        free(alpha);
        return false;
    }

    bool drew = false;
    for (int y = 0; y < height; ++y) {
        int dst_y = bounds.top + y;
        if (dst_y < 0 || dst_y >= bitmap->height_pixels) {
            continue;
        }
        for (int x = 0; x < width; ++x) {
            int dst_x = bounds.left + x;
            if (dst_x < 0 || dst_x >= bitmap->width_pixels) {
                continue;
            }
            uint8_t sample = alpha[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
            uint8_t blended_alpha = static_cast<uint8_t>((static_cast<uint32_t>(sample) *
                                                          static_cast<uint32_t>(layer_alpha) + 127u) / 255u);
            if (blended_alpha != 0u) {
                winxterm_glyph_fallback_scratch_compose_color_pixel(bitmap, dst_x, dst_y, rgb, blended_alpha);
                drew = true;
            }
        }
    }
    free(alpha);
    return drew;
}

static bool winxterm_glyph_fallback_build_color_run(const WinxtermFallbackFont *font,
                                                    uint32_t codepoint,
                                                    int target_width,
                                                    UINT16 *glyph,
                                                    FLOAT *advance,
                                                    FLOAT *baseline_x,
                                                    FLOAT *baseline_y,
                                                    DWRITE_GLYPH_OFFSET *offset,
                                                    DWRITE_GLYPH_RUN *run)
{
    *glyph = 0u;
    HRESULT hr = font->face->GetGlyphIndices(&codepoint, 1u, glyph);
    if (FAILED(hr) || *glyph == 0u) {
        return false;
    }

    DWRITE_FONT_METRICS metrics = {};
    font->face->GetMetrics(&metrics);
    FLOAT design_height = static_cast<FLOAT>(metrics.ascent + metrics.descent);
    FLOAT scale = design_height > 0.0f ?
        static_cast<FLOAT>(WINXTERM_CELL_HEIGHT_PIXELS) / design_height : 1.0f;
    *advance = static_cast<FLOAT>(target_width);
    *baseline_x = 0.0f;
    *baseline_y = static_cast<FLOAT>(metrics.ascent) * scale;

    memset(offset, 0, sizeof(*offset));
    memset(run, 0, sizeof(*run));
    run->fontFace = font->face;
    run->fontEmSize = static_cast<FLOAT>(WINXTERM_CELL_HEIGHT_PIXELS);
    run->glyphCount = 1u;
    run->glyphIndices = glyph;
    run->glyphAdvances = advance;
    run->glyphOffsets = offset;
    run->isSideways = FALSE;
    run->bidiLevel = 0u;
    return true;
}

static bool winxterm_glyph_fallback_init_d2d(WinxtermGlyphFallbackState *state)
{
    if (state == nullptr) {
        return false;
    }
    if (state->d2d_context != nullptr && state->d2d_foreground_brush != nullptr) {
        return true;
    }

    if (state->d2d_factory == nullptr) {
        D2D1_FACTORY_OPTIONS options = {};
        HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                       __uuidof(ID2D1Factory8),
                                       &options,
                                       reinterpret_cast<void **>(&state->d2d_factory));
        if (FAILED(hr) || state->d2d_factory == nullptr) {
            return false;
        }
    }

    if (state->d3d_device == nullptr) {
        static const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
            D3D_FEATURE_LEVEL_10_0,
        };
        ID3D11DeviceContext *immediate_context = nullptr;
        D3D_FEATURE_LEVEL created_level = D3D_FEATURE_LEVEL_10_0;
        HRESULT hr = D3D11CreateDevice(nullptr,
                                       D3D_DRIVER_TYPE_HARDWARE,
                                       nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       levels,
                                       ARRAYSIZE(levels),
                                       D3D11_SDK_VERSION,
                                       &state->d3d_device,
                                       &created_level,
                                       &immediate_context);
        if (FAILED(hr)) {
            hr = D3D11CreateDevice(nullptr,
                                   D3D_DRIVER_TYPE_WARP,
                                   nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                   levels,
                                   ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION,
                                   &state->d3d_device,
                                   &created_level,
                                   &immediate_context);
        }
        if (immediate_context != nullptr) {
            immediate_context->Release();
        }
        if (FAILED(hr) || state->d3d_device == nullptr) {
            return false;
        }
    }

    if (state->dxgi_device == nullptr &&
        FAILED(state->d3d_device->QueryInterface(__uuidof(IDXGIDevice),
                                                 reinterpret_cast<void **>(&state->dxgi_device)))) {
        return false;
    }

    if (state->d2d_device == nullptr &&
        FAILED(state->d2d_factory->CreateDevice(state->dxgi_device, &state->d2d_device))) {
        return false;
    }

    if (state->d2d_context == nullptr &&
        FAILED(state->d2d_device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &state->d2d_context))) {
        return false;
    }

    if (state->d2d_foreground_brush == nullptr) {
        D2D1_COLOR_F white = {};
        white.r = 1.0f;
        white.g = 1.0f;
        white.b = 1.0f;
        white.a = 1.0f;
        if (FAILED(state->d2d_context->CreateSolidColorBrush(white, &state->d2d_foreground_brush))) {
            return false;
        }
    }

    return true;
}

static bool winxterm_glyph_fallback_copy_d2d_bitmap(ID2D1Bitmap1 *bitmap,
                                                    WinxtermFallbackScratchBitmap *fallback)
{
    if (bitmap == nullptr || fallback == nullptr) {
        return false;
    }

    D2D1_MAPPED_RECT mapped = {};
    HRESULT hr = bitmap->Map(D2D1_MAP_OPTIONS_READ, &mapped);
    if (FAILED(hr)) {
        return false;
    }

    bool drew = false;
    for (int y = 0; y < fallback->height_pixels; ++y) {
        const uint8_t *src_row = mapped.bits + static_cast<size_t>(y) * static_cast<size_t>(mapped.pitch);
        for (int x = 0; x < fallback->width_pixels; ++x) {
            const uint8_t *src = src_row + static_cast<size_t>(x) * 4u;
            uint8_t blue = src[0];
            uint8_t green = src[1];
            uint8_t red = src[2];
            uint8_t alpha = src[3];
            if (alpha == 0u) {
                continue;
            }
            if (alpha != 255u) {
                red = static_cast<uint8_t>((static_cast<uint32_t>(red) * 255u + alpha / 2u) / alpha);
                green = static_cast<uint8_t>((static_cast<uint32_t>(green) * 255u + alpha / 2u) / alpha);
                blue = static_cast<uint8_t>((static_cast<uint32_t>(blue) * 255u + alpha / 2u) / alpha);
            }
            size_t dst = static_cast<size_t>(y) * static_cast<size_t>(fallback->width_pixels) + static_cast<size_t>(x);
            fallback->alpha[dst] = alpha;
            fallback->pixels[dst] =
                (static_cast<uint32_t>(red) << 16) |
                (static_cast<uint32_t>(green) << 8) |
                static_cast<uint32_t>(blue);
            drew = true;
        }
    }

    bitmap->Unmap();
    return drew;
}

static bool winxterm_glyph_fallback_raster_d2d_color_one(WinxtermGlyphFallbackState *state,
                                                         const WinxtermFallbackFont *font,
                                                         uint32_t codepoint,
                                                         WinxtermGlyphFallbackBitmap *bitmap)
{
    if (font->kind != WINXTERM_GLYPH_FALLBACK_FONT_EMOJI ||
        !winxterm_glyph_fallback_init_d2d(state)) {
        return false;
    }

    WinxtermFallbackScratchBitmap scratch = {};
    if (!winxterm_glyph_fallback_scratch_init(&scratch,
                                              WINXTERM_FALLBACK_SCRATCH_WIDTH_PIXELS,
                                              WINXTERM_FALLBACK_SCRATCH_HEIGHT_PIXELS,
                                              true)) {
        return false;
    }

    UINT16 glyph = 0u;
    FLOAT advance = 0.0f;
    FLOAT baseline_x = 0.0f;
    FLOAT baseline_y = 0.0f;
    DWRITE_GLYPH_OFFSET offset = {};
    DWRITE_GLYPH_RUN run = {};
    if (!winxterm_glyph_fallback_build_color_run(font,
                                                 codepoint,
                                                 WINXTERM_CELL_WIDTH_PIXELS,
                                                 &glyph,
                                                 &advance,
                                                 &baseline_x,
                                                 &baseline_y,
                                                 &offset,
                                                 &run)) {
        winxterm_glyph_fallback_scratch_dispose(&scratch);
        return false;
    }
    baseline_x += static_cast<FLOAT>(scratch.width_pixels) / 2.0f;
    baseline_y += static_cast<FLOAT>(scratch.height_pixels - WINXTERM_CELL_HEIGHT_PIXELS) / 2.0f;

    D2D1_SIZE_U size = {};
    size.width = static_cast<UINT32>(scratch.width_pixels);
    size.height = static_cast<UINT32>(scratch.height_pixels);
    D2D1_PIXEL_FORMAT pixel_format = {};
    pixel_format.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    pixel_format.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;

    D2D1_BITMAP_PROPERTIES1 target_props = {};
    target_props.pixelFormat = pixel_format;
    target_props.dpiX = 96.0f;
    target_props.dpiY = 96.0f;
    target_props.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET;

    ID2D1Bitmap1 *target = nullptr;
    HRESULT hr = state->d2d_context->CreateBitmap(size, nullptr, 0u, &target_props, &target);
    if (FAILED(hr) || target == nullptr) {
        winxterm_glyph_fallback_scratch_dispose(&scratch);
        return false;
    }

    state->d2d_context->SetTarget(target);
    state->d2d_context->BeginDraw();
    D2D1_COLOR_F transparent = {};
    state->d2d_context->Clear(&transparent);
    D2D1_POINT_2F origin = {};
    origin.x = baseline_x;
    origin.y = baseline_y;
    state->d2d_context->DrawGlyphRunWithColorSupport(origin,
                                                     &run,
                                                     nullptr,
                                                     state->d2d_foreground_brush,
                                                     nullptr,
                                                     0u,
                                                     DWRITE_MEASURING_MODE_NATURAL,
                                                     D2D1_COLOR_BITMAP_GLYPH_SNAP_OPTION_DEFAULT);
    hr = state->d2d_context->EndDraw();
    state->d2d_context->SetTarget(nullptr);
    if (FAILED(hr)) {
        target->Release();
        winxterm_glyph_fallback_scratch_dispose(&scratch);
        return false;
    }

    D2D1_BITMAP_PROPERTIES1 read_props = {};
    read_props.pixelFormat = pixel_format;
    read_props.dpiX = 96.0f;
    read_props.dpiY = 96.0f;
    read_props.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

    ID2D1Bitmap1 *readback = nullptr;
    hr = state->d2d_context->CreateBitmap(size, nullptr, 0u, &read_props, &readback);
    if (FAILED(hr) || readback == nullptr) {
        target->Release();
        winxterm_glyph_fallback_scratch_dispose(&scratch);
        return false;
    }

    hr = readback->CopyFromBitmap(nullptr, target, nullptr);
    target->Release();
    if (FAILED(hr)) {
        readback->Release();
        winxterm_glyph_fallback_scratch_dispose(&scratch);
        return false;
    }

    bool drew = winxterm_glyph_fallback_copy_d2d_bitmap(readback, &scratch);
    readback->Release();
    if (drew) {
        drew = winxterm_glyph_fallback_normalize_scratch(&scratch, bitmap);
    }
    winxterm_glyph_fallback_scratch_dispose(&scratch);
    return drew;
}

static bool winxterm_glyph_fallback_raster_color_one(WinxtermGlyphFallbackState *state,
                                                     const WinxtermFallbackFont *font,
                                                     uint32_t codepoint,
                                                     WinxtermGlyphFallbackBitmap *bitmap)
{
    if (state->factory2 == nullptr || font->kind != WINXTERM_GLYPH_FALLBACK_FONT_EMOJI) {
        return false;
    }

    UINT16 glyph = 0u;
    FLOAT advance = 0.0f;
    FLOAT baseline_x = 0.0f;
    FLOAT baseline_y = 0.0f;
    DWRITE_GLYPH_OFFSET offset = {};
    DWRITE_GLYPH_RUN run = {};
    if (!winxterm_glyph_fallback_build_color_run(font,
                                                 codepoint,
                                                 WINXTERM_CELL_WIDTH_PIXELS,
                                                 &glyph,
                                                 &advance,
                                                 &baseline_x,
                                                 &baseline_y,
                                                 &offset,
                                                 &run)) {
        return false;
    }
    WinxtermFallbackScratchBitmap scratch = {};
    if (!winxterm_glyph_fallback_scratch_init(&scratch,
                                              WINXTERM_FALLBACK_SCRATCH_WIDTH_PIXELS,
                                              WINXTERM_FALLBACK_SCRATCH_HEIGHT_PIXELS,
                                              true)) {
        return false;
    }
    baseline_x += static_cast<FLOAT>(scratch.width_pixels) / 2.0f;
    baseline_y += static_cast<FLOAT>(scratch.height_pixels - WINXTERM_CELL_HEIGHT_PIXELS) / 2.0f;

    IDWriteColorGlyphRunEnumerator *layers = nullptr;
    HRESULT hr = state->factory2->TranslateColorGlyphRun(baseline_x,
                                                         baseline_y,
                                                         &run,
                                                         nullptr,
                                                         DWRITE_MEASURING_MODE_NATURAL,
                                                         nullptr,
                                                         0u,
                                                         &layers);
    if (FAILED(hr) || layers == nullptr) {
        if (layers != nullptr) {
            layers->Release();
        }
        winxterm_glyph_fallback_scratch_dispose(&scratch);
        return false;
    }

    bool drew = false;
    BOOL has_run = FALSE;
    while (SUCCEEDED(layers->MoveNext(&has_run)) && has_run) {
        const DWRITE_COLOR_GLYPH_RUN *color_run = nullptr;
        if (FAILED(layers->GetCurrentRun(&color_run)) || color_run == nullptr) {
            continue;
        }
        if (color_run->paletteIndex == DWRITE_NO_PALETTE_INDEX) {
            layers->Release();
            winxterm_glyph_fallback_scratch_dispose(&scratch);
            return false;
        }
        uint32_t rgb = winxterm_glyph_fallback_rgb_from_color(color_run->runColor);
        uint8_t layer_alpha = winxterm_glyph_fallback_color_component(color_run->runColor.a);
        drew = winxterm_glyph_fallback_raster_color_layer(state,
                                                          &color_run->glyphRun,
                                                          color_run->baselineOriginX,
                                                          color_run->baselineOriginY,
                                                          &scratch,
                                                          rgb,
                                                          layer_alpha) || drew;
    }
    layers->Release();
    if (drew) {
        drew = winxterm_glyph_fallback_normalize_scratch(&scratch, bitmap);
    }
    winxterm_glyph_fallback_scratch_dispose(&scratch);
    return drew;
}

static bool winxterm_glyph_fallback_raster_one(WinxtermGlyphFallbackState *state,
                                               const WinxtermFallbackFont *font,
                                               uint32_t codepoint,
                                               WinxtermFallbackScratchBitmap *scratch)
{
    UINT16 glyph = 0u;
    HRESULT hr = font->face->GetGlyphIndices(&codepoint, 1u, &glyph);
    if (FAILED(hr) || glyph == 0u) {
        return false;
    }

    int target_width = WINXTERM_CELL_WIDTH_PIXELS;
    FLOAT advance = static_cast<FLOAT>(target_width);
    IDWriteGlyphRunAnalysis *measure_analysis = nullptr;
    if (!winxterm_glyph_fallback_create_analysis(state, font, glyph, advance, 0.0f, 0.0f, &measure_analysis)) {
        return false;
    }
    RECT measure_bounds = {};
    hr = measure_analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &measure_bounds);
    measure_analysis->Release();
    if (FAILED(hr) || measure_bounds.right <= measure_bounds.left || measure_bounds.bottom <= measure_bounds.top) {
        return false;
    }

    int measure_width = measure_bounds.right - measure_bounds.left;
    int measure_height = measure_bounds.bottom - measure_bounds.top;
    FLOAT baseline_x = (static_cast<FLOAT>(scratch->width_pixels) - static_cast<FLOAT>(measure_width)) / 2.0f -
                       static_cast<FLOAT>(measure_bounds.left);
    FLOAT baseline_y = (static_cast<FLOAT>(scratch->height_pixels) - static_cast<FLOAT>(measure_height)) / 2.0f -
                       static_cast<FLOAT>(measure_bounds.top);

    IDWriteGlyphRunAnalysis *analysis = nullptr;
    if (!winxterm_glyph_fallback_create_analysis(state, font, glyph, advance, baseline_x, baseline_y, &analysis)) {
        return false;
    }
    RECT bounds = {};
    hr = analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &bounds);
    if (FAILED(hr) || bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
        analysis->Release();
        return false;
    }

    int width = bounds.right - bounds.left;
    int height = bounds.bottom - bounds.top;
    size_t buffer_size = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (buffer_size == 0u || buffer_size > UINT32_MAX) {
        analysis->Release();
        return false;
    }
    uint8_t *alpha = static_cast<uint8_t *>(malloc(buffer_size));
    if (alpha == nullptr) {
        analysis->Release();
        return false;
    }
    hr = analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &bounds, alpha, static_cast<UINT32>(buffer_size));
    analysis->Release();
    if (FAILED(hr)) {
        free(alpha);
        return false;
    }

    for (int y = 0; y < height; ++y) {
        int dst_y = bounds.top + y;
        if (dst_y < 0 || dst_y >= scratch->height_pixels) {
            continue;
        }
        for (int x = 0; x < width; ++x) {
            int dst_x = bounds.left + x;
            if (dst_x < 0 || dst_x >= scratch->width_pixels) {
                continue;
            }
            uint8_t sample = alpha[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
            uint8_t *dst =
                &scratch->alpha[static_cast<size_t>(dst_y) * static_cast<size_t>(scratch->width_pixels) +
                                static_cast<size_t>(dst_x)];
            if (sample > *dst) {
                *dst = sample;
            }
        }
    }
    free(alpha);
    return true;
}

static bool winxterm_glyph_fallback_raster_sequence(WinxtermGlyphFallbackState *state,
                                                    const WinxtermFallbackFont *font,
                                                    const uint32_t *codepoints,
                                                    uint8_t codepoint_count,
                                                    uint8_t width_cells,
                                                    WinxtermGlyphFallbackBitmap *bitmap)
{
    memset(bitmap, 0, sizeof(*bitmap));
    (void)width_cells;
    bitmap->width_pixels = WINXTERM_CELL_WIDTH_PIXELS;
    bitmap->height_pixels = WINXTERM_CELL_HEIGHT_PIXELS;
    bitmap->color_pixels = false;

    if (winxterm_glyph_fallback_raster_d2d_color_one(state, font, codepoints[0], bitmap) ||
        winxterm_glyph_fallback_raster_color_one(state, font, codepoints[0], bitmap)) {
        return true;
    }

    memset(bitmap->alpha, 0, sizeof(bitmap->alpha));
    memset(bitmap->pixels, 0, sizeof(bitmap->pixels));
    bitmap->color_pixels = false;

    WinxtermFallbackScratchBitmap scratch = {};
    if (!winxterm_glyph_fallback_scratch_init(&scratch,
                                              WINXTERM_FALLBACK_SCRATCH_WIDTH_PIXELS,
                                              WINXTERM_FALLBACK_SCRATCH_HEIGHT_PIXELS,
                                              false)) {
        return false;
    }
    bool drew_base = winxterm_glyph_fallback_raster_one(state, font, codepoints[0], &scratch);
    if (!drew_base) {
        winxterm_glyph_fallback_scratch_dispose(&scratch);
        return false;
    }
    for (uint8_t i = 1u; i < codepoint_count; ++i) {
        (void)winxterm_glyph_fallback_raster_one(state, font, codepoints[i], &scratch);
    }
    bool drew = winxterm_glyph_fallback_normalize_scratch(&scratch, bitmap);
    winxterm_glyph_fallback_scratch_dispose(&scratch);
    return drew;
}

const WinxtermGlyphFallbackBitmap *winxterm_glyph_fallback_get_glyph(
    WinxtermGlyphFallbackState *state,
    uint32_t codepoint,
    const uint32_t *combining_codepoints,
    uint8_t combining_count,
    uint8_t width_cells)
{
    if (state == nullptr || !winxterm_glyph_fallback_ready(state) || codepoint == 0u) {
        return nullptr;
    }
    (void)width_cells;
    width_cells = 1u;

    uint32_t codepoints[WINXTERM_FALLBACK_MAX_CODEPOINTS] = {};
    codepoints[0] = codepoint;
    uint8_t count = 1u;
    uint8_t limit = combining_count;
    if (limit > WINXTERM_FALLBACK_MAX_COMBINING_CODEPOINTS) {
        limit = WINXTERM_FALLBACK_MAX_COMBINING_CODEPOINTS;
    }
    for (uint8_t i = 0; i < limit; ++i) {
        if (combining_codepoints != nullptr && combining_codepoints[i] != 0u) {
            codepoints[count++] = combining_codepoints[i];
        }
    }

    WinxtermGlyphFallbackFontKind preferred = winxterm_glyph_fallback_select_font(codepoint);
    WinxtermFallbackFont *font = winxterm_glyph_fallback_choose_font(state, codepoint, preferred);
    if (font == nullptr) {
        return nullptr;
    }

    WinxtermFallbackCacheEntry *entry =
        winxterm_glyph_fallback_find_cache(state, codepoints, count, width_cells, font->kind);
    if (entry != nullptr) {
        return &entry->bitmap;
    }

    ++state->miss_count;
    entry = winxterm_glyph_fallback_alloc_cache_entry(state);
    if (entry == nullptr) {
        return nullptr;
    }
    if (!winxterm_glyph_fallback_raster_sequence(state, font, codepoints, count, width_cells, &entry->bitmap)) {
        memset(entry, 0, sizeof(*entry));
        return nullptr;
    }
    memcpy(entry->codepoints, codepoints, static_cast<size_t>(count) * sizeof(codepoints[0]));
    entry->codepoint_count = count;
    entry->width_cells = width_cells;
    entry->font_kind = font->kind;
    entry->last_used = ++state->use_clock;
    return &entry->bitmap;
}
