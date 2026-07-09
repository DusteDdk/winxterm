#include "winxterm_client.h"

#include "winxterm_host.h"
#include "winxterm_render.h"

#include <stdbool.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static unsigned int winxterm_client_count_output_lines(const uint8_t *bytes, size_t byte_count)
{
    unsigned int count = 0u;
    if (bytes == 0) {
        return 0u;
    }
    for (size_t i = 0u; i < byte_count; ++i) {
        if (bytes[i] == '\n' && count != UINT_MAX) {
            ++count;
        }
    }
    return count;
}

bool winxterm_client_write_bytes(WinxtermBridge *bridge,
                                 WinxtermUtf8Decoder *decoder,
                                 const uint8_t *bytes,
                                 size_t byte_count)
{
    return winxterm_client_write_bytes_with_policy(bridge, decoder, bytes, byte_count, 0, true);
}

bool winxterm_client_write_bytes_with_policy(WinxtermBridge *bridge,
                                             WinxtermUtf8Decoder *decoder,
                                             const uint8_t *bytes,
                                             size_t byte_count,
                                             HANDLE shutdown_event,
                                             bool wait_for_unpainted_budget)
{
    (void)decoder;
    if (bridge == 0 || bytes == 0 || byte_count == 0u) {
        return false;
    }
    if (wait_for_unpainted_budget &&
        !winxterm_bridge_wait_for_unpainted_budget(bridge, shutdown_event)) {
        return false;
    }

    winxterm_bridge_note_output_batch(bridge, byte_count);
    return winxterm_bridge_enqueue_output_with_unpainted_lines(
        bridge,
        bytes,
        byte_count,
        wait_for_unpainted_budget ? winxterm_client_count_output_lines(bytes, byte_count) : 0u);
}

