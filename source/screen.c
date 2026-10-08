/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 cmyksoda
 */
#include "screen.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>

uint32_t screen_pair(const uint8_t a[3], const uint8_t b[3])
{
    int y0 = ((66 * a[0] + 129 * a[1] + 25 * a[2] + 128) >> 8) + 16;
    int y1 = ((66 * b[0] + 129 * b[1] + 25 * b[2] + 128) >> 8) + 16;
    int r = a[0] + b[0], g = a[1] + b[1], blue = a[2] + b[2];
    int cb = ((-38 * r - 74 * g + 112 * blue + 256) >> 9) + 128;
    int cr = ((112 * r - 94 * g - 18 * blue + 256) >> 9) + 128;
    return (uint32_t)y0 << 24 | (uint32_t)cb << 16 | (uint32_t)y1 << 8 | (uint32_t)cr;
}

static uint32_t color(unsigned hex)
{
    uint8_t rgb[] = {hex >> 16, hex >> 8, hex};
    return screen_pair(rgb, rgb);
}

static void box(Screen *s, unsigned x, unsigned y, unsigned w, unsigned h, unsigned rgb)
{
    unsigned left = (x * s->width / 640) & ~1u;
    unsigned right = ((x + w) * s->width / 640) & ~1u;
    unsigned top = y * s->height / 480;
    unsigned bottom = (y + h) * s->height / 480;
    if (right > s->width) right = s->width & ~1u;
    if (bottom > s->height) bottom = s->height;
    uint32_t pair = color(rgb);
    for (unsigned row = top; row < bottom; ++row)
        for (unsigned col = left; col < right; col += 2)
            s->pixels[((size_t)row * s->stride + col) / 2] = pair;
}

