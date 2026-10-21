/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */

#include <gccore.h>
#include <asndlib.h>
#include <wiiuse/wpad.h>
#include <ogc/usb.h>
#include <ogc/lwp_watchdog.h>
#include <sdcard/wiisd_io.h>
#include <fat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
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
#include "av_recorder.h"
#include "clip.h"

#define FRAME_TIMEOUT_SECONDS 10
#define AV_RATE_SETTLE_MS 50
#define CONFIG_CAPACITY CAMERA_CONFIG_CAPACITY
#define VIDEO_DIRECTORY "sd:/wc04-camera/videos"
#define PHOTO_DIRECTORY "sd:/DCIM/100WC04"
#define READY_STATUS "Open viewfinder to start the camera."
_Static_assert(UVC_PROBE_LENGTH <= 32, "probe must fit reserved MEM2 bank");

static uint8_t *config;
static uint8_t *probe;
static uint8_t *iso_data;
static uint16_t *iso_sizes;
static uint8_t *audio_iso_data;
static uint16_t *audio_iso_sizes;
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
static AvRecorder av_recording;
static uint8_t av_jpeg[WC04_FRAME_CAPACITY];
static uint8_t audio_stack[32 * 1024] ATTRIBUTE_ALIGN(32);
static Clip clip;
static uint8_t playback_pcm[AV_MAX_FRAMES * AUDIO_BLOCK_BYTES] ATTRIBUTE_ALIGN(32);

typedef struct {
    s32 handle;
    UacMode mode;
    AudioStream stream;
    IsoQueue queue;
    lwp_t thread;
    atomic_bool stop;
    atomic_uint last_ms;
    uint64_t start;
    IsoStop reason;
    int failure, cancel_result;
    bool opened, running, natural_drain;
} AvCapture;

static AvCapture av_audio;

/* IOS58 microphone packets become transaction errors after video alternate 0, or after any video
 * alternate change before the microphone is selected; only an IOS reload or replug clears it. */
typedef struct {
    s32 video, microphone;
    UacMode microphone_mode;
    MicRateResult microphone_rate;
    UvcMode video_mode;
    UvcAlternate video_alt;
    bool video_changed, video_idle, video_parked, microphone_selected, audio_poisoned;
} UsbSession;

static UsbSession usb_session;
static bool av_audio_start(s32 handle, const UvcMode *mode);
static void av_audio_stop(void);
static void av_audio_close(void);

s32 WC04_SetAlternate(s32 device_id, u8 alternate);
s32 WC04_CancelIso(s32 device_id, u8 endpoint);
s32 WC04_GetInterfaceParameters(s32 device_id, u8 *parameters);
s32 WC04_GetLegacyDeviceList(u8 host, u8 num, u32 *entries, u8 *count);

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
    const size_t bytes = CONFIG_CAPACITY + 32 + 2 * ISO_QUEUE_DEPTH * ISO_DATA_BYTES
                         + 2 * ISO_QUEUE_DEPTH * ISO_PACKETS * sizeof(uint16_t);
    uintptr_t high = (uintptr_t)SYS_GetArena2Hi() & ~(uintptr_t)31;
    uintptr_t low = (uintptr_t)SYS_GetArena2Lo();
    if (high < low || high - low < bytes) return false;
    /* Reserve MEM2 before other subsystems start; leave the small IPC heap free. */
    config = (uint8_t *)(high - bytes);
    SYS_SetArena2Hi(config);
    probe = config + CONFIG_CAPACITY;
    iso_data = probe + 32;
    iso_sizes = (uint16_t *)(iso_data + ISO_QUEUE_DEPTH * ISO_DATA_BYTES);
    audio_iso_data = (uint8_t *)(iso_sizes + ISO_QUEUE_DEPTH * ISO_PACKETS);
    audio_iso_sizes = (uint16_t *)(audio_iso_data + ISO_QUEUE_DEPTH * ISO_DATA_BYTES);
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
    return USB_ReadCtrlMsg(handle, 0x80, request, value, 0, length, data);
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

/* Test build for issue #1: when no camera is found, record what every USB host can see. */
static void report_usb_hosts(void)
{
    usb_device_entry devices[CAMERA_MAX_DEVICES];
    u8 count = 0;
    s32 result = USB_GetDeviceList(devices, CAMERA_MAX_DEVICES, 0, &count);
    report("Test: IOS58 ven/hid list result %ld, %u entries\n", (long)result, count);
    for (unsigned n = 0; n < count && n < CAMERA_MAX_DEVICES; ++n)
        report("Test:   entry %u: %04x:%04x id %08lx token %08lx\n", n, devices[n].vid,
               devices[n].pid, (unsigned long)(u32)devices[n].device_id,
               (unsigned long)devices[n].token);
    for (u8 host = 0; host < 2; ++host) {
        u32 entries[2 * 16];
        u8 found = 0;
        s32 listed = WC04_GetLegacyDeviceList(host, 16, entries, &found);
        report("Test: legacy oh%u list result %ld, %u entries\n", host, (long)listed, found);
        for (unsigned n = 0; n < found; ++n)
            report("Test:   oh%u entry %u: %04x:%04x (raw %08lx %08lx)\n", host, n,
                   (unsigned)(entries[2 * n + 1] >> 16), (unsigned)(entries[2 * n + 1] & 0xffff),
                   (unsigned long)entries[2 * n], (unsigned long)entries[2 * n + 1]);
    }
    static const u16 c920_pids[] = {0x082d, 0x0892, 0x08e5};
    for (unsigned n = 0; n < sizeof(c920_pids) / sizeof(c920_pids[0]); ++n) {
        s32 fd = -1;
        s32 opened = USB_OpenDevice(USB_OH0_DEVICE_ID, 0x046d, c920_pids[n], &fd);
        report("Test: open oh0 046d:%04x -> %ld\n", c920_pids[n], (long)opened);
        if (opened < 0) continue;
        usb_devdesc device;
        s32 got = USB_GetDeviceDescription(fd, &device);
        report("Test:   device descriptor %ld: USB %x.%02x class %02x ep0 %u configurations %u\n",
               (long)got, device.bcdUSB >> 8, device.bcdUSB & 0xff, device.bDeviceClass,
               device.bMaxPacketSize0, device.bNumConfigurations);
        memset(probe, 0, 9);
        s32 header = USB_ReadCtrlMsg(fd, 0x80, 6, 0x0200, 0, 9, probe);
        report("Test:   configuration header %ld: total %u bytes, %u interfaces, %u mA\n",
               (long)header, uvc_le16(probe + 2), probe[4], probe[8] * 2u);
        USB_CloseDevice(&fd);
    }
}

