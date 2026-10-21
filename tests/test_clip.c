/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "clip.h"
#include "frame_limits.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define PATH "build/test-clip.avi"
static AvAvi av;
static Avi silent;
static Clip clip;
static uint8_t jpeg[16], pcm[AUDIO_BLOCK_BYTES], out[WC04_FRAME_CAPACITY];
static uint8_t audio[AV_MAX_FRAMES * AUDIO_BLOCK_BYTES];

static size_t make_jpeg(unsigned n)
{
    size_t length = 5 + n % 7;
    memset(jpeg, (int)n, length);
    jpeg[0] = 0xff;
    jpeg[1] = 0xd8;
    jpeg[length - 2] = 0xff;
    jpeg[length - 1] = 0xd9;
    return length;
}

static void make_pcm(unsigned n)
{
    for (unsigned i = 0; i < AUDIO_BLOCK_BYTES; ++i)
        pcm[i] = (uint8_t)(n * 7 + i);
}

static void write_av(unsigned frames)
{
    FILE *file = fopen(PATH, "w+b");
    assert(file && av_avi_begin(&av, file, 640, 480));
    for (unsigned n = 0; n < frames; ++n) {
        size_t length = make_jpeg(n);
        make_pcm(n);
        assert(av_avi_pair(&av, jpeg, length, pcm));
    }
    assert(av_avi_finish(&av) && fclose(file) == 0);
}

static void poke(long offset, const void *data, size_t length)
{
    FILE *file = fopen(PATH, "r+b");
    assert(file && fseek(file, offset, SEEK_SET) == 0);
    assert(fwrite(data, 1, length, file) == length && fclose(file) == 0);
}

static void truncate_to(long size)
{
    static uint8_t copy[1 << 20];
    FILE *file = fopen(PATH, "rb");
    assert(file);
    size_t read = fread(copy, 1, sizeof(copy), file);
    assert(fclose(file) == 0 && (long)read > size);
    file = fopen(PATH, "wb");
    assert(file && fwrite(copy, 1, (size_t)size, file) == (size_t)size && fclose(file) == 0);
}

static void test_sound_clip(void)
{
    write_av(23);
    assert(clip_open(&clip, PATH));
    assert(clip.width == 640 && clip.height == 480 && clip.frame_us == 100000);
    assert(clip.frames == 23 && clip.blocks == 23 && clip.movi == 320);
    for (unsigned n = 23; n-- > 0;) {
        size_t length = 0, expected = make_jpeg(n);
        assert(clip_frame(&clip, n, out, sizeof(out), &length));
        assert(length == expected && !memcmp(out, jpeg, length));
    }
    size_t length;
    assert(!clip_frame(&clip, 23, out, sizeof(out), &length));
    assert(!clip_frame(&clip, 0, out, 4, &length));
    assert(clip_audio(&clip, audio, 23 * AUDIO_BLOCK_BYTES - 1) == 0);
    assert(clip_audio(&clip, audio, sizeof(audio)) == 23 * AUDIO_BLOCK_BYTES);
    for (unsigned n = 0; n < 23; ++n) {
        make_pcm(n);
        assert(!memcmp(audio + n * AUDIO_BLOCK_BYTES, pcm, AUDIO_BLOCK_BYTES));
    }
    clip_close(&clip);
    clip_close(&clip);
    assert(!clip_frame(&clip, 0, out, sizeof(out), &length));
}

static void test_silent_clip(void)
{
    FILE *file = fopen(PATH, "w+b");
    assert(file && avi_begin(&silent, file, 320, 240));
    for (unsigned n = 0; n < 9; ++n) {
        size_t length = make_jpeg(n);
        assert(avi_append(&silent, jpeg, length));
    }
    assert(avi_finish(&silent, 2700) && fclose(file) == 0);
    assert(clip_open(&clip, PATH));
    assert(clip.width == 320 && clip.height == 240 && clip.frame_us == 300000);
    assert(clip.frames == 9 && !clip.blocks && clip.movi == 220);
    size_t length;
    assert(clip_frame(&clip, 8, out, sizeof(out), &length) && length == make_jpeg(8));
    assert(!memcmp(out, jpeg, length));
    assert(clip_audio(&clip, audio, sizeof(audio)) == 0);
    clip_close(&clip);
}

