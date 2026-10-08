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
    screen_draw(&screen, page, page == SCREEN_MENU ? NULL : rgb,
                 image_width, image_height, "", true);
    assert(allocation[0] == 0xdeadbeef && allocation[words + 1] == 0xdeadbeef);
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = width; x < stride; x += 2)
            assert(screen.pixels[((size_t)y * stride + x) / 2] == 0xdeadbeef);
    if (page != SCREEN_MENU) {
        unsigned view_width = wide ? 324 : 432, left = (640 - view_width) / 2;
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
    free(rgb);
    free(allocation);
}

int main(void)
{
    assert(screen_pair(bars[7], bars[7]) == 0x10801080);
    assert(screen_pair(bars[0], bars[0]) == 0xeb80eb80);
    assert(screen_pair(bars[5], bars[5]) == 0x525a52f0);
    assert(screen_pair(bars[7], bars[0]) == 0x1080eb80);
    for (unsigned page = SCREEN_MENU; page <= SCREEN_REVIEW; ++page) {
        render(640, 480, 640, false, 320, 240, page);
        render(640, 574, 672, false, 640, 480, page);
        render(640, 480, 640, true, 640, 480, page);
    }
    puts("PASS: YCbCr reference colors, pair ordering, scaling, PAL, widescreen, framebuffer bounds");
}
