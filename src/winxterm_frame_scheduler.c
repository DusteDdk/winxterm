#include "winxterm_frame_scheduler.h"

#include <string.h>

void winxterm_frame_scheduler_init(WinxtermFrameScheduler *scheduler)
{
    if (scheduler != 0) {
        memset(scheduler, 0, sizeof(*scheduler));
        scheduler->mode = WINXTERM_FRAME_PACING_DEBOUNCED;
    }
}

size_t winxterm_frame_scheduler_recent_visible_updates(const WinxtermFrameScheduler *scheduler,
                                                       uint64_t now_ns)
{
    if (scheduler == 0) return 0u;
    size_t recent = 0u;
    for (size_t i = 0u; i < scheduler->visible_update_count; ++i) {
        uint64_t timestamp = scheduler->visible_update_times[i];
        if (timestamp <= now_ns && now_ns - timestamp <= WINXTERM_VISIBLE_RATE_WINDOW_NS) {
            ++recent;
        }
    }
    return recent;
}

static void winxterm_frame_scheduler_update_mode(WinxtermFrameScheduler *scheduler,
                                                 uint64_t now_ns)
{
    size_t recent = winxterm_frame_scheduler_recent_visible_updates(scheduler, now_ns);
    WinxtermFramePacingMode next = scheduler->mode;
    if (recent >= WINXTERM_VISIBLE_RATE_SAMPLE_CAPACITY) {
        next = WINXTERM_FRAME_PACING_SUSTAINED;
    } else if (recent <= 15u) {
        next = WINXTERM_FRAME_PACING_DEBOUNCED;
    }
    if (next != scheduler->mode) {
        scheduler->mode = next;
        ++scheduler->mode_transitions;
    }
}

static void winxterm_frame_scheduler_arm(WinxtermFrameScheduler *scheduler,
                                         uint64_t now_ns,
                                         WinxtermFramePacingMode previous_mode)
{
    uint64_t deadline = now_ns + WINXTERM_FRAME_INTERVAL_NS;
    if (deadline < now_ns) deadline = UINT64_MAX;
    if (scheduler->mode == WINXTERM_FRAME_PACING_SUSTAINED) {
        if (scheduler->deadline_armed && previous_mode == scheduler->mode) return;
        uint64_t cadence = deadline;
        if (scheduler->last_present_ns != 0u) {
            cadence = scheduler->last_present_ns + WINXTERM_FRAME_INTERVAL_NS;
            if (cadence < scheduler->last_present_ns) cadence = UINT64_MAX;
            if (cadence < now_ns) cadence = now_ns;
        }
        if (scheduler->deadline_armed && scheduler->deadline_ns < cadence) {
            cadence = scheduler->deadline_ns;
        }
        deadline = cadence;
    }

    if (scheduler->deadline_armed && scheduler->deadline_ns != deadline) {
        ++scheduler->deadline_rearms;
    }
    scheduler->deadline_armed = true;
    scheduler->deadline_ns = deadline;
}

void winxterm_frame_scheduler_schedule(WinxtermFrameScheduler *scheduler,
                                       uint64_t now_ns)
{
    if (scheduler == 0) return;
    WinxtermFramePacingMode previous_mode = scheduler->mode;
    winxterm_frame_scheduler_update_mode(scheduler, now_ns);
    winxterm_frame_scheduler_arm(scheduler, now_ns, previous_mode);
}

void winxterm_frame_scheduler_note_visible_update(WinxtermFrameScheduler *scheduler,
                                                  uint64_t now_ns)
{
    if (scheduler == 0) return;
    WinxtermFramePacingMode previous_mode = scheduler->mode;
    scheduler->visible_update_times[scheduler->visible_update_next] = now_ns;
    scheduler->visible_update_next =
        (scheduler->visible_update_next + 1u) % WINXTERM_VISIBLE_RATE_SAMPLE_CAPACITY;
    if (scheduler->visible_update_count < WINXTERM_VISIBLE_RATE_SAMPLE_CAPACITY) {
        ++scheduler->visible_update_count;
    }
    ++scheduler->visible_updates;
    winxterm_frame_scheduler_update_mode(scheduler, now_ns);
    winxterm_frame_scheduler_arm(scheduler, now_ns, previous_mode);
}

bool winxterm_frame_scheduler_due(const WinxtermFrameScheduler *scheduler,
                                  uint64_t now_ns)
{
    return scheduler != 0 && scheduler->deadline_armed &&
           now_ns >= scheduler->deadline_ns;
}

void winxterm_frame_scheduler_note_deadline_fire(WinxtermFrameScheduler *scheduler)
{
    if (scheduler != 0) ++scheduler->deadline_fires;
}

void winxterm_frame_scheduler_note_presented(WinxtermFrameScheduler *scheduler,
                                             uint64_t now_ns)
{
    if (scheduler == 0) return;
    scheduler->last_present_ns = now_ns;
    scheduler->deadline_armed = false;
    scheduler->deadline_ns = 0u;
}

void winxterm_frame_scheduler_cancel(WinxtermFrameScheduler *scheduler)
{
    if (scheduler == 0) return;
    scheduler->deadline_armed = false;
    scheduler->deadline_ns = 0u;
}

uint64_t winxterm_frame_scheduler_delay_ns(const WinxtermFrameScheduler *scheduler,
                                           uint64_t now_ns)
{
    if (scheduler == 0 || !scheduler->deadline_armed ||
        scheduler->deadline_ns <= now_ns) {
        return 0u;
    }
    return scheduler->deadline_ns - now_ns;
}
