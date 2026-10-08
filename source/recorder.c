/* SPDX-License-Identifier: GPL-3.0-only */
#include "recorder.h"
#include "storage.h"

void recorder_init(Recorder *r)
{
    *r = (Recorder){0};
}

bool recorder_start(Recorder *r, const char *directory, unsigned width, unsigned height, uint64_t now)
{
    if (r->active)
        return false;
    recorder_init(r);
    FILE *file = storage_create(directory, "VID", "AVI", r->path, sizeof(r->path));
    if (!file) {
        r->failed = true;
        return false;
    }
    if (!avi_begin(&r->avi, file, width, height)) {
        storage_finish(file, r->path, false);
        r->failed = true;
        return false;
    }
    r->started = now;
    r->last_frame = now;
    r->active = true;
    return true;
}

bool recorder_frame(Recorder *r, const uint8_t *jpeg, size_t length, uint64_t now)
{
    if (!r->active || r->failed || r->limited)
        return false;
    if (now-r->started >= AVI_MAX_MS || !avi_has_space(&r->avi, length)) {
        r->limited = true;
        return false;
    }
    if (!avi_append(&r->avi, jpeg, length)) {
        r->failed = true;
        return false;
    }
    r->last_frame = now;
    return true;
}

bool recorder_stop(Recorder *r, uint64_t now)
{
    if (!r->active)
        return false;
    uint64_t duration = now > r->started ? now-r->started : 1;
    r->duration_ms = duration > AVI_MAX_MS ? AVI_MAX_MS : (unsigned)duration;
    bool finished = !r->failed && avi_finish(&r->avi, r->duration_ms);
    r->saved = storage_finish(r->avi.file, r->path, finished);
    r->failed = r->failed || (r->avi.frames && !r->saved);
    r->active = false;
    return r->saved;
}
