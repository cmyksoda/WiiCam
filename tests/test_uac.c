/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "uac.h"
#include "uvc_fixture.h"
#include <stdio.h>
#include <stdlib.h>

enum {
    AC = 9,
    HEADER = 18,
    INPUT = 27,
    FEATURE = 39,
    OUTPUT = 48,
    IDLE = 57,
    ALT = 66,
    GENERAL = 75,
    FORMAT = 82,
    ENDPOINT = 96,
    CONTROLS = 105
};

static Fixture audio(void)
{
    Fixture f = {0};
    const uint8_t data[] = {
        9,    2, 0,    0,    2,  1,  0,  0x80, 100, 9,    4, 6,    0,  0, 1,    1,    0, 0,    9,
        0x24, 1, 0,    1,    39, 0,  1,  7,    12,  0x24, 2, 1,    1,  2, 0,    1,    0, 0,    0,
        0,    9, 0x24, 6,    2,  1,  1,  0,    0,   0,    9, 0x24, 3,  3, 1,    1,    0, 2,    0,
        9,    4, 7,    0,    0,  1,  2,  0,    0,   9,    4, 7,    1,  1, 1,    2,    0, 0,    7,
        0x24, 1, 3,    1,    1,  0,  14, 0x24, 2,   1,    1, 2,    16, 2, 0x80, 0x3e, 0, 0x80, 0xbb,
        0,    9, 5,    0x84, 5,  98, 0,  1,    0,   0,    7, 0x25, 1,  1, 0,    0,    0};
    append(&f, data, sizeof(data));
    return f;
}

static UacResult parse(const Fixture *f, UacMode *mode)
{
    memset(mode, 0xcc, sizeof(*mode));
    Fixture before = *f;
    UacResult result = uac_parse(f->data, f->length, 7, 1, mode);
    assert(!memcmp(&before, f, sizeof(before)));
    if (result != UAC_OK) {
        UacMode empty = {0};
        assert(!memcmp(mode, &empty, sizeof(empty)));
    }
    return result;
}

