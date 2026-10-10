/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "refresh_control.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    RefreshControl control;
    atomic_bool release, closed;
    bool found;
} Job;

static void *complete_worker(void *context)
{
    Job *job = context;
    while (!atomic_load_explicit(&job->release, memory_order_acquire))
        sched_yield();
    atomic_store_explicit(&job->closed, true, memory_order_relaxed);
    refresh_control_complete(&job->control, job->found);
    return NULL;
}

int main(void)
{
    for (unsigned found = 0; found <= 1; ++found) {
        for (unsigned cancel_at = 0; cancel_at < 3; ++cancel_at) {
            for (unsigned repeat = 0; repeat < 20; ++repeat) {
                Job job = {.found = found};
                refresh_control_init(&job.control);
                atomic_init(&job.release, false);
                atomic_init(&job.closed, false);
                pthread_t worker;
                assert(pthread_create(&worker, NULL, complete_worker, &job) == 0);
                assert(refresh_control_poll(&job.control, false) == REFRESH_BUSY);
                if (cancel_at == 1)
                    assert(refresh_control_poll(&job.control, true) == REFRESH_CANCELLING);
                /* Repeated UI polls cannot report completion or undo cancellation. */
                for (unsigned n = 0; n < 100; ++n)
                    assert(refresh_control_poll(&job.control, false)
                           == (cancel_at == 1 ? REFRESH_CANCELLING : REFRESH_BUSY));
                atomic_store_explicit(&job.release, true, memory_order_release);
                assert(pthread_join(worker, NULL) == 0);
                RefreshResult expected = cancel_at ? REFRESH_CANCELLED
                                         : found   ? REFRESH_FOUND
                                                   : REFRESH_MISSING;
                assert(refresh_control_poll(&job.control, cancel_at == 2) == expected);
                assert(atomic_load(&job.closed));
                assert(refresh_control_poll(&job.control, false) == expected);
            }
        }
    }
    char status[80], other[80];
    refresh_status(status, sizeof(status), REFRESH_FOUND, 320, 240);
    assert(strstr(status, "320x240") && strstr(status, "MJPEG")
           && strstr(status, "A opens viewfinder"));
    refresh_status(other, sizeof(other), REFRESH_FOUND, 640, 480);
    assert(strstr(other, "640x480") && !strstr(other, "320x240"));
    refresh_status(status, sizeof(status), REFRESH_MISSING, 640, 480);
    assert(strstr(status, "No 640x480 mode") && strstr(status, "press 2"));
    assert(!strstr(status, "A opens"));
    refresh_status(status, sizeof(status), REFRESH_BUSY, 640, 480);
    assert(strstr(status, "B cancels"));
    refresh_status(status, sizeof(status), REFRESH_CANCELLING, 640, 480);
    assert(strstr(status, "please wait") && !strstr(status, "cancelled"));

    struct {
        char text[8];
        char guard;
    } bounded = {.guard = 'x'};

    refresh_status(bounded.text, sizeof(bounded.text), REFRESH_FOUND, 640, 480);
    assert(!bounded.text[7] && bounded.guard == 'x');
    puts(
        "PASS: refresh worker publication, deferred/late cancellation, repeated sessions, size-specific results, bounded messages");
}
