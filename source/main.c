/* SPDX-License-Identifier: GPL-3.0-only
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

#define FRAME_TIMEOUT_SECONDS 10
#define CONFIG_CAPACITY 4096

static uint8_t *config;
static uint8_t *probe;
static uint8_t *iso_data;
static uint16_t *iso_sizes;
static uint8_t frame_data[WC04_FRAME_CAPACITY];
static uint8_t rgb[640 * 480 * 3];
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

s32 WC04_SetAlternate(s32 device_id, u8 alternate);
s32 WC04_CancelIso(s32 device_id, u8 endpoint);

static void draw_screen(ScreenPage page, const uint8_t *pixels,
                         unsigned width, unsigned height, const char *status)
{
    Screen screen = {
        .pixels = xfb[back_buffer], .width = video->fbWidth, .height = video->xfbHeight,
        .stride = VIDEO_PadFramebufferWidth(video->fbWidth), .widescreen = widescreen
    };
    screen_draw(&screen, page, pixels, width, height, status, mounted);
    DCFlushRange(xfb[back_buffer], VIDEO_GetFrameBufferSize(video));
    VIDEO_SetNextFramebuffer(xfb[back_buffer]);
    VIDEO_Flush();
    /* Only the drawing thread waits; USB reception remains runnable. */
    do {
        VIDEO_WaitVSync();
    } while (VIDEO_GetCurrentFramebuffer() != xfb[back_buffer]);
    back_buffer ^= 1;
}

static bool allocate_usb_buffers(void)
{
    const size_t bytes = CONFIG_CAPACITY + 32 + ISO_QUEUE_DEPTH * ISO_DATA_BYTES +
                         ISO_QUEUE_DEPTH * ISO_PACKETS * sizeof(uint16_t);
    uintptr_t high = (uintptr_t)SYS_GetArena2Hi() & ~(uintptr_t)31;
    uintptr_t low = (uintptr_t)SYS_GetArena2Lo();
    if (high < low || high - low < bytes)
        return false;
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
        if (length >= 0)
            log_text(message);
        if (length >= (int)sizeof(message))
            log_text("Log message truncated.\n");
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
    if (down & WPAD_BUTTON_HOME)
        exit_requested = true;
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
    if (stat(path, &st) == 0)
        return S_ISDIR(st.st_mode);
    return mkdir(path, 0777) == 0;
}

static bool find_camera(s32 *handle)
{
    usb_device_entry devices[32];
    u8 count = 0;
    s32 result = USB_GetDeviceList(devices, 32, 0, &count);
    report("USB enumeration: %ld, devices: %u\n", (long)result, count);
    if (result < 0)
        return false;
    for (unsigned i = 0; i < count; ++i) {
        unsigned interface = (devices[i].token >> 8) & 0xff;
        report("  %04x:%04x id=%ld interface=%u\n", devices[i].vid, devices[i].pid,
               (long)devices[i].device_id, interface);
        if (devices[i].vid == 0x057e && devices[i].pid == 0x030a && interface == 1) {
            result = USB_OpenDevice(devices[i].device_id, 0x057e, 0x030a, handle);
            report("WC04 open: %ld\n", (long)result);
            /* IOS USBv5 vendor handles are negative; test the result, not fd. */
            if (result < 0)
                continue;
            usb_devdesc description;
            memset(&description, 0, sizeof(description));
            result = USB_GetDescriptors(*handle, &description);
            bool streaming = false;
            if (result >= 0) {
                if (description.bNumConfigurations && description.configurations &&
                    description.configurations[0].bNumInterfaces &&
                    description.configurations[0].interfaces) {
                    usb_interfacedesc *in = description.configurations[0].interfaces;
                    streaming = in->bInterfaceNumber == 1 &&
                                in->bInterfaceClass == 14 && in->bInterfaceSubClass == 2;
                }
                USB_FreeDescriptors(&description);
            }
            report("IOS streaming-interface descriptors: %ld, match=%u\n",
                   (long)result, streaming);
            if (streaming)
                return true;
            USB_CloseDevice(handle);
        }
    }
    report("No WC04 (057e:030a). Connect it, then press A.\n");
    return false;
}

