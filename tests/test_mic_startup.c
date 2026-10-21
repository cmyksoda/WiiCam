/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */
#include "audio_stream.h"
#include "iso_queue.h"
#include "microphone.h"
#include "uvc_fixture.h"
#include <stdarg.h>
#include <stdio.h>

typedef int32_t s32;
typedef uint8_t u8;
typedef unsigned lwp_t;
typedef struct {
    int32_t device_id;
    uint16_t vid, pid;
    uint32_t token;
} usb_device_entry;

enum {
    SUCCESS, REUSED, NO_MODE, ENUM_FAIL, AMBIGUOUS_PAIR, BAD_IDENTITY, CONFIG_MISMATCH,
    CONFIG_SHORT, IDLE_FAIL, ACTIVE_FAIL, SET_SHORT, SET_FAIL, USER_CANCEL, USER_EXIT,
    THREAD_FAIL, OTHER_OK, OTHER_RATE_MISMATCH, OTHER_GET_SHORT, OTHER_GET_FAIL, SCENARIOS
};
#define CONFIG_CAPACITY CAMERA_CONFIG_CAPACITY
#define WPAD_BUTTON_B 2u

static bool exit_requested;
static unsigned scenario;
static uint64_t clock_ms, queue_ms;
static void *thread_context;
static char calls[256], log_text[4096];
static size_t log_size;
static uint8_t config_data[CONFIG_CAPACITY], probe_data[32];
static uint8_t *config = config_data, *probe = probe_data;
static uint8_t audio_data[ISO_QUEUE_DEPTH * ISO_DATA_BYTES], audio_stack[32 * 1024];
static uint16_t audio_sizes[ISO_QUEUE_DEPTH * ISO_PACKETS];
static uint8_t *audio_iso_data = audio_data;
static uint16_t *audio_iso_sizes = audio_sizes;
static Fixture descriptor;
static const s32 video_id = (s32)0xe71e0042u, audio_id = (s32)0xe71c0042u;

static bool other_camera(void)
{
    return scenario >= OTHER_OK;
}

static void call(const char *name)
{
    size_t used = strlen(calls);
    int written = snprintf(calls + used, sizeof(calls) - used, "%s%s", used ? " " : "", name);
    assert(written > 0 && (size_t)written < sizeof(calls) - used);
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
    va_list args;
    va_start(args, format);
    int written = vsnprintf(log_text + log_size, sizeof(log_text) - log_size, format, args);
    va_end(args);
    assert(written >= 0 && (size_t)written < sizeof(log_text) - log_size);
    log_size += (size_t)written;
}

static uint32_t buttons(void)
{
    if (scenario == USER_EXIT) exit_requested = true;
    return scenario == USER_CANCEL ? WPAD_BUTTON_B : 0;
}

static void usleep(unsigned microseconds)
{
    assert(microseconds == 1000);
    ++clock_ms;
}

static s32 USB_GetDeviceList(usb_device_entry *devices, u8 maximum, u8 cls, u8 *count)
{
    assert(maximum == CAMERA_MAX_DEVICES && !cls);
    call("list");
    if (scenario == ENUM_FAIL) return -4;
    unsigned vid = other_camera() ? 0x1234 : 0x0c45;
    devices[0] = (usb_device_entry){video_id, vid, 0x6366, 0x00420107};
    devices[1] = (usb_device_entry){audio_id, vid, 0x6366, 0x00420302};
    *count = 2;
    if (scenario == AMBIGUOUS_PAIR) {
        devices[2] = devices[1];
        devices[2].device_id = (s32)0xe7190043u;
        *count = 3;
    }
    return 0;
}

static int camera_open(void *context, const CameraEntry *entry, int32_t *handle)
{
    (void)context;
    assert(entry->id == audio_id);
    call("open");
    *handle = audio_id;
    return 0;
}

static int camera_parameters(void *context, int32_t handle, uint8_t *data)
{
    (void)context;
    assert(handle == audio_id);
    call("params");
    memset(data, 0, 192);
    data[20] = 18;
    data[21] = data[37] = 1;
    data[28] = other_camera() ? 0x12 : 0x0c;
    data[29] = other_camera() ? 0x34 : 0x45;
    data[30] = 0x63;
    data[31] = 0x66;
    data[40] = 9;
    data[41] = 2;
    data[44] = 4;
    data[45] = 1;
    data[52] = 9;
    data[53] = 4;
    data[54] = 3;
    data[57] = 1;
    data[58] = scenario == BAD_IDENTITY ? 1 : 2;
    return 0;
}

