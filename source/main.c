/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */

#include <gccore.h>
#include <wiiuse/wpad.h>
#include <ogc/usb.h>
#include <ogc/lwp_watchdog.h>
#include <sdcard/wiisd_io.h>
#include <fat.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "photo.h"
#include "uvc.h"
#include "log.h"
#include "iso_queue.h"
#include "screen.h"
#include "preview.h"
#include "capture_control.h"
#include "recorder.h"
#include "storage.h"
#include "camera.h"
#include "refresh_control.h"
#include "microphone.h"

#define FRAME_TIMEOUT_SECONDS 10
#define CONFIG_CAPACITY CAMERA_CONFIG_CAPACITY
_Static_assert(UVC_PROBE_LENGTH <= 32, "probe must fit reserved MEM2 bank");

static uint8_t *config;
static uint8_t *probe;
static uint8_t *iso_data;
static uint16_t *iso_sizes;
static uint8_t frame_data[WC04_FRAME_CAPACITY];
static uint8_t rgb[WC04_RGB_CAPACITY];
static uint8_t decode_rgb[WC04_RGB_CAPACITY];
static uint32_t available_sizes = VIDEO_DEFAULT_SIZES;
static volatile bool exit_requested;
static bool mounted;
static FILE *log_file;
static uint64_t session_start;
static void *xfb[2];
static unsigned back_buffer;
static bool widescreen;
static uint8_t preview_jpeg[WC04_FRAME_CAPACITY];
static uint8_t worker_stack[64 * 1024] ATTRIBUTE_ALIGN(32);
static GXRModeObj *video;
static ScreenInfo screen_info;
static Recorder recording;

s32 WC04_SetAlternate(s32 device_id, u8 alternate);
s32 WC04_CancelIso(s32 device_id, u8 endpoint);
s32 WC04_GetInterfaceParameters(s32 device_id, u8 *parameters);

static void draw_screen_info(ScreenPage page, const uint8_t *pixels, unsigned width,
                             unsigned height, const ScreenInfo *info)
{
    Screen screen = {.pixels = xfb[back_buffer],
                     .width = video->fbWidth,
                     .height = video->xfbHeight,
                     .stride = VIDEO_PadFramebufferWidth(video->fbWidth),
                     .widescreen = widescreen};
    screen_draw(&screen, page, pixels, width, height, info);
    DCFlushRange(xfb[back_buffer], VIDEO_GetFrameBufferSize(video));
    VIDEO_SetNextFramebuffer(xfb[back_buffer]);
    VIDEO_Flush();
    /* Only the drawing thread waits; USB reception remains runnable. */
    do {
        VIDEO_WaitVSync();
    } while (VIDEO_GetCurrentFramebuffer() != xfb[back_buffer]);
    back_buffer ^= 1;
}

static void draw_screen(ScreenPage page, const uint8_t *pixels, unsigned width, unsigned height,
                        const char *status)
{
    ScreenInfo info = screen_info;
    info.sd = mounted;
    info.status = status;
    draw_screen_info(page, pixels, width, height, &info);
}

static bool allocate_usb_buffers(void)
{
    const size_t bytes = CONFIG_CAPACITY + 32 + ISO_QUEUE_DEPTH * ISO_DATA_BYTES
                         + ISO_QUEUE_DEPTH * ISO_PACKETS * sizeof(uint16_t);
    uintptr_t high = (uintptr_t)SYS_GetArena2Hi() & ~(uintptr_t)31;
    uintptr_t low = (uintptr_t)SYS_GetArena2Lo();
    if (high < low || high - low < bytes) return false;
    /* Reserve MEM2 before other subsystems start; leave the small IPC heap free. */
    config = (uint8_t *)(high - bytes);
    SYS_SetArena2Hi(config);
    probe = config + CONFIG_CAPACITY;
    iso_data = probe + 32;
    iso_sizes = (uint16_t *)(iso_data + ISO_QUEUE_DEPTH * ISO_DATA_BYTES);
    return true;
}

static void log_text(const char *text)
{
    if (log_file)
        log_message(log_file, time(NULL), ticks_to_millisecs(gettime() - session_start), text);
}

static void report(const char *format, ...)
{
    va_list args;
    if (log_file) {
        char message[1024];
        va_start(args, format);
        int length = vsnprintf(message, sizeof(message), format, args);
        va_end(args);
        if (length >= 0) log_text(message);
        if (length >= (int)sizeof(message)) log_text("Log message truncated.\n");
        fflush(log_file);
    }
}

static void log_hex(const uint8_t *data, unsigned length)
{
    for (unsigned offset = 0; offset < length; offset += 16) {
        char line[64];
        unsigned used = (unsigned)snprintf(line, sizeof(line), "%04x:", offset);
        for (unsigned i = offset; i < length && i < offset + 16; ++i)
            used += (unsigned)snprintf(line + used, sizeof(line) - used, " %02x", data[i]);
        log_text(line);
    }
}

static void request_exit(void)
{
    exit_requested = true;
}

static void remote_power(s32 channel)
{
    (void)channel;
    request_exit();
}

static void reset_pressed(u32 irq, void *context)
{
    (void)irq;
    (void)context;
    request_exit();
}

static uint32_t buttons(void)
{
    WPAD_ScanPads();
    uint32_t down = WPAD_ButtonsDown(0);
    if (down & WPAD_BUTTON_HOME) exit_requested = true;
    return down;
}

static s32 iso_finished(s32 result, void *context)
{
    iso_queue_complete(context, result);
    return 0;
}

static bool ensure_directory(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode);
    return mkdir(path, 0777) == 0;
}

static int camera_open(void *context, const CameraEntry *entry, int32_t *handle)
{
    (void)context;
    s32 device;
    int result = USB_OpenDevice(entry->id, entry->vid, entry->pid, &device);
    /* Negative vendor handles are valid; only the return code signals failure. */
    if (result >= 0) *handle = device;
    return result;
}

static int camera_parameters(void *context, int32_t handle, uint8_t *data)
{
    (void)context;
    return WC04_GetInterfaceParameters(handle, data);
}

static int camera_read(void *context, int32_t handle, uint8_t request, uint16_t value,
                       uint16_t length, uint8_t *data)
{
    (void)context;
    int result = USB_ReadCtrlMsg(handle, 0x80, request, value, 0, length, data);
    report("Descriptor/control request %u value %04x length %u: raw %d, payload %d\n", request,
           value, length, result, uvc_control_payload_length(result, length));
    return result;
}

