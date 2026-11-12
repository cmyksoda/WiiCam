/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "microphone.h"
#include "uvc_fixture.h"
#include <stdio.h>
#include <string.h>

/* Logged UAC descriptors in a synthetic configuration envelope. */
static Fixture microphone(void)
{
    Fixture f = {0};
    const uint8_t bytes[] = {
        9,    2, 0,    0,    2,    1,    0,    0x80, 100,  9,    4,    2,    0,    0,    1,
        1,    0, 0,    9,    0x24, 1,    0,    1,    41,   0,    1,    3,    12,   0x24, 2,
        1,    1, 2,    0,    1,    0,    0,    0,    0,    11,   0x24, 6,    2,    1,    2,
        1,    0, 2,    0,    0,    9,    0x24, 3,    3,    1,    1,    0,    2,    0,    9,
        4,    3, 0,    0,    1,    2,    0,    0,    9,    4,    3,    1,    1,    1,    2,
        0,    0, 7,    0x24, 1,    3,    1,    1,    0,    29,   0x24, 2,    1,    1,    2,
        16,   7, 0x40, 0x1f, 0,    0x11, 0x2b, 0,    0x80, 0x3e, 0,    0x22, 0x56, 0,    0xc0,
        0x5d, 0, 0x44, 0xac, 0,    0x80, 0xbb, 0,    9,    5,    0x84, 5,    192,  0,    4,
        0,    0, 7,    0x25, 1,    1,    0,    0,    0};
    append(&f, bytes, sizeof(bytes));
    return f;
}

typedef struct {
    unsigned calls, fault, endpoint;
} Control;

static int control(void *context, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                   uint8_t *data, unsigned length)
{
    Control *c = context;
    assert(value == 0x100 && index == (c->endpoint ? c->endpoint : 0x84) && length == 3);
    assert(type == (c->calls ? 0xa2 : 0x22) && request == (c->calls ? 0x81 : 1));
    if (!c->calls)
        assert(data[0] == 0x80 && data[1] == 0x3e && data[2] == 0);
    else {
        assert(!data[0] && !data[1] && !data[2]);
        data[0] = c->fault == 5 ? 0x44 : 0x80;
        data[1] = c->fault == 5 ? 0xac : 0x3e;
    }
    unsigned step = c->calls++;
    if (c->fault == 1 && !step) return -4;
    if (c->fault == 2 && !step) return 2;
    if (c->fault == 3 && step) return -4;
    if (c->fault == 4 && step) return 2;
    if (c->fault == 6 && step) return -7102;
    if (c->fault == 7 && !step) return -7102;
    return 3;
}


typedef struct {
    unsigned channels, rate_count;
    uint32_t rates[4];
    unsigned capacity;
} Alt;

/* A one-terminal microphone whose streaming interface offers the given alternates. */
static Fixture built(unsigned channels, const Alt *alts, unsigned count)
{
    Fixture f = {0};
    const uint8_t feature_length = (uint8_t)(7 + 2 * (channels + 1));
    const uint8_t header[] = {9, 2, 0, 0, 2, 1, 0, 0x80, 100, 9, 4, 2, 0, 0, 1, 1, 0, 0};
    append(&f, header, sizeof(header));
    const uint8_t ac[] = {9, 0x24, 1, 0, 1, (uint8_t)(30 + feature_length), 0, 1, 3,
                          12, 0x24, 2, 1, 1, 2, 0, (uint8_t)channels, channels == 2 ? 3 : 0, 0, 0, 0};
    append(&f, ac, sizeof(ac));
    uint8_t feature[16] = {feature_length, 0x24, 6, 2, 1, 2, 1, 0};
    for (unsigned c = 1; c <= channels; ++c) feature[6 + 2 * c] = 2;
    append(&f, feature, feature_length);
    const uint8_t output[] = {9, 0x24, 3, 3, 1, 1, 0, 2, 0, 9, 4, 3, 0, 0, 1, 2, 0, 0};
    append(&f, output, sizeof(output));
    for (unsigned a = 0; a < count; ++a) {
        const uint8_t head[] = {9, 4, 3, (uint8_t)(a + 1), 1, 1, 2, 0, 0, 7, 0x24, 1, 3, 1, 1, 0};
        append(&f, head, sizeof(head));
        uint8_t format[20] = {(uint8_t)(8 + 3 * alts[a].rate_count), 0x24, 2, 1,
                              (uint8_t)alts[a].channels, 2, 16, (uint8_t)alts[a].rate_count};
        for (unsigned r = 0; r < alts[a].rate_count; ++r) {
            format[8 + 3 * r] = alts[a].rates[r] & 255;
            format[9 + 3 * r] = alts[a].rates[r] >> 8 & 255;
            format[10 + 3 * r] = alts[a].rates[r] >> 16 & 255;
        }
        append(&f, format, format[0]);
        const uint8_t endpoint[] = {9, 5, 0x84, 5, (uint8_t)(alts[a].capacity & 255),
                                    (uint8_t)(alts[a].capacity >> 8), 4, 0, 0, 7, 0x25, 1, 1, 0, 0, 0};
        append(&f, endpoint, sizeof(endpoint));
    }
    f.data[2] = f.length & 255;
    f.data[3] = (uint8_t)(f.length >> 8);
    return f;
}