static bool find_camera(s32 *handle, unsigned width, unsigned height, UvcMode *mode)
{
    usb_device_entry devices[CAMERA_MAX_DEVICES];
    CameraEntry entries[CAMERA_MAX_DEVICES];
    u8 count = 0;
    s32 result = USB_GetDeviceList(devices, CAMERA_MAX_DEVICES, 0, &count);
    if (result < 0 || count > CAMERA_MAX_DEVICES) {
        report("USB device list failed: %ld, interfaces: %u\n", (long)result, count);
        report_usb_hosts();
        return false;
    }
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
        report_usb_hosts();
        return false;
    }
    available_sizes = mode->supported_sizes;
    *handle = selected;
    report("Selected VS %u / endpoint %02x / MJPEG format %u frame %u, %ux%u\n", mode->interface,
           mode->endpoint, mode->format, mode->frame, mode->width, mode->height);
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
    report("Refresh camera source %ux%u: %s\n", width, height,
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
    s32 result = request == UVC_SET_CUR
                     ? USB_WriteCtrlMsg(camera->handle, 0x21, request, selector, camera->interface,
                                        UVC_PROBE_LENGTH, data)
                     : USB_ReadCtrlMsg(camera->handle, 0xa1, request, selector, camera->interface,
                                       UVC_PROBE_LENGTH, data);
    /* IOS58 counts the eight-byte setup packet in control completions. */
    return uvc_control_payload_length(result, UVC_PROBE_LENGTH);
}

static void release_microphone(void)
{
    if (!usb_session.microphone_selected) return;
    s32 result = WC04_SetAlternate(usb_session.microphone, 0);
    report("Microphone released: idle alternate=%ld\n", (long)result);
    USB_CloseDevice(&usb_session.microphone);
    usb_session.microphone_selected = false;
}

static void bind_usb_session(s32 video)
{
    if (usb_session.video == video) return;
    release_microphone();
    usb_session = (UsbSession){.video = video};
}

static s32 set_video_alternate(s32 handle, u8 alternate)
{
    bind_usb_session(handle);
    if (!usb_session.microphone_selected || !alternate) usb_session.audio_poisoned = true;
    usb_session.video_parked = false;
    s32 result = WC04_SetAlternate(handle, alternate);
    usb_session.video_changed = true;
    usb_session.video_idle = !alternate && result == 0;
    return result;
}

static bool take_parked_camera(unsigned width, unsigned height, s32 *handle, UvcMode *mode,
                               UvcAlternate *alt)
{
    if (!usb_session.video_parked || usb_session.video_mode.width != width
        || usb_session.video_mode.height != height)
        return false;
    usb_session.video_parked = false;
    *handle = usb_session.video;
    *mode = usb_session.video_mode;
    *alt = usb_session.video_alt;
    report("Reusing parked camera stream %ux%u alternate %u.\n", width, height, alt->number);
    return true;
}

static bool park_video_stream(const UvcMode *mode, const UvcAlternate *alt, IsoStop result)
{
    bool healthy =
        result == ISO_RECORD_STOPPED || result == ISO_CANCELLED || result == ISO_PHOTO_READY;
    if (!healthy || !usb_session.microphone_selected || usb_session.audio_poisoned
        || !usb_session.video_changed || usb_session.video_idle)
        return false;
    usb_session.video_parked = true;
    usb_session.video_mode = *mode;
    usb_session.video_alt = *alt;
    report("Video parked at alternate %u for %ux%u.\n", alt->number, mode->width, mode->height);
    return true;
}

/* Changing size needs video alternate 0, so only the parked size keeps sound in this launch. */
static bool parked_elsewhere(unsigned width, unsigned height, char *status, size_t capacity)
{
    if (!usb_session.video_parked || usb_session.audio_poisoned
        || (usb_session.video_mode.width == width && usb_session.video_mode.height == height))
        return false;
    snprintf(status, capacity, "Sound needs %ux%u or a restart", usb_session.video_mode.width,
             usb_session.video_mode.height);
    return true;
}

static bool sound_needs_restart(unsigned width, unsigned height, char *status, size_t capacity)
{
    if (parked_elsewhere(width, height, status, capacity)) return true;
    if (!usb_session.audio_poisoned && !(usb_session.video_changed && !usb_session.video_parked))
        return false;
    snprintf(status, capacity, "Restart WiiCam to record with sound");
    return true;
}

static void release_camera(void)
{
    release_microphone();
    if (!usb_session.video_changed || usb_session.video_idle) return;
    s32 result = WC04_SetAlternate(usb_session.video, 0);
    report("Video released: idle alternate=%ld\n", (long)result);
    usb_session.video_idle = true;
}

static bool idle_camera(s32 handle)
{
    s32 result = set_video_alternate(handle, 0);
    if (result < 0) {
        report("Idle alternate failed: %ld\n", (long)result);
        return false;
    }
    return true;
}

