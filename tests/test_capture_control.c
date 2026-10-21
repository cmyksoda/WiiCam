/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "capture_control.h"
#include <assert.h>
#include <stdio.h>
#define STEP(t, a, b, e, s, sd) capture_control_update(&c, t, a, b, e, s, sd)

int main(void)
{
    CaptureControl c;
    assert(capture_timer_next(0, 1) == 3);
    assert(capture_timer_next(3, 1) == 5);
    assert(capture_timer_next(5, 1) == 10);
    assert(capture_timer_next(10, 1) == 0);
    assert(capture_timer_next(0, -1) == 10);
    for (unsigned repeat = 0; repeat < 20; ++repeat) {
        const unsigned delays[] = {0, 3, 5, 10};
        for (unsigned n = 0; n < 4; ++n) {
            unsigned delay = delays[n];
            capture_control_init(&c, delay);
            assert(STEP(100, true, false, false, true, true)
                   == (delay ? CONTROL_NONE : CONTROL_PHOTO));
            if (delay) {
                assert(c.remaining == delay);
                assert(STEP(100 + delay * 1000 - 1, true, false, false, true, true)
                       == CONTROL_NONE);
                assert(c.remaining == 1);
                assert(STEP(100 + delay * 1000, false, false, false, true, true) == CONTROL_PHOTO);
            }
            assert(STEP(20000, true, false, false, true, true) == CONTROL_NONE);
        }
    }
    capture_control_init(&c, 3);
    STEP(0, true, false, false, true, true);
    assert(STEP(3000, false, true, false, true, true) == CONTROL_NONE);
    assert(!c.counting && !c.shutter);
    assert(STEP(4000, false, false, false, true, true) == CONTROL_NONE);
    assert(STEP(4001, false, true, false, true, true) == CONTROL_BACK);
    STEP(5000, true, false, false, true, true);
    STEP(6000, false, false, false, false, true);
    assert(!c.counting);
    assert(STEP(9000, false, false, false, true, true) == CONTROL_NONE);
    STEP(10000, true, false, false, true, true);
    STEP(11000, false, false, false, true, false);
    assert(!c.counting);
    assert(STEP(12000, true, false, true, true, true) == CONTROL_EXIT);
    capture_control_init(&c, 0);
    assert(STEP(0, true, false, false, false, true) == CONTROL_NONE);
    assert(STEP(1, true, false, false, true, false) == CONTROL_NONE);
    capture_control_init(&c, 3);
    STEP(100, true, false, false, true, true);
    STEP(500, false, true, false, true, true);
    STEP(700, true, false, false, true, true);
    assert(c.deadline == 3700);
    assert(STEP(3100, false, false, false, true, true) == CONTROL_NONE);
    assert(STEP(3700, false, false, false, true, true) == CONTROL_PHOTO);
    assert(STEP(4700, false, false, false, false, true) == CONTROL_SIGNAL_LOST);
    assert(!c.shutter && !c.counting);

    RecordControl record;
    record_control_init(&record);
    assert(record_control_request(&record));
    /* Decoder holds a frame while A/B/HOME cancels pending start. */
    record_control_stop(&record, 1200);
    assert(!record_control_begin(&record));
    assert(!record_control_request(&record));
    assert(atomic_load(&record.state) == RECORD_STOPPING);
    assert(atomic_load(&record.stopped_ms) == 1200);
    /* Stop while the writer is opening the file must not reactivate it. */
    record_control_init(&record);
    assert(record_control_request(&record) && record_control_begin(&record));
    record_control_stop(&record, 1500);
    assert(!record_control_started(&record, true, 1000));
    assert(atomic_load(&record.state) == RECORD_STOPPING);
    /* An in-flight write finishing late cannot alter the accepted stop time. */
    record_control_init(&record);
    assert(record_control_request(&record) && record_control_begin(&record));
    assert(record_control_started(&record, true, 1000));
    record_control_stop(&record, 1700);
    record_control_result(&record, RECORD_LIMIT);
    assert(atomic_load(&record.state) == RECORD_STOPPING);
    assert(atomic_load(&record.stopped_ms) == 1700);
    record_control_init(&record);
    assert(record_control_request(&record) && record_control_begin(&record));
    assert(record_control_started(&record, false, 2000));
    assert(atomic_load(&record.state) == RECORD_FAILED);

#define REVIEW(a, b, one, minus) review_control_update(&review, a, b, one, minus)
    ReviewControl review = {.file = true, .playable = true};
    assert(REVIEW(false, false, false, false) == REVIEW_NONE);
    assert(REVIEW(false, false, false, true) == REVIEW_CONFIRM && review.armed);
    /* Idle frames keep the confirmation armed; only a second Minus deletes. */
    assert(REVIEW(false, false, false, false) == REVIEW_NONE && review.armed);
    assert(REVIEW(false, false, false, true) == REVIEW_DELETE && !review.armed);
    review.file = false;
    assert(REVIEW(false, false, false, true) == REVIEW_NONE);
    assert(REVIEW(false, false, true, false) == REVIEW_NONE);
    assert(REVIEW(true, false, false, false) == REVIEW_LIVE);
    assert(REVIEW(false, true, false, false) == REVIEW_MENU);
    review = (ReviewControl){.file = true, .playable = true};
    REVIEW(false, false, false, true);
    assert(REVIEW(false, false, true, false) == REVIEW_PLAY && !review.armed);
    assert(REVIEW(false, false, false, true) == REVIEW_CONFIRM);
    assert(REVIEW(false, true, false, false) == REVIEW_MENU && !review.armed);
    REVIEW(false, false, false, true);
    assert(REVIEW(true, false, false, false) == REVIEW_LIVE && !review.armed);
    /* Photos have no playback: 1 only cancels a pending delete. */
    review = (ReviewControl){.file = true};
    assert(REVIEW(false, false, true, false) == REVIEW_NONE);
    REVIEW(false, false, false, true);
    assert(REVIEW(false, false, true, false) == REVIEW_CANCEL && !review.armed);
    assert(REVIEW(false, false, false, true) == REVIEW_CONFIRM);
    assert(REVIEW(false, true, false, true) == REVIEW_MENU && !review.armed);
    puts(
        "PASS: immediate/repeated capture, 3/5/10 s, cancel/restart, pending shutter loss, record start/stop interleavings, latched duration, review delete/play/cancel");
}