static void camera_close(void *context, int32_t handle)
{
    (void)context;
    s32 device = handle;
    USB_CloseDevice(&device);
}

static void camera_report(void *context, const CameraEntry *entry, const char *message)
{
    (void)context;
    report("Candidate %04x:%04x interface %u: %s\n", entry->vid, entry->pid, entry->interface,
           message);
}

static bool find_camera(s32 *handle, unsigned width, unsigned height, UvcMode *mode)
{
    usb_device_entry devices[CAMERA_MAX_DEVICES];
    CameraEntry entries[CAMERA_MAX_DEVICES];
    u8 count = 0;
    s32 result = USB_GetDeviceList(devices, CAMERA_MAX_DEVICES, 0, &count);
    report("USB enumeration: %ld, interfaces: %u; actual bus speed unknown\n", (long)result, count);
    if (result < 0 || count > CAMERA_MAX_DEVICES) return false;
    for (unsigned i = 0; i < count; ++i)
        entries[i] = (CameraEntry){devices[i].device_id, devices[i].vid, devices[i].pid,
                                   (uint8_t)(devices[i].token >> 8)};
    const CameraOps ops = {.open = camera_open,
                           .parameters = camera_parameters,
                           .read = camera_read,
                           .close = camera_close,
                           .report = camera_report};
    int32_t selected;
    if (!camera_find(entries, count, width, height, config, CONFIG_CAPACITY, &ops, &selected,
                     mode)) {
        report(
            "No supported camera mode. Bounded UVC 1.0 MJPEG sizes through 848x480, interval-1 ISO only.\n");
        return false;
    }
    available_sizes = mode->supported_sizes;
    *handle = selected;
    report("Selected VC %u / VS %u / endpoint %02x / MJPEG format %u frame %u, %ux%u\n",
           mode->control_interface, mode->interface, mode->endpoint, mode->format, mode->frame,
           mode->width, mode->height);
    report("Descriptor default interval %lu; other-camera operation is unverified.\n",
           (unsigned long)mode->interval);
    return true;
}

typedef struct {
    RefreshControl control;
    unsigned width, height;
} RefreshJob;

static void *refresh_worker(void *context)
{
    RefreshJob *job = context;
    s32 handle;
    UvcMode mode;
    bool found = find_camera(&handle, job->width, job->height, &mode);
    if (found) USB_CloseDevice(&handle);
    refresh_control_complete(&job->control, found);
    return NULL;
}

static void refresh_camera(unsigned width, unsigned height, char *status, size_t capacity)
{
    RefreshJob job = {.width = width, .height = height};
    refresh_control_init(&job.control);
    ScreenInfo info = screen_info;
    info.sd = mounted;
    info.refreshing = true;
    info.refresh_phase = 0;
    info.status = status;
    refresh_status(status, capacity, REFRESH_BUSY, width, height);
    /* Present the loading circle before any potentially blocking USB call. */
    draw_screen_info(SCREEN_MENU, NULL, width, height, &info);
    lwp_t worker;
    if (LWP_CreateThread(&worker, refresh_worker, &job, worker_stack, sizeof(worker_stack), 60)
        < 0) {
        snprintf(status, capacity, "Refresh unavailable - press 2 to retry");
        draw_screen(SCREEN_MENU, NULL, width, height, status);
        return;
    }
    RefreshResult result = REFRESH_BUSY, previous = REFRESH_BUSY;
    while (result == REFRESH_BUSY || result == REFRESH_CANCELLING) {
        uint32_t down = buttons();
        result = refresh_control_poll(&job.control, exit_requested || (down & WPAD_BUTTON_B));
        if (result != REFRESH_BUSY && result != REFRESH_CANCELLING) break;
        unsigned phase = (unsigned)(ticks_to_millisecs(gettime()) / 100) & 7;
        if (phase != info.refresh_phase || result != previous) {
            info.refresh_phase = phase;
            refresh_status(status, capacity, result, width, height);
            draw_screen_info(SCREEN_MENU, NULL, width, height, &info);
        }
        previous = result;
        VIDEO_WaitVSync();
    }
    /* B/HOME defer cleanup until the IOS call returns; never kill a USB owner.
     * Navigation/repeated refresh presses were consumed, not queued for replay. */
    LWP_JoinThread(worker, NULL);
    refresh_status(status, capacity, result, width, height);
    report("Refresh camera source %ux%u (descriptors only): %s; stream not started.\n", width,
           height,
           result == REFRESH_CANCELLED ? "cancelled"
           : result == REFRESH_FOUND   ? "mode found"
                                       : "no supported mode found");
    if (!exit_requested) draw_screen(SCREEN_MENU, NULL, width, height, status);
}

typedef struct {
    s32 handle;
    uint8_t interface;
} ProbeContext;

static int probe_transfer(void *context, uint8_t request, uint16_t selector,
                          uint8_t data[UVC_PROBE_LENGTH])
{
    const ProbeContext *camera = context;
    const char *operation = request == UVC_SET_CUR   ? "SET_CUR"
                            : request == UVC_GET_MAX ? "GET_MAX"
                            : request == UVC_GET_DEF ? "GET_DEF"
                                                     : "GET_CUR";
    const char *control = selector == UVC_VS_PROBE ? "PROBE" : "COMMIT";
    report("%s %s (%u payload bytes):\n", operation, control, UVC_PROBE_LENGTH);
    s32 result;
    if (request == UVC_SET_CUR) {
        log_hex(data, UVC_PROBE_LENGTH);
        result = USB_WriteCtrlMsg(camera->handle, 0x21, request, selector, camera->interface,
                                  UVC_PROBE_LENGTH, data);
    } else {
        result = USB_ReadCtrlMsg(camera->handle, 0xa1, request, selector, camera->interface,
                                 UVC_PROBE_LENGTH, data);
    }
    /* IOS58 counts the eight-byte setup packet in control completions. */
    int length = uvc_control_payload_length(result, UVC_PROBE_LENGTH);
    report("%s %s result: %ld; payload count: %d\n", operation, control, (long)result, length);
    if (request != UVC_SET_CUR && length > 0)
        log_hex(data, length < UVC_PROBE_LENGTH ? (unsigned)length : UVC_PROBE_LENGTH);
    if (length == UVC_PROBE_LENGTH) {
        report("Probe fields: format %u frame %u interval %lu quality %u\n", data[2], data[3],
               (unsigned long)uvc_le32(data + 4), uvc_le16(data + 12));
        report("Frame limit %lu, payload limit %lu\n", (unsigned long)uvc_le32(data + 18),
               (unsigned long)uvc_le32(data + 22));
    }
    return length;
}

