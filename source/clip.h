/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WIICAM_CLIP_H
#define WIICAM_CLIP_H
#include "av_avi.h"

typedef struct {
    FILE *file;
    unsigned width, height, frame_us, frames, blocks;
    uint32_t movi;
    uint32_t frame_offsets[AVI_MAX_FRAMES], frame_lengths[AVI_MAX_FRAMES];
    uint32_t block_offsets[AV_MAX_FRAMES];
} Clip;

/* Reads the AVIs WiiCam writes: idx1 offsets relative to the movi list, PCM in 3200-byte blocks. */
bool clip_open(Clip *clip, const char *path);
bool clip_frame(Clip *clip, unsigned index, uint8_t *jpeg, size_t capacity, size_t *length);
/* Returns the PCM16 little-endian bytes read, or 0 for a silent or damaged clip. */
size_t clip_audio(Clip *clip, uint8_t *pcm, size_t capacity);
void clip_close(Clip *clip);
#endif
