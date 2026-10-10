/* SPDX-License-Identifier: GPL-3.0-only */
#include "screen.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static const uint8_t bars[][3] = {
    {255,255,255}, {255,255,0}, {0,255,255}, {0,255,0},
    {255,0,255}, {255,0,0}, {0,0,255}, {0,0,0}
};

static uint8_t clamp(int n) { return n < 0 ? 0 : n > 255 ? 255 : (uint8_t)n; }

static void unpack(uint32_t pair, uint8_t rgb[6])
{
    int cb = (int)((pair >> 16) & 255) - 128, cr = (int)(pair & 255) - 128;
    for (unsigned i = 0; i < 2; ++i) {
        int y = (int)((pair >> (i ? 8 : 24)) & 255) - 16;
        rgb[i * 3] = clamp((298 * y + 409 * cr + 128) >> 8);
        rgb[i * 3 + 1] = clamp((298 * y - 100 * cb - 208 * cr + 128) >> 8);
        rgb[i * 3 + 2] = clamp((298 * y + 516 * cb + 128) >> 8);
    }
}

static void render(unsigned width, unsigned height, unsigned stride, bool wide,
                    unsigned image_width, unsigned image_height, ScreenPage page)
{
    size_t words = (size_t)stride * height / 2;
    uint32_t *allocation = malloc((words + 2) * sizeof(uint32_t));
    assert(allocation);
    for (size_t i = 0; i < words + 2; ++i) allocation[i] = 0xdeadbeef;
    uint8_t *rgb = malloc((size_t)image_width * image_height * 3);
    assert(rgb);
    for (unsigned y = 0; y < image_height; ++y)
        for (unsigned x = 0; x < image_width; ++x)
            for (unsigned c = 0; c < 3; ++c)
                rgb[((size_t)y * image_width + x) * 3 + c] = bars[x * 8 / image_width][c];
    Screen screen = {allocation + 1, width, height, stride, wide};
    screen_draw(&screen, page, (page == SCREEN_MENU || page == SCREEN_NO_SIGNAL) ? NULL : rgb,
                 image_width, image_height, &(ScreenInfo){.status = "", .sd = true});
    assert(allocation[0] == 0xdeadbeef && allocation[words + 1] == 0xdeadbeef);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = width; x < stride; x += 2)
            assert(screen.pixels[((size_t)y * stride + x) / 2] == 0xdeadbeef);
    if (page != SCREEN_MENU && page != SCREEN_NO_SIGNAL) {
        unsigned view_width = wide ? 270 : 360, left = (640 - view_width) / 2;
        for (unsigned bar = 0; bar < 8; ++bar) {
            unsigned x = (left + (2 * bar + 1) * view_width / 16) * width / 640;
            uint8_t actual[6];
            unpack(screen.pixels[((size_t)(height / 2) * stride + x) / 2], actual);
            for (unsigned c = 0; c < 3; ++c)
                assert(abs((int)actual[c] - bars[bar][c]) <= 2);
        }
    }
    if (width == 640 && height == 480 && !wide) {
        char path[80];
        snprintf(path, sizeof(path), "build/screen-%u.ppm", page);
        FILE *file = fopen(path, "wb");
        assert(file);
        fprintf(file, "P6\n%u %u\n255\n", width, height);
        for (unsigned y = 0; y < height; ++y)
            for (unsigned x = 0; x < width; x += 2) {
                uint8_t pixels[6];
                unpack(screen.pixels[((size_t)y * stride + x) / 2], pixels);
                assert(fwrite(pixels, 1, 6, file) == 6);
            }
        assert(fclose(file) == 0);
    }
    if (page == SCREEN_NO_SIGNAL && width == 640 && height == 480 && !wide) {
        /* Bottom calibration patches distinguish full SMPTE from top stripes only. */
        uint8_t patch[6];
        unpack(screen.pixels[(330*stride+230)/2], patch);
        assert(patch[0] > 245 && patch[1] > 245 && patch[2] > 245);
        unpack(screen.pixels[(330*stride+300)/2], patch);
        assert(patch[0] > 30 && patch[2] > 80 && patch[1] < 10);
        unpack(screen.pixels[(330*stride+400)/2], patch);
        assert(patch[0] < 10 && patch[1] < 10 && patch[2] < 10);
    }
    free(rgb);
    free(allocation);
}

static void test_loading_circle(void)
{
    const size_t count = 640*480/2;
    uint32_t *idle = malloc(count*sizeof(*idle));
    uint32_t *first = malloc(count*sizeof(*first));
    uint32_t *next = malloc(count*sizeof(*next));
    assert(idle && first && next);
    Screen screen = {idle, 640, 480, 640, false};
    ScreenInfo info = {.sd=true, .status="Refreshing camera source - B cancels"};
    screen_draw(&screen, SCREEN_MENU, NULL, 640, 480, &info);
    info.refreshing = true;
    screen.pixels = first;
    screen_draw(&screen, SCREEN_MENU, NULL, 640, 480, &info);
    info.refresh_phase = 3;
    screen.pixels = next;
    screen_draw(&screen, SCREEN_MENU, NULL, 640, 480, &info);
    unsigned changed = 0;
    for (unsigned y = 0; y < 480; ++y) {
        for (unsigned x = 0; x < 640; x += 2) {
            size_t at = (y*640+x)/2;
            if (first[at] != next[at]) ++changed;
            /* Every animation update stays in the menu's existing button area. */
            if (x < 336 || x > 366 || y < 326 || y > 355) {
                assert(idle[at] == first[at]);
                assert(first[at] == next[at]);
            }
        }
    }
    assert(changed > 20);
    info.refreshing = false;
    screen_draw(&screen, SCREEN_MENU, NULL, 640, 480, &info);
    for (size_t n = 0; n < count; ++n) assert(next[n] == idle[n]);
    free(idle); free(first); free(next);
}

int main(void)
{
    test_loading_circle();
    assert(screen_pair(bars[7], bars[7]) == 0x10801080);
    assert(screen_pair(bars[0], bars[0]) == 0xeb80eb80);
    assert(screen_pair(bars[5], bars[5]) == 0x525a52f0);
    assert(screen_pair(bars[7], bars[0]) == 0x1080eb80);
    for (unsigned page = SCREEN_MENU; page <= SCREEN_NO_SIGNAL; ++page) {
        render(640, 480, 640, false, 320, 240, page);
        render(640, 574, 672, false, 640, 480, page);
        render(640, 480, 640, true, 640, 480, page);
    }
    puts("PASS: YCbCr reference colors, pair ordering, scaling, PAL, widescreen, framebuffer bounds, menu-only loading animation/cache reset");
}
