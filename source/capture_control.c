/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "capture_control.h"

void capture_control_init(CaptureControl *c, unsigned delay)
{
    *c = (CaptureControl){.delay = delay == 3 || delay == 5 || delay == 10 ? delay : 0};
}

unsigned capture_timer_next(unsigned delay, int direction)
{
    const unsigned timers[] = {0, 3, 5, 10};
    for (unsigned i = 0; i < 4; ++i)
        if (timers[i] == delay) return timers[(i + (direction < 0 ? 3 : 1)) % 4];
    return 0;
}

CaptureAction capture_control_update(CaptureControl *c, uint64_t now, bool a, bool b, bool exit,
                                     bool signal, bool storage)
{
    if (exit) {
        c->counting = false;
        c->remaining = 0;
        return CONTROL_EXIT;
    }
    if (b) {
        if (!c->counting) return CONTROL_BACK;
        c->counting = false;
        c->remaining = 0;
        return CONTROL_NONE;
    }
    if (!signal || !storage) {
        c->counting = false;
        c->remaining = 0;
        if (c->shutter) {
            c->shutter = false;
            return CONTROL_SIGNAL_LOST;
        }
        return CONTROL_NONE;
    }
    if (c->shutter) return CONTROL_NONE;
    if (a && !c->counting) {
        c->counting = true;
        c->deadline = now + (uint64_t)c->delay * 1000;
    }
    if (c->counting) {
        if (now >= c->deadline) {
            c->counting = false;
            c->remaining = 0;
            c->shutter = true;
            return CONTROL_PHOTO;
        }
        c->remaining = (unsigned)((c->deadline - now + 999) / 1000);
    }
    return CONTROL_NONE;
}

void record_control_init(RecordControl *c)
{
    atomic_init(&c->state, RECORD_READY);
    atomic_init(&c->started_ms, 0);
    atomic_init(&c->stopped_ms, 0);
}

bool record_control_request(RecordControl *c)
{
    unsigned expected = RECORD_READY;
    return atomic_compare_exchange_strong(&c->state, &expected, RECORD_REQUESTED);
}

bool record_control_begin(RecordControl *c)
{
    unsigned expected = RECORD_REQUESTED;
    return atomic_compare_exchange_strong(&c->state, &expected, RECORD_STARTING);
}

bool record_control_started(RecordControl *c, bool success, unsigned now)
{
    atomic_store(&c->started_ms, now);
    unsigned expected = RECORD_STARTING;
    return atomic_compare_exchange_strong(&c->state, &expected,
                                          success ? RECORD_ACTIVE : RECORD_FAILED);
}

void record_control_result(RecordControl *c, unsigned result)
{
    unsigned expected = RECORD_ACTIVE;
    atomic_compare_exchange_strong(&c->state, &expected, result);
}

void record_control_stop(RecordControl *c, unsigned now)
{
    atomic_store(&c->stopped_ms, now);
    atomic_exchange(&c->state, RECORD_STOPPING);
}
