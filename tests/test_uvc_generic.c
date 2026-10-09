/* SPDX-License-Identifier: GPL-3.0-only */
#include "uvc_fixture.h"
#include <stdio.h>

static UvcParseResult parse(const Fixture *f, UvcMode *mode)
{
    return uvc_parse(f->data, f->length, 5, 320, 240, mode);
}

int main(void)
{
    Fixture f = fixture(5, 3, 0x83, false), bad;
    UvcMode mode;
    assert(parse(&f, &mode) == UVC_OK);
    assert(mode.interface == 5 && mode.control_interface == 3 && mode.endpoint == 0x83);
    assert(mode.format == 2 && mode.frame == 7 && mode.alt_count == 1);
    assert(uvc_interval_supported(&mode, 333333) && uvc_interval_supported(&mode, 666666));
    assert(!uvc_interval_supported(&mode, 500000));
    assert(uvc_parse(f.data, f.length, 1, 320, 240, &mode) == UVC_ASSOCIATION);
    for (size_t n = 0; n < f.length; ++n)
        assert(uvc_parse(f.data, n, 5, 320, 240, &mode) != UVC_OK);
    for (unsigned version = 0x110; version <= 0x150; version += 0x40) {
        bad = f; bad.data[bad.vc + 3] = version & 255;
        assert(parse(&bad, &mode) == UVC_VERSION);
    }
    /* An unrelated newer video function must not replace the target VC version. */
    Fixture other = fixture(1, 0, 0x81, false);
    other.data[other.vc + 3] = 0x50;
    append(&f, other.data + 9, other.length - 9);
    assert(parse(&f, &mode) == UVC_OK && mode.version == 0x100);
    f.data[f.vc + 3] = 0x10;
    other = fixture(1, 0, 0x81, false);
    append(&f, other.data + 9, other.length - 9);
    assert(parse(&f, &mode) == UVC_VERSION);
    f = fixture(5, 3, 0x83, false);
    other = fixture(5, 0, 0x81, false);
    bad = f; append(&bad, other.data + 9, other.length - 9);
    assert(parse(&bad, &mode) == UVC_ASSOCIATION);

    for (unsigned fault = 0; fault < 15; ++fault) {
        bad = f;
        switch (fault) {
        case 0: bad.data[bad.vc + 11] = 2; break;
        case 1: bad.data[bad.header + 6] = 3; break;
        case 2: bad.data[bad.header + 6] = 0x80; break;
        case 3: bad.data[bad.header + 12] = 4; break;
        case 4: bad.data[bad.frame + 25] = 3; break;
        case 5: uvc_put32(bad.data + bad.frame + 21, 500000); break;
        case 6: uvc_put32(bad.data + bad.frame + 17, WC04_FRAME_CAPACITY + 1); break;
        case 7: bad.data[bad.endpoint + 5] = 0x1c; break;
        case 8: bad.data[bad.endpoint + 4] = 1; break; /* 1025 bytes/transaction */
        case 9: bad.data[bad.endpoint + 5] = 0x84; break;
        case 10: bad.data[bad.endpoint + 5] = 0; break; /* zero capacity */
        case 11: bad.data[bad.format + 3] = 0; break;
        case 12: bad.data[bad.frame + 3] = 0; break;
        case 13: bad.data[bad.header + 4] = 255; break;
        case 14: bad.data[bad.vc + 5] = 255; break;
        }
        assert(parse(&bad, &mode) != UVC_OK);
    }
    bad = f; bad.data[bad.format + 9] = 1;
    assert(parse(&bad, &mode) == UVC_NO_MODE);
    bad = f; bad.data[bad.format + 2] = 0x99;
    assert(parse(&bad, &mode) == UVC_NO_MODE);
    bad = f; bad.data[bad.endpoint + 2] = 0x84;
    assert(parse(&bad, &mode) == UVC_TRANSPORT);
    bad = f; bad.data[bad.endpoint + 3] = 2;
    assert(parse(&bad, &mode) == UVC_TRANSPORT);
    bad = f; bad.data[bad.endpoint + 6] = 2;
    assert(parse(&bad, &mode) == UVC_TRANSPORT);
    /* A different endpoint must not supply bandwidth for the advertised stream. */
    const uint8_t unrelated[] = {7,5,0x84,5,0,0x14,1};
    append(&f, unrelated, sizeof(unrelated));
    assert(parse(&f, &mode) == UVC_OK && mode.alt_count == 1 && mode.alts[0].capacity == 1024);
    for (unsigned n = 2; n <= 17; ++n) {
        uint8_t alternate[] = {9,4,5,n,1,14,2,0,0,7,5,0x83,5,0,4,1};
        append(&f, alternate, sizeof(alternate));
    }
    assert(parse(&f, &mode) == UVC_LIMIT);
    f = fixture(5, 3, 0x83, true);
    assert(parse(&f, &mode) == UVC_OK && uvc_interval_supported(&mode, 9999990));
    assert(!uvc_interval_supported(&mode, 333334));
    bad = f; uvc_put32(bad.data + bad.frame + 34, 0);
    assert(parse(&bad, &mode) == UVC_LIMIT);
    bad = f; uvc_put32(bad.data + bad.frame + 30, 1);
    assert(parse(&bad, &mode) == UVC_LIMIT);
    for (unsigned n = 0; n <= 64; ++n) {
        assert(uvc_control_payload_length(8 + (int)n, 64) == (int)n);
        if (n < 8) assert(uvc_control_payload_length((int)n, 64) == -1);
    }
    assert(uvc_control_payload_length(-7022, 26) == -7022);
    assert(uvc_control_payload_length(35, 26) == -1);
    uint32_t random = 0x55564331;
    for (unsigned round = 0; round < 50000; ++round) {
        bad = f;
        for (unsigned i = 0; i < 4; ++i) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            bad.data[random % bad.length] ^= (uint8_t)(random >> 24);
        }
        if (parse(&bad, &mode) == UVC_OK) {
            assert(mode.alt_count <= WC04_MAX_ALTS && mode.interval_count <= UVC_MAX_INTERVALS);
            assert(mode.interface == 5 && mode.version == 0x100);
            for (size_t i = 0; i < mode.alt_count; ++i)
                assert(mode.alts[i].capacity <= 3072 && mode.alts[i].endpoint == mode.endpoint);
        }
    }
    puts("PASS: 50000 synthetic mutations; synthetic generic interfaces/functions/endpoints, version/interval/packet bounds, unsupported formats/transports");
    return 0;
}
