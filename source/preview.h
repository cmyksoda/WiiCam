/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WC04_PREVIEW_H
#define WC04_PREVIEW_H
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { PREVIEW_EMPTY, PREVIEW_FRAME, PREVIEW_PHOTO };

typedef struct {
    atomic_uint state;
    atomic_bool shutter;
    uint8_t *jpeg;
    size_t capacity, length;
    unsigned audio_position;
    bool save;
} Preview;

void preview_init(Preview *preview, uint8_t *jpeg, size_t capacity);
void preview_shutter(Preview *preview);
/* Single producer. Busy frames are skipped, never overwrite decoder input. */
bool preview_offer(Preview *preview, const uint8_t *jpeg, size_t length);
bool preview_offer_at(Preview *preview, const uint8_t *jpeg, size_t length,
                      unsigned audio_position);
/* Single consumer owns the JPEG until preview_finish. */
bool preview_take(Preview *preview);
void preview_finish(Preview *preview, bool decoded);
bool preview_photo_ready(const Preview *preview);
#endif
