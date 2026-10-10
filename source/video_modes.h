/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WIICAM_VIDEO_MODES_H
#define WIICAM_VIDEO_MODES_H
#include <stdbool.h>
#include <stdint.h>

#define VIDEO_SIZE_COUNT 7u
#define VIDEO_DEFAULT_SIZES 3u

typedef struct {
    unsigned width, height;
} VideoSize;

/* A finite capture policy; each size must also pass the camera's descriptors. */
static inline VideoSize video_size(unsigned index)
{
    const VideoSize sizes[VIDEO_SIZE_COUNT] = {{320, 240}, {640, 480}, {640, 400}, {848, 480},
                                               {352, 288}, {176, 144}, {160, 120}};
    return index < VIDEO_SIZE_COUNT ? sizes[index] : (VideoSize){0, 0};
}

static inline bool video_size_allowed(unsigned width, unsigned height)
{
    for (unsigned n = 0; n < VIDEO_SIZE_COUNT; ++n) {
        VideoSize size = video_size(n);
        if (size.width == width && size.height == height) return true;
    }
    return false;
}

static inline VideoSize video_size_next(unsigned width, unsigned height, uint32_t available)
{
    unsigned current = VIDEO_SIZE_COUNT - 1;
    for (unsigned n = 0; n < VIDEO_SIZE_COUNT; ++n) {
        VideoSize size = video_size(n);
        if (size.width == width && size.height == height) current = n;
    }
    for (unsigned step = 1; step <= VIDEO_SIZE_COUNT; ++step) {
        unsigned next = (current + step) % VIDEO_SIZE_COUNT;
        if (available & (1u << next)) return video_size(next);
    }
    return (VideoSize){width, height};
}
#endif
