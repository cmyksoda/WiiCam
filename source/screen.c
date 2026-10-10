/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 cmyksoda
 */
#include "screen.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "ui_assets.h"

#define CANVAS_WIDTH 854
static uint8_t canvas[CANVAS_WIDTH * 480 * 3];
static uint32_t chrome[864 * 576 / 2];
static char cached_key[256];
static unsigned logical_width;

uint32_t screen_pair(const uint8_t a[3], const uint8_t b[3])
{
    int y0 = ((66 * a[0] + 129 * a[1] + 25 * a[2] + 128) >> 8) + 16;
    int y1 = ((66 * b[0] + 129 * b[1] + 25 * b[2] + 128) >> 8) + 16;
    int r = a[0] + b[0], g = a[1] + b[1], blue = a[2] + b[2];
    int cb = ((-38 * r - 74 * g + 112 * blue + 256) >> 9) + 128;
    int cr = ((112 * r - 94 * g - 18 * blue + 256) >> 9) + 128;
    return (uint32_t)y0 << 24 | (uint32_t)cb << 16 | (uint32_t)y1 << 8 | (uint32_t)cr;
}

static void pixel(int x, int y, unsigned color, unsigned alpha)
{
    if (x < 0 || y < 0 || x >= (int)logical_width || y >= 480)
        return;
    uint8_t *p = canvas + ((size_t)y * CANVAS_WIDTH + x) * 3;
    for (unsigned c = 0; c < 3; ++c) {
        unsigned value = (color >> ((2-c)*8)) & 255;
        p[c] = (uint8_t)((value * alpha + p[c] * (255-alpha) + 127) / 255);
    }
}

static bool inside_round(int x, int y, int w, int h, int radius)
{
    if (x < 0 || y < 0 || x >= w || y >= h)
        return false;
    int dx = x < radius ? radius-x : x >= w-radius ? x-(w-radius-1) : 0;
    int dy = y < radius ? radius-y : y >= h-radius ? y-(h-radius-1) : 0;
    return dx*dx+dy*dy <= radius*radius;
}

static void round_box(int x, int y, int w, int h, int radius, unsigned color)
{
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
            if (inside_round(i, j, w, h, radius))
                pixel(x+i, y+j, color, 255);
}

static const UiGlyph *font(unsigned size)
{
    switch (size) {
    case 12: return ui_font_12;
    case 15: return ui_font_15;
    case 18: return ui_font_18;
    case 22: return ui_font_22;
    case 25: return ui_font_25;
    case 28: return ui_font_28;
    case 48: return ui_font_48;
    default: return ui_font_20;
    }
}

static int text_width(const char *text, unsigned size)
{
    int width = 0;
    for (; *text; ++text)
        width += font(size)[*text >= 32 && *text <= 126 ? *text-32 : '?'-32].advance;
    return width;
}

static void text(int x, int baseline, const char *message, unsigned size, unsigned color)
{
    for (; *message; ++message) {
        const UiGlyph *g = font(size) + (*message >= 32 && *message <= 126 ? *message-32 : '?'-32);
        for (int y = 0; y < g->h; ++y)
            for (int col = 0; col < g->w; ++col)
                pixel(x+g->x+col, baseline+g->y+y, color,
                      ui_font_pixels[g->offset + y*g->w + col]);
        x += g->advance;
    }
}

static void centered(int x, int baseline, const char *message, unsigned size, unsigned color)
{
    text(x-text_width(message, size)/2, baseline, message, size, color);
}

static void key(int x, int y, const char *label, unsigned fill, unsigned line, unsigned ink)
{
    round_box(x-14, y-14, 28, 28, 14, line);
    round_box(x-13, y-13, 26, 26, 13, fill);
    centered(x, y+7, label, 20, ink);
}

static void loading_circle(int x, int y, unsigned phase, unsigned color)
{
    static const int offsets[8][2] = {
        {0,-10}, {7,-7}, {10,0}, {7,7}, {0,10}, {-7,7}, {-10,0}, {-7,-7}
    };
    for (unsigned n = 0; n < 8; ++n) {
        unsigned alpha = 255 - 27 * ((phase - n) & 7);
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                if (dx*dx + dy*dy <= 4)
                    pixel(x + offsets[n][0] + dx, y + offsets[n][1] + dy, color, alpha);
    }
}

static void home(int x)
{
    round_box(x, 419, 68, 25, 12, 0xbcced6);
    round_box(x+1, 420, 66, 23, 11, 0xffffff);
    centered(x+34, 438, "HOME", 18, 0x253f4c);
    text(x+76, 438, "Exit", 20, 0x4e6572);
}

static void image_rgb(const uint8_t *rgb, unsigned width, unsigned height, int x, int y)
{
    for (unsigned row = 0; row < height; ++row)
        memcpy(canvas + ((y+row)*CANVAS_WIDTH+x)*3, rgb+row*width*3, width*3);
}

