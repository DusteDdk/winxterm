#include "winxterm_transfer_format.h"

#include <windows.h>

void winxterm_transfer_format_bytes(uint64_t bytes, wchar_t *out, size_t out_count)
{
    if (out == 0 || out_count == 0u) return;
    if (bytes == 1u) {
        (void)wcscpy_s(out, out_count, L"1 byte");
    } else if (bytes < 1024u) {
        (void)_snwprintf_s(out, out_count, _TRUNCATE, L"%llu bytes",
                           (unsigned long long)bytes);
    } else {
        static const wchar_t *units[] = {L"KiB", L"MiB", L"GiB", L"TiB"};
        double value = (double)bytes / 1024.0;
        size_t unit = 0u;
        while (value >= 1024.0 && unit + 1u < sizeof(units) / sizeof(units[0])) {
            value /= 1024.0;
            ++unit;
        }
        (void)_snwprintf_s(out, out_count, _TRUNCATE, L"%.2f %ls", value, units[unit]);
    }
}

void winxterm_transfer_format_duration(uint64_t elapsed_ns, wchar_t *out, size_t out_count)
{
    if (out == 0 || out_count == 0u) return;
    if (elapsed_ns < 1000000000ull) {
        uint64_t milliseconds = (elapsed_ns + 500000ull) / 1000000ull;
        if (milliseconds == 0u && elapsed_ns != 0u) milliseconds = 1u;
        (void)_snwprintf_s(out, out_count, _TRUNCATE, L"%llu ms",
                           (unsigned long long)milliseconds);
    } else {
        double seconds = (double)elapsed_ns / 1000000000.0;
        (void)_snwprintf_s(out, out_count, _TRUNCATE,
                           seconds < 10.0 ? L"%.2f s" : L"%.1f s", seconds);
    }
}

void winxterm_transfer_format_speed(uint64_t bytes, uint64_t elapsed_ns,
                                    wchar_t *out, size_t out_count)
{
    if (out == 0 || out_count == 0u) return;
    if (elapsed_ns == 0u) elapsed_ns = 1u;
    double rate = ((double)bytes * 1000000000.0) / (double)elapsed_ns;
    if (rate < 1024.0) {
        (void)_snwprintf_s(out, out_count, _TRUNCATE, L"%.0f bytes/s", rate);
    } else {
        static const wchar_t *units[] = {L"KiB/s", L"MiB/s", L"GiB/s", L"TiB/s"};
        double value = rate / 1024.0;
        size_t unit = 0u;
        while (value >= 1024.0 && unit + 1u < sizeof(units) / sizeof(units[0])) {
            value /= 1024.0;
            ++unit;
        }
        (void)_snwprintf_s(out, out_count, _TRUNCATE, L"%.2f %ls", value, units[unit]);
    }
}
