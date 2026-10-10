/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */
#include "avi.h"
#include "frame_limits.h"
#include <string.h>

static void le32(uint8_t *p, uint32_t n)
{
    for (unsigned i = 0; i < 4; ++i)
        p[i] = (uint8_t)(n >> (i * 8));
}

static bool write_bytes(Avi *a, const void *data, size_t length)
{
    if (a->failed || fwrite(data, 1, length, a->file) != length || ferror(a->file)) {
        a->failed = true;
        return false;
    }
    return true;
}

static void header(uint8_t data[224], const Avi *a, unsigned duration, bool finished)
{
    memset(data, 0, 224);
    memcpy(data, "RIFF", 4);
    le32(data + 4, 216 + a->bytes + (finished ? 8 + a->frames * 16 : 0));
    memcpy(data + 8, "AVI LIST", 8);
    le32(data + 16, 192);
    memcpy(data + 20, "hdrlavih", 8);
    le32(data + 28, 56);
    le32(data + 32, a->frames ? (uint32_t)((uint64_t)duration * 1000 / a->frames) : 33333);
    le32(data + 36, duration ? (uint32_t)((uint64_t)a->bytes * 1000 / duration) : 0);
    le32(data + 44, finished ? 0x10 : 0);
    le32(data + 48, a->frames);
    le32(data + 56, 1);
    le32(data + 60, a->largest);
    le32(data + 64, a->width);
    le32(data + 68, a->height);
    memcpy(data + 88, "LIST", 4);
    le32(data + 92, 116);
    memcpy(data + 96, "strlstrh", 8);
    le32(data + 104, 56);
    memcpy(data + 108, "vidsMJPG", 8);
    le32(data + 128, duration ? duration : 1);
    le32(data + 132, a->frames ? a->frames * 1000 : 30);
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
    le32(data + 216, 4 + a->bytes);
    memcpy(data + 220, "movi", 4);
}

bool avi_begin(Avi *a, FILE *file, unsigned width, unsigned height)
{
    *a = (Avi){.file = file, .width = width, .height = height};
    if (!file || !width || width > 640 || !height || height > 480) {
        a->failed = true;
        return false;
    }
    uint8_t data[224];
    header(data, a, 0, false);
    return write_bytes(a, data, sizeof(data));
}

bool avi_has_space(const Avi *a, size_t length)
{
    return !a->failed && !a->finished && a->file && a->frames < AVI_MAX_FRAMES
           && length <= WC04_FRAME_CAPACITY
           && (uint64_t)224 + a->bytes + 8 + length + (length & 1) + 8 + (a->frames + 1) * 16
                  <= AVI_MAX_BYTES;
}

bool avi_append(Avi *a, const uint8_t *jpeg, size_t length)
{
    if (!avi_has_space(a, length) || !jpeg || length < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8
        || jpeg[length - 2] != 0xff || jpeg[length - 1] != 0xd9)
        return false;
    uint8_t chunk[8];
    memcpy(chunk, "00dc", 4);
    le32(chunk + 4, (uint32_t)length);
    const uint8_t pad = 0;
    if (!write_bytes(a, chunk, 8) || !write_bytes(a, jpeg, length)
        || ((length & 1) && !write_bytes(a, &pad, 1)))
        return false;
    a->offsets[a->frames] = 4 + a->bytes;
    a->lengths[a->frames] = (uint32_t)length;
    ++a->frames;
    a->bytes += (unsigned)(8 + length + (length & 1));
    if (length > a->largest) a->largest = (unsigned)length;
    return true;
}

bool avi_finish(Avi *a, unsigned duration)
{
    if (a->failed || a->finished || !a->frames || !duration || duration > AVI_MAX_MS) return false;
    uint8_t chunk[16];
    memcpy(chunk, "idx1", 4);
    le32(chunk + 4, a->frames * 16);
    if (!write_bytes(a, chunk, 8)) return false;
    for (unsigned n = 0; n < a->frames; ++n) {
        memcpy(chunk, "00dc", 4);
        le32(chunk + 4, 0x10);
        le32(chunk + 8, a->offsets[n]);
        le32(chunk + 12, a->lengths[n]);
        if (!write_bytes(a, chunk, sizeof(chunk))) return false;
    }
    uint8_t data[224];
    header(data, a, duration, true);
    if (fseek(a->file, 0, SEEK_SET) != 0) {
        a->failed = true;
        return false;
    }
    bool ok = write_bytes(a, data, sizeof(data)) && fflush(a->file) == 0;
    a->failed = !ok;
    a->finished = ok;
    return ok;
}