static bool probe_camera(s32 handle, const UvcMode *mode, UvcAlternate *alt)
{
    ProbeContext camera = {.handle = handle, .interface = mode->interface};
    if (!uvc_negotiate(mode, alt, probe, probe_transfer, &camera)) {
        report("Probe/commit failed, short, or returned unsupported values.\n");
        return false;
    }
    return true;
}

static bool prepare_camera(s32 handle, const UvcMode *mode, UvcAlternate *alt)
{
    return idle_camera(handle) && probe_camera(handle, mode, alt);
}

static bool activate_camera(s32 handle, const UvcAlternate *alt)
{
    s32 result = set_video_alternate(handle, alt->number);
    report("Streaming alternate %u, endpoint %02x, capacity %u: %ld\n", alt->number, alt->endpoint,
           alt->capacity, (long)result);
    return result >= 0;
}

static bool negotiate(s32 handle, const UvcMode *mode, UvcAlternate *alt)
{
    return prepare_camera(handle, mode, alt) && activate_camera(handle, alt);
}

static bool microphone_first(s32 video)
{
    bind_usb_session(video);
    return !usb_session.video_changed;
}

typedef struct {
    s32 handle;
    const UvcMode *mode;
    const UvcAlternate *alt;
    UvcFrame frame;
    uint64_t start;
    unsigned rejected;
    Preview preview;
    atomic_bool worker_stop;
    atomic_uint last_decoded_ms;
    lwp_t worker;
    uint64_t last_input;
    unsigned decoded;
    CaptureControl control;
    atomic_uint countdown;
    RecordControl record_control;
    bool video_mode, audio_mode;
    unsigned audio_end;
} Capture;

static void *preview_worker(void *context)
{
    Capture *capture = context;
    uint64_t last_draw = 0;

    while (!atomic_load_explicit(&capture->worker_stop, memory_order_acquire)) {
        bool have_frame = preview_take(&capture->preview);
        if (have_frame) {
            bool decoded = photo_decode(capture->preview.jpeg, capture->preview.length, decode_rgb,
                                        capture->mode->width, capture->mode->height);
            if (decoded) {
                memcpy(rgb, decode_rgb, (size_t)capture->mode->width * capture->mode->height * 3);
                ++capture->decoded;
                unsigned elapsed = (unsigned)ticks_to_millisecs(gettime() - capture->start);
                atomic_store_explicit(&capture->last_decoded_ms, elapsed, memory_order_release);
                unsigned state =
                    atomic_load_explicit(&capture->record_control.state, memory_order_acquire);
                if (record_control_begin(&capture->record_control)) {
                    bool ready = ensure_directory(VIDEO_DIRECTORY);
                    if (ready && capture->audio_mode)
                        ready = av_recorder_start(&av_recording, VIDEO_DIRECTORY,
                                                  capture->mode->width, capture->mode->height,
                                                  capture->preview.jpeg, capture->preview.length);
                    else if (ready)
                        ready =
                            recorder_start(&recording, VIDEO_DIRECTORY,
                                           capture->mode->width, capture->mode->height, elapsed);
                    if (!ready) {
                        if (capture->audio_mode)
                            av_recording.failed = true;
                        else
                            recording.failed = true;
                    }
                    record_control_started(&capture->record_control, ready, elapsed);
                    state = atomic_load(&capture->record_control.state);
                }
                if (state == RECORD_ACTIVE) {
                    bool ok = capture->audio_mode
                                  ? av_recorder_frame(&av_recording, capture->preview.jpeg,
                                                      capture->preview.length,
                                                      capture->preview.audio_position)
                                  : recorder_frame(&recording, capture->preview.jpeg,
                                                   capture->preview.length, elapsed);
                    if (!ok)
                        record_control_result(
                            &capture->record_control,
                            (capture->audio_mode ? av_recording.limited : recording.limited)
                                ? RECORD_LIMIT
                                : RECORD_FAILED);
                }
            } else
                ++capture->rejected;
            preview_finish(&capture->preview, decoded);
        }

        if (capture->audio_mode) {
            unsigned state = atomic_load(&capture->record_control.state);
            if (state == RECORD_ACTIVE) {
                if (!av_recorder_pump(&av_recording, audio_stream_position(&av_audio.stream)))
                    record_control_result(&capture->record_control,
                                          av_recording.limited ? RECORD_LIMIT : RECORD_FAILED);
            } else if (!av_recording.active)
                audio_stream_discard(&av_audio.stream);
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
            else if (capture->audio_mode && state == RECORD_STOPPING)
                info.status = "Stopping USB - unplug camera if stuck";
            draw_screen_info(signal ? SCREEN_LIVE : SCREEN_NO_SIGNAL, signal ? rgb : NULL,
                             capture->mode->width, capture->mode->height, &info);
            last_draw = now;
        }
        if (!have_frame) usleep(1000);
    }

    /* Only this worker writes the video; finalize before its join returns. */
    if (capture->audio_mode && av_recording.active)
        av_recorder_stop(&av_recording, capture->audio_end, audio_stream_valid(&av_audio.stream));
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
        if (capture->audio_mode && atomic_load(&av_audio.stream.failed)) return ISO_RECORD_STOPPED;
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
        if (action == CONTROL_PHOTO) preview_shutter(&capture->preview);
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
    if (capture->audio_mode) {
        capture->audio_end = audio_stream_position(&av_audio.stream);
        av_audio_stop();
    }
    atomic_store_explicit(&capture->worker_stop, true, memory_order_release);
    LWP_JoinThread(capture->worker, NULL);
    draw_screen(SCREEN_WAITING, reason == ISO_PHOTO_READY ? rgb : NULL, capture->mode->width,
                capture->mode->height,
                capture->video_mode ? "Stopping camera / saving video" : "Stopping camera");
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
    for (unsigned i = 0; i < ISO_PACKETS; ++i) {
        unsigned length = slot->sizes[i];
        if (length > alt->capacity) {
            frame->discard = true;
            continue;
        }
        /* IOS retains requested strides; returned sizes are actual lengths. */
        if (uvc_packet(frame, slot->data + i * alt->capacity, length))
            preview_offer_at(&capture->preview, frame->data, frame->size,
                             capture->audio_mode ? audio_stream_position(&av_audio.stream) : 0);
    }
    return false;
}

