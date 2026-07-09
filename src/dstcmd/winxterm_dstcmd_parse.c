#include "dstcmd/winxterm_dstcmd_parse.h"

#include "dstcmd/api/path.h"
#include "dstcmd/winxterm_dstcmd.h"

#include <stdlib.h>
#include <string.h>
#include <wctype.h>
#include <windows.h>

typedef struct WinxtermDstcmdTokenBuilder {
    wchar_t *chars;
    size_t length;
    size_t capacity;
} WinxtermDstcmdTokenBuilder;

static void winxterm_dstcmd_set_error(wchar_t *error, size_t error_count, const wchar_t *message)
{
    if (error == 0 || error_count == 0u) {
        return;
    }
    if (message == 0) {
        error[0] = L'\0';
        return;
    }
    wcsncpy_s(error, error_count, message, _TRUNCATE);
}

wchar_t *winxterm_dstcmd_wcsdup(const wchar_t *text)
{
    if (text == 0) {
        return 0;
    }
    size_t length = wcslen(text);
    wchar_t *copy = (wchar_t *)calloc(length + 1u, sizeof(*copy));
    if (copy == 0) {
        return 0;
    }
    memcpy(copy, text, (length + 1u) * sizeof(*copy));
    return copy;
}

static void winxterm_dstcmd_token_dispose(WinxtermDstcmdTokenBuilder *builder)
{
    if (builder == 0) {
        return;
    }
    free(builder->chars);
    memset(builder, 0, sizeof(*builder));
}

static bool winxterm_dstcmd_token_append(WinxtermDstcmdTokenBuilder *builder, wchar_t ch)
{
    if (builder == 0) {
        return false;
    }
    if (builder->length + 1u >= builder->capacity) {
        size_t new_capacity = builder->capacity == 0u ? 32u : builder->capacity * 2u;
        wchar_t *new_chars = (wchar_t *)realloc(builder->chars, new_capacity * sizeof(*new_chars));
        if (new_chars == 0) {
            return false;
        }
        builder->chars = new_chars;
        builder->capacity = new_capacity;
    }
    builder->chars[builder->length++] = ch;
    builder->chars[builder->length] = L'\0';
    return true;
}

static bool winxterm_dstcmd_token_append_text(WinxtermDstcmdTokenBuilder *builder, const wchar_t *text)
{
    if (builder == 0 || text == 0) {
        return false;
    }
    for (const wchar_t *p = text; *p != L'\0'; ++p) {
        if (!winxterm_dstcmd_token_append(builder, *p)) {
            return false;
        }
    }
    return true;
}

static bool winxterm_dstcmd_env_name_start(wchar_t ch)
{
    return (ch >= L'a' && ch <= L'z') ||
           (ch >= L'A' && ch <= L'Z') ||
           ch == L'_';
}

static bool winxterm_dstcmd_env_name_char(wchar_t ch)
{
    return winxterm_dstcmd_env_name_start(ch) || (ch >= L'0' && ch <= L'9');
}

static bool winxterm_dstcmd_should_escape(wchar_t ch)
{
    return ch == L' ' ||
           ch == L'\t' ||
           ch == L'\'' ||
           ch == L'"' ||
           ch == L'$' ||
           ch == L'*' ||
           ch == L'|' ||
           ch == L';' ||
           ch == L'&';
}

static bool winxterm_dstcmd_token_append_env_value(WinxtermDstcmdTokenBuilder *builder,
                                                   const wchar_t *name_start,
                                                   const wchar_t **name_end)
{
    if (builder == 0 || name_start == 0 || name_end == 0) {
        return false;
    }
    const wchar_t *end = name_start;
    while (winxterm_dstcmd_env_name_char(*end)) {
        ++end;
    }
    size_t name_length = (size_t)(end - name_start);
    wchar_t *name = (wchar_t *)calloc(name_length + 1u, sizeof(*name));
    if (name == 0) {
        return false;
    }
    memcpy(name, name_start, name_length * sizeof(*name));
    name[name_length] = L'\0';

    DWORD needed = GetEnvironmentVariableW(name, 0, 0);
    if (needed != 0u) {
        wchar_t *value = (wchar_t *)calloc((size_t)needed, sizeof(*value));
        if (value == 0) {
            free(name);
            return false;
        }
        DWORD length = GetEnvironmentVariableW(name, value, needed);
        if (length != 0u && length < needed &&
            !winxterm_dstcmd_token_append_text(builder, value)) {
            free(value);
            free(name);
            return false;
        }
        free(value);
    }

    free(name);
    *name_end = end;
    return true;
}

