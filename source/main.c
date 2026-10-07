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

#define ISO_PACKETS 16
#define ISO_MAX_PAYLOAD 3072
#define CAPTURE_SECONDS 10
#define CONFIG_CAPACITY 4096

static uint8_t *config;
static uint8_t *probe;
static uint8_t *iso_data;
static uint16_t *iso_sizes;
static uint8_t frame_data[WC04_FRAME_CAPACITY];
static uint8_t rgb[640 * 480 * 3];
static volatile bool transfer_done;
static volatile s32 transfer_result;
static volatile bool exit_requested;
static bool mounted;
static FILE *log_file;
static uint64_t session_start;
static void *xfb;
static GXRModeObj *video;

s32 WC04_SetAlternate(s32 device_id, u8 alternate);
s32 WC04_CancelIso(s32 device_id, u8 endpoint);

static bool allocate_usb_buffers(void)
{
    const size_t bytes = CONFIG_CAPACITY + 32 + ISO_PACKETS * ISO_MAX_PAYLOAD + 32;
    uintptr_t high = (uintptr_t)SYS_GetArena2Hi() & ~(uintptr_t)31;
    uintptr_t low = (uintptr_t)SYS_GetArena2Lo();
    if (high < low || high - low < bytes)
        return false;
    /* Reserve MEM2 before other subsystems start; leave the small IPC heap free. */
    config = (uint8_t *)(high - bytes);
    SYS_SetArena2Hi(config);
    probe = config + CONFIG_CAPACITY;
    iso_data = probe + 32;
    iso_sizes = (uint16_t *)(iso_data + ISO_PACKETS * ISO_MAX_PAYLOAD);
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
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
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
    (void)context;
    transfer_result = result;
    __sync_synchronize();
    transfer_done = true;
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

static void sample_packet(PacketSample *sample, unsigned slot, unsigned length,
                          unsigned offset, uint64_t start)
{
    sample->slot = slot;
    sample->length = length;
    sample->offset = offset;
    sample->elapsed_ms = ticks_to_millisecs(gettime() - start);
    sample->copied = length < sizeof(sample->data) ? length : sizeof(sample->data);
    memcpy(sample->data, iso_data + offset, sample->copied);
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

static const char *capture_stop(uint64_t start)
{
    uint32_t down = buttons();
    if (exit_requested)
        return "HOME/power/reset exit requested";
    if (down & WPAD_BUTTON_B)
        return "B pressed";
    if (ticks_to_millisecs(gettime() - start) >= CAPTURE_SECONDS * 1000)
        return "10-second capture timeout";
    return NULL;
}

static bool receive_photo(s32 handle, const UvcMode *mode, const UvcAlternate *alt)
{
    UvcFrame frame;
    uvc_frame_init(&frame, frame_data, sizeof(frame_data));
    uint64_t start = gettime();
    unsigned rejected = 0, oversized = 0, batches = 0;
    unsigned min_length = ISO_MAX_PAYLOAD, max_length = 0;
    PacketSample first[ISO_PACKETS] = {0}, packed[ISO_PACKETS] = {0};
    PacketSample first_payload = {0}, first_error = {0}, largest = {0};
    s32 first_result = 0;
    bool success = false;
    const char *stop = NULL;
    report("Capturing... B cancels, HOME exits.\n");
    while (!success && !stop) {
        stop = capture_stop(start);
        if (stop) {
            report("Capture stop requested: %s.\n", stop);
            break;
        }
        memset(iso_data, 0, ISO_PACKETS * ISO_MAX_PAYLOAD);
        for (unsigned i = 0; i < ISO_PACKETS; ++i)
            iso_sizes[i] = alt->capacity;
        transfer_done = false;
        s32 result = USB_ReadIsoMsgAsync(handle, alt->endpoint, ISO_PACKETS,
                                         iso_sizes, iso_data, iso_finished, NULL);
        if (result < 0) {
            report("Isochronous submission failed: %ld\n", (long)result);
            stop = "USB submission failure";
            break;
        }
        while (!transfer_done) {
            if (!stop)
                stop = capture_stop(start);
            if (stop) {
                report("Capture stop requested: %s.\n", stop);
                s32 cancel = WC04_CancelIso(handle, alt->endpoint);
                report("Cancel endpoint: %ld. Waiting for USB completion.\n", (long)cancel);
                report("If stuck, unplug camera.\n");
                while (!transfer_done) {
                    buttons();
                    usleep(100);
                }
            }
            /* Keep buffers alive until IOS returns ownership, even on cancel. */
            usleep(100);
        }
        __sync_synchronize();
        if (stop) {
            report("Isochronous completion after stop request: %ld\n", (long)transfer_result);
            break;
        }
        if (transfer_result < 0) {
            report("Isochronous completion failed: %ld\n", (long)transfer_result);
            stop = "USB completion failure before cancellation";
            break;
        }
        ++batches;
        if (batches == 1) {
            first_result = transfer_result;
            unsigned packed_offset = 0;
            bool packed_valid = true;
            for (unsigned i = 0; i < ISO_PACKETS; ++i) {
                unsigned length = iso_sizes[i];
                unsigned bounded = length < alt->capacity ? length : alt->capacity;
                sample_packet(&first[i], i, bounded, i * alt->capacity, start);
                first[i].length = length;
                if (length > alt->capacity)
                    packed_valid = false;
                if (packed_valid) {
                    sample_packet(&packed[i], i, length, packed_offset, start);
                    packed_offset += length;
                } else
                    packed[i].offset = UINT32_MAX;
            }
        }
        for (unsigned i = 0; i < ISO_PACKETS; ++i) {
            unsigned length = iso_sizes[i];
            if (length > alt->capacity) {
                frame.discard = true;
                ++oversized;
                continue;
            }
            unsigned offset = i * alt->capacity;
            const uint8_t *packet = iso_data + offset;
            if (length < min_length)
                min_length = length;
            if (length > max_length) {
                max_length = length;
                sample_packet(&largest, i, length, offset, start);
            }
            if (length >= 2) {
                if (!first_payload.copied && packet[0] >= 2 && packet[0] < length)
                    sample_packet(&first_payload, i, length, offset, start);
                if (!first_error.copied && (packet[1] & 0x40))
                    sample_packet(&first_error, i, length, offset, start);
            }
            /* Slots retain requested strides; returned sizes are actual lengths. */
            if (uvc_packet(&frame, packet, length)) {
                if (photo_decode(frame.data, frame.size, rgb, mode->width, mode->height)) {
                    success = true;
                    break;
                }
                ++rejected;
            }
        }
    }
    const UvcStats *stats = &frame.stats;
    report("Capture ended: %s; elapsed %llu ms (including drain).\n",
           success ? "decoded JPEG" : stop, (unsigned long long)ticks_to_millisecs(gettime() - start));
    report("Batches %u, slots %u, bytes %u, length %u..%u\n", batches,
           stats->packets, stats->bytes, stats->packets ? min_length : 0, max_length);
    report("Empty %u, header-only %u, payload %u (%u bytes)\n",
           stats->empty, stats->header_only, stats->payload_packets, stats->payload_bytes);
    report("Bad headers %u, ERR %u, EOF %u, FID changes %u\n",
           stats->bad_headers, stats->errors, stats->eof, stats->fid_changes);
    report("Dropped %u: missing EOF %u, empty frames %u\n",
           frame.dropped, stats->missing_eof, stats->empty_frames);
    report("Invalid JPEG %u, overflow %u\n", stats->invalid_jpeg, stats->overflow);
    report("JPEG candidates %u, decode rejected %u, oversized slots %u\n",
           stats->complete, rejected, oversized);
    report("Unfinished frame: %u bytes, discard=%u, ended=%u\n",
           (unsigned)frame.size, frame.discard, frame.ended);
    if (stats->header_only && !stats->payload_bytes && !stats->bad_headers)
        report("Only UVC headers received; no image payload reached the decoder.\n");
    /* Defer diagnostic I/O until IOS has returned the capture buffer. */
    if (log_file && batches) {
        report("First batch completion: %ld; packet samples in SD log.\n", (long)first_result);
        for (unsigned i = 0; i < ISO_PACKETS; ++i) {
            log_sample("Requested-stride sample", &first[i]);
            if (packed[i].offset != UINT32_MAX)
                log_sample("Packed-offset comparison (not parsed)", &packed[i]);
            else
                log_text("Packed comparison unavailable after invalid returned size.\n");
        }
        if (largest.copied)
            log_sample("Largest packet", &largest);
        if (first_payload.copied)
            log_sample("First apparent payload", &first_payload);
        if (first_error.copied)
            log_sample("First apparent ERR flag", &first_error);
        fflush(log_file);
    }
    return success;
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

static uint8_t clamp(int value)
{
    return (uint8_t)(value < 0 ? 0 : value > 255 ? 255 : value);
}

static void show_photo(unsigned width, unsigned height)
{
    printf("\x1b[2J\x1b[HWC04 Camera - captured photo\n\nA / B: menu    HOME: exit\n");
    unsigned out_width = width > 320 ? width / 2 : width;
    unsigned out_height = height > 240 ? height / 2 : height;
    unsigned left = ((video->fbWidth - out_width) / 2) & ~1u;
    unsigned top = (video->xfbHeight - out_height) / 2;
    uint8_t *screen = xfb;
    for (unsigned y = 0; y < out_height; ++y) {
        for (unsigned x = 0; x < out_width; x += 2) {
            int yy[2], cb[2], cr[2];
            for (unsigned p = 0; p < 2; ++p) {
                size_t pos = ((size_t)(y * height / out_height) * width +
                              (x + p) * width / out_width) * 3;
                int r = rgb[pos], g = rgb[pos + 1], b = rgb[pos + 2];
                yy[p] = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
                cb[p] = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                cr[p] = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            }
            size_t dest = ((size_t)(top + y) * VIDEO_PadFramebufferWidth(video->fbWidth) + left + x) * 2;
            screen[dest] = clamp(yy[0]);
            screen[dest + 1] = clamp((cb[0] + cb[1]) / 2);
            screen[dest + 2] = clamp(yy[1]);
            screen[dest + 3] = clamp((cr[0] + cr[1]) / 2);
        }
    }
    while (!exit_requested) {
        if (buttons() & (WPAD_BUTTON_A | WPAD_BUTTON_B))
            break;
        VIDEO_WaitVSync();
    }
}

int main(void)
{
    session_start = gettime();
    bool buffers_ready = allocate_usb_buffers();
    VIDEO_Init();
    video = VIDEO_GetPreferredMode(NULL);
    xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(video));
    if (!xfb)
        return 1;
    VIDEO_Configure(video);
    VIDEO_ClearFrameBuffer(video, xfb, COLOR_BLACK);
    console_init(xfb, 20, 20, video->fbWidth - 40, video->xfbHeight - 40,
                 VIDEO_PadFramebufferWidth(video->fbWidth) * 2);
    VIDEO_SetNextFramebuffer(xfb);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    WPAD_Init();
    SYS_SetResetCallback(reset_pressed);
    SYS_SetPowerCallback(request_exit);
    WPAD_SetPowerButtonCallback(remote_power);
    mounted = fatMountSimple("sd", &__io_wiisd);
    if (mounted && ensure_directory("sd:/wc04-camera"))
        log_file = fopen("sd:/wc04-camera/session.log", "a");
    report("\nWC04 Camera 0.1.2 experimental - IOS %u\n", IOS_GetVersion());
    report("Log timestamps: Wii clock and monotonic seconds since app start.\n");
    report("Hardware capture and Photo Channel playback are unverified.\n");
    bool usb_ready = false;
    if (IOS_GetVersion() == 58)
        usb_ready = buffers_ready && USB_Initialize() >= 0;
    else
        report("Launch under IOS58. This app does not install/reload IOS.\n");
    unsigned width = 320, height = 240;
    bool menu = true;
    while (!exit_requested) {
        if (menu) {
            report("\nA: capture %ux%u    1: change resolution\n", width, height);
            report("2: scan camera       HOME: exit\n");
            report("SD: %s   USB: %s\n", mounted ? "mounted" : "unavailable",
                   usb_ready ? "initialized" : "unavailable");
            menu = false;
        }
        uint32_t down = buttons();
        if (down & WPAD_BUTTON_1) {
            width = width == 320 ? 640 : 320;
            height = width == 320 ? 240 : 480;
            menu = true;
        }
        if (usb_ready && (down & (WPAD_BUTTON_A | WPAD_BUTTON_2))) {
            printf("\x1b[2J\x1b[H");
            s32 handle;
            if (find_camera(&handle)) {
                UvcMode mode;
                UvcAlternate alt;
                bool mode_ok = read_mode(handle, width, height, &mode);
                bool captured = false;
                if (mode_ok && (down & WPAD_BUTTON_A)) {
                    if (mounted && negotiate(handle, &mode, &alt))
                        captured = receive_photo(handle, &mode, &alt);
                    else if (!mounted)
                        report("Insert writable SD and restart before capturing.\n");
                    s32 stopped = WC04_SetAlternate(handle, 0);
                    report("Stop streaming: %ld\n", (long)stopped);
                }
                USB_CloseDevice(&handle);
                if (captured) {
                    char path[80];
                    if (save_photo(width, height, path)) {
                        report("Saved %s\nPress A to view.\n", path);
                        while (!exit_requested) {
                            if (buttons() & WPAD_BUTTON_A) {
                                show_photo(width, height);
                                break;
                            }
                            VIDEO_WaitVSync();
                        }
                    }
                } else if (down & WPAD_BUTTON_A)
                    report("No photo saved. See SD session.log for details.\n");
            }
            menu = true;
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