static bool negotiate(s32 handle, const UvcMode *mode, UvcAlternate *alt)
{
    s32 result = WC04_SetAlternate(handle, 0);
    if (result < 0) {
        report("Idle alternate failed: %ld\n", (long)result);
        return false;
    }
    ProbeContext camera = {.handle = handle, .interface = mode->interface};
    report("Read camera probe seed: GET_MAX, then GET_DEF/GET_CUR if unavailable.\n");
    if (!uvc_negotiate(mode, alt, probe, probe_transfer, &camera)) {
        report("Probe/commit failed, short, or returned unsupported values.\n");
        return false;
    }
    result = WC04_SetAlternate(handle, alt->number);
    report("Streaming alternate %u, endpoint %02x, capacity %u: %ld\n", alt->number, alt->endpoint,
           alt->capacity, (long)result);
    return result >= 0;
}

typedef struct {
    unsigned slot, length, offset, copied;
    uint64_t elapsed_ms;
    uint8_t data[32];
} PacketSample;

static void sample_packet(PacketSample *sample, const uint8_t *data, unsigned slot, unsigned length,
                          unsigned offset, uint64_t start)
{
    sample->slot = slot;
    sample->length = length;
    sample->offset = offset;
    sample->elapsed_ms = ticks_to_millisecs(gettime() - start);
    sample->copied = length < sizeof(sample->data) ? length : sizeof(sample->data);
    memcpy(sample->data, data + offset, sample->copied);
}

static void log_sample(const char *label, const PacketSample *sample)
{
    char line[160];
    snprintf(line, sizeof(line),
             "%s at capture +%llu ms: slot=%u size=%u offset=%u prefix=%u bytes", label,
             (unsigned long long)sample->elapsed_ms, sample->slot, sample->length, sample->offset,
             sample->copied);
    log_text(line);
    log_hex(sample->data, sample->copied);
}

typedef struct {
    s32 handle;
    const UvcMode *mode;
    const UvcAlternate *alt;
    UvcFrame frame;
    uint64_t start, last_batch, max_batch_gap;
    unsigned rejected, oversized, batches, min_length, max_length;
    PacketSample first[ISO_PACKETS], packed[ISO_PACKETS];
    PacketSample first_payload, first_error, largest;
    int first_result;
    Preview preview;
    atomic_bool worker_stop;
    atomic_uint last_decoded_ms;
    lwp_t worker;
    uint64_t last_input;
    unsigned decoded, skipped, displayed, first_decoded_ms, shutter_ms;
    uint64_t max_decode_time, max_video_write_time;
    CaptureControl control;
    atomic_uint countdown;
    RecordControl record_control;
    bool video_mode;
} Capture;

static void *preview_worker(void *context)
{
    Capture *capture = context;
    uint64_t last_draw = 0;
    while (!atomic_load_explicit(&capture->worker_stop, memory_order_acquire)) {
        bool have_frame = preview_take(&capture->preview);
        if (have_frame) {
            uint64_t decode_start = gettime();
            bool decoded = photo_decode(capture->preview.jpeg, capture->preview.length, decode_rgb,
                                        capture->mode->width, capture->mode->height);
            uint64_t decode_time = gettime() - decode_start;
            if (decode_time > capture->max_decode_time) capture->max_decode_time = decode_time;
            if (decoded) {
                memcpy(rgb, decode_rgb, (size_t)capture->mode->width * capture->mode->height * 3);
                ++capture->decoded;
                unsigned elapsed = (unsigned)ticks_to_millisecs(gettime() - capture->start);
                if (capture->decoded == 1) capture->first_decoded_ms = elapsed;
                atomic_store_explicit(&capture->last_decoded_ms, elapsed, memory_order_release);
                uint64_t write_start = gettime();
                unsigned state =
                    atomic_load_explicit(&capture->record_control.state, memory_order_acquire);
                if (record_control_begin(&capture->record_control)) {
                    bool ready =
                        ensure_directory("sd:/wc04-camera/videos")
                        && recorder_start(&recording, "sd:/wc04-camera/videos",
                                          capture->mode->width, capture->mode->height, elapsed);
                    if (!ready) recording.failed = true;
                    record_control_started(&capture->record_control, ready, elapsed);
                    state = atomic_load(&capture->record_control.state);
                }
                if (state == RECORD_ACTIVE
                    && !recorder_frame(&recording, capture->preview.jpeg, capture->preview.length,
                                       elapsed))
                    record_control_result(&capture->record_control,
                                          recording.limited ? RECORD_LIMIT : RECORD_FAILED);
                uint64_t write_time = gettime() - write_start;
                if (state != RECORD_READY && write_time > capture->max_video_write_time)
                    capture->max_video_write_time = write_time;
            } else
                ++capture->rejected;
            preview_finish(&capture->preview, decoded);
        }
        if (atomic_load_explicit(&capture->worker_stop, memory_order_acquire)) break;
        uint64_t now = gettime();
        if (!last_draw || ticks_to_millisecs(now - last_draw) >= 66) {
            unsigned elapsed = (unsigned)ticks_to_millisecs(now - capture->start);
            unsigned last = atomic_load(&capture->last_decoded_ms);
            unsigned age = elapsed >= last ? elapsed - last : 0;
            bool signal = last && age < 1000;
            ScreenInfo info = screen_info;
            info.sd = mounted;
            info.status = "";
            info.countdown = atomic_load(&capture->countdown);
            info.shutter = atomic_load(&capture->preview.shutter);
            unsigned state = atomic_load(&capture->record_control.state);
            info.recording = state == RECORD_ACTIVE;
            info.seconds = info.recording
                               ? (elapsed - atomic_load(&capture->record_control.started_ms)) / 1000
                               : 0;
            if (state == RECORD_REQUESTED || state == RECORD_STARTING)
                info.status = "Starting video...";
            draw_screen_info(signal ? SCREEN_LIVE : SCREEN_NO_SIGNAL, signal ? rgb : NULL,
                             capture->mode->width, capture->mode->height, &info);
            last_draw = now;
            ++capture->displayed;
        }
        if (!have_frame) usleep(1000);
    }
    /* Only this worker writes the video; finalize before its join returns. */
    if (recording.active)
        recorder_stop(&recording, atomic_load(&capture->record_control.stopped_ms));
    return NULL;
}

