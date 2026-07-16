#ifndef WINXTERM_FRAME_SCHEDULER_H
#define WINXTERM_FRAME_SCHEDULER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WINXTERM_FRAME_INTERVAL_NS (16ull * 1000ull * 1000ull)
#define WINXTERM_VISIBLE_RATE_WINDOW_NS (1000ull * 1000ull * 1000ull)
#define WINXTERM_VISIBLE_RATE_SAMPLE_CAPACITY 17u

typedef enum WinxtermFramePacingMode {
    WINXTERM_FRAME_PACING_DEBOUNCED = 0,
    WINXTERM_FRAME_PACING_SUSTAINED
} WinxtermFramePacingMode;

typedef struct WinxtermFrameScheduler {
    uint64_t visible_update_times[WINXTERM_VISIBLE_RATE_SAMPLE_CAPACITY];
    size_t visible_update_next;
    size_t visible_update_count;
    WinxtermFramePacingMode mode;
    bool deadline_armed;
    uint64_t deadline_ns;
    uint64_t last_present_ns;
    uint64_t visible_updates;
    uint64_t deadline_rearms;
    uint64_t mode_transitions;
    uint64_t deadline_fires;
} WinxtermFrameScheduler;

void winxterm_frame_scheduler_init(WinxtermFrameScheduler *scheduler);
void winxterm_frame_scheduler_schedule(WinxtermFrameScheduler *scheduler,
                                       uint64_t now_ns);
void winxterm_frame_scheduler_note_visible_update(WinxtermFrameScheduler *scheduler,
                                                  uint64_t now_ns);
bool winxterm_frame_scheduler_due(const WinxtermFrameScheduler *scheduler,
                                  uint64_t now_ns);
void winxterm_frame_scheduler_note_deadline_fire(WinxtermFrameScheduler *scheduler);
void winxterm_frame_scheduler_note_presented(WinxtermFrameScheduler *scheduler,
                                             uint64_t now_ns);
void winxterm_frame_scheduler_cancel(WinxtermFrameScheduler *scheduler);
uint64_t winxterm_frame_scheduler_delay_ns(const WinxtermFrameScheduler *scheduler,
                                           uint64_t now_ns);
size_t winxterm_frame_scheduler_recent_visible_updates(const WinxtermFrameScheduler *scheduler,
                                                       uint64_t now_ns);

#endif
