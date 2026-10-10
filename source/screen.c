/* SPDX-License-Identifier: GPL-3.0-or-later
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
    if (x < 0 || y < 0 || x >= (int)logical_width || y >= 480) return;
    uint8_t *p = canvas + ((size_t)y * CANVAS_WIDTH + x) * 3;
    for (unsigned c = 0; c < 3; ++c) {
        unsigned value = (color >> ((2 - c) * 8)) & 255;
        p[c] = (uint8_t)((value * alpha + p[c] * (255 - alpha) + 127) / 255);
    }
}

static bool inside_round(int x, int y, int w, int h, int radius)
{
    if (x < 0 || y < 0 || x >= w || y >= h) return false;
    int dx = x < radius ? radius - x : x >= w - radius ? x - (w - radius - 1) : 0;
    int dy = y < radius ? radius - y : y >= h - radius ? y - (h - radius - 1) : 0;
    return dx * dx + dy * dy <= radius * radius;
}

static void round_box(int x, int y, int w, int h, int radius, unsigned color)
{
    for (int j = 0; j < h; ++j)
        for (int i = 0; i < w; ++i)
            if (inside_round(i, j, w, h, radius)) pixel(x + i, y + j, color, 255);
}

static const UiGlyph *font(unsigned size)
{
    switch (size) {
    case 12:
        return ui_font_12;
    case 15:
        return ui_font_15;
    case 18:
        return ui_font_18;
    case 22:
        return ui_font_22;
    case 25:
        return ui_font_25;
    case 28:
        return ui_font_28;
    default:
        return ui_font_20;
    }
}

static int text_width(const char *text, unsigned size)
{
    int width = 0;
    for (; *text; ++text)
        width += font(size)[*text >= 32 && *text <= 126 ? *text - 32 : '?' - 32].advance;
    return width;
}

static void text(int x, int baseline, const char *message, unsigned size, unsigned color)
{
    for (; *message; ++message) {
        const UiGlyph *g =
            font(size) + (*message >= 32 && *message <= 126 ? *message - 32 : '?' - 32);
        for (int y = 0; y < g->h; ++y)
            for (int col = 0; col < g->w; ++col)
                pixel(x + g->x + col, baseline + g->y + y, color,
                      ui_font_pixels[g->offset + y * g->w + col]);
        x += g->advance;
    }
}

static void badge(int x, int y, unsigned index, unsigned ink)
{
    const unsigned char *mask = ui_badges + index * UI_BADGE_SIZE * UI_BADGE_SIZE;
    for (int row = 0; row < UI_BADGE_SIZE; ++row)
        for (int col = 0; col < UI_BADGE_SIZE; ++col)
            pixel(x - UI_BADGE_SIZE / 2 + col, y - UI_BADGE_SIZE / 2 + row, ink,
                  mask[row * UI_BADGE_SIZE + col]);
}

static void key(int x, int y, const char *label, unsigned fill, unsigned line, unsigned ink)
{
    for (int row = -14; row <= 14; ++row)
        for (int col = -14; col <= 14; ++col) {
            unsigned ring = 0, interior = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    int dx = 8 * col + 2 * sx - 3, dy = 8 * row + 2 * sy - 3;
                    int distance = dx * dx + dy * dy;
                    ring += distance <= 104 * 104 && distance >= 92 * 92;
                    interior += distance < 92 * 92;
                }
            if (interior) pixel(x + col, y + row, fill, interior * 255 / 16);
            if (ring) pixel(x + col, y + row, line, ring * 255 / 16);
        }
    switch (*label) {
    case 'A':
        badge(x, y, 0, ink);
        break;
    case 'B':
        badge(x, y, 1, ink);
        break;
    case '+':
        badge(x, y, 2, ink);
        break;
    case '1':
        badge(x, y, 3, ink);
        break;
    case '2':
        badge(x, y, 4, ink);
        break;
    default:
        break;
    }
}

static void loading_circle(int x, int y, unsigned phase, unsigned color)
{
    static const int offsets[8][2] = {{0, -10}, {7, -7}, {10, 0},  {7, 7},
                                      {0, 10},  {-7, 7}, {-10, 0}, {-7, -7}};
    for (unsigned n = 0; n < 8; ++n) {
        unsigned alpha = 255 - 27 * ((phase - n) & 7);
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx)
                if (dx * dx + dy * dy <= 4)
                    pixel(x + offsets[n][0] + dx, y + offsets[n][1] + dy, color, alpha);
    }
}

static int text_right_edge(const char *message, unsigned size)
{
    int advance = 0, right = 0;
    for (; *message; ++message) {
        const UiGlyph *g =
            font(size) + (*message >= 32 && *message <= 126 ? *message - 32 : '?' - 32);
        if (advance + g->x + g->w > right) right = advance + g->x + g->w;
        advance += g->advance;
    }
    return right;
}

static void text_top(int x, int y, const char *message, unsigned size, unsigned color)
{
    int top = 0;
    for (const char *p = message; *p; ++p) {
        const UiGlyph *g = font(size) + (*p >= 32 && *p <= 126 ? *p - 32 : '?' - 32);
        if (g->h && g->y < top) top = g->y;
    }
    text(x, y - top, message, size, color);
}

static void text_center_y(int x, int cy, const char *message, unsigned size, unsigned color)
{
    int top = 480, bottom = -480;
    for (const char *p = message; *p; ++p) {
        const UiGlyph *g = font(size) + (*p >= 32 && *p <= 126 ? *p - 32 : '?' - 32);
        if (!g->h) continue;
        if (g->y < top) top = g->y;
        if (g->y + g->h - 1 > bottom) bottom = g->y + g->h - 1;
    }
    text(x, cy - (top + bottom) / 2, message, size, color);
}

static void countdown_pixel(uint8_t *rgb, int x, int y, unsigned count)
{
    if (x < 0 || y < 0 || x >= 56 || y >= 48) return;
    unsigned ink =
        count >= 1 && count <= 10 ? ui_countdown_ink[(count - 1) * 56 * 48 + y * 56 + x] : 0;
    if (!x || !y || x == 55 || y == 47) ink = 255;
    for (unsigned c = 0; c < 3; ++c) {
        unsigned yellow = c < 2 ? 255 : 0;
        unsigned background = (rgb[c] + 1) / 2;
        rgb[c] = (uint8_t)((yellow * ink + background * (255 - ink) + 127) / 255);
    }
}

static void status_line(int left, int y, int width, const char *message, unsigned color,
                        bool center)
{
    unsigned size = 18;
    if (text_width(message, size) > width) size = 15;
    if (text_width(message, size) > width) size = 12;
    text_top(center ? left + (width - text_width(message, size)) / 2 : left, y, message, size,
             color);
}

static void home(int x, int y)
{
    key(x + 14, y + 12, "", 0, 0xb8b8b8, 0xffffff);
    badge(x + 14, y + 12, 5, 0xffffff);
    text_center_y(x + 35, y + 12, "Exit", 18, 0xb8b8b8);
}

static void refresh_arrow(int x, int y)
{
    for (int dy = -10; dy <= 10; ++dy)
        for (int dx = -10; dx <= 10; ++dx) {
            int d = dx * dx + dy * dy;
            if (d >= 72 && d <= 100 && !(dx > 4 && dy < 4)) pixel(x + dx, y + dy, 0xb8b8b8, 255);
        }
    for (int dy = 0; dy < 7; ++dy)
        for (int dx = 0; dx <= dy; ++dx)
            pixel(x + 10 - dx, y - 10 + dy, 0xb8b8b8, 255);
}

static void image_rgb(const uint8_t *rgb, unsigned width, unsigned height, int x, int y)
{
    for (unsigned row = 0; row < height; ++row)
        memcpy(canvas + ((y + row) * CANVAS_WIDTH + x) * 3, rgb + row * width * 3, width * 3);
}

static void compose(ScreenPage page, unsigned width, unsigned height, const ScreenInfo *i)
{
    const unsigned white = 0xffffff, gray = 0xb8b8b8, line = 0x333333;
    const unsigned cyan = 0x00ffff, magenta = 0xff00ff, yellow = 0xffff00;
    int w = (int)logical_width, left = (w - 544) / 2, right = left + 544;
    memset(canvas, 0, sizeof(canvas));
    int title_x = left, title_top = font(28)['W' - 32].y;
    const char *title = "WiiCam";
    for (unsigned n = 0; title[n]; ++n) {
        char glyph[] = {title[n], 0};
        text(title_x, 40 - title_top, glyph, 28, n < 3 ? cyan : white);
        title_x += text_width(glyph, 28);
    }
    const char *heading = page == SCREEN_MENU        ? (i->sd ? "SD ready" : "No SD")
                          : page == SCREEN_REVIEW    ? "Photo review"
                          : page == SCREEN_NO_SIGNAL ? "No signal"
                          : i->microphone            ? "Mic test"
                          : page == SCREEN_WAITING   ? "Please wait"
                          : i->recording             ? "Recording"
                          : i->video                 ? "Video"
                                                     : "Photo";
    unsigned accent = i->recording ? magenta : cyan;
    text_top(right - text_right_edge(heading, 18), 49, heading, 18,
             page == SCREEN_MENU        ? gray
             : page == SCREEN_NO_SIGNAL ? yellow
                                        : accent);
    text_top(right - text_right_edge("v0.1.2", 15), 424, "v0.1.2", 15, gray);
    if (page == SCREEN_MENU) {
        unsigned action_fill = i->refreshing ? line : cyan;
        unsigned action_ink = i->refreshing ? 0x777777 : 0;
        round_box(left, 106, 544, 90, 3, action_fill);
        key(left + 28, 151, "A", action_fill, action_ink, action_ink);
        text_center_y(left + 55, 151, i->microphone ? "Record microphone" : "Open viewfinder", 25,
                      action_ink);
        round_box(left, 223, 544, 1, 0, line);
        round_box(left + 181, 237, 1, 59, 0, line);
        round_box(left + 362, 237, 1, 59, 0, line);
        key(left + 14, 250, "+", 0, magenta, magenta);
        text_center_y(left + 39, 250, "Mode", 18, gray);
        text_top(left, 277, i->microphone ? "Mic test" : i->video ? "Video" : "Photo", 22, white);
        if (!i->microphone) key(left + 206, 250, "1", 0, yellow, yellow);
        text_center_y(left + (i->microphone ? 194 : 232), 250, i->microphone ? "Audio" : "Size", 18,
                      gray);
        char size[40], timer[40];
        if (i->microphone)
            snprintf(size, sizeof(size), "16 kHz mono");
        else
            snprintf(size, sizeof(size), "%u x %u", width, height);
        text_top(left + 194, 277, size, i->microphone ? 18 : 22, white);
        text_center_y(left + 377, 250,
                      i->microphone ? "Recording length"
                      : i->video    ? "Recording limit"
                                    : "Left / Right",
                      18, gray);
        if (i->microphone)
            snprintf(timer, sizeof(timer), "5 seconds");
        else if (i->video)
            snprintf(timer, sizeof(timer), "60s / 128MiB");
        else if (i->timer)
            snprintf(timer, sizeof(timer), "Timer %u s", i->timer);
        else
            snprintf(timer, sizeof(timer), "Timer off");
        text_top(left + 377, 277, timer, i->video ? 18 : 22, white);
        round_box(left, 315, 544, 1, 0, line);
        key(left + 14, 346, "2", 0, i->refreshing ? yellow : gray, i->refreshing ? yellow : gray);
        text_center_y(left + 40, 346, "Refresh camera source", 20, i->refreshing ? yellow : white);
        if (i->refreshing)
            loading_circle(right - 14, 346, i->refresh_phase, yellow);
        else
            refresh_arrow(right - 14, 346);
        status_line(left, 374, 544,
                    i->status && *i->status ? i->status : "Open viewfinder to start the camera.",
                    gray, false);
        home(left, 420);
        return;
    }
    int picture = (w - 360) / 2;
    image_rgb(ui_bars, 360, 270, picture, 99);
    for (unsigned n = 0; n < 4; ++n) {
        int x = picture - 4 + (n % 2) * 368, y = 95 + (n / 2) * 278;
        int dx = n % 2 ? -1 : 1, dy = n / 2 ? -1 : 1;
        for (int at = 0; at <= 12; ++at) {
            pixel(x + dx * at, y, accent, 255);
            pixel(x + dx * at, y + dy, accent, 255);
            pixel(x, y + dy * at, accent, 255);
            pixel(x + dx, y + dy * at, accent, 255);
        }
    }
    if (i->countdown && page == SCREEN_LIVE) {
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 56; ++x)
                countdown_pixel(canvas + ((309 + y) * CANVAS_WIDTH + picture + 292 + x) * 3, x, y,
                                i->countdown);
    }
    char status[100];
    if (i->status && *i->status)
        snprintf(status, sizeof(status), "%s", i->status);
    else if (page == SCREEN_NO_SIGNAL)
        snprintf(status, sizeof(status), "%s",
                 i->recording ? "Signal lost - B stops recording" : "Waiting for camera signal");
    else if (i->recording)
        snprintf(status, sizeof(status), "REC %02u:%02u / 01:00  |  Silent video", i->seconds / 60,
                 i->seconds % 60);
    else if (i->countdown)
        snprintf(status, sizeof(status), "%u x %u  |  Timer armed  |  %s", width, height,
                 i->sd ? "SD ready" : "No SD");
    else if (i->shutter)
        snprintf(status, sizeof(status), "Taking photo...");
    else if (i->video)
        snprintf(status, sizeof(status), "%u x %u  |  Silent video  |  %s", width, height,
                 i->sd ? "SD ready" : "No SD");
    else if (i->timer)
        snprintf(status, sizeof(status), "%u x %u  |  Timer %u s  |  %s", width, height, i->timer,
                 i->sd ? "SD ready" : "No SD");
    else
        snprintf(status, sizeof(status), "%u x %u  |  Timer off  |  %s", width, height,
                 i->sd ? "SD ready" : "No SD");
    status_line(left, 384, 544, status, i->recording ? magenta : gray, true);
    if (page == SCREEN_NO_SIGNAL || page == SCREEN_WAITING)
        text_top(left, 419, "No capture", 18, gray);
    else if (!i->sd)
        text_top(left, 419, "Preview only", 18, gray);
    else if (!i->countdown) {
        key(left + 14, 429, "A", 0, cyan, cyan);
        text_center_y(left + (i->recording ? 35 : 37), 429,
                      page == SCREEN_REVIEW ? "Live view"
                      : i->recording        ? "Stop"
                      : i->video            ? "Start video"
                                            : "Take photo",
                      18, white);
    }
    int bx = left + (i->recording ? 128 : 221);
    key(bx, 429, "B", 0, magenta, magenta);
    text_center_y(bx + (i->recording ? 21 : 23), 429,
                  i->recording                    ? "Stop + menu"
                  : i->countdown || i->microphone ? "Cancel"
                                                  : "Menu",
                  18, white);
    home(left + 350, 417);
}

void screen_draw(Screen *s, ScreenPage page, const uint8_t *rgb, unsigned width, unsigned height,
                 const ScreenInfo *info)
{
    if (!s || !s->pixels || !info || !s->width || s->width > 864 || (s->width & 1) || !s->height
        || s->height > 576 || s->stride < s->width || (s->stride & 1))
        return;
    logical_width = s->widescreen ? 854 : 640;
    char key_string[256];
    snprintf(key_string, sizeof(key_string), "%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u:%s",
             page, width, height, s->width, s->height, logical_width, info->sd, info->video,
             info->recording, info->shutter, info->timer, info->countdown, info->seconds,
             info->refreshing, info->refresh_phase, info->microphone,
             info->status ? info->status : "");
    if (strcmp(cached_key, key_string)) {
        compose(page, width, height, info);
        for (unsigned y = 0; y < s->height; ++y)
            for (unsigned x = 0; x < s->width; x += 2) {
                size_t row = (y * 480 / s->height) * CANVAS_WIDTH;
                chrome[(y * s->width + x) / 2] =
                    screen_pair(canvas + (row + x * logical_width / s->width) * 3,
                                canvas + (row + (x + 1) * logical_width / s->width) * 3);
            }
        snprintf(cached_key, sizeof(cached_key), "%s", key_string);
    }
    for (unsigned y = 0; y < s->height; ++y)
        memcpy(s->pixels + y * s->stride / 2, chrome + y * s->width / 2, s->width * 2);
    if (!rgb || !width || !height || page == SCREEN_MENU || page == SCREEN_NO_SIGNAL) return;
    unsigned left = (logical_width - 360) / 2;
    unsigned fit_width = 360, fit_height = 270;
    if ((uint64_t)width * 270 > (uint64_t)height * 360)
        fit_height = (unsigned)((uint64_t)height * 360 / width);
    else
        fit_width = (unsigned)((uint64_t)width * 270 / height);
    unsigned fit_left = left + (360 - fit_width) / 2, fit_top = 99 + (270 - fit_height) / 2;
    unsigned x0 = (fit_left * s->width / logical_width) & ~1u;
    unsigned x1 = ((fit_left + fit_width) * s->width / logical_width) & ~1u;
    unsigned y0 = fit_top * s->height / 480, y1 = (fit_top + fit_height) * s->height / 480;
    if (x1 <= x0 || y1 <= y0) return;
    unsigned view_x0 = (left * s->width / logical_width) & ~1u;
    unsigned view_x1 = ((left + 360) * s->width / logical_width) & ~1u;
    const uint8_t black[3] = {0, 0, 0};
    for (unsigned y = 99 * s->height / 480; y < 369 * s->height / 480; ++y) {
        for (unsigned x = view_x0; x < view_x1; x += 2) {
            if (x < x0 || x >= x1 || y < y0 || y >= y1) {
                s->pixels[((size_t)y * s->stride + x) / 2] = screen_pair(black, black);
                continue;
            }
            unsigned row = (y - y0) * height / (y1 - y0);
            unsigned a = (x - x0) * width / (x1 - x0), b = (x + 1 - x0) * width / (x1 - x0);
            const uint8_t *first = rgb + ((size_t)row * width + a) * 3;
            const uint8_t *second = rgb + ((size_t)row * width + b) * 3;
            uint8_t overlay[6];
            if (info->countdown && page == SCREEN_LIVE) {
                memcpy(overlay, first, 3);
                memcpy(overlay + 3, second, 3);
                int oy = (int)(y * 480 / s->height) - (int)(fit_top + fit_height) + 60;
                countdown_pixel(
                    overlay, (int)(x * logical_width / s->width) - (int)(fit_left + fit_width) + 68,
                    oy, info->countdown);
                countdown_pixel(overlay + 3,
                                (int)((x + 1) * logical_width / s->width)
                                    - (int)(fit_left + fit_width) + 68,
                                oy, info->countdown);
                first = overlay;
                second = overlay + 3;
            }
            s->pixels[((size_t)y * s->stride + x) / 2] = screen_pair(first, second);
        }
    }
}
