/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "screen.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint8_t bars[][3] = {{255, 255, 255}, {255, 255, 0}, {0, 255, 255}, {0, 255, 0},
                                  {255, 0, 255},   {255, 0, 0},   {0, 0, 255},   {0, 0, 0}};

static uint8_t clamp(int n)
{
    return n < 0 ? 0 : n > 255 ? 255 : (uint8_t)n;
}

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
    for (size_t i = 0; i < words + 2; ++i)
        allocation[i] = 0xdeadbeef;
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
        unpack(screen.pixels[(330 * stride + 230) / 2], patch);
        assert(patch[0] > 245 && patch[1] > 245 && patch[2] > 245);
        unpack(screen.pixels[(330 * stride + 300) / 2], patch);
        assert(patch[0] > 30 && patch[2] > 80 && patch[1] < 10);
        unpack(screen.pixels[(330 * stride + 400) / 2], patch);
        assert(patch[0] < 10 && patch[1] < 10 && patch[2] < 10);
    }
    free(rgb);
    free(allocation);
}

static void test_loading_circle(void)
{
    const size_t count = 640 * 480 / 2;
    uint32_t *idle = malloc(count * sizeof(*idle));
    uint32_t *first = malloc(count * sizeof(*first));
    uint32_t *next = malloc(count * sizeof(*next));
    assert(idle && first && next);
    Screen screen = {idle, 640, 480, 640, false};
    ScreenInfo info = {.sd = true, .status = "Refreshing camera source - B cancels"};
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
            size_t at = (y * 640 + x) / 2;
            if (first[at] != next[at]) ++changed;
            /* Every animation update stays in the menu's existing button area. */
            if ((x < 48 || x > 592 || y < 106 || y > 196)
                && (x < 48 || x > 332 || y < 332 || y > 361)
                && (x < 564 || x > 592 || y < 332 || y > 360)) {
                assert(idle[at] == first[at]);
                assert(first[at] == next[at]);
            }
        }
    }
    assert(changed > 20);
    info.refreshing = false;
    screen_draw(&screen, SCREEN_MENU, NULL, 640, 480, &info);
    for (size_t n = 0; n < count; ++n)
        assert(next[n] == idle[n]);
    free(idle);
    free(first);
    free(next);
}

