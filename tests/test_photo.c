/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 cmyksoda
 */

#include "photo.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static size_t strip_huffman(const uint8_t *src, size_t size, uint8_t *dest)
{
    size_t in = 2, out = 2;
    memcpy(dest, src, 2);
    while (in + 4 <= size) {
        assert(src[in] == 0xff);
        if (src[in + 1] == 0xda) {
            memcpy(dest + out, src + in, size - in);
            return out + size - in;
        }
        size_t length = ((size_t)src[in + 2] << 8 | src[in + 3]) + 2;
        assert(length >= 4 && length <= size - in);
        if (src[in + 1] != 0xc4) {
            memcpy(dest + out, src + in, length);
            out += length;
        }
        in += length;
    }
    abort();
}

int main(void)
{
    uint8_t pixels[32 * 24 * 3], output[sizeof(pixels)];
    for (unsigned y = 0; y < 24; ++y) {
        for (unsigned x = 0; x < 32; ++x) {
            size_t at = (y * 32 + x) * 3;
            pixels[at] = (uint8_t)(x * 8);
            pixels[at + 1] = (uint8_t)(y * 10);
            pixels[at + 2] = 120;
        }
    }
    FILE *file = fopen("build/test-photo.jpg", "wb+");
    assert(file);
    assert(photo_encode(file, pixels, 32, 24));
    assert(fflush(file) == 0);
    long size = ftell(file);
    assert(size > 0);
    rewind(file);
    uint8_t *jpeg = malloc((size_t)size), *abbreviated = malloc((size_t)size);
    assert(jpeg && abbreviated);
    assert(fread(jpeg, 1, (size_t)size, file) == (size_t)size);
    assert(fclose(file) == 0);
    assert(photo_decode(jpeg, (size_t)size, output, 32, 24));
    unsigned long error = 0;
    for (size_t i = 0; i < sizeof(output); ++i)
        error += (unsigned)abs((int)pixels[i] - output[i]);
    assert(error / sizeof(output) < 5);
    assert(!photo_decode(jpeg, (size_t)size, output, 16, 24));
    assert(!photo_decode(jpeg, (size_t)size / 2, output, 32, 24));
    const uint8_t corrupt[] = {0xff, 0xd8, 0x42, 0xff, 0xd9};
    assert(!photo_decode(corrupt, sizeof(corrupt), output, 32, 24));
    size_t abbreviated_size = strip_huffman(jpeg, (size_t)size, abbreviated);
    assert(abbreviated_size < (size_t)size);
    assert(photo_decode(abbreviated, abbreviated_size, output, 32, 24));
    file = fopen("build/test-photo-normalized.jpg", "wb");
    assert(file && photo_encode(file, output, 32, 24));
    assert(fclose(file) == 0);
    for (size_t n = 0; n < (size_t)size; ++n)
        assert(!photo_decode(jpeg, n, output, 32, 24));
    free(abbreviated);
    free(jpeg);
    puts("PASS: JPEG round trip, missing DHT, corrupt data, every truncation, dimensions");
    return 0;
}
