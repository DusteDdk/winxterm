#ifndef WINXTERM_DIAGNOSTICS_H
#define WINXTERM_DIAGNOSTICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <windows.h>

#define WINXTERM_DIAG_GROUPED_U64_CAPACITY 32u

typedef struct WinxtermCommandDiagnostics {
    uint64_t command_entered_ns;
    uint64_t started_ns;
    uint64_t command_finished_ns;
    uint64_t prompt_ready_ns;
    uint64_t elapsed_ns;
    uint64_t parse_ns;
    uint64_t builtin_lookup_ns;
    uint64_t builtin_run_ns;
    uint64_t program_resolve_ns;
    uint64_t exec_run_ns;
    uint64_t process_run_ns;
    uint64_t finish_to_prompt_ns;
    uint64_t builtin_output_bytes;
    uint64_t external_output_bytes;
    uint64_t total_output_bytes;
    uint64_t rendered_output_bytes;
    uint64_t skipped_output_bytes;
    uint64_t shell_write_calls;
    uint64_t external_read_calls;
    uint64_t output_feed_calls;
    uint64_t input_enqueue_calls;
    uint64_t input_enqueue_bytes;
    uint64_t terminal_refresh_calls;
    uint64_t update_requests;
    uint64_t coalesced_update_requests;
    uint64_t render_messages_handled;
    uint64_t output_batches;
    uint64_t output_batch_bytes;
    uint64_t output_batch_max_bytes;
    uint64_t rendered_frames;
    uint64_t rendered_cells;
    uint64_t skipped_cells;
    uint64_t empty_cell_skips;
    uint64_t continuation_cell_skips;
    uint64_t glyph_draw_calls;
    uint64_t glyph_rendered_count;
    uint64_t glyph_cache_hits;
    uint64_t glyph_cache_misses;
    uint64_t precolored_cache_hits;
    uint64_t precolored_cache_misses;
    uint64_t fallback_cache_hits;
    uint64_t fallback_cache_misses;
    uint64_t render_prepare_ns;
    uint64_t render_snapshot_ns;
    uint64_t render_dispatch_wait_ns;
    uint64_t render_worker_total_ns;
    uint64_t render_worker_max_ns;
    uint64_t render_flip_ns;
    uint64_t render_present_ns;
    uint64_t dirty_rows_rendered;
    uint64_t full_repaints;
    uint64_t scroll_blits;
} WinxtermCommandDiagnostics;

static inline void winxterm_diag_add_u64(uint64_t *field, uint64_t value)
{
    if (field != 0 && value != 0u) {
        (void)InterlockedAdd64((volatile LONG64 *)field, (LONG64)value);
    }
}

static inline void winxterm_diag_inc_u64(uint64_t *field)
{
    if (field != 0) {
        (void)InterlockedIncrement64((volatile LONG64 *)field);
    }
}

static inline bool winxterm_diag_format_grouped_u64(uint64_t value, wchar_t *buffer, size_t buffer_count)
{
    if (buffer == 0 || buffer_count == 0u) {
        return false;
    }

    wchar_t reverse[WINXTERM_DIAG_GROUPED_U64_CAPACITY];
    size_t reverse_count = 0u;
    unsigned int group_count = 0u;
    do {
        if (group_count == 3u) {
            if (reverse_count + 1u >= WINXTERM_DIAG_GROUPED_U64_CAPACITY) {
                buffer[0] = L'\0';
                return false;
            }
            reverse[reverse_count++] = L' ';
            group_count = 0u;
        }
        if (reverse_count + 1u >= WINXTERM_DIAG_GROUPED_U64_CAPACITY) {
            buffer[0] = L'\0';
            return false;
        }
        reverse[reverse_count++] = (wchar_t)(L'0' + (value % 10u));
        value /= 10u;
        ++group_count;
    } while (value != 0u);

    if (reverse_count + 1u > buffer_count) {
        buffer[0] = L'\0';
        return false;
    }
    for (size_t i = 0u; i < reverse_count; ++i) {
        buffer[i] = reverse[reverse_count - i - 1u];
    }
    buffer[reverse_count] = L'\0';
    return true;
}

#endif
