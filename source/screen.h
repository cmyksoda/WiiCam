/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WC04_SCREEN_H
#define WC04_SCREEN_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t *pixels;
    unsigned width, height, stride;
    bool widescreen;
} Screen;

typedef enum {
    SCREEN_MENU,
    SCREEN_WAITING,
    SCREEN_LIVE,
    SCREEN_REVIEW,
    SCREEN_NO_SIGNAL
} ScreenPage;

typedef struct {
    const char *status;
    bool sd, video, microphone, recording, shutter, refreshing;
    unsigned timer, countdown, seconds, refresh_phase;
} ScreenInfo;

/* One native-endian word becomes Y0 Cb Y1 Cr on the Wii's big-endian CPU. */
uint32_t screen_pair(const uint8_t a[3], const uint8_t b[3]);
void screen_draw(Screen *screen, ScreenPage page, const uint8_t *rgb, unsigned width,
                 unsigned height, const ScreenInfo *info);
#endif
