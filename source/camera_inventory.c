/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */
#include "camera_inventory.h"
#include "camera.h"
#include "uac.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    CameraInventory *result;
    CameraInventoryReport report;
    void *context;
} InventoryLog;

static void emit(InventoryLog *log, const char *format, ...)
{
    if (log->result->lines >= CAMERA_INVENTORY_LINES - 1) {
        log->result->truncated = true;
        return;
    }
    char line[CAMERA_INVENTORY_LINE_BYTES];
    memcpy(line, "Inventory: ", 11);
    va_list args;
    va_start(args, format);
    int length = vsnprintf(line + 11, sizeof(line) - 11, format, args);
    va_end(args);
    if (length < 0 || (size_t)length >= sizeof(line) - 11) {
        log->result->truncated = true;
        return;
    }
    ++log->result->lines;
    if (log->report) log->report(log->context, line);
}

static uint32_t le24(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
}

static bool video_descriptor(InventoryLog *log, const uint8_t *d, unsigned size, unsigned interface,
                             unsigned *format)
{
    if (size < 3) return false;
    if (d[2] == 6) {
        if (size < 11) return false;
        *format = d[3];
        emit(log,
             "MJPEG if=%u format=%u frames=%u flags=%02x default=%u aspect=%u:%u interlace=%02x",
             interface, *format, d[4], d[5], d[6], d[7], d[8], d[9]);
    } else if (d[2] == 7) {
        if (size < 26) return false;
        unsigned count = d[25];
        if (size < (count ? 26 + count * 4 : 38)) return false;
        ++log->result->mjpeg_frames;
        emit(
            log,
            "MJPEG if=%u format=%u frame=%u size=%ux%u max_frame=%lu default_100ns=%lu intervals=%u",
            interface, *format, d[3], uvc_le16(d + 5), uvc_le16(d + 7),
            (unsigned long)uvc_le32(d + 17), (unsigned long)uvc_le32(d + 21), count);
        if (!count) {
            emit(log, "MJPEG continuous_100ns min=%lu max=%lu step=%lu",
                 (unsigned long)uvc_le32(d + 26), (unsigned long)uvc_le32(d + 30),
                 (unsigned long)uvc_le32(d + 34));
        } else {
            for (unsigned n = 0; n < count; n += 8) {
                char list[100] = {0};
                unsigned used = 0;
                for (unsigned i = n; i < count && i < n + 8; ++i)
                    used += (unsigned)snprintf(list + used, sizeof(list) - used, " %lu",
                                               (unsigned long)uvc_le32(d + 26 + i * 4));
                emit(log, "MJPEG intervals_100ns from=%u:%s", n, list);
            }
        }
    } else if (d[2] == 4 || d[2] == 0x10 || d[2] == 0x12) {
        *format = 0;
    }
    return true;
}

static bool audio_descriptor(InventoryLog *log, const uint8_t *d, unsigned size, unsigned interface,
                             unsigned alt, unsigned subclass, unsigned protocol)
{
    if (size < 3) return false;
    /* Retain complete numeric topology, including unknown units, in bounded lines.
     * These are configuration bytes, never string descriptors or microphone data. */
    for (unsigned at = 0; at < size; at += 16) {
        char bytes[49] = {0};
        for (unsigned n = at; n < size && n < at + 16; ++n)
            snprintf(bytes + (n - at) * 3, sizeof(bytes) - (n - at) * 3, "%02x ", d[n]);
        emit(log, "audio if=%u alt=%u descriptor=%02x/%02x offset=%u bytes=%s", interface, alt,
             d[1], d[2], at, bytes);
    }
    if (protocol != 0) return true;
    if (d[1] == 0x25 && subclass == 2 && d[2] == 1) {
        if (size < 7) return false;
        emit(log, "UAC1 endpoint controls=%02x frequency_control=%u lock_units=%u lock_delay=%u",
             d[3], d[3] & 1, d[4], uvc_le16(d + 5));
    } else if (d[1] == 0x24 && subclass == 1 && d[2] == 1) {
        if (size < 8 || size < 8u + d[7]) return false;
        emit(log, "audio control if=%u version=%04x total=%u streaming_interfaces=%u", interface,
             uvc_le16(d + 3), uvc_le16(d + 5), d[7]);
    } else if (d[1] == 0x24 && subclass == 2 && d[2] == 1) {
        if (size < 7) return false;
        emit(log, "UAC1 stream if=%u alt=%u terminal=%u delay=%u format_tag=%04x", interface, alt,
             d[3], d[4], uvc_le16(d + 5));
    } else if (d[1] == 0x24 && subclass == 2 && d[2] == 2) {
        if (size < 4) return false;
        if (d[3] != 1) return true;
        if (size < 8) return false;
        unsigned count = d[7];
        if (size < (count ? 8 + count * 3 : 14)) return false;
        ++log->result->audio_formats;
        emit(log, "UAC1 TypeI if=%u alt=%u channels=%u sample_bytes=%u bits=%u rates=%u", interface,
             alt, d[4], d[5], d[6], count);
        if (!count) {
            emit(log, "UAC1 continuous_hz min=%lu max=%lu", (unsigned long)le24(d + 8),
                 (unsigned long)le24(d + 11));
        } else {
            for (unsigned n = 0; n < count; ++n)
                emit(log, "UAC1 rate_hz index=%u value=%lu", n, (unsigned long)le24(d + 8 + n * 3));
        }
    }
    return true;
}

