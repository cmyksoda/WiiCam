/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "av_avi.h"
#include "avi.h"
#include "mjpeg.h"
#include <assert.h>
#include <jpeglib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 64, HEIGHT = 48 };

static unsigned char *encode(unsigned long *size)
{
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr error;
    c.err = jpeg_std_error(&error);
    jpeg_create_compress(&c);
    unsigned char *out = NULL;
    jpeg_mem_dest(&c, &out, size);
    c.image_width = WIDTH;
    c.image_height = HEIGHT;
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 80, TRUE);
    jpeg_start_compress(&c, TRUE);
    uint8_t row[WIDTH * 3];
    while (c.next_scanline < HEIGHT) {
        for (unsigned x = 0; x < WIDTH; ++x) {
            row[x * 3] = (uint8_t)(x * 4);
            row[x * 3 + 1] = (uint8_t)(c.next_scanline * 5);
            row[x * 3 + 2] = (uint8_t)((x ^ c.next_scanline) * 3);
        }
        JSAMPROW rows[1] = {row};
        jpeg_write_scanlines(&c, rows, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    return out;
}

static size_t strip_tables(const uint8_t *in, size_t length, uint8_t *out)
{
    size_t at = 2, used = 2;
    memcpy(out, in, 2);
    while (in[at + 1] != 0xda) {
        size_t segment = 2 + ((size_t)in[at + 2] << 8 | in[at + 3]);
        if (in[at + 1] != 0xc4) {
            memcpy(out + used, in + at, segment);
            used += segment;
        }
        at += segment;
    }
    memcpy(out + used, in + at, length - at);
    return used + length - at;
}

/* A frame with no tables makes libjpeg fail, so this also proves the tables are present. */
static void decode(const uint8_t *jpeg, size_t length, uint8_t *pixels)
{
    struct jpeg_decompress_struct d;
    struct jpeg_error_mgr error;
    d.err = jpeg_std_error(&error);
    jpeg_create_decompress(&d);
    jpeg_mem_src(&d, jpeg, length);
    assert(jpeg_read_header(&d, TRUE) == JPEG_HEADER_OK);
    jpeg_start_decompress(&d);
    while (d.output_scanline < HEIGHT) {
        JSAMPROW rows[1] = {pixels + d.output_scanline * WIDTH * 3};
        jpeg_read_scanlines(&d, rows, 1);
    }
    jpeg_finish_decompress(&d);
    jpeg_destroy_decompress(&d);
}

static size_t first_frame(FILE *f, uint8_t *out)
{
    static uint8_t file[65536];
    rewind(f);
    size_t size = fread(file, 1, sizeof(file), f);
    for (size_t at = 0; at + 8 < size; ++at)
        if (!memcmp(file + at, "00dc", 4)) {
            size_t length = file[at + 4] | file[at + 5] << 8 | (size_t)file[at + 6] << 16;
            memcpy(out, file + at + 8, length);
            return length;
        }
    return 0;
}

int main(void)
{
    unsigned long full_size = 0;
    unsigned char *full = encode(&full_size);
    static uint8_t bare[65536], saved[65536], want[WIDTH * HEIGHT * 3], got[WIDTH * HEIGHT * 3];
    size_t bare_size = strip_tables(full, full_size, bare);
    assert(bare_size + MJPEG_DHT_BYTES <= full_size);
    assert(!mjpeg_dht_insert_at(full, full_size));
    size_t sos = mjpeg_dht_insert_at(bare, bare_size);
    assert(sos && bare[sos] == 0xff && bare[sos + 1] == 0xda);
    decode(full, full_size, want);

    static Avi avi;
    FILE *f = tmpfile();
    assert(f && avi_begin(&avi, f, WIDTH, HEIGHT) && avi_append(&avi, bare, bare_size));
    assert(avi_append(&avi, full, full_size) && avi_finish(&avi, 100) && !fflush(f));
    assert(avi.lengths[0] == bare_size + MJPEG_DHT_BYTES && avi.lengths[1] == full_size);
    size_t length = first_frame(f, saved);
    assert(length == bare_size + MJPEG_DHT_BYTES && !memcmp(saved, bare, sos));
    assert(!memcmp(saved + sos, mjpeg_dht, MJPEG_DHT_BYTES));
    assert(!memcmp(saved + sos + MJPEG_DHT_BYTES, bare + sos, bare_size - sos));
    decode(saved, length, got);
    assert(!memcmp(want, got, sizeof(want)));
    fclose(f);

    static AvAvi av;
    static uint8_t pcm[AUDIO_BLOCK_BYTES];
    f = tmpfile();
    assert(f && av_avi_begin(&av, f, WIDTH, HEIGHT) && av_avi_pair(&av, bare, bare_size, pcm));
    assert(av_avi_finish(&av) && !fflush(f) && av.largest == bare_size + MJPEG_DHT_BYTES);
    length = first_frame(f, saved);
    memset(got, 0, sizeof(got));
    decode(saved, length, got);
    assert(!memcmp(want, got, sizeof(want)));
    fclose(f);

    const uint8_t odd[] = {0xff, 0xd8, 0xff, 0xe0, 0, 4, 0, 0, 0xff, 0xd9};
    assert(!mjpeg_dht_insert_at(odd, sizeof(odd)));
    for (size_t n = 0; n < bare_size; ++n) {
        size_t at = mjpeg_dht_insert_at(bare, n);
        assert(!at || (at == sos && n >= sos + 4));
    }
    bare[4] = 0xff;
    assert(!mjpeg_dht_insert_at(bare, bare_size));
    free(full);
    puts("PASS: standard tables spliced before SOS in both writers, identical decode, frames with tables untouched, truncated and odd frames left alone");
}
