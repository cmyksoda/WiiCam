/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 cmyksoda
 */

#include "uvc.h"
#include <string.h>

uint16_t uvc_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}

uint32_t uvc_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void uvc_put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> (8 * i));
}

bool uvc_parse(const uint8_t *data, size_t length, unsigned width,
               unsigned height, UvcMode *mode)
{
    memset(mode, 0, sizeof(*mode));
    if (length < 9 || data[0] != 9 || data[1] != 2 ||
        uvc_le16(data + 2) != length)
        return false;
    mode->configuration = data[5];
    unsigned interface = 255, alternate = 0, subclass = 0, format = 0;
    bool video = false, found = false;
    for (size_t offset = 9; offset < length;) {
        if (length - offset < 2)
            return false;
        const uint8_t *d = data + offset;
        size_t n = d[0];
        if (n < 2 || n > length - offset)
            return false;
        if (d[1] == 4) {
            if (n < 9)
                return false;
            interface = d[2];
            alternate = d[3];
            video = d[5] == 14;
            subclass = d[6];
            format = 0;
        } else if (d[1] == 0x24 && video) {
            if (n < 3)
                return false;
            if (subclass == 1 && d[2] == 1 && n >= 5)
                mode->version = uvc_le16(d + 3);
            if (subclass == 2 && alternate == 0) {
                if (d[2] == 6 && n >= 11)
                    format = d[3];
                else if (d[2] == 4 || d[2] == 0x10)
                    format = 0;
                else if (d[2] == 7 && format && n >= 26 && !found &&
                         uvc_le16(d + 5) == width && uvc_le16(d + 7) == height) {
                    mode->interface = (uint8_t)interface;
                    mode->format = (uint8_t)format;
                    mode->frame = d[3];
                    mode->width = (uint16_t)width;
                    mode->height = (uint16_t)height;
                    mode->max_frame = uvc_le32(d + 17);
                    mode->interval = uvc_le32(d + 21);
                    found = true;
                }
            }
        } else if (d[1] == 5 && found && video && subclass == 2 &&
                   interface == mode->interface && alternate) {
            if (n < 7)
                return false;
            if ((d[2] & 0x80) && (d[3] & 3) == 1 && d[6] == 1) {
                unsigned raw = uvc_le16(d + 4);
                unsigned multiplier = (raw >> 11) & 3;
                if (multiplier == 3 || !(raw & 0x7ff) || (raw & 0xe000) ||
                    mode->alt_count == WC04_MAX_ALTS)
                    return false;
                UvcAlternate *a = &mode->alts[mode->alt_count++];
                a->number = (uint8_t)alternate;
                a->endpoint = d[2];
                a->capacity = (uint16_t)((raw & 0x7ff) * (multiplier + 1));
                mode->endpoint = d[2];
            }
        }
        offset += n;
    }
    return found && mode->version == 0x100 && mode->alt_count &&
           mode->interval && mode->max_frame &&
           mode->max_frame <= WC04_FRAME_CAPACITY;
}

bool uvc_alternate(const UvcMode *mode, uint32_t payload, UvcAlternate *alt)
{
    bool found = false;
    if (!payload)
        return false;
    for (size_t i = 0; i < mode->alt_count; ++i) {
        const UvcAlternate *candidate = &mode->alts[i];
        if (candidate->capacity >= payload &&
            (!found || candidate->capacity < alt->capacity)) {
            *alt = *candidate;
            found = true;
        }
    }
    return found;
}

void uvc_probe_select(uint8_t data[UVC_PROBE_LENGTH], const UvcMode *mode)
{
    /* Preserve camera-provided compression and timing fields. */
    data[0] = 1;
    data[1] = 0;
    data[2] = mode->format;
    data[3] = mode->frame;
    uvc_put32(data + 4, mode->interval);
}