static void compose(ScreenPage page, unsigned width, unsigned height, const ScreenInfo *i)
{
    unsigned w = logical_width, margin = (w*75+500)/1000;
    const unsigned ink = 0x253f4c, muted = 0x4e6572, line = 0xbcced6, cyan = 0x006a82;
    for (unsigned y = 0; y < 480; ++y)
        for (unsigned x = 0; x < w; ++x) {
            unsigned top[] = {251,253,254}, bottom[] = {220,232,237};
            unsigned color = 0;
            for (unsigned c = 0; c < 3; ++c)
                color = (color << 8) | (top[c]*(480-y)+bottom[c]*y)/480;
            if (y > 418u-24*x*(w-x)/(w*w))
                color = 0xedf3f5;
            pixel(x, y, color, 255);
        }
    text(margin, 60, "WC04 Camera", page == SCREEN_MENU ? 28 : 25, ink);
    const char *heading = page == SCREEN_MENU ? (i->sd ? "SD ready" : "No SD") :
        page == SCREEN_REVIEW ? "Photo review" : page == SCREEN_NO_SIGNAL ? "No signal" :
        page == SCREEN_WAITING ? "Please wait" : i->recording ? "Recording" : "Live";
    int hx = w-margin-text_width(heading, 18);
    round_box(hx-17, 48, 7, 7, 3, page == SCREEN_NO_SIGNAL ? 0x805d04 : i->recording ? 0x984668 : cyan);
    text(hx, 59, heading, 18, muted);
    round_box(margin, 76, 11, 3, 1, 0x8ad0e0);
    round_box(margin+15, 76, 11, 3, 1, 0xd993b5);
    round_box(margin+30, 76, 11, 3, 1, 0xefd17e);
    text(margin+60, 78, "USB experimental", 12, muted);
    if (page == SCREEN_MENU) {
        int left = (w-544)/2;
        round_box(left, 102, 544, 294, 24, 0xd3e1e6);
        round_box(left, 98, 544, 294, 24, line);
        round_box(left+1, 99, 542, 292, 23, 0xffffff);
        for (unsigned y = 0; y < UI_ICON_HEIGHT; ++y)
            for (unsigned x = 0; x < UI_ICON_WIDTH; ++x) {
                const unsigned char *p = ui_icon + (y*UI_ICON_WIDTH+x)*4;
                pixel(left+38+x, 123+y, p[0]<<16 | p[1]<<8 | p[2], p[3]);
            }
        centered(left+103, 298, "Vecteezy.com", 12, muted);
        text(left+194, 146, "Ready when", 28, ink);
        text(left+194, 180, "you are.", 28, ink);
        text(left+194, 214, "Connect a USB camera.", 20, muted);
        round_box(left+187, 237, 325, 58, 29, cyan);
        round_box(left+188, 238, 323, 56, 28, 0xe2f5fb);
        key(left+215, 266, "A", 0xe2f5fb, cyan, cyan);
        text(left+242, 274, "Open viewfinder", 22, cyan);
        round_box(left+30, 316, 484, 1, 0, line);
        key(left+43, 341, "1", 0xfff1c9, 0xc4a257, 0x735509);
        char size[40];
        snprintf(size, sizeof(size), "Size: %u x %u", width, height);
        text(left+65, 348, size, 20, ink);
        if (i->refreshing)
            loading_circle(left+303, 341, i->refresh_phase, cyan);
        else
            key(left+303, 341, "2", 0xffffff, line, ink);
        text(left+325, 348, "Refresh camera source", 18, ink);
        text(left+30, 378, i->video ? "+ Mode: Video (silent)" : "+ Mode: Photo", 15, muted);
        char timer[40];
        if (i->video)
            snprintf(timer, sizeof(timer), "60 s / 128 MiB limit");
        else if (i->timer)
            snprintf(timer, sizeof(timer), "Left/Right: Timer %u s", i->timer);
        else
            snprintf(timer, sizeof(timer), "Left/Right: Timer Off");
        text(left+290, 378, timer, 15, muted);
        if (i->status && *i->status)
            centered(w/2, 411, i->status, 15, muted);
        home(w/2-58);
        return;
    }
    int left = (w-360)/2;
    round_box(left-6, 88, 372, 282, 9, 0xcddde3);
    round_box(left-5, 84, 370, 280, 8, line);
    round_box(left-4, 85, 368, 278, 7, 0xffffff);
    image_rgb(ui_bars, 360, 270, left, 89);
    if (i->countdown && page == SCREEN_LIVE) {
        char count[16];
        snprintf(count, sizeof(count), "%u", i->countdown);
        round_box(w/2-44, 178, 88, 88, 20, 0xffffff);
        centered(w/2, 239, count, 48, cyan);
    }
    char status[100];
    if (i->status && *i->status)
        snprintf(status, sizeof(status), "%s", i->status);
    else if (page == SCREEN_NO_SIGNAL)
        snprintf(status, sizeof(status), "%s", i->recording ?
                 "Signal lost - B stops recording" : "Waiting for camera signal");
    else if (i->recording)
        snprintf(status, sizeof(status), "REC %02u:%02u / 01:00 - A stops", i->seconds/60, i->seconds%60);
    else if (i->countdown)
        snprintf(status, sizeof(status), "Photo in %u - B cancels", i->countdown);
    else if (i->shutter)
        snprintf(status, sizeof(status), "Taking photo...");
    else if (i->video)
        snprintf(status, sizeof(status), "%u x %u - silent video - %s", width, height, i->sd ? "SD ready" : "No SD");
    else
        if (i->timer)
            snprintf(status, sizeof(status), "%u x %u - %s - Timer %u s", width, height,
                     i->sd ? "SD ready" : "No SD", i->timer);
        else
            snprintf(status, sizeof(status), "%u x %u - %s", width, height,
                     i->sd ? "SD ready" : "No SD");
    centered(w/2, 399, status, 18, muted);
    if (page == SCREEN_NO_SIGNAL || page == SCREEN_WAITING)
        text(margin, 438, "No capture", 20, muted);
    else if (!i->sd)
        text(margin, 438, "Preview only", 20, muted);
    else {
        key(margin+13, 431, "A", 0xe2f5fb, cyan, cyan);
        text(margin+35, 438, page == SCREEN_REVIEW ? "Live view" : i->recording ? "Stop video" :
             i->video ? "Start video" : "Take photo", 20, ink);
    }
    home(w/2-58);
    int bx = w-margin-(i->countdown ? 103 : 91);
    key(bx+13, 431, "B", 0xf8e3ee, 0xc17fa0, 0x984668);
    text(bx+35, 438, i->countdown ? "Cancel" : "Menu", 20, 0x984668);
}