static HANDLE winxterm_demo_create_timer(void)
{
    HANDLE timer = CreateWaitableTimerExW(0, 0, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (timer == 0) {
        timer = CreateWaitableTimerExW(0, 0, 0, TIMER_ALL_ACCESS);
    }
    return timer;
}

static bool winxterm_demo_wait_ms(HANDLE timer, HANDLE shutdown_event, DWORD milliseconds)
{
    if (timer == 0) {
        return WaitForSingleObject(shutdown_event, milliseconds) == WAIT_TIMEOUT;
    }

    LARGE_INTEGER due_time;
    due_time.QuadPart = -(LONGLONG)milliseconds * 10000ll;
    if (!SetWaitableTimer(timer, &due_time, 0, 0, 0, FALSE)) {
        return WaitForSingleObject(shutdown_event, milliseconds) == WAIT_TIMEOUT;
    }

    HANDLE waits[2] = {shutdown_event, timer};
    return WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0 + 1u;
}

static size_t winxterm_demo_append(char *buffer, size_t capacity, size_t offset, const char *text)
{
    while (text != 0 && *text != '\0' && offset + 1u < capacity) {
        buffer[offset++] = *text++;
    }
    if (capacity != 0u) {
        buffer[offset] = '\0';
    }
    return offset;
}

static unsigned long long winxterm_demo_count_output_lines(const char *buffer, size_t byte_count)
{
    unsigned long long line_count = 0u;
    for (size_t i = 0; i < byte_count; ++i) {
        if (buffer[i] == '\n') {
            ++line_count;
        }
    }
    return line_count;
}

static size_t winxterm_demo_append_colored_line(char *buffer,
                                                size_t capacity,
                                                size_t offset,
                                                const char *text)
{
    static const char *background_sgr[] = {
        "\x1b[49m",
        "\x1b[40m",
        "\x1b[42m",
        "\x1b[44m",
        "\x1b[41m",
    };

    size_t visible = 0u;
    while (text != 0 && *text != '\0' && offset + 1u < capacity) {
        if ((visible % 10u) == 0u) {
            offset = winxterm_demo_append(buffer,
                                          capacity,
                                          offset,
                                          background_sgr[(visible / 10u) % 5u]);
        }
        buffer[offset++] = *text++;
        buffer[offset] = '\0';
        ++visible;
    }

    offset = winxterm_demo_append(buffer, capacity, offset, "\x1b[49m\n");
    return offset;
}

DWORD winxterm_client_run_demo(WinxtermBridge *bridge, HANDLE shutdown_event)
{
    if (bridge == 0) {
        return 1;
    }

    WinxtermUtf8Decoder decoder;
    winxterm_utf8_decoder_init(&decoder);

    static const char alphabet[] =
        " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"
        "`abcdefghijklmnopqrstuvwxyz{|}~";
    static const char emoji_demo_text[] =
        "\xf0\x9f\x98\x80"
        " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_"
        "`abcdefghijklmnopqrstuvwxyz{|}~";

    DWORD frame = 0;
    unsigned long long output_lines = 0u;
    unsigned long long output_bytes = 0u;
    HANDLE demo_timer = winxterm_demo_create_timer();
    while (WaitForSingleObject(shutdown_event, 0) == WAIT_TIMEOUT) {
        if (bridge->cycle_render_backends) {
            WinxtermRenderBackend cycled_backend =
                (WinxtermRenderBackend)((frame / 180u) % WINXTERM_RENDER_BACKEND_COUNT);
            winxterm_bridge_set_backend(bridge, cycled_backend);
        }
        WinxtermRenderBackend backend = winxterm_bridge_backend(bridge);

        char buffer[1024];
        int header = snprintf(buffer,
                              sizeof(buffer),
                              "\xf0\x9f\x98\x80 demo frame=%lu lines=%llu bytes=%llu renderer=%s\n",
                              (unsigned long)frame,
                              output_lines,
                              output_bytes,
                              winxterm_render_backend_name(backend));
        if (header > 0) {
            size_t offset = (size_t)header < sizeof(buffer) ? (size_t)header : sizeof(buffer) - 1u;
            offset = winxterm_demo_append_colored_line(buffer, sizeof(buffer), offset, emoji_demo_text);
            offset = winxterm_demo_append_colored_line(buffer, sizeof(buffer), offset, alphabet + 1);
            offset = winxterm_demo_append_colored_line(buffer, sizeof(buffer), offset, alphabet + 2);
            unsigned long long batch_lines = winxterm_demo_count_output_lines(buffer, offset);
            if (winxterm_client_write_bytes_with_policy(bridge,
                                                        &decoder,
                                                        (const uint8_t *)buffer,
                                                        offset,
                                                        shutdown_event,
                                                        false)) {
                output_lines += batch_lines;
                output_bytes += (unsigned long long)offset;
            }
        }

        ++frame;
        if (!winxterm_demo_wait_ms(demo_timer, shutdown_event, 8)) {
            break;
        }
    }

    if (demo_timer != 0) {
        CloseHandle(demo_timer);
    }
    return 0;
}

DWORD winxterm_client_run_process(WinxtermBridge *bridge,
                                  const wchar_t * const *argv,
                                  int argc,
                                  HANDLE shutdown_event)
{
    return winxterm_host_run_conpty(bridge, argv, argc, shutdown_event);
}

DWORD winxterm_client_run_process_in_directory(WinxtermBridge *bridge,
                                               const wchar_t * const *argv,
                                               int argc,
                                               const wchar_t *current_directory,
                                               HANDLE shutdown_event)
{
    return winxterm_host_run_conpty_in_directory(bridge,
                                                argv,
                                                argc,
                                                current_directory,
                                                shutdown_event);
}

static double winxterm_glyphbench_seconds(LARGE_INTEGER start, LARGE_INTEGER end, LARGE_INTEGER frequency)
{
    return (double)(end.QuadPart - start.QuadPart) / (double)frequency.QuadPart;
}

static ULONGLONG winxterm_glyphbench_filetime_ticks(FILETIME filetime)
{
    ULARGE_INTEGER value;
    value.LowPart = filetime.dwLowDateTime;
    value.HighPart = filetime.dwHighDateTime;
    return value.QuadPart;
}

static double winxterm_glyphbench_process_cpu_seconds(void)
{
    FILETIME create_time;
    FILETIME exit_time;
    FILETIME kernel_time;
    FILETIME user_time;
    if (!GetProcessTimes(GetCurrentProcess(), &create_time, &exit_time, &kernel_time, &user_time)) {
        return 0.0;
    }
    ULONGLONG cpu_ticks = winxterm_glyphbench_filetime_ticks(kernel_time) +
                          winxterm_glyphbench_filetime_ticks(user_time);
    return (double)cpu_ticks / 10000000.0;
}

int winxterm_glyphbench_run(WinxtermLog *log, WinxtermRenderBackend selected_backend, bool selected_only)
{
    enum {
        width = WINXTERM_INITIAL_PIXEL_WIDTH,
        height = WINXTERM_INITIAL_PIXEL_HEIGHT,
        iterations = 2000,
        rounds = 10
    };

    uint32_t *front = (uint32_t *)malloc((size_t)width * (size_t)height * sizeof(*front));
    uint32_t *back = (uint32_t *)malloc((size_t)width * (size_t)height * sizeof(*back));
    if (front == 0 || back == 0) {
        free(front);
        free(back);
        return 1;
    }

    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    int first_backend = selected_only ? (int)selected_backend : 0;
    int backend_limit = selected_only ? (int)selected_backend + 1 : WINXTERM_RENDER_BACKEND_COUNT;
    winxterm_log_writef(log,
                        "glyphbench profile rounds=%d iterations_per_round=%d cells=%dx%d",
                        rounds,
                        iterations,
                        WINXTERM_TERMINAL_COLUMNS,
                        WINXTERM_TERMINAL_ROWS);
    for (int backend = first_backend; backend < backend_limit; ++backend) {
        WinxtermRenderContext context;
        winxterm_render_context_init(&context);
        (void)winxterm_render_context_load_fallback_fonts(&context, GetModuleHandleW(0), log);
        winxterm_render_draw_cell_glyph(&context,
                                        back,
                                        width,
                                        height,
                                        0,
                                        0,
                                        WINXTERM_MISSING_GLYPH_INDEX,
                                        0x20acu,
                                        0,
                                        0u,
                                        1u,
                                        WINXTERM_DEFAULT_FOREGROUND_RGB,
                                        WINXTERM_DEFAULT_BACKGROUND_RGB,
                                        (WinxtermRenderBackend)backend);
        winxterm_render_draw_cell_glyph(&context,
                                        back,
                                        width,
                                        height,
                                        1,
                                        0,
                                        WINXTERM_MISSING_GLYPH_INDEX,
                                        0x2211u,
                                        0,
                                        0u,
                                        1u,
                                        WINXTERM_DEFAULT_FOREGROUND_RGB,
                                        WINXTERM_DEFAULT_BACKGROUND_RGB,
                                        (WinxtermRenderBackend)backend);
        winxterm_render_draw_cell_glyph(&context,
                                        back,
                                        width,
                                        height,
                                        2,
                                        0,
                                        WINXTERM_MISSING_GLYPH_INDEX,
                                        0x1f600u,
                                        0,
                                        0u,
                                        2u,
                                        WINXTERM_DEFAULT_FOREGROUND_RGB,
                                        WINXTERM_DEFAULT_BACKGROUND_RGB,
                                        (WinxtermRenderBackend)backend);
        double total_wall_seconds = 0.0;
        double total_cpu_seconds = 0.0;
        double best_wall_seconds = 0.0;
        double best_cpu_seconds = 0.0;
        for (int round = 0; round < rounds; ++round) {
            winxterm_render_clear(back, width, height, WINXTERM_DEFAULT_BACKGROUND_RGB);
            LARGE_INTEGER start;
            LARGE_INTEGER end;
            double cpu_start = winxterm_glyphbench_process_cpu_seconds();
            QueryPerformanceCounter(&start);
            for (int iteration = 0; iteration < iterations; ++iteration) {
                for (int row = 0; row < WINXTERM_TERMINAL_ROWS; ++row) {
                    for (int col = 0; col < WINXTERM_TERMINAL_COLUMNS; ++col) {
                        uint32_t glyph = (uint32_t)((row * WINXTERM_TERMINAL_COLUMNS + col + iteration) & 0xff);
                        winxterm_render_draw_glyph(&context,
                                                   back,
                                                   width,
                                                   height,
                                                   col,
                                                   row,
                                                   glyph,
                                                   WINXTERM_DEFAULT_FOREGROUND_RGB,
                                                   WINXTERM_DEFAULT_BACKGROUND_RGB,
                                                   (WinxtermRenderBackend)backend);
                    }
                }
                winxterm_render_swap(&front, &back);
            }
            QueryPerformanceCounter(&end);
            double cpu_end = winxterm_glyphbench_process_cpu_seconds();
            double wall_seconds = winxterm_glyphbench_seconds(start, end, frequency);
            double cpu_seconds = cpu_end - cpu_start;
            double frames_per_second = wall_seconds > 0.0 ? (double)iterations / wall_seconds : 0.0;
            double glyphs = (double)iterations * (double)WINXTERM_TERMINAL_ROWS * (double)WINXTERM_TERMINAL_COLUMNS;
            total_wall_seconds += wall_seconds;
            total_cpu_seconds += cpu_seconds;
            if (round == 0 || wall_seconds < best_wall_seconds) {
                best_wall_seconds = wall_seconds;
            }
            if (round == 0 || cpu_seconds < best_cpu_seconds) {
                best_cpu_seconds = cpu_seconds;
            }
            winxterm_log_writef(log,
                                "glyphbench backend=%s round=%d wall_seconds=%.6f cpu_seconds=%.6f fps=%.2f glyphs_per_second=%.2f cached_glyphs=%zu fallback_cached=%zu fallback_misses=%zu",
                                winxterm_render_backend_name((WinxtermRenderBackend)backend),
                                round + 1,
                                wall_seconds,
                                cpu_seconds,
                                frames_per_second,
                                wall_seconds > 0.0 ? glyphs / wall_seconds : 0.0,
                                context.precolored_count,
                                winxterm_render_context_fallback_cached_count(&context),
                                winxterm_render_context_fallback_miss_count(&context));
        }
        winxterm_log_writef(log,
                            "glyphbench summary backend=%s rounds=%d total_wall_seconds=%.6f total_cpu_seconds=%.6f average_wall_seconds=%.6f average_cpu_seconds=%.6f best_wall_seconds=%.6f best_cpu_seconds=%.6f average_fps=%.2f",
                            winxterm_render_backend_name((WinxtermRenderBackend)backend),
                            rounds,
                            total_wall_seconds,
                            total_cpu_seconds,
                            total_wall_seconds / (double)rounds,
                            total_cpu_seconds / (double)rounds,
                            best_wall_seconds,
                            best_cpu_seconds,
                            total_wall_seconds > 0.0 ? ((double)rounds * (double)iterations) / total_wall_seconds : 0.0);
        winxterm_render_context_dispose(&context);
    }

    free(front);
    free(back);
    return 0;
}