/* Original five-column glyphs, rows encoded from top to bottom. */
static const uint8_t glyphs[][7] = {
    {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30},
    {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
    {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16},
    {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
    {14,4,4,4,4,4,14}, {7,2,2,2,18,18,12},
    {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
    {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17},
    {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
    {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17},
    {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4},
    {17,17,17,21,21,21,10}, {17,17,10,4,10,17,17},
    {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
    {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
    {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
    {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
    {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14},
    {0,0,0,31,0,0,0}, {0,4,4,0,4,4,0},
    {0,0,0,0,0,4,4}, {1,2,2,4,8,8,16}
};

static void text(Screen *s, unsigned x, unsigned y, const char *message, unsigned rgb)
{
    for (; *message && x + 10 <= 624; ++message, x += 12) {
        unsigned c = (unsigned char)*message;
        if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
        const char *symbols = "-:./";
        const char *symbol = strchr(symbols, (int)c);
        unsigned index;
        if (c >= 'A' && c <= 'Z') index = c - 'A';
        else if (c >= '0' && c <= '9') index = 26 + c - '0';
        else if (symbol) index = 36 + (unsigned)(symbol - symbols);
        else continue;
        for (unsigned row = 0; row < 7; ++row)
            for (unsigned col = 0; col < 5; ++col)
                if (glyphs[index][row] & (16 >> col))
                    box(s, x + col * 2, y + row * 2, 2, 2, rgb);
    }
}

static void camera_icon(Screen *s)
{
    static const char pixels[][25] = {
        "                        ",
        "        WWWWWWWW        ",
        "      WWWWWWWWWWWS      ",
        "     WWWWWWWWWWWWSS     ",
        "     WWWWWWWWWWWWSS     ",
        "    WWWWWWWWWWWWWSSS    ",
        "    WWWWWWBBBBWWWSSS    ",
        "    WWWWWBBLLBBWWSSS    ",
        "   WWWWWBBGGLLBBWSSSS   ",
        "    WWWWBLLLLLLBWSSS    ",
        "    WWWBBLLLLLLBBSSS    ",
        "    WWWWBLLLLLLBWSSS    ",
        "     WWWBBLLLLBBWSS     ",
        "     WWWWBBLLBBWWSS     ",
        "      WWWWBBBBWWWS      ",
        "        SSSSSSSS        ",
        "         WWWWSS         ",
        "         WWWWSS         ",
        "         WWWWSS         ",
        "   WWWWWWWWWWWWWWWWWS   ",
        "   WWWWWWWWWWWWWWWWWS   ",
        "   WWWWWWBBBBBBWWWWWS   ",
        "   SSSSSSSSSSSSSSSSSS   ",
        "   SSSSSSSSSSSSSSSSSS   ",
    };
    for (unsigned y = 0; y < 24; ++y) {
        for (unsigned x = 0; x < 24; ++x) {
            unsigned rgb;
            switch (pixels[y][x]) {
            case 'W': rgb = 0xf5f2f7; break;
            case 'S': rgb = 0xaaa6b5; break;
            case 'B': rgb = 0x303238; break;
            case 'L': rgb = 0x07090d; break;
            case 'G': rgb = 0x80858d; break;
            default: continue;
            }
            box(s, 272 + x * 4, 168 + y * 4, 4, 4, rgb);
        }
    }
}

static void photo(Screen *s, const uint8_t *rgb, unsigned width, unsigned height,
                  unsigned left, unsigned view_width)
{
    unsigned x0 = (left * s->width / 640) & ~1u;
    unsigned x1 = ((left + view_width) * s->width / 640) & ~1u;
    unsigned y0 = 66 * s->height / 480, y1 = 390 * s->height / 480;
    for (unsigned y = y0; y < y1; ++y) {
        unsigned source_y = (y - y0) * height / (y1 - y0);
        for (unsigned x = x0; x < x1; x += 2) {
            unsigned a = (x - x0) * width / (x1 - x0);
            unsigned b = (x + 1 - x0) * width / (x1 - x0);
            s->pixels[((size_t)y * s->stride + x) / 2] =
                screen_pair(rgb + ((size_t)source_y * width + a) * 3,
                            rgb + ((size_t)source_y * width + b) * 3);
        }
    }
}

void screen_draw(Screen *s, ScreenPage page, const uint8_t *rgb,
                 unsigned width, unsigned height, const char *status, bool sd)
{
    const unsigned ink = 0xf5f2f7, muted = 0xb6b0bf;
    const unsigned cyan = 0x25d9f0, magenta = 0xff5bb5, yellow = 0xffe06b;
    const unsigned background = 0x15141b, well = 0x09090d, border = 0x36333f;
    box(s, 0, 0, 640, 480, background);
    text(s, 254, 26, "WC04 CAMERA", ink);
    unsigned view_width = s->widescreen ? 324 : 432;
    unsigned left = (640 - view_width) / 2;
    box(s, left - 2, 64, view_width + 4, 328, border);
    box(s, left, 66, view_width, 324, well);
    if (rgb && width && height)
        photo(s, rgb, width, height, left, view_width);
    else {
        camera_icon(s);
        const char *label = page == SCREEN_MENU ? "READY WHEN YOU ARE" : "CONNECTING CAMERA";
        text(s, 320 - (unsigned)strlen(label) * 6, 274, label, muted);
        if (page == SCREEN_MENU) {
            char size[32];
            snprintf(size, sizeof(size), "1 - SIZE %u X %u", width, height);
            text(s, 320 - (unsigned)strlen(size) * 6, 312, size, yellow);
            text(s, 224, 340, "2 - CAMERA CHECK", muted);
        }
    }
    if (page == SCREEN_LIVE) {
        box(s, left + 12, 78, 72, 26, background);
        text(s, left + 24, 84, "LIVE", yellow);
    }
    text(s, 320 - (unsigned)(strlen(status) > 48 ? 48 : strlen(status)) * 6,
         400, status, page == SCREEN_REVIEW ? cyan : muted);
    box(s, 48, 422, 544, 2, border);
    if (page == SCREEN_REVIEW) {
        text(s, 48, 432, "A - LIVE", cyan);
        text(s, 498, 432, "B - MENU", magenta);
    } else if (page != SCREEN_MENU) {
        text(s, 48, 432, sd ? "A - PHOTO" : "NO SD", cyan);
        text(s, 498, 432, "B - MENU", magenta);
    }
    text(s, 254, 432, "HOME - EXIT", muted);
}
