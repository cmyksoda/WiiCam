/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "av_recorder.h"
#include "storage.h"
#include "frame_limits.h"
#include <string.h>

static bool valid_jpeg(const uint8_t *jpeg, size_t length)
{
    return jpeg && length >= 4 && length <= WC04_FRAME_CAPACITY && jpeg[0] == 0xff
           && jpeg[1] == 0xd8 && jpeg[length - 2] == 0xff && jpeg[length - 1] == 0xd9;
}

void av_recorder_init(AvRecorder *r, AudioStream *audio, uint8_t *jpeg)
{
    *r = (AvRecorder){.audio = audio, .jpeg = jpeg};
}

bool av_recorder_start(AvRecorder *r, const char *directory, unsigned width, unsigned height,
                       const uint8_t *jpeg, size_t length)
{
    if (r->active || !r->audio || !r->jpeg || !valid_jpeg(jpeg, length)
        || atomic_load(&r->audio->failed)) {
        r->failed = true;
        return false;
    }
    FILE *file = storage_create(directory, "AV", "AVI", r->path, sizeof(r->path));
    if (!file) {
        r->failed = true;
        return false;
    }
    if (!av_avi_begin(&r->avi, file, width, height)) {
        storage_finish(file, r->path, false);
        r->failed = true;
        return false;
    }
    audio_stream_discard(r->audio);
    r->anchor = atomic_load(&r->audio->consumed);
    memcpy(r->jpeg, jpeg, length);
    r->length = length;
    r->source_generation = 1;
    r->active = true;
    return true;
}

bool av_recorder_pump(AvRecorder *r, unsigned until)
{
    if (!r->active || r->failed || r->limited) return false;
    if (atomic_load(&r->audio->failed)) {
        r->failed = true;
        return false;
    }
    unsigned read = atomic_load(&r->audio->consumed);
    unsigned ready = audio_stream_position(r->audio) - read;
    unsigned wanted = until - read;
    /* A video timestamp can precede the already written interval. */
    if (wanted > UINT32_MAX / 2) return true;
    if (wanted > ready) wanted = ready;
    while (wanted >= AUDIO_BLOCK_BYTES) {
        if (!av_avi_has_space(&r->avi, r->length)) {
            r->limited = true;
            return false;
        }
        uint8_t pcm[AUDIO_BLOCK_BYTES];
        if (!audio_stream_read(r->audio, pcm, sizeof(pcm))
            || !av_avi_pair(&r->avi, r->jpeg, r->length, pcm)) {
            r->failed = true;
            return false;
        }
        if (r->written_generation == r->source_generation) ++r->repeated;
        r->written_generation = r->source_generation;
        wanted -= AUDIO_BLOCK_BYTES;
    }
    return true;
}

bool av_recorder_frame(AvRecorder *r, const uint8_t *jpeg, size_t length, unsigned position)
{
    if (!valid_jpeg(jpeg, length)) {
        r->failed = true;
        return false;
    }
    if (!av_recorder_pump(r, position)) return false;
    unsigned elapsed = position - r->anchor;
    if (elapsed > UINT32_MAX / 2 || elapsed < r->avi.frames * AUDIO_BLOCK_BYTES) return true;
    memcpy(r->jpeg, jpeg, length);
    r->length = length;
    ++r->source_generation;
    return true;
}

bool av_recorder_stop(AvRecorder *r, unsigned until, bool valid)
{
    if (!r->active) return false;
    if (!valid) r->failed = true;
    if (!r->failed && !r->limited) av_recorder_pump(r, until);
    bool finished = !r->failed && av_avi_finish(&r->avi);
    r->saved = storage_finish(r->avi.file, r->path, finished);
    r->failed = r->failed || !r->saved;
    r->active = false;
    return r->saved;
}