static bool read_mode(s32 handle, unsigned width, unsigned height, UvcMode *mode)
{
    memset(config, 0, CONFIG_CAPACITY);
    s32 result = USB_ReadCtrlMsg(handle, 0x80, USB_REQ_GETDESCRIPTOR,
                                0x0200, 0, 9, config);
    if (result < 9) {
        report("Configuration header failed: %ld\n", (long)result);
        return false;
    }
    unsigned length = uvc_le16(config + 2);
    if (length < 9 || length > CONFIG_CAPACITY) {
        report("Unsupported configuration length: %u\n", length);
        return false;
    }
    result = USB_ReadCtrlMsg(handle, 0x80, USB_REQ_GETDESCRIPTOR,
                             0x0200, 0, (u16)length, config);
    if (result < (s32)length) {
        report("Configuration read failed/short: %ld\n", (long)result);
        return false;
    }
    if (log_file) {
        report("Configuration bytes (%u); SD log contains the dump.\n", length);
        log_hex(config, length);
        fflush(log_file);
    }
    if (!uvc_parse(config, length, width, height, mode) || mode->interface != 1) {
        report("No supported UVC 1.0 MJPEG %ux%u mode.\n", width, height);
        return false;
    }
    report("MJPEG format %u frame %u, interface %u, %ux%u\n",
           mode->format, mode->frame, mode->interface, mode->width, mode->height);
    u8 current = 0;
    result = USB_GetConfiguration(handle, &current);
    if (result < 0) {
        report("GET_CONFIGURATION failed: %ld\n", (long)result);
        return false;
    }
    if (current != mode->configuration) {
        result = USB_SetConfiguration(handle, mode->configuration);
        if (result < 0) {
            report("SET_CONFIGURATION failed: %ld\n", (long)result);
            return false;
        }
    }
    return true;
}

typedef struct {
    s32 handle;
    uint8_t interface;
} ProbeContext;