/* A clock that reads back whatever rate was last set. */
static int echo(void *context, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                uint8_t *data, unsigned length)
{
    uint32_t *clock = context;
    assert(value == 0x100 && index == 0x84 && length == 3);
    if (type == 0x22 && request == 1)
        *clock = (uint32_t)data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16;
    else {
        assert(type == 0xa2 && request == 0x81);
        data[0] = *clock & 255;
        data[1] = *clock >> 8 & 255;
        data[2] = *clock >> 16 & 255;
    }
    return 3;
}

static void format_tests(void)
{
    UacMode mode;
    const Alt stereo48 = {2, 1, {48000}, 196};
    Fixture f = built(2, &stereo48, 1);
    assert(microphone_mode(f.data, f.length, &mode));
    assert(mode.channels == 2 && mode.rate == 48000 && mode.alternate == 1);

    const Alt choices[] = {{2, 1, {48000}, 196}, {2, 2, {32000, 16000}, 68}, {2, 1, {32000}, 132}};
    f = built(2, choices, 3);
    assert(microphone_mode(f.data, f.length, &mode));
    assert(mode.alternate == 2 && mode.rate == 16000 && mode.channels == 2);

    const Alt only44 = {2, 1, {44100}, 180};
    f = built(2, &only44, 1);
    assert(!microphone_mode(f.data, f.length, &mode));
    char text[256];
    size_t used = microphone_summary(f.data, f.length, text, sizeof(text));
    assert(used == strlen(text));
    assert(!strcmp(text, "Audio interface 3 alternate 1: format 1, 2 ch, 16-bit, 44100 Hz\n"));
    assert(microphone_summary(f.data, f.length, text, 20) < 20);

    const Alt pair[] = {{1, 1, {16000}, 32}, {1, 1, {16000}, 32}};
    f = built(1, pair, 2);
    assert(microphone_mode(f.data, f.length, &mode) && mode.alternate == 1);

    const Alt mono32 = {1, 1, {32000}, 64};
    f = built(1, &mono32, 1);
    assert(microphone_mode(f.data, f.length, &mode) && mode.rate == 32000);
    CameraEntry entry = {.vid = 0x07ca, .pid = 0x313a, .interface = 3};
    uint8_t data[3];
    uint32_t clock = 0;
    assert(mode.frequency_control
           && microphone_set_rate(&entry, &mode, echo, &clock, data) == MIC_RATE_OK);
    assert(clock == 32000);
    mode.channels = 3;
    assert(microphone_set_rate(&entry, &mode, NULL, NULL, NULL) == MIC_RATE_FAILED);
    mode.channels = 1;
    mode.rate = 44100;
    assert(microphone_set_rate(&entry, &mode, NULL, NULL, NULL) == MIC_RATE_FAILED);
}

