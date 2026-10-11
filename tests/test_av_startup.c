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
#include <stdlib.h>
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
    POSITIVE_IDLE,
    CONFIG_MISMATCH,
    UNREADABLE_CONFIG,
    SHORT_CONFIG,
    PAUSE_FAIL,
    RESTART_FAIL,
    EMPTY_AFTER_RESTART,
    PARTIAL_AFTER_RESTART,
    INVALID_AFTER_RESTART,
    STALL_AFTER_RESTART,
    SLOW_ENABLE,
    B_AFTER_RESTART,
    HOME_AFTER_RESTART,
    SCENARIOS
};

static unsigned scenario, starts, enables, captures, closes, probes, idles;
static uint64_t clock_ms, controls_ms, restarted_ms;
static unsigned pauses, restarts, pre_pause_rejected;
static bool exit_requested, silent, settling_test;
static char events[32];
static unsigned event_count;
static bool idle_unit_test, cleanup_test;
static unsigned cleanup_calls;
static unsigned idle_reads, idle_sets;
static int configuration_raw, idle_set_result;
static uint8_t configuration_byte, expected_configuration;
static bool leave_configuration_unchanged;
static uint8_t probe[UVC_PROBE_LENGTH];
static bool rejection_log_test;
static char rejection_log[4096];
static unsigned rejection_log_used;
static uint8_t audio_data[ISO_QUEUE_DEPTH * ISO_DATA_BYTES], audio_stack[32 * 1024];
static uint16_t audio_sizes[ISO_QUEUE_DEPTH * ISO_PACKETS];
static uint8_t *audio_iso_data = audio_data;
static uint16_t *audio_iso_sizes = audio_sizes;
static void *av_audio_worker(void *context);
static int LWP_CreateThread(lwp_t *thread, void *(*entry)(void *), void *context, void *stack,
                            unsigned size, unsigned priority);

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
        if (cleanup_test) {
            assert(!av_audio.opened);
            ++cleanup_calls;
            event('S');
            return 0;
        }
        if (!silent) {
            assert(!idle_reads);
            assert(!starts && !av_audio.opened && !probes && !enables);
            event('Z');
            ++idles;
            return scenario == IDLE_FAIL ? -4 : scenario == POSITIVE_IDLE ? 1 : 0;
        }
        event('I');
        ++idles;
        return scenario == IDLE_FAIL ? -4 : 0;
    }
    assert(alternate == 3);
    ++enables;
    event('V');
    if (!silent) {
        assert(starts == 1 && av_audio.opened && !av_audio.running && pauses == 1 && !restarts);
        assert(atomic_load(&av_audio.phase) == AV_VIDEO_ENABLING);
        assert(audio_stream_position(&av_audio.stream) >= 2 * AUDIO_BLOCK_BYTES);
        assert(clock_ms >= 200);
        if (scenario == SLOW_ENABLE) clock_ms += 624;
        if (scenario == FAIL_DURING_ENABLE) atomic_store(&av_audio.stream.failed, true);
    }
    return scenario == ENABLE_FAIL ? -4 : 0;
}

static s32 USB_ReadCtrlMsg(s32 handle, uint8_t type, uint8_t request, uint16_t value,
                           uint16_t index, uint16_t length, void *data)
{
    assert(handle == 42 && type == 0x80 && request == 8 && !value && !index);
    assert(length == 1 && data == probe && probe[0] == (uint8_t)(expected_configuration ^ 0xffu));
    assert(!idle_reads && idle_sets == 1);
    assert(!starts && !av_audio.opened && !probes && !enables && !silent);
    ++idle_reads;
    event('K');
    if (idle_unit_test) {
        assert(idle_set_result == 0);
        if (!leave_configuration_unchanged) probe[0] = configuration_byte;
        return configuration_raw;
    }
    assert(scenario != IDLE_FAIL && scenario != POSITIVE_IDLE);
    probe[0] = scenario == CONFIG_MISMATCH ? 2 : 1;
    return scenario == UNREADABLE_CONFIG ? -7102 : scenario == SHORT_CONFIG ? 8 : 9;
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
        assert(idles == 1 && clock_ms >= 100);
        assert(audio_stream_position(&av_audio.stream) >= AUDIO_BLOCK_BYTES);
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
    assert(!enables && !probes);
    assert(idles == 1 && idle_sets == 1);
    event('A');
    ++starts;
    if (scenario == AUDIO_OPEN_FAIL) return false;
    audio_stream_init(&av_audio.stream, MIC_RATE_UNREADABLE);
    atomic_init(&av_audio.phase, AV_AUDIO_ONLY);
    av_audio.opened = av_audio.running = true;
    av_audio.handle = 43;
    av_audio.mode = (UacMode){.interface = 3, .endpoint = 0x84, .packet_capacity = 192};
    av_audio.start = clock_ms;
    if (scenario == EARLY_EXIT) exit_requested = true;
    return true;
}

