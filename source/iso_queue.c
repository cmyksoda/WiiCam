/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */

#include "iso_queue.h"
#include <string.h>

_Static_assert(ISO_PACKETS *ISO_MAX_PAYLOAD <= UINT16_MAX,
               "libogc uses a 16-bit aggregate transfer length");
_Static_assert(ISO_DATA_BYTES % 32 == 0 && ISO_PACKETS * sizeof(uint16_t) % 32 == 0,
               "every IOS data and size bank must remain cache-line aligned");
_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "USB callbacks require lock-free completion flags");

void iso_queue_init(IsoQueue *queue, uint8_t *data, uint16_t *sizes, unsigned capacity)
{
    memset(queue, 0, sizeof(*queue));
    queue->capacity = capacity;
    for (unsigned i = 0; i < ISO_QUEUE_DEPTH; ++i) {
        queue->slots[i].data = data + i * ISO_DATA_BYTES;
        queue->slots[i].sizes = sizes + i * ISO_PACKETS;
        atomic_init(&queue->slots[i].completed, 0);
    }
}

void iso_queue_complete(IsoSlot *slot, int result)
{
    slot->result = result;
    atomic_store_explicit(&slot->completed, 1, memory_order_release);
}

static bool completed(const IsoSlot *slot)
{
    return atomic_load_explicit(&slot->completed, memory_order_acquire) != 0;
}

static bool submit(IsoQueue *queue, IsoSlot *slot, const IsoHooks *hooks, void *context)
{
    memset(slot->data, 0, ISO_PACKETS * queue->capacity);
    for (unsigned i = 0; i < ISO_PACKETS; ++i)
        slot->sizes[i] = queue->capacity;
    atomic_store_explicit(&slot->completed, 0, memory_order_relaxed);
    slot->pending = true;
    int result = hooks->submit(context, slot);
    if (result < 0) {
        slot->pending = false;
        queue->failure = result;
        return false;
    }
    ++queue->submitted;
    return true;
}

IsoStop iso_queue_run(IsoQueue *queue, const IsoHooks *hooks, void *context)
{
    IsoStop stop = ISO_RUNNING;
    if (!queue->capacity || queue->capacity > ISO_MAX_PAYLOAD) {
        queue->failure = -1;
        stop = ISO_SUBMIT_FAILED;
    }
    for (unsigned i = 0; stop == ISO_RUNNING && i < ISO_QUEUE_DEPTH; ++i) {
        if (!submit(queue, &queue->slots[i], hooks, context)) stop = ISO_SUBMIT_FAILED;
    }
    unsigned next = 0;
    while (stop == ISO_RUNNING) {
        stop = hooks->poll(context);
        if (stop != ISO_RUNNING) break;
        IsoSlot *slot = &queue->slots[next];
        if (!completed(slot)) {
            hooks->idle(context);
            continue;
        }
        slot->pending = false;
        if (slot->result < 0) {
            queue->failure = slot->result;
            stop = ISO_COMPLETION_FAILED;
            break;
        }
        /* Consume in submission order even if callbacks arrive out of order. */
        ++queue->consumed;
        if (hooks->consume(context, slot)) {
            stop = ISO_PHOTO_READY;
            break;
        }
        stop = hooks->poll(context);
        if (stop == ISO_RUNNING && !submit(queue, slot, hooks, context)) stop = ISO_SUBMIT_FAILED;
        next = (next + 1) % ISO_QUEUE_DEPTH;
    }
    hooks->stopping(context, stop, queue->failure);
    bool outstanding = false;
    for (unsigned i = 0; i < ISO_QUEUE_DEPTH; ++i)
        outstanding |= queue->slots[i].pending && !completed(&queue->slots[i]);
    if (outstanding) hooks->cancel(context);
    for (unsigned i = 0; i < ISO_QUEUE_DEPTH; ++i) {
        IsoSlot *slot = &queue->slots[i];
        if (!slot->pending) continue;
        /* Cancellation does not return ownership; every callback must arrive. */
        while (!completed(slot)) {
            hooks->poll(context);
            hooks->idle(context);
        }
        ++queue->drained;
        slot->pending = false;
    }
    return stop;
}