int main(void)
{
    format_tests();
    Fixture f = microphone();
    UacMode mode;
    assert(microphone_mode(f.data, f.length, &mode));
    assert(mode.control_interface == 2 && mode.interface == 3 && mode.alternate == 1);
    assert(mode.endpoint == 0x84 && mode.packet_capacity == 192 && mode.interval == 4);
    assert(mode.channels == 1 && mode.rate == 16000 && mode.rate_count == 7);
    assert(mode.microphone == 1 && mode.terminal == 3);
    for (size_t n = 0; n < f.length; ++n)
        assert(!microphone_mode(f.data, n, &mode));
    assert(microphone_mode(f.data, f.length, &mode));
    CameraEntry entry = {.vid = 0x0c45, .pid = 0x6366, .interface = 3};
    uint8_t data[32];
    for (unsigned fault = 0; fault <= 7; ++fault) {
        Control state = {.fault = fault};
        bool set_failed = fault == 1 || fault == 2 || fault == 7;
        assert(microphone_set_rate(&entry, &mode, control, &state, data)
               == (set_failed ? MIC_RATE_FAILED : MIC_RATE_UNREADABLE));
        assert(state.calls == 1);
        CameraEntry other = entry;
        other.pid ^= 1;
        state = (Control){.fault = fault};
        assert(microphone_set_rate(&other, &mode, control, &state, data)
               == (fault == 0 ? MIC_RATE_OK : MIC_RATE_FAILED));
        assert(state.calls == (set_failed ? 1u : 2u));
    }
    for (unsigned n = 0; n < 10; ++n) {
        CameraEntry other = entry;
        UacMode changed = mode;
        if (n == 0) other.vid ^= 1;
        if (n == 1) other.pid ^= 1;
        if (n == 2) other.interface ^= 1;
        if (n == 3) changed.control_interface ^= 1;
        if (n == 4) changed.interface ^= 1;
        if (n == 5) changed.alternate ^= 2;
        if (n == 6) changed.endpoint ^= 1;
        if (n == 7) changed.packet_capacity += 2;
        if (n == 8) changed.interval ^= 1;
        Control state = {.fault = 6, .endpoint = changed.endpoint};
        assert(microphone_set_rate(n == 9 ? NULL : &other, &changed, control, &state, data)
               == MIC_RATE_FAILED);
        assert(state.calls == 2);
    }
    mode.frequency_control = false;
    assert(!microphone_set_rate(&entry, &mode, NULL, NULL, NULL));
    mode.rate_count = 1;
    mode.rates[0] = MIC_RATE;
    assert(microphone_set_rate(&entry, &mode, NULL, NULL, NULL));
    mode.channels = 2;
    assert(microphone_set_rate(&entry, &mode, NULL, NULL, NULL));

    MicEntry entries[3] = {{{(int32_t)0xe71e0042u, 0x0c45, 0x6366, 1}, 0x00420107},
                           {{(int32_t)0xe71c0042u, 0x0c45, 0x6366, 3}, 0x00420302},
                           {{(int32_t)0xe7190043u, 0x1234, 0x5678, 3}, 0x00430302}};
    size_t selected = 999;
    assert(microphone_match(entries, 3, entries[0].camera.id, 1, 3, &selected) && selected == 1);
    MicEntry saved = entries[2];
    entries[2] = entries[1];
    entries[2].camera.id = (int32_t)0xe7190043u;
    entries[2].token = 0x00430302;
    assert(!microphone_match(entries, 3, entries[0].camera.id, 1, 3, &selected));
    entries[2] = entries[0];
    assert(!microphone_match(entries, 3, entries[0].camera.id, 1, 3, &selected));
    entries[2] = saved;
    entries[1].token ^= 0x10000;
    assert(!microphone_match(entries, 3, entries[0].camera.id, 1, 3, &selected));
    entries[1].token ^= 0x10000;
    entries[1].camera.id = -1;
    assert(!microphone_match(entries, 3, entries[0].camera.id, 1, 3, &selected));
    entries[1].camera.id = (int32_t)0xe71c0042u;
    uint8_t parameters[192] = {0};
    parameters[20] = 18;
    parameters[21] = 1;
    parameters[37] = 1;
    parameters[28] = 0x0c;
    parameters[29] = 0x45;
    parameters[30] = 0x63;
    parameters[31] = 0x66;
    parameters[40] = 9;
    parameters[41] = 2;
    parameters[44] = 4;
    parameters[45] = 1;
    parameters[52] = 9;
    parameters[53] = 4;
    parameters[54] = 3;
    parameters[57] = 1;
    parameters[58] = 2;
    assert(microphone_parameters(&entries[1].camera, parameters, 1));
    const unsigned faults[] = {20, 21, 28, 29, 30, 31, 37, 40, 41,
                               45, 52, 53, 54, 55, 56, 57, 58, 59};
    for (unsigned n = 0; n < sizeof(faults) / sizeof(*faults); ++n) {
        parameters[faults[n]] ^= 1;
        assert(!microphone_parameters(&entries[1].camera, parameters, 1));
        parameters[faults[n]] ^= 1;
    }
    puts(
        "PASS: stereo and 32/48 kHz selection, cheapest alternate, refusal summary, physical UAC fields, ambiguous/changed IOS pairing, strict rate controls, exact-profile readback skip and strict readback for other devices");
}
