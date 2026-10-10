/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 cmyksoda
 */

#include "iso_queue.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static _Alignas(32) uint8_t data[ISO_QUEUE_DEPTH * ISO_DATA_BYTES];
static _Alignas(32) uint16_t sizes[ISO_QUEUE_DEPTH * ISO_PACKETS];

typedef struct {
    IsoQueue queue;
    bool active[ISO_QUEUE_DEPTH];
    unsigned sequence[ISO_QUEUE_DEPTH];
    unsigned attempts, consumed, idle_calls, cancels, stops, late_callbacks;
    unsigned fail_submit, fail_completion, finish_after, stop_after, cancel_delay;
    bool synchronous, reverse, normal_drain;
    IsoStop requested_stop;
} Fake;

static unsigned index_of(Fake *fake, const IsoSlot *slot)
{
    assert(slot >= fake->queue.slots && slot < fake->queue.slots + ISO_QUEUE_DEPTH);
    return (unsigned)(slot - fake->queue.slots);
}

static void check_owned_buffers(const Fake *fake)
{
    for (unsigned i = 0; i < ISO_QUEUE_DEPTH; ++i) {
        if (fake->active[i]) {
            assert(fake->queue.slots[i].pending);
            assert(fake->queue.slots[i].data[0] == fake->sequence[i]);
            assert(fake->queue.slots[i].sizes[0] == 1);
        }
    }
}

static void complete(Fake *fake, unsigned i)
{
    assert(fake->active[i]);
    check_owned_buffers(fake);
    int result = fake->sequence[i] == fake->fail_completion ? -7004 : 192;
    if (fake->cancels && !fake->normal_drain) result = -7022;
    if (fake->stops) ++fake->late_callbacks;
    fake->active[i] = false;
    iso_queue_complete(&fake->queue.slots[i], result);
}

static int submit(void *context, IsoSlot *slot)
{
    Fake *fake = context;
    unsigned i = index_of(fake, slot);
    assert(!fake->active[i] && !fake->stops);
    assert(slot->pending && !atomic_load(&slot->completed));
    assert((uintptr_t)slot->data % 32 == 0 && (uintptr_t)slot->sizes % 32 == 0);
    for (unsigned j = 0; j < ISO_PACKETS; ++j)
        assert(slot->sizes[j] == fake->queue.capacity);
    for (unsigned j = 0; j < ISO_PACKETS * fake->queue.capacity; ++j)
        assert(slot->data[j] == 0);
    if (++fake->attempts == fake->fail_submit) return -22;
    fake->active[i] = true;
    fake->sequence[i] = fake->attempts;
    slot->data[0] = (uint8_t)fake->attempts;
    slot->sizes[0] = 1;
    if (fake->synchronous) complete(fake, i);
    return 0;
}

static bool consume(void *context, const IsoSlot *slot)
{
    Fake *fake = context;
    unsigned i = index_of(fake, slot);
    assert(!fake->active[i] && !fake->stops && !slot->pending);
    assert(atomic_load(&slot->completed));
    assert(fake->sequence[i] == fake->consumed + 1);
    assert(slot->data[0] == fake->sequence[i] && slot->sizes[0] == 1);
    ++fake->consumed;
    return fake->consumed == fake->finish_after;
}

static IsoStop poll(void *context)
{
    Fake *fake = context;
    check_owned_buffers(fake);
    if (fake->stop_after && fake->consumed >= fake->stop_after) return fake->requested_stop;
    return ISO_RUNNING;
}

static void idle(void *context)
{
    Fake *fake = context;
    assert(++fake->idle_calls < 1000);
    check_owned_buffers(fake);
    if (fake->stops && fake->cancel_delay) {
        --fake->cancel_delay;
        return;
    }
    int selected = -1;
    for (unsigned i = 0; i < ISO_QUEUE_DEPTH; ++i) {
        if (!fake->active[i]) continue;
        if (selected < 0
            || (fake->reverse ? fake->sequence[i] > fake->sequence[selected]
                              : fake->sequence[i] < fake->sequence[selected]))
            selected = (int)i;
    }
    assert(selected >= 0);
    complete(fake, (unsigned)selected);
}

