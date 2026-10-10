/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "preview.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

static Preview preview;
static uint8_t storage[4096];

static void *consumer(void *unused)
{
    (void)unused;
    unsigned received = 0;
    while (!preview_photo_ready(&preview)) {
        if (!preview_take(&preview)) {
            sched_yield();
            continue;
        }
        assert(preview.length == sizeof(storage));
        for (unsigned i = 0; i < sizeof(storage); ++i)
            assert(preview.jpeg[i] == (uint8_t)received);
        ++received;
        preview_finish(&preview, true);
    }
    assert(received == 2000);
    return NULL;
}

int main(void)
{
    uint8_t input[sizeof(storage)];
    memset(input, 7, sizeof(input));
    preview_init(&preview, storage, sizeof(storage));
    assert(!preview_offer(&preview, input, 0));
    assert(!preview_offer(&preview, input, sizeof(input) + 1));
    assert(!preview_take(&preview));
    assert(preview_offer(&preview, input, sizeof(input)));
    preview_shutter(&preview);
    assert(preview_take(&preview) && !preview.save);
    input[0] = 8;
    assert(!preview_offer(&preview, input, sizeof(input)));
    assert(preview.jpeg[0] == 7);
    preview_finish(&preview, true);
    assert(!preview_photo_ready(&preview));
    assert(preview_offer(&preview, input, sizeof(input)));
    assert(preview_take(&preview) && preview.save);
    preview_finish(&preview, false);
    assert(!preview_photo_ready(&preview));
    assert(preview_offer(&preview, input, sizeof(input)));
    assert(preview_take(&preview) && preview.save);
    preview_finish(&preview, true);
    assert(preview_photo_ready(&preview));
    assert(!preview_take(&preview));
    assert(!preview_offer(&preview, input, sizeof(input)));

    preview_init(&preview, storage, sizeof(storage));
    pthread_t thread;
    assert(pthread_create(&thread, NULL, consumer, NULL) == 0);
    for (unsigned i = 0; i < 2000; ++i) {
        memset(input, (uint8_t)i, sizeof(input));
        if (i == 1999) preview_shutter(&preview);
        while (!preview_offer(&preview, input, sizeof(input)))
            sched_yield();
    }
    assert(pthread_join(thread, NULL) == 0);
    assert(preview_photo_ready(&preview));
    puts("PASS: preview ownership, busy skips, shutter timing, decode retry, threaded handoff");
}
