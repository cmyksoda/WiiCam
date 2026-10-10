/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */
#include "uac.h"
#include "camera.h"

typedef struct {
    uint8_t id, kind, source, channels, controls;
    uint16_t type;
} Entity;

typedef struct {
    size_t start, end;
    unsigned control;
} Function;

static uint32_t le24(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
}

static bool envelope(const uint8_t *data, size_t length)
{
    if (!data || length < 9 || length > CAMERA_CONFIG_CAPACITY || data[0] != 9 || data[1] != 2
        || uvc_le16(data + 2) != length)
        return false;
    for (size_t at = 9; at < length; at += data[at]) {
        if (length - at < 2 || data[at] < 2 || data[at] > length - at) return false;
        if (data[at + 1] == 4 && data[at] < 9) return false;
    }
    return true;
}

static UacResult find_function(const uint8_t *data, size_t length, unsigned target, Function *f)
{
    unsigned interface = 0, protocol = 0;
    bool control = false, found = false;
    for (size_t at = 9; at < length; at += data[at]) {
        const uint8_t *d = data + at;
        if (d[1] == 4) {
            interface = d[2];
            protocol = d[7];
            control = d[3] == 0 && d[5] == 1 && d[6] == 1;
        } else if (control && d[1] == 0x24) {
            if (d[0] < 3) return UAC_MALFORMED;
            if (d[2] != 1) continue;
            if (protocol != 0) continue;
            if (d[0] < 8 || d[0] != 8u + d[7]) return UAC_MALFORMED;
            bool linked = false;
            for (unsigned n = 0; n < d[7]; ++n) {
                if (d[8 + n] == target) {
                    if (linked) return UAC_ASSOCIATION;
                    linked = true;
                }
            }
            if (!linked) continue;
            if (found) return UAC_ASSOCIATION;
            if (uvc_le16(d + 3) != 0x100) return UAC_VERSION;
            unsigned total = uvc_le16(d + 5);
            if (total < d[0] || total > length - at) return UAC_MALFORMED;
            size_t end = at + total;
            for (size_t p = at; p < end; p += data[p])
                if (data[p] > end - p || data[p + 1] != 0x24 || data[p] < 3) return UAC_MALFORMED;
            if (end < length && data[end + 1] == 0x24) return UAC_MALFORMED;
            *f = (Function){at + d[0], end, interface};
            found = true;
        }
    }
    return found ? UAC_OK : UAC_ASSOCIATION;
}

static UacResult entities(const uint8_t *data, const Function *f, Entity out[UAC_MAX_ENTITIES],
                          unsigned *count)
{
    *count = 0;
    for (size_t at = f->start; at < f->end; at += data[at]) {
        const uint8_t *d = data + at;
        if (d[0] < 4 || !d[3] || d[2] == 1) return UAC_MALFORMED;
        for (unsigned n = 0; n < *count; ++n)
            if (out[n].id == d[3]) return UAC_ASSOCIATION;
        if (*count == UAC_MAX_ENTITIES) return UAC_LIMIT;
        Entity e = {.id = d[3], .kind = d[2]};
        if (d[2] == 2) {
            if (d[0] != 12) return UAC_MALFORMED;
            e.type = uvc_le16(d + 4);
            e.channels = d[7];
        } else if (d[2] == 3) {
            if (d[0] != 9) return UAC_MALFORMED;
            e.type = uvc_le16(d + 4);
            e.source = d[7];
        } else if (d[2] == 6) {
            if (d[0] < 7 || !d[5] || d[5] > 4 || (d[0] - 7) % d[5]) return UAC_MALFORMED;
            e.source = d[4];
            e.controls = (d[0] - 7) / d[5];
        }
        out[(*count)++] = e;
    }
    return UAC_OK;
}

static const Entity *entity(const Entity *list, unsigned count, unsigned id)
{
    for (unsigned n = 0; n < count; ++n)
        if (list[n].id == id) return list + n;
    return NULL;
}

static bool microphone(const Entity *list, unsigned count, UacMode *mode)
{
    const Entity *current = entity(list, count, mode->terminal);
    if (!current || current->kind != 3 || current->type != 0x0101) return false;
    unsigned id = current->source;
    for (unsigned steps = 0; steps < count; ++steps) {
        current = entity(list, count, id);
        if (!current) return false;
        if (current->kind == 2) {
            if (current->type < 0x0201 || current->type > 0x0206
                || current->channels != mode->channels)
                return false;
            mode->microphone = current->id;
            return true;
        }
        if (current->kind != 6 || current->controls != mode->channels + 1u) return false;
        id = current->source;
    }
    return false;
}

bool uac_rate_supported(const UacMode *mode, uint32_t rate)
{
    if (!mode || rate < 8000 || rate > 48000 || mode->rate_count > UAC_MAX_RATES) return false;
    if (!mode->rate_count)
        return mode->frequency_control && rate >= mode->minimum_rate && rate <= mode->maximum_rate;
    if (!mode->frequency_control && mode->rate_count != 1) return false;
    for (unsigned n = 0; n < mode->rate_count; ++n)
        if (mode->rates[n] == rate) return true;
    return false;
}

