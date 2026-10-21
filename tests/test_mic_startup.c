/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */
#define _POSIX_C_SOURCE 200809L
#include "microphone.h"
#include "iso_trace.h"
#include "iso_queue.h"
#include "screen.h"
#include "storage.h"
#include "uvc_fixture.h"
#include <stdarg.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

typedef int32_t s32;
typedef uint8_t u8;
typedef struct {
    int32_t device_id;
    uint16_t vid, pid;
    uint32_t token;
} usb_device_entry;

enum {
    SUCCESS, NO_CAMERA, ENUM_FAIL, AMBIGUOUS_PAIR, BAD_IDENTITY, CONFIG_MISMATCH,
    CONFIG_SHORT, IDLE_FAIL, ACTIVE_FAIL, SET_SHORT, SET_FAIL, USER_CANCEL, USER_EXIT,
    SUBMIT_FAIL, COMPLETION_FAIL, ALLOC_FAIL, STORAGE_FAIL, NO_SD, OTHER_OK,
    OTHER_RATE_MISMATCH, OTHER_GET_SHORT, OTHER_GET_FAIL, PCM_48K, PCM_24K, PCM_8K,
    ODD_SIZE, OVERSIZE, DELAYED_PCM, SPARSE_LOSS, NO_PCM, EMPTY_PCM, LOSS_BURST,
    LOSS_BUDGET, NATURAL_CAPTURE, NATURAL_CANCEL, STALLED_CANCEL, OTHER_STATUS, PCM_STALL,
    VIDEO_CHANGED, SCENARIO_COUNT
};
#define CONFIG_CAPACITY CAMERA_CONFIG_CAPACITY
#define WPAD_BUTTON_B 2
#define PPC_TIMER_CLOCK 1000

static bool mounted, exit_requested, stopping;
static uint8_t config_data[CONFIG_CAPACITY], probe_data[32];
static uint8_t iso_buffer[ISO_QUEUE_DEPTH * ISO_DATA_BYTES];
static uint16_t size_buffer[ISO_QUEUE_DEPTH * ISO_PACKETS];
static uint8_t *config = config_data, *probe = probe_data, *iso_data = iso_buffer;
static uint16_t *iso_sizes = size_buffer;
static unsigned scenario, video_closes, audio_closes, opens, submissions, controls, cancels;
static unsigned pending_count, callback_count, frees, rejected_injected;
static uint64_t clock_ms, stopping_ms, cancel_ms;
static IsoSlot *pending[ISO_QUEUE_DEPTH];
static char log_text[16384];
static size_t log_size;
static Fixture descriptor;
static const s32 video_id = (s32)0xe71e0042u, audio_id = (s32)0xe71c0042u;

static bool other_camera(void)
{
    return (scenario >= OTHER_OK && scenario <= OTHER_GET_FAIL) || scenario == OTHER_STATUS;
}

static uint64_t gettime(void) { return clock_ms; }
static uint64_t ticks_to_millisecs(uint64_t value) { return value; }
static int IOS_GetVersion(void) { return 58; }
static int IOS_GetRevision(void) { return 6176; }

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
    if (scenario == USER_EXIT && clock_ms > 300) exit_requested = true;
    return clock_ms > 300
                   && (scenario == USER_CANCEL || scenario == NATURAL_CANCEL
                       || scenario == STALLED_CANCEL)
               ? WPAD_BUTTON_B
               : 0;
}

static void draw_screen(ScreenPage page, const uint8_t *rgb, unsigned width, unsigned height,
                        const char *status)
{
    (void)rgb;
    assert(page == SCREEN_WAITING && width == 320 && height == 240);
    if (!strcmp(status, "Stopping microphone")) {
        stopping = true;
        stopping_ms = clock_ms;
    }
}

static bool find_camera(s32 *handle, unsigned width, unsigned height, UvcMode *mode)
{
    assert(width == 320 && height == 240);
    if (scenario == NO_CAMERA) return false;
    memcpy(config, descriptor.data, descriptor.length);
    *handle = video_id;
    *mode = (UvcMode){.interface = 1, .width = 320, .height = 240};
    return true;
}