static void test_display_aspect(unsigned height, bool wide)
{
    uint32_t *pixels = calloc(640 * height / 2, sizeof(*pixels));
    uint8_t *rgb = calloc(640 * 480 * 3, 1);
    assert(pixels && rgb);
    for (int y = 0; y < 480; ++y)
        for (int x = 0; x < 640; ++x)
            if ((x - 320) * (x - 320) + (y - 240) * (y - 240) <= 60 * 60)
                for (unsigned c = 0; c < 3; ++c)
                    rgb[(y * 640 + x) * 3 + c] = 255;
    Screen screen = {pixels, 640, height, 640, wide};
    screen_draw(&screen, SCREEN_LIVE, rgb, 640, 480, &(ScreenInfo){.sd = true});
    unsigned min_x = 640, max_x = 0, min_y = height, max_y = 0;
    for (unsigned y = 150 * height / 480; y < 320 * height / 480; ++y)
        for (unsigned x = 160; x < 480; x += 2) {
            uint8_t pair[6];
            unpack(pixels[(y * 640 + x) / 2], pair);
            for (unsigned p = 0; p < 2; ++p) {
                if (pair[3 * p] < 240 || pair[3 * p + 1] < 240 || pair[3 * p + 2] < 240) continue;
                if (x + p < min_x) min_x = x + p;
                if (x + p > max_x) max_x = x + p;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    assert(max_x > min_x && max_y > min_y);
    double display_width = (max_x - min_x + 1) * (wide ? 854.0 : 640.0) / 640.0;
    double display_height = (max_y - min_y + 1) * 480.0 / height;
    double ratio = display_width / display_height;
    assert(ratio > .96 && ratio < 1.04);
    if (wide) assert((max_x - min_x + 1) / display_height < .8);
    if (height == 480) {
        char name[80];
        snprintf(name, sizeof(name), "build/aspect-%s-framebuffer.ppm", wide ? "16x9" : "4x3");
        FILE *f = fopen(name, "wb");
        assert(f);
        fprintf(f, "P6\n640 480\n255\n");
        for (unsigned at = 0; at < 640 * 480 / 2; ++at) {
            uint8_t pair[6];
            unpack(pixels[at], pair);
            assert(fwrite(pair, 1, 6, f) == 6);
        }
        assert(fclose(f) == 0);
    }
    printf("PASS: %s TV, 640x%u framebuffer, displayed circle %.2fx%.2f\n", wide ? "16:9" : "4:3",
           height, display_width, display_height);
    free(rgb);
    free(pixels);
}

static void test_countdown(void)
{
    uint32_t *pixels = malloc(640 * 480 / 2 * sizeof(*pixels));
    uint8_t *rgb = malloc(320 * 240 * 3);
    assert(pixels && rgb);
    Screen screen = {pixels, 640, 480, 640, false};
    for (unsigned count = 1; count <= 10; ++count) {
        ScreenInfo info = {.sd = true, .timer = 10, .countdown = count};
        for (unsigned brightness = 80; brightness <= 200; brightness += 120) {
            memset(rgb, brightness, 320 * 240 * 3);
            screen_draw(&screen, SCREEN_LIVE, rgb, 320, 240, &info);
            uint8_t actual[6];
            unpack(pixels[(315 * 640 + 440) / 2], actual);
            for (unsigned c = 0; c < 6; ++c)
                assert(abs((int)actual[c] - (int)brightness / 2) <= 2);
            /* The overlay must not leave the old opaque center box behind. */
            unpack(pixels[(210 * 640 + 300) / 2], actual);
            for (unsigned c = 0; c < 6; ++c)
                assert(abs((int)actual[c] - (int)brightness) <= 2);
            unpack(pixels[(309 * 640 + 440) / 2], actual);
            assert(actual[0] > 245 && actual[1] > 245 && actual[2] < 8);
            for (unsigned y = 413; y < 449; ++y)
                for (unsigned x = 48; x < 220; x += 2)
                    assert(pixels[(y * 640 + x) / 2] == 0x10801080);
            for (size_t at = 0; at < 320 * 240 * 3; ++at)
                assert(rgb[at] == brightness);
        }
    }
    free(pixels);
    free(rgb);
    puts(
        "PASS: all countdown values blend over changing live frames, preserve source pixels and clear duplicate footer");
}

static void draw_review(uint32_t *pixels, const uint8_t *rgb, ScreenInfo info)
{
    Screen screen = {pixels, 640, 480, 640, false};
    screen_draw(&screen, SCREEN_REVIEW, rgb, 320, 240, &info);
}

/* Counts changed pixel pairs and asserts every change lies in one of the allowed row bands. */
static unsigned changes(const uint32_t *a, const uint32_t *b, unsigned top, unsigned bottom,
                        unsigned top2, unsigned bottom2)
{
    unsigned changed = 0;
    for (unsigned y = 0; y < 480; ++y)
        for (unsigned x = 0; x < 640; x += 2)
            if (a[(y * 640 + x) / 2] != b[(y * 640 + x) / 2]) {
                assert((y >= top && y <= bottom) || (y >= top2 && y <= bottom2));
                ++changed;
            }
    return changed;
}

static void test_review_layout(void)
{
    uint32_t *plain = malloc(640 * 480 / 2 * sizeof(*plain));
    uint32_t *actions = malloc(640 * 480 / 2 * sizeof(*actions));
    uint32_t *other = malloc(640 * 480 / 2 * sizeof(*other));
    uint8_t *rgb = malloc(320 * 240 * 3);
    assert(plain && actions && other && rgb);
    memset(rgb, 128, 320 * 240 * 3);
    ScreenInfo base = {.sd = true, .video = true, .audio = true, .status = "Saved AV0005.AVI"};
    ScreenInfo with = base;
    with.can_play = with.can_delete = true;
    draw_review(plain, rgb, base);
    draw_review(actions, rgb, with);
    /* Play and Delete stay in the status row, clear of the picture and the bottom buttons. */
    assert(changes(plain, actions, 376, 407, 376, 407) > 100);
    ScreenInfo confirm = with;
    confirm.confirm_delete = true;
    confirm.status = "Press - again to delete AV0005.AVI";
    draw_review(other, rgb, confirm);
    assert(changes(actions, other, 376, 407, 376, 407) > 100);
    ScreenInfo photo = with;
    photo.video = photo.audio = photo.can_play = false;
    draw_review(other, rgb, photo);
    assert(changes(actions, other, 0, 79, 376, 407) > 100);
    ScreenInfo playing = base;
    playing.playing = true;
    draw_review(other, rgb, playing);
    assert(changes(plain, other, 0, 79, 412, 479) > 100);
    free(plain);
    free(actions);
    free(other);
    free(rgb);
    puts("PASS: review actions, delete confirmation and playback stay in their rows");
}

int main(void)
{
    test_countdown();
    test_review_layout();
    test_loading_circle();
    test_display_aspect(480, false);
    test_display_aspect(480, true);
    test_display_aspect(574, false);
    test_display_aspect(574, true);
    assert(screen_pair(bars[7], bars[7]) == 0x10801080);
    assert(screen_pair(bars[0], bars[0]) == 0xeb80eb80);
    assert(screen_pair(bars[5], bars[5]) == 0x525a52f0);
    assert(screen_pair(bars[7], bars[0]) == 0x1080eb80);
    for (unsigned page = SCREEN_MENU; page <= SCREEN_NO_SIGNAL; ++page) {
        render(640, 480, 640, false, 320, 240, page);
        render(640, 574, 672, false, 640, 480, page);
        render(640, 480, 640, true, 640, 480, page);
    }
    puts(
        "PASS: YCbCr reference colors, pair ordering, scaling, PAL, widescreen, framebuffer bounds, menu-only loading animation/cache reset");
}