static const char *stop_description(IsoStop stop)
{
    switch (stop) {
    case ISO_PHOTO_READY:
        return "shutter photo decoded";
    case ISO_RECORD_STOPPED:
        return "video stopped or limit/write failure";
    case ISO_CANCELLED:
        return "B pressed";
    case ISO_EXIT:
        return "HOME/power/reset exit requested";
    case ISO_TIMEOUT:
        return "no decoded frame for 10 seconds";
    case ISO_SUBMIT_FAILED:
        return "USB submission failure";
    case ISO_COMPLETION_FAILED:
        return "USB completion failure before cancellation";
    default:
        return "capture running";
    }
}

static IsoStop poll_capture(void *context)
{
    Capture *capture = context;
    uint64_t now = gettime();
    uint32_t down = 0;
    if (ticks_to_millisecs(now - capture->last_input) >= 10) {
        down = buttons();
        capture->last_input = now;
    }
    unsigned elapsed = (unsigned)ticks_to_millisecs(now - capture->start);
    unsigned last = atomic_load_explicit(&capture->last_decoded_ms, memory_order_acquire);
    unsigned age = elapsed >= last ? elapsed - last : 0;
    bool signal = last && age < 1000;
    if (capture->video_mode) {
        if (exit_requested) return ISO_EXIT;
        if (down & WPAD_BUTTON_B) return ISO_CANCELLED;
        unsigned state = atomic_load(&capture->record_control.state);
        unsigned started = atomic_load(&capture->record_control.started_ms);
        if (state == RECORD_FAILED || state == RECORD_LIMIT
            || (state == RECORD_ACTIVE && elapsed >= started && elapsed - started >= AVI_MAX_MS))
            return ISO_RECORD_STOPPED;
        if (down & WPAD_BUTTON_A) {
            if (state != RECORD_READY) return ISO_RECORD_STOPPED;
            if (mounted && signal) record_control_request(&capture->record_control);
        }
    } else {
        CaptureAction action =
            capture_control_update(&capture->control, elapsed, down & WPAD_BUTTON_A,
                                   down & WPAD_BUTTON_B, exit_requested, signal, mounted);
        atomic_store(&capture->countdown, capture->control.remaining);
        if (action == CONTROL_EXIT) return ISO_EXIT;
        if (action == CONTROL_SIGNAL_LOST) return ISO_TIMEOUT;
        if (action == CONTROL_BACK) return ISO_CANCELLED;
        if (action == CONTROL_PHOTO) {
            capture->shutter_ms = elapsed;
            preview_shutter(&capture->preview);
        }
        if (preview_photo_ready(&capture->preview)) return ISO_PHOTO_READY;
    }
    if (age >= FRAME_TIMEOUT_SECONDS * 1000) return ISO_TIMEOUT;
    return ISO_RUNNING;
}

static void idle_capture(void *context)
{
    (void)context;
    usleep(100);
}

static void stopping_capture(void *context, IsoStop reason, int failure)
{
    Capture *capture = context;
    record_control_stop(&capture->record_control,
                        (unsigned)ticks_to_millisecs(gettime() - capture->start));
    atomic_store_explicit(&capture->worker_stop, true, memory_order_release);
    LWP_JoinThread(capture->worker, NULL);
    draw_screen(SCREEN_WAITING, reason == ISO_PHOTO_READY ? rgb : NULL, capture->mode->width,
                capture->mode->height,
                capture->video_mode ? "Stopping camera / saving video" : "Stopping camera");
    report("Capture stop requested: %s.\n", stop_description(reason));
    if (reason == ISO_SUBMIT_FAILED || reason == ISO_COMPLETION_FAILED)
        report("USB failure result: %d\n", failure);
}

static void cancel_capture(void *context)
{
    const Capture *capture = context;
    draw_screen(SCREEN_WAITING, NULL, capture->mode->width, capture->mode->height,
                "If stuck here, unplug camera");
    s32 result = WC04_CancelIso(capture->handle, capture->alt->endpoint);
    report("Cancel endpoint: %ld. Waiting for all queued USB completions.\n", (long)result);
    report("If stuck, unplug camera.\n");
}

static int submit_capture(void *context, IsoSlot *slot)
{
    const Capture *capture = context;
    return USB_ReadIsoMsgAsync(capture->handle, capture->alt->endpoint, ISO_PACKETS, slot->sizes,
                               slot->data, iso_finished, slot);
}

static bool consume_capture(void *context, const IsoSlot *slot)
{
    Capture *capture = context;
    const UvcAlternate *alt = capture->alt;
    UvcFrame *frame = &capture->frame;
    uint64_t now = gettime();
    if (capture->batches && now - capture->last_batch > capture->max_batch_gap)
        capture->max_batch_gap = now - capture->last_batch;
    capture->last_batch = now;
    ++capture->batches;
    if (capture->batches == 1) {
        capture->first_result = slot->result;
        unsigned packed_offset = 0;
        bool packed_valid = true;
        for (unsigned i = 0; i < ISO_PACKETS; ++i) {
            unsigned length = slot->sizes[i];
            unsigned bounded = length < alt->capacity ? length : alt->capacity;
            sample_packet(&capture->first[i], slot->data, i, bounded, i * alt->capacity,
                          capture->start);
            capture->first[i].length = length;
            if (length > alt->capacity) packed_valid = false;
            if (packed_valid) {
                sample_packet(&capture->packed[i], slot->data, i, length, packed_offset,
                              capture->start);
                packed_offset += length;
            } else
                capture->packed[i].offset = UINT32_MAX;
        }
    }
    for (unsigned i = 0; i < ISO_PACKETS; ++i) {
        unsigned length = slot->sizes[i];
        if (length > alt->capacity) {
            frame->discard = true;
            ++capture->oversized;
            continue;
        }
        unsigned offset = i * alt->capacity;
        const uint8_t *packet = slot->data + offset;
        if (length < capture->min_length) capture->min_length = length;
        if (length > capture->max_length) {
            capture->max_length = length;
            sample_packet(&capture->largest, slot->data, i, length, offset, capture->start);
        }
        if (length >= 2) {
            if (!capture->first_payload.copied && packet[0] >= 2 && packet[0] < length)
                sample_packet(&capture->first_payload, slot->data, i, length, offset,
                              capture->start);
            if (!capture->first_error.copied && (packet[1] & 0x40))
                sample_packet(&capture->first_error, slot->data, i, length, offset, capture->start);
        }
        /* IOS retains requested strides; returned sizes are actual lengths. */
        if (uvc_packet(frame, packet, length)) {
            if (!preview_offer(&capture->preview, frame->data, frame->size)) ++capture->skipped;
        }
    }
    return false;
}

