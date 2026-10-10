/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WIICAM_AV_AVI_H
#define WIICAM_AV_AVI_H
#include "avi.h"
#include "audio_stream.h"
#define AV_MAX_FRAMES (AVI_MAX_MS / AUDIO_FRAME_MS)

typedef struct {
    FILE *file;
    unsigned width, height, frames, bytes, largest;
    uint32_t offsets[AV_MAX_FRAMES * 2], lengths[AV_MAX_FRAMES * 2];
    bool failed, finished;
} AvAvi;

bool av_avi_begin(AvAvi *avi, FILE *file, unsigned width, unsigned height);
bool av_avi_has_space(const AvAvi *avi, size_t length);
/* One JPEG and 1600 PCM16 mono samples describe the same 100 ms interval. */
bool av_avi_pair(AvAvi *avi, const uint8_t *jpeg, size_t length, const uint8_t *pcm);
bool av_avi_finish(AvAvi *avi);
#endif