static int camera_read(void *context, int32_t handle, uint8_t request, uint16_t value,
                       uint16_t length, uint8_t *data)
{
    (void)context;
    assert(handle == audio_id);
    if (request == 8) {
        assert(!value && length == 1);
        call("config");
        data[0] = 1;
        return 9;
    }
    assert(request == 6 && value == 0x200 && length == descriptor.length);
    call("descriptor");
    memcpy(data, descriptor.data, length);
    if (scenario == CONFIG_MISMATCH) data[8] ^= 1;
    return length + (scenario == CONFIG_SHORT ? 7 : 8);
}

static void camera_close(void *context, int32_t handle)
{
    (void)context;
    assert(handle == audio_id);
    call("close");
}

static void USB_CloseDevice(s32 *handle)
{
    assert(*handle == audio_id);
    call("close");
    *handle = 0;
}

static s32 WC04_SetAlternate(s32 handle, u8 alternate)
{
    assert(handle == audio_id && alternate <= 1);
    call(alternate ? "alt1" : "alt0");
    return (scenario == IDLE_FAIL && !alternate) || (scenario == ACTIVE_FAIL && alternate) ? -4
                                                                                          : 0;
}

static s32 USB_WriteCtrlMsg(s32 handle, u8 type, u8 request, uint16_t value, uint16_t index,
                            unsigned length, uint8_t *data)
{
    assert(handle == audio_id && type == 0x22 && request == 1 && value == 0x100
           && index == 0x84 && length == 3);
    assert(data[0] == 0x80 && data[1] == 0x3e && !data[2]);
    call("set");
    return scenario == SET_SHORT ? 10 : scenario == SET_FAIL ? -7102 : 11;
}

static s32 USB_ReadCtrlMsg(s32 handle, u8 type, u8 request, uint16_t value, uint16_t index,
                           unsigned length, uint8_t *data)
{
    assert(other_camera());
    assert(handle == audio_id && type == 0xa2 && request == 0x81 && value == 0x100
           && index == 0x84 && length == 3);
    call("get");
    if (scenario == OTHER_GET_SHORT) return 10;
    if (scenario == OTHER_GET_FAIL) return -7102;
    data[0] = scenario == OTHER_RATE_MISMATCH ? 0x44 : 0x80;
    data[1] = scenario == OTHER_RATE_MISMATCH ? 0xac : 0x3e;
    data[2] = 0;
    return 11;
}

/* The audio worker never runs here; these only satisfy its references. */
static s32 iso_finished(s32 result, void *context)
{
    (void)result;
    (void)context;
    assert(false);
    return 0;
}

static s32 USB_ReadIsoMsgAsync(s32 handle, u8 endpoint, u8 packets, uint16_t *sizes, void *data,
                               s32 (*callback)(s32, void *), void *context)
{
    (void)handle, (void)endpoint, (void)packets, (void)sizes, (void)data, (void)callback;
    (void)context;
    assert(false);
    return -1;
}

static s32 WC04_CancelIso(s32 handle, u8 endpoint)
{
    (void)handle, (void)endpoint;
    assert(false);
    return -1;
}

static void idle_capture(void *context)
{
    (void)context;
    assert(false);
}

static void *av_audio_worker(void *context);

static int LWP_CreateThread(lwp_t *thread, void *(*entry)(void *), void *context, void *stack,
                            unsigned size, unsigned priority)
{
    assert(entry == av_audio_worker && stack == audio_stack);
    assert(size == sizeof(audio_stack) && priority == 80);
    call("queue");
    queue_ms = clock_ms;
    thread_context = context;
    if (scenario == THREAD_FAIL) return -1;
    *thread = 1;
    return 0;
}

static int LWP_JoinThread(lwp_t thread, void **result)
{
    assert(thread == 1 && !result);
    call("join");
    return 0;
}

#include "mic_fixture.h"
#include "mic_startup_under_test.h"