static IsoStop receive_photo(s32 handle, const UvcMode *mode, const UvcAlternate *alt)
{
    Capture capture = {.handle = handle,
                       .mode = mode,
                       .alt = alt,
                       .min_length = ISO_MAX_PAYLOAD,
                       .video_mode = screen_info.video};
    uvc_frame_init(&capture.frame, frame_data, sizeof(frame_data));
    IsoQueue queue;
    iso_queue_init(&queue, iso_data, iso_sizes, alt->capacity);
    const IsoHooks hooks = {.submit = submit_capture,
                            .consume = consume_capture,
                            .poll = poll_capture,
                            .idle = idle_capture,
                            .stopping = stopping_capture,
                            .cancel = cancel_capture};
    report(
        "Viewfinder with %u queued batches of %u slots. A captures/records, B stops, HOME exits.\n",
        ISO_QUEUE_DEPTH, ISO_PACKETS);
    preview_init(&capture.preview, preview_jpeg, sizeof(preview_jpeg));
    atomic_init(&capture.worker_stop, false);
    atomic_init(&capture.last_decoded_ms, 0);
    atomic_init(&capture.countdown, 0);
    record_control_init(&capture.record_control);
    capture_control_init(&capture.control, screen_info.timer);
    recorder_init(&recording);
    draw_screen(SCREEN_WAITING, NULL, mode->width, mode->height, "WAITING FOR FIRST FRAME");
    capture.start = gettime();
    if (LWP_CreateThread(&capture.worker, preview_worker, &capture, worker_stack,
                         sizeof(worker_stack), 60)
        < 0) {
        report("Could not start preview worker.\n");
        return ISO_SUBMIT_FAILED;
    }
    IsoStop stop = iso_queue_run(&queue, &hooks, &capture);
    const UvcFrame *frame = &capture.frame;
    const UvcStats *stats = &frame->stats;
    report("Capture ended: %s; elapsed %llu ms (including drain).\n", stop_description(stop),
           (unsigned long long)ticks_to_millisecs(gettime() - capture.start));
    report("Queue submitted %u, consumed %u, drained %u; max consume gap %llu us\n",
           queue.submitted, queue.consumed, queue.drained,
           (unsigned long long)ticks_to_microsecs(capture.max_batch_gap));
    for (unsigned i = 0; i < queue.drained; ++i)
        report("Completion drained after stop: %d\n", queue.drain_results[i]);
    report("Batches %u, slots %u, bytes %u, length %u..%u\n", capture.batches, stats->packets,
           stats->bytes, stats->packets ? capture.min_length : 0, capture.max_length);
    report("Empty %u, header-only %u, payload %u (%u bytes)\n", stats->empty, stats->header_only,
           stats->payload_packets, stats->payload_bytes);
    report("Bad headers %u, ERR %u, EOF %u, FID changes %u\n", stats->bad_headers, stats->errors,
           stats->eof, stats->fid_changes);
    report("Dropped %u: missing EOF %u, empty frames %u\n", frame->dropped, stats->missing_eof,
           stats->empty_frames);
    report("Invalid JPEG %u, overflow %u\n", stats->invalid_jpeg, stats->overflow);
    report("JPEG boundary reasons (overlap): short=%u missing_soi=%u missing_terminal_eoi=%u\n",
           stats->short_jpeg, stats->missing_soi, stats->missing_eoi);
    report("JPEG normalized zero padding: frames=%u bytes=%u (aligned, max 7 bytes)\n",
           stats->normalized_frames, stats->padding_bytes);
    report("JPEG accepted assembled size mod8 [0..7]: %u %u %u %u %u %u %u %u\n",
           stats->accepted_mod8[0], stats->accepted_mod8[1], stats->accepted_mod8[2],
           stats->accepted_mod8[3], stats->accepted_mod8[4], stats->accepted_mod8[5],
           stats->accepted_mod8[6], stats->accepted_mod8[7]);
    report("JPEG rejected size mod8 [0..7]: %u %u %u %u %u %u %u %u\n", stats->rejected_mod8[0],
           stats->rejected_mod8[1], stats->rejected_mod8[2], stats->rejected_mod8[3],
           stats->rejected_mod8[4], stats->rejected_mod8[5], stats->rejected_mod8[6],
           stats->rejected_mod8[7]);
    report("JPEG candidates %u, decode rejected %u, oversized slots %u\n", stats->complete,
           capture.rejected, capture.oversized);
    report("Unfinished frame: %u bytes, discard=%u, ended=%u\n", (unsigned)frame->size,
           frame->discard, frame->ended);
    if (stats->header_only && !stats->payload_bytes && !stats->bad_headers)
        report("Only UVC headers received; no image payload reached the decoder.\n");
    /* All queued transfers have returned ownership before diagnostic I/O. */
    if (log_file && capture.batches) {
        report("JPEG rejected boundary samples: %u/%u; reasons bits short=1 noSOI=2 noEOI=4.\n",
               frame->boundary_samples, UVC_BOUNDARY_SAMPLES);
        for (unsigned i = 0; i < frame->boundary_samples; ++i) {
            const UvcBoundarySample *sample = &frame->boundary[i];
            report(
                "Rejected JPEG %u: size=%lu mod8=%u reasons=%u EOF_header=%u flags=%02x raw_EOI_tail_gap=%d\n",
                i, (unsigned long)sample->size, (unsigned)(sample->size & 7), sample->reasons,
                sample->header_length, sample->flags, sample->eoi_distance);
            log_text("Rejected JPEG prefix (up to 8 bytes):");
            log_hex(sample->head, sample->head_length);
            log_text("Rejected JPEG suffix (up to 16 bytes):");
            log_hex(sample->tail, sample->tail_length);
        }
        log_text(
            "raw_EOI_tail_gap is a byte-pair hint in the final 16 bytes, -1 absent; validation is unchanged.");
        report("First batch completion: %d; packet samples in SD log.\n", capture.first_result);
        for (unsigned i = 0; i < ISO_PACKETS; ++i) {
            log_sample("Requested-stride sample", &capture.first[i]);
            if (capture.packed[i].offset != UINT32_MAX)
                log_sample("Packed-offset comparison (not parsed)", &capture.packed[i]);
            else
                log_text("Packed comparison unavailable after invalid returned size.\n");
        }
        if (capture.largest.copied) log_sample("Largest packet", &capture.largest);
        if (capture.first_payload.copied)
            log_sample("First apparent payload", &capture.first_payload);
        if (capture.first_error.copied) log_sample("First apparent ERR flag", &capture.first_error);
        fflush(log_file);
    }
    report("Preview decoded %u, skipped while busy %u.\n", capture.decoded, capture.skipped);
    report("Preview displayed %u; first decode +%u ms; longest decode %llu us\n", capture.displayed,
           capture.first_decoded_ms,
           (unsigned long long)ticks_to_microsecs(capture.max_decode_time));
    if (atomic_load(&capture.preview.shutter))
        report("Shutter pressed at viewfinder +%u ms\n", capture.shutter_ms);
    if (capture.video_mode) {
        report("Longest video create/write %llu us; busy preview frames skipped %u\n",
               (unsigned long long)ticks_to_microsecs(capture.max_video_write_time),
               capture.skipped);
        report("Video frames %u, bytes %u, duration %u ms, saved=%u, failed=%u, limit=%u\n",
               recording.avi.frames, recording.avi.bytes, recording.duration_ms, recording.saved,
               recording.failed, recording.limited);
        if (recording.saved)
            report(
                "Video saved: %s; sampled average rate %.2f fps (not a camera rate guarantee).\n",
                recording.path, recording.avi.frames * 1000.0 / recording.duration_ms);
        else if (recording.failed)
            report("Video SD creation/write failed. No completed clip.\n");
    }
    return stop;
}

