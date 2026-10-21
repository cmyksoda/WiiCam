/* SPDX-License-Identifier: GPL-3.0-or-later
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
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void uvc_put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(value >> (8 * i));
}

const char *uvc_parse_description(UvcParseResult result)
{
    const char *const descriptions[] = {"supported UVC 1.0 MJPEG descriptors",
                                        "malformed configuration descriptors",
                                        "missing or ambiguous video-control association",
                                        "only UVC 1.0 controls supported",
                                        "no progressive MJPEG mode at selected size",
                                        "frame or interval limits unsupported",
                                        "no matching interval-1 isochronous IN alternate"};
    return (unsigned)result < sizeof(descriptions) / sizeof(descriptions[0])
               ? descriptions[result]
               : "unknown descriptor result";
}

int uvc_control_payload_length(int result, unsigned requested)
{
    if (result < 0) return result;
    if (result < 8 || (unsigned)(result - 8) > requested) return -1;
    return result - 8;
}

bool uvc_interval_supported(const UvcMode *mode, uint32_t interval)
{
    if (!interval) return false;
    if (mode->interval_count) {
        for (unsigned i = 0; i < mode->interval_count; ++i)
            if (mode->intervals[i] == interval) return true;
        return false;
    }
    return mode->interval_step && interval >= mode->interval_min && interval <= mode->interval_max
           && (interval - mode->interval_min) % mode->interval_step == 0;
}

/* Called only after the entire configuration walk has been bounded. */
static size_t class_length(const uint8_t *data, size_t length, size_t offset)
{
    size_t start = offset;
    while (offset < length && data[offset + 1] == 0x24)
        offset += data[offset];
    return offset - start;
}

static UvcParseResult limit(UvcMode *mode, const char *reason, uint32_t value, uint32_t bound)
{
    mode->limit_reason = reason;
    mode->limit_value = value;
    mode->limit_bound = bound;
    return UVC_LIMIT;
}