static const char *const expected[SCENARIOS] = {
    [SUCCESS] = "list open params config descriptor alt0 alt1 set queue",
    [REUSED] = "queue",
    [NO_MODE] = "",
    [ENUM_FAIL] = "list",
    [AMBIGUOUS_PAIR] = "list",
    [BAD_IDENTITY] = "list open params close",
    [CONFIG_MISMATCH] = "list open params config descriptor close",
    [CONFIG_SHORT] = "list open params config descriptor close",
    [IDLE_FAIL] = "list open params config descriptor alt0 alt0 close",
    [ACTIVE_FAIL] = "list open params config descriptor alt0 alt1 alt0 close",
    [SET_SHORT] = "list open params config descriptor alt0 alt1 set alt0 close",
    [SET_FAIL] = "list open params config descriptor alt0 alt1 set alt0 close",
    [USER_CANCEL] = "list open params config descriptor alt0 alt1 set alt0 close",
    [USER_EXIT] = "list open params config descriptor alt0 alt1 set alt0 close",
    [THREAD_FAIL] = "list open params config descriptor alt0 alt1 set queue alt0 close",
    [OTHER_OK] = "list open params config descriptor alt0 alt1 set get queue",
    [OTHER_RATE_MISMATCH] = "list open params config descriptor alt0 alt1 set get alt0 close",
    [OTHER_GET_SHORT] = "list open params config descriptor alt0 alt1 set get alt0 close",
    [OTHER_GET_FAIL] = "list open params config descriptor alt0 alt1 set get alt0 close",
};

int main(void)
{
    descriptor = microphone();
    const UvcMode video = {.interface = 1, .width = 320, .height = 240};
    const UacMode selected = {.control_interface = 2, .interface = 3, .alternate = 1,
                              .endpoint = 0x84, .packet_capacity = 192, .interval = 4};
    for (scenario = 0; scenario < SCENARIOS; ++scenario) {
        clock_ms = queue_ms = 0;
        thread_context = NULL;
        exit_requested = false;
        calls[0] = log_text[0] = 0;
        log_size = 0;
        memset(&av_audio, 0, sizeof(av_audio));
        usb_session = (UsbSession){.video = video_id};
        if (scenario == REUSED)
            usb_session = (UsbSession){.video = video_id, .microphone = audio_id,
                                       .microphone_mode = selected,
                                       .microphone_rate = MIC_RATE_UNREADABLE,
                                       .microphone_selected = true, .video_changed = true};
        memcpy(config, descriptor.data, descriptor.length);
        if (scenario == NO_MODE) config[2] = 9, config[3] = 0;
        bool started = av_audio_start(video_id, &video);
        assert(!strcmp(calls, expected[scenario]));
        assert(started == (scenario == SUCCESS || scenario == REUSED || scenario == OTHER_OK));
        if (!started) {
            assert(!av_audio.opened && !av_audio.running && !usb_session.microphone_selected);
            const char *message = scenario == NO_MODE ? "Sound requires one unambiguous"
                                  : scenario == AMBIGUOUS_PAIR ? "pairing absent or ambiguous"
                                  : scenario >= BAD_IDENTITY && scenario <= CONFIG_SHORT
                                      ? "configuration mismatch"
                                  : scenario == USER_CANCEL || scenario == USER_EXIT
                                          || scenario == THREAD_FAIL || scenario == ENUM_FAIL
                                      ? ""
                                      : "Microphone alternate or rate setup failed";
            assert(strstr(log_text, message));
            continue;
        }
        assert(av_audio.opened && av_audio.running && av_audio.natural_drain);
        assert(thread_context == &av_audio);
        assert(av_audio.handle == audio_id && av_audio.mode.endpoint == 0x84);
        assert(av_audio.mode.packet_capacity == 192 && usb_session.microphone_selected);
        assert(usb_session.microphone == audio_id && usb_session.microphone_mode.alternate == 1);
        MicRateResult rate = scenario == OTHER_OK ? MIC_RATE_OK : MIC_RATE_UNREADABLE;
        assert(usb_session.microphone_rate == rate && av_audio.stream.rate == rate);
        /* Only an unreadable rate waits for the clock to settle before streaming. */
        assert(queue_ms == (scenario == SUCCESS ? AV_RATE_SETTLE_MS : 0));
        /* After a video alternate change the microphone stays selected for the next clip. */
        calls[0] = 0;
        usb_session.video_changed = scenario == REUSED;
        av_audio_close();
        assert(!strcmp(calls, scenario == REUSED ? "join" : "join alt0 close"));
        assert(!av_audio.opened && usb_session.microphone_selected == (scenario == REUSED));
        assert(strstr(log_text, "Audio stopped: "));
    }
    printf("PASS: microphone startup and release: %u pairing, identity, configuration, alternate, "
           "rate, settling, thread and reuse scenarios\n",
           SCENARIOS);
    return 0;
}