typedef struct {
    s32 handle;
    UacMode mode;
    MicPcm pcm;
    CameraEntry entry;
    uint64_t start, last_input, first_packet, last_packet;
    unsigned width, height;
} MicTest;

static int mic_control(void *context, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                       uint8_t *data, unsigned length)
{
    const MicTest *test = context;
    s32 result = type & 0x80
                     ? USB_ReadCtrlMsg(test->handle, type, request, value, index, length, data)
                     : USB_WriteCtrlMsg(test->handle, type, request, value, index, length, data);
    int payload = uvc_control_payload_length(result, length);
    report("Microphone control type=%02x request=%02x endpoint=%02x raw=%ld payload=%d\n", type,
           request, index, (long)result, payload);
    return payload;
}

static int mic_submit(void *context, IsoSlot *slot)
{
    const MicTest *test = context;
    return USB_ReadIsoMsgAsync(test->handle, test->mode.endpoint, ISO_PACKETS, slot->sizes,
                               slot->data, iso_finished, slot);
}

static bool mic_consume(void *context, const IsoSlot *slot)
{
    MicTest *test = context;
    for (unsigned n = 0; n < ISO_PACKETS; ++n) {
        size_t before = test->pcm.size;
        if (!microphone_pcm_packet(&test->pcm, slot->data + n * test->mode.packet_capacity,
                                   slot->sizes[n], test->mode.packet_capacity))
            return true;
        if (test->pcm.size > before) {
            test->last_packet = gettime();
            if (!test->first_packet) test->first_packet = test->last_packet;
        }
        if (test->pcm.size == MIC_BYTES) return true;
    }
    return false;
}

static IsoStop mic_poll(void *context)
{
    MicTest *test = context;
    uint64_t now = gettime();
    uint32_t down = 0;
    if (ticks_to_millisecs(now - test->last_input) >= 10) {
        down = buttons();
        test->last_input = now;
    }
    if (exit_requested) return ISO_EXIT;
    if (down & WPAD_BUTTON_B) return ISO_CANCELLED;
    if (ticks_to_millisecs(now - test->start) >= 10000) return ISO_TIMEOUT;
    return ISO_RUNNING;
}

static void mic_stopping(void *context, IsoStop reason, int failure)
{
    const MicTest *test = context;
    draw_screen(SCREEN_WAITING, NULL, test->width, test->height, "Stopping microphone");
    report("Microphone stop reason=%u failure=%d; draining before saving.\n", reason, failure);
}

static void mic_cancel(void *context)
{
    const MicTest *test = context;
    draw_screen(SCREEN_WAITING, NULL, test->width, test->height, "If stuck here, unplug camera");
    s32 result = WC04_CancelIso(test->handle, test->mode.endpoint);
    report("Microphone cancel endpoint=%02x result=%ld; waiting for all completions.\n",
           test->mode.endpoint, (long)result);
}

static bool mic_open(s32 video_handle, const UvcMode *video_mode, MicTest *test)
{
    uint8_t reference[CONFIG_CAPACITY];
    unsigned length = uvc_le16(config + 2);
    if (length < 9 || length > sizeof(reference) || !microphone_mode(config, length, &test->mode)) {
        report("Microphone test requires one unambiguous UAC1 mono PCM16 16000 Hz mode.\n");
        return false;
    }
    memcpy(reference, config, length);
    usb_device_entry devices[CAMERA_MAX_DEVICES];
    MicEntry entries[CAMERA_MAX_DEVICES];
    u8 count = 0;
    if (USB_GetDeviceList(devices, CAMERA_MAX_DEVICES, 0, &count) < 0 || count > CAMERA_MAX_DEVICES)
        return false;
    for (unsigned n = 0; n < count; ++n) {
        entries[n] = (MicEntry){{devices[n].device_id, devices[n].vid, devices[n].pid,
                                 (uint8_t)(devices[n].token >> 8)},
                                devices[n].token};
        report("Microphone pairing: id=%08lx token=%08lx vid=%04x pid=%04x interface=%u\n",
               (unsigned long)(uint32_t)devices[n].device_id, (unsigned long)devices[n].token,
               devices[n].vid, devices[n].pid, entries[n].camera.interface);
    }
    size_t selected;
    if (!microphone_match(entries, count, video_handle, video_mode->interface, test->mode.interface,
                          &selected)) {
        report("Microphone pairing absent or ambiguous; no audio interface opened.\n");
        return false;
    }
    int32_t opened;
    const CameraEntry *entry = &entries[selected].camera;
    if (camera_open(NULL, entry, &opened) < 0) return false;
    bool valid = camera_parameters(NULL, opened, config) == 0
                 && microphone_parameters(entry, config, reference[5]);
    if (valid) {
        memset(config, 0, CONFIG_CAPACITY);
        valid = uvc_control_payload_length(camera_read(NULL, opened, 8, 0, 1, config), 1) == 1
                && config[0] == reference[5];
    }
    if (valid) {
        memset(config, 0, CONFIG_CAPACITY);
        valid =
            uvc_control_payload_length(camera_read(NULL, opened, 6, 0x0200, length, config), length)
                == (int)length
            && !memcmp(config, reference, length);
    }
    if (!valid) {
        report("Microphone IOS identity/interface or complete configuration mismatch.\n");
        camera_close(NULL, opened);
        return false;
    }
    test->handle = opened;
    test->entry = *entry;
    return true;
}