static IsoStop receive_photo(s32 handle, const UvcMode *mode, const UvcAlternate *alt)
{
    Capture capture = {.handle = handle,
                       .mode = mode,
                       .alt = alt,
                       .video_mode = screen_info.video,
                       .audio_mode = screen_info.audio};
    uvc_frame_init(&capture.frame, frame_data, sizeof(frame_data));
    IsoQueue queue;
    iso_queue_init(&queue, iso_data, iso_sizes, alt->capacity);
    const IsoHooks hooks = {.submit = submit_capture,
                            .consume = consume_capture,
                            .poll = poll_capture,
                            .idle = idle_capture,
                            .stopping = stopping_capture,
                            .cancel = cancel_capture};
    preview_init(&capture.preview, preview_jpeg, sizeof(preview_jpeg));
    atomic_init(&capture.worker_stop, false);
    atomic_init(&capture.last_decoded_ms, 0);
    atomic_init(&capture.countdown, 0);
    record_control_init(&capture.record_control);
    capture_control_init(&capture.control, screen_info.timer);
    recorder_init(&recording);
    av_recorder_init(&av_recording, &av_audio.stream, av_jpeg);
    draw_screen(SCREEN_WAITING, NULL, mode->width, mode->height, "WAITING FOR FIRST FRAME");
    capture.start = gettime();
    if (LWP_CreateThread(&capture.worker, preview_worker, &capture, worker_stack,
                         sizeof(worker_stack), 60)
        < 0) {
        report("Could not start preview worker.\n");
        return ISO_SUBMIT_FAILED;
    }
    IsoStop stop = iso_queue_run(&queue, &hooks, &capture);
    if (capture.audio_mode && atomic_load(&av_audio.stream.failed)) av_recording.failed = true;
    const UvcFrame *frame = &capture.frame;
    const UvcStats *stats = &frame->stats;
    report("Viewfinder closed: %s after %llu ms; %u frames decoded, %u dropped, %u undecodable.\n",
           stop_description(stop),
           (unsigned long long)ticks_to_millisecs(gettime() - capture.start), capture.decoded,
           frame->dropped, capture.rejected);
    if (stats->header_only && !stats->payload_bytes && !stats->bad_headers)
        report("Only UVC headers received; no image payload reached the decoder.\n");
    if (capture.audio_mode && av_recording.saved)
        report("Video saved: %s; %u frames with sound, %u repeated.\n", av_recording.path,
               av_recording.avi.frames, av_recording.repeated);
    else if (capture.audio_mode && av_recording.failed)
        report("Video with sound not saved: limit=%u, %u frames written.\n", av_recording.limited,
               av_recording.avi.frames);
    else if (recording.saved)
        report("Video saved: %s; %u frames in %u ms.\n", recording.path, recording.avi.frames,
               recording.duration_ms);
    else if (recording.failed)
        report("Video SD creation/write failed. No completed clip.\n");
    return stop;
}

typedef struct {
    s32 handle;
    UacMode mode;
    CameraEntry entry;
} Microphone;

static int mic_control(void *context, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                       uint8_t *data, unsigned length)
{
    const Microphone *microphone = context;
    s32 result = type & 0x80 ? USB_ReadCtrlMsg(microphone->handle, type, request, value, index,
                                               length, data)
                             : USB_WriteCtrlMsg(microphone->handle, type, request, value, index,
                                                length, data);
    return uvc_control_payload_length(result, length);
}

static bool drain_audio_queue(const IsoQueue *queue)
{
    uint64_t start = gettime();
    /* Four 16-packet requests cover 64 ms on the observed 1 ms audio endpoint.
     * Callback completion, not the wait deadline, returns buffer ownership. */
    for (;;) {
        bool pending = false;
        for (unsigned n = 0; n < ISO_QUEUE_DEPTH; ++n) {
            const IsoSlot *slot = &queue->slots[n];
            pending |= slot->pending
                       && !atomic_load_explicit(&slot->completed, memory_order_acquire);
        }
        if (!pending) return true;
        if (ticks_to_millisecs(gettime() - start) >= 100) return false;
        usleep(1000);
    }
}

