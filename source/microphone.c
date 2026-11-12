/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "microphone.h"
#include <stdio.h>
#include <string.h>

bool microphone_match(const MicEntry *entries, size_t count, int32_t video_id,
                      unsigned video_interface, unsigned audio_interface, size_t *index)
{
    if (!entries || !index || count > CAMERA_MAX_DEVICES || video_id >= 0 || video_interface > 255
        || audio_interface > 255 || video_interface == audio_interface)
        return false;
    const MicEntry *video = NULL;
    for (size_t n = 0; n < count; ++n) {
        if (entries[n].camera.id != video_id) continue;
        if (video) return false;
        video = entries + n;
    }
    if (!video || video->camera.interface != video_interface
        || (video->token >> 16) != ((uint32_t)video_id & 0xffff))
        return false;
    unsigned videos = 0, microphones = 0;
    size_t candidate = 0;
    for (size_t n = 0; n < count; ++n) {
        const MicEntry *entry = entries + n;
        if (entry->camera.vid != video->camera.vid || entry->camera.pid != video->camera.pid)
            continue;
        if (entry->camera.interface == video_interface) ++videos;
        if (entry->camera.interface == audio_interface) {
            ++microphones;
            candidate = n;
        }
    }
    if (videos != 1 || microphones != 1) return false;
    const MicEntry *audio = entries + candidate;
    if (audio->camera.id >= 0 || audio->camera.id == video_id
        || (audio->token >> 16) != (video->token >> 16)
        || ((uint32_t)audio->camera.id & 0xffff) != (audio->token >> 16))
        return false;
    *index = candidate;
    return true;
}

bool microphone_parameters(const CameraEntry *entry, const uint8_t *p, unsigned configuration)
{
    return entry && p && p[20] == 18 && p[21] == 1 && p[37] == 1
           && ((unsigned)p[28] << 8 | p[29]) == entry->vid
           && ((unsigned)p[30] << 8 | p[31]) == entry->pid && p[40] == 9 && p[41] == 2 && p[44]
           && p[45] == configuration && p[52] == 9 && p[53] == 4
           && p[54] == entry->interface && p[55] == 0 && p[56] == 0 && p[57] == 1 && p[58] == 2
           && p[59] == 0;
}

static uint32_t usable_rate(const UacMode *mode)
{
    for (uint32_t factor = 1; factor <= MIC_MAX_DECIMATION; ++factor)
        if (uac_rate_supported(mode, MIC_RATE * factor)) return MIC_RATE * factor;
    return 0;
}

bool microphone_mode(const uint8_t *config, size_t length, UacMode *mode)
{
    if (!mode) return false;
    memset(mode, 0, sizeof(*mode));
    if (!config || length < 9 || length > CAMERA_CONFIG_CAPACITY || config[0] != 9 || config[1] != 2
        || uvc_le16(config + 2) != length)
        return false;
    UacMode selected = {0};
    uint32_t cost = 0;
    for (size_t at = 9; at < length; at += config[at]) {
        if (length - at < 2 || config[at] < 2 || config[at] > length - at) return false;
        const uint8_t *d = config + at;
        if (d[1] == 4 && d[0] >= 9 && d[3] && d[5] == 1 && d[6] == 2) {
            UacMode candidate;
            if (uac_parse(config, length, d[2], d[3], &candidate) != UAC_OK
                || candidate.channels > MIC_MAX_CHANNELS)
                continue;
            uint32_t rate = usable_rate(&candidate);
            if (!rate || (cost && rate * candidate.channels >= cost)) continue;
            selected = candidate;
            selected.rate = rate;
            cost = rate * candidate.channels;
        }
    }
    if (!cost) return false;
    *mode = selected;
    return true;
}

size_t microphone_summary(const uint8_t *config, size_t length, char *text, size_t size)
{
    if (!config || !text || !size || length < 9) return 0;
    size_t used = 0;
    unsigned interface = 0, alternate = 0;
    bool streaming = false;
    text[0] = 0;
    for (size_t at = 9; at + 2 <= length && config[at] >= 2 && config[at] <= length - at;
         at += config[at]) {
        const uint8_t *d = config + at;
        if (d[1] == 4 && d[0] >= 9) {
            streaming = d[5] == 1 && d[6] == 2 && d[3];
            interface = d[2];
            alternate = d[3];
        } else if (streaming && d[1] == 0x24 && d[0] >= 8 && d[2] == 2) {
            int n = snprintf(text + used, size - used,
                             "Audio interface %u alternate %u: format %u, %u ch, %u-bit,", interface,
                             alternate, d[3], d[4], d[6]);
            if (n < 0 || (size_t)n >= size - used) break;
            used += (size_t)n;
            unsigned count = d[7];
            for (unsigned r = 0; (count ? r < count : r < 2) && 11u + 3 * r <= d[0]; ++r) {
                unsigned rate = d[8 + 3 * r] | d[9 + 3 * r] << 8 | (unsigned)d[10 + 3 * r] << 16;
                n = snprintf(text + used, size - used, "%s%u", count || !r ? " " : "-", rate);
                if (n < 0 || (size_t)n >= size - used) return used;
                used += (size_t)n;
            }
            n = snprintf(text + used, size - used, " Hz\n");
            if (n < 0 || (size_t)n >= size - used) break;
            used += (size_t)n;
        }
    }
    return used;
}

MicRateResult microphone_set_rate(const CameraEntry *entry, const UacMode *mode, MicControl control,
                                  void *context, uint8_t *data)
{
    if (!mode || !mode->channels || mode->channels > MIC_MAX_CHANNELS || mode->rate % MIC_RATE
        || mode->rate > MIC_RATE * MIC_MAX_DECIMATION || !uac_rate_supported(mode, mode->rate))
        return MIC_RATE_FAILED;
    if (!mode->frequency_control) return MIC_RATE_OK;
    if (!control || !data || !(mode->endpoint & 0x80)) return MIC_RATE_FAILED;
    // An alternate with one listed rate can only run at that rate, whatever the clock requests say.
    bool fixed = mode->rate_count == 1;
    data[0] = mode->rate & 255;
    data[1] = mode->rate >> 8 & 255;
    data[2] = mode->rate >> 16 & 255;
    if (control(context, 0x22, 1, 0x0100, mode->endpoint, data, 3) != 3)
        return fixed ? MIC_RATE_UNREADABLE : MIC_RATE_FAILED;
    /* This exact camera profile repeatedly rejects rate readback. Avoid issuing
     * the failing GET before streaming; successful SET alone is not proof
     * of the clock rate, so retain the PCM timing check for this quirk. */
    if (entry && entry->vid == 0x0c45 && entry->pid == 0x6366
        && entry->interface == 3 && mode->control_interface == 2 && mode->interface == 3
        && mode->alternate == 1 && mode->endpoint == 0x84 && mode->packet_capacity == 192
        && mode->interval == 4)
        return MIC_RATE_UNREADABLE;
    memset(data, 0, 3);
    // Many webcams never answer this read; Linux carries on without it too.
    int result = control(context, 0xa2, 0x81, 0x0100, mode->endpoint, data, 3);
    if (result != 3) return MIC_RATE_UNREADABLE;
    uint32_t rate = (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16;
    if (rate == mode->rate) return MIC_RATE_OK;
    return fixed || !rate ? MIC_RATE_UNREADABLE : MIC_RATE_FAILED;
}