static void microphone_test(unsigned width, unsigned height, char *status, size_t capacity)
{
    snprintf(status, capacity, "Microphone unavailable - see log");
    if (!mounted) {
        snprintf(status, capacity, "Microphone test needs an SD card");
        return;
    }
    draw_screen(SCREEN_WAITING, NULL, width, height, "Connecting microphone");
    s32 video_handle;
    UvcMode video_mode;
    if (!find_camera(&video_handle, width, height, &video_mode)) return;
    MicTest test = {.width = width, .height = height};
    bool opened = mic_open(video_handle, &video_mode, &test);
    USB_CloseDevice(&video_handle);
    if (!opened) return;
    uint8_t *samples = malloc(MIC_BYTES);
    microphone_pcm_init(&test.pcm, samples, MIC_BYTES);
    MicRateResult rate = MIC_RATE_FAILED;
    if (samples && WC04_SetAlternate(test.handle, 0) >= 0
        && WC04_SetAlternate(test.handle, test.mode.alternate) >= 0)
        rate = microphone_set_rate(&test.entry, &test.mode, mic_control, &test, probe);
    if (rate == MIC_RATE_UNREADABLE) test.pcm.rejection_budget = MIC_MAX_REJECTED_PACKETS;
    if (rate == MIC_RATE_UNREADABLE)
        report("Microphone 0c45:6366 rate readback unavailable (-7102) after exact SET; "
               "diagnostic capture requires 4500..5500 ms packet span before WAV save.\n");
    if (rate != MIC_RATE_FAILED && !exit_requested) {
        report("Microphone test: interface=%u alt=%u endpoint=%02x PCM16 mono rate=%u "
               "packet=%u interval=%u; bus speed unknown; video idle.\n",
               test.mode.interface, test.mode.alternate, test.mode.endpoint, MIC_RATE,
               test.mode.packet_capacity, test.mode.interval);
        draw_screen(SCREEN_WAITING, NULL, width, height,
                    "Recording microphone for 5 seconds - B cancels");
        IsoQueue queue;
        iso_queue_init(&queue, iso_data, iso_sizes, test.mode.packet_capacity);
        const IsoHooks hooks = {.submit = mic_submit,
                                .consume = mic_consume,
                                .poll = mic_poll,
                                .idle = idle_capture,
                                .stopping = mic_stopping,
                                .cancel = mic_cancel};
        test.start = gettime();
        IsoStop stop = iso_queue_run(&queue, &hooks, &test);
        unsigned elapsed = (unsigned)ticks_to_millisecs(gettime() - test.start);
        report("Microphone result: bytes=%u samples=%u packets=%u empty=%u lengths=%u..%u "
               "peak=%u fatal=%u elapsed_ms=%u first_packet_ms=%llu last_packet_ms=%llu\n",
               (unsigned)test.pcm.size, (unsigned)test.pcm.size / 2, test.pcm.packets,
               test.pcm.empty, test.pcm.packets ? test.pcm.smallest : 0, test.pcm.largest,
               test.pcm.peak, test.pcm.failed, elapsed,
               (unsigned long long)(test.first_packet
                                        ? ticks_to_millisecs(test.first_packet - test.start)
                                        : 0),
               (unsigned long long)(test.last_packet
                                        ? ticks_to_millisecs(test.last_packet - test.start)
                                        : 0));
        report("Microphone packet rejection: rejected=%u budget=%u; invalid slots never copied.\n",
               test.pcm.rejected, test.pcm.rejection_budget);
        for (unsigned n = 0; n < test.pcm.rejected && n < MIC_MAX_REJECTED_PACKETS; ++n)
            report("Microphone rejected packet: index=%u slot=%u length=%u cap=%u odd=%u\n",
                   test.pcm.rejected_indices[n], test.pcm.rejected_indices[n] % ISO_PACKETS,
                   (unsigned)test.pcm.rejected_lengths[n], test.mode.packet_capacity,
                   (unsigned)(test.pcm.rejected_lengths[n] & 1));
        report("Microphone queue: submitted=%u consumed=%u drained=%u failure=%d\n",
               queue.submitted, queue.consumed, queue.drained, queue.failure);
        uint64_t span_ms = test.first_packet && test.last_packet >= test.first_packet
                               ? ticks_to_millisecs(test.last_packet - test.first_packet)
                               : 0;
        bool plausible = microphone_capture_valid(rate, &test.pcm, span_ms);
        report("Microphone rate check: readback_unavailable=%u span_ms=%llu "
               "approx_samples_per_second=%llu capture_valid=%u\n",
               rate == MIC_RATE_UNREADABLE, (unsigned long long)span_ms,
               (unsigned long long)(span_ms ? test.pcm.size * 500ull / span_ms : 0), plausible);
        bool success = stop == ISO_PHOTO_READY && !exit_requested && plausible;
        if (success && ensure_directory("sd:/wc04-camera/audio")) {
            char path[128];
            FILE *file = storage_create("sd:/wc04-camera/audio", "MIC", "WAV", path, sizeof(path));
            success = file && storage_finish(file, path, microphone_wav(file, &test.pcm));
            if (success) {
                report("Microphone WAV saved: %s; 5 seconds by sample count; listen to verify.\n",
                       path);
                snprintf(status, capacity, "%s saved - verify audio", strrchr(path, '/') + 1);
            }
        } else
            success = false;
        if (!success)
            snprintf(status, capacity, "%s",
                     stop == ISO_CANCELLED || stop == ISO_EXIT ? "Microphone test cancelled"
                                                               : "Microphone not saved - see log");
    } else
        report("Microphone setup failed or cancelled; no recording started.\n");
    s32 stopped = WC04_SetAlternate(test.handle, 0);
    report("Microphone idle alternate: %ld\n", (long)stopped);
    USB_CloseDevice(&test.handle);
    free(samples);
}

static bool save_photo(unsigned width, unsigned height, char path[80])
{
    if (!mounted || !ensure_directory("sd:/DCIM") || !ensure_directory("sd:/DCIM/100WC04")) {
        report("SD photo directory unavailable.\n");
        return false;
    }
    FILE *file = storage_create("sd:/DCIM/100WC04", "WC04", "JPG", path, 80);
    if (!file) {
        report("Photo creation failed: %s\n", strerror(errno));
        return false;
    }
    bool ok = storage_finish(file, path, photo_encode(file, rgb, width, height));
    if (!ok) report("JPEG write failed; incomplete-file cleanup attempted.\n");
    return ok;
}