static bool mic_open(s32 video_handle, const UvcMode *video_mode, Microphone *microphone)
{
    uint8_t reference[CONFIG_CAPACITY];
    unsigned length = uvc_le16(config + 2);
    if (length < 9 || length > sizeof(reference)
        || !microphone_mode(config, length, &microphone->mode)) {
        report("No usable microphone: sound needs one UAC1 mono PCM16 16000 Hz mode.\n");
        return false;
    }
    memcpy(reference, config, length);
    usb_device_entry devices[CAMERA_MAX_DEVICES];
    MicEntry entries[CAMERA_MAX_DEVICES];
    u8 count = 0;
    if (USB_GetDeviceList(devices, CAMERA_MAX_DEVICES, 0, &count) < 0 || count > CAMERA_MAX_DEVICES)
        return false;
    for (unsigned n = 0; n < count; ++n)
        entries[n] = (MicEntry){{devices[n].device_id, devices[n].vid, devices[n].pid,
                                 (uint8_t)(devices[n].token >> 8)},
                                devices[n].token};
    size_t selected;
    if (!microphone_match(entries, count, video_handle, video_mode->interface,
                          microphone->mode.interface, &selected)) {
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
    microphone->handle = opened;
    microphone->entry = *entry;
    return true;
}

static int av_audio_submit(void *context, IsoSlot *slot)
{
    const AvCapture *audio = context;
    return USB_ReadIsoMsgAsync(audio->handle, audio->mode.endpoint, ISO_PACKETS, slot->sizes,
                               slot->data, iso_finished, slot);
}

static bool av_audio_consume(void *context, const IsoSlot *slot)
{
    AvCapture *audio = context;
    unsigned now = (unsigned)ticks_to_millisecs(gettime() - audio->start);
    unsigned before = audio_stream_received(&audio->stream);
    for (unsigned n = 0; n < ISO_PACKETS; ++n)
        if (!audio_stream_packet(&audio->stream, slot->data + n * audio->mode.packet_capacity,
                                 slot->sizes[n], audio->mode.packet_capacity, now))
            return true;
    if (audio_stream_received(&audio->stream) != before) atomic_store(&audio->last_ms, now);
    return false;
}

static IsoStop av_audio_poll(void *context)
{
    AvCapture *audio = context;
    if (atomic_load(&audio->stop)) return ISO_CANCELLED;
    unsigned now = (unsigned)ticks_to_millisecs(gettime() - audio->start);
    if (now - atomic_load(&audio->last_ms) >= 500) return ISO_TIMEOUT;
    return ISO_RUNNING;
}

static void av_audio_stopping(void *context, IsoStop reason, int failure)
{
    AvCapture *audio = context;
    audio->reason = reason;
    audio->failure = failure;
    if (reason != ISO_CANCELLED) atomic_store(&audio->stream.failed, true);
}

static void av_audio_cancel(void *context)
{
    AvCapture *audio = context;
    audio->natural_drain = drain_audio_queue(&audio->queue);
    if (audio->natural_drain) return;
    audio->cancel_result = WC04_CancelIso(audio->handle, audio->mode.endpoint);
}

static void *av_audio_worker(void *context)
{
    AvCapture *audio = context;
    const IsoHooks hooks = {.submit = av_audio_submit,
                            .consume = av_audio_consume,
                            .poll = av_audio_poll,
                            .idle = idle_capture,
                            .stopping = av_audio_stopping,
                            .cancel = av_audio_cancel};
    iso_queue_run(&audio->queue, &hooks, audio);
    return NULL;
}

static bool av_audio_wait_rate(uint64_t configured)
{
    uint64_t last_input = configured;
    while (ticks_to_millisecs(gettime() - configured) < AV_RATE_SETTLE_MS) {
        uint64_t now = gettime();
        if (exit_requested) return false;
        if (ticks_to_millisecs(now - last_input) >= 10) {
            uint32_t down = buttons();
            if (exit_requested || (down & WPAD_BUTTON_B)) return false;
            last_input = now;
        }
        usleep(1000);
    }
    return !exit_requested;
}

static bool av_audio_queue_start(MicRateResult rate)
{
    if (!av_audio.opened || av_audio.running || rate == MIC_RATE_FAILED
        || atomic_load(&av_audio.stream.failed))
        return false;
    s32 handle = av_audio.handle;
    UacMode mode = av_audio.mode;
    /* A stopped worker has joined and returned every buffer before this reset. */
    memset(&av_audio, 0, sizeof(av_audio));
    av_audio.handle = handle;
    av_audio.mode = mode;
    av_audio.opened = true;
    av_audio.natural_drain = true;
    atomic_init(&av_audio.stop, false);
    atomic_init(&av_audio.last_ms, 0);
    audio_stream_init(&av_audio.stream, rate);
    iso_queue_init(&av_audio.queue, audio_iso_data, audio_iso_sizes, mode.packet_capacity);
    av_audio.start = gettime();
    if (LWP_CreateThread(&av_audio.thread, av_audio_worker, &av_audio, audio_stack,
                         sizeof(audio_stack), 80)
        < 0)
        return false;
    av_audio.running = true;
    return true;
}

static bool av_audio_start(s32 handle, const UvcMode *mode)
{
    memset(&av_audio, 0, sizeof(av_audio));
    atomic_init(&av_audio.stop, false);
    atomic_init(&av_audio.last_ms, 0);
    bool reused = usb_session.microphone_selected;
    Microphone microphone = {.handle = usb_session.microphone,
                             .mode = usb_session.microphone_mode};
    if (!reused && !mic_open(handle, mode, &microphone)) return false;
    av_audio.handle = microphone.handle;
    av_audio.mode = microphone.mode;
    av_audio.opened = true;
    MicRateResult rate = usb_session.microphone_rate;
    if (!reused) {
        rate = MIC_RATE_FAILED;
        if (WC04_SetAlternate(microphone.handle, 0) == 0
            && WC04_SetAlternate(microphone.handle, microphone.mode.alternate) == 0)
            rate = microphone_set_rate(&microphone.entry, &microphone.mode, mic_control,
                                       &microphone, probe);
        usb_session.microphone = microphone.handle;
        usb_session.microphone_mode = microphone.mode;
        usb_session.microphone_rate = rate;
        usb_session.microphone_selected = rate != MIC_RATE_FAILED;
    }
    uint64_t configured = gettime();
    audio_stream_init(&av_audio.stream, rate);
    if (rate == MIC_RATE_FAILED || exit_requested
        || (!reused && rate == MIC_RATE_UNREADABLE && !av_audio_wait_rate(configured))) {
        if (rate == MIC_RATE_FAILED) report("Microphone alternate or rate setup failed.\n");
        av_audio_close();
        return false;
    }
    if (!av_audio_queue_start(rate)) {
        av_audio_close();
        return false;
    }
    if (!reused)
        report("Microphone interface %u alternate %u endpoint %02x: 16 kHz, rate readback %s.\n",
               microphone.mode.interface, microphone.mode.alternate, microphone.mode.endpoint,
               rate == MIC_RATE_OK ? "confirmed" : "skipped for this camera");
    return true;
}

static void av_audio_stop(void)
{
    if (!av_audio.running) return;
    atomic_store(&av_audio.stop, true);
    LWP_JoinThread(av_audio.thread, NULL);
    av_audio.running = false;
}

static void av_audio_close(void)
{
    if (!av_audio.opened) return;
    av_audio_stop();
    const AudioStream *stream = &av_audio.stream;
    report("Audio stopped: reason=%u failure=%d fatal=%u; %llu ms of sound, %llu ms concealed, "
           "%u rejected packets, peak %u.\n",
           av_audio.reason, av_audio.failure, atomic_load(&stream->failed),
           (unsigned long long)(stream->bytes * 1000 / (MIC_RATE * 2)),
           (unsigned long long)(stream->concealed_bytes * 1000 / (MIC_RATE * 2)), stream->rejected,
           stream->peak);
    if (av_audio.queue.submitted && !av_audio.natural_drain)
        report("Audio requests cancelled: %d\n", av_audio.cancel_result);
    /* IOS58 cannot select the microphone again after a video alternate change. */
    if (!usb_session.microphone_selected || !usb_session.video_changed) {
        s32 result = WC04_SetAlternate(av_audio.handle, 0);
        report("Microphone released: idle alternate=%ld\n", (long)result);
        USB_CloseDevice(&av_audio.handle);
        usb_session.microphone_selected = false;
    }
    av_audio.opened = false;
}

static IsoStop av_audio_prime(unsigned before)
{
    uint64_t start = gettime(), last_input = start;
    for (;;) {
        uint64_t now = gettime();
        uint32_t down = 0;
        if (ticks_to_millisecs(now - last_input) >= 10) {
            down = buttons();
            last_input = now;
        }
        if (exit_requested) return ISO_EXIT;
        if (down & WPAD_BUTTON_B) return ISO_CANCELLED;
        if (atomic_load(&av_audio.stream.failed)) {
            report("Microphone stream failed while waiting for sound.\n");
            return ISO_COMPLETION_FAILED;
        }
        unsigned received = audio_stream_received(&av_audio.stream);
        /* The main thread consumes only until the preview worker takes ownership. */
        audio_stream_discard(&av_audio.stream);
        if (received - before >= AUDIO_BLOCK_BYTES) return ISO_RUNNING;
        if (ticks_to_millisecs(now - start) >= 1000) {
            report("Microphone sent no sound for 1 s.\n");
            return ISO_TIMEOUT;
        }
        usleep(1000);
    }
}

static IsoStop av_enable_video(s32 handle, const UvcMode *mode, UvcAlternate *alt,
                               bool *video_changed)
{
    if (!probe_camera(handle, mode, alt)) return ISO_SUBMIT_FAILED;
    if (atomic_load(&av_audio.stream.failed)) return ISO_COMPLETION_FAILED;
    /* Earlier PCM and concealed silence cannot satisfy the post-control gate. */
    IsoStop result = av_audio_prime(audio_stream_received(&av_audio.stream));
    if (result != ISO_RUNNING) return result;
    av_audio_stop();
    if (atomic_load(&av_audio.stream.failed)) return ISO_COMPLETION_FAILED;
    MicRateResult rate = av_audio.stream.rate;
    *video_changed = true;
    if (!activate_camera(handle, alt)) return ISO_SUBMIT_FAILED;
    if (atomic_load(&av_audio.stream.failed)) return ISO_COMPLETION_FAILED;
    if (!av_audio_queue_start(rate)) return ISO_SUBMIT_FAILED;
    result = av_audio_prime(0);
    if (result != ISO_RUNNING) return result;
    /* Photos and silent video keep the microphone selected without streaming it. */
    if (!screen_info.audio) av_audio_close();
    return receive_photo(handle, mode, alt);
}

static IsoStop receive_av(s32 handle, const UvcMode *mode, UvcAlternate *alt, bool parked,
                          bool *video_changed)
{
    *video_changed = false;
    bind_usb_session(handle);
    if (usb_session.audio_poisoned || (usb_session.video_changed && !parked)) {
        report("A/V needs a restart: this launch's video alternate changes leave IOS58 without "
               "microphone data.\n");
        av_recording.failed = true;
        return ISO_SUBMIT_FAILED;
    }
    draw_screen(SCREEN_WAITING, NULL, mode->width, mode->height,
                screen_info.audio ? "Checking microphone - B cancels"
                                  : "Starting camera - B cancels");
    if (!av_audio_start(handle, mode)) {
        av_recording.failed = true;
        return ISO_SUBMIT_FAILED;
    }
    IsoStop result = av_audio_prime(0);
    if (result == ISO_RUNNING && !parked)
        result = av_enable_video(handle, mode, alt, video_changed);
    else if (result == ISO_RUNNING)
        result = receive_photo(handle, mode, alt);
    else if (parked && (result == ISO_TIMEOUT || result == ISO_COMPLETION_FAILED))
        usb_session.audio_poisoned = true;
    if (result == ISO_SUBMIT_FAILED || result == ISO_COMPLETION_FAILED || result == ISO_TIMEOUT)
        av_recording.failed = true;
    av_audio_close();
    return result;
}

static void stop_video_stream(s32 handle, bool video_changed)
{
    if (!video_changed) return;
    s32 stopped = set_video_alternate(handle, 0);
    report("Stop streaming: %ld\n", (long)stopped);
}

/* Every mode starts the microphone first, so sound stays available whichever mode opens first. */
static IsoStop run_camera(s32 handle, UvcMode *mode, UvcAlternate *alt, bool parked,
                          bool *video_changed)
{
    if (screen_info.audio || (!parked && microphone_first(handle))) {
        IsoStop result = receive_av(handle, mode, alt, parked, video_changed);
        /* Without a working microphone, photos and silent video still start the old way. */
        if (screen_info.audio || *video_changed || result == ISO_CANCELLED || result == ISO_EXIT)
            return result;
    }
    *video_changed = !parked;
    return parked || negotiate(handle, mode, alt) ? receive_photo(handle, mode, alt)
                                                  : ISO_SUBMIT_FAILED;
}

static bool save_photo(unsigned width, unsigned height, char path[80])
{
    if (!mounted || !ensure_directory("sd:/DCIM") || !ensure_directory(PHOTO_DIRECTORY)) {
        report("SD photo directory unavailable.\n");
        return false;
    }
    FILE *file = storage_create(PHOTO_DIRECTORY, "WC04", "JPG", path, 80);
    if (!file) {
        report("Photo creation failed: %s\n", strerror(errno));
        return false;
    }
    bool ok = storage_finish(file, path, photo_encode(file, rgb, width, height));
    if (!ok) report("JPEG write failed; incomplete-file cleanup attempted.\n");
    return ok;
}

/* Sound starts and stops with each playback, so recording always runs with the DSP idle. */
static bool play_clip(const char *path, unsigned width, unsigned height, ScreenInfo info)
{
    char status[80];
    const char *name = strrchr(path, '/') + 1;
    info.status = status;
    info.playing = true;
    info.can_play = info.can_delete = info.confirm_delete = false;
    snprintf(status, sizeof(status), "Loading %s", name);
    draw_screen_info(SCREEN_REVIEW, rgb, width, height, &info);
    uint64_t start = gettime();

    if (!clip_open(&clip, path) || clip.width != width || clip.height != height) {
        report("Playback: %s did not open as a %ux%u WiiCam clip.\n", path, width, height);
        clip_close(&clip);
        return false;
    }

    size_t pcm_bytes = clip_audio(&clip, playback_pcm, sizeof(playback_pcm));
    for (size_t n = 0; n + 1 < pcm_bytes; n += 2) {
        uint8_t low = playback_pcm[n];
        playback_pcm[n] = playback_pcm[n + 1];
        playback_pcm[n + 1] = low;
    }
    unsigned load_ms = (unsigned)ticks_to_millisecs(gettime() - start);
    bool sound = pcm_bytes > 0;

    if (sound) {
        DCFlushRange(playback_pcm, pcm_bytes);
        ASND_Init();
        ASND_Pause(0);
        sound = ASND_SetVoice(0, VOICE_MONO_16BIT, MIC_RATE, 0, playback_pcm, (s32)pcm_bytes, 255,
                              255, NULL)
                == SND_OK;
        if (!sound) ASND_End();
    }

    start = gettime();
    unsigned shown = clip.frames, drawn = 0, failed = 0;
    bool finished = false;
    while (!exit_requested) {
        if (buttons() & (WPAD_BUTTON_A | WPAD_BUTTON_B | WPAD_BUTTON_1)) break;
        unsigned ms = sound ? ASND_GetTimerVoice(0)
                            : (unsigned)ticks_to_millisecs(gettime() - start);
        unsigned index = (unsigned)((uint64_t)ms * 1000 / clip.frame_us);
        if (index >= clip.frames || (sound && ASND_StatusVoice(0) == SND_UNUSED)) {
            finished = true;
            break;
        }
        if (index == shown) {
            VIDEO_WaitVSync();
            continue;
        }
        shown = index;
        size_t length;
        if (!clip_frame(&clip, index, frame_data, sizeof(frame_data), &length)
            || !photo_decode(frame_data, length, decode_rgb, width, height)) {
            ++failed;
            continue;
        }
        memcpy(rgb, decode_rgb, (size_t)width * height * 3);
        ++drawn;
        unsigned seconds = (unsigned)((uint64_t)index * clip.frame_us / 1000000);
        unsigned total = (unsigned)((uint64_t)clip.frames * clip.frame_us / 1000000);
        snprintf(status, sizeof(status), "%s  %02u:%02u / %02u:%02u", name, seconds / 60,
                 seconds % 60, total / 60, total % 60);
        draw_screen_info(SCREEN_REVIEW, rgb, width, height, &info);
    }

    if (sound) {
        ASND_StopVoice(0);
        ASND_End();
    }
    report("Playback %s: frames=%u drawn=%u failed=%u last=%u finished=%u sound=%u "
           "PCM_bytes=%u load_ms=%u frame_us=%u\n",
           name, clip.frames, drawn, failed, shown, finished, sound, (unsigned)pcm_bytes, load_ms,
           clip.frame_us);
    clip_close(&clip);
    return drawn > 0;
}

/* Returns true when A asks for the viewfinder again. */
static bool review(const char *path, unsigned width, unsigned height, char *status,
                   size_t capacity)
{
    ReviewControl control = {.file = path != NULL, .playable = screen_info.video};
    const char *name = path ? strrchr(path, '/') + 1 : "";
    bool picture = true;
    ScreenInfo info = screen_info;
    info.sd = mounted;
    info.status = status;
    while (!exit_requested) {
        info.can_play = control.file && control.playable;
        info.can_delete = control.file;
        info.confirm_delete = control.armed;
        draw_screen_info(SCREEN_REVIEW, picture ? rgb : NULL, width, height, &info);
        ReviewAction action = REVIEW_NONE;
        while (!exit_requested && action == REVIEW_NONE) {
            uint32_t down = buttons();
            action = review_control_update(&control, down & WPAD_BUTTON_A, down & WPAD_BUTTON_B,
                                           down & WPAD_BUTTON_1, down & WPAD_BUTTON_MINUS);
            if (action == REVIEW_NONE) VIDEO_WaitVSync();
        }
        if (action == REVIEW_LIVE || action == REVIEW_MENU) return action == REVIEW_LIVE;
        if (action == REVIEW_CONFIRM)
            snprintf(status, capacity, "Press - again to delete %s", name);
        else if (action == REVIEW_CANCEL)
            snprintf(status, capacity, "Saved %s", name);
        else if (action == REVIEW_PLAY) {
            bool played = play_clip(path, width, height, info);
            snprintf(status, capacity, played ? "Saved %s" : "Playback failed - %s kept", name);
        } else if (action == REVIEW_DELETE && unlink(path) == 0) {
            report("Deleted %s\n", path);
            control.file = false;
            picture = false;
            snprintf(status, capacity, "Deleted %s", name);
        } else if (action == REVIEW_DELETE) {
            report("Delete %s failed: %s\n", path, strerror(errno));
            snprintf(status, capacity, "Delete failed - see log");
        }
    }
    return false;
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

    report("\nWiiCam C920 test 1 - IOS %u\n", IOS_GetVersion());
    bool usb_ready = false;
    if (IOS_GetVersion() == 58)
        usb_ready = buffers_ready && USB_Initialize() >= 0;
    else
        report("Launch under IOS58. This app does not install/reload IOS.\n");

    unsigned width = 320, height = 240;
    char status[80];
    snprintf(status, sizeof(status), "%s",
             !usb_ready ? "Camera needs IOS58 and USB"
             : mounted  ? READY_STATUS
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
            snprintf(status, sizeof(status), READY_STATUS);
            menu = true;
            down = 0;
        }

        if (idle_page == SCREEN_MENU && (down & WPAD_BUTTON_PLUS)) {
            if (screen_info.audio) {
                screen_info.audio = false;
                screen_info.video = false;
            } else if (screen_info.video)
                screen_info.audio = true;
            else
                screen_info.video = true;
            snprintf(status, sizeof(status), READY_STATUS);
            menu = true;
        }

        if (idle_page == SCREEN_MENU && (down & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT))) {
            screen_info.timer =
                capture_timer_next(screen_info.timer, down & WPAD_BUTTON_LEFT ? -1 : 1);
            menu = true;
        }

        if (idle_page == SCREEN_MENU && (down & WPAD_BUTTON_1)) {
            VideoSize next = video_size_next(width, height, available_sizes);
            width = next.width;
            height = next.height;
            if (!parked_elsewhere(width, height, status, sizeof(status)))
                snprintf(status, sizeof(status), "%ux%u selected - 2 refreshes camera source",
                         width, height);
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

            draw_screen(SCREEN_WAITING, NULL, width, height, "CONNECTING CAMERA");
            s32 handle;
            idle_page = SCREEN_NO_SIGNAL;
            recorder_init(&recording);
            av_recorder_init(&av_recording, &av_audio.stream, av_jpeg);
            snprintf(status, sizeof(status), "Camera not found - A retries, B menu");
            UvcMode mode;
            UvcAlternate alt;
            bool parked = take_parked_camera(width, height, &handle, &mode, &alt);
            if (parked || find_camera(&handle, width, height, &mode)) {
                bool video_changed = !parked;
                IsoStop result = run_camera(handle, &mode, &alt, parked, &video_changed);
                if (!park_video_stream(&mode, &alt, result))
                    stop_video_stream(handle, video_changed);

                snprintf(status, sizeof(status), "%s",
                         result == ISO_CANCELLED ? READY_STATUS
                         : result == ISO_TIMEOUT ? "No frames - A retries, B menu"
                                                 : "Camera stopped - A retries, B menu");
                USB_CloseDevice(&handle);
                if (result == ISO_CANCELLED || result == ISO_RECORD_STOPPED
                    || result == ISO_PHOTO_READY)
                    idle_page = SCREEN_MENU;

                if (screen_info.audio) {
                    if (av_recording.saved)
                        snprintf(status, sizeof(status), "Saved %s",
                                 strrchr(av_recording.path, '/') + 1);
                    else if (sound_needs_restart(width, height, status, sizeof(status)))
                        idle_page = SCREEN_MENU;
                    else
                        snprintf(status, sizeof(status), "%s",
                                 av_recording.failed ? "Audio/video not saved - see log"
                                                     : "No audio/video recorded");
                } else if (screen_info.video) {
                    if (recording.saved)
                        snprintf(status, sizeof(status), "Saved %s",
                                 strrchr(recording.path, '/') + 1);
                    else if (recording.failed)
                        snprintf(status, sizeof(status), "Video not saved - check SD and log");
                    else if (result == ISO_RECORD_STOPPED)
                        snprintf(status, sizeof(status), "No video frames recorded");
                }

                bool clip_saved =
                    screen_info.audio ? av_recording.saved : screen_info.video && recording.saved;
                /* B is labeled "Stop + menu", so only its stop skips the review. */
                if (result == ISO_RECORD_STOPPED && clip_saved)
                    open_live = review(screen_info.audio ? av_recording.path : recording.path,
                                       width, height, status, sizeof(status));

                if (result == ISO_PHOTO_READY) {
                    char path[80];
                    draw_screen(SCREEN_REVIEW, rgb, width, height, "Saving photo");
                    bool saved = save_photo(width, height, path);
                    if (saved) {
                        report("Saved %s\n", path);
                        snprintf(status, sizeof(status), "Saved %s", strrchr(path, '/') + 1);
                    } else
                        snprintf(status, sizeof(status), "Save failed - check SD and log");
                    open_live = review(saved ? path : NULL, width, height, status, sizeof(status));
                }
            }
            menu = !open_live;
        }
        VIDEO_WaitVSync();
    }

    release_camera();
    if (usb_ready) USB_Deinitialize();
    if (log_file) fclose(log_file);
    if (mounted) fatUnmount("sd:");
    WPAD_Shutdown();
    return 0;
}
