/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "uvc_fixture.h"
#include <stdio.h>

static Fixture discrete(unsigned count)
{
    assert(count >= 2 && count <= 33);
    Fixture f = fixture(5, 3, 0x83, false);
    size_t extra = 4 * (count - 2), end = f.frame + f.data[f.frame];
    memmove(f.data + end + extra, f.data + end, f.length - end);
    f.length += extra;
    f.alt += extra;
    f.endpoint += extra;
    f.data[2] = f.length;
    f.data[3] = f.length >> 8;
    f.data[f.frame] += extra;
    f.data[f.frame + 25] = count;
    unsigned total = uvc_le16(f.data + f.header + 4) + extra;
    f.data[f.header + 4] = total;
    f.data[f.header + 5] = total >> 8;
    for (unsigned i = 0; i < count; ++i)
        uvc_put32(f.data + f.frame + 26 + 4 * i, 333333 + i * 100000);
    return f;
}

static void limited(Fixture *f, const char *reason, uint32_t value, uint32_t bound)
{
    UvcMode mode;
    unsigned width = uvc_le16(f->data + f->frame + 5);
    unsigned height = uvc_le16(f->data + f->frame + 7);
    assert(uvc_parse(f->data, f->length, 5, width, height, &mode) == UVC_LIMIT);
    assert(mode.limit_reason && strcmp(mode.limit_reason, reason) == 0);
    assert(mode.limit_value == value && mode.limit_bound == bound);
    assert(mode.width == width && mode.height == height);
    assert(mode.max_frame == uvc_le32(f->data + f->frame + 17));
    assert(mode.interval == uvc_le32(f->data + f->frame + 21));
}

static void test_limits(void)
{
    Fixture f = discrete(33);
    limited(&f, "too many discrete intervals", 33, 32);
    f = discrete(32);
    UvcMode mode;
    assert(uvc_parse(f.data, f.length, 5, 320, 240, &mode) == UVC_OK);
    assert(!mode.limit_reason && !mode.limit_value && !mode.limit_bound);
    assert(mode.intervals[31] == 3433333);
    uvc_put32(f.data + f.frame + 26 + 4, 0);
    limited(&f, "zero discrete interval at index", 1, 32);
    assert(uvc_parse(f.data, f.length, 5, 320, 240, &mode) == UVC_LIMIT);
    assert(mode.intervals[31] == 3433333); /* full bounded list retained */
    f = fixture(5, 3, 0x83, true);
    uvc_put32(f.data + f.frame + 26, 0);
    limited(&f, "zero continuous minimum", 0, 9999990);
    f = fixture(5, 3, 0x83, true);
    uvc_put32(f.data + f.frame + 30, 1);
    limited(&f, "continuous minimum exceeds maximum", 333333, 1);
    f = fixture(5, 3, 0x83, true);
    uvc_put32(f.data + f.frame + 34, 0);
    limited(&f, "zero continuous step", 0, 1);
    f = fixture(5, 3, 0x83, false);
    uvc_put32(f.data + f.frame + 17, 0);
    limited(&f, "zero frame buffer size", 0, WC04_FRAME_CAPACITY);
    /* Synthetic descriptor carrying the physically observed frame limit. */
    f.data[f.frame + 5] = 0x80;
    f.data[f.frame + 6] = 2;
    f.data[f.frame + 7] = 0xe0;
    f.data[f.frame + 8] = 1;
    uvc_put32(f.data + f.frame + 17, 614989);
    assert(uvc_parse(f.data, f.length, 5, 640, 480, &mode) == UVC_OK);
    assert(mode.max_frame == 614989);
    uvc_put32(f.data + f.frame + 17, WC04_FRAME_CAPACITY + 1);
    limited(&f, "frame buffer exceeds capacity", WC04_FRAME_CAPACITY + 1, WC04_FRAME_CAPACITY);
    uvc_put32(f.data + f.frame + 17, WC04_FRAME_CAPACITY);
    assert(uvc_parse(f.data, f.length, 5, 640, 480, &mode) == UVC_OK);
    uvc_put32(f.data + f.frame + 21, 123456);
    limited(&f, "default interval not advertised", 123456, 2);
    f = fixture(5, 3, 0x83, false);
    for (unsigned n = 2; n <= 17; ++n) {
        uint8_t alternate[] = {9, 4, 5, n, 1, 14, 2, 0, 0, 7, 5, 0x83, 5, 0, 4, 1};
        append(&f, alternate, sizeof(alternate));
    }
    limited(&f, "too many matching alternates", 17, 16);
    f = fixture(5, 3, 0x83, false);
    assert(uvc_parse(f.data, f.length, 5, 320, 240, &mode) == UVC_OK);
    assert(!mode.limit_reason); /* no stale diagnostics after retry */
}