static bool winxterm_dstcmd_token_append_status(WinxtermDstcmdTokenBuilder *builder, int last_status)
{
    wchar_t status_text[32];
    if (_snwprintf_s(status_text,
                     sizeof(status_text) / sizeof(status_text[0]),
                     _TRUNCATE,
                     L"%d",
                     last_status) < 0) {
        return false;
    }
    return winxterm_dstcmd_token_append_text(builder, status_text);
}

static wchar_t *winxterm_dstcmd_token_finish(WinxtermDstcmdTokenBuilder *builder)
{
    if (builder == 0) {
        return 0;
    }
    if (builder->chars == 0) {
        builder->chars = (wchar_t *)calloc(1u, sizeof(*builder->chars));
        if (builder->chars == 0) {
            return 0;
        }
        builder->capacity = 1u;
    }
    wchar_t *result = builder->chars;
    builder->chars = 0;
    builder->length = 0u;
    builder->capacity = 0u;
    return result;
}

static bool winxterm_dstcmd_argv_append(WinxtermDstcmdArgv *argv, wchar_t *token)
{
    if (argv == 0 || token == 0) {
        return false;
    }
    wchar_t **new_items = (wchar_t **)realloc(argv->items, ((size_t)argv->count + 1u) * sizeof(*new_items));
    if (new_items == 0) {
        return false;
    }
    argv->items = new_items;
    argv->items[argv->count++] = token;
    return true;
}

void winxterm_dstcmd_argv_dispose(WinxtermDstcmdArgv *argv)
{
    if (argv == 0) {
        return;
    }
    for (int i = 0; i < argv->count; ++i) {
        free(argv->items[i]);
    }
    free(argv->items);
    memset(argv, 0, sizeof(*argv));
}

static bool winxterm_dstcmd_glob_is_slash(wchar_t ch)
{
    return ch == L'\\' || ch == L'/';
}

static bool winxterm_dstcmd_glob_match(const wchar_t *pattern, const wchar_t *text)
{
    if (pattern == 0 || text == 0) {
        return false;
    }
    while (*pattern != L'\0') {
        if (*pattern == L'*') {
            while (*pattern == L'*') {
                ++pattern;
            }
            if (*pattern == L'\0') {
                return true;
            }
            for (const wchar_t *candidate = text;; ++candidate) {
                if (winxterm_dstcmd_glob_match(pattern, candidate)) {
                    return true;
                }
                if (*candidate == L'\0') {
                    return false;
                }
            }
        }
        if (*text == L'\0' || towlower(*pattern) != towlower(*text)) {
            return false;
        }
        ++pattern;
        ++text;
    }
    return *text == L'\0';
}

static int winxterm_dstcmd_glob_compare(const void *left, const void *right)
{
    const wchar_t *const *a = (const wchar_t *const *)left;
    const wchar_t *const *b = (const wchar_t *const *)right;
    return _wcsicmp(*a, *b);
}

static bool winxterm_dstcmd_glob_join_prefix(const wchar_t *pattern,
                                             size_t prefix_length,
                                             const wchar_t *name,
                                             wchar_t **out)
{
    if (pattern == 0 || name == 0 || out == 0) {
        return false;
    }
    *out = 0;
    size_t name_length = wcslen(name);
    wchar_t *match = (wchar_t *)calloc(prefix_length + name_length + 1u, sizeof(*match));
    if (match == 0) {
        return false;
    }
    if (prefix_length != 0u) {
        memcpy(match, pattern, prefix_length * sizeof(*match));
    }
    memcpy(match + prefix_length, name, (name_length + 1u) * sizeof(*match));
    *out = match;
    return true;
}