static void av_audio_stop(void)
{
    assert(av_audio.opened && av_audio.running && !pauses && !enables && !restarts);
    assert(atomic_load(&av_audio.phase) == AV_VIDEO_ENABLING);
    av_audio.running = false;
    av_audio.reason = ISO_CANCELLED;
    pre_pause_rejected = av_audio.stream.rejected;
    if (scenario == PAUSE_FAIL) atomic_store(&av_audio.stream.failed, true);
    ++pauses;
    event('Q');
}

static void *av_audio_worker(void *context)
{
    (void)context;
    assert(false);
    return NULL;
}

static int LWP_CreateThread(lwp_t *thread, void *(*entry)(void *), void *context, void *stack,
                            unsigned size, unsigned priority)
{
    assert(entry == av_audio_worker && context == &av_audio && stack == audio_stack);
    assert(size == sizeof(audio_stack) && priority == 80);
    assert(av_audio.opened && !av_audio.running && pauses == 1 && enables == 1 && !restarts);
    assert(av_audio.stream.rate == MIC_RATE_UNREADABLE
           && atomic_load(&av_audio.phase) == AV_VIDEO_ACTIVE);
    assert(av_audio.start == clock_ms && av_audio.setup_to_queue_ms == 0);
    assert(av_audio.handle == 43 && av_audio.mode.interface == 3 && av_audio.mode.endpoint == 0x84);
    assert(!atomic_load(&av_audio.stream.failed) && !audio_stream_position(&av_audio.stream));
    assert(!av_audio.stream.bytes && !av_audio.stream.packets
           && av_audio.setup_rejected == pre_pause_rejected);
    assert(!atomic_load(&av_audio.stop) && !atomic_load(&av_audio.done)
           && !atomic_load(&av_audio.last_ms));
    for (unsigned n = 0; n < ISO_QUEUE_DEPTH; ++n) {
        assert(!av_audio.queue.slots[n].pending && !av_audio.initial_completed[n]);
        assert(av_audio.queue.slots[n].data == audio_data + n * ISO_DATA_BYTES);
        assert(av_audio.queue.slots[n].sizes == audio_sizes + n * ISO_PACKETS);
    }
    ++restarts;
    event('R');
    if (scenario == RESTART_FAIL) return -1;
    *thread = 1;
    restarted_ms = clock_ms;
    return 0;
}

static void av_audio_close(void)
{
    assert(av_audio.opened && !closes);
    ++closes;
    av_audio.opened = av_audio.running = false;
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
    if (rejection_log_test) {
        assert(!av_audio.running);
        va_list args;
        va_start(args, format);
        int size = vsnprintf(rejection_log + rejection_log_used,
                             sizeof(rejection_log) - rejection_log_used, format, args);
        va_end(args);
        assert(size >= 0 && (unsigned)size < sizeof(rejection_log) - rejection_log_used);
        rejection_log_used += (unsigned)size;
    }
}

static void draw_screen(ScreenPage page, const uint8_t *rgb, unsigned width, unsigned height,
                        const char *message)
{
    assert(page == SCREEN_WAITING && !rgb && width == 320 && height == 240 && message);
    assert(!starts);
}

static uint32_t buttons(void)
{
    if (scenario == HOME || (scenario == HOME_AFTER_RESTART && restarts)) exit_requested = true;
    return scenario == B_CANCEL || (scenario == B_AFTER_RESTART && restarts) ? WPAD_BUTTON_B : 0;
}

