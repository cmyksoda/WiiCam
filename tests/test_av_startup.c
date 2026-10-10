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
    FAIL_DURING_CONTROLS,
    EMPTY_AFTER_CONTROLS,
    PARTIAL_AFTER_CONTROLS,
    INVALID_AFTER_CONTROLS,
    CONTROL_PCM_ONLY,
    FAIL_DURING_IDLE,
    EMPTY_AFTER_IDLE,
    PARTIAL_AFTER_IDLE,
    INVALID_AFTER_IDLE,
    IDLE_PCM_ONLY,
    B_AFTER_IDLE,
    HOME_AFTER_IDLE,
    SCENARIOS
};

static unsigned scenario, starts, enables, captures, closes, probes, idles;
static uint64_t clock_ms, controls_ms, idle_ms;
static bool exit_requested, silent, settling_test;
static char events[32];
static unsigned event_count;
static bool idle_unit_test;
static unsigned idle_reads, idle_sets, expected_interface = 1;
static int idle_raw[2], idle_set_result;
static uint8_t idle_alternate[2];
static uint8_t probe[UVC_PROBE_LENGTH];

static struct {
    bool failed;
} av_recording;

static s32 WC04_SetAlternate(s32 handle, uint8_t alternate);
static s32 USB_ReadCtrlMsg(s32 handle, uint8_t type, uint8_t request, uint16_t value,
                           uint16_t index, uint16_t length, void *data);
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
        ++idle_sets;
        if (idle_unit_test) return idle_set_result;
        /* A/V must not reset video when GET_INTERFACE already reports zero. */
        assert(silent);
        event('I');
        ++idles;
        return scenario == IDLE_FAIL ? -4 : 0;
    }
    assert(alternate == 3);
    ++enables;
    event('V');
    if (!silent) {
        assert(starts == 1 && av_audio.opened);
        assert(atomic_load(&av_audio.phase) == AV_VIDEO_ENABLING);
        assert(audio_stream_position(&av_audio.stream) >= 3 * AUDIO_BLOCK_BYTES);
        assert(clock_ms >= 300);
        if (scenario == FAIL_DURING_ENABLE) atomic_store(&av_audio.stream.failed, true);
    }
    return scenario == ENABLE_FAIL ? -4 : 0;
}

static s32 USB_ReadCtrlMsg(s32 handle, uint8_t type, uint8_t request, uint16_t value,
                           uint16_t index, uint16_t length, void *data)
{
    assert(handle == 42 && type == 0x81 && request == 10 && !value);
    assert(index == expected_interface && length == 1 && data == probe);
    if (idle_unit_test) {
        assert(idle_reads < 2);
        probe[0] = idle_alternate[idle_reads];
        return idle_raw[idle_reads++];
    }
    ++idle_reads;
    ++idles;
    idle_ms = clock_ms;
    event('I');
    assert(starts == 1 && av_audio.opened && !probes && !enables && !silent);
    assert(atomic_load(&av_audio.phase) == AV_VIDEO_IDLING);
    assert(audio_stream_position(&av_audio.stream) >= AUDIO_BLOCK_BYTES);
    if (scenario == FAIL_DURING_IDLE) atomic_store(&av_audio.stream.failed, true);
    if (scenario == IDLE_PCM_ONLY) {
        uint8_t samples[32] = {0};
        for (unsigned n = 0; n < 100; ++n)
            assert(audio_stream_packet(&av_audio.stream, samples, sizeof(samples), 192, clock_ms));
    }
    probe[0] = 0;
    return scenario == IDLE_FAIL ? -7102 : 9;
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
    assert(!enables);
    ++probes;
    controls_ms = clock_ms;
    if (silent)
        assert(!starts);
    else {
        assert(starts == 1 && av_audio.opened);
        assert(atomic_load(&av_audio.phase) == AV_VIDEO_PREPARING);
        assert(idles == 1 && clock_ms >= 200);
        assert(audio_stream_position(&av_audio.stream) >= 2 * AUDIO_BLOCK_BYTES);
        if (scenario == FAIL_DURING_CONTROLS) atomic_store(&av_audio.stream.failed, true);
        if (scenario == CONTROL_PCM_ONLY) {
            uint8_t data[32] = {0};
            for (unsigned n = 0; n < 100; ++n)
                assert(audio_stream_packet(&av_audio.stream, data, sizeof(data), 192, clock_ms));
        }
    }
    event('P');
    *alternate = (UvcAlternate){.number = 3, .endpoint = 0x81, .capacity = 800};
    return scenario != PROBE_FAIL;
}