void screen_draw(Screen *s, ScreenPage page, const uint8_t *rgb,
                 unsigned width, unsigned height, const ScreenInfo *info)
{
    if (!s || !s->pixels || !info || !s->width || s->width > 864 || (s->width & 1) ||
        !s->height || s->height > 576 || s->stride < s->width || (s->stride & 1))
        return;
    logical_width = s->widescreen ? 854 : 640;
    char key_string[256];
    snprintf(key_string, sizeof(key_string), "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u:%s",
             page, width, height, s->width, s->height, logical_width,
             info->sd, info->video, info->recording, info->shutter, info->timer,
             info->countdown, info->seconds, info->refreshing, info->refresh_phase,
             info->status ? info->status : "");
    if (strcmp(cached_key, key_string)) {
        compose(page, width, height, info);
        for (unsigned y = 0; y < s->height; ++y)
            for (unsigned x = 0; x < s->width; x += 2) {
                size_t row = (y*480/s->height)*CANVAS_WIDTH;
                chrome[(y*s->width+x)/2] = screen_pair(
                    canvas+(row+x*logical_width/s->width)*3,
                    canvas+(row+(x+1)*logical_width/s->width)*3);
            }
        snprintf(cached_key, sizeof(cached_key), "%s", key_string);
    }
    for (unsigned y = 0; y < s->height; ++y)
        memcpy(s->pixels+y*s->stride/2, chrome+y*s->width/2, s->width*2);
    if (!rgb || !width || !height || page == SCREEN_MENU || page == SCREEN_NO_SIGNAL)
        return;
    unsigned left = (logical_width-360)/2;
    unsigned x0 = (left*s->width/logical_width) & ~1u;
    unsigned x1 = ((left+360)*s->width/logical_width) & ~1u;
    unsigned y0 = 89*s->height/480, y1 = 359*s->height/480;
    if (x1 <= x0 || y1 <= y0)
        return;
    for (unsigned y = y0; y < y1; ++y) {
        unsigned row = (y-y0)*height/(y1-y0);
        for (unsigned x = x0; x < x1; x += 2) {
            if (info->countdown && page == SCREEN_LIVE) {
                int ox = (int)(x*logical_width/s->width)-(int)logical_width/2+44;
                int ox1 = (int)((x+1)*logical_width/s->width)-(int)logical_width/2+44;
                int oy = (int)(y*480/s->height)-178;
                if (inside_round(ox, oy, 88, 88, 20) && inside_round(ox1, oy, 88, 88, 20))
                    continue;
            }
            unsigned a = (x-x0)*width/(x1-x0), b = (x+1-x0)*width/(x1-x0);
            s->pixels[((size_t)y*s->stride+x)/2] = screen_pair(
                rgb+((size_t)row*width+a)*3, rgb+((size_t)row*width+b)*3);
        }
    }
}