static void usleep(unsigned microseconds)
{
    assert(microseconds == 1000 && (!enables || restarts));
    ++clock_ms;
    if (settling_test) return;
    uint8_t data[32] = {0};
    if (scenario == STALL) {
        if (clock_ms == 500) atomic_store(&av_audio.stream.failed, true);
        return;
    }
    if (scenario == STALL_AFTER_RESTART && restarts) {
        if (clock_ms - restarted_ms == 500) atomic_store(&av_audio.stream.failed, true);
        return;
    }
    if (scenario == PARTIAL_AFTER_RESTART && restarts && clock_ms - restarted_ms >= 100) return;
    if (scenario == PARTIAL && clock_ms >= 100) return;
    if (scenario == SLOW_VALID && clock_ms % 5) return;
    if (scenario == PARTIAL_AFTER_CONTROLS && probes && clock_ms - controls_ms >= 100) return;
    unsigned length =
        scenario == EMPTY || (scenario == EMPTY_AFTER_RESTART && restarts)
                || (probes && (scenario == EMPTY_AFTER_CONTROLS || scenario == CONTROL_PCM_ONLY))
            ? 0
            : sizeof(data);
    if (scenario == INVALID || (scenario == INVALID_AFTER_CONTROLS && probes)
        || (scenario == INVALID_AFTER_RESTART && restarts))
        length = 3;
    if ((scenario == FOUR_REJECTED && clock_ms <= 4) || scenario == FIFTH_REJECTED) length = 4096;
    audio_stream_packet(&av_audio.stream, data, length, 192, clock_ms);
}

static IsoStop receive_photo(s32 handle, const UvcMode *mode, const UvcAlternate *alternate)
{
    assert(handle == 42 && mode->width == 320 && alternate->endpoint == 0x81);
    assert(atomic_load(&av_audio.phase) == AV_VIDEO_ACTIVE && enables == 1);
    assert(restarts == 1 && av_audio.running);
    assert(audio_stream_position(&av_audio.stream) >= AUDIO_BLOCK_BYTES);
    assert(clock_ms - restarted_ms >= 100);
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
    clock_ms = controls_ms = restarted_ms = 0;
    pauses = restarts = pre_pause_rejected = 0;
    exit_requested = silent = idle_unit_test = cleanup_test = settling_test = false;
    cleanup_calls = 0;
    configuration_raw = 9;
    configuration_byte = expected_configuration = 1;
    leave_configuration_unchanged = false;
    idle_reads = idle_sets = 0;
    memset(events, 0, sizeof(events));
    memset(&av_audio, 0, sizeof(av_audio));
    av_recording.failed = false;
}

