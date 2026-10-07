/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 cmyksoda
 */
#include "preview.h"
#include <string.h>

void preview_init(Preview *p, uint8_t *jpeg, size_t capacity)
{
    atomic_init(&p->state, PREVIEW_EMPTY);
    atomic_init(&p->shutter, false);
    p->jpeg = jpeg;
    p->capacity = capacity;
    p->length = 0;
    p->save = false;
}

void preview_shutter(Preview *p)
{
    atomic_store_explicit(&p->shutter, true, memory_order_release);
}

bool preview_offer(Preview *p, const uint8_t *jpeg, size_t length)
{
    if (!length || length > p->capacity ||
        atomic_load_explicit(&p->state, memory_order_acquire) != PREVIEW_EMPTY)
        return false;
    memcpy(p->jpeg, jpeg, length);
    p->length = length;
    p->save = atomic_load_explicit(&p->shutter, memory_order_acquire);
    atomic_store_explicit(&p->state, PREVIEW_FRAME, memory_order_release);
    return true;
}

bool preview_take(Preview *p)
{
    return atomic_load_explicit(&p->state, memory_order_acquire) == PREVIEW_FRAME;
}

void preview_finish(Preview *p, bool decoded)
{
    atomic_store_explicit(&p->state, decoded && p->save ? PREVIEW_PHOTO : PREVIEW_EMPTY,
                          memory_order_release);
}

bool preview_photo_ready(const Preview *p)
{
    return atomic_load_explicit(&p->state, memory_order_acquire) == PREVIEW_PHOTO;
}
