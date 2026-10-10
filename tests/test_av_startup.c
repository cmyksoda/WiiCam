/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */
#include "audio_stream.h"
#include "iso_queue.h"
#include "screen.h"
#include "uvc.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef int32_t s32;
typedef unsigned lwp_t;

typedef struct {
    s32 handle;
    unsigned interface;
} ProbeContext;

#define WPAD_BUTTON_B 2u

enum {
    SUCCESS,
    IDLE_FAIL,
    PROBE_FAIL,
    AUDIO_OPEN_FAIL,
    EMPTY,
    PARTIAL,
    INVALID,
    STALL,
    B_CANCEL,
    HOME,
    ENABLE_FAIL,
    FAIL_DURING_ENABLE,
    FAIL_AFTER_ENABLE,
    EARLY_EXIT,
    FOUR_REJECTED,
    FIFTH_REJECTED,
    SLOW_VALID,
    SCENARIOS
};

static unsigned scenario, starts, enables, captures, closes;
static uint64_t clock_ms;
static bool exit_requested, silent;
static char events[32];
static unsigned event_count;
static uint8_t probe[UVC_PROBE_LENGTH];

static struct {
    bool failed;
} av_recording;

static s32 WC04_SetAlternate(s32 handle, uint8_t alternate);
static int probe_transfer(void *context, uint8_t request, uint16_t selector, uint8_t *data);
static uint64_t gettime(void);
static uint64_t ticks_to_millisecs(uint64_t ticks);
static void report(const char *format, ...);
static void draw_screen(ScreenPage page, const uint8_t *rgb, unsigned width, unsigned height,
                        const char *message);
static uint32_t buttons(void);
static void usleep(unsigned microseconds);
static IsoStop receive_photo(s32 handle, const UvcMode *mode, const UvcAlternate *alternate);

#include "av_startup_under_test.h"

static void event(char code)
{
    assert(event_count + 1 < sizeof(events));
    events[event_count++] = code;
}

static s32 WC04_SetAlternate(s32 handle, uint8_t alternate)
{
    assert(handle == 42);
    if (!alternate) {
        event('I');
        return scenario == IDLE_FAIL ? -4 : 0;
    }
    assert(alternate == 3);
    ++enables;
    event('V');
    if (!silent) {
        assert(starts == 1 && av_audio.opened);
        assert(atomic_load(&av_audio.phase) == AV_VIDEO_ENABLING);
        assert(audio_stream_position(&av_audio.stream) >= AUDIO_BLOCK_BYTES);
        assert(clock_ms >= 100);
        if (scenario == FAIL_DURING_ENABLE) atomic_store(&av_audio.stream.failed, true);
    }
    return scenario == ENABLE_FAIL ? -4 : 0;
}

static int probe_transfer(void *context, uint8_t request, uint16_t selector, uint8_t *data)
{
    (void)context;
    (void)request;
    (void)selector;
    (void)data;
    assert(false);
    return -1;
}

bool uvc_negotiate(const UvcMode *mode, UvcAlternate *alternate, uint8_t data[UVC_PROBE_LENGTH],
                   UvcControlTransfer transfer, void *context)
{
    ProbeContext *camera = context;
    assert(camera->handle == 42 && camera->interface == 1);
    assert(mode->interface == 1 && data == probe && transfer == probe_transfer);
    assert(!starts && !enables);
    event('P');
    *alternate = (UvcAlternate){.number = 3, .endpoint = 0x81, .capacity = 800};
    return scenario != PROBE_FAIL;
}

static bool av_audio_start(s32 handle, const UvcMode *mode)
{
    assert(handle == 42 && mode->width == 320 && mode->height == 240);
    assert(!enables);
    event('A');
    ++starts;
    if (scenario == AUDIO_OPEN_FAIL) return false;
    audio_stream_init(&av_audio.stream, MIC_RATE_UNREADABLE);
    atomic_init(&av_audio.phase, AV_AUDIO_ONLY);
    av_audio.opened = true;
    av_audio.start = clock_ms;
    if (scenario == EARLY_EXIT) exit_requested = true;
    return true;
}

static void av_audio_close(void)
{
    assert(av_audio.opened && !closes);
    ++closes;
    av_audio.opened = false;
    event('X');
}

static uint64_t gettime(void)
{
    return clock_ms;
}

static uint64_t ticks_to_millisecs(uint64_t ticks)
{
    return ticks;
}

static void report(const char *format, ...)
{
    (void)format;
}

static void draw_screen(ScreenPage page, const uint8_t *rgb, unsigned width, unsigned height,
                        const char *message)
{
    assert(page == SCREEN_WAITING && !rgb && width == 320 && height == 240 && message);
}

static uint32_t buttons(void)
{
    if (scenario == HOME) exit_requested = true;
    return scenario == B_CANCEL ? WPAD_BUTTON_B : 0;
}

