/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef WC04_TEST_UVC_FIXTURE_H
#define WC04_TEST_UVC_FIXTURE_H
#include <assert.h>
#include <string.h>
#include "uvc.h"

typedef struct {
    uint8_t data[4096];
    size_t length, vc, vs, header, format, frame, alt, endpoint;
} Fixture;

static inline size_t append(Fixture *f, const uint8_t *data, size_t length)
{
    size_t at = f->length;
    assert(at + length <= sizeof(f->data));
    memcpy(f->data + at, data, length);
    f->length += length;
    f->data[2] = (uint8_t)f->length;
    f->data[3] = (uint8_t)(f->length >> 8);
    return at;
}

/* Synthetic layouts: these are NOT captured descriptors from another camera. */
static inline Fixture fixture(unsigned interface, unsigned control, unsigned endpoint,
                              bool continuous)
{
    Fixture f = {0};
    const uint8_t config[] = {9, 2, 0, 0, 2, 1, 0, 0x80, 100};
    append(&f, config, sizeof(config));
    uint8_t vc[] = {9, 4, control, 0,  0, 14, 1, 0, 0, 13, 0x24,
                    1, 0, 1,       13, 0, 0,  0, 0, 0, 1,  interface};
    f.vc = append(&f, vc, sizeof(vc)) + 9;
    uint8_t vs[] = {9, 4, interface, 0, 0, 14, 2, 0, 0};
    f.vs = append(&f, vs, sizeof(vs));
    uint8_t header[] = {14, 0x24, 1, 1, 0, 0, endpoint, 0, 1, 0, 0, 0, 1, 0};
    f.header = append(&f, header, sizeof(header));
    const uint8_t format[] = {11, 0x24, 6, 2, 1, 0, 7, 0, 0, 0, 0};
    f.format = append(&f, format, sizeof(format));
    uint8_t frame[38] = {0, 0x24, 7, 7, 0, 0x40, 1, 0xf0, 0};
    frame[0] = continuous ? 38 : 34;
    uvc_put32(frame + 17, 153600);
    uvc_put32(frame + 21, 333333);
    frame[25] = continuous ? 0 : 2;
    uvc_put32(frame + 26, 333333);
    uvc_put32(frame + 30, continuous ? 9999990 : 666666);
    uvc_put32(frame + 34, 333333);
    f.frame = append(&f, frame, frame[0]);
    f.data[f.header + 4] = (uint8_t)(f.length - f.header);
    uint8_t alt[] = {9, 4, interface, 1, 1, 14, 2, 0, 0};
    f.alt = append(&f, alt, sizeof(alt));
    uint8_t ep[] = {7, 5, endpoint, 5, 0, 4, 1};
    f.endpoint = append(&f, ep, sizeof(ep));
    return f;
}
#endif
