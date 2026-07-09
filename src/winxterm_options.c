#include "winxterm_options.h"

#include "winxterm_scale.h"

#include <limits.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

unsigned int winxterm_options_default_render_thread_count(void)
{
    DWORD count = 0;
#if defined(ALL_PROCESSOR_GROUPS)
    count = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#endif
    if (count == 0u) {
        SYSTEM_INFO info;
        memset(&info, 0, sizeof(info));
        GetSystemInfo(&info);
        count = info.dwNumberOfProcessors;
    }
    if (count == 0u) {
        count = 1u;
    }
    return count > WINXTERM_MAX_RENDER_THREADS ? WINXTERM_MAX_RENDER_THREADS : (unsigned int)count;
}

static bool winxterm_options_parse_render_backend(const wchar_t *value,
                                                  WinxtermRenderBackend *backend,
                                                  bool *all)
{
    if (value == 0 || backend == 0 || all == 0) {
        return false;
    }

    *all = false;
    if (wcscmp(value, L"spans") == 0 || wcscmp(value, L"span") == 0) {
        *backend = WINXTERM_RENDER_BACKEND_SPANS;
        return true;
    }
    if (wcscmp(value, L"row-masks") == 0 || wcscmp(value, L"rowmask") == 0 ||
        wcscmp(value, L"row-masks-avx2") == 0) {
        *backend = WINXTERM_RENDER_BACKEND_ROW_MASKS;
        return true;
    }
    if (wcscmp(value, L"precolored-cache") == 0 || wcscmp(value, L"precolored") == 0 ||
        wcscmp(value, L"cache") == 0) {
        *backend = WINXTERM_RENDER_BACKEND_PRECLORED_CACHE;
        return true;
    }
    if (wcscmp(value, L"all") == 0) {
        *backend = WINXTERM_RENDER_BACKEND_SPANS;
        *all = true;
        return true;
    }

    return false;
}

static bool winxterm_options_parse_unsigned(const wchar_t *value, unsigned int *out)
{
    if (value == 0 || value[0] == L'\0' || out == 0) {
        return false;
    }

    unsigned long result = 0;
    for (const wchar_t *p = value; *p != L'\0'; ++p) {
        if (*p < L'0' || *p > L'9') {
            return false;
        }
        result = result * 10ul + (unsigned long)(*p - L'0');
        if (result > UINT_MAX) {
            return false;
        }
    }

    if (result == 0ul) {
        return false;
    }

    *out = (unsigned int)result;
    return true;
}

int winxterm_options_parse(int argc, const wchar_t * const *argv, WinxtermOptions *options)
{
    if (options == 0) {
        return -1;
    }

    memset(options, 0, sizeof(*options));
    options->render_backend = WINXTERM_DEFAULT_RENDER_BACKEND;
    options->unpainted_line_limit = WINXTERM_DEFAULT_UNPAINTED_LINE_LIMIT;
    options->display_scale = WINXTERM_DEFAULT_DISPLAY_SCALE;
    options->render_thread_count = winxterm_options_default_render_thread_count();

    for (int i = 1; i < argc; ++i) {
        if (argv == 0 || argv[i] == 0) {
            return -1;
        }

        if (wcscmp(argv[i], L"--") == 0) {
            options->client_arg_start = i + 1;
            options->client_argc = argc - options->client_arg_start;
            return 0;
        } else if (wcscmp(argv[i], L"--smoke") == 0) {
            options->smoke = true;
        } else if (wcscmp(argv[i], L"--demo") == 0) {
            options->demo = true;
        } else if (wcscmp(argv[i], L"--glyphbench") == 0) {
            options->glyphbench = true;
        } else if (wcscmp(argv[i], L"--macro") == 0) {
            if (i + 1 >= argc || argv[i + 1] == 0 || argv[i + 1][0] == L'\0') {
                return -1;
            }
            options->macro_path = argv[i + 1];
            ++i;
        } else if (wcscmp(argv[i], L"--rendermethod") == 0) {
            if (i + 1 >= argc || argv[i + 1] == 0 ||
                !winxterm_options_parse_render_backend(argv[i + 1],
                                                       &options->render_backend,
                                                       &options->render_backend_all)) {
                return -1;
            }
            options->render_backend_set = true;
            ++i;
        } else if (wcscmp(argv[i], L"--unpaintedlines") == 0) {
            if (i + 1 >= argc || argv[i + 1] == 0 ||
                !winxterm_options_parse_unsigned(argv[i + 1], &options->unpainted_line_limit)) {
                return -1;
            }
            ++i;
        } else if (wcscmp(argv[i], L"--ncputhreads") == 0) {
            if (i + 1 >= argc || argv[i + 1] == 0 ||
                !winxterm_options_parse_unsigned(argv[i + 1], &options->render_thread_count) ||
                options->render_thread_count > WINXTERM_MAX_RENDER_THREADS) {
                return -1;
            }
            ++i;
        } else if (wcscmp(argv[i], L"-x") == 0) {
            if (i + 1 >= argc || argv[i + 1] == 0 ||
                !winxterm_parse_display_scale_wide(argv[i + 1], &options->display_scale)) {
                return -1;
            }
            ++i;
        } else if (wcscmp(argv[i], L"--help") == 0 || wcscmp(argv[i], L"-h") == 0) {
            options->help = true;
            if (i + 1 < argc && argv[i + 1] != 0 &&
                argv[i + 1][0] != L'-' && argv[i + 1][0] != L'\0') {
                options->help_topic = argv[i + 1];
                ++i;
            }
        } else if (argv[i][0] != L'-' || argv[i][1] == L'\0') {
            options->client_arg_start = i;
            options->client_argc = argc - i;
            return 0;
        } else {
            return -1;
        }
    }

    return 0;
}