static bool winxterm_dstcmd_glob_collect_matches(const wchar_t *cwd,
                                                 const wchar_t *pattern,
                                                 WinxtermDstcmdScratch *scratch,
                                                 WinxtermDstcmdArgv *matches,
                                                 wchar_t *error,
                                                 size_t error_count)
{
    if (matches == 0 || scratch == 0) {
        return false;
    }
    memset(matches, 0, sizeof(*matches));
    if (cwd == 0 || pattern == 0) {
        return true;
    }

    const wchar_t *basename = pattern;
    for (const wchar_t *p = pattern; *p != L'\0'; ++p) {
        if (winxterm_dstcmd_glob_is_slash(*p)) {
            basename = p + 1;
        }
    }
    if (basename[0] == L'\0') {
        return true;
    }

    size_t prefix_length = (size_t)(basename - pattern);
    WinxtermDstcmdScratchMark mark = winxterm_dstcmd_scratch_mark(scratch);
    wchar_t *directory_operand = winxterm_dstcmd_scratch_alloc_path(scratch);
    wchar_t *directory = winxterm_dstcmd_scratch_alloc_path(scratch);
    if (directory_operand == 0 || directory == 0) {
        winxterm_dstcmd_set_error(error, error_count, L"out of memory");
        return false;
    }
    bool ok = false;
    if (prefix_length == 0u) {
        wcscpy_s(directory_operand, WINXTERM_DSTCMD_PATH_CAPACITY, L".");
    } else {
        if (prefix_length >= WINXTERM_DSTCMD_PATH_CAPACITY) {
            winxterm_dstcmd_set_error(error, error_count, L"glob path too long");
            goto cleanup;
        }
        memcpy(directory_operand, pattern, prefix_length * sizeof(*directory_operand));
        directory_operand[prefix_length] = L'\0';
        if (wcschr(directory_operand, L'*') != 0) {
            ok = true;
            goto cleanup;
        }
    }

    if (!winxterm_dstcmd_path_resolve_scratch(scratch,
                                             cwd,
                                             directory_operand,
                                             directory,
                                             WINXTERM_DSTCMD_PATH_CAPACITY)) {
        ok = true;
        goto cleanup;
    }

    WinxtermDstcmdDirIter iter;
    if (!winxterm_dstcmd_dir_iter_open_scratch(scratch, directory, &iter)) {
        ok = true;
        goto cleanup;
    }
    const WIN32_FIND_DATAW *data = 0;
    while (winxterm_dstcmd_dir_iter_next(&iter, &data)) {
        if (wcscmp(data->cFileName, L".") == 0 ||
            wcscmp(data->cFileName, L"..") == 0 ||
            (data->cFileName[0] == L'.' && basename[0] != L'.') ||
            !winxterm_dstcmd_glob_match(basename, data->cFileName)) {
            continue;
        }
        wchar_t *match = 0;
        if (!winxterm_dstcmd_glob_join_prefix(pattern, prefix_length, data->cFileName, &match) ||
            !winxterm_dstcmd_argv_append(matches, match)) {
            free(match);
            winxterm_dstcmd_dir_iter_close(&iter);
            winxterm_dstcmd_argv_dispose(matches);
            winxterm_dstcmd_set_error(error, error_count, L"out of memory");
            goto cleanup;
        }
    }
    winxterm_dstcmd_dir_iter_close(&iter);

    if (matches->count > 1) {
        qsort(matches->items, (size_t)matches->count, sizeof(matches->items[0]), winxterm_dstcmd_glob_compare);
    }
    ok = true;

cleanup:
    winxterm_dstcmd_scratch_rewind(scratch, mark);
    return ok;
}