static void test_rejection_diagnostics(void)
{
    const unsigned slots[] = {3, 15, 0, 8, 2};
    const unsigned lengths[] = {4096, 65535, 193, 1, 4096};
    for (unsigned test = 0; test < 8; ++test) {
        reset();
        audio_stream_init(&av_audio.stream, test == 5 ? MIC_RATE_OK : MIC_RATE_UNREADABLE);
        av_audio.mode.packet_capacity = 192;
        atomic_init(&av_audio.phase, AV_VIDEO_ACTIVE);
        atomic_init(&av_audio.last_ms, 0);
        av_audio.setup_rejected = test == 4 ? 4 : 0;
        av_audio.running = true;
        rejection_log_used = 0;
        rejection_log[0] = 0;
        unsigned rounds = test == 2 ? 5 : 1;
        for (unsigned round = 0; round < rounds; ++round) {
            clock_ms = 19 + 16 * round;
            memset(audio_data, 0, sizeof(audio_data));
            for (unsigned n = 0; n < ISO_PACKETS; ++n) {
                audio_sizes[n] = 32;
                memset(audio_data + n * 192, 0x20, n + 1);
            }
            if (test == 1 || test == 4) audio_sizes[test == 4 ? 0 : 3] = 4096;
            if (test == 2) audio_sizes[slots[round]] = lengths[round];
            if (test == 3)
                for (unsigned n = 0; n < ISO_PACKETS; ++n)
                    audio_sizes[n] = 4096;
            if (test == 5 || test == 7) audio_sizes[15] = test == 5 ? 193 : 65535;
            if (test == 6) {
                audio_sizes[0] = 0;
                audio_sizes[3] = 4096;
                audio_sizes[7] = 1;
                audio_sizes[12] = 65535;
            }
            IsoSlot batch = {.data = audio_data,
                             .sizes = audio_sizes,
                             .result = test == 3   ? 0
                                       : test == 6 ? 384
                                       : test      ? 480
                                                   : 512};
            unsigned before = av_audio.stream.rejected;
            unsigned index = av_audio.stream.packets + av_audio.stream.empty + before;
            ++av_audio.queue.consumed;
            bool stop = av_audio_consume(&av_audio, &batch);
            assert(stop == ((test == 2 && round == 4) || test == 3 || test == 4 || test == 5));
            for (unsigned n = before; n < av_audio.stream.rejected; ++n) {
                const AvRejectedPacket *sample = &av_audio.rejected_packets[n];
                unsigned slot = test == 1   ? 3
                                : test == 2 ? slots[round]
                                : test == 3 ? n
                                : test == 4 ? 0
                                : test == 6 ? (unsigned[]){3, 7, 12}[n]
                                            : 15;
                assert(sample->index == index + slot && sample->slot == slot);
                assert(sample->elapsed_ms == clock_ms && sample->phase == AV_VIDEO_ACTIVE);
                assert(sample->batch_result == batch.result);
                assert(!memcmp(sample->sizes, audio_sizes, sizeof(sample->sizes)));
                assert(sample->invalid_slots == (test == 3 ? 16u : test == 6 ? 3u : 1u));
                assert(sample->plausible_bytes == (test == 3 ? 0u : test == 6 ? 384u : 480u));
                assert(sample->slot_nonzero == slot + 1);
            }
            assert(!rejection_log_used);
        }
        assert(av_audio.stream.rejected
               == (test == 0                ? 0u
                   : test == 2 || test == 3 ? 5u
                   : test == 6              ? 3u
                                            : 1u));
        assert(av_audio.setup_rejected == (test == 4 ? 4u : 0u));
        if (test == 3 || test == 4) assert(!audio_stream_position(&av_audio.stream));
        av_audio.running = false;
        rejection_log_test = true;
        av_audio_log_rejections();
        rejection_log_test = false;
        unsigned rows = 0;
        const char *next = rejection_log;
        while ((next = strstr(next, "A/V rejected batch sizes (hex):"))) {
            ++rows;
            ++next;
        }
        assert(rows == av_audio.stream.rejected);
        if (test == 2) assert(strstr(rejection_log, "index=66 slot=2 length=4096"));
        if (test == 3)
            assert(strstr(rejection_log, "batch_result=0 plausible_bytes=0 invalid_slots=16"));
        if (test == 7)
            assert(strstr(rejection_log, "slot_nonzero=16") && strstr(rejection_log, "ffff"));
    }
    puts(
        "PASS: 8 bounded rejected-batch diagnostic cases, including fifth packet, all-invalid raw0, cumulative budget, odd/oversized lengths, requested-stride reads and deferred logs");
}

