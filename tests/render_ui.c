/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "screen.h"
#include "refresh_control.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static uint32_t framebuffer[864 * 576 / 2];
static uint8_t photo[640 * 480 * 3];

static uint8_t clamp(int n)
{
    return n < 0 ? 0 : n > 255 ? 255 : n;
}

static void render_size(const char *name, ScreenPage page, ScreenInfo info, bool wide,
                        unsigned width, bool picture)
{
    for (unsigned y = 0; y < width * 3 / 4; ++y)
        for (unsigned x = 0; x < width; ++x) {
            photo[(y * width + x) * 3] = x * 255 / width;
            photo[(y * width + x) * 3 + 1] = y * 255 / (width * 3 / 4);
            photo[(y * width + x) * 3 + 2] = 144;
        }
    Screen screen = {framebuffer, wide ? 854 : 640, 480, wide ? 854 : 640, wide};
    screen_draw(&screen, page, picture ? photo : NULL, width, width * 3 / 4, &info);
    char path[100];
    snprintf(path, sizeof(path), "build/ui-%s.ppm", name);
    FILE *f = fopen(path, "wb");
    assert(f);
    fprintf(f, "P6\n%u 480\n255\n", screen.width);
    for (unsigned i = 0; i < screen.width * 480 / 2; ++i) {
        uint32_t pair = framebuffer[i];
        int cb = (int)((pair >> 16) & 255) - 128, cr = (int)(pair & 255) - 128;
        for (unsigned p = 0; p < 2; ++p) {
            int y = (int)((pair >> (p ? 8 : 24)) & 255) - 16;
            uint8_t rgb[] = {clamp((298 * y + 409 * cr + 128) >> 8),
                             clamp((298 * y - 100 * cb - 208 * cr + 128) >> 8),
                             clamp((298 * y + 516 * cb + 128) >> 8)};
            assert(fwrite(rgb, 1, 3, f) == 3);
        }
    }
    assert(fclose(f) == 0);
}

static void render(const char *name, ScreenPage page, ScreenInfo info, bool wide)
{
    render_size(name, page, info, wide, 320, page == SCREEN_LIVE || page == SCREEN_REVIEW);
}

static void refresh(const char *name, RefreshResult result, unsigned phase, bool wide,
                    unsigned width)
{
    char status[80];
    refresh_status(status, sizeof(status), result, width, width * 3 / 4);
    ScreenInfo info = {.sd = true,
                       .status = status,
                       .refresh_phase = phase,
                       .refreshing = result == REFRESH_BUSY || result == REFRESH_CANCELLING};
    render_size(name, SCREEN_MENU, info, wide, width, false);
}

int main(void)
{
    render("av-menu", SCREEN_MENU, (ScreenInfo){.sd = true, .video = true, .audio = true}, false);
    render("av-live", SCREEN_LIVE, (ScreenInfo){.sd = true, .video = true, .audio = true}, false);
    render("av-recording", SCREEN_LIVE,
           (ScreenInfo){.sd = true, .video = true, .audio = true, .recording = true, .seconds = 15},
           false);
    render("av-menu-wide", SCREEN_MENU, (ScreenInfo){.sd = true, .video = true, .audio = true},
           true);
    render("av-recording-wide", SCREEN_LIVE,
           (ScreenInfo){.sd = true, .video = true, .audio = true, .recording = true, .seconds = 15},
           true);
    render("menu", SCREEN_MENU, (ScreenInfo){.sd = true}, false);
    render("no-signal", SCREEN_NO_SIGNAL, (ScreenInfo){.sd = true}, false);
    render("no-signal-wide", SCREEN_NO_SIGNAL, (ScreenInfo){.sd = true}, true);
    render("timer", SCREEN_LIVE, (ScreenInfo){.sd = true, .timer = 5, .countdown = 3}, false);
    render("video", SCREEN_LIVE,
           (ScreenInfo){.sd = true, .video = true, .recording = true, .seconds = 15}, false);
    render("no-sd", SCREEN_LIVE, (ScreenInfo){0}, false);
    render("review", SCREEN_REVIEW, (ScreenInfo){.sd = true, .status = "Saved WC040023.JPG"},
           false);
    render("write-error", SCREEN_MENU,
           (ScreenInfo){.sd = true, .video = true, .status = "Video not saved - check SD and log"},
           false);
    refresh("refresh-busy", REFRESH_BUSY, 0, false, 640);
    refresh("refresh-spin", REFRESH_BUSY, 3, false, 640);
    refresh("refresh-found-320", REFRESH_FOUND, 0, false, 320);
    refresh("refresh-found-640", REFRESH_FOUND, 0, false, 640);
    refresh("refresh-missing", REFRESH_MISSING, 0, false, 640);
    refresh("refresh-cancelling", REFRESH_CANCELLING, 5, false, 640);
    refresh("refresh-cancelled", REFRESH_CANCELLED, 0, false, 640);
    refresh("refresh-wide", REFRESH_BUSY, 2, true, 640);
    render("live", SCREEN_LIVE, (ScreenInfo){.sd = true}, false);
    render("shutter", SCREEN_LIVE, (ScreenInfo){.sd = true, .shutter = true}, false);
    render("waiting", SCREEN_WAITING, (ScreenInfo){.sd = true, .status = "Starting camera..."},
           false);
    render("signal-recording", SCREEN_NO_SIGNAL,
           (ScreenInfo){.sd = true, .video = true, .recording = true}, false);
    render("menu-no-sd", SCREEN_MENU, (ScreenInfo){.status = "No SD - preview only"}, false);
    render("menu-timer-10", SCREEN_MENU, (ScreenInfo){.sd = true, .timer = 10}, false);
    render_size("live-640", SCREEN_LIVE, (ScreenInfo){.sd = true}, false, 640, true);
    render_size("menu-wide", SCREEN_MENU, (ScreenInfo){.sd = true}, true, 640, false);
    const ScreenInfo clip = {.sd = true, .video = true, .audio = true, .can_play = true,
                             .can_delete = true, .status = "Saved AV0005.AVI"};
    render("video-review", SCREEN_REVIEW, clip, false);
    render("video-review-wide", SCREEN_REVIEW, clip, true);
    ScreenInfo confirm = clip;
    confirm.confirm_delete = true;
    confirm.status = "Press - again to delete AV0005.AVI";
    render("video-review-confirm", SCREEN_REVIEW, confirm, false);
    render("video-playing", SCREEN_REVIEW,
           (ScreenInfo){.sd = true, .video = true, .audio = true, .playing = true,
                        .status = "AV0005.AVI  00:03 / 00:10"},
           false);
    render_size("video-deleted", SCREEN_REVIEW,
                (ScreenInfo){.sd = true, .video = true, .status = "Deleted AV0005.AVI"}, false, 320,
                false);
    render("photo-review", SCREEN_REVIEW,
           (ScreenInfo){.sd = true, .can_delete = true, .status = "Saved WC040023.JPG"}, false);
    ScreenInfo photo_confirm = {.sd = true, .can_delete = true, .confirm_delete = true,
                                .status = "Press - again to delete WC040023.JPG"};
    render("photo-review-confirm", SCREEN_REVIEW, photo_confirm, false);
    for (unsigned n = 1; n <= 10; ++n) {
        char name[32];
        snprintf(name, sizeof(name), "countdown-%u", n);
        render(name, SCREEN_LIVE, (ScreenInfo){.sd = true, .timer = 10, .countdown = n}, false);
    }
    puts("PASS: UI renders ready for host visual review");
}