static void test_boundaries(void)
{
    struct {
        uint8_t before, bytes[128], after;
    } memory = {.before = 0x5a, .after = 0xa5};

    UvcFrame frame;
    /* Nonzero padding remains rejected even with a nearby raw EOI pair. */
    for (unsigned padding = 0; padding <= 7; ++padding) {
        uint8_t packet[20] = {2, 0x82, 0xff, 0xd8, 0x11, 0x22, 0xff, 0xd9};
        for (unsigned i = 0; i < padding; ++i)
            packet[8 + i] = 0xa5;
        uvc_frame_init(&frame, memory.bytes, sizeof(memory.bytes));
        assert(uvc_packet(&frame, packet, 8 + padding) == (padding == 0));
        assert(frame.stats.invalid_jpeg == (padding != 0));
        assert(frame.stats.complete == (padding == 0));
    }
    uvc_frame_init(&frame, memory.bytes, sizeof(memory.bytes));
    uint8_t bad[] = {2, 0x82, 0, 1, 2, 3};
    for (unsigned i = 0; i < 1000; ++i) {
        bad[1] = 0x82 | (i & 1);
        assert(!uvc_packet(&frame, bad, sizeof(bad)));
    }
    assert(frame.stats.invalid_jpeg == 1000 && frame.dropped == 1000);

    /* Zero/one-byte frames and bad-start-only are rejected without overreads. */
    uvc_frame_init(&frame, memory.bytes, sizeof(memory.bytes));
    uint8_t tiny[] = {2, 0x82, 0xff};
    assert(!uvc_packet(&frame, tiny, 2));
    assert(frame.stats.empty_frames == 1 && !frame.stats.invalid_jpeg);
    tiny[1] ^= 1;
    assert(!uvc_packet(&frame, tiny, sizeof(tiny)));
    assert(frame.stats.invalid_jpeg == 1);
    uint8_t wrong_start[] = {2, 0x82, 1, 2, 0xff, 0xd9};
    assert(!uvc_packet(&frame, wrong_start, sizeof(wrong_start)));
    assert(frame.stats.invalid_jpeg == 2);

    /* Markers split across packets still assemble; no EOF still does not pass. */
    uvc_frame_init(&frame, memory.bytes, sizeof(memory.bytes));
    uint8_t start[] = {2, 0x80, 0xff}, middle[] = {2, 0x80, 0xd8, 1, 0xff}, end[] = {2, 0x82, 0xd9};
    assert(!uvc_packet(&frame, start, sizeof(start)));
    assert(!uvc_packet(&frame, middle, sizeof(middle)));
    assert(uvc_packet(&frame, end, sizeof(end)));
    assert(frame.stats.complete == 1 && !frame.stats.invalid_jpeg);

    /* A long zero tail is rejected, and an oversized frame never writes past the buffer. */
    uvc_frame_init(&frame, memory.bytes, sizeof(memory.bytes));
    uint8_t large[82] = {2, 0x82, 0xff, 0xd8, 0xff, 0xd9};
    assert(!uvc_packet(&frame, large, sizeof(large)));
    assert(frame.stats.invalid_jpeg == 1);
    uvc_frame_init(&frame, memory.bytes, 3);
    assert(!uvc_packet(&frame, large, sizeof(large)));
    assert(frame.stats.overflow == 1 && !frame.stats.invalid_jpeg);
    assert(memory.before == 0x5a && memory.after == 0xa5);
    uvc_frame_init(&frame, memory.bytes, sizeof(memory.bytes));
    assert(!frame.stats.invalid_jpeg && !frame.dropped);
}

int main(void)
{
    test_limits();
    test_boundaries();
    puts(
        "PASS: exact descriptor limits, strict nonzero-tail rejection, padding/split/truncated markers, short/overflow bounds and reset");
    return 0;
}