static void stopping(void *context, IsoStop reason, int failure)
{
    Fake *fake = context;
    assert(++fake->stops == 1);
    assert(reason != ISO_RUNNING);
    if (reason == ISO_SUBMIT_FAILED || reason == ISO_COMPLETION_FAILED) assert(failure < 0);
}

static void cancel(void *context)
{
    Fake *fake = context;
    assert(fake->stops && ++fake->cancels == 1);
    /* IOS still owns these buffers; callbacks arrive on later idle calls. */
    check_owned_buffers(fake);
}

static void run(Fake *fake, unsigned capacity, IsoStop expected)
{
    iso_queue_init(&fake->queue, data, sizes, capacity);
    const IsoHooks hooks = {.submit = submit,
                            .consume = consume,
                            .poll = poll,
                            .idle = idle,
                            .stopping = stopping,
                            .cancel = cancel};
    assert(iso_queue_run(&fake->queue, &hooks, fake) == expected);
    assert(fake->stops == 1 && fake->queue.consumed == fake->consumed);
    for (unsigned i = 0; i < ISO_QUEUE_DEPTH; ++i)
        assert(!fake->active[i] && !fake->queue.slots[i].pending);
    unsigned failed_completion = expected == ISO_COMPLETION_FAILED ? 1 : 0;
    assert(fake->queue.submitted == fake->queue.consumed + fake->queue.drained + failed_completion);
}

int main(void)
{
    for (unsigned reverse = 0; reverse < 2; ++reverse) {
        for (unsigned count = 1; count <= 20; ++count) {
            Fake fake = {.finish_after = count, .reverse = reverse, .cancel_delay = 4};
            run(&fake, 1216, ISO_PHOTO_READY);
            assert(fake.queue.submitted == ISO_QUEUE_DEPTH + count - 1);
            assert(fake.queue.drained == ISO_QUEUE_DEPTH - 1);
        }
    }
    const IsoStop reasons[] = {ISO_CANCELLED, ISO_EXIT, ISO_TIMEOUT, ISO_PHOTO_READY,
                               ISO_RECORD_STOPPED};
    for (unsigned i = 0; i < sizeof(reasons) / sizeof(reasons[0]); ++i) {
        for (unsigned normal = 0; normal < 2; ++normal) {
            Fake fake = {.stop_after = 2,
                         .requested_stop = reasons[i],
                         .normal_drain = normal,
                         .cancel_delay = 5};
            run(&fake, 3060, reasons[i]);
            assert(fake.cancels == 1 && fake.late_callbacks == 3);
            assert(fake.queue.submitted == 5 && fake.queue.drained == 3);
        }
    }
    for (unsigned fail = 1; fail <= 8; ++fail) {
        Fake fake = {.fail_submit = fail};
        run(&fake, 1216, ISO_SUBMIT_FAILED);
        assert(fake.queue.failure == -22 && fake.queue.submitted == fail - 1);
    }
    for (unsigned fail = 1; fail <= 8; ++fail) {
        Fake fake = {.fail_completion = fail, .reverse = true};
        run(&fake, 1216, ISO_COMPLETION_FAILED);
        assert(fake.queue.failure == -7004 && fake.consumed == fail - 1);
    }
    Fake synchronous = {.finish_after = 7, .synchronous = true};
    run(&synchronous, 1216, ISO_PHOTO_READY);
    assert(synchronous.cancels == 0);
    Fake zero = {0}, oversized = {0};
    run(&zero, 0, ISO_SUBMIT_FAILED);
    run(&oversized, ISO_MAX_PAYLOAD + 1, ISO_SUBMIT_FAILED);
    puts(
        "PASS: queued ordering, ownership, wraparound, sync callbacks, partial submission, errors, stop and drain");
    return 0;
}