bool uvc_negotiate(const UvcMode *mode, UvcAlternate *alt,
                   uint8_t data[UVC_PROBE_LENGTH], UvcControlTransfer transfer,
                   void *context)
{
    const uint8_t seeds[] = {UVC_GET_MAX, UVC_GET_DEF, UVC_GET_CUR};
    bool seeded = false;
    for (size_t i = 0; i < sizeof(seeds); ++i) {
        memset(data, 0, UVC_PROBE_LENGTH);
        if (transfer(context, seeds[i], UVC_VS_PROBE, data) == UVC_PROBE_LENGTH) {
            seeded = true;
            break;
        }
    }
    if (!seeded)
        return false;
    uvc_probe_select(data, mode);
    if (transfer(context, UVC_SET_CUR, UVC_VS_PROBE, data) != UVC_PROBE_LENGTH)
        return false;
    memset(data, 0, UVC_PROBE_LENGTH);
    if (transfer(context, UVC_GET_CUR, UVC_VS_PROBE, data) != UVC_PROBE_LENGTH)
        return false;
    uint32_t max_frame = uvc_le32(data + 18);
    if (data[2] != mode->format || data[3] != mode->frame ||
        !uvc_le32(data + 4) || !max_frame || max_frame > WC04_FRAME_CAPACITY ||
        !uvc_alternate(mode, uvc_le32(data + 22), alt))
        return false;
    /* Commit exactly the accepted GET_CUR block, including adjusted fields. */
    return transfer(context, UVC_SET_CUR, UVC_VS_COMMIT, data) == UVC_PROBE_LENGTH;
}

void uvc_frame_init(UvcFrame *frame, uint8_t *data, size_t capacity)
{
    *frame = (UvcFrame){.data = data, .capacity = capacity, .fid = -1};
}

bool uvc_packet(UvcFrame *frame, const uint8_t *packet, size_t length)
{
    ++frame->stats.packets;
    frame->stats.bytes += length;
    if (!length) {
        ++frame->stats.empty;
        return false;
    }
    if (length < 2 || packet[0] < 2 || packet[0] > length) {
        ++frame->stats.bad_headers;
        frame->discard = true;
        return false;
    }
    unsigned flags = packet[1];
    unsigned minimum_header = 2 + ((flags & 4) ? 4 : 0) + ((flags & 8) ? 6 : 0);
    size_t payload = length - packet[0];
    if (packet[0] < minimum_header)
        ++frame->stats.bad_headers;
    else if (payload) {
        ++frame->stats.payload_packets;
        frame->stats.payload_bytes += payload;
    } else
        ++frame->stats.header_only;
    if (flags & 0x40)
        ++frame->stats.errors;
    if (flags & 2)
        ++frame->stats.eof;
    int fid = flags & 1;
    if (fid != frame->fid) {
        if (frame->fid != -1)
            ++frame->stats.fid_changes;
        if (frame->size && !frame->ended) {
            ++frame->dropped;
            ++frame->stats.missing_eof;
        }
        frame->size = 0;
        frame->discard = false;
        frame->ended = false;
        frame->fid = fid;
    }
    if (frame->ended)
        return false;
    if (payload > frame->capacity - frame->size)
        ++frame->stats.overflow;
    if ((flags & 0x40) || packet[0] < minimum_header ||
        payload > frame->capacity - frame->size)
        frame->discard = true;
    if (!frame->discard && payload) {
        memcpy(frame->data + frame->size, packet + packet[0], payload);
        frame->size += payload;
    }
    if (!(flags & 2))
        return false;
    frame->ended = true;
    if (frame->discard || frame->size < 4 || frame->data[0] != 0xff ||
        frame->data[1] != 0xd8 || frame->data[frame->size - 2] != 0xff ||
        frame->data[frame->size - 1] != 0xd9) {
        ++frame->dropped;
        if (!frame->discard) {
            if (!frame->size)
                ++frame->stats.empty_frames;
            else
                ++frame->stats.invalid_jpeg;
        }
        return false;
    }
    ++frame->stats.complete;
    return true;
}