static UacResult format_type(const uint8_t *d, UacMode *mode)
{
    if (d[0] < 4) return UAC_MALFORMED;
    if (d[3] != 1) return UAC_NO_MODE;
    if (d[0] < 8) return UAC_MALFORMED;
    unsigned count = d[7];
    if (d[0] != (count ? 8 + 3 * count : 14)) return UAC_MALFORMED;
    if (count > UAC_MAX_RATES) return UAC_LIMIT;
    if (d[4] < 1 || d[4] > 2 || d[5] != 2 || d[6] != 16) return UAC_NO_MODE;
    mode->channels = d[4];
    mode->rate_count = count;
    if (count) {
        for (unsigned n = 0; n < count; ++n) {
            mode->rates[n] = le24(d + 8 + n * 3);
            if (!mode->rates[n]) return UAC_MALFORMED;
            for (unsigned p = 0; p < n; ++p)
                if (mode->rates[n] == mode->rates[p]) return UAC_MALFORMED;
        }
    } else {
        mode->minimum_rate = le24(d + 8);
        mode->maximum_rate = le24(d + 11);
        if (!mode->minimum_rate || mode->minimum_rate > mode->maximum_rate) return UAC_MALFORMED;
    }
    return UAC_OK;
}

static UacResult stream(const uint8_t *data, size_t length, unsigned target, unsigned alternate,
                        UacMode *mode)
{
    bool selected = false, seen = false, idle = false, general = false, format = false,
         endpoint = false;
    bool controls = false, idle_selected = false;
    unsigned alternates = 0;
    uint8_t alt_seen[256] = {0};
    for (size_t at = 9; at < length; at += data[at]) {
        const uint8_t *d = data + at;
        if (d[1] == 4) {
            selected = idle_selected = false;
            if (d[2] != target) continue;
            if (d[5] != 1 || d[6] != 2 || d[7] != 0) return UAC_VERSION;
            if (alt_seen[d[3]]) return UAC_ASSOCIATION;
            alt_seen[d[3]] = 1;
            if (++alternates > UAC_MAX_ALTERNATES) return UAC_LIMIT;
            if (!d[3]) {
                if (d[4]) return UAC_NO_MODE;
                idle = idle_selected = true;
            }
            if (d[3] != alternate) continue;
            if (d[4] != 1) return UAC_NO_MODE;
            selected = seen = true;
        } else if (idle_selected && d[1] == 5) {
            return UAC_MALFORMED;
        } else if (selected && d[1] == 0x24) {
            if (d[0] < 3) return UAC_MALFORMED;
            if (d[2] == 1) {
                if (general || d[0] != 7) return UAC_MALFORMED;
                if (uvc_le16(d + 5) != 1) return UAC_NO_MODE;
                mode->terminal = d[3];
                general = true;
            } else if (d[2] == 2) {
                if (format) return UAC_MALFORMED;
                UacResult result = format_type(d, mode);
                if (result != UAC_OK) return result;
                format = true;
            } else {
                return UAC_NO_MODE;
            }
        } else if (selected && d[1] == 5) {
            if (endpoint || d[0] != 9) return UAC_MALFORMED;
            unsigned packet = uvc_le16(d + 4);
            if (!(d[2] & 0x80) || !(d[2] & 15) || (d[2] & 0x70) || (d[3] & 3) != 1 || (d[3] & 0xf0)
                || (d[3] & 12) == 8 || !packet || packet > 1024 || !d[6] || d[6] > 16 || d[7]
                || d[8])
                return UAC_NO_MODE;
            mode->endpoint = d[2];
            mode->packet_capacity = packet;
            mode->interval = d[6];
            endpoint = true;
        } else if (selected && d[1] == 0x25) {
            if (!endpoint || controls || d[0] != 7 || d[2] != 1) return UAC_MALFORMED;
            mode->endpoint_controls = d[3];
            mode->frequency_control = (d[3] & 1) != 0;
            mode->lock_units = d[4];
            mode->lock_delay = uvc_le16(d + 5);
            if ((d[3] & 0x7c) || d[4] > 2) return UAC_NO_MODE;
            controls = true;
        }
    }
    if (!seen || !idle || !general || !format || !endpoint || !controls) return UAC_NO_MODE;
    if (mode->packet_capacity < mode->channels * 2u) return UAC_NO_MODE;
    return UAC_OK;
}

UacResult uac_parse(const uint8_t *data, size_t length, unsigned interface, unsigned alternate,
                    UacMode *mode)
{
    if (!mode) return UAC_MALFORMED;
    *mode = (UacMode){0};
    if (interface > 255 || !alternate || alternate > 255 || !envelope(data, length))
        return UAC_MALFORMED;
    Function f = {0};
    UacResult result = find_function(data, length, interface, &f);
    if (result != UAC_OK) return result;
    Entity list[UAC_MAX_ENTITIES];
    unsigned count;
    result = entities(data, &f, list, &count);
    if (result != UAC_OK) return result;
    UacMode candidate = {
        .control_interface = f.control, .interface = interface, .alternate = alternate};
    result = stream(data, length, interface, alternate, &candidate);
    if (result != UAC_OK) return result;
    if (!microphone(list, count, &candidate)) return UAC_ASSOCIATION;
    if (uac_rate_supported(&candidate, 16000))
        candidate.rate = 16000;
    else if (!candidate.rate_count) {
        uint32_t rate = candidate.minimum_rate < 8000 ? 8000 : candidate.minimum_rate;
        if (uac_rate_supported(&candidate, rate)) candidate.rate = rate;
    } else {
        for (unsigned n = 0; n < candidate.rate_count; ++n) {
            uint32_t rate = candidate.rates[n];
            if (uac_rate_supported(&candidate, rate) && (!candidate.rate || rate < candidate.rate))
                candidate.rate = rate;
        }
    }
    if (!candidate.rate) return UAC_NO_MODE;
    *mode = candidate;
    return UAC_OK;
}