static bool av_audio_start(s32 handle, const UvcMode *mode)
{
    assert(handle == 42 && mode->width == 320 && mode->height == 240);
    assert(!enables && !idles && !probes);
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
    assert(!starts);
}

static uint32_t buttons(void)
{
    if (scenario == HOME || (scenario == HOME_AFTER_IDLE && idles)) exit_requested = true;
    return scenario == B_CANCEL || (scenario == B_AFTER_IDLE && idles) ? WPAD_BUTTON_B : 0;
}

static void usleep(unsigned microseconds)
{
    assert(microseconds == 1000 && !enables);
    ++clock_ms;
    if (settling_test) return;
    uint8_t data[32] = {0};
    if (scenario == STALL) {
        if (clock_ms == 500) atomic_store(&av_audio.stream.failed, true);
        return;
    }
    if (scenario == PARTIAL && clock_ms >= 100) return;
    if (scenario == SLOW_VALID && clock_ms % 5) return;
    if (scenario == PARTIAL_AFTER_IDLE && idles && clock_ms - idle_ms >= 100) return;
    if (scenario == PARTIAL_AFTER_CONTROLS && probes && clock_ms - controls_ms >= 100) return;
    unsigned length =
        scenario == EMPTY || (idles && (scenario == EMPTY_AFTER_IDLE || scenario == IDLE_PCM_ONLY))
                || (probes && (scenario == EMPTY_AFTER_CONTROLS || scenario == CONTROL_PCM_ONLY))
            ? 0
            : sizeof(data);
    if (scenario == INVALID || (scenario == INVALID_AFTER_CONTROLS && probes)
        || (scenario == INVALID_AFTER_IDLE && idles))
        length = 3;
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
    starts = enables = captures = closes = event_count = probes = idles = 0;
    clock_ms = controls_ms = idle_ms = 0;
    exit_requested = silent = idle_unit_test = false;
    idle_reads = idle_sets = 0;
    expected_interface = 1;
    memset(events, 0, sizeof(events));
    memset(&av_audio, 0, sizeof(av_audio));
    av_recording.failed = false;
}

