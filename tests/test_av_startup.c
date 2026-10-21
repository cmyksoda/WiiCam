/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */
#include "audio_stream.h"
#include "iso_queue.h"
#include "screen.h"
#include "uvc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t s32;
typedef uint8_t u8;
typedef unsigned lwp_t;

typedef struct {
    s32 handle;
    unsigned interface;
} ProbeContext;

#define WPAD_BUTTON_B 2u

enum {
    SUCCESS,
    POISONED,
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
    PROBE_SET_SHORT,
    PROBE_GET_SHORT,
    PROBE_WRONG_FORMAT,
    PROBE_WRONG_FRAME,
    PROBE_WRONG_INTERVAL,
    UNPARKED,
    PROBE_OVERSIZE_FRAME,
    PROBE_BAD_PAYLOAD,
    COMMIT_FAIL,
    PAUSE_FAIL,
    RESTART_FAIL,
    EMPTY_AFTER_RESTART,
    PARTIAL_AFTER_RESTART,
    INVALID_AFTER_RESTART,
    STALL_AFTER_RESTART,
    SLOW_ENABLE,
    B_AFTER_RESTART,
    HOME_AFTER_RESTART,
    FAIL_DURING_PROBE,
    EMPTY_AFTER_PROBE,
    PARTIAL_AFTER_PROBE,
    INVALID_AFTER_PROBE,
    STALL_AFTER_PROBE,
    CONCEAL_AFTER_PROBE,
    B_AFTER_PROBE,
    HOME_AFTER_PROBE,
    PARTIAL_CONCEALED_AFTER_PROBE,
    PARKED,
    PARKED_SILENT,
    RESTART_REQUIRED,
    REPLUGGED,
    SCENARIOS
};

static unsigned scenario, starts, enables, captures, closes, probes, idles;
static uint64_t clock_ms, restarted_ms, probed_ms;
static unsigned pauses, restarts, pre_pause_rejected;
static bool exit_requested, silent, settling_test, drain_test;
static unsigned drain_complete_at, cancel_calls;
static char events[32];
static unsigned event_count;
static bool idle_unit_test, cleanup_test;
static unsigned cleanup_calls;
static unsigned idle_sets, probe_calls;
static int idle_set_result;
static bool committed, parked_run, fallback_run, alternate_log_test;
static char alternate_log[16];
static unsigned alternate_log_count;
static int alternate_result;
static uint8_t probe[UVC_PROBE_LENGTH];
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
static ScreenInfo screen_info;

static s32 WC04_SetAlternate(s32 handle, uint8_t alternate);
static s32 USB_CloseDevice(s32 *handle);
static int probe_transfer(void *context, uint8_t request, uint16_t selector, uint8_t *data);
static uint64_t gettime(void);
static uint64_t ticks_to_millisecs(uint64_t ticks);
static void report(const char *format, ...);
static void draw_screen(ScreenPage page, const uint8_t *rgb, unsigned width, unsigned height,
                        const char *message);
static uint32_t buttons(void);
static void usleep(unsigned microseconds);
static s32 WC04_CancelIso(s32 handle, uint8_t endpoint);
static IsoStop receive_photo(s32 handle, const UvcMode *mode, const UvcAlternate *alternate);

#include "av_startup_under_test.h"

static void event(char code)
{
    assert(event_count + 1 < sizeof(events));
    events[event_count++] = code;
}

static s32 WC04_SetAlternate(s32 handle, uint8_t alternate)
{
    if (alternate_log_test) {
        assert(alternate_log_count + 2 < sizeof(alternate_log));
        alternate_log[alternate_log_count++] = handle == 42 ? 'v' : 'm';
        alternate_log[alternate_log_count++] = (char)('0' + alternate);
        return alternate_result;
    }
    if (handle == 44) {
        assert(scenario == REPLUGGED && !alternate && !starts && !event_count);
        event('L');
        return 0;
    }
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
        if (!silent && !fallback_run) {
            assert(!starts && !av_audio.opened && !probes && !enables);
            event('Z');
            ++idles;
            return 0;
        }
        event('I');
        ++idles;
        return scenario == POISONED ? -4 : 0;
    }
    assert(alternate == 3);
    ++enables;
    event('V');
    if (!silent && !fallback_run) {
        assert(starts == 1 && av_audio.opened && !av_audio.running && pauses == 1 && !restarts);
        assert(committed);
        assert(audio_stream_position(&av_audio.stream) >= AUDIO_BLOCK_BYTES);
        assert(clock_ms >= 100);
        if (scenario == SLOW_ENABLE) clock_ms += 624;
        if (scenario == FAIL_DURING_ENABLE) atomic_store(&av_audio.stream.failed, true);
    }
    return scenario == ENABLE_FAIL ? -4 : 0;
}

