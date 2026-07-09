#include "winxterm_clipboard.h"

#include <stdint.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static bool winxterm_clipboard_wide_to_utf8(const wchar_t *wide, char **out_text, size_t *out_length)
{
    if (wide == 0 || out_text == 0 || out_length == 0) {
        return false;
    }

    int required = WideCharToMultiByte(CP_UTF8, 0, wide, -1, 0, 0, 0, 0);
    if (required <= 0) {
        return false;
    }
    char *text = (char *)malloc((size_t)required);
    if (text == 0) {
        return false;
    }
    int written = WideCharToMultiByte(CP_UTF8, 0, wide, -1, text, required, 0, 0);
    if (written <= 0) {
        free(text);
        return false;
    }
    *out_text = text;
    *out_length = (size_t)written - 1u;
    return true;
}

static bool winxterm_clipboard_utf8_to_wide(const char *text, size_t length, wchar_t **out_wide, size_t *out_count)
{
    if (text == 0 || out_wide == 0 || out_count == 0) {
        return false;
    }
    if (length > (size_t)INT_MAX) {
        return false;
    }

    int required = MultiByteToWideChar(CP_UTF8, 0, text, (int)length, 0, 0);
    if (required < 0) {
        return false;
    }
    wchar_t *wide = (wchar_t *)malloc(((size_t)required + 1u) * sizeof(*wide));
    if (wide == 0) {
        return false;
    }
    if (required > 0) {
        int written = MultiByteToWideChar(CP_UTF8, 0, text, (int)length, wide, required);
        if (written != required) {
            free(wide);
            return false;
        }
    }
    wide[required] = L'\0';
    *out_wide = wide;
    *out_count = (size_t)required + 1u;
    return true;
}

bool winxterm_clipboard_get_text_utf8(HWND owner, char **out_text, size_t *out_length)
{
    if (out_text == 0 || out_length == 0) {
        return false;
    }
    *out_text = 0;
    *out_length = 0u;

    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(owner)) {
        return false;
    }

    bool ok = false;
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (handle != 0) {
        const wchar_t *wide = (const wchar_t *)GlobalLock(handle);
        if (wide != 0) {
            ok = winxterm_clipboard_wide_to_utf8(wide, out_text, out_length);
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return ok;
}

bool winxterm_clipboard_set_text_utf8(HWND owner, const char *text, size_t length)
{
    if (text == 0) {
        return false;
    }

    wchar_t *wide = 0;
    size_t wide_count = 0u;
    if (!winxterm_clipboard_utf8_to_wide(text, length, &wide, &wide_count)) {
        return false;
    }

    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, wide_count * sizeof(wchar_t));
    if (handle == 0) {
        free(wide);
        return false;
    }
    wchar_t *dst = (wchar_t *)GlobalLock(handle);
    if (dst == 0) {
        GlobalFree(handle);
        free(wide);
        return false;
    }
    memcpy(dst, wide, wide_count * sizeof(wchar_t));
    GlobalUnlock(handle);
    free(wide);

    if (!OpenClipboard(owner)) {
        GlobalFree(handle);
        return false;
    }
    bool ok = false;
    if (EmptyClipboard() && SetClipboardData(CF_UNICODETEXT, handle) != 0) {
        ok = true;
        handle = 0;
    }
    CloseClipboard();
    if (handle != 0) {
        GlobalFree(handle);
    }
    return ok;
}

bool winxterm_clipboard_prepare_paste(const char *text,
                                      size_t length,
                                      bool bracketed,
                                      char **out_text,
                                      size_t *out_length)
{
    if (text == 0 || out_text == 0 || out_length == 0) {
        return false;
    }

    const char prefix[] = "\x1b[200~";
    const char suffix[] = "\x1b[201~";
    size_t prefix_length = bracketed ? sizeof(prefix) - 1u : 0u;
    size_t suffix_length = bracketed ? sizeof(suffix) - 1u : 0u;
    if (length > SIZE_MAX - prefix_length - suffix_length - 1u) {
        return false;
    }

    char *prepared = (char *)malloc(prefix_length + length + suffix_length + 1u);
    if (prepared == 0) {
        return false;
    }

    size_t offset = 0u;
    if (bracketed) {
        memcpy(prepared + offset, prefix, prefix_length);
        offset += prefix_length;
    }
    for (size_t i = 0u; i < length; ++i) {
        if (text[i] == '\r') {
            if (i + 1u < length && text[i + 1u] == '\n') {
                ++i;
            }
            prepared[offset++] = '\n';
        } else {
            prepared[offset++] = text[i];
        }
    }
    if (bracketed) {
        memcpy(prepared + offset, suffix, suffix_length);
        offset += suffix_length;
    }
    prepared[offset] = '\0';

    *out_text = prepared;
    *out_length = offset;
    return true;
}