static bool winxterm_dstcmd_append_token_or_glob(const wchar_t *glob_cwd,
                                                 WinxtermDstcmdScratch *scratch,
                                                 WinxtermDstcmdArgv *argv,
                                                 wchar_t *token,
                                                 bool expand_glob,
                                                 wchar_t *error,
                                                 size_t error_count)
{
    if (argv == 0 || token == 0) {
        free(token);
        return false;
    }
    if (!expand_glob || glob_cwd == 0 || scratch == 0) {
        if (winxterm_dstcmd_argv_append(argv, token)) {
            return true;
        }
        free(token);
        return false;
    }

    WinxtermDstcmdArgv matches;
    if (!winxterm_dstcmd_glob_collect_matches(glob_cwd, token, scratch, &matches, error, error_count)) {
        free(token);
        return false;
    }
    if (matches.count == 0) {
        winxterm_dstcmd_argv_dispose(&matches);
        if (winxterm_dstcmd_argv_append(argv, token)) {
            return true;
        }
        free(token);
        return false;
    }

    free(token);
    for (int i = 0; i < matches.count; ++i) {
        wchar_t *match = matches.items[i];
        matches.items[i] = 0;
        if (!winxterm_dstcmd_argv_append(argv, match)) {
            free(match);
            winxterm_dstcmd_argv_dispose(&matches);
            winxterm_dstcmd_set_error(error, error_count, L"out of memory");
            return false;
        }
    }
    winxterm_dstcmd_argv_dispose(&matches);
    return true;
}

static bool winxterm_dstcmd_parse_line_internal(const wchar_t *line,
                                                const wchar_t *glob_cwd,
                                                WinxtermDstcmdScratch *scratch,
                                                bool expand_status,
                                                int last_status,
                                                WinxtermDstcmdArgv *argv,
                                                wchar_t *error,
                                                size_t error_count)
{
    if (argv == 0) {
        return false;
    }
    memset(argv, 0, sizeof(*argv));
    winxterm_dstcmd_set_error(error, error_count, L"");
    if (line == 0) {
        return true;
    }

    const wchar_t *p = line;
    while (*p != L'\0') {
        while (*p == L' ' || *p == L'\t') {
            ++p;
        }
        if (*p == L'\0') {
            break;
        }

        WinxtermDstcmdTokenBuilder token;
        memset(&token, 0, sizeof(token));
        wchar_t quote = L'\0';
        bool token_started = false;
        bool expand_glob = false;
        while (*p != L'\0') {
            wchar_t ch = *p;
            if (quote == L'\0' && (ch == L' ' || ch == L'\t')) {
                break;
            }
            if ((ch == L'\'' || ch == L'"') && quote == L'\0') {
                quote = ch;
                token_started = true;
                ++p;
                continue;
            }
            if (quote != L'\0' && ch == quote) {
                quote = L'\0';
                token_started = true;
                ++p;
                continue;
            }
            bool escaped = false;
            if (ch == L'\\' && quote != L'\'') {
                if (p[1] != L'\0' && winxterm_dstcmd_should_escape(p[1])) {
                    ++p;
                    ch = *p;
                    escaped = true;
                }
            }
            if (!escaped && quote != L'\'' && ch == L'$' && p[1] == L'?' && expand_status) {
                if (!winxterm_dstcmd_token_append_status(&token, last_status)) {
                    winxterm_dstcmd_token_dispose(&token);
                    winxterm_dstcmd_argv_dispose(argv);
                    winxterm_dstcmd_set_error(error, error_count, L"out of memory");
                    return false;
                }
                p += 2;
                token_started = true;
                continue;
            }
            if (!escaped && quote != L'\'' && ch == L'$' &&
                winxterm_dstcmd_env_name_start(p[1])) {
                const wchar_t *next = p + 1;
                if (!winxterm_dstcmd_token_append_env_value(&token, next, &p)) {
                    winxterm_dstcmd_token_dispose(&token);
                    winxterm_dstcmd_argv_dispose(argv);
                    winxterm_dstcmd_set_error(error, error_count, L"out of memory");
                    return false;
                }
                token_started = true;
                continue;
            }
            if (!escaped && quote == L'\0' && ch == L'*') {
                expand_glob = true;
            }
            if (!winxterm_dstcmd_token_append(&token, ch)) {
                winxterm_dstcmd_token_dispose(&token);
                winxterm_dstcmd_argv_dispose(argv);
                winxterm_dstcmd_set_error(error, error_count, L"out of memory");
                return false;
            }
            token_started = true;
            if (*p != L'\0') {
                ++p;
            }
        }
        if (quote != L'\0') {
            winxterm_dstcmd_token_dispose(&token);
            winxterm_dstcmd_argv_dispose(argv);
            winxterm_dstcmd_set_error(error, error_count, L"unterminated quote");
            return false;
        }
        if (token_started) {
            wchar_t *finished = winxterm_dstcmd_token_finish(&token);
            if (finished == 0 ||
                !winxterm_dstcmd_append_token_or_glob(glob_cwd,
                                                      scratch,
                                                      argv,
                                                      finished,
                                                      expand_glob,
                                                      error,
                                                      error_count)) {
                winxterm_dstcmd_token_dispose(&token);
                winxterm_dstcmd_argv_dispose(argv);
                if (error != 0 && error[0] == L'\0') {
                    winxterm_dstcmd_set_error(error, error_count, L"out of memory");
                }
                return false;
            }
        }
        while (*p == L' ' || *p == L'\t') {
            ++p;
        }
    }
    return true;
}

