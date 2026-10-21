/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "microphone.h"
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

bool microphone_mode(const uint8_t *config, size_t length, UacMode *mode)
{
    if (!mode) return false;
    memset(mode, 0, sizeof(*mode));
    if (!config || length < 9 || length > CAMERA_CONFIG_CAPACITY || config[0] != 9 || config[1] != 2
        || uvc_le16(config + 2) != length)
        return false;
    unsigned found = 0;
    UacMode selected = {0};
    for (size_t at = 9; at < length; at += config[at]) {
        if (length - at < 2 || config[at] < 2 || config[at] > length - at) return false;
        const uint8_t *d = config + at;
        if (d[1] == 4 && d[0] >= 9 && d[3] && d[5] == 1 && d[6] == 2) {
            UacMode candidate;
            if (uac_parse(config, length, d[2], d[3], &candidate) == UAC_OK
                && candidate.channels == 1 && uac_rate_supported(&candidate, MIC_RATE)) {
                selected = candidate;
                selected.rate = MIC_RATE;
                ++found;
            }
        }
    }
    if (found != 1) return false;
    *mode = selected;
    return true;
}

MicRateResult microphone_set_rate(const CameraEntry *entry, const UacMode *mode, MicControl control,
                                  void *context, uint8_t *data)
{
    if (!mode || mode->channels != 1 || !uac_rate_supported(mode, MIC_RATE)) return MIC_RATE_FAILED;
    if (!mode->frequency_control) return MIC_RATE_OK;
    if (!control || !data || !(mode->endpoint & 0x80)) return MIC_RATE_FAILED;
    data[0] = MIC_RATE & 255;
    data[1] = MIC_RATE >> 8;
    data[2] = 0;
    if (control(context, 0x22, 1, 0x0100, mode->endpoint, data, 3) != 3) return MIC_RATE_FAILED;
    /* This exact camera profile repeatedly rejects rate readback. Avoid issuing
     * the failing GET before streaming; successful SET alone is not proof
     * of the clock rate, so retain the PCM timing check for this quirk. */
    if (entry && entry->vid == 0x0c45 && entry->pid == 0x6366
        && entry->interface == 3 && mode->control_interface == 2 && mode->interface == 3
        && mode->alternate == 1 && mode->endpoint == 0x84 && mode->packet_capacity == 192
        && mode->interval == 4)
        return MIC_RATE_UNREADABLE;
    memset(data, 0, 3);
    int result = control(context, 0xa2, 0x81, 0x0100, mode->endpoint, data, 3);
    if (result != 3) return MIC_RATE_FAILED;
    uint32_t rate = (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16;
    return rate == MIC_RATE ? MIC_RATE_OK : MIC_RATE_FAILED;
}
