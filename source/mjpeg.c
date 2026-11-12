/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */
#include "mjpeg.h"
#include <stdbool.h>
#include <string.h>

/* ITU-T T.81 Annex K.3 tables, in libjpeg's order: DC luma, AC luma, DC chroma, AC chroma. */
const uint8_t mjpeg_dht[MJPEG_DHT_BYTES] = {
    0xff, 0xc4, 0x01, 0xa2, 0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
    0x09, 0x0a, 0x0b, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04,
    0x04, 0x00, 0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31,
    0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08, 0x23,
    0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
    0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57,
    0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76,
    0x77, 0x78, 0x79, 0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94,
    0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa,
    0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3,
    0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
    0xf9, 0xfa, 0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
    0x0b, 0x11, 0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00,
    0x01, 0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41,
    0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1,
    0x09, 0x23, 0x33, 0x52, 0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25,
    0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38, 0x39,
    0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
    0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77,
    0x78, 0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94,
    0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa,
    0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5, 0xc6, 0xc7,
    0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4,
    0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

size_t mjpeg_dht_insert_at(const uint8_t *jpeg, size_t length)
{
    if (!jpeg || length < 4 || jpeg[0] != 0xff || jpeg[1] != 0xd8) return 0;
    size_t at = 2;
    while (at + 4 <= length) {
        if (jpeg[at] != 0xff) return 0;
        uint8_t marker = jpeg[at + 1];
        if (marker == 0xff) {
            ++at;
            continue;
        }
        if (marker == 0xc4) return 0;
        if (marker == 0xda) return at;
        if (marker == 0xd9 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01) return 0;
        size_t segment = (size_t)jpeg[at + 2] << 8 | jpeg[at + 3];
        if (segment < 2 || segment > length - at - 2) return 0;
        at += 2 + segment;
    }
    return 0;
}

enum { FAST_BITS = 8, MAX_PADDING = 8 };

typedef struct {
    uint16_t fast[1 << FAST_BITS];
    int32_t maxcode[17];
    int32_t offset[17];
    uint8_t symbols[256];
    bool defined;
} Huffman;

typedef struct {
    uint8_t id, h, v;
} Component;

typedef struct {
    Huffman huffman[2][2];
    Component component[3];
    unsigned width, height, components;
} Frame;

typedef struct {
    const uint8_t *next, *end;
    uint32_t bits;
    unsigned count, fake;
    bool bad;
} Reader;

static bool huffman_define(Huffman *h, const uint8_t counts[16], const uint8_t *symbols,
                           unsigned total)
{
    memcpy(h->symbols, symbols, total);
    memset(h->fast, 0, sizeof(h->fast));
    int32_t code = 0;
    unsigned index = 0;
    for (unsigned length = 1; length <= 16; ++length) {
        unsigned n = counts[length - 1];
        if (code + (int32_t)n > 1 << length) return false;
        h->offset[length] = (int32_t)index - code;
        h->maxcode[length] = n ? code + (int32_t)n - 1 : -1;
        for (; n; --n, ++code, ++index) {
            if (length > FAST_BITS) continue;
            unsigned span = 1u << (FAST_BITS - length), first = (unsigned)code * span;
            for (unsigned i = 0; i < span; ++i)
                h->fast[first + i] = (uint16_t)(length << 8 | symbols[index]);
        }
        code <<= 1;
    }
    h->defined = true;
    return true;
}

static bool read_dht(Frame *f, const uint8_t *segment, size_t size)
{
    while (size) {
        if (size < 17) return false;
        unsigned type = segment[0] >> 4, id = segment[0] & 15, total = 0;
        for (unsigned i = 1; i <= 16; ++i)
            total += segment[i];
        if (type > 1 || id > 1 || total > 256 || size < 17 + total
            || !huffman_define(&f->huffman[type][id], segment + 1, segment + 17, total))
            return false;
        segment += 17 + total;
        size -= 17 + total;
    }
    return true;
}

static bool read_sof(Frame *f, const uint8_t *segment, size_t size)
{
    if (f->components || size < 6 || segment[0] != 8) return false;
    unsigned count = segment[5];
    if ((count != 1 && count != 3) || size != 6 + 3 * count) return false;
    f->height = (unsigned)segment[1] << 8 | segment[2];
    f->width = (unsigned)segment[3] << 8 | segment[4];
    if (!f->width || !f->height) return false;
    for (unsigned i = 0; i < count; ++i) {
        unsigned h = segment[7 + 3 * i] >> 4, v = segment[7 + 3 * i] & 15;
        if (h < 1 || h > 4 || v < 1 || v > 4) return false;
        /* A lone component is scanned block by block whatever its sampling factors say. */
        f->component[i] = (Component){segment[6 + 3 * i], count == 1 ? 1 : h, count == 1 ? 1 : v};
    }
    f->components = count;
    return true;
}

static void fill(Reader *r)
{
    while (r->count <= 24) {
        uint8_t byte = 0;
        if (r->next < r->end) {
            byte = *r->next++;
            if (byte == 0xff) {
                if (r->next < r->end && *r->next == 0)
                    ++r->next;
                else
                    r->bad = true;
            }
        } else {
            ++r->fake;
        }
        r->bits = r->bits << 8 | byte;
        r->count += 8;
    }
}

static void skip_bits(Reader *r, unsigned n)
{
    if (r->count < n) fill(r);
    r->count -= n;
}

static int read_symbol(Reader *r, const Huffman *h)
{
    if (r->count < 16) fill(r);
    unsigned entry = h->fast[r->bits >> (r->count - FAST_BITS) & ((1u << FAST_BITS) - 1)];
    if (entry) {
        r->count -= entry >> 8;
        return (int)(entry & 255);
    }
    for (unsigned length = FAST_BITS + 1; length <= 16; ++length) {
        int32_t code = (int32_t)(r->bits >> (r->count - length) & ((1u << length) - 1));
        if (code <= h->maxcode[length]) {
            r->count -= length;
            return h->symbols[code + h->offset[length]];
        }
    }
    return -1;
}

static bool skip_block(Reader *r, const Huffman *dc, const Huffman *ac)
{
    int size = read_symbol(r, dc);
    if (size < 0 || size > 11) return false;
    skip_bits(r, (unsigned)size);
    for (unsigned k = 1; k < 64;) {
        int symbol = read_symbol(r, ac);
        if (symbol < 0) return false;
        unsigned run = (unsigned)symbol >> 4, bits = (unsigned)symbol & 15;
        if (!bits) {
            if (run != 15) {
                if (run) return false;
                break;
            }
            k += 16;
        } else {
            if (bits > 10) return false;
            k += run + 1;
            skip_bits(r, bits);
        }
        if (k > 64) return false;
    }
    return !r->bad && r->count >= r->fake * 8;
}

static bool scan_used(const Frame *f, const uint8_t *sos, size_t sos_size, const uint8_t *data,
                      size_t size, size_t *used)
{
    unsigned n = f->components, hmax = 1, vmax = 1, blocks[3];
    const Huffman *dc[3], *ac[3];
    if (!n || sos_size != 4 + 2 * n || sos[0] != n || sos[sos_size - 3] != 0
        || sos[sos_size - 2] != 63 || sos[sos_size - 1] != 0)
        return false;
    for (unsigned i = 0; i < n; ++i) {
        unsigned td = sos[2 + 2 * i] >> 4, ta = sos[2 + 2 * i] & 15;
        if (sos[1 + 2 * i] != f->component[i].id || td > 1 || ta > 1
            || !f->huffman[0][td].defined || !f->huffman[1][ta].defined)
            return false;
        dc[i] = &f->huffman[0][td];
        ac[i] = &f->huffman[1][ta];
        blocks[i] = f->component[i].h * f->component[i].v;
        if (f->component[i].h > hmax) hmax = f->component[i].h;
        if (f->component[i].v > vmax) vmax = f->component[i].v;
    }
    size_t mcus = (size_t)((f->width + 8 * hmax - 1) / (8 * hmax))
                  * ((f->height + 8 * vmax - 1) / (8 * vmax));
    Reader r = {.next = data, .end = data + size};
    for (size_t mcu = 0; mcu < mcus; ++mcu)
        for (unsigned i = 0; i < n; ++i)
            for (unsigned b = 0; b < blocks[i]; ++b)
                if (!skip_block(&r, dc[i], ac[i])) return false;
    /* Prefetched whole bytes go back; a stuffed FF00 pair counts as one data byte. */
    const uint8_t *stop = r.next;
    for (unsigned spare = r.count / 8 - r.fake; spare; --spare)
        stop -= stop - data >= 2 && stop[-1] == 0 && stop[-2] == 0xff ? 2 : 1;
    *used = (size_t)(stop - data);
    if (size - *used > MAX_PADDING) return false;
    for (; stop < data + size; ++stop)
        if (*stop) return false;
    return true;
}

size_t mjpeg_scan_end(const uint8_t *jpeg, size_t length)
{
    const size_t eoi = length - 2;
    Frame f = {0};
    bool tables = false;
    for (size_t at = 2; at + 4 <= eoi;) {
        if (jpeg[at] != 0xff) return eoi;
        uint8_t marker = jpeg[at + 1];
        if (marker == 0xff) {
            ++at;
            continue;
        }
        size_t segment = (size_t)jpeg[at + 2] << 8 | jpeg[at + 3];
        if (segment < 2 || segment > eoi - at - 2) return eoi;
        const uint8_t *body = jpeg + at + 4;
        size_t size = segment - 2;
        at += 2 + segment;
        if (marker == 0xc0) {
            if (!read_sof(&f, body, size)) return eoi;
        } else if (marker == 0xc4) {
            tables = true;
            if (!read_dht(&f, body, size)) return eoi;
        } else if (marker == 0xdd) {
            if (size != 2 || body[0] || body[1]) return eoi;
        } else if (marker == 0xda) {
            /* Without tables the writers splice in the standard ones, so the walk uses those. */
            if (!tables) read_dht(&f, mjpeg_dht + 4, MJPEG_DHT_BYTES - 4);
            size_t used;
            return scan_used(&f, body, size, jpeg + at, eoi - at, &used) ? at + used : eoi;
        } else if (marker != 0xdb && marker != 0xfe && (marker & 0xf0) != 0xe0) {
            return eoi;
        }
    }
    return eoi;
}

size_t mjpeg_frame_parts(const uint8_t *jpeg, size_t length, MjpegPart parts[MJPEG_PARTS])
{
    size_t insert = mjpeg_dht_insert_at(jpeg, length);
    size_t end = mjpeg_scan_end(jpeg, length), count = 0;
    if (insert) {
        parts[count++] = (MjpegPart){jpeg, insert};
        parts[count++] = (MjpegPart){mjpeg_dht, MJPEG_DHT_BYTES};
    }
    parts[count++] = (MjpegPart){jpeg + insert, end - insert};
    parts[count++] = (MjpegPart){jpeg + length - 2, 2};
    return count;
}