bool winxterm_dstcmd_parse_line(const wchar_t *line,
                                WinxtermDstcmdArgv *argv,
                                wchar_t *error,
                                size_t error_count)
{
    return winxterm_dstcmd_parse_line_internal(line, 0, 0, false, 0, argv, error, error_count);
}

bool winxterm_dstcmd_parse_line_expanding_globs(const wchar_t *line,
                                                const wchar_t *cwd,
                                                WinxtermDstcmdArgv *argv,
                                                wchar_t *error,
                                                size_t error_count)
{
    WinxtermDstcmdScratch scratch;
    winxterm_dstcmd_scratch_init(&scratch);
    bool ok = winxterm_dstcmd_parse_line_internal(line, cwd, &scratch, false, 0, argv, error, error_count);
    winxterm_dstcmd_scratch_dispose(&scratch);
    return ok;
}

bool winxterm_dstcmd_parse_line_expanding_globs_and_status(const wchar_t *line,
                                                           const wchar_t *cwd,
                                                           int last_status,
                                                           WinxtermDstcmdArgv *argv,
                                                           wchar_t *error,
                                                           size_t error_count)
{
    WinxtermDstcmdScratch scratch;
    winxterm_dstcmd_scratch_init(&scratch);
    bool ok = winxterm_dstcmd_parse_line_internal(line,
                                                  cwd,
                                                  &scratch,
                                                  true,
                                                  last_status,
                                                  argv,
                                                  error,
                                                  error_count);
    winxterm_dstcmd_scratch_dispose(&scratch);
    return ok;
}

bool winxterm_dstcmd_parse_line_expanding_globs_and_status_scratch(WinxtermDstcmdScratch *scratch,
                                                                   const wchar_t *line,
                                                                   const wchar_t *cwd,
                                                                   int last_status,
                                                                   WinxtermDstcmdArgv *argv,
                                                                   wchar_t *error,
                                                                   size_t error_count)
{
    return winxterm_dstcmd_parse_line_internal(line,
                                               cwd,
                                               scratch,
                                               true,
                                               last_status,
                                               argv,
                                               error,
                                               error_count);
}

bool winxterm_dstcmd_parse_single_token_expanding_status_scratch(WinxtermDstcmdScratch *scratch,
                                                                 const wchar_t *line,
                                                                 int last_status,
                                                                 wchar_t **out,
                                                                 wchar_t *error,
                                                                 size_t error_count)
{
    if (out != 0) {
        *out = 0;
    }
    if (out == 0) {
        return false;
    }

    WinxtermDstcmdArgv argv;
    if (!winxterm_dstcmd_parse_line_internal(line,
                                             0,
                                             scratch,
                                             true,
                                             last_status,
                                             &argv,
                                             error,
                                             error_count)) {
        return false;
    }
    if (argv.count != 1 || argv.items == 0 || argv.items[0] == 0 || argv.items[0][0] == L'\0') {
        winxterm_dstcmd_argv_dispose(&argv);
        winxterm_dstcmd_set_error(error, error_count, L"missing redirect target");
        return false;
    }

    *out = argv.items[0];
    argv.items[0] = 0;
    winxterm_dstcmd_argv_dispose(&argv);
    return true;
}