int main(void)
{
    session_start = gettime();
    bool buffers_ready = allocate_usb_buffers();
    VIDEO_Init();
    video = VIDEO_GetPreferredMode(NULL);
    xfb[0] = SYS_AllocateFramebuffer(video);
    xfb[1] = SYS_AllocateFramebuffer(video);
    if (!xfb[0] || !xfb[1]) return 1;
    widescreen = CONF_GetAspectRatio() == CONF_ASPECT_16_9;
    VIDEO_Configure(video);
    VIDEO_SetBlack(FALSE);
    LWP_SetThreadPriority(LWP_GetSelf(), 80);
    WPAD_Init();
    SYS_SetResetCallback(reset_pressed);
    SYS_SetPowerCallback(request_exit);
    WPAD_SetPowerButtonCallback(remote_power);
    mounted = fatMountSimple("sd", &__io_wiisd);
    if (mounted && ensure_directory("sd:/wc04-camera"))
        log_file = fopen("sd:/wc04-camera/session.log", "a");
    report("\nWiiCam 0.1.2 - IOS %u\n", IOS_GetVersion());
    report("Log timestamps: Wii clock and monotonic seconds since app start.\n");
    report(
        "Bounded sizes and explicit 5-second microphone test enabled. Video recording is silent.\n");
    bool usb_ready = false;
    if (IOS_GetVersion() == 58)
        usb_ready = buffers_ready && USB_Initialize() >= 0;
    else
        report("Launch under IOS58. This app does not install/reload IOS.\n");
    unsigned width = 320, height = 240;
    char status[80];
    snprintf(status, sizeof(status), "%s",
             !usb_ready ? "Camera needs IOS58 and USB"
             : mounted  ? "Open viewfinder to start the camera."
                        : "No SD - preview only");
    bool menu = true, open_live = false;
    ScreenPage idle_page = SCREEN_MENU;
    while (!exit_requested) {
        if (menu) {
            draw_screen(idle_page, NULL, width, height, status);
            menu = false;
        }
        uint32_t down = buttons();
        if (exit_requested) break;
        if (idle_page == SCREEN_NO_SIGNAL && (down & WPAD_BUTTON_B)) {
            idle_page = SCREEN_MENU;
            snprintf(status, sizeof(status), "Open viewfinder to start the camera.");
            menu = true;
            down = 0;
        }
        if (idle_page == SCREEN_MENU && (down & WPAD_BUTTON_PLUS)) {
            if (screen_info.microphone)
                screen_info.microphone = false;
            else if (screen_info.video) {
                screen_info.video = false;
                screen_info.microphone = true;
            } else
                screen_info.video = true;
            snprintf(status, sizeof(status), "%s",
                     screen_info.microphone ? "A records a 5-second microphone test"
                                            : "Open viewfinder to start the camera.");
            menu = true;
        }
        if (idle_page == SCREEN_MENU && !screen_info.microphone
            && (down & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT))) {
            screen_info.timer =
                capture_timer_next(screen_info.timer, down & WPAD_BUTTON_LEFT ? -1 : 1);
            menu = true;
        }
        if (idle_page == SCREEN_MENU && !screen_info.microphone && (down & WPAD_BUTTON_1)) {
            VideoSize next = video_size_next(width, height, available_sizes);
            width = next.width;
            height = next.height;
            snprintf(status, sizeof(status), "%ux%u selected - 2 refreshes camera source", width,
                     height);
            menu = true;
        }
        if (usb_ready && (open_live || (down & (WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_2)))) {
            bool scanning = !open_live && !(down & (WPAD_BUTTON_A | WPAD_BUTTON_B));
            open_live = false;
            if (scanning) {
                refresh_camera(width, height, status, sizeof(status));
                idle_page = SCREEN_MENU;
                menu = false;
                continue;
            }
            if (screen_info.microphone) {
                if (down & WPAD_BUTTON_A) microphone_test(width, height, status, sizeof(status));
                idle_page = SCREEN_MENU;
                menu = true;
                continue;
            }
            draw_screen(SCREEN_WAITING, NULL, width, height, "CONNECTING CAMERA");
            s32 handle;
            idle_page = SCREEN_NO_SIGNAL;
            recorder_init(&recording);
            snprintf(status, sizeof(status), "Camera not found - A retries, B menu");
            UvcMode mode;
            if (find_camera(&handle, width, height, &mode)) {
                UvcAlternate alt;
                IsoStop result = ISO_SUBMIT_FAILED;
                if (negotiate(handle, &mode, &alt)) result = receive_photo(handle, &mode, &alt);
                s32 stopped = WC04_SetAlternate(handle, 0);
                report("Stop streaming: %ld\n", (long)stopped);
                snprintf(status, sizeof(status), "%s",
                         result == ISO_CANCELLED ? "Open viewfinder to start the camera."
                         : result == ISO_TIMEOUT ? "No frames - A retries, B menu"
                                                 : "Camera stopped - A retries, B menu");
                USB_CloseDevice(&handle);
                if (result == ISO_CANCELLED || result == ISO_RECORD_STOPPED
                    || result == ISO_PHOTO_READY)
                    idle_page = SCREEN_MENU;
                if (screen_info.video) {
                    if (recording.saved)
                        snprintf(status, sizeof(status), "Saved %s",
                                 strrchr(recording.path, '/') + 1);
                    else if (recording.failed)
                        snprintf(status, sizeof(status), "Video not saved - check SD and log");
                    else if (result == ISO_RECORD_STOPPED)
                        snprintf(status, sizeof(status), "No video frames recorded");
                }
                if (result == ISO_PHOTO_READY) {
                    char path[80];
                    draw_screen(SCREEN_REVIEW, rgb, width, height, "SAVING PHOTO");
                    bool saved = save_photo(width, height, path);
                    if (saved) {
                        report("Saved %s\n", path);
                        snprintf(status, sizeof(status), "SAVED %s", strrchr(path, '/') + 1);
                    } else
                        snprintf(status, sizeof(status), "Save failed - check SD and log");
                    draw_screen(SCREEN_REVIEW, rgb, width, height, status);
                    while (!exit_requested) {
                        uint32_t review = buttons();
                        if (exit_requested || (review & WPAD_BUTTON_B)) break;
                        if (review & WPAD_BUTTON_A) {
                            open_live = true;
                            break;
                        }
                        VIDEO_WaitVSync();
                    }
                }
            }
            menu = !open_live;
        }
        VIDEO_WaitVSync();
    }
    if (usb_ready) USB_Deinitialize();
    if (log_file) fclose(log_file);
    if (mounted) fatUnmount("sd:");
    WPAD_Shutdown();
    return 0;
}