int main(void)
{
    const UvcMode mode = {.interface = 1, .width = 320, .height = 240};
    const IsoStop expected[] = {ISO_RECORD_STOPPED,
                                ISO_SUBMIT_FAILED,
                                ISO_SUBMIT_FAILED,
                                ISO_SUBMIT_FAILED,
                                ISO_TIMEOUT,
                                ISO_TIMEOUT,
                                ISO_COMPLETION_FAILED,
                                ISO_COMPLETION_FAILED,
                                ISO_CANCELLED,
                                ISO_EXIT,
                                ISO_SUBMIT_FAILED,
                                ISO_COMPLETION_FAILED,
                                ISO_COMPLETION_FAILED,
                                ISO_EXIT,
                                ISO_RECORD_STOPPED,
                                ISO_COMPLETION_FAILED,
                                ISO_RECORD_STOPPED,
                                ISO_COMPLETION_FAILED,
                                ISO_TIMEOUT,
                                ISO_TIMEOUT,
                                ISO_COMPLETION_FAILED,
                                ISO_TIMEOUT,
                                ISO_COMPLETION_FAILED,
                                ISO_TIMEOUT,
                                ISO_TIMEOUT,
                                ISO_COMPLETION_FAILED,
                                ISO_TIMEOUT,
                                ISO_CANCELLED,
                                ISO_EXIT};
    for (scenario = 0; scenario < SCENARIOS; ++scenario) {
        reset();
        UvcAlternate alternate;
        assert(receive_av(42, &mode, &alternate) == expected[scenario]);
        assert(av_recording.failed
               == (expected[scenario] == ISO_SUBMIT_FAILED
                   || expected[scenario] == ISO_COMPLETION_FAILED
                   || expected[scenario] == ISO_TIMEOUT));
        assert(!av_audio.opened && !idle_sets);
        if (scenario == AUDIO_OPEN_FAIL) {
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
        if (captures) assert(!strcmp(events, "AIPVCX"));
        if (scenario == EMPTY || scenario == PARTIAL) assert(clock_ms == 1000);
        if (scenario == INVALID || scenario == FIFTH_REJECTED)
            assert(av_audio.stream.rejected == 5 && !audio_stream_position(&av_audio.stream));
        if (scenario == FOUR_REJECTED) assert(av_audio.stream.rejected == 4 && clock_ms == 304);
        if (scenario == SLOW_VALID) assert(clock_ms == 1500);
        if (scenario == EMPTY_AFTER_CONTROLS || scenario == PARTIAL_AFTER_CONTROLS
            || scenario == CONTROL_PCM_ONLY)
            assert(clock_ms == 1200);
        if (scenario == EMPTY_AFTER_IDLE || scenario == PARTIAL_AFTER_IDLE
            || scenario == IDLE_PCM_ONLY)
            assert(clock_ms == 1100 && !probes && !enables);
        bool negotiated = activated || scenario == PROBE_FAIL
                          || (scenario >= FAIL_DURING_CONTROLS && scenario <= CONTROL_PCM_ONLY);
        assert(probes == negotiated);
        if (scenario == INVALID_AFTER_CONTROLS)
            assert(av_audio.stream.rejected == 5
                   && audio_stream_position(&av_audio.stream) == 2 * AUDIO_BLOCK_BYTES);
        if (scenario == INVALID_AFTER_IDLE)
            assert(av_audio.stream.rejected == 5
                   && audio_stream_position(&av_audio.stream) == AUDIO_BLOCK_BYTES);
        if (scenario == IDLE_FAIL || scenario == FAIL_DURING_IDLE || scenario >= EMPTY_AFTER_IDLE)
            assert(idles == 1 && !probes && !enables && closes == 1);
        if (scenario == IDLE_PCM_ONLY)
            assert(audio_stream_position(&av_audio.stream) == 2 * AUDIO_BLOCK_BYTES);
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
    const unsigned settle_cases[] = {SUCCESS, B_CANCEL, HOME, EARLY_EXIT};
    const unsigned settle_times[] = {50, 10, 10, 0};
    for (unsigned n = 0; n < 4; ++n) {
        reset();
        settling_test = true;
        scenario = settle_cases[n];
        exit_requested = scenario == EARLY_EXIT;
        assert(av_audio_wait_rate(clock_ms) == (scenario == SUCCESS));
        assert(clock_ms == settle_times[n]);
        assert(!starts && !probes && !enables && !captures && !closes);
    }

    const struct {
        int first_raw, second_raw, set_result;
        uint8_t first_alternate, second_alternate;
        bool expected;
        unsigned reads, sets;
    } idle_cases[] = {
        {9, 9, 0, 0, 0, true, 1, 0},      {1, 9, 0, 0, 0, false, 1, 0},
        {-7102, 9, 0, 0, 0, false, 1, 0}, {0, 9, 0, 0, 0, false, 1, 0},
        {8, 9, 0, 0, 0, false, 1, 0},     {10, 9, 0, 0, 0, false, 1, 0},
        {9, 9, 0, 3, 0, true, 2, 1},      {9, 9, 0, 3, 3, false, 2, 1},
        {9, 8, 0, 3, 0, false, 2, 1},     {9, 9, -4, 3, 0, false, 1, 1},
        {9, -7102, 0, 3, 0, false, 2, 1}, {9, 9, 0, 255, 0, true, 2, 1},
    };

    for (unsigned n = 0; n < sizeof(idle_cases) / sizeof(idle_cases[0]); ++n) {
        reset();
        idle_unit_test = true;
        idle_raw[0] = idle_cases[n].first_raw;
        idle_raw[1] = idle_cases[n].second_raw;
        idle_alternate[0] = idle_cases[n].first_alternate;
        idle_alternate[1] = idle_cases[n].second_alternate;
        idle_set_result = idle_cases[n].set_result;
        UvcMode checked_mode = mode;
        if (n == 11) expected_interface = checked_mode.interface = 7;
        assert(av_video_idle(42, &checked_mode) == idle_cases[n].expected);
        assert(idle_reads == idle_cases[n].reads && idle_sets == idle_cases[n].sets);
        assert(!starts && !probes && !enables && !captures && !closes);
    }
    puts(
        "PASS: actual startup: 29 readiness/idle/control/failure cases, 4 silent paths, 4 bounded settling/cancellation cases, 12 checked video-idle cases");
}