static void usleep(unsigned microseconds)
{
    assert(microseconds == 1000 && !enables);
    ++clock_ms;
    uint8_t data[32] = {0};
    if (scenario == STALL) {
        if (clock_ms == 500) atomic_store(&av_audio.stream.failed, true);
        return;
    }
    if (scenario == PARTIAL && clock_ms >= 100) return;
    if (scenario == SLOW_VALID && clock_ms % 5) return;
    unsigned length = scenario == EMPTY ? 0 : sizeof(data);
    if (scenario == INVALID) length = 3;
    if ((scenario == FOUR_REJECTED && clock_ms <= 4) || scenario == FIFTH_REJECTED) length = 4096;
    audio_stream_packet(&av_audio.stream, data, length, 192, clock_ms);
}

static IsoStop receive_photo(s32 handle, const UvcMode *mode, const UvcAlternate *alternate)
{
    assert(handle == 42 && mode->width == 320 && alternate->endpoint == 0x81);
    assert(atomic_load(&av_audio.phase) == AV_VIDEO_ACTIVE && enables == 1);
    assert(atomic_load(&av_audio.stream.consumed) == audio_stream_position(&av_audio.stream));
    event('C');
    ++captures;
    if (scenario == FAIL_AFTER_ENABLE) {
        atomic_store(&av_audio.stream.failed, true);
        return ISO_COMPLETION_FAILED;
    }
    return ISO_RECORD_STOPPED;
}

static void reset(void)
{
    starts = enables = captures = closes = event_count = 0;
    clock_ms = 0;
    exit_requested = silent = false;
    memset(events, 0, sizeof(events));
    memset(&av_audio, 0, sizeof(av_audio));
    av_recording.failed = false;
}

int main(void)
{
    const UvcMode mode = {.interface = 1, .width = 320, .height = 240};
    const IsoStop expected[] = {
        ISO_RECORD_STOPPED,    ISO_SUBMIT_FAILED, ISO_SUBMIT_FAILED,     ISO_SUBMIT_FAILED,
        ISO_TIMEOUT,           ISO_TIMEOUT,       ISO_COMPLETION_FAILED, ISO_COMPLETION_FAILED,
        ISO_CANCELLED,         ISO_EXIT,          ISO_SUBMIT_FAILED,     ISO_COMPLETION_FAILED,
        ISO_COMPLETION_FAILED, ISO_EXIT,          ISO_RECORD_STOPPED,    ISO_COMPLETION_FAILED,
        ISO_RECORD_STOPPED};
    for (scenario = 0; scenario < SCENARIOS; ++scenario) {
        reset();
        UvcAlternate alternate;
        assert(receive_av(42, &mode, &alternate) == expected[scenario]);
        assert(av_recording.failed
               == (expected[scenario] == ISO_SUBMIT_FAILED
                   || expected[scenario] == ISO_COMPLETION_FAILED
                   || expected[scenario] == ISO_TIMEOUT));
        assert(!av_audio.opened);
        if (scenario <= AUDIO_OPEN_FAIL && scenario != SUCCESS) {
            assert(!enables && !captures && !closes);
        } else {
            assert(starts == 1 && closes == 1);
        }
        bool activated = scenario == SUCCESS || scenario == ENABLE_FAIL
                         || scenario == FAIL_DURING_ENABLE || scenario == FAIL_AFTER_ENABLE
                         || scenario == FOUR_REJECTED || scenario == SLOW_VALID;
        assert(enables == activated);
        assert(captures
               == (activated && scenario != ENABLE_FAIL && scenario != FAIL_DURING_ENABLE));
        if (captures) assert(!strcmp(events, "IPAVCX"));
        if (scenario == EMPTY || scenario == PARTIAL) assert(clock_ms == 1000);
        if (scenario == INVALID || scenario == FIFTH_REJECTED)
            assert(av_audio.stream.rejected == 5 && !audio_stream_position(&av_audio.stream));
        if (scenario == FOUR_REJECTED) assert(av_audio.stream.rejected == 4 && clock_ms == 104);
        if (scenario == SLOW_VALID) assert(clock_ms == 500);
    }
    for (unsigned n = 0; n < 4; ++n) {
        scenario = n == 3 ? ENABLE_FAIL : n;
        reset();
        silent = true;
        UvcAlternate alternate;
        assert(negotiate(42, &mode, &alternate) == (scenario == SUCCESS));
        assert(!starts && !closes && !captures);
        assert(!strcmp(events, scenario == IDLE_FAIL    ? "I"
                               : scenario == PROBE_FAIL ? "IP"
                                                        : "IPV"));
    }
    puts(
        "PASS: actual startup: 17 audio readiness/order/failure/cancel scenarios and 4 unchanged silent negotiation paths");
}
