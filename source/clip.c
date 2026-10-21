/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "clip.h"
#include "frame_limits.h"
#include <string.h>

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static bool read_at(FILE *file, uint32_t offset, void *data, size_t length)
{
    return fseek(file, (long)offset, SEEK_SET) == 0 && fread(data, 1, length, file) == length;
}

static bool read_chunk(Clip *c, uint32_t offset, const char *id, uint8_t *data, uint32_t length)
{
    uint8_t prefix[8];
    return read_at(c->file, c->movi + offset, prefix, sizeof(prefix)) && !memcmp(prefix, id, 4)
           && le32(prefix + 4) == length && fread(data, 1, length, c->file) == length;
}

static bool read_index(Clip *c, uint32_t at, uint32_t size, uint32_t end)
{
    if (fseek(c->file, (long)at, SEEK_SET) != 0) return false;
    for (uint32_t n = 0; n < size / 16; ++n) {
        uint8_t entry[16];
        if (fread(entry, 1, sizeof(entry), c->file) != sizeof(entry)) return false;
        uint32_t offset = le32(entry + 8), length = le32(entry + 12);
        if ((uint64_t)c->movi + offset + 8 + length > end) return false;
        if (!memcmp(entry, "00dc", 4)) {
            if (c->frames == AVI_MAX_FRAMES || length > WC04_FRAME_CAPACITY) return false;
            c->frame_offsets[c->frames] = offset;
            c->frame_lengths[c->frames++] = length;
        } else if (!memcmp(entry, "01wb", 4)) {
            if (c->blocks == AV_MAX_FRAMES || length != AUDIO_BLOCK_BYTES) return false;
            c->block_offsets[c->blocks++] = offset;
        }
    }
    return c->frames && (!c->blocks || c->blocks == c->frames);
}

bool clip_open(Clip *c, const char *path)
{
    memset(c, 0, sizeof(*c));
    c->file = fopen(path, "rb");
    uint8_t head[12];
    long end = -1;
    if (c->file && fseek(c->file, 0, SEEK_END) == 0) end = ftell(c->file);
    if (end < 12 || end > (long)AVI_MAX_BYTES || !read_at(c->file, 0, head, sizeof(head))
        || memcmp(head, "RIFF", 4) || memcmp(head + 8, "AVI ", 4)) {
        clip_close(c);
        return false;
    }
    uint32_t at = 12, index = 0, index_size = 0;
    while (at + 12 <= (uint32_t)end) {
        uint8_t chunk[12];
        if (!read_at(c->file, at, chunk, sizeof(chunk))) break;
        uint32_t size = le32(chunk + 4);
        if (size > (uint32_t)end - at - 8) break;
        if (!memcmp(chunk, "LIST", 4) && !memcmp(chunk + 8, "hdrl", 4)) {
            uint8_t avih[48];
            if (read_at(c->file, at + 12, avih, sizeof(avih)) && !memcmp(avih, "avih", 4)) {
                c->frame_us = le32(avih + 8);
                c->width = le32(avih + 40);
                c->height = le32(avih + 44);
            }
        } else if (!memcmp(chunk, "LIST", 4) && !memcmp(chunk + 8, "movi", 4))
            c->movi = at + 8;
        else if (!memcmp(chunk, "idx1", 4)) {
            index = at + 8;
            index_size = size;
        }
        at += 8 + size + (size & 1);
    }
    if (!c->frame_us || !c->width || c->width > WC04_MAX_WIDTH || !c->height
        || c->height > WC04_MAX_HEIGHT || !c->movi || !index
        || !read_index(c, index, index_size, (uint32_t)end)) {
        clip_close(c);
        return false;
    }
    return true;
}

bool clip_frame(Clip *c, unsigned index, uint8_t *jpeg, size_t capacity, size_t *length)
{
    if (!c->file || index >= c->frames || c->frame_lengths[index] > capacity
        || !read_chunk(c, c->frame_offsets[index], "00dc", jpeg, c->frame_lengths[index]))
        return false;
    *length = c->frame_lengths[index];
    return true;
}

size_t clip_audio(Clip *c, uint8_t *pcm, size_t capacity)
{
    if (!c->file || !c->blocks || (size_t)c->blocks * AUDIO_BLOCK_BYTES > capacity) return 0;
    for (unsigned n = 0; n < c->blocks; ++n)
        if (!read_chunk(c, c->block_offsets[n], "01wb", pcm + (size_t)n * AUDIO_BLOCK_BYTES,
                        AUDIO_BLOCK_BYTES))
            return 0;
    return (size_t)c->blocks * AUDIO_BLOCK_BYTES;
}

void clip_close(Clip *c)
{
    if (c->file) fclose(c->file);
    c->file = NULL;
}
