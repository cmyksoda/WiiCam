/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */
#include "av_avi.h"
#include "frame_limits.h"
#include <string.h>

_Static_assert(MIC_RATE * 2u * AUDIO_FRAME_MS / 1000u == AUDIO_BLOCK_BYTES,
               "audio samples and video intervals must have matching durations");

static void le32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(n >> (i * 8));
}

static bool write_bytes(AvAvi *a, const void *data, size_t length)
{
    if (a->failed || fwrite(data, 1, length, a->file) != length || ferror(a->file)) {
        a->failed = true;
        return false;
    }
    return true;
}

static void header(uint8_t data[324], const AvAvi *a, bool finished)
{
    memset(data, 0, 324);
    memcpy(data, "RIFF", 4);
    le32(data + 4, 316 + a->bytes + (finished ? 8 + a->frames * 32 : 0));
    memcpy(data + 8, "AVI LIST", 8);
    le32(data + 16, 292);
    memcpy(data + 20, "hdrlavih", 8);
    le32(data + 28, 56);
    le32(data + 32, AUDIO_FRAME_MS * 1000);
    le32(data + 36, a->frames ? (uint32_t)((uint64_t)a->bytes * AUDIO_FRAME_RATE / a->frames) : 0);
    le32(data + 44, finished ? 0x110 : 0);
    le32(data + 48, a->frames);
    le32(data + 56, 2);
    le32(data + 60, a->largest > AUDIO_BLOCK_BYTES ? a->largest : AUDIO_BLOCK_BYTES);
    le32(data + 64, a->width);
    le32(data + 68, a->height);
    memcpy(data + 88, "LIST", 4);
    le32(data + 92, 116);
    memcpy(data + 96, "strlstrh", 8);
    le32(data + 104, 56);
    memcpy(data + 108, "vidsMJPG", 8);
    le32(data + 128, 1);
    le32(data + 132, AUDIO_FRAME_RATE);
    le32(data + 140, a->frames);
    le32(data + 144, a->largest);
    le32(data + 148, UINT32_MAX);
    data[160] = a->width & 255;
    data[161] = a->width >> 8;
    data[162] = a->height & 255;
    data[163] = a->height >> 8;
    memcpy(data + 164, "strf", 4);
    le32(data + 168, 40);
    le32(data + 172, 40);
    le32(data + 176, a->width);
    le32(data + 180, a->height);
    data[184] = 1;
    data[186] = 24;
    memcpy(data + 188, "MJPG", 4);
    le32(data + 192, a->width * a->height * 3);
    memcpy(data + 212, "LIST", 4);
    le32(data + 216, 92);
    memcpy(data + 220, "strlstrh", 8);
    le32(data + 228, 56);
    memcpy(data + 232, "auds", 4);
    le32(data + 252, 2);
    le32(data + 256, MIC_RATE * 2);
    le32(data + 264, a->frames * AUDIO_BLOCK_BYTES / 2);
    le32(data + 268, AUDIO_BLOCK_BYTES);
    le32(data + 272, UINT32_MAX);
    le32(data + 276, 2);
    memcpy(data + 288, "strf", 4);
    le32(data + 292, 16);
    data[296] = 1;
    data[298] = 1;
    le32(data + 300, MIC_RATE);
    le32(data + 304, MIC_RATE * 2);
    data[308] = 2;
    data[310] = 16;
    memcpy(data + 312, "LIST", 4);
    le32(data + 316, 4 + a->bytes);
    memcpy(data + 320, "movi", 4);
}

bool av_avi_begin(AvAvi *a, FILE *file, unsigned width, unsigned height)
{
    *a = (AvAvi){.file = file, .width = width, .height = height};
    if (!file || !width || width > WC04_MAX_WIDTH || !height || height > WC04_MAX_HEIGHT) {
        a->failed = true;
        return false;
    }
    uint8_t data[324];
    header(data, a, false);
    return write_bytes(a, data, sizeof(data));
}

bool av_avi_has_space(const AvAvi *a, size_t length)
{
    return !a->failed && !a->finished && a->file && a->frames < AV_MAX_FRAMES
           && length <= WC04_FRAME_CAPACITY
           && 324ull + a->bytes + 16 + length + (length & 1) + AUDIO_BLOCK_BYTES + 8
                      + (a->frames + 1) * 32
                  <= AVI_MAX_BYTES;
}

static bool chunk(AvAvi *a, const char *id, const uint8_t *data, unsigned length, unsigned index)
{
    uint8_t prefix[8], pad = 0;
    memcpy(prefix, id, 4);
    le32(prefix + 4, length);
    if (!write_bytes(a, prefix, 8) || !write_bytes(a, data, length)
        || ((length & 1) && !write_bytes(a, &pad, 1)))
        return false;
    a->offsets[index] = 4 + a->bytes;
    a->lengths[index] = length;
    a->bytes += 8 + length + (length & 1);
    return true;
}

bool av_avi_pair(AvAvi *a, const uint8_t *jpeg, size_t length, const uint8_t *pcm)
{
    if (!av_avi_has_space(a, length) || !pcm || !jpeg || length < 4 || jpeg[0] != 0xff
        || jpeg[1] != 0xd8 || jpeg[length - 2] != 0xff || jpeg[length - 1] != 0xd9)
        return false;
    if (!chunk(a, "00dc", jpeg, (unsigned)length, a->frames * 2)
        || !chunk(a, "01wb", pcm, AUDIO_BLOCK_BYTES, a->frames * 2 + 1))
        return false;
    ++a->frames;
    if (length > a->largest) a->largest = (unsigned)length;
    return true;
}

bool av_avi_finish(AvAvi *a)
{
    if (a->failed || a->finished || !a->frames) return false;
    uint8_t entry[16];
    memcpy(entry, "idx1", 4);
    le32(entry + 4, a->frames * 32);
    if (!write_bytes(a, entry, 8)) return false;
    for (unsigned n = 0; n < a->frames * 2; ++n) {
        memcpy(entry, n & 1 ? "01wb" : "00dc", 4);
        le32(entry + 4, 0x10);
        le32(entry + 8, a->offsets[n]);
        le32(entry + 12, a->lengths[n]);
        if (!write_bytes(a, entry, sizeof(entry))) return false;
    }
    uint8_t data[324];
    header(data, a, true);
    if (fseek(a->file, 0, SEEK_SET) != 0) {
        a->failed = true;
        return false;
    }
    bool ok = write_bytes(a, data, sizeof(data)) && fflush(a->file) == 0;
    a->failed = !ok;
    a->finished = ok;
    return ok;
}