static s32 USB_CloseDevice(s32 *handle)
{
    assert(alternate_log_test || (scenario == REPLUGGED && *handle == 44));
    *handle = -1;
    return 0;
}

static int probe_transfer(void *context, uint8_t request, uint16_t selector, uint8_t *data)
{
    ProbeContext *camera = context;
    assert(camera->handle == 42 && camera->interface == 1 && data == probe);
    assert(!enables);
    if (silent || fallback_run)
        assert(starts == fallback_run && !av_audio.opened && idles == 1 && idle_sets == 1);
    else {
        assert(starts == 1 && av_audio.opened && av_audio.running);
        assert(!pauses && !restarts);
        assert(audio_stream_received(&av_audio.stream) >= AUDIO_BLOCK_BYTES);
    }
    if (!probe_calls++) {
        ++probes;
        event('P');
    }
    if (scenario == PROBE_FAIL) return -7102;
    assert(!parked_run);
    if (request == UVC_GET_MAX) {
        assert(probe_calls == 1 && selector == UVC_VS_PROBE);
        return UVC_PROBE_LENGTH;
    }
    if (request == UVC_SET_CUR && selector == UVC_VS_PROBE) {
        assert(probe_calls == 2 && data[0] == 1 && !data[1]);
        assert(data[2] == 1 && data[3] == 11 && uvc_le32(data + 4) == 333333);
        return scenario == PROBE_SET_SHORT ? UVC_PROBE_LENGTH - 1 : UVC_PROBE_LENGTH;
    }
    if (request == UVC_GET_CUR && selector == UVC_VS_PROBE) {
        assert(probe_calls == 3);
        data[2] = scenario == PROBE_WRONG_FORMAT ? 2 : 1;
        data[3] = scenario == PROBE_WRONG_FRAME ? 12 : 11;
        uvc_put32(data + 4, scenario == PROBE_WRONG_INTERVAL ? 666666 : 333333);
        uvc_put32(data + 18, scenario == PROBE_OVERSIZE_FRAME ? 154190 : 154189);
        uvc_put32(data + 22, scenario == PROBE_BAD_PAYLOAD ? 801 : 800);
        data[16] = 0x5a; /* Camera-adjusted fields must survive into COMMIT. */
        return scenario == PROBE_GET_SHORT ? UVC_PROBE_LENGTH - 1 : UVC_PROBE_LENGTH;
    }
    assert(request == UVC_SET_CUR && selector == UVC_VS_COMMIT && probe_calls == 4);
    assert(data[2] == 1 && data[3] == 11 && uvc_le32(data + 4) == 333333);
    assert(uvc_le32(data + 18) == 154189 && uvc_le32(data + 22) == 800 && data[16] == 0x5a);
    if (scenario == COMMIT_FAIL) return -7102;
    committed = true;
    probed_ms = clock_ms;
    if (scenario == FAIL_DURING_PROBE) atomic_store(&av_audio.stream.failed, true);
    return UVC_PROBE_LENGTH;
}

