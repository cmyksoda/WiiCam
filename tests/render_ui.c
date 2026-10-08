/* SPDX-License-Identifier: GPL-3.0-only */
#include "screen.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
static uint32_t framebuffer[864*576/2];
static uint8_t photo[320*240*3];
static uint8_t clamp(int n) { return n < 0 ? 0 : n > 255 ? 255 : n; }
static void render(const char *name, ScreenPage page, ScreenInfo info, bool wide)
{
    Screen screen = {framebuffer, wide ? 854 : 640, 480, wide ? 854 : 640, wide};
    screen_draw(&screen, page, page == SCREEN_LIVE || page == SCREEN_REVIEW ? photo : NULL,
                320, 240, &info);
    char path[100];
    snprintf(path, sizeof(path), "build/ui-%s.ppm", name);
    FILE *f = fopen(path, "wb");
    assert(f);
    fprintf(f, "P6\n%u 480\n255\n", screen.width);
    for (unsigned i = 0; i < screen.width*480/2; ++i) {
        uint32_t pair = framebuffer[i];
        int cb = (int)((pair>>16)&255)-128, cr = (int)(pair&255)-128;
        for (unsigned p = 0; p < 2; ++p) {
            int y = (int)((pair>>(p ? 8 : 24))&255)-16;
            uint8_t rgb[] = {clamp((298*y+409*cr+128)>>8),
                            clamp((298*y-100*cb-208*cr+128)>>8), clamp((298*y+516*cb+128)>>8)};
            assert(fwrite(rgb, 1, 3, f) == 3);
        }
    }
    assert(fclose(f) == 0);
}
int main(void)
{
    for (unsigned y = 0; y < 240; ++y)
        for (unsigned x = 0; x < 320; ++x) {
            photo[(y*320+x)*3] = x*255/320;
            photo[(y*320+x)*3+1] = y*255/240;
            photo[(y*320+x)*3+2] = 144;
        }
    render("menu", SCREEN_MENU, (ScreenInfo){.sd=true}, false);
    render("no-signal", SCREEN_NO_SIGNAL, (ScreenInfo){.sd=true}, false);
    render("no-signal-wide", SCREEN_NO_SIGNAL, (ScreenInfo){.sd=true}, true);
    render("timer", SCREEN_LIVE, (ScreenInfo){.sd=true,.timer=5,.countdown=3}, false);
    render("video", SCREEN_LIVE, (ScreenInfo){.sd=true,.video=true,.recording=true,.seconds=15}, false);
    render("no-sd", SCREEN_LIVE, (ScreenInfo){0}, false);
    render("review", SCREEN_REVIEW, (ScreenInfo){.sd=true,.status="Saved WC040023.JPG"}, false);
    render("write-error", SCREEN_MENU, (ScreenInfo){.sd=true,.video=true,.status="Video not saved - check SD and log"}, false);
    puts("PASS: UI renders ready for host visual review");
}