UvcParseResult uvc_parse(const uint8_t *data, size_t length, unsigned target, unsigned width,
                         unsigned height, UvcMode *mode)
{
    memset(mode, 0, sizeof(*mode));
    if (!data || length < 9 || data[0] != 9 || data[1] != 2 || !data[5]
        || uvc_le16(data + 2) != length || target > 255)
        return UVC_MALFORMED;
    if (!video_size_allowed(width, height)) return UVC_NO_MODE;
    /* Validate the whole descriptor walk before subsequent bounded passes. */
    for (size_t offset = 9; offset < length; offset += data[offset]) {
        if (length - offset < 2 || data[offset] < 2 || data[offset] > length - offset)
            return UVC_MALFORMED;
        const uint8_t *d = data + offset;
        if ((d[1] == 4 && d[0] < 9) || (d[1] == 5 && d[0] < 7) || (d[1] == 0x24 && d[0] < 3))
            return UVC_MALFORMED;
    }
    unsigned interface = 256, alternate = 0, subclass = 0, association = 0;
    bool video = false;
    for (size_t offset = 9; offset < length; offset += data[offset]) {
        const uint8_t *d = data + offset;
        if (d[1] == 4) {
            interface = d[2];
            alternate = d[3];
            subclass = d[6];
            video = d[5] == 14 && d[7] == 0;
        } else if (d[1] == 0x24 && video && subclass == 1 && alternate == 0 && d[2] == 1) {
            if (d[0] < 12 || d[0] != 12u + d[11]
                || uvc_le16(d + 5) != class_length(data, length, offset))
                return UVC_MALFORMED;
            for (unsigned i = 0; i < d[11]; ++i) {
                if (d[12 + i] == target) {
                    ++association;
                    mode->control_interface = (uint8_t)interface;
                    mode->version = uvc_le16(d + 3);
                }
            }
        }
    }
    if (association != 1) return UVC_ASSOCIATION;
    if (mode->version != 0x100) return UVC_VERSION;
    mode->configuration = data[5];
    mode->interface = (uint8_t)target;
    unsigned format = 0, headers = 0, zero_alts = 0;
    bool found = false, progressive = false;
    uint8_t formats[256] = {0}, frames[256] = {0};
    for (size_t offset = 9; offset < length; offset += data[offset]) {
        const uint8_t *d = data + offset;
        if (d[1] == 4) {
            interface = d[2];
            alternate = d[3];
            subclass = d[6];
            video = d[5] == 14 && d[7] == 0;
            format = 0;
            if (video && subclass == 2 && interface == target && alternate == 0) ++zero_alts;
        } else if (d[1] == 0x24 && video && subclass == 2 && interface == target
                   && alternate == 0) {
            if (d[2] == 1) {
                if (d[0] < 13 || !d[3] || !d[12] || d[0] != 13u + (unsigned)d[3] * d[12]
                    || uvc_le16(d + 4) != class_length(data, length, offset) || !(d[6] & 0x80)
                    || (d[6] & 0x70) || !(d[6] & 15))
                    return UVC_MALFORMED;
                ++headers;
                mode->endpoint = d[6];
                format = 0;
            } else if (d[2] == 6) {
                if (d[0] != 11 || !d[3] || !d[4] || !d[6] || formats[d[3]]) return UVC_MALFORMED;
                formats[d[3]] = 1;
                format = d[3];
                progressive = !(d[9] & 1);
                memset(frames, 0, sizeof(frames));
            } else if (d[2] == 7 && format) {
                if (d[0] < 26 || !d[3] || frames[d[3]]) return UVC_MALFORMED;
                frames[d[3]] = 1;
                unsigned count = d[25];
                if (d[0] != (count ? 26u + 4u * count : 38u)) return UVC_MALFORMED;
                if (found || !progressive || uvc_le16(d + 5) != width || uvc_le16(d + 7) != height)
                    continue;
                mode->format = (uint8_t)format;
                mode->frame = d[3];
                mode->width = (uint16_t)width;
                mode->height = (uint16_t)height;
                mode->max_frame = uvc_le32(d + 17);
                mode->interval = uvc_le32(d + 21);
                mode->interval_count = count;
                if (count > UVC_MAX_INTERVALS)
                    return limit(mode, "too many discrete intervals", count, UVC_MAX_INTERVALS);
                for (unsigned i = 0; i < count; ++i)
                    mode->intervals[i] = uvc_le32(d + 26 + 4 * i);
                for (unsigned i = 0; i < count; ++i) {
                    if (!mode->intervals[i])
                        return limit(mode, "zero discrete interval at index", i, count);
                }
                if (!count) {
                    mode->interval_min = uvc_le32(d + 26);
                    mode->interval_max = uvc_le32(d + 30);
                    mode->interval_step = uvc_le32(d + 34);
                    if (!mode->interval_min)
                        return limit(mode, "zero continuous minimum", 0, mode->interval_max);
                    if (mode->interval_min > mode->interval_max)
                        return limit(mode, "continuous minimum exceeds maximum", mode->interval_min,
                                     mode->interval_max);
                    if (!mode->interval_step) return limit(mode, "zero continuous step", 0, 1);
                }
                if (!mode->max_frame)
                    return limit(mode, "zero frame buffer size", 0, WC04_FRAME_CAPACITY);
                if (mode->max_frame > WC04_FRAME_CAPACITY)
                    return limit(mode, "frame buffer exceeds capacity", mode->max_frame,
                                 WC04_FRAME_CAPACITY);
                if (!uvc_interval_supported(mode, mode->interval))
                    return limit(mode, "default interval not advertised", mode->interval, count);
                found = true;
            } else if (d[2] != 3 && d[2] != 0x0d) {
                /* Unknown format/subtype must never inherit an MJPEG format index. */
                format = 0;
            }
        }
    }
    if (headers != 1 || zero_alts != 1) return UVC_MALFORMED;
    if (!found) return UVC_NO_MODE;
    bool seen_alts[256] = {false};
    for (size_t offset = 9; offset < length; offset += data[offset]) {
        const uint8_t *d = data + offset;
        if (d[1] == 4) {
            interface = d[2];
            alternate = d[3];
            subclass = d[6];
            video = d[5] == 14 && d[7] == 0;
        } else if (d[1] == 5 && video && subclass == 2 && interface == target && alternate
                   && d[2] == mode->endpoint && (d[3] & 3) == 1 && !(d[3] & 0xf0) && d[6] == 1) {
            unsigned raw = uvc_le16(d + 4), packet = raw & 0x7ff;
            unsigned multiplier = (raw >> 11) & 3;
            if (!packet || packet > 1024 || multiplier == 3 || (raw & 0xe000)
                || seen_alts[alternate])
                return UVC_MALFORMED;
            if (mode->alt_count == WC04_MAX_ALTS)
                return limit(mode, "too many matching alternates", (uint32_t)mode->alt_count + 1,
                             WC04_MAX_ALTS);
            seen_alts[alternate] = true;
            mode->alts[mode->alt_count++] =
                (UvcAlternate){(uint8_t)alternate, d[2], (uint16_t)(packet * (multiplier + 1))};
        }
    }
    return mode->alt_count ? UVC_OK : UVC_TRANSPORT;
}

