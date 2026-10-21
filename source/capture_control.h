/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WC04_CAPTURE_CONTROL_H
#define WC04_CAPTURE_CONTROL_H
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>

typedef struct {
    unsigned delay, remaining;
    uint64_t deadline;
    bool counting, shutter;
} CaptureControl;

typedef enum {
    CONTROL_NONE,
    CONTROL_PHOTO,
    CONTROL_BACK,
    CONTROL_EXIT,
    CONTROL_SIGNAL_LOST
} CaptureAction;

void capture_control_init(CaptureControl *control, unsigned delay);
unsigned capture_timer_next(unsigned delay, int direction);
CaptureAction capture_control_update(CaptureControl *control, uint64_t now_ms, bool a, bool b,
                                     bool exit, bool signal, bool storage);

enum {
    RECORD_READY,
    RECORD_REQUESTED,
    RECORD_STARTING,
    RECORD_ACTIVE,
    RECORD_FAILED,
    RECORD_LIMIT,
    RECORD_STOPPING
};

typedef struct {
    atomic_uint state, started_ms, stopped_ms;
} RecordControl;

void record_control_init(RecordControl *control);
bool record_control_request(RecordControl *control);
bool record_control_begin(RecordControl *control);
bool record_control_started(RecordControl *control, bool success, unsigned now_ms);
void record_control_result(RecordControl *control, unsigned result);
/* Single input thread calls once per session, before joining the writer. */
void record_control_stop(RecordControl *control, unsigned now_ms);

typedef enum {
    REVIEW_NONE,
    REVIEW_LIVE,
    REVIEW_MENU,
    REVIEW_PLAY,
    REVIEW_CONFIRM,
    REVIEW_CANCEL,
    REVIEW_DELETE
} ReviewAction;

typedef struct {
    bool file, playable, armed;
} ReviewControl;

/* Minus arms deletion and a second Minus confirms; A, B or 1 in between disarms. */
ReviewAction review_control_update(ReviewControl *control, bool a, bool b, bool one, bool minus);
#endif