static s32 USB_GetDeviceList(usb_device_entry *devices, u8 maximum, u8 cls, u8 *count)
{
    assert(maximum == CAMERA_MAX_DEVICES && !cls);
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
    ++opens;
    *handle = audio_id;
    return 0;
}

static int camera_parameters(void *context, int32_t handle, uint8_t *data)
{
    (void)context;
    assert(handle == audio_id);
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
        data[0] = 1;
        return 9;
    }
    assert(request == 6 && value == 0x200 && length == descriptor.length);
    memcpy(data, descriptor.data, length);
    if (scenario == CONFIG_MISMATCH) data[8] ^= 1;
    return length + (scenario == CONFIG_SHORT ? 7 : 8);
}

static void camera_close(void *context, int32_t handle)
{
    (void)context;
    assert(handle == audio_id && !pending_count);
    ++audio_closes;
}

static void USB_CloseDevice(s32 *handle)
{
    assert(!pending_count);
    if (*handle == video_id)
        ++video_closes;
    else {
        assert(*handle == audio_id);
        ++audio_closes;
    }
    *handle = 0;
}

static bool microphone_selectable(s32 video)
{
    assert(video == video_id && !opens);
    return scenario != VIDEO_CHANGED;
}

static s32 WC04_SetAlternate(s32 handle, u8 alternate)
{
    assert(handle == audio_id && alternate <= 1 && !pending_count);
    return (scenario == IDLE_FAIL && !alternate) || (scenario == ACTIVE_FAIL && alternate)
               ? -4
               : 0;
}

static s32 USB_WriteCtrlMsg(s32 handle, u8 type, u8 request, uint16_t value, uint16_t index,
                            unsigned length, uint8_t *data)
{
    assert(handle == audio_id && type == 0x22 && request == 1 && value == 0x100
           && index == 0x84 && length == 3);
    assert(data[0] == 0x80 && data[1] == 0x3e && !data[2]);
    ++controls;
    return scenario == SET_SHORT ? 10 : scenario == SET_FAIL ? -7102 : 11;
}

static s32 USB_ReadCtrlMsg(s32 handle, u8 type, u8 request, uint16_t value, uint16_t index,
                           unsigned length, uint8_t *data)
{
    assert(other_camera());
    assert(handle == audio_id && type == 0xa2 && request == 0x81 && value == 0x100
           && index == 0x84 && length == 3);
    ++controls;
    if (scenario == OTHER_GET_SHORT) return 10;
    if (scenario == OTHER_GET_FAIL) return -7102;
    data[0] = scenario == OTHER_RATE_MISMATCH ? 0x44 : 0x80;
    data[1] = scenario == OTHER_RATE_MISMATCH ? 0xac : 0x3e;
    data[2] = 0;
    return 11;
}

static s32 iso_finished(s32 result, void *context)
{
    ++callback_count;
    iso_queue_complete(context, result);
    return 0;
}

