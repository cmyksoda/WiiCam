/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 cmyksoda
 */

#include "uvc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static const uint8_t fixture[] = {
    9, 2, 119, 0, 2, 1, 0, 0x80, 250,
    9, 4, 0, 0, 0, 14, 1, 0, 0,
    13, 0x24, 1, 0, 1, 13, 0, 0xc0, 0xe1, 0xe4, 0, 1, 1,
    9, 4, 1, 0, 0, 14, 2, 0, 0,
    14, 0x24, 1, 1, 63, 0, 0x81, 0, 1, 0, 0, 0, 1, 0,
    11, 0x24, 6, 1, 1, 1, 1, 0, 0, 0, 0,
    38, 0x24, 7, 2, 0, 0x40, 1, 0xf0, 0,
    0, 0, 0, 0, 0, 0, 0, 0,
    0, 0xc2, 1, 0, 0x15, 0x16, 5, 0, 0,
    0x15, 0x16, 5, 0, 0x76, 0x96, 0x98, 0, 0x15, 0x16, 5, 0,
    9, 4, 1, 1, 1, 14, 2, 0, 0,
    7, 5, 0x81, 5, 0x60, 0x0a, 1
};

static void test_descriptors(void)
{
    UvcMode mode;
    UvcAlternate alt = {0};
    assert(sizeof(fixture) == 119);
    assert(uvc_parse(fixture, sizeof(fixture), 1, 320, 240, &mode) == UVC_OK);
    assert(mode.configuration == 1 && mode.interface == 1);
    assert(mode.format == 1 && mode.frame == 2 && mode.endpoint == 0x81);
    assert(mode.version == 0x100 && mode.max_frame == 115200);
    assert(mode.interval == 333333 && mode.alts[0].capacity == 1216);
    assert(uvc_alternate(&mode, 1216, &alt) && alt.number == 1);
    assert(!uvc_alternate(&mode, 1217, &alt));
    assert(!uvc_alternate(&mode, 0, &alt));
    uint8_t probe[UVC_PROBE_LENGTH] = {0};
    uvc_probe_select(probe, &mode);
    assert(probe[0] == 1 && probe[2] == 1 && probe[3] == 2);
    assert(uvc_le32(probe + 4) == 333333);
    assert(uvc_le32(probe + 22) == 0);
    assert(uvc_parse(fixture, sizeof(fixture), 1, 640, 480, &mode) != UVC_OK);
    for (size_t length = 0; length < sizeof(fixture); ++length)
        assert(uvc_parse(fixture, length, 1, 320, 240, &mode) != UVC_OK);
    uint8_t damaged[sizeof(fixture)];
    memcpy(damaged, fixture, sizeof(fixture));
    damaged[9] = 0;
    assert(uvc_parse(damaged, sizeof(damaged), 1, 320, 240, &mode) != UVC_OK);
    memcpy(damaged, fixture, sizeof(fixture));
    damaged[117] = 0x1a;
    assert(uvc_parse(damaged, sizeof(damaged), 1, 320, 240, &mode) != UVC_OK);
    memcpy(damaged, fixture, sizeof(fixture));
    damaged[117] = 0x13;
    damaged[116] = 0xfc;
    assert(uvc_parse(damaged, sizeof(damaged), 1, 320, 240, &mode) == UVC_OK);
    assert(mode.alts[0].capacity == 3060);
    mode.alt_count = 3;
    mode.alts[0] = (UvcAlternate){6, 0x81, 3060};
    mode.alts[1] = (UvcAlternate){1, 0x81, 1216};
    mode.alts[2] = (UvcAlternate){2, 0x81, 1536};
    assert(uvc_alternate(&mode, 1220, &alt) && alt.number == 2);
}