static void test_damaged_clips(void)
{
    assert(!clip_open(&clip, "build/no-such-clip.avi") && !clip.file);
    write_av(4);
    poke(0, "RIFX", 4);
    assert(!clip_open(&clip, PATH) && !clip.file);
    /* An unfinished recording has no index. */
    write_av(4);
    long idx1 = 324 + (long)av.bytes;
    truncate_to(idx1);
    assert(!clip_open(&clip, PATH));
    /* Index entries must stay inside the file. */
    write_av(4);
    const uint8_t far[4] = {0, 0, 0, 0x7f};
    poke(idx1 + 8 + 16 + 8, far, sizeof(far));
    assert(!clip_open(&clip, PATH));
    /* Audio blocks must match the frame count. */
    write_av(4);
    poke(idx1 + 8 + 3 * 16, "JUNK", 4);
    assert(!clip_open(&clip, PATH));
    /* A chunk that no longer matches the index fails alone. */
    write_av(4);
    poke(320 + (long)av.offsets[2], "01dc", 4);
    assert(clip_open(&clip, PATH));
    size_t length;
    assert(clip_frame(&clip, 0, out, sizeof(out), &length));
    assert(!clip_frame(&clip, 1, out, sizeof(out), &length));
    assert(clip_frame(&clip, 2, out, sizeof(out), &length));
    clip_close(&clip);
    write_av(4);
    const uint8_t longer[4] = {7, 0, 0, 0};
    poke(320 + (long)av.offsets[2] + 4, longer, sizeof(longer));
    assert(clip_open(&clip, PATH));
    assert(!clip_frame(&clip, 1, out, sizeof(out), &length));
    clip_close(&clip);
    write_av(4);
    poke(320 + (long)av.offsets[1], "01wc", 4);
    assert(clip_open(&clip, PATH) && clip_audio(&clip, audio, sizeof(audio)) == 0);
    clip_close(&clip);
}

static void test_mutations(void)
{
    static uint8_t original[1 << 16];
    write_av(6);
    FILE *file = fopen(PATH, "rb");
    assert(file);
    size_t size = fread(original, 1, sizeof(original), file);
    assert(fclose(file) == 0 && size < sizeof(original));
    uint32_t seed = 12345;
    unsigned opened = 0;
    for (unsigned round = 0; round < 20000; ++round) {
        file = fopen(PATH, "wb");
        assert(file && fwrite(original, 1, size, file) == size && fclose(file) == 0);
        for (unsigned flips = 1 + round % 4; flips--;) {
            seed = seed * 1103515245 + 12345;
            /* Headers and the index carry every offset the reader trusts. */
            long at = (seed >> 8) % 2 ? (long)((seed >> 9) % 324)
                                      : 324 + (long)av.bytes + (long)((seed >> 9) % (6 * 32 + 8));
            uint8_t value = (uint8_t)(seed >> 20);
            poke(at, &value, 1);
        }
        if (!clip_open(&clip, PATH)) continue;
        ++opened;
        assert(clip.frames <= AVI_MAX_FRAMES && clip.blocks <= AV_MAX_FRAMES);
        for (unsigned n = 0; n < clip.frames; ++n) {
            size_t length = 0;
            if (clip_frame(&clip, n, out, 64, &length)) assert(length <= 64);
        }
        size_t bytes = clip_audio(&clip, audio, sizeof(audio));
        assert(bytes == 0 || bytes == (size_t)clip.blocks * AUDIO_BLOCK_BYTES);
        clip_close(&clip);
    }
    assert(opened > 1000);
}

/* Optional arguments: real WiiCam AVIs, checked frame by frame. */
static void check_recording(const char *path)
{
    assert(clip_open(&clip, path));
    for (unsigned n = 0; n < clip.frames; ++n) {
        size_t length;
        assert(clip_frame(&clip, n, out, sizeof(out), &length));
        assert(out[0] == 0xff && out[1] == 0xd8 && out[length - 2] == 0xff
               && out[length - 1] == 0xd9);
    }
    size_t bytes = clip_audio(&clip, audio, sizeof(audio));
    assert(bytes == (size_t)clip.blocks * AUDIO_BLOCK_BYTES);
    printf("PASS: %s %ux%u, %u frames at %u us, %zu PCM bytes\n", path, clip.width, clip.height,
           clip.frames, clip.frame_us, bytes);
    clip_close(&clip);
}

int main(int argc, char **argv)
{
    test_sound_clip();
    test_silent_clip();
    test_damaged_clips();
    test_mutations();
    remove(PATH);
    for (int n = 1; n < argc; ++n)
        check_recording(argv[n]);
    puts("PASS: clip reader sound/silent playback data, bounds, damaged-file rejection and 20000 "
         "header/index mutations");
}