int main(void)
{
    Fixture f = audio(), bad;
    UacMode mode;
    assert(parse(&f, &mode) == UAC_OK);
    assert(mode.control_interface == 6 && mode.interface == 7 && mode.alternate == 1);
    assert(mode.endpoint == 0x84 && mode.microphone == 1 && mode.terminal == 3);
    assert(mode.rate == 16000 && mode.channels == 1 && mode.packet_capacity == 98);
    assert(mode.frequency_control && mode.interval == 1);
    assert(uac_rate_supported(&mode, 16000) && uac_rate_supported(&mode, 48000));
    assert(!uac_rate_supported(&mode, 44100));
    for (unsigned fault = 0; fault < 24; ++fault) {
        bad = f;
        switch (fault) {
        case 0:
            bad.data[HEADER + 3] = 0x10;
            break;
        case 1:
            bad.data[HEADER + 8] = 8;
            break;
        case 2:
            bad.data[HEADER + 5] = 255;
            break;
        case 3:
            bad.data[HEADER + 5] = 30;
            break;
        case 4:
            bad.data[INPUT + 3] = 2;
            break;
        case 5:
            bad.data[INPUT + 5] = 6;
            break;
        case 6:
            bad.data[FEATURE + 4] = 2;
            break;
        case 7:
            bad.data[FEATURE + 5] = 0;
            break;
        case 8:
            bad.data[OUTPUT + 7] = 99;
            break;
        case 9:
            bad.data[OUTPUT + 5] = 2;
            break;
        case 10:
            bad.data[GENERAL + 3] = 1;
            break;
        case 11:
            bad.data[GENERAL + 5] = 3;
            break;
        case 12:
            bad.data[FORMAT + 3] = 3;
            break;
        case 13:
            bad.data[FORMAT + 4] = 3;
            break;
        case 14:
            bad.data[FORMAT + 6] = 24;
            break;
        case 15:
            bad.data[FORMAT + 7] = 255;
            break;
        case 16:
            bad.data[FORMAT + 8] = bad.data[FORMAT + 9] = bad.data[FORMAT + 10] = 0;
            break;
        case 17:
            bad.data[ENDPOINT + 2] = 4;
            break;
        case 18:
            bad.data[ENDPOINT + 3] = 2;
            break;
        case 19:
            bad.data[ENDPOINT + 5] = 0x18;
            break;
        case 20:
            bad.data[ENDPOINT + 6] = 0;
            break;
        case 21:
            bad.data[ENDPOINT + 8] = 0x05;
            break;
        case 22:
            bad.data[IDLE + 4] = 1;
            break;
        case 23:
            bad.data[ALT + 7] = 0x20;
            break;
        }
        assert(parse(&bad, &mode) != UAC_OK);
    }
    /* Fixed-rate microphones need no unsupported SET_CUR request. */
    bad = f;
    bad.data[FORMAT] = 11;
    bad.data[FORMAT + 7] = 1;
    memmove(bad.data + FORMAT + 11, bad.data + FORMAT + 14, bad.length - FORMAT - 14);
    bad.length -= 3;
    bad.data[2] = bad.length;
    bad.data[CONTROLS] = 0;
    assert(parse(&bad, &mode) == UAC_OK && !mode.frequency_control && mode.rate == 16000);
    bad = f;
    bad.data[CONTROLS + 3] = 0;
    assert(parse(&bad, &mode) == UAC_NO_MODE);
    bad = f;
    bad.data[FORMAT + 7] = 0;
    assert(parse(&bad, &mode) == UAC_OK && mode.minimum_rate == 16000
           && mode.maximum_rate == 48000);
    assert(uac_rate_supported(&mode, 44100));
    bad.data[CONTROLS + 3] = 0;
    assert(parse(&bad, &mode) == UAC_NO_MODE);
    /* Stereo uses two-channel topology and a master plus two feature controls. */
    bad = f;
    memmove(bad.data + FEATURE + 10, bad.data + FEATURE + 9, bad.length - FEATURE - 9);
    ++bad.length;
    bad.data[2] = bad.length;
    ++bad.data[HEADER + 5];
    bad.data[FEATURE] = 10;
    bad.data[FEATURE + 9] = 0;
    bad.data[INPUT + 7] = 2;
    bad.data[FORMAT + 1 + 4] = 2;
    assert(parse(&bad, &mode) == UAC_OK && mode.channels == 2);
    bad.data[ENDPOINT + 1 + 4] = 2;
    assert(parse(&bad, &mode) == UAC_NO_MODE);
    bad = f;
    for (unsigned i = 2; i <= UAC_MAX_ALTERNATES; ++i) {
        uint8_t alt[] = {9, 4, 7, i, 0, 1, 2, 0, 0};
        append(&bad, alt, sizeof(alt));
    }
    assert(parse(&bad, &mode) == UAC_LIMIT);
    bad = f;
    for (unsigned i = 0; i < 14; ++i) {
        size_t at = HEADER + uvc_le16(bad.data + HEADER + 5);
        memmove(bad.data + at + 4, bad.data + at, bad.length - at);
        uint8_t unit[] = {4, 0x24, 0x7f, 10 + i};
        memcpy(bad.data + at, unit, sizeof(unit));
        bad.length += 4;
        bad.data[2] = bad.length;
        bad.data[3] = bad.length >> 8;
        bad.data[HEADER + 5] += 4;
    }
    assert(parse(&bad, &mode) == UAC_LIMIT);
    bad = f;
    size_t extra = 33 * 3 + 8 - 14;
    memmove(bad.data + FORMAT + 14 + extra, bad.data + FORMAT + 14, bad.length - FORMAT - 14);
    bad.length += extra;
    bad.data[2] = bad.length;
    bad.data[3] = bad.length >> 8;
    bad.data[FORMAT] = 107;
    bad.data[FORMAT + 7] = 33;
    assert(parse(&bad, &mode) == UAC_LIMIT);
    for (size_t n = 0; n < f.length; ++n) {
        uint8_t *data = malloc(n ? n : 1);
        assert(data);
        memcpy(data, f.data, n);
        if (n >= 9) {
            data[2] = n;
            data[3] = n >> 8;
        }
        assert(uac_parse(data, n, 7, 1, &mode) != UAC_OK);
        free(data);
    }
    bad = f;
    append(&bad, f.data + ALT, 9);
    assert(parse(&bad, &mode) == UAC_ASSOCIATION);
    bad = f;
    append(&bad, f.data + AC, 9 + 39);
    assert(parse(&bad, &mode) == UAC_ASSOCIATION);
    assert(!uac_rate_supported(NULL, 16000));
    assert(uac_parse(NULL, 0, 7, 1, &mode) == UAC_MALFORMED);
    assert(uac_parse(f.data, f.length, 7, 1, NULL) == UAC_MALFORMED);
    assert(uac_parse(f.data, f.length, 256, 1, &mode) == UAC_MALFORMED);
    uint32_t random = 0x50434d31;
    for (unsigned n = 0; n < 100000; ++n) {
        bad = f;
        for (unsigned i = 0; i < 4; ++i) {
            random ^= random << 13;
            random ^= random >> 17;
            random ^= random << 5;
            bad.data[random % bad.length] ^= (uint8_t)(random >> 24);
        }
        if (parse(&bad, &mode) == UAC_OK) {
            assert(mode.interface == 7 && mode.alternate == 1 && mode.microphone);
            assert(mode.channels >= 1 && mode.channels <= 2 && mode.rate_count <= UAC_MAX_RATES);
            assert(mode.packet_capacity >= mode.channels * 2u && mode.packet_capacity <= 1024);
            assert(uac_rate_supported(&mode, mode.rate));
        }
    }
    puts(
        "PASS: UAC1 microphone topology, PCM16, fixed/discrete/continuous rates, endpoint bounds and 100000 mutations");
}
