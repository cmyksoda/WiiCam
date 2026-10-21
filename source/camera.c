/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */
#include "camera.h"
#include <string.h>
#include <stdio.h>

static void report(const CameraOps *ops, const CameraEntry *entry, const char *message)
{
    if (ops->report) ops->report(ops->context, entry, message);
}

/* IOS GETDEVPARAMS returns CPU-order (big-endian) descriptor fields.
 * Raw USB GET_DESCRIPTOR and UVC controls are different: they stay little-endian.
 * See the pinned libogc USBV5_GetDescriptors and Dolphin VEN GetDeviceInfo paths. */
static uint16_t ios_be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

static const char *describe(const CameraEntry *entry, const uint8_t *data, uint8_t *configuration)
{
    /* Read only the bounded first view, not counts beyond the 192-byte reply. */
    const uint8_t *device = data + 20, *config = data + 40, *interface = data + 52;
    if (device[0] != 18 || device[1] != 1) return "invalid IOS device descriptor header";
    if (device[17] != 1) return "unsupported IOS configuration count (requires one)";
    if (ios_be16(device + 8) != entry->vid || ios_be16(device + 10) != entry->pid)
        return "IOS vendor/product identity differs from enumeration";
    if (config[0] != 9 || config[1] != 2 || !config[4] || !config[5])
        return "invalid IOS configuration descriptor";
    if (interface[0] != 9 || interface[1] != 4) return "invalid IOS interface descriptor header";
    if (interface[2] != entry->interface) return "IOS interface number differs from enumeration";
    if (interface[5] != 14 || interface[6] != 2)
        return "not a VideoStreaming interface (requires class 0e/subclass 02)";
    *configuration = config[5];
    return NULL;
}

/* Discovery runs before streaming. At most two header lines plus four
 * eight-interval lines per rejected candidate; no strings or raw descriptors. */
static void report_limit(const CameraOps *ops, const CameraEntry *entry, unsigned width,
                         unsigned height, const UvcMode *mode)
{
    char line[192];
    snprintf(line, sizeof(line), "UVC limit: requested=%ux%u reason=%s value=%lu bound=%lu", width,
             height, mode->limit_reason ? mode->limit_reason : "unknown",
             (unsigned long)mode->limit_value, (unsigned long)mode->limit_bound);
    report(ops, entry, line);
    snprintf(
        line, sizeof(line),
        "Descriptor: format=%u frame=%u max_frame=%lu cap=%u default_interval=%lu interval_count=%u",
        mode->format, mode->frame, (unsigned long)mode->max_frame, WC04_FRAME_CAPACITY,
        (unsigned long)mode->interval, mode->interval_count);
    report(ops, entry, line);
    if (!mode->interval_count) {
        snprintf(line, sizeof(line), "Continuous intervals: min=%lu max=%lu step=%lu",
                 (unsigned long)mode->interval_min, (unsigned long)mode->interval_max,
                 (unsigned long)mode->interval_step);
        report(ops, entry, line);
    } else if (mode->interval_count <= UVC_MAX_INTERVALS) {
        for (unsigned i = 0; i < mode->interval_count; i += 8) {
            unsigned used =
                (unsigned)snprintf(line, sizeof(line), "Discrete intervals from index %u:", i);
            for (unsigned j = i; j < mode->interval_count && j < i + 8; ++j)
                used += (unsigned)snprintf(line + used, sizeof(line) - used, " %lu",
                                           (unsigned long)mode->intervals[j]);
            report(ops, entry, line);
        }
    }
}

bool camera_find(const CameraEntry *entries, size_t count, unsigned width, unsigned height,
                 uint8_t *scratch, size_t capacity, const CameraOps *ops, int32_t *handle,
                 UvcMode *mode)
{
    if (!entries || count > CAMERA_MAX_DEVICES || !scratch || capacity < CAMERA_CONFIG_CAPACITY
        || !ops || !ops->open || !ops->parameters || !ops->read || !ops->close || !handle || !mode)
        return false;
    /* Deterministic preference for WC04, without excluding other identities. */
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < count; ++i) {
            const CameraEntry *entry = &entries[i];
            bool wc04 = entry->vid == 0x057e && entry->pid == 0x030a;
            if (wc04 != (pass == 0)) continue;
            int32_t opened = 0;
            if (ops->open(ops->context, entry, &opened) < 0) {
                report(ops, entry, "open failed");
                continue;
            }

            memset(scratch, 0, CAMERA_IOS_PARAMETERS);
            uint8_t configuration = 0;
            int result = ops->parameters(ops->context, opened, scratch);
            char detail[160];
            if (result != 0) {
                snprintf(detail, sizeof(detail), "IOS GETDEVPARAMS failed: %d", result);
                report(ops, entry, detail);
                ops->close(ops->context, opened);
                continue;
            }

            /* Numeric descriptor metadata only; no strings, serials or image data. */
            snprintf(
                detail, sizeof(detail),
                "IOS descriptor fields: id=%04x:%04x configs=%u config=%u interface=%u class=%02x subclass=%02x",
                (unsigned)ios_be16(scratch + 28), (unsigned)ios_be16(scratch + 30),
                (unsigned)scratch[37], (unsigned)scratch[45], (unsigned)scratch[54],
                (unsigned)scratch[57], (unsigned)scratch[58]);
            report(ops, entry, detail);

            const char *reason = describe(entry, scratch, &configuration);
            if (reason) {
                report(ops, entry, reason);
                ops->close(ops->context, opened);
                continue;
            }

            memset(scratch, 0, CAMERA_CONFIG_CAPACITY);
            result = ops->read(ops->context, opened, 8, 0, 1, scratch);
            if (uvc_control_payload_length(result, 1) != 1 || scratch[0] != configuration) {
                report(ops, entry,
                       "configuration inactive or unreadable; no configuration change attempted");
                ops->close(ops->context, opened);
                continue;
            }

            memset(scratch, 0, CAMERA_CONFIG_CAPACITY);
            result = ops->read(ops->context, opened, 6, 0x0200, 9, scratch);
            unsigned length = uvc_le16(scratch + 2);
            if (uvc_control_payload_length(result, 9) != 9 || scratch[0] != 9 || scratch[1] != 2
                || length < 9 || length > CAMERA_CONFIG_CAPACITY || scratch[5] != configuration) {
                report(ops, entry, "configuration header short, malformed or beyond 4096 bytes");
                ops->close(ops->context, opened);
                continue;
            }

            memset(scratch, 0, CAMERA_CONFIG_CAPACITY);
            result = ops->read(ops->context, opened, 6, 0x0200, (uint16_t)length, scratch);
            if (uvc_control_payload_length(result, length) != (int)length
                || scratch[5] != configuration) {
                report(ops, entry, "configuration read short or changed");
                ops->close(ops->context, opened);
                continue;
            }

            UvcParseResult parsed =
                uvc_parse(scratch, length, entry->interface, width, height, mode);
            if (parsed == UVC_LIMIT) report_limit(ops, entry, width, height, mode);
            report(ops, entry, uvc_parse_description(parsed));
            if (parsed != UVC_OK) {
                ops->close(ops->context, opened);
                continue;
            }
            mode->supported_sizes = uvc_supported_sizes(scratch, length, entry->interface);
            *handle = opened;
            return true;
        }
    }
    memset(mode, 0, sizeof(*mode));
    return false;
}