uint32_t uvc_supported_sizes(const uint8_t *data, size_t length, unsigned interface)
{
    uint32_t sizes = 0;
    for (unsigned n = 0; n < VIDEO_SIZE_COUNT; ++n) {
        VideoSize size = video_size(n);
        UvcMode mode;
        if (uvc_parse(data, length, interface, size.width, size.height, &mode) == UVC_OK)
            sizes |= 1u << n;
    }
    return sizes;
}

bool uvc_alternate(const UvcMode *mode, uint32_t payload, UvcAlternate *alt)
{
    bool found = false;
    if (!payload) return false;
    for (size_t i = 0; i < mode->alt_count; ++i) {
        const UvcAlternate *candidate = &mode->alts[i];
        if (candidate->capacity >= payload && (!found || candidate->capacity < alt->capacity)) {
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

bool uvc_negotiate(const UvcMode *mode, UvcAlternate *alt, uint8_t data[UVC_PROBE_LENGTH],
                   UvcControlTransfer transfer, void *context)
{
    if (mode->version != 0x100 || !uvc_interval_supported(mode, mode->interval)) return false;
    const uint8_t seeds[] = {UVC_GET_MAX, UVC_GET_DEF, UVC_GET_CUR};
    bool seeded = false;
    for (size_t i = 0; i < sizeof(seeds); ++i) {
        memset(data, 0, UVC_PROBE_LENGTH);
        if (transfer(context, seeds[i], UVC_VS_PROBE, data) == UVC_PROBE_LENGTH) {
            seeded = true;
            break;
        }
    }
    if (!seeded) return false;
    uvc_probe_select(data, mode);
    if (transfer(context, UVC_SET_CUR, UVC_VS_PROBE, data) != UVC_PROBE_LENGTH) return false;
    memset(data, 0, UVC_PROBE_LENGTH);
    if (transfer(context, UVC_GET_CUR, UVC_VS_PROBE, data) != UVC_PROBE_LENGTH) return false;
    uint32_t max_frame = uvc_le32(data + 18);
    if (data[2] != mode->format || data[3] != mode->frame
        || !uvc_interval_supported(mode, uvc_le32(data + 4)) || !max_frame
        || max_frame > mode->max_frame || max_frame > WC04_FRAME_CAPACITY
        || !uvc_alternate(mode, uvc_le32(data + 22), alt))
        return false;
    /* Commit exactly the accepted GET_CUR block, including adjusted fields. */
    return transfer(context, UVC_SET_CUR, UVC_VS_COMMIT, data) == UVC_PROBE_LENGTH;
}

void uvc_frame_init(UvcFrame *frame, uint8_t *data, size_t capacity)
{
    *frame = (UvcFrame){.data = data, .capacity = capacity, .fid = -1};
}

/* Only the observed eight-byte alignment suffix is eligible for removal.
 * Boundary acceptance still requires full decoding before display or saving. */
static size_t jpeg_length(const UvcFrame *frame)
{
    size_t length = frame->size;
    if (frame->discard || length < 4 || frame->data[0] != 0xff || frame->data[1] != 0xd8) return 0;
    if (!(length & 7)) {
        unsigned padding = 0;
        while (padding < 7 && length > 4 && frame->data[length - 1] == 0) {
            --length;
            ++padding;
        }
    }
    return frame->data[length - 2] == 0xff && frame->data[length - 1] == 0xd9 ? length : 0;
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
    if (flags & 0x40) ++frame->stats.errors;
    if (flags & 2) ++frame->stats.eof;
    int fid = flags & 1;
    if (fid != frame->fid) {
        if (frame->fid != -1) ++frame->stats.fid_changes;
        if (frame->size && !frame->ended) {
            ++frame->dropped;
            ++frame->stats.missing_eof;
        }
        frame->size = 0;
        frame->discard = false;
        frame->ended = false;
        frame->fid = fid;
    }
    if (frame->ended) return false;
    if (payload > frame->capacity - frame->size) ++frame->stats.overflow;
    if ((flags & 0x40) || packet[0] < minimum_header || payload > frame->capacity - frame->size)
        frame->discard = true;
    if (!frame->discard && payload) {
        memcpy(frame->data + frame->size, packet + packet[0], payload);
        frame->size += payload;
    }
    if (!(flags & 2)) return false;
    frame->ended = true;
    size_t normalized = jpeg_length(frame);
    if (!normalized) {
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
    if (normalized != frame->size) {
        ++frame->stats.normalized_frames;
        frame->stats.padding_bytes += (unsigned)(frame->size - normalized);
        frame->size = normalized;
    }
    return true;
}