static int probe_transfer(void *context, uint8_t request, uint16_t selector,
                          uint8_t data[UVC_PROBE_LENGTH])
{
    const ProbeContext *camera = context;
    const char *operation = request == UVC_SET_CUR ? "SET_CUR" :
                            request == UVC_GET_MAX ? "GET_MAX" :
                            request == UVC_GET_DEF ? "GET_DEF" : "GET_CUR";
    const char *control = selector == UVC_VS_PROBE ? "PROBE" : "COMMIT";
    report("%s %s (%u payload bytes):\n", operation, control, UVC_PROBE_LENGTH);
    s32 result;
    if (request == UVC_SET_CUR) {
        log_hex(data, UVC_PROBE_LENGTH);
        result = USB_WriteCtrlMsg(camera->handle, 0x21, request, selector,
                                  camera->interface, UVC_PROBE_LENGTH, data);
    } else {
        result = USB_ReadCtrlMsg(camera->handle, 0xa1, request, selector,
                                 camera->interface, UVC_PROBE_LENGTH, data);
    }
    /* IOS58 counts the eight-byte setup packet in control completions. */
    int length = result < 8 ? -1 : result - 8;
    report("%s %s result: %ld; payload count: %d\n", operation, control,
           (long)result, length);
    if (request != UVC_SET_CUR && length > 0)
        log_hex(data, length < UVC_PROBE_LENGTH ? (unsigned)length : UVC_PROBE_LENGTH);
    if (length == UVC_PROBE_LENGTH) {
        report("Probe fields: format %u frame %u interval %lu quality %u\n",
               data[2], data[3], (unsigned long)uvc_le32(data + 4), uvc_le16(data + 12));
        report("Frame limit %lu, payload limit %lu\n",
               (unsigned long)uvc_le32(data + 18), (unsigned long)uvc_le32(data + 22));
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
    report("Streaming alternate %u, endpoint %02x, capacity %u: %ld\n",
           alt->number, alt->endpoint, alt->capacity, (long)result);
    return result >= 0;
}

typedef struct {
    unsigned slot, length, offset, copied;
    uint64_t elapsed_ms;
    uint8_t data[32];
} PacketSample;

static void sample_packet(PacketSample *sample, const uint8_t *data,
                          unsigned slot, unsigned length,
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
    snprintf(line, sizeof(line), "%s at capture +%llu ms: slot=%u size=%u offset=%u prefix=%u bytes",
             label, (unsigned long long)sample->elapsed_ms, sample->slot,
             sample->length, sample->offset, sample->copied);
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
    uint64_t max_decode_time;
} Capture;

static void *preview_worker(void *context)
{
    Capture *capture = context;
    uint64_t last_draw = 0;
    while (!atomic_load_explicit(&capture->worker_stop, memory_order_acquire)) {
        if (!preview_take(&capture->preview)) {
            usleep(1000);
            continue;
        }
        uint64_t decode_start = gettime();
        bool decoded = photo_decode(capture->preview.jpeg, capture->preview.length,
                                     rgb, capture->mode->width, capture->mode->height);
        uint64_t decode_time = gettime() - decode_start;
        if (decode_time > capture->max_decode_time)
            capture->max_decode_time = decode_time;
        if (decoded) {
            ++capture->decoded;
            uint64_t now = gettime();
            if (capture->decoded == 1)
                capture->first_decoded_ms = (unsigned)ticks_to_millisecs(now - capture->start);
            atomic_store_explicit(&capture->last_decoded_ms,
                (unsigned)ticks_to_millisecs(now - capture->start), memory_order_release);
            if (!capture->preview.save && (!last_draw || ticks_to_millisecs(now - last_draw) >= 66)) {
                draw_screen(SCREEN_LIVE, rgb, capture->mode->width, capture->mode->height,
                            mounted ? "FRAME YOUR SHOT" : "INSERT SD AND RESTART TO SAVE");
                last_draw = now;
                ++capture->displayed;
            }
        } else
            ++capture->rejected;
        preview_finish(&capture->preview, decoded);
    }
    return NULL;
}

static const char *stop_description(IsoStop stop)
{
    switch (stop) {
    case ISO_PHOTO_READY: return "shutter photo decoded";
    case ISO_CANCELLED: return "B pressed";
    case ISO_EXIT: return "HOME/power/reset exit requested";
    case ISO_TIMEOUT: return "no decoded frame for 10 seconds";
    case ISO_SUBMIT_FAILED: return "USB submission failure";
    case ISO_COMPLETION_FAILED: return "USB completion failure before cancellation";
    default: return "capture running";
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
    if (exit_requested)
        return ISO_EXIT;
    if (down & WPAD_BUTTON_B)
        return ISO_CANCELLED;
    if (mounted && (down & WPAD_BUTTON_A) && !atomic_load(&capture->preview.shutter)) {
        capture->shutter_ms = (unsigned)ticks_to_millisecs(now - capture->start);
        preview_shutter(&capture->preview);
    }
    if (preview_photo_ready(&capture->preview))
        return ISO_PHOTO_READY;
    unsigned last = atomic_load_explicit(&capture->last_decoded_ms, memory_order_acquire);
    unsigned elapsed = (unsigned)ticks_to_millisecs(gettime() - capture->start);
    if (elapsed - last >= FRAME_TIMEOUT_SECONDS * 1000)
        return ISO_TIMEOUT;
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
    atomic_store_explicit(&capture->worker_stop, true, memory_order_release);
    LWP_JoinThread(capture->worker, NULL);
    draw_screen(SCREEN_WAITING, reason == ISO_PHOTO_READY ? rgb : NULL,
                capture->mode->width, capture->mode->height, "STOPPING CAMERA");
    report("Capture stop requested: %s.\n", stop_description(reason));
    if (reason == ISO_SUBMIT_FAILED || reason == ISO_COMPLETION_FAILED)
        report("USB failure result: %d\n", failure);
}

static void cancel_capture(void *context)
{
    const Capture *capture = context;
    draw_screen(SCREEN_WAITING, NULL, capture->mode->width, capture->mode->height,
                "IF STUCK HERE UNPLUG CAMERA");
    s32 result = WC04_CancelIso(capture->handle, capture->alt->endpoint);
    report("Cancel endpoint: %ld. Waiting for all queued USB completions.\n", (long)result);
    report("If stuck, unplug camera.\n");
}

static int submit_capture(void *context, IsoSlot *slot)
{
    const Capture *capture = context;
    return USB_ReadIsoMsgAsync(capture->handle, capture->alt->endpoint, ISO_PACKETS,
                               slot->sizes, slot->data, iso_finished, slot);
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
            sample_packet(&capture->first[i], slot->data, i, bounded,
                          i * alt->capacity, capture->start);
            capture->first[i].length = length;
            if (length > alt->capacity)
                packed_valid = false;
            if (packed_valid) {
                sample_packet(&capture->packed[i], slot->data, i, length,
                              packed_offset, capture->start);
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
        if (length < capture->min_length)
            capture->min_length = length;
        if (length > capture->max_length) {
            capture->max_length = length;
            sample_packet(&capture->largest, slot->data, i, length, offset, capture->start);
        }
        if (length >= 2) {
            if (!capture->first_payload.copied && packet[0] >= 2 && packet[0] < length)
                sample_packet(&capture->first_payload, slot->data, i, length, offset, capture->start);
            if (!capture->first_error.copied && (packet[1] & 0x40))
                sample_packet(&capture->first_error, slot->data, i, length, offset, capture->start);
        }
        /* IOS retains requested strides; returned sizes are actual lengths. */
        if (uvc_packet(frame, packet, length)) {
            if (!preview_offer(&capture->preview, frame->data, frame->size))
                ++capture->skipped;
        }
    }
    return false;
}

static IsoStop receive_photo(s32 handle, const UvcMode *mode, const UvcAlternate *alt)
{
    Capture capture = {
        .handle = handle, .mode = mode, .alt = alt, .min_length = ISO_MAX_PAYLOAD
    };
    uvc_frame_init(&capture.frame, frame_data, sizeof(frame_data));
    IsoQueue queue;
    iso_queue_init(&queue, iso_data, iso_sizes, alt->capacity);
    const IsoHooks hooks = {
        .submit = submit_capture, .consume = consume_capture, .poll = poll_capture,
        .idle = idle_capture, .stopping = stopping_capture, .cancel = cancel_capture
    };
    report("Viewfinder with %u queued batches of %u slots. A takes photo, B stops, HOME exits.\n",
           ISO_QUEUE_DEPTH, ISO_PACKETS);
    preview_init(&capture.preview, preview_jpeg, sizeof(preview_jpeg));
    atomic_init(&capture.worker_stop, false);
    atomic_init(&capture.last_decoded_ms, 0);
    draw_screen(SCREEN_WAITING, NULL, mode->width, mode->height, "WAITING FOR FIRST FRAME");
    capture.start = gettime();
    if (LWP_CreateThread(&capture.worker, preview_worker, &capture, worker_stack,
                         sizeof(worker_stack), 60) < 0) {
        report("Could not start preview worker.\n");
        return ISO_SUBMIT_FAILED;
    }
    IsoStop stop = iso_queue_run(&queue, &hooks, &capture);
    const UvcFrame *frame = &capture.frame;
    const UvcStats *stats = &frame->stats;
    report("Capture ended: %s; elapsed %llu ms (including drain).\n",
           stop_description(stop),
           (unsigned long long)ticks_to_millisecs(gettime() - capture.start));
    report("Queue submitted %u, consumed %u, drained %u; max consume gap %llu us\n",
           queue.submitted, queue.consumed, queue.drained,
           (unsigned long long)ticks_to_microsecs(capture.max_batch_gap));
    for (unsigned i = 0; i < queue.drained; ++i)
        report("Completion drained after stop: %d\n", queue.drain_results[i]);
    report("Batches %u, slots %u, bytes %u, length %u..%u\n", capture.batches,
           stats->packets, stats->bytes, stats->packets ? capture.min_length : 0,
           capture.max_length);
    report("Empty %u, header-only %u, payload %u (%u bytes)\n",
           stats->empty, stats->header_only, stats->payload_packets, stats->payload_bytes);
    report("Bad headers %u, ERR %u, EOF %u, FID changes %u\n",
           stats->bad_headers, stats->errors, stats->eof, stats->fid_changes);
    report("Dropped %u: missing EOF %u, empty frames %u\n",
           frame->dropped, stats->missing_eof, stats->empty_frames);
    report("Invalid JPEG %u, overflow %u\n", stats->invalid_jpeg, stats->overflow);
    report("JPEG candidates %u, decode rejected %u, oversized slots %u\n",
           stats->complete, capture.rejected, capture.oversized);
    report("Unfinished frame: %u bytes, discard=%u, ended=%u\n",
           (unsigned)frame->size, frame->discard, frame->ended);
    if (stats->header_only && !stats->payload_bytes && !stats->bad_headers)
        report("Only UVC headers received; no image payload reached the decoder.\n");
    /* All queued transfers have returned ownership before diagnostic I/O. */
    if (log_file && capture.batches) {
        report("First batch completion: %d; packet samples in SD log.\n", capture.first_result);
        for (unsigned i = 0; i < ISO_PACKETS; ++i) {
            log_sample("Requested-stride sample", &capture.first[i]);
            if (capture.packed[i].offset != UINT32_MAX)
                log_sample("Packed-offset comparison (not parsed)", &capture.packed[i]);
            else
                log_text("Packed comparison unavailable after invalid returned size.\n");
        }
        if (capture.largest.copied)
            log_sample("Largest packet", &capture.largest);
        if (capture.first_payload.copied)
            log_sample("First apparent payload", &capture.first_payload);
        if (capture.first_error.copied)
            log_sample("First apparent ERR flag", &capture.first_error);
        fflush(log_file);
    }
    report("Preview decoded %u, skipped while busy %u.\n", capture.decoded, capture.skipped);
    report("Preview displayed %u; first decode +%u ms; longest decode %llu us\n",
           capture.displayed, capture.first_decoded_ms,
           (unsigned long long)ticks_to_microsecs(capture.max_decode_time));
    if (atomic_load(&capture.preview.shutter))
        report("Shutter pressed at viewfinder +%u ms\n", capture.shutter_ms);
    return stop;
}

static bool save_photo(unsigned width, unsigned height, char path[80])
{
    if (!mounted || !ensure_directory("sd:/DCIM") ||
        !ensure_directory("sd:/DCIM/100WC04")) {
        report("SD photo directory unavailable.\n");
        return false;
    }
    int fd = -1;
    for (unsigned number = 1; number <= 9999; ++number) {
        snprintf(path, 80, "sd:/DCIM/100WC04/WC04%04u.JPG", number);
        fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd >= 0)
            break;
        if (errno != EEXIST) {
            report("Photo creation failed: %s\n", strerror(errno));
            return false;
        }
    }
    if (fd < 0) {
        report("100WC04 is full (9999 photos).\n");
        return false;
    }
    FILE *file = fdopen(fd, "wb");
    if (!file) {
        close(fd);
        unlink(path);
        return false;
    }
    bool ok = photo_encode(file, rgb, width, height);
    if (fflush(file) != 0)
        ok = false;
    if (fclose(file) != 0)
        ok = false;
    if (!ok) {
        unlink(path);
        report("JPEG write failed; incomplete file removed.\n");
    }
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
    if (!xfb[0] || !xfb[1])
        return 1;
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
    report("\nWC04 Camera 0.2.0 experimental - IOS %u\n", IOS_GetVersion());
    report("Log timestamps: Wii clock and monotonic seconds since app start.\n");
    report("0.1.3 captures and Photo Channel playback confirmed by tester; new viewfinder pending hardware test.\n");
    bool usb_ready = false;
    if (IOS_GetVersion() == 58)
        usb_ready = buffers_ready && USB_Initialize() >= 0;
    else
        report("Launch under IOS58. This app does not install/reload IOS.\n");
    unsigned width = 320, height = 240;
    char status[80];
    snprintf(status, sizeof(status), "%s", !usb_ready ? "CAMERA NEEDS IOS58 AND USB" :
             mounted ? "CONNECT CAMERA THEN PRESS A" : "SD MISSING - PREVIEW ONLY");
    bool menu = true, open_live = false;
    while (!exit_requested) {
        if (menu) {
            draw_screen(SCREEN_MENU, NULL, width, height, status);
            menu = false;
        }
        uint32_t down = buttons();
        if (exit_requested)
            break;
        if (down & WPAD_BUTTON_1) {
            width = width == 320 ? 640 : 320;
            height = width == 320 ? 240 : 480;
            menu = true;
        }
        if (usb_ready && (open_live || (down & (WPAD_BUTTON_A | WPAD_BUTTON_2)))) {
            bool scanning = !open_live && !(down & WPAD_BUTTON_A);
            open_live = false;
            draw_screen(SCREEN_WAITING, NULL, width, height, "CONNECTING CAMERA");
            s32 handle;
            snprintf(status, sizeof(status), "CAMERA NOT FOUND - CHECK USB");
            if (find_camera(&handle)) {
                UvcMode mode;
                UvcAlternate alt;
                bool mode_ok = read_mode(handle, width, height, &mode);
                IsoStop result = ISO_SUBMIT_FAILED;
                snprintf(status, sizeof(status), "%s", mode_ok ? "CAMERA FOUND - READY" :
                         "CAMERA SETUP FAILED - SEE LOG");
                if (mode_ok && !scanning) {
                    if (negotiate(handle, &mode, &alt))
                        result = receive_photo(handle, &mode, &alt);
                    s32 stopped = WC04_SetAlternate(handle, 0);
                    report("Stop streaming: %ld\n", (long)stopped);
                    snprintf(status, sizeof(status), "%s", result == ISO_CANCELLED ?
                             "READY WHEN YOU ARE" : result == ISO_TIMEOUT ?
                             "NO FRAMES - CHECK CAMERA AND LOG" : "CAMERA STOPPED - SEE LOG");
                }
                USB_CloseDevice(&handle);
                if (result == ISO_PHOTO_READY) {
                    char path[80];
                    draw_screen(SCREEN_REVIEW, rgb, width, height, "SAVING PHOTO");
                    bool saved = save_photo(width, height, path);
                    if (saved) {
                        report("Saved %s\n", path);
                        snprintf(status, sizeof(status), "SAVED %s", strrchr(path, '/') + 1);
                    } else
                        snprintf(status, sizeof(status), "SAVE FAILED - CHECK SD AND LOG");
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
    if (usb_ready)
        USB_Deinitialize();
    if (log_file)
        fclose(log_file);
    if (mounted)
        fatUnmount("sd:");
    WPAD_Shutdown();
    return 0;
}
