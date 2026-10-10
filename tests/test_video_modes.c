/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "uvc_fixture.h"
#include "photo.h"
#include "avi.h"
#include "screen.h"
#include <stdlib.h>

static void dimensions(Fixture *f, unsigned width, unsigned height)
{
    f->data[f->frame + 5] = width;
    f->data[f->frame + 6] = width >> 8;
    f->data[f->frame + 7] = height;
    f->data[f->frame + 8] = height >> 8;
    uvc_put32(f->data + f->frame + 17, width * height * 2 + 589);
}

static void check_preview(unsigned width, unsigned height, bool wide, unsigned fb_height)
{
    size_t count = (size_t)width * height * 3;
    uint8_t *rgb = calloc(count, 1);
    uint32_t *guard = malloc((640 * fb_height / 2 + 2) * sizeof(*guard));
    assert(rgb && guard);
    guard[0] = guard[640 * fb_height / 2 + 1] = 0xdeadbeef;
    int radius = (int)height / 8;
    for (int y = 0; y < (int)height; ++y)
        for (int x = 0; x < (int)width; ++x)
            if ((x - (int)width / 2) * (x - (int)width / 2)
                    + (y - (int)height / 2) * (y - (int)height / 2)
                <= radius * radius)
                memset(rgb + ((size_t)y * width + x) * 3, 255, 3);
    Screen s = {guard + 1, 640, fb_height, 640, wide};
    ScreenInfo info = {.sd = true};
    screen_draw(&s, SCREEN_LIVE, rgb, width, height, &info);
    unsigned min_x = 640, max_x = 0, min_y = fb_height, max_y = 0;
    for (unsigned y = 150 * fb_height / 480; y < 320 * fb_height / 480; ++y)
        for (unsigned x = 200; x < 440; ++x) {
            uint32_t pair = s.pixels[(y * 640 + x) / 2];
            unsigned luma = (pair >> (x & 1 ? 8 : 24)) & 255;
            if (luma < 220) continue;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    assert(max_x > min_x && max_y > min_y);
    double ratio = ((max_x - min_x + 1) * (wide ? 854.0 : 640.0) / 640.0)
                   / ((max_y - min_y + 1) * 480.0 / fb_height);
    assert(ratio > .93 && ratio < 1.07);
    assert(guard[0] == 0xdeadbeef && guard[640 * fb_height / 2 + 1] == 0xdeadbeef);
    /* Uniform source makes both the fit rectangle and overlay placement measurable. */
    memset(rgb, 80, count);
    info.countdown = 3;
    screen_draw(&s, SCREEN_LIVE, rgb, width, height, &info);
    unsigned logical = wide ? 854 : 640, fw = 360, fh = 270;
    if (width * 270 > height * 360)
        fh = height * 360 / width;
    else
        fw = width * 270 / height;
    unsigned left = (logical - fw) / 2, top = 99 + (270 - fh) / 2;
    unsigned border_x = (left + fw - 50) * 640 / logical,
             border_y = (top + fh - 60) * fb_height / 480;
    /* Nearest-coordinate mapping can land just before the logical border. */
    bool yellow = false;
    for (unsigned dy = 0; dy < 3; ++dy) {
        uint32_t pair = s.pixels[((border_y + dy) * 640 + border_x) / 2];
        unsigned luma = (pair >> (border_x & 1 ? 8 : 24)) & 255;
        yellow |= luma > 180;
    }
    assert(yellow);
    unsigned black_x = (logical - 360) / 2 * 640 / logical + 4, black_y = 103 * fb_height / 480;
    if (width * 270 > height * 360) {
        uint32_t pair = s.pixels[(black_y * 640 + 320) / 2];
        assert(pair == 0x10801080);
    } else if (width * 270 < height * 360) {
        uint32_t pair = s.pixels[(230 * fb_height / 480 * 640 + black_x) / 2];
        assert(pair == 0x10801080);
    }
    free(guard);
    free(rgb);
}

int main(void)
{
    assert(WC04_FRAME_CAPACITY == 815104 && WC04_RGB_CAPACITY == 1221120);
    for (unsigned n = 0; n < VIDEO_SIZE_COUNT; ++n) {
        VideoSize size = video_size(n);
        Fixture f = fixture(1, 0, 0x81, false);
        dimensions(&f, size.width, size.height);
        UvcMode mode;
        assert(uvc_parse(f.data, f.length, 1, size.width, size.height, &mode) == UVC_OK);
        assert(uvc_supported_sizes(f.data, f.length, 1) == (1u << n));
        VideoSize next = video_size_next(320, 240, 1u << n);
        assert(next.width == size.width && next.height == size.height);
        uvc_put32(f.data + f.frame + 17, WC04_FRAME_CAPACITY + 1);
        assert(uvc_parse(f.data, f.length, 1, size.width, size.height, &mode) == UVC_LIMIT);
        assert(!uvc_supported_sizes(f.data, f.length, 1));
        for (unsigned wide = 0; wide < 2; ++wide)
            for (unsigned fb = 480; fb <= 574; fb += 94)
                check_preview(size.width, size.height, wide, fb);
        size_t pixels = (size_t)size.width * size.height * 3;
        uint8_t *rgb = malloc(pixels), *decoded = malloc(pixels + 2);
        assert(rgb && decoded);
        memset(rgb, 100, pixels);
        decoded[0] = 0xa5;
        decoded[pixels + 1] = 0x5a;
        FILE *jpg = tmpfile();
        assert(jpg && photo_encode(jpg, rgb, size.width, size.height));
        assert(fflush(jpg) == 0);
        long bytes = ftell(jpg);
        assert(bytes > 0);
        uint8_t *jpeg = malloc((size_t)bytes);
        assert(jpeg);
        rewind(jpg);
        assert(fread(jpeg, 1, (size_t)bytes, jpg) == (size_t)bytes);
        fclose(jpg);
        assert(photo_decode(jpeg, (size_t)bytes, decoded + 1, size.width, size.height));
        assert(decoded[0] == 0xa5 && decoded[pixels + 1] == 0x5a);
        assert(!photo_decode(jpeg, (size_t)bytes, decoded + 1, size.width + 1, size.height));
        char path[80];
        snprintf(path, sizeof(path), "build/mode-%ux%u.avi", size.width, size.height);
        FILE *video = fopen(path, "wb+");
        Avi avi;
        assert(video && avi_begin(&avi, video, size.width, size.height));
        for (unsigned k = 0; k < 5; ++k)
            assert(avi_append(&avi, jpeg, (size_t)bytes));
        assert(avi_finish(&avi, 1000) && fclose(video) == 0);
        free(jpeg);
        free(decoded);
        free(rgb);
    }
    const VideoSize rejected[] = {{1280, 720}, {1920, 1080}, {800, 600}, {640, 360}, {0, 0}};
    for (unsigned n = 0; n < sizeof(rejected) / sizeof(*rejected); ++n) {
        Fixture f = fixture(1, 0, 0x81, false);
        UvcMode mode;
        dimensions(&f, rejected[n].width, rejected[n].height);
        assert(uvc_parse(f.data, f.length, 1, rejected[n].width, rejected[n].height, &mode)
               == UVC_NO_MODE);
    }
    FILE *file = tmpfile();
    uint8_t dummy[3] = {0};
    Avi avi;
    assert(file && !photo_encode(file, dummy, WC04_MAX_WIDTH + 1, 480));
    assert(!avi_begin(&avi, file, 848, WC04_MAX_HEIGHT + 1));
    fclose(file);
    puts(
        "PASS: seven bounded descriptor modes, filtering/cycling, JPEG/AVI dimensions, aspect-fit and countdown across TV modes");
}
