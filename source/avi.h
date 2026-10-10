/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef WC04_AVI_H
#define WC04_AVI_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define AVI_MAX_FRAMES 3600u
#define AVI_MAX_BYTES (128u * 1024u * 1024u)
#define AVI_MAX_MS 60000u

typedef struct {
    FILE *file;
    unsigned width, height, frames, bytes, largest;
    uint32_t offsets[AVI_MAX_FRAMES], lengths[AVI_MAX_FRAMES];
    bool failed, finished;
} Avi;

/* Single writer. Caller closes the file and checks flush/close errors too. */
bool avi_begin(Avi *avi, FILE *file, unsigned width, unsigned height);
bool avi_has_space(const Avi *avi, size_t length);
bool avi_append(Avi *avi, const uint8_t *jpeg, size_t length);
/* Sampled frames use an average rate so the file keeps the measured duration. */
bool avi_finish(Avi *avi, unsigned duration_ms);
#endif
