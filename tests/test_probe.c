/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 cmyksoda
 */

#include "uvc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint8_t request;
    uint16_t selector;
    int result;
    const uint8_t *bytes;
} Step;

typedef struct {
    const Step *steps;
    size_t count, next;
} Camera;

static int transfer(void *context, uint8_t request, uint16_t selector,
                    uint8_t data[UVC_PROBE_LENGTH])
{
    Camera *camera = context;
    assert(camera->next < camera->count);
    const Step *step = &camera->steps[camera->next++];
    assert(request == step->request && selector == step->selector);
    if (request == UVC_SET_CUR) {
        assert(memcmp(data, step->bytes, UVC_PROBE_LENGTH) == 0);
    } else {
        for (unsigned i = 0; i < UVC_PROBE_LENGTH; ++i)
            assert(data[i] == 0);
        /* Failed or short reads must not contaminate the next seed attempt. */
        memset(data, 0xa5, UVC_PROBE_LENGTH);
        if (step->result > 0) {
            unsigned length = step->result < UVC_PROBE_LENGTH ?
                              (unsigned)step->result : UVC_PROBE_LENGTH;
            memcpy(data, step->bytes, length);
        }
    }
    return step->result;
}

static void run(const UvcMode *mode, const Step *steps, size_t count, bool success)
{
    Camera camera = {.steps = steps, .count = count};
    uint8_t data[UVC_PROBE_LENGTH];
    memset(data, 0xcc, sizeof(data));
    UvcAlternate alt = {0};
    assert(uvc_negotiate(mode, &alt, data, transfer, &camera) == success);
    assert(camera.next == count);
    if (success)
        assert(alt.number == 1 && alt.endpoint == 0x81 && alt.capacity == 1216);
}

static void test_mode(unsigned width, unsigned height)
{
    uint8_t descriptors[777];
    FILE *file = fopen("tests/fixtures/wc04-configuration.bin", "rb");
    assert(file);
    assert(fread(descriptors, 1, sizeof(descriptors), file) == sizeof(descriptors));
    fclose(file);
    UvcMode mode;
    assert(uvc_parse(descriptors, sizeof(descriptors), 1, width, height, &mode) == UVC_OK);

    uint8_t seed[UVC_PROBE_LENGTH];
    for (unsigned i = 0; i < sizeof(seed); ++i)
        seed[i] = (uint8_t)(i + 1);
    uint8_t selected[UVC_PROBE_LENGTH];
    memcpy(selected, seed, sizeof(selected));
    selected[0] = 1;
    selected[1] = 0;
    selected[2] = 1;
    selected[3] = width == 320 ? 2 : 1;
    selected[4] = 0x15;
    selected[5] = 0x16;
    selected[6] = 0x05;
    selected[7] = 0;

    /* Recorded GET_CUR layout, with a camera-adjusted compression value. */
    uint8_t negotiated[UVC_PROBE_LENGTH] = {
        1, 0, 1, 2, 0x15, 0x16, 5, 0, 0, 0, 0, 0, 0x34, 0x12, 0, 0,
        0, 0, 0, 0x2c, 1, 0, 0xc0, 4, 0, 0
    };
    negotiated[3] = selected[3];
    uvc_put32(negotiated + 18, width * height);
    Step normal[] = {
        {UVC_GET_MAX, UVC_VS_PROBE, 26, seed},
        {UVC_SET_CUR, UVC_VS_PROBE, 26, selected},
        {UVC_GET_CUR, UVC_VS_PROBE, 26, negotiated},
        {UVC_SET_CUR, UVC_VS_COMMIT, 26, negotiated}
    };
    run(&mode, normal, 4, true);
    /* Headroom is accepted without loosening the descriptor's negotiated bound. */
    UvcMode roomy = mode;
    roomy.max_frame = 614989;
    uvc_put32(negotiated + 18, 614989);
    run(&roomy, normal, 4, true);
    uvc_put32(negotiated + 18, 614990);
    run(&roomy, normal, 3, false);
    roomy.max_frame = WC04_FRAME_CAPACITY;
    uvc_put32(negotiated + 18, WC04_FRAME_CAPACITY);
    run(&roomy, normal, 4, true);
    uvc_put32(negotiated + 18, WC04_FRAME_CAPACITY + 1);
    run(&roomy, normal, 3, false);
    roomy.max_frame = WC04_FRAME_CAPACITY + 1;
    run(&roomy, normal, 3, false);
    uvc_put32(negotiated + 18, width * height);
    /* Newer control versions must fail before touching the 26-byte bank. */
    for (unsigned version = 0x110; version <= 0x150; version += 0x40) {
        UvcMode newer = mode;
        newer.version = version;
        run(&newer, NULL, 0, false);
    }

    /* A different advertised continuous interval is accepted and committed intact. */
    uvc_put32(negotiated + 4, 666666);
    run(&mode, normal, 4, true);
    uvc_put32(negotiated + 4, 333333);

    const Step fallback[] = {
        {UVC_GET_MAX, UVC_VS_PROBE, 2, seed},
        {UVC_GET_DEF, UVC_VS_PROBE, -1, NULL},
        {UVC_GET_CUR, UVC_VS_PROBE, 26, seed},
        normal[1], normal[2], normal[3]
    };
    run(&mode, fallback, 6, true);
    const Step defaults[] = {
        {UVC_GET_MAX, UVC_VS_PROBE, -1, NULL},
        {UVC_GET_DEF, UVC_VS_PROBE, 26, seed},
        normal[1], normal[2], normal[3]
    };
    run(&mode, defaults, 5, true);
    const Step unavailable[] = {
        {UVC_GET_MAX, UVC_VS_PROBE, -1, NULL},
        {UVC_GET_DEF, UVC_VS_PROBE, 2, seed},
        {UVC_GET_CUR, UVC_VS_PROBE, 18, seed}
    };
    run(&mode, unavailable, 3, false);

    const int failures[] = {-1, 0, 18, 25, 27, 34, 48};
    for (unsigned step = 1; step < 4; ++step) {
        for (unsigned i = 0; i < sizeof(failures) / sizeof(failures[0]); ++i) {
            normal[step].result = failures[i];
            run(&mode, normal, step + 1, false);
        }
        normal[step].result = 26;
    }

    for (unsigned fault = 0; fault < 8; ++fault) {
        uint8_t bad[UVC_PROBE_LENGTH];
        memcpy(bad, negotiated, sizeof(bad));
        switch (fault) {
        case 0: bad[2] = 2; break;
        case 1: bad[3] = 5; break;
        case 2: uvc_put32(bad + 4, 0); break;
        case 3: uvc_put32(bad + 18, 0); break;
        case 4: uvc_put32(bad + 18, WC04_FRAME_CAPACITY + 1); break;
        case 5: uvc_put32(bad + 22, 0); break;
        case 6: uvc_put32(bad + 22, 4000); break;
        case 7: uvc_put32(bad + 4, 333334); break;
        }
        normal[2].bytes = bad;
        run(&mode, normal, 3, false);
    }
}

int main(void)
{
    test_mode(320, 240);
    test_mode(640, 480);
    puts("PASS: seeded probe, fallbacks, camera adjustments, short transfers, invalid replies, exact commit");
    return 0;
}