static bool av_audio_start(s32 handle, const UvcMode *mode)
{
    assert(handle == 42 && mode->width == 320 && mode->height == 240);
    assert(!enables && !probes && !probe_calls && !committed && !idle_sets);
    assert(scenario != RESTART_REQUIRED && scenario != POISONED && scenario != UNPARKED);
    assert(usb_session.video == 42 && !usb_session.audio_poisoned);
    assert(usb_session.video_changed == parked_run
           && usb_session.microphone_selected == parked_run);
    event('A');
    ++starts;
    if (scenario == AUDIO_OPEN_FAIL) return false;
    usb_session.microphone = 43;
    usb_session.microphone_selected = true;
    audio_stream_init(&av_audio.stream, MIC_RATE_UNREADABLE);
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
    assert(committed);
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
    assert(av_audio.stream.rate == MIC_RATE_UNREADABLE && committed);
    assert(av_audio.start == clock_ms && av_audio.natural_drain);
    assert(av_audio.handle == 43 && av_audio.mode.interface == 3 && av_audio.mode.endpoint == 0x84);
    assert(!atomic_load(&av_audio.stream.failed) && !audio_stream_position(&av_audio.stream));
    assert(!av_audio.stream.bytes && !av_audio.stream.packets && !av_audio.stream.rejected);
    assert(!atomic_load(&av_audio.stop) && !atomic_load(&av_audio.last_ms));
    for (unsigned n = 0; n < ISO_QUEUE_DEPTH; ++n) {
        assert(!av_audio.queue.slots[n].pending);
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
    if (!av_audio.opened) return;
    assert(!closes);
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
    if (scenario == HOME || (scenario == HOME_AFTER_RESTART && restarts)
        || (scenario == HOME_AFTER_PROBE && committed))
        exit_requested = true;
    return scenario == B_CANCEL || (scenario == B_AFTER_RESTART && restarts)
                   || (scenario == B_AFTER_PROBE && committed)
               ? WPAD_BUTTON_B
               : 0;
}

static void usleep(unsigned microseconds)
{
    assert(microseconds == 1000 && (!enables || restarts));
    ++clock_ms;
    if (drain_test) {
        if (clock_ms == drain_complete_at) {
            for (unsigned n = 0; n < ISO_QUEUE_DEPTH; ++n)
                iso_queue_complete(&av_audio.queue.slots[n], 512);
        }
        return;
    }
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
    if (committed && !restarts) {
        if (scenario == STALL_AFTER_PROBE) {
            if (clock_ms - probed_ms == 500) atomic_store(&av_audio.stream.failed, true);
            return;
        }
        if (scenario == PARTIAL_AFTER_PROBE && clock_ms - probed_ms >= 100) return;
        if (scenario == PARTIAL_CONCEALED_AFTER_PROBE && clock_ms - probed_ms > 100) return;
    }
    if (scenario == PARTIAL_AFTER_RESTART && restarts && clock_ms - restarted_ms >= 100) return;
    if (scenario == PARTIAL && clock_ms >= 100) return;
    if (scenario == PARKED_SILENT) {
        audio_stream_packet(&av_audio.stream, data, 4096, 192, clock_ms);
        return;
    }
    if (scenario == SLOW_VALID && clock_ms % 5) return;
    assert(av_audio.running);
    assert(restarts ? pauses == 1 && enables == 1 : !pauses && !enables && (committed || !probes));
    unsigned length =
        scenario == EMPTY || (scenario == EMPTY_AFTER_RESTART && restarts)
                || (scenario == EMPTY_AFTER_PROBE && committed)
            ? 0
            : sizeof(data);
    if (scenario == INVALID || (scenario == INVALID_AFTER_RESTART && restarts)
        || (scenario == INVALID_AFTER_PROBE && committed))
        length = 3;
    if ((scenario == FOUR_REJECTED && clock_ms <= 4) || scenario == FIFTH_REJECTED) length = 4096;
    if (scenario == CONCEAL_AFTER_PROBE && committed) length = 4096;
    if (scenario == PARTIAL_CONCEALED_AFTER_PROBE && committed
        && (clock_ms - probed_ms) % 10 == 0)
        length = 4096;
    audio_stream_packet(&av_audio.stream, data, length, 192, clock_ms);
}

static IsoStop receive_photo(s32 handle, const UvcMode *mode, const UvcAlternate *alternate)
{
    assert(handle == 42 && mode->width == 320 && alternate->endpoint == 0x81);
    if (fallback_run) {
        assert(enables == 1 && !pauses && !restarts && !av_audio.opened && idles == 1);
    } else {
        assert(enables == !parked_run && pauses == !parked_run);
        assert(restarts == !parked_run && av_audio.running == screen_info.audio);
        assert(!usb_session.audio_poisoned);
        if (screen_info.audio || !parked_run) {
            assert(audio_stream_position(&av_audio.stream) >= AUDIO_BLOCK_BYTES);
            assert(clock_ms - restarted_ms >= 100);
        }
        assert(atomic_load(&av_audio.stream.consumed) == audio_stream_position(&av_audio.stream));
    }
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
    clock_ms = restarted_ms = probed_ms = 0;
    pauses = restarts = pre_pause_rejected = 0;
    exit_requested = silent = idle_unit_test = cleanup_test = settling_test = false;
    cleanup_calls = 0;
    idle_sets = probe_calls = 0;
    committed = parked_run = fallback_run = alternate_log_test = false;
    screen_info.audio = true;
    alternate_log_count = 0;
    memset(alternate_log, 0, sizeof(alternate_log));
    memset(events, 0, sizeof(events));
    memset(&av_audio, 0, sizeof(av_audio));
    usb_session = (UsbSession){0};
    av_recording.failed = false;
}

static s32 WC04_CancelIso(s32 handle, uint8_t endpoint)
{
    assert(drain_test && handle == 43 && endpoint == 0x84);
    ++cancel_calls;
    return -7;
}

static void test_audio_drain(void)
{
    const unsigned completions[] = {0, 1, 48, 64, 99, 100, 101};
    for (unsigned test = 0; test < sizeof(completions) / sizeof(completions[0]); ++test) {
        reset();
        drain_test = true;
        cancel_calls = 0;
        drain_complete_at = completions[test];
        av_audio.handle = 43;
        av_audio.mode.endpoint = 0x84;
        iso_queue_init(&av_audio.queue, audio_data, audio_sizes, 192);
        for (unsigned n = 0; n < ISO_QUEUE_DEPTH; ++n) {
            av_audio.queue.slots[n].pending = true;
            if (!drain_complete_at || n == 1) iso_queue_complete(&av_audio.queue.slots[n], 512);
        }
        av_audio_cancel(&av_audio);
        bool completed = drain_complete_at <= 100;
        assert(av_audio.natural_drain == completed);
        assert(cancel_calls == !completed);
        assert(av_audio.cancel_result == (completed ? 0 : -7));
        assert(clock_ms == (completed ? drain_complete_at : 100));
        for (unsigned n = 0; n < ISO_QUEUE_DEPTH; ++n)
            assert(av_audio.queue.slots[n].pending);
        drain_test = false;
    }
    puts("PASS: natural audio drain with out-of-order completion, bounded cancellation fallback and callback ownership");
}

static void test_rejections(void)
{
    for (unsigned test = 0; test < 5; ++test) {
        reset();
        audio_stream_init(&av_audio.stream, MIC_RATE_UNREADABLE);
        av_audio.mode.packet_capacity = 192;
        atomic_init(&av_audio.last_ms, 0);
        atomic_init(&av_audio.stop, false);
        av_audio.running = true;
        unsigned rounds = test < 2 ? 40 : 1;
        for (unsigned round = 0; round < rounds; ++round) {
            clock_ms = 19 + 16 * round;
            memset(audio_data, 0, sizeof(audio_data));
            for (unsigned n = 0; n < ISO_PACKETS; ++n) {
                audio_sizes[n] = test == 1 ? 0x1000 : 32;
                memset(audio_data + n * 192, 0x20, n + 1);
            }
            if (test == 0 && round % 10 == 0) audio_sizes[3] = 0x1000;
            if (test == 2) audio_sizes[15] = 193;
            if (test == 3) audio_sizes[15] = 65535;
            if (test == 4) audio_sizes[15] = 1;
            IsoSlot batch = {.data = audio_data, .sizes = audio_sizes,
                             .result = test == 1 ? 0 : 480};
            unsigned before = av_audio.stream.rejected;
            ++av_audio.queue.consumed;
            assert(av_audio_consume(&av_audio, &batch) == (test >= 2));
            assert(av_audio.stream.rejected - before
                   == (test == 0 ? round % 10 == 0 : test == 1 ? 16u : 1u));
            if (test == 1) {
                assert(!audio_stream_position(&av_audio.stream));
                assert(!audio_stream_received(&av_audio.stream));
                assert(av_audio_poll(&av_audio) == (clock_ms >= 500 ? ISO_TIMEOUT : ISO_RUNNING));
            } else if (!test) {
                assert(av_audio_poll(&av_audio) == ISO_RUNNING);
                assert(atomic_load(&av_audio.last_ms) == clock_ms);
            }
            audio_stream_discard(&av_audio.stream);
        }
        av_audio.running = false;
    }
    reset();
    audio_stream_init(&av_audio.stream, MIC_RATE_UNREADABLE);
    atomic_init(&av_audio.stop, false);
    atomic_init(&av_audio.last_ms, 0);
    assert(audio_stream_packet(&av_audio.stream, audio_data, 32, 192, 0));
    for (unsigned n = 0; n < ISO_PACKETS; ++n) audio_sizes[n] = 0x1000;
    av_audio.mode.packet_capacity = 192;
    clock_ms = 499;
    IsoSlot batch = {.data = audio_data, .sizes = audio_sizes};
    assert(av_audio_consume(&av_audio, &batch));
    assert(atomic_load(&av_audio.last_ms) == 0);
    clock_ms = 500;
    assert(av_audio_poll(&av_audio) == ISO_TIMEOUT);
    puts("PASS: rejected packets, all-error startup timeout, cumulative errors, strict malformed lengths and concealment-independent watchdog");
}

int main(void)
{
    test_audio_drain();
    test_rejections();
    const UvcMode mode = {.configuration = 1,
                          .version = 0x100,
                          .format = 1,
                          .frame = 11,
                          .interval = 333333,
                          .interval_count = 1,
                          .intervals = {333333},
                          .max_frame = 154189,
                          .endpoint = 0x81,
                          .interface = 1,
                          .width = 320,
                          .height = 240,
                          .alt_count = 1,
                          .alts = {{.number = 3, .endpoint = 0x81, .capacity = 800}}};
    const IsoStop expected[] = {
        [SUCCESS] = ISO_RECORD_STOPPED,
        [POISONED] = ISO_SUBMIT_FAILED,
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
        [FIFTH_REJECTED] = ISO_TIMEOUT,
        [SLOW_VALID] = ISO_RECORD_STOPPED,
        [PROBE_SET_SHORT] = ISO_SUBMIT_FAILED,
        [PROBE_GET_SHORT] = ISO_SUBMIT_FAILED,
        [PROBE_WRONG_FORMAT] = ISO_SUBMIT_FAILED,
        [PROBE_WRONG_FRAME] = ISO_SUBMIT_FAILED,
        [PROBE_WRONG_INTERVAL] = ISO_SUBMIT_FAILED,
        [UNPARKED] = ISO_SUBMIT_FAILED,
        [PROBE_OVERSIZE_FRAME] = ISO_SUBMIT_FAILED,
        [PROBE_BAD_PAYLOAD] = ISO_SUBMIT_FAILED,
        [COMMIT_FAIL] = ISO_SUBMIT_FAILED,
        [PAUSE_FAIL] = ISO_COMPLETION_FAILED,
        [RESTART_FAIL] = ISO_SUBMIT_FAILED,
        [EMPTY_AFTER_RESTART] = ISO_TIMEOUT,
        [PARTIAL_AFTER_RESTART] = ISO_TIMEOUT,
        [INVALID_AFTER_RESTART] = ISO_COMPLETION_FAILED,
        [STALL_AFTER_RESTART] = ISO_COMPLETION_FAILED,
        [SLOW_ENABLE] = ISO_RECORD_STOPPED,
        [B_AFTER_RESTART] = ISO_CANCELLED,
        [HOME_AFTER_RESTART] = ISO_EXIT,
        [FAIL_DURING_PROBE] = ISO_COMPLETION_FAILED,
        [EMPTY_AFTER_PROBE] = ISO_TIMEOUT,
        [PARTIAL_AFTER_PROBE] = ISO_TIMEOUT,
        [INVALID_AFTER_PROBE] = ISO_COMPLETION_FAILED,
        [STALL_AFTER_PROBE] = ISO_COMPLETION_FAILED,
        [CONCEAL_AFTER_PROBE] = ISO_COMPLETION_FAILED,
        [B_AFTER_PROBE] = ISO_CANCELLED,
        [HOME_AFTER_PROBE] = ISO_EXIT,
        [PARTIAL_CONCEALED_AFTER_PROBE] = ISO_TIMEOUT,
        [PARKED] = ISO_RECORD_STOPPED,
        [PARKED_SILENT] = ISO_TIMEOUT,
        [RESTART_REQUIRED] = ISO_SUBMIT_FAILED,
        [REPLUGGED] = ISO_RECORD_STOPPED,
    };
    _Static_assert(sizeof(expected) / sizeof(expected[0]) == SCENARIOS, "scenario results");
    for (scenario = 0; scenario < SCENARIOS; ++scenario) {
        reset();
        parked_run = scenario == PARKED || scenario == PARKED_SILENT;
        if (parked_run || scenario == POISONED || scenario == UNPARKED)
            usb_session = (UsbSession){.video = 42, .microphone = 43, .video_changed = true,
                                       .microphone_selected = true,
                                       .audio_poisoned = scenario == POISONED};
        if (scenario == RESTART_REQUIRED)
            usb_session = (UsbSession){.video = 42, .video_changed = true, .video_idle = true};
        if (scenario == REPLUGGED)
            usb_session = (UsbSession){.video = 41, .microphone = 44, .video_changed = true,
                                       .microphone_selected = true};
        UvcAlternate alternate = parked_run ? mode.alts[0] : (UvcAlternate){0};
        bool video_changed = true;
        assert(receive_av(42, &mode, &alternate, parked_run, &video_changed)
               == expected[scenario]);
        assert(av_recording.failed
               == (expected[scenario] == ISO_SUBMIT_FAILED
                   || expected[scenario] == ISO_COMPLETION_FAILED
                   || expected[scenario] == ISO_TIMEOUT));
        assert(!av_audio.opened && !idle_sets);
        bool idle_failed = scenario == POISONED || scenario == UNPARKED
                           || scenario == RESTART_REQUIRED;
        bool preparation_failed = scenario == PROBE_FAIL
            || (scenario >= PROBE_SET_SHORT && scenario <= PROBE_WRONG_INTERVAL)
            || (scenario >= PROBE_OVERSIZE_FRAME && scenario <= COMMIT_FAIL);
        if (idle_failed) {
            assert(!starts && !enables && !captures && !closes);
        } else if (scenario == AUDIO_OPEN_FAIL) {
            assert(starts == 1 && !enables && !captures && !closes);
        } else {
            assert(starts == 1 && closes == 1);
        }
        bool activated = scenario == SUCCESS || scenario == ENABLE_FAIL
                         || scenario == FAIL_DURING_ENABLE || scenario == FAIL_AFTER_ENABLE
                         || scenario == FOUR_REJECTED || scenario == SLOW_VALID
                         || (scenario >= RESTART_FAIL && scenario <= HOME_AFTER_RESTART)
                         || scenario == REPLUGGED;
        assert(enables == activated);
        bool recorded = scenario == SUCCESS || scenario == FAIL_AFTER_ENABLE
                        || scenario == FOUR_REJECTED || scenario == SLOW_VALID
                        || scenario == SLOW_ENABLE || scenario == PARKED
                        || scenario == REPLUGGED;
        assert(captures == recorded);
        assert(pauses == (activated || scenario == PAUSE_FAIL));
        assert(restarts
               == (activated && scenario != ENABLE_FAIL && scenario != FAIL_DURING_ENABLE));
        if (captures)
            assert(!strcmp(events, parked_run              ? "ACX"
                                   : scenario == REPLUGGED ? "LAPQVRCX"
                                                           : "APQVRCX"));
        if (idle_failed) assert(!event_count);
        assert(usb_session.audio_poisoned == (scenario == POISONED || scenario == PARKED_SILENT));
        assert(video_changed == activated);
        cleanup_test = true;
        stop_video_stream(42, video_changed);
        assert(cleanup_calls == video_changed);
        assert(idle_sets == (unsigned)video_changed);
        if (video_changed)
            assert(usb_session.video_changed && usb_session.video_idle
                   && usb_session.audio_poisoned);
        if (scenario == EMPTY || scenario == PARTIAL) assert(clock_ms == 1000);
        if (scenario == INVALID)
            assert(av_audio.stream.rejected == 1 && !audio_stream_position(&av_audio.stream));
        if (scenario == FIFTH_REJECTED || scenario == PARKED_SILENT)
            assert(av_audio.stream.rejected == 1000 && !audio_stream_position(&av_audio.stream));
        if (scenario == FOUR_REJECTED)
            assert(pre_pause_rejected == 4 && !av_audio.stream.rejected && clock_ms == 304);
        if (scenario == SLOW_VALID) assert(clock_ms == 1500);
        if (scenario == SLOW_ENABLE) assert(clock_ms == 924);
        if (scenario == EMPTY_AFTER_RESTART || scenario == PARTIAL_AFTER_RESTART)
            assert(clock_ms == 1200);
        if (scenario == STALL_AFTER_RESTART) assert(clock_ms == 700);
        bool initial_audio_failed = parked_run || scenario == AUDIO_OPEN_FAIL || scenario == EMPTY
                                    || scenario == PARTIAL || scenario == INVALID
                                    || scenario == STALL || scenario == B_CANCEL
                                    || scenario == HOME || scenario == EARLY_EXIT
                                    || scenario == FIFTH_REJECTED;
        assert(probes == (!idle_failed && !initial_audio_failed));
        assert(committed == (probes && !preparation_failed));
        if (scenario == EMPTY_AFTER_PROBE || scenario == PARTIAL_AFTER_PROBE)
            assert(clock_ms == 1100 && !pauses && !enables);
        if (scenario == CONCEAL_AFTER_PROBE) {
            assert(audio_stream_received(&av_audio.stream) == AUDIO_BLOCK_BYTES);
            assert(audio_stream_position(&av_audio.stream) > AUDIO_BLOCK_BYTES);
            assert(!pauses && !enables && clock_ms == 111);
        }
        if (scenario == PARTIAL_CONCEALED_AFTER_PROBE) {
            assert(audio_stream_position(&av_audio.stream) == 2 * AUDIO_BLOCK_BYTES);
            assert(audio_stream_received(&av_audio.stream) == 2 * AUDIO_BLOCK_BYTES - 320);
            assert(!pauses && !enables && clock_ms == 1100);
        }
    }
    const unsigned silent_cases[] = {SUCCESS, POISONED, PROBE_FAIL, ENABLE_FAIL};
    for (unsigned n = 0; n < 4; ++n) {
        scenario = silent_cases[n];
        reset();
        silent = true;
        UvcAlternate alternate;
        assert(negotiate(42, &mode, &alternate) == (scenario == SUCCESS));
        assert(!starts && !closes && !captures);
        assert(!strcmp(events, scenario == POISONED     ? "I"
                               : scenario == PROBE_FAIL ? "IP"
                                                        : "IPV"));
        assert(usb_session.video == 42 && usb_session.video_changed && usb_session.audio_poisoned);
        assert(usb_session.video_idle == (scenario == PROBE_FAIL));
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
        unsigned scenario;
        bool parked;
        IsoStop result;
        const char *events;
    } photo_cases[] = {{SUCCESS, false, ISO_RECORD_STOPPED, "APQVRXC"},
                       {AUDIO_OPEN_FAIL, false, ISO_RECORD_STOPPED, "AIPVC"},
                       {B_CANCEL, false, ISO_CANCELLED, "AX"},
                       {SUCCESS, true, ISO_RECORD_STOPPED, "C"}};
    for (unsigned n = 0; n < 4; ++n) {
        reset();
        scenario = photo_cases[n].scenario;
        screen_info.audio = false;
        parked_run = photo_cases[n].parked;
        fallback_run = scenario == AUDIO_OPEN_FAIL;
        if (parked_run)
            usb_session = (UsbSession){.video = 42, .microphone = 43, .video_changed = true,
                                       .microphone_selected = true};
        UvcAlternate alternate = parked_run ? mode.alts[0] : (UvcAlternate){0};
        bool video_changed = !parked_run;
        assert(run_camera(42, (UvcMode *)&mode, &alternate, parked_run, &video_changed)
               == photo_cases[n].result);
        assert(!strcmp(events, photo_cases[n].events) && !av_audio.opened);
        assert(video_changed == (scenario != B_CANCEL && !parked_run));
        assert(usb_session.audio_poisoned == fallback_run);
        /* The stubbed close keeps the microphone; test_mic_startup covers the real release. */
        assert(usb_session.microphone_selected == !fallback_run);
    }

    reset();
    alternate_log_test = true;
    usb_session = (UsbSession){.video = 42, .microphone = 43, .microphone_selected = true};
    assert(set_video_alternate(42, 3) == 0 && !usb_session.audio_poisoned);
    assert(usb_session.video_changed && !usb_session.video_idle);
    UvcAlternate parked_alt = mode.alts[0];
    char status[48] = "";
    for (unsigned n = 0; n < 8; ++n) {
        static const IsoStop results[] = {ISO_RECORD_STOPPED, ISO_CANCELLED, ISO_PHOTO_READY,
                                          ISO_EXIT, ISO_TIMEOUT, ISO_SUBMIT_FAILED,
                                          ISO_COMPLETION_FAILED, ISO_RUNNING};
        usb_session.video_parked = false;
        bool parked = park_video_stream(&mode, &parked_alt, results[n]);
        assert(parked == (n < 3) && usb_session.video_parked == parked);
    }
    assert(park_video_stream(&mode, &parked_alt, ISO_RECORD_STOPPED));
    assert(!sound_needs_restart(320, 240, status, sizeof(status)));
    assert(sound_needs_restart(640, 480, status, sizeof(status)));
    assert(!strcmp(status, "Sound needs 320x240 or a restart"));
    status[0] = 0;
    assert(!parked_elsewhere(320, 240, status, sizeof(status)) && !status[0]);
    assert(parked_elsewhere(640, 480, status, sizeof(status)));
    s32 handle = 0;
    UvcMode taken;
    UvcAlternate taken_alt;
    assert(!take_parked_camera(640, 480, &handle, &taken, &taken_alt) && usb_session.video_parked);
    assert(take_parked_camera(320, 240, &handle, &taken, &taken_alt) && !usb_session.video_parked);
    assert(handle == 42 && taken.width == 320 && taken_alt.number == 3
           && taken_alt.endpoint == 0x81);
    assert(!take_parked_camera(320, 240, &handle, &taken, &taken_alt));
    assert(!parked_elsewhere(640, 480, status, sizeof(status)));
    assert(sound_needs_restart(320, 240, status, sizeof(status)));
    assert(!strcmp(status, "Restart WiiCam to record with sound"));
    assert(park_video_stream(&mode, &parked_alt, ISO_CANCELLED));
    assert(set_video_alternate(42, 0) == 0 && usb_session.audio_poisoned);
    assert(!usb_session.video_parked && usb_session.video_idle);
    assert(!park_video_stream(&mode, &parked_alt, ISO_RECORD_STOPPED));
    assert(sound_needs_restart(320, 240, status, sizeof(status)));
    assert(!strcmp(alternate_log, "v3v0"));

    const struct {
        bool selected, poisoned, changed, idle;
        const char *log;
    } releases[] = {{true, false, true, false, "m0v0"}, {true, true, true, true, "m0"},
                    {false, true, true, false, "v0"},  {false, false, false, false, ""},
                    {true, false, false, false, "m0"}};
    for (unsigned n = 0; n < sizeof(releases) / sizeof(releases[0]); ++n) {
        reset();
        alternate_log_test = true;
        usb_session = (UsbSession){.video = 42,
                                   .microphone = 43,
                                   .microphone_selected = releases[n].selected,
                                   .audio_poisoned = releases[n].poisoned,
                                   .video_changed = releases[n].changed,
                                   .video_idle = releases[n].idle};
        release_camera();
        assert(!strcmp(alternate_log, releases[n].log));
        assert(!usb_session.microphone_selected);
        assert(!usb_session.video_changed || usb_session.video_idle);
    }
    reset();
    alternate_log_test = true;
    assert(set_video_alternate(42, 3) == 0 && usb_session.audio_poisoned);
    assert(!park_video_stream(&mode, &parked_alt, ISO_RECORD_STOPPED));
    puts(
        "PASS: actual startup: 48 audio-first/parked/refusal/readiness/control/cleanup cases, 4 video-without-audio paths, 4 bounded settling/cancellation cases, 8 parking outcomes, park/take/restart-status and poison rules, 5 release cases, 4 photo-first paths; real UVC negotiation linked");
}
