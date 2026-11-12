/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "av_avi.h"
#include "avi.h"
#include "mjpeg.h"
#include <assert.h>
#include <jpeglib.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { WIDTH = 64, HEIGHT = 48, MAX_WIDTH = 80 };

typedef enum { YUV420, YUV422, YUV444, GRAY, RESTARTS, PROGRESSIVE } Kind;

static unsigned char *encode(Kind kind, unsigned width, unsigned height, unsigned long *size)
{
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr error;
    c.err = jpeg_std_error(&error);
    jpeg_create_compress(&c);
    unsigned char *out = NULL;
    jpeg_mem_dest(&c, &out, size);
    c.image_width = width;
    c.image_height = height;
    c.input_components = kind == GRAY ? 1 : 3;
    c.in_color_space = kind == GRAY ? JCS_GRAYSCALE : JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 80, TRUE);
    if (kind == YUV422 || kind == YUV444) {
        c.comp_info[0].h_samp_factor = kind == YUV422 ? 2 : 1;
        c.comp_info[0].v_samp_factor = 1;
    }
    if (kind == RESTARTS) c.restart_interval = 2;
    if (kind == PROGRESSIVE) jpeg_simple_progression(&c);
    jpeg_start_compress(&c, TRUE);
    uint8_t row[MAX_WIDTH * 3];
    while (c.next_scanline < height) {
        for (unsigned x = 0; x < width; ++x) {
            row[x * c.input_components] = (uint8_t)(x * 4 + (x * c.next_scanline & 7) * 9);
            if (kind == GRAY) continue;
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

static size_t scan_start(const uint8_t *jpeg)
{
    size_t at = 2;
    while (jpeg[at + 1] != 0xda)
        at += 2 + ((size_t)jpeg[at + 2] << 8 | jpeg[at + 3]);
    return at + 2 + ((size_t)jpeg[at + 2] << 8 | jpeg[at + 3]);
}

/* Copies the frame with `zeros` zero bytes put before its final FFD9. */
static size_t pad_frame(const uint8_t *jpeg, size_t length, size_t zeros, uint8_t *out)
{
    memcpy(out, jpeg, length - 2);
    memset(out + length - 2, 0, zeros);
    memcpy(out + length - 2 + zeros, jpeg + length - 2, 2);
    return length + zeros;
}

/* Whatever the frame holds, only up to 8 zero bytes may ever be cut. */
static void check_safe(const uint8_t *jpeg, size_t length)
{
    size_t end = mjpeg_scan_end(jpeg, length);
    assert(end <= length - 2 && length - 2 - end <= 8);
    for (size_t i = end; i < length - 2; ++i)
        assert(!jpeg[i]);
}

static void check_padding(const uint8_t *jpeg, size_t length)
{
    static uint8_t padded[65536];
    assert(mjpeg_scan_end(jpeg, length) == length - 2);
    for (size_t zeros = 1; zeros <= 8; ++zeros) {
        size_t size = pad_frame(jpeg, length, zeros, padded);
        assert(mjpeg_scan_end(padded, size) == length - 2);
    }
    size_t size = pad_frame(jpeg, length, 9, padded);
    assert(mjpeg_scan_end(padded, size) == size - 2);
    size = pad_frame(jpeg, length, 3, padded);
    padded[length] = 1;
    assert(mjpeg_scan_end(padded, size) == size - 2);
    padded[length - 2] = 0xff;
    padded[length - 1] = 0;
    padded[length] = 0;
    assert(mjpeg_scan_end(padded, size) == size - 2);
    size = pad_frame(jpeg, length - 3, 0, padded);
    assert(mjpeg_scan_end(padded, size) == size - 2);
}

typedef struct {
    uint8_t bytes[64];
    size_t size;
    unsigned byte, count;
} Scan;

static void put(Scan *s, const char *bits)
{
    for (; *bits; ++bits) {
        s->byte = s->byte << 1 | (unsigned)(*bits == '1');
        if (++s->count < 8) continue;
        s->bytes[s->size++] = (uint8_t)s->byte;
        if (s->byte == 0xff) s->bytes[s->size++] = 0;
        s->byte = s->count = 0;
    }
}

static void align(Scan *s)
{
    while (s->count)
        put(s, "1");
}

/* DC: 0 size 0, 1 size 7. AC: 00 end, 01 sixteen zeros, 10 run 0 size 1, 110 run 14 size 1. */
static const uint8_t codes_dht[] = {
    0xff, 0xc4, 0x00, 0x2a, 0x00, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x07,
    0x10, 0, 3, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0xf0, 0x01, 0xe1,
};
/* DC as above. AC: 0 run 0 size 1, 1 end of block. */
static const uint8_t ones_dht[] = {
    0xff, 0xc4, 0x00, 0x28, 0x00, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x07,
    0x10, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x01, 0x00,
};

/* A gray frame of `blocks` stacked 8x8 blocks, with `zeros` zero bytes before the FFD9. */
static size_t tiny_frame(uint8_t *out, const uint8_t *dht, size_t dht_size, unsigned blocks,
                         const Scan *scan, size_t zeros)
{
    static const uint8_t sos[] = {0xff, 0xda, 0, 8, 1, 1, 0, 0, 63, 0};
    uint8_t sof[] = {0xff, 0xc0, 0, 11, 8, 0, (uint8_t)(blocks * 8), 0, 8, 1, 1, 0x11, 0};
    size_t size = 0;
    out[size++] = 0xff;
    out[size++] = 0xd8;
    memcpy(out + size, (uint8_t[]){0xff, 0xdb, 0, 67, 0}, 5);
    memset(out + size + 5, 1, 64);
    size += 69;
    memcpy(out + size, sof, sizeof(sof));
    size += sizeof(sof);
    memcpy(out + size, dht, dht_size);
    size += dht_size;
    memcpy(out + size, sos, sizeof(sos));
    size += sizeof(sos);
    memcpy(out + size, scan->bytes, scan->size);
    size += scan->size;
    memset(out + size, 0, zeros);
    size += zeros;
    out[size++] = 0xff;
    out[size++] = 0xd9;
    return size;
}

/* Zeros after the scan are cut back to its real end, unless the scan itself is wrong. */
static void check_tiny(const uint8_t *dht, size_t dht_size, unsigned blocks, const Scan *scan,
                       bool valid)
{
    static uint8_t frame[512];
    size_t exact = tiny_frame(frame, dht, dht_size, blocks, scan, 0);
    assert(mjpeg_scan_end(frame, exact) == exact - 2);
    for (size_t zeros = 1; zeros <= 8; ++zeros) {
        size_t size = tiny_frame(frame, dht, dht_size, blocks, scan, zeros);
        assert(mjpeg_scan_end(frame, size) == (valid ? exact : size) - 2);
    }
}

static void test_tiny_scans(void)
{
    Scan s = {0};
    /* Eight empty blocks are three real zero bytes, which stripping zeros would eat. */
    for (int i = 0; i < 8; ++i)
        put(&s, "000");
    align(&s);
    assert(s.size == 3 && !s.bytes[0] && !s.bytes[1] && !s.bytes[2]);
    check_tiny(codes_dht, sizeof(codes_dht), 8, &s, true);

    /* A stuffed FF00 counts as one data byte. */
    s = (Scan){0};
    put(&s, "1111111100");
    put(&s, "000");
    align(&s);
    assert(s.size == 3 && s.bytes[0] == 0xff && !s.bytes[1] && s.bytes[2] == 7);
    check_tiny(codes_dht, sizeof(codes_dht), 2, &s, true);

    /* A last data byte of FF keeps its stuffed zero. */
    s = (Scan){0};
    put(&s, "111111111");
    align(&s);
    assert(s.size == 4 && s.bytes[2] == 0xff && !s.bytes[3]);
    check_tiny(ones_dht, sizeof(ones_dht), 1, &s, true);

    /* Zero runs: three sixteen-zero codes, then a coefficient and the end of the block. */
    s = (Scan){0};
    put(&s, "0" "010101" "10" "1" "00");
    align(&s);
    check_tiny(codes_dht, sizeof(codes_dht), 1, &s, true);

    /* The last coefficient lands on 63, so the block has no end code. */
    s = (Scan){0};
    put(&s, "0" "010101" "110" "1");
    align(&s);
    check_tiny(codes_dht, sizeof(codes_dht), 1, &s, true);

    /* Four sixteen-zero codes run past the block, and 111 is no code at all. */
    s = (Scan){0};
    put(&s, "0" "01010101" "00");
    align(&s);
    check_tiny(codes_dht, sizeof(codes_dht), 1, &s, false);
    s = (Scan){0};
    put(&s, "0" "111" "00");
    align(&s);
    check_tiny(codes_dht, sizeof(codes_dht), 1, &s, false);
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

static void test_walked_frames(void)
{
    static const struct {
        Kind kind;
        unsigned width, height;
    } cases[] = {{YUV420, 64, 48}, {YUV420, 70, 50}, {YUV422, 64, 48}, {YUV422, 75, 33},
                 {YUV444, 64, 48}, {GRAY, 64, 48},   {GRAY, 61, 41}};
    static uint8_t padded[65536], mutated[65536];
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        unsigned long size;
        unsigned char *frame = encode(cases[i].kind, cases[i].width, cases[i].height, &size);
        check_padding(frame, size);
        size_t padded_size = pad_frame(frame, size, 3, padded);
        for (size_t cut = 4; cut <= padded_size; ++cut) {
            memcpy(mutated, padded, cut - 2);
            memcpy(mutated + cut - 2, "\xff\xd9", 2);
            check_safe(mutated, cut);
        }
        for (size_t at = 0; at < padded_size - 2; ++at) {
            memcpy(mutated, padded, padded_size);
            mutated[at] ^= (uint8_t)(1u << (at % 8));
            check_safe(mutated, padded_size);
        }
        memcpy(mutated, padded, padded_size);
        memcpy(mutated + scan_start(padded) + 8, "\xff\xd0", 2);
        assert(mjpeg_scan_end(mutated, padded_size) == padded_size - 2);
        free(frame);
    }

    unsigned long size;
    unsigned char *gray = encode(GRAY, 64, 48, &size);
    size_t sof = 2;
    while (gray[sof + 1] != 0xc0)
        sof += 2 + ((size_t)gray[sof + 2] << 8 | gray[sof + 3]);
    gray[sof + 11] = 0x22;
    check_padding(gray, size);
    gray[scan_start(gray) - 5] ^= 0x10;
    size_t padded_size = pad_frame(gray, size, 2, padded);
    assert(mjpeg_scan_end(padded, padded_size) == padded_size - 2);
    free(gray);

    const Kind never[] = {RESTARTS, PROGRESSIVE};
    for (size_t i = 0; i < sizeof(never) / sizeof(never[0]); ++i) {
        unsigned char *frame = encode(never[i], 64, 48, &size);
        padded_size = pad_frame(frame, size, 2, padded);
        assert(mjpeg_scan_end(frame, size) == size - 2);
        assert(mjpeg_scan_end(padded, padded_size) == padded_size - 2);
        free(frame);
    }
}

int main(void)
{
    unsigned long full_size = 0;
    unsigned char *full = encode(YUV420, WIDTH, HEIGHT, &full_size);
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

    static uint8_t padded[65536], stored[65536];
    size_t padded_size = pad_frame(bare, bare_size, 3, padded);
    f = tmpfile();
    assert(f && avi_begin(&avi, f, WIDTH, HEIGHT) && avi_append(&avi, padded, padded_size));
    assert(avi_finish(&avi, 100) && !fflush(f) && avi.lengths[0] == length);
    assert(first_frame(f, stored) == length && !memcmp(stored, saved, length));
    fclose(f);
    padded_size = pad_frame(full, full_size, 2, padded);
    f = tmpfile();
    assert(f && avi_begin(&avi, f, WIDTH, HEIGHT) && avi_append(&avi, padded, padded_size));
    assert(avi_finish(&avi, 100) && !fflush(f) && avi.lengths[0] == full_size);
    assert(first_frame(f, stored) == full_size && !memcmp(stored, full, full_size));
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
    padded_size = pad_frame(bare, bare_size, 3, padded);
    f = tmpfile();
    assert(f && av_avi_begin(&av, f, WIDTH, HEIGHT) && av_avi_pair(&av, padded, padded_size, pcm));
    assert(av_avi_finish(&av) && !fflush(f) && av.largest == length);
    assert(first_frame(f, stored) == length && !memcmp(stored, saved, length));
    fclose(f);

    const uint8_t odd[] = {0xff, 0xd8, 0xff, 0xe0, 0, 4, 0, 0, 0xff, 0xd9};
    assert(!mjpeg_dht_insert_at(odd, sizeof(odd)));
    for (size_t n = 0; n < bare_size; ++n) {
        size_t at = mjpeg_dht_insert_at(bare, n);
        assert(!at || (at == sos && n >= sos + 4));
    }
    bare[4] = 0xff;
    assert(!mjpeg_dht_insert_at(bare, bare_size));
    test_tiny_scans();
    test_walked_frames();
    free(full);
    puts("PASS: standard tables spliced before SOS in both writers, identical decode, frames with tables untouched, truncated and odd frames left alone, zeros after the last MCU cut in both writers but never real data");
}