static void test_frames(void)
{
    uint8_t buffer[8];
    UvcFrame frame;
    const uint8_t start[] = {2, 0x80, 0xff, 0xd8, 1};
    const uint8_t end[] = {2, 0x82, 2, 0xff, 0xd9};
    const uint8_t complete[] = {2, 0x83, 0xff, 0xd8, 0xff, 0xd9};
    uvc_frame_init(&frame, buffer, sizeof(buffer));
    assert(!uvc_packet(&frame, start, sizeof(start)));
    assert(uvc_packet(&frame, end, sizeof(end)));
    assert(frame.size == 6 && buffer[2] == 1 && buffer[3] == 2);
    assert(!uvc_packet(&frame, end, sizeof(end)));
    assert(uvc_packet(&frame, complete, sizeof(complete)));

    uvc_frame_init(&frame, buffer, sizeof(buffer));
    assert(!uvc_packet(&frame, end, sizeof(end)));
    assert(uvc_packet(&frame, complete, sizeof(complete)));
    assert(frame.dropped == 1);

    uvc_frame_init(&frame, buffer, sizeof(buffer));
    assert(!uvc_packet(&frame, start, sizeof(start)));
    const uint8_t error[] = {2, 0xc0, 1};
    assert(!uvc_packet(&frame, error, sizeof(error)));
    assert(!uvc_packet(&frame, end, sizeof(end)));
    assert(uvc_packet(&frame, complete, sizeof(complete)));

    uvc_frame_init(&frame, buffer, 4);
    assert(!uvc_packet(&frame, start, sizeof(start)));
    assert(!uvc_packet(&frame, end, sizeof(end)));
    assert(uvc_packet(&frame, complete, sizeof(complete)));

    uvc_frame_init(&frame, buffer, sizeof(buffer));
    assert(!uvc_packet(&frame, start, sizeof(start)));
    assert(uvc_packet(&frame, complete, sizeof(complete)));
    assert(frame.dropped == 1);

    const uint8_t bad_pts[] = {2, 0x86, 0xff, 0xd8, 0xff, 0xd9};
    uvc_frame_init(&frame, buffer, sizeof(buffer));
    assert(!uvc_packet(&frame, bad_pts, sizeof(bad_pts)));
    assert(uvc_packet(&frame, complete, sizeof(complete)));
    const uint8_t timestamps[] = {
        12, 0x8e, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xd8, 0xff, 0xd9
    };
    assert(uvc_packet(&frame, timestamps, sizeof(timestamps)));
    const uint8_t invalid[] = {20, 0};
    assert(!uvc_packet(&frame, invalid, sizeof(invalid)));
    assert(!uvc_packet(&frame, invalid, 1));
    assert(!uvc_packet(&frame, NULL, 0));
}

static void test_live_descriptors(void)
{
    uint8_t data[777];
    FILE *file = fopen("tests/fixtures/wc04-configuration.bin", "rb");
    assert(file);
    assert(fread(data, 1, sizeof(data), file) == sizeof(data));
    assert(fgetc(file) == EOF);
    assert(fclose(file) == 0);
    const unsigned capacities[] = {1216, 1536, 1600, 2304, 2400, 3060};
    const unsigned widths[] = {320, 640};
    for (unsigned i = 0; i < 2; ++i) {
        UvcMode mode;
        UvcAlternate alt;
        assert(uvc_parse(data, sizeof(data), 1, widths[i], widths[i] * 3 / 4, &mode) == UVC_OK);
        assert(mode.configuration == 1 && mode.interface == 1 && mode.format == 1);
        assert(mode.frame == (i == 0 ? 2 : 1));
        assert(mode.interval == 333333 && mode.alt_count == 6);
        for (unsigned j = 0; j < 6; ++j) {
            assert(mode.alts[j].number == j + 1 && mode.alts[j].endpoint == 0x81);
            assert(mode.alts[j].capacity == capacities[j]);
        }
        assert(uvc_alternate(&mode, 1216, &alt) && alt.number == 1);
    }
}