int main(void)
{
    test_rejection_diagnostics();
    const UvcMode mode = {.configuration = 1,
                          .interface = 1,
                          .width = 320,
                          .height = 240,
                          .alt_count = 1,
                          .alts = {{.number = 3, .endpoint = 0x81, .capacity = 800}}};
    const IsoStop expected[] = {
        [SUCCESS] = ISO_RECORD_STOPPED,
        [IDLE_FAIL] = ISO_SUBMIT_FAILED,
        [PROBE_FAIL] = ISO_SUBMIT_FAILED,
        [AUDIO_OPEN_FAIL] = ISO_SUBMIT_FAILED,
        [EMPTY] = ISO_TIMEOUT,
        [PARTIAL] = ISO_TIMEOUT,
        [INVALID] = ISO_COMPLETION_FAILED,
        [STALL] = ISO_COMPLETION_FAILED,
        [B_CANCEL] = ISO_CANCELLED,
        [HOME] = ISO_EXIT,
        [ENABLE_FAIL] = ISO_SUBMIT_FAILED,
        [FAIL_DURING_ENABLE] = ISO_COMPLETION_FAILED,
        [FAIL_AFTER_ENABLE] = ISO_COMPLETION_FAILED,
        [EARLY_EXIT] = ISO_EXIT,
        [FOUR_REJECTED] = ISO_RECORD_STOPPED,
        [FIFTH_REJECTED] = ISO_COMPLETION_FAILED,
        [SLOW_VALID] = ISO_RECORD_STOPPED,
        [FAIL_DURING_CONTROLS] = ISO_COMPLETION_FAILED,
        [EMPTY_AFTER_CONTROLS] = ISO_TIMEOUT,
        [PARTIAL_AFTER_CONTROLS] = ISO_TIMEOUT,
        [INVALID_AFTER_CONTROLS] = ISO_COMPLETION_FAILED,
        [CONTROL_PCM_ONLY] = ISO_TIMEOUT,
        [POSITIVE_IDLE] = ISO_SUBMIT_FAILED,
        [CONFIG_MISMATCH] = ISO_SUBMIT_FAILED,
        [UNREADABLE_CONFIG] = ISO_SUBMIT_FAILED,
        [SHORT_CONFIG] = ISO_SUBMIT_FAILED,
        [PAUSE_FAIL] = ISO_COMPLETION_FAILED,
        [RESTART_FAIL] = ISO_SUBMIT_FAILED,
        [EMPTY_AFTER_RESTART] = ISO_TIMEOUT,
        [PARTIAL_AFTER_RESTART] = ISO_TIMEOUT,
        [INVALID_AFTER_RESTART] = ISO_COMPLETION_FAILED,
        [STALL_AFTER_RESTART] = ISO_COMPLETION_FAILED,
        [SLOW_ENABLE] = ISO_RECORD_STOPPED,
        [B_AFTER_RESTART] = ISO_CANCELLED,
        [HOME_AFTER_RESTART] = ISO_EXIT,
    };
    _Static_assert(sizeof(expected) / sizeof(expected[0]) == SCENARIOS, "scenario results");
    for (scenario = 0; scenario < SCENARIOS; ++scenario) {
        reset();
        UvcAlternate alternate;
        bool video_changed = true;
        assert(receive_av(42, &mode, &alternate, &video_changed) == expected[scenario]);
        assert(av_recording.failed
               == (expected[scenario] == ISO_SUBMIT_FAILED
                   || expected[scenario] == ISO_COMPLETION_FAILED
                   || expected[scenario] == ISO_TIMEOUT));
        assert(!av_audio.opened && idle_sets == 1);
        assert(idle_reads == (scenario == IDLE_FAIL || scenario == POSITIVE_IDLE ? 0u : 1u));
        if (scenario == IDLE_FAIL || scenario == POSITIVE_IDLE || scenario == CONFIG_MISMATCH
            || scenario == UNREADABLE_CONFIG || scenario == SHORT_CONFIG) {
            assert(!starts && !enables && !captures && !closes);
        } else if (scenario == AUDIO_OPEN_FAIL) {
            assert(starts == 1 && !enables && !captures && !closes);
        } else {
            assert(starts == 1 && closes == 1);
        }
        bool activated = scenario == SUCCESS || scenario == ENABLE_FAIL
                         || scenario == FAIL_DURING_ENABLE || scenario == FAIL_AFTER_ENABLE
                         || scenario == FOUR_REJECTED || scenario == SLOW_VALID
                         || scenario >= RESTART_FAIL;
        assert(enables == activated);
        bool recorded = scenario == SUCCESS || scenario == FAIL_AFTER_ENABLE
                        || scenario == FOUR_REJECTED || scenario == SLOW_VALID
                        || scenario == SLOW_ENABLE;
        assert(captures == recorded);
        assert(pauses == (activated || scenario == PAUSE_FAIL));
        assert(restarts
               == (activated && scenario != ENABLE_FAIL && scenario != FAIL_DURING_ENABLE));
        if (captures) assert(!strcmp(events, "ZKAPQVRCX"));
        assert(video_changed);
        cleanup_test = true;
        stop_video_stream(42, video_changed);
        assert(cleanup_calls == video_changed);
        assert(idle_sets == 2);
        if (scenario == EMPTY || scenario == PARTIAL) assert(clock_ms == 1000);
        if (scenario == INVALID || scenario == FIFTH_REJECTED)
            assert(av_audio.stream.rejected == 5 && !audio_stream_position(&av_audio.stream));
        if (scenario == FOUR_REJECTED)
            assert(pre_pause_rejected == 4 && !av_audio.stream.rejected && clock_ms == 304);
        if (scenario == SLOW_VALID) assert(clock_ms == 1500);
        if (scenario == SLOW_ENABLE) assert(clock_ms == 924);
        if (scenario == EMPTY_AFTER_RESTART || scenario == PARTIAL_AFTER_RESTART)
            assert(clock_ms == 1200);
        if (scenario == STALL_AFTER_RESTART) assert(clock_ms == 700);
        if (scenario == EMPTY_AFTER_CONTROLS || scenario == PARTIAL_AFTER_CONTROLS
            || scenario == CONTROL_PCM_ONLY)
            assert(clock_ms == 1100);
        bool negotiated = activated || scenario == PAUSE_FAIL || scenario == PROBE_FAIL
                          || (scenario >= FAIL_DURING_CONTROLS && scenario <= CONTROL_PCM_ONLY);
        assert(probes == negotiated);
        if (scenario == INVALID_AFTER_CONTROLS)
            assert(av_audio.stream.rejected == 5
                   && audio_stream_position(&av_audio.stream) == AUDIO_BLOCK_BYTES);
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
        int raw, set_result;
        uint8_t value, expected_configuration;
        bool leave_unchanged, expected;
    } idle_cases[] = {
        {9, 0, 1, 1, false, true},      {1, 0, 1, 1, false, false},
        {-7102, 0, 1, 1, false, false}, {-7022, 0, 1, 1, false, false},
        {0, 0, 1, 1, false, false},     {8, 0, 1, 1, false, false},
        {10, 0, 1, 1, false, false},    {9, 0, 0, 1, false, false},
        {9, 0, 2, 1, false, false},     {9, 0, 220, 1, false, false},
        {9, 0, 255, 1, false, false},   {9, -4, 1, 1, false, false},
        {9, -7102, 1, 1, false, false}, {9, 1, 1, 1, false, false},
        {9, 8, 1, 1, false, false},     {9, 0, 2, 2, false, true},
        {9, 0, 1, 1, true, false},      {9, 0, 255, 255, false, true},
        {9, 0, 255, 255, true, false},
    };

    for (unsigned n = 0; n < sizeof(idle_cases) / sizeof(idle_cases[0]); ++n) {
        reset();
        idle_unit_test = true;
        configuration_raw = idle_cases[n].raw;
        configuration_byte = idle_cases[n].value;
        idle_set_result = idle_cases[n].set_result;
        leave_configuration_unchanged = idle_cases[n].leave_unchanged;
        UvcMode checked_mode = mode;
        expected_configuration = checked_mode.configuration = idle_cases[n].expected_configuration;
        checked_mode.interface = 7;
        bool video_changed = false;
        assert(av_video_idle(42, &checked_mode, &video_changed) == idle_cases[n].expected);
        assert(video_changed && idle_sets == 1);
        assert(idle_reads == (idle_set_result == 0 ? 1u : 0u));
        assert(!starts && !probes && !enables && !captures && !closes);
        idle_unit_test = false;
        cleanup_test = true;
        stop_video_stream(42, video_changed);
        assert(idle_sets == 2 && cleanup_calls == 1);
    }
    for (unsigned n = 0; n < 3; ++n) {
        reset();
        idle_unit_test = true;
        UvcMode invalid = mode;
        if (n == 0) invalid.alt_count = WC04_MAX_ALTS + 1;
        if (n == 1) invalid.alt_count = 0;
        if (n == 2) invalid.configuration = 0;
        bool video_changed = false;
        assert(!av_video_idle(42, &invalid, &video_changed));
        assert(!idle_sets && !idle_reads && !video_changed);
        idle_unit_test = false;
        cleanup_test = true;
        stop_video_stream(42, video_changed);
        assert(!idle_sets && !cleanup_calls);
    }
    puts(
        "PASS: actual startup: 35 readiness/preflight/control/cleanup cases, 4 video-without-audio paths, 4 bounded settling/cancellation cases, 22 checked IOS-idle/configuration cases");
}