static s32 USB_ReadIsoMsgAsync(s32 handle, u8 endpoint, u8 packets, uint16_t *sizes, void *data,
                               s32 (*callback)(s32, void *), void *context)
{
    assert(handle == audio_id && endpoint == 0x84 && packets == ISO_PACKETS);
    ++submissions;
    clock_ms += 16;
    if (scenario == SUBMIT_FAIL) return -4;
    unsigned total = 0;
    for (unsigned n = 0; n < packets; ++n) {
        assert(sizes[n] == 192);
        unsigned length = scenario == PCM_48K ? 96 : scenario == PCM_24K ? 48
                          : scenario == PCM_8K ? 16 : scenario == EMPTY_PCM ? 0 : 32;
        if (scenario == PCM_STALL && submissions > 10) length = 0;
        bool error = scenario == NO_PCM || (scenario == DELAYED_PCM && submissions <= 8)
                     || (scenario == SPARSE_LOSS && n == 2 && submissions % 40 == 0)
                     || (scenario == LOSS_BURST && submissions == 40 && n < 11)
                     || (scenario == LOSS_BUDGET && submissions >= 10 && n % 2 == 0)
                     || scenario == OTHER_STATUS;
        if (error) length = 0x1000;
        if (submissions == 40 && n == 2 && scenario == ODD_SIZE) length = 31;
        if (submissions == 40 && n == 2 && scenario == OVERSIZE) length = 65535;
        sizes[n] = length;
        bool invalid = length > 192 || (length & 1);
        if (invalid) ++rejected_injected;
        memset((uint8_t *)data + n * 192, invalid ? 0x7f : 0x20, 192);
        if (!invalid) total += length;
    }
    if (scenario == NATURAL_CANCEL || scenario == STALLED_CANCEL
        || (scenario == NATURAL_CAPTURE && submissions >= 314)) {
        assert(pending_count < ISO_QUEUE_DEPTH);
        pending[pending_count++] = context;
        return 0;
    }
    callback(scenario == COMPLETION_FAIL ? -4 : (s32)total, context);
    return 0;
}

static void advance_callbacks(void)
{
    bool natural = (scenario == NATURAL_CANCEL || scenario == NATURAL_CAPTURE)
                   && stopping && clock_ms - stopping_ms >= 32;
    bool cancelled = scenario == STALLED_CANCEL && cancels && clock_ms - cancel_ms >= 3;
    if (natural || cancelled) {
        while (pending_count)
            iso_finished(cancelled ? -4 : 512, pending[--pending_count]);
    }
}

static int test_usleep(unsigned microseconds)
{
    assert(microseconds == 1000);
    ++clock_ms;
    advance_callbacks();
    return 0;
}

static void idle_capture(void *context)
{
    (void)context;
    ++clock_ms;
    advance_callbacks();
}

static s32 WC04_CancelIso(s32 handle, u8 endpoint)
{
    assert(handle == audio_id && endpoint == 0x84 && scenario == STALLED_CANCEL);
    assert(stopping && clock_ms - stopping_ms >= 100 && pending_count == ISO_QUEUE_DEPTH);
    ++cancels;
    cancel_ms = clock_ms;
    return 0;
}

static bool ensure_directory(const char *path)
{
    assert(!strcmp(path, "sd:/wc04-camera/audio"));
    return scenario != STORAGE_FAIL;
}

static void *test_malloc(size_t size)
{
    assert(size == MIC_BYTES);
    return scenario == ALLOC_FAIL ? NULL : malloc(size);
}

static void test_free(void *data)
{
    assert(!pending_count);
    ++frees;
    free(data);
}

#include "mic_fixture.h"
#define malloc test_malloc
#define free test_free
#define usleep test_usleep
#include "mic_startup_under_test.h"
#undef malloc
#undef free
#undef usleep

static void check_wav(unsigned number, bool concealed)
{
    char path[96];
    snprintf(path, sizeof(path), "sd:/wc04-camera/audio/MIC%04u.WAV", number);
    FILE *file = fopen(path, "rb");
    assert(file);
    uint8_t header[44];
    assert(fread(header, 1, sizeof(header), file) == sizeof(header));
    assert(!memcmp(header, "RIFF", 4) && !memcmp(header + 8, "WAVEfmt ", 8));
    assert(uvc_le32(header + 24) == MIC_RATE && uvc_le32(header + 40) == MIC_BYTES);
    unsigned silence = 0;
    for (unsigned byte = 0; byte < MIC_BYTES; ++byte) {
        int value = fgetc(file);
        assert(value == 0x20 || (concealed && value == 0));
        silence += value == 0;
    }
    assert(fgetc(file) == EOF && silence == (concealed ? 7u * 32u : 0));
    fclose(file);
}