static void test_diagnostics(void)
{
    uint8_t buffer[16];
    UvcFrame frame;
    uvc_frame_init(&frame, buffer, sizeof(buffer));
    const uint8_t header[] = {12, 0x8c, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const uint8_t error[] = {12, 0xce, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    assert(!uvc_packet(&frame, NULL, 0));
    for (unsigned i = 0; i < 16; ++i)
        assert(!uvc_packet(&frame, header, sizeof(header)));
    assert(!uvc_packet(&frame, error, sizeof(error)));
    assert(frame.stats.packets == 18 && frame.stats.bytes == 204);
    assert(frame.stats.empty == 1 && frame.stats.header_only == 17);
    assert(frame.stats.payload_packets == 0 && frame.stats.payload_bytes == 0);
    assert(frame.stats.errors == 1 && frame.stats.eof == 1 && frame.dropped == 1);
    const uint8_t complete[] = {2, 0x83, 0xff, 0xd8, 0xff, 0xd9};
    assert(uvc_packet(&frame, complete, sizeof(complete)));
    assert(frame.stats.fid_changes == 1 && frame.stats.complete == 1);
    assert(frame.stats.payload_packets == 1 && frame.stats.payload_bytes == 4);

    uvc_frame_init(&frame, buffer, sizeof(buffer));
    const uint8_t start[] = {2, 0x80, 0xff, 0xd8};
    assert(!uvc_packet(&frame, start, sizeof(start)));
    assert(uvc_packet(&frame, complete, sizeof(complete)));
    assert(frame.stats.missing_eof == 1 && frame.dropped == 1);
    const uint8_t empty_end[] = {2, 0x82};
    assert(!uvc_packet(&frame, empty_end, sizeof(empty_end)));
    assert(frame.stats.invalid_jpeg == 0 && frame.stats.empty_frames == 1);
    const uint8_t bad_timing[] = {2, 0x8f};
    assert(!uvc_packet(&frame, bad_timing, sizeof(bad_timing)));
    assert(frame.stats.bad_headers == 1);
    uvc_frame_init(&frame, buffer, 3);
    assert(!uvc_packet(&frame, complete, sizeof(complete)));
    assert(frame.stats.overflow == 1 && frame.stats.complete == 0);

    /* Recorded headers from 0.1.1 hardware samples: ordinary, EOF, ordinary. */
    const uint8_t hardware_headers[][12] = {
        {0x0c, 0x8c, 0xe0, 0xa6, 0xf6, 0x5b, 0xad, 0xee, 0x31, 0x5c, 0x29, 0x03},
        {0x0c, 0x8f, 0x40, 0xf6, 0x32, 0x5c, 0x59, 0xf6, 0x32, 0x5c, 0x2e, 0x03},
        {0x0c, 0x8c, 0x40, 0xf6, 0x32, 0x5c, 0xac, 0xfd, 0x32, 0x5c, 0x2e, 0x03}
    };
    uvc_frame_init(&frame, buffer, sizeof(buffer));
    for (unsigned i = 0; i < 3; ++i)
        assert(!uvc_packet(&frame, hardware_headers[i], 12));
    assert(frame.stats.header_only == 3 && frame.stats.payload_bytes == 0);
    assert(frame.stats.bad_headers == 0 && frame.stats.errors == 0);
    assert(frame.stats.empty_frames == 1 && frame.stats.invalid_jpeg == 0);
    assert(frame.stats.fid_changes == 2 && frame.stats.complete == 0);
}

static uint32_t random_state = 0x57433034;

static uint8_t random_byte(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return (uint8_t)random_state;
}

static void test_mutations(void)
{
    uint8_t data[sizeof(fixture)], output[64];
    UvcMode mode;
    UvcFrame frame;
    uvc_frame_init(&frame, output, sizeof(output));
    for (unsigned round = 0; round < 20000; ++round) {
        memcpy(data, fixture, sizeof(data));
        for (unsigned i = 0; i < 4; ++i) {
            unsigned index = random_byte() % sizeof(data);
            data[index] = random_byte();
        }
        (void)uvc_parse(data, sizeof(data), 1, 320, 240, &mode);
        size_t length = random_byte() % sizeof(data);
        (void)uvc_packet(&frame, data, length);
        assert(frame.size <= frame.capacity);
    }
}

int main(void)
{
    test_descriptors();
    test_frames();
    test_live_descriptors();
    test_diagnostics();
    test_mutations();
    puts("PASS: synthetic/live descriptors, negotiation, UVC framing/diagnostics, 20000 mutations");
    return 0;
}
