#include "dstcmd/winxterm_dstcmd_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

enum {
    WINXTERM_DSTCMD_HOST_REQUEST_CAPACITY = 4096u,
    WINXTERM_DSTCMD_HOST_RESPONSE_CAPACITY = 4096u,
    WINXTERM_DSTCMD_HOST_VALUE_CAPACITY = 1024u,
    WINXTERM_DSTCMD_HOST_TIMEOUT_MS = 500u
};

static bool winxterm_dstcmd_host_append(char *buffer, size_t capacity, size_t *offset, const char *text)
{
    if (buffer == 0 || offset == 0 || text == 0) {
        return false;
    }
    while (*text != '\0') {
        if (*offset + 1u >= capacity) {
            return false;
        }
        buffer[(*offset)++] = *text++;
    }
    buffer[*offset] = '\0';
    return true;
}

static bool winxterm_dstcmd_host_append_percent(char *buffer,
                                                size_t capacity,
                                                size_t *offset,
                                                const char *text)
{
    static const char hex[] = "0123456789ABCDEF";
    if (buffer == 0 || offset == 0 || text == 0) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)text; *p != '\0'; ++p) {
        bool safe = (*p >= 'A' && *p <= 'Z') ||
                    (*p >= 'a' && *p <= 'z') ||
                    (*p >= '0' && *p <= '9') ||
                    *p == '-' || *p == '_' || *p == '.' || *p == '/' || *p == '\\' || *p == ':';
        if (safe) {
            if (*offset + 1u >= capacity) {
                return false;
            }
            buffer[(*offset)++] = (char)*p;
        } else {
            if (*offset + 3u >= capacity) {
                return false;
            }
            buffer[(*offset)++] = '%';
            buffer[(*offset)++] = hex[*p >> 4];
            buffer[(*offset)++] = hex[*p & 0x0f];
        }
    }
    buffer[*offset] = '\0';
    return true;
}

static char *winxterm_dstcmd_host_wide_to_utf8(const wchar_t *text)
{
    if (text == 0) {
        return 0;
    }
    int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, 0, 0, 0, 0);
    if (needed <= 0) {
        return 0;
    }
    char *utf8 = (char *)calloc((size_t)needed, sizeof(*utf8));
    if (utf8 == 0) {
        return 0;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, needed, 0, 0) <= 0) {
        free(utf8);
        return 0;
    }
    return utf8;
}

static bool winxterm_dstcmd_host_extract_field(const char *payload,
                                               const char *name,
                                               char *out,
                                               size_t out_count)
{
    if (payload == 0 || name == 0 || out == 0 || out_count == 0u) {
        return false;
    }
    size_t name_length = strlen(name);
    const char *p = payload;
    while (*p != '\0') {
        const char *field = p;
        const char *end = strchr(field, ';');
        size_t field_length = end != 0 ? (size_t)(end - field) : strlen(field);
        if (field_length > name_length &&
            strncmp(field, name, name_length) == 0 &&
            field[name_length] == '=') {
            size_t value_length = field_length - name_length - 1u;
            if (value_length >= out_count) {
                value_length = out_count - 1u;
            }
            memcpy(out, field + name_length + 1u, value_length);
            out[value_length] = '\0';
            return true;
        }
        if (end == 0) {
            break;
        }
        p = end + 1;
    }
    return false;
}

static bool winxterm_dstcmd_host_response_matches(const char *payload, unsigned int id, bool *ok)
{
    char value[WINXTERM_DSTCMD_HOST_VALUE_CAPACITY];
    if (payload != 0 && strncmp(payload, "9001;", 5u) == 0) {
        payload += 5u;
    }
    if (strncmp(payload, "winxterm;", 9u) != 0) {
        return false;
    }
    if (!winxterm_dstcmd_host_extract_field(payload, "id", value, sizeof(value))) {
        return false;
    }
    if ((unsigned int)strtoul(value, 0, 10) != id) {
        return false;
    }
    if (!winxterm_dstcmd_host_extract_field(payload, "status", value, sizeof(value))) {
        return false;
    }
    if (ok != 0) {
        *ok = strcmp(value, "ok") == 0;
    }
    return true;
}

