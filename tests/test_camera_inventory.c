/* SPDX-License-Identifier: GPL-3.0-only */
#include "camera_inventory.h"
#include "uvc_fixture.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    unsigned lines;
    char text[CAMERA_INVENTORY_LINES * CAMERA_INVENTORY_LINE_BYTES];
} Report;

static void report(void *context, const char *message)
{
    Report *r = context;
    assert(++r->lines <= CAMERA_INVENTORY_LINES);
    assert(strlen(message) < CAMERA_INVENTORY_LINE_BYTES);
    assert(strncmp(message, "Inventory: ", 11) == 0);
    size_t used = strlen(r->text);
    int result = snprintf(r->text + used, sizeof(r->text) - used, "%s\n", message);
    assert(result >= 0 && (size_t)result < sizeof(r->text) - used);
}

static bool inspect(const Fixture *f, Report *r, CameraInventory *inventory)
{
    *r = (Report){0};
    Fixture before = *f;
    bool valid = camera_inventory(f->data, f->length, report, r, inventory);
    assert(memcmp(f, &before, sizeof(before)) == 0);
    assert(inventory->lines == r->lines);
    return valid;
}

int main(void)
{
    Fixture f = fixture(5, 3, 0x83, false);
    CameraInventory inventory;
    Report r;
    assert(inspect(&f, &r, &inventory));
    assert(inventory.mjpeg_frames == 1 && inventory.audio_formats == 0);
    assert(strstr(r.text, "format=2 frame=7 size=320x240 max_frame=153600"));
    assert(strstr(r.text, "intervals_100ns from=0: 333333 666666"));
    assert(strstr(r.text, "if=5 alt=1 address=83 attributes=05 max_packet_raw=0400 interval=1"));
    f = fixture(5, 3, 0x83, true);
    assert(inspect(&f, &r, &inventory));
    assert(strstr(r.text, "continuous_100ns min=333333 max=9999990 step=333333"));
    uint8_t audio[] = {9,    4,    6,  0,    0, 1, 1,  0, 0,    9,    0x24, 1,    0,    1, 30, 0,
                       1,    7,    12, 0x24, 2, 1, 1,  2, 0,    1,    0,    0,    0,    0, 9,  0x24,
                       3,    2,    1,  1,    0, 1, 0,  9, 4,    7,    0,    0,    1,    2, 0,  0,
                       9,    4,    7,  1,    1, 1, 2,  0, 0,    7,    0x24, 1,    2,    1, 1,  0,
                       14,   0x24, 2,  1,    1, 2, 16, 2, 0x80, 0x3e, 0,    0x80, 0xbb, 0, 9,  5,
                       0x84, 5,    98, 0,    1, 0, 0,  7, 0x25, 1,    1,    0,    0,    0};
    size_t start = append(&f, audio, sizeof(audio));
    assert(inspect(&f, &r, &inventory));
    assert(inventory.audio_formats == 1 && !inventory.truncated);
    assert(strstr(r.text, "control if=6 version=0100 total=30 streaming_interfaces=1"));
    assert(strstr(r.text, "stream if=7 alt=1 terminal=2 delay=1 format_tag=0001"));
    assert(strstr(r.text, "channels=1 sample_bytes=2 bits=16 rates=2"));
    assert(strstr(r.text, "rate_hz index=0 value=16000"));
    assert(strstr(r.text, "rate_hz index=1 value=48000"));
    assert(strstr(r.text, "PCM16 candidate if=7 alt=1 ep=84 mic=1 channels=1 rate=16000"));
    assert(strstr(r.text, "frequency_control=1"));
    /* Continuous rates and fixed-rate endpoints are inventory facts, not rejection. */
    size_t format = start + 9 + 9 + 12 + 9 + 9 + 9 + 7;
    f.data[format + 7] = 0;
    f.data[f.length - 4] = 0;
    assert(inspect(&f, &r, &inventory));
    assert(strstr(r.text, "continuous_hz min=16000 max=48000"));
    assert(strstr(r.text, "frequency_control=0"));
    /* Later protocols remain raw numeric descriptors, never UAC1 fields. */
    Fixture other = f;
    other.data[start + 9 + 9 + 12 + 9 + 9 + 7] = 0x20;
    assert(inspect(&other, &r, &inventory));
    assert(!inventory.audio_formats && !strstr(r.text, "UAC1 TypeI"));
    assert(strstr(r.text, "protocol=20"));
    for (size_t n = 0; n < f.length; ++n) {
        uint8_t *short_data = malloc(n ? n : 1);
        assert(short_data);
        memcpy(short_data, f.data, n);
        assert(!camera_inventory(short_data, n, NULL, NULL, &inventory));
        free(short_data);
    }
    other = f;
    other.data[format + 7] = 255;
    assert(!inspect(&other, &r, &inventory) && inventory.malformed);
    other = f;
    other.data[start] = 2;
    assert(!inspect(&other, &r, &inventory));
    other = f;
    other.data[start] = 0;
    assert(!inspect(&other, &r, &inventory));
    assert(!camera_inventory(NULL, 10, NULL, NULL, &inventory));
    assert(!camera_inventory(f.data, f.length, NULL, NULL, NULL));
    /* A long valid descriptor chain must stop log growth but finish counting. */
    other = f;
    uint8_t endpoint[] = {7, 5, 0x84, 5, 98, 0, 1};
    for (unsigned n = 0; n < 200; ++n)
        append(&other, endpoint, sizeof(endpoint));
    assert(inspect(&other, &r, &inventory));
    assert(inventory.truncated && inventory.lines == CAMERA_INVENTORY_LINES);
    assert(strstr(r.text, "truncated=1"));
    uint32_t random = 0x55414331;
    for (unsigned round = 0; round < 100000; ++round) {
        other = f;
        for (unsigned n = 0; n < 4; ++n) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            other.data[random % other.length] ^= (uint8_t)(random >> 24);
        }
        camera_inventory(other.data, other.length, NULL, NULL, &inventory);
        assert(inventory.lines <= CAMERA_INVENTORY_LINES);
    }
    puts(
        "PASS: bounded MJPEG/UAC1 inventory, fixed/discrete/continuous rates, raw topology, truncation and 100000 mutations");
}