bool camera_inventory(const uint8_t *data, size_t length, CameraInventoryReport report,
                      void *context, CameraInventory *inventory)
{
    if (!inventory) return false;
    *inventory = (CameraInventory){0};
    InventoryLog log = {inventory, report, context};
    unsigned interface = 0, alt = 0, class = 0, subclass = 0, protocol = 0, format = 0;
    bool have_interface = false;
    if (!data || length < 9 || length > CAMERA_CONFIG_CAPACITY || data[0] != 9 || data[1] != 2
        || uvc_le16(data + 2) != length) {
        inventory->malformed = true;
    } else {
        emit(&log, "config=%u bytes=%u; advertised descriptors, stream support not implied",
             data[5], (unsigned)length);
        for (size_t at = 9; at < length;) {
            if (length - at < 2 || data[at] < 2 || data[at] > length - at) {
                inventory->malformed = true;
                break;
            }
            const uint8_t *d = data + at;
            unsigned size = d[0];
            ++inventory->descriptors;
            bool valid = true;
            if (d[1] == 4) {
                if (size < 9) {
                    inventory->malformed = true;
                    break;
                }
                interface = d[2];
                alt = d[3];
                class = d[5];
                subclass = d[6];
                protocol = d[7];
                have_interface = true;
                format = 0;
                emit(&log,
                     "interface=%u alt=%u endpoints=%u class=%02x subclass=%02x protocol=%02x",
                     interface, alt, d[4], class, subclass, protocol);
                if (class == 1 && subclass == 2 && protocol == 0 && alt) {
                    UacMode audio;
                    UacResult parsed = uac_parse(data, length, interface, alt, &audio);
                    if (parsed == UAC_OK)
                        emit(
                            &log,
                            "PCM16 candidate if=%u alt=%u ep=%02x mic=%u channels=%u rate=%lu packet=%u interval=%u frequency_control=%u",
                            audio.interface, audio.alternate, audio.endpoint, audio.microphone,
                            audio.channels, (unsigned long)audio.rate, audio.packet_capacity,
                            audio.interval, audio.frequency_control);
                    else
                        emit(&log, "PCM16 candidate if=%u alt=%u validation=%u", interface, alt,
                             parsed);
                }
            } else if (have_interface && d[1] == 5) {
                if (size < 7) {
                    inventory->malformed = true;
                    break;
                }
                emit(
                    &log,
                    "endpoint if=%u alt=%u address=%02x attributes=%02x max_packet_raw=%04x interval=%u",
                    interface, alt, d[2], d[3], uvc_le16(d + 4), d[6]);
                if (class == 1 && size >= 9)
                    emit(&log, "audio endpoint refresh=%u synch_address=%02x", d[7], d[8]);
            } else if (have_interface && class == 14 && subclass == 2 && d[1] == 0x24) {
                valid = video_descriptor(&log, d, size, interface, &format);
            } else if (have_interface && class == 1 && (d[1] == 0x24 || d[1] == 0x25)) {
                valid = audio_descriptor(&log, d, size, interface, alt, subclass, protocol);
            }
            if (!valid) {
                inventory->malformed = true;
                break;
            }
            at += size;
        }
    }
    char summary[CAMERA_INVENTORY_LINE_BYTES];
    snprintf(
        summary, sizeof(summary),
        "Inventory: end descriptors=%u mjpeg_frames=%u audio_formats=%u malformed=%u truncated=%u",
        inventory->descriptors, inventory->mjpeg_frames, inventory->audio_formats,
        inventory->malformed, inventory->truncated);
    ++inventory->lines;
    if (report) report(context, summary);
    return !inventory->malformed;
}