static bool winxterm_dstcmd_host_read_response(WinxtermDstcmdShell *shell, unsigned int id, bool *ok)
{
    char response[WINXTERM_DSTCMD_HOST_RESPONSE_CAPACITY];
    size_t count = 0u;
    bool in_osc = false;
    bool in_private_reply = false;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < WINXTERM_DSTCMD_HOST_TIMEOUT_MS) {
        uint8_t byte = 0;
        if (winxterm_dstcmd_shell_read_input(shell, &byte, 1u, false) == 0u) {
            Sleep(5);
            continue;
        }
        if (in_private_reply) {
            if (byte == 0x1eu) {
                response[count] = '\0';
                if (winxterm_dstcmd_host_response_matches(response, id, ok)) {
                    return true;
                }
                in_private_reply = false;
                count = 0u;
                continue;
            }
            if (count + 1u < sizeof(response)) {
                response[count++] = (char)byte;
            } else {
                in_private_reply = false;
                count = 0u;
            }
            continue;
        }
        if (!in_osc) {
            if (byte == 0x1fu) {
                in_private_reply = true;
                count = 0u;
                continue;
            }
            if (byte == 0x1bu) {
                uint8_t next = 0;
                DWORD pair_start = GetTickCount();
                while (GetTickCount() - pair_start < 250u &&
                       winxterm_dstcmd_shell_read_input(shell, &next, 1u, false) == 0u) {
                    Sleep(1);
                }
                if (next == ']') {
                    in_osc = true;
                    count = 0u;
                }
            }
            continue;
        }
        if (byte == '\a') {
            response[count] = '\0';
            if (winxterm_dstcmd_host_response_matches(response, id, ok)) {
                return true;
            }
            in_osc = false;
            count = 0u;
            continue;
        }
        if (byte == 0x1bu) {
            uint8_t next = 0;
            DWORD pair_start = GetTickCount();
            while (GetTickCount() - pair_start < 250u &&
                   winxterm_dstcmd_shell_read_input(shell, &next, 1u, false) == 0u) {
                Sleep(1);
            }
            if (next == '\\') {
                response[count] = '\0';
                if (winxterm_dstcmd_host_response_matches(response, id, ok)) {
                    return true;
                }
                in_osc = false;
                count = 0u;
                continue;
            }
            in_osc = false;
            count = 0u;
            continue;
        }
        if (count + 1u < sizeof(response)) {
            response[count++] = (char)byte;
        } else {
            in_osc = false;
            count = 0u;
        }
    }
    return false;
}

static int winxterm_dstcmd_host_send_request(WinxtermDstcmdShell *shell,
                                             const char *command,
                                             const char *argument_name,
                                             const char *argument_value)
{
    if (shell == 0 || command == 0) {
        return 1;
    }

    unsigned int id = ++shell->host_request_id;
    char request[WINXTERM_DSTCMD_HOST_REQUEST_CAPACITY];
    int written = sprintf_s(request,
                            sizeof(request),
                            "\x1b]9001;winxterm;v=1;id=%u;cmd=%s",
                            id,
                            command);
    if (written <= 0) {
        return 1;
    }
    size_t offset = (size_t)written;
    if (argument_name != 0 && argument_value != 0) {
        if (!winxterm_dstcmd_host_append(request, sizeof(request), &offset, ";") ||
            !winxterm_dstcmd_host_append(request, sizeof(request), &offset, argument_name) ||
            !winxterm_dstcmd_host_append(request, sizeof(request), &offset, "=") ||
            !winxterm_dstcmd_host_append_percent(request, sizeof(request), &offset, argument_value)) {
            return 1;
        }
    }
    if (!winxterm_dstcmd_host_append(request, sizeof(request), &offset, "\a")) {
        return 1;
    }
    if (!winxterm_dstcmd_shell_write_utf8(shell, request)) {
        return 1;
    }
    bool ok = false;
    if (!winxterm_dstcmd_host_read_response(shell, id, &ok)) {
        (void)winxterm_dstcmd_shell_write_wide(shell, L"dstshell: no winxterm protocol acknowledgement\r\n");
        return 1;
    }
    return ok ? 0 : 1;
}

int winxterm_dstcmd_host_query(WinxtermDstcmdShell *shell)
{
    return winxterm_dstcmd_host_send_request(shell, "query", 0, 0);
}

int winxterm_dstcmd_host_set_scale(WinxtermDstcmdShell *shell, unsigned int scale)
{
    char value[32];
    int written = sprintf_s(value, sizeof(value), "%u", scale);
    if (written <= 0) {
        return 1;
    }
    return winxterm_dstcmd_host_send_request(shell, "set-scale", "value", value);
}

int winxterm_dstcmd_host_set_bell(WinxtermDstcmdShell *shell, bool enabled)
{
    return winxterm_dstcmd_host_send_request(shell,
                                             "set-bell",
                                             "value",
                                             enabled ? "on" : "off");
}

int winxterm_dstcmd_host_set_debuglog(WinxtermDstcmdShell *shell, bool enabled)
{
    return winxterm_dstcmd_host_send_request(shell,
                                             "set-debuglog",
                                             "value",
                                             enabled ? "on" : "off");
}

int winxterm_dstcmd_host_playmacro(WinxtermDstcmdShell *shell, const wchar_t *path)
{
    char *utf8 = winxterm_dstcmd_host_wide_to_utf8(path);
    if (utf8 == 0) {
        return 1;
    }
    int status = winxterm_dstcmd_host_send_request(shell, "playmacro", "path", utf8);
    free(utf8);
    return status;
}
