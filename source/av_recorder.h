/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WIICAM_AV_RECORDER_H
#define WIICAM_AV_RECORDER_H
#include "av_avi.h"

typedef struct {
    AvAvi avi;
    AudioStream *audio;
    uint8_t *jpeg;
    size_t length;
    char path[128];
    unsigned anchor, repeated, source_generation, written_generation;
    bool active, failed, limited, saved;
} AvRecorder;

void av_recorder_init(AvRecorder *recorder, AudioStream *audio, uint8_t *jpeg);
bool av_recorder_start(AvRecorder *recorder, const char *directory, unsigned width, unsigned height,
                       const uint8_t *jpeg, size_t length);
bool av_recorder_frame(AvRecorder *recorder, const uint8_t *jpeg, size_t length,
                       unsigned audio_position);
bool av_recorder_pump(AvRecorder *recorder, unsigned until);
bool av_recorder_stop(AvRecorder *recorder, unsigned until, bool valid);
#endif
