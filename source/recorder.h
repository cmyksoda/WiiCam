/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef WC04_RECORDER_H
#define WC04_RECORDER_H
#include "avi.h"
typedef struct {
    Avi avi;
    char path[128];
    uint64_t started, last_frame;
    bool active, failed, limited, saved;
    unsigned duration_ms;
} Recorder;
void recorder_init(Recorder *r);
bool recorder_start(Recorder *r, const char *directory, unsigned width, unsigned height, uint64_t now);
bool recorder_frame(Recorder *r, const uint8_t *jpeg, size_t length, uint64_t now);
bool recorder_stop(Recorder *r, uint64_t now);
#endif
