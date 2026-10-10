/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "av_avi.h"
#include "photo.h"
#include "video_modes.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
static uint8_t pixels[848 * 480 * 3];
static uint8_t pcm[AUDIO_BLOCK_BYTES];
static AvAvi avi;

int main(void)
{
    const VideoSize modes[] = {{320, 240}, {640, 480}, {640, 400}, {848, 480},
                               {352, 288}, {176, 144}, {160, 120}};
    for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
        unsigned w = modes[m].width, h = modes[m].height;
        char path[100];
        snprintf(path, sizeof(path), "build/test-av-%ux%u.avi", w, h);
        FILE *file = fopen(path, "wb+");
        assert(file);
        assert(av_avi_begin(&avi, file, w, h));
        for (unsigned frame = 0; frame < 20; frame++) {
            for (unsigned y = 0; y < h; y++)
                for (unsigned x = 0; x < w; x++) {
                    unsigned at = (y * w + x) * 3;
                    pixels[at] = (uint8_t)(frame * 12);
                    pixels[at + 1] = (uint8_t)(x * 255 / w);
                    pixels[at + 2] = (uint8_t)(y * 255 / h);
                }
            char *jpeg = NULL;
            size_t length = 0;
            FILE *memory = open_memstream(&jpeg, &length);
            assert(memory);
            assert(photo_encode(memory, pixels, w, h) && !fclose(memory));
            /* Alternating full and abbreviated MJPEG exercises camera-style missing DHT. */
            if (frame & 1) {
                size_t in = 2, out = 2;
                while (in + 4 < length && ((uint8_t *)jpeg)[in + 1] != 0xda) {
                    assert((uint8_t)jpeg[in] == 0xff);
                    size_t n = ((unsigned)(uint8_t)jpeg[in + 2] << 8 | (uint8_t)jpeg[in + 3]) + 2;
                    assert(n >= 4 && n <= length - in);
                    if ((uint8_t)jpeg[in + 1] != 0xc4) {
                        memmove(jpeg + out, jpeg + in, n);
                        out += n;
                    }
                    in += n;
                }
                memmove(jpeg + out, jpeg + in, length - in);
                length = out + length - in;
            }
            for (unsigned sample = 0; sample < AUDIO_BLOCK_BYTES / 2; sample++) {
                uint16_t value = (uint16_t)((frame * 1600 + sample) * 37u);
                pcm[sample * 2] = (uint8_t)value;
                pcm[sample * 2 + 1] = (uint8_t)(value >> 8);
            }
            assert(av_avi_pair(&avi, (uint8_t *)jpeg, length, pcm));
            free(jpeg);
        }
        assert(av_avi_finish(&avi) && !fclose(file));
    }
    puts("PASS: seven bounded MJPEG/PCM fixtures, full/abbreviated JPEG and known PCM samples");
}