int main(void)
{
    descriptor = microphone();
    assert(!mkdir("sd:", 0700));
    assert(!mkdir("sd:/wc04-camera", 0700));
    assert(!mkdir("sd:/wc04-camera/audio", 0700));
    unsigned saved = 0;
    for (scenario = 0; scenario < SCENARIO_COUNT; ++scenario) {
        clock_ms = stopping_ms = cancel_ms = 0;
        mounted = scenario != NO_SD;
        exit_requested = stopping = false;
        video_closes = audio_closes = opens = submissions = controls = cancels = 0;
        pending_count = callback_count = frees = rejected_injected = 0;
        log_size = 0;
        log_text[0] = 0;
        char status[80];
        microphone_test(320, 240, status, sizeof(status));
        assert(!pending_count && audio_closes == opens);
        assert(video_closes == (scenario == NO_CAMERA || scenario == NO_SD ? 0u : 1u));
        bool success = scenario == SUCCESS || scenario == OTHER_OK || scenario == DELAYED_PCM
                       || scenario == SPARSE_LOSS || scenario == NATURAL_CAPTURE;
        if (success) {
            char expected[40];
            snprintf(expected, sizeof(expected), "MIC%04u.WAV saved", ++saved);
            assert(strstr(status, expected) && submissions > 300 && frees == 1);
            check_wav(saved, scenario == SPARSE_LOSS);
        } else
            assert(!strstr(status, "WAV saved"));
        if (submissions) assert(controls == (other_camera() ? 2u : 1u) && frees == 1);
        if (scenario >= NO_CAMERA && scenario <= SET_FAIL) assert(!submissions);
        if (scenario == VIDEO_CHANGED) {
            assert(!opens && !submissions && !frees);
            assert(!strcmp(status, "Restart WiiCam to test the microphone"));
            assert(strstr(log_text, "Microphone test skipped"));
        }
        if (scenario >= OTHER_RATE_MISMATCH && scenario <= OTHER_GET_FAIL) assert(!submissions);
        if (scenario == USER_CANCEL || scenario == USER_EXIT || scenario == NATURAL_CANCEL
            || scenario == STALLED_CANCEL)
            assert(strstr(status, "cancelled"));
        if (scenario == NO_PCM || scenario == EMPTY_PCM) {
            assert(strstr(log_text, "Microphone stop reason=5 failure=0"));
            assert(strstr(log_text, "actual_PCM_bytes=0 silent_bytes=0"));
            assert(stopping_ms >= 500 && stopping_ms < 550 && !cancels);
        }
        if (scenario == DELAYED_PCM) assert(rejected_injected == 128);
        if (scenario == SPARSE_LOSS) assert(rejected_injected > MIC_MAX_REJECTED_PACKETS);
        if (scenario == PCM_STALL) {
            assert(strstr(log_text, "Microphone stop reason=5 failure=0"));
            assert(strstr(log_text, "actual_PCM_bytes=5120 silent_bytes=0"));
            assert(stopping_ms >= 708 && stopping_ms < 724 && !cancels);
        }
        if (scenario >= PCM_48K && scenario <= PCM_8K)
            assert(strstr(log_text, "fatal=0") && strstr(log_text, "capture_valid=0"));
        if (scenario == ODD_SIZE || scenario == OVERSIZE || scenario == LOSS_BURST
            || scenario == LOSS_BUDGET || scenario == OTHER_STATUS)
            assert(strstr(log_text, "fatal=1") && !strstr(log_text, "capture_valid=1"));
        if (scenario == NATURAL_CAPTURE || scenario == NATURAL_CANCEL) {
            assert(!cancels && clock_ms - stopping_ms == 32 && callback_count == submissions);
            assert(strstr(log_text, "requests drained naturally"));
        }
        if (scenario == STALLED_CANCEL)
            assert(cancels == 1 && callback_count == ISO_QUEUE_DEPTH && clock_ms > cancel_ms);
    }
    assert(saved == 5);
    assert(access("sd:/wc04-camera/audio/MIC0006.WAV", F_OK) != 0);
    printf("microphone adapter: %u startup, recovery, rate, storage, timeout and drain scenarios passed\n",
           SCENARIO_COUNT);
    return 0;
}
