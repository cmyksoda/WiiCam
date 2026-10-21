/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "uvc.h"
#include "preview.h"
#include "photo.h"
#include "recorder.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct {
    uint8_t before, bytes[WC04_FRAME_CAPACITY], after;
} assembly = {.before = 0x5a, .after = 0xa5}, mailbox = {.before = 0x3c, .after = 0xc3};

static uint8_t input[WC04_FRAME_CAPACITY + 16], pixels[640 * 480 * 3];
static UvcFrame frame;
static Preview preview;
static Recorder recorder;

static bool feed(const uint8_t *bytes, size_t size, size_t stride, unsigned flags)
{
    uint8_t packet[1026];
    assert(stride && stride <= sizeof(packet) - 2);
    uvc_frame_init(&frame, assembly.bytes, sizeof(assembly.bytes));
    bool accepted = false;
    for (size_t at = 0; at < size;) {
        size_t count = size - at < stride ? size - at : stride;
        packet[0] = 2;
        packet[1] = 0x80 | (at + count == size ? flags : 0);
        memcpy(packet + 2, bytes + at, count);
        accepted = uvc_packet(&frame, packet, count + 2);
        at += count;
        assert(at == size || !accepted);
    }
    assert(assembly.before == 0x5a && assembly.after == 0xa5);
    return accepted;
}

static void test_boundaries(void)
{
    /* Exhaust all small lengths and suffixes, including unaligned zero tails. */
    for (size_t jpeg_size = 4; jpeg_size <= 40; ++jpeg_size) {
        for (unsigned padding = 0; padding <= 16; ++padding) {
            memset(input, 0x11, jpeg_size);
            input[0] = 0xff;
            input[1] = 0xd8;
            input[jpeg_size - 2] = 0xff;
            input[jpeg_size - 1] = 0xd9;
            memset(input + jpeg_size, 0, padding);
            bool expected = !padding || (padding <= 7 && !((jpeg_size + padding) & 7));
            assert(feed(input, jpeg_size + padding, 1, 2) == expected);
            if (expected) {
                assert(frame.size == jpeg_size && frame.stats.complete == 1);
                assert(frame.stats.padding_bytes == padding);
                assert(frame.stats.normalized_frames == (padding != 0));
                assert(!frame.stats.invalid_jpeg);
                uint8_t duplicate[] = {2, 0x82};
                assert(!uvc_packet(&frame, duplicate, sizeof(duplicate)));
                assert(frame.stats.complete == 1);
            } else {
                assert(frame.size == jpeg_size + padding);
                assert(frame.stats.invalid_jpeg == 1);
                assert(!frame.stats.normalized_frames && !frame.stats.padding_bytes);
            }
            for (unsigned n = 0; n < padding; ++n) {
                input[jpeg_size + n] = 0xa5;
                assert(!feed(input, jpeg_size + padding, 3, 2));
                input[jpeg_size + n] = 0;
            }
        }
    }
    /* Recorded sizes and exact tails; full physical JPEG contents are unavailable. */
    const size_t sizes[] = {8048, 11328, 11424, 11224};
    const unsigned pads[] = {6, 2, 7, 1};
    const uint8_t tails[][16] = {
        {0x81, 0x85, 0x14, 0x30, 0xdc, 0x29, 0x69, 0x08, 0xff, 0xd9, 0, 0, 0, 0, 0, 0},
        {0x82, 0x94, 0x50, 0x30, 0xeb, 0x45, 0x31, 0x7a, 0x05, 0x2d, 0x2d, 0xc0, 0xff, 0xd9, 0, 0},
        {0x8c, 0x64, 0x83, 0xfa, 0xd0, 0x3e, 0x80, 0xff, 0xd9, 0, 0, 0, 0, 0, 0, 0},
        {0x05, 0x2f, 0xe1, 0x4a, 0xc0, 0x82, 0x8c, 0x71, 0x90, 0x28, 0x61, 0x76, 0, 0xff, 0xd9, 0}};
    const uint8_t head[] = {0xff, 0xd8, 0xff, 0xc0, 0, 0x11, 8, 0};
    for (unsigned n = 0; n < 4; ++n) {
        memset(input, 0x11, sizes[n]);
        memcpy(input, head, sizeof(head));
        memcpy(input + sizes[n] - 16, tails[n], 16);
        assert(feed(input, sizes[n], 798, 2));
        assert(frame.size == sizes[n] - pads[n] && frame.stats.padding_bytes == pads[n]);
        assert(!feed(input, sizes[n], 798, 0x42));
        assert(frame.discard && frame.stats.errors == 1 && !frame.stats.normalized_frames);
        assert(!feed(input, sizes[n], 798, 0));
        uint8_t next[] = {2, 0x83, 0xff, 0xd8, 0xff, 0xd9};
        assert(uvc_packet(&frame, next, sizeof(next)));
        assert(frame.stats.missing_eof == 1 && !frame.stats.normalized_frames);
        input[0] = 0;
        assert(!feed(input, sizes[n], 798, 2) && frame.stats.invalid_jpeg == 1);
    }
    /* An earlier malformed header is not rescued by a padded EOF. */
    uvc_frame_init(&frame, assembly.bytes, sizeof(assembly.bytes));
    uint8_t bad_header[] = {2, 0x84, 0xff, 0xd8};
    uint8_t padded[] = {2, 0x82, 0xff, 0xd8, 0x11, 0xff, 0xd9, 0, 0, 0};
    assert(!uvc_packet(&frame, bad_header, sizeof(bad_header)));
    assert(!uvc_packet(&frame, padded, sizeof(padded)));
    assert(frame.stats.bad_headers == 1 && !frame.stats.normalized_frames);
    /* Internal marker, truncated EOI, and all-zero payloads still fail. */
    uint8_t internal[] = {0xff, 0xd8, 0xff, 0xd9, 0x11, 0, 0, 0};
    assert(!feed(internal, sizeof(internal), 1, 2));
    internal[3] = 0;
    internal[4] = 0;
    assert(!feed(internal, sizeof(internal), 1, 2));
    memset(internal, 0, sizeof(internal));
    assert(!feed(internal, sizeof(internal), 1, 2));
}

/* Add legal COM segments to reach an exact compressed length without changing pixels. */
static void extend_jpeg(const uint8_t *jpeg, size_t size, size_t target)
{
    assert(target >= size + 4 && target <= sizeof(input));
    memcpy(input, jpeg, 2);
    size_t at = 2, extra = target - size;
    while (extra) {
        size_t chunk = extra > 65537 ? 65537 : extra;
        if (extra > chunk && extra - chunk < 4) chunk -= 4;
        assert(chunk >= 4);
        input[at] = 0xff;
        input[at + 1] = 0xfe;
        input[at + 2] = (uint8_t)((chunk - 2) >> 8);
        input[at + 3] = (uint8_t)(chunk - 2);
        memset(input + at + 4, 0x31, chunk - 4);
        at += chunk;
        extra -= chunk;
    }
    memcpy(input + at, jpeg + 2, size - 2);
}

static void save_frame(const char *directory, unsigned width, size_t size)
{
    preview_init(&preview, mailbox.bytes, sizeof(mailbox.bytes));
    preview_shutter(&preview);
    assert(preview_offer(&preview, frame.data, frame.size));
    assert(preview_take(&preview) && preview.length == size);
    assert(!memcmp(preview.jpeg, input, size));
    assert(photo_decode(preview.jpeg, preview.length, pixels, width, width * 3 / 4));
    /* The worker decodes first, then records this exact mailbox length. */
    assert(recorder_start(&recorder, directory, width, width * 3 / 4, 0));
    assert(recorder_frame(&recorder, preview.jpeg, preview.length, 0));
    assert(recorder.avi.lengths[0] == size && recorder.avi.largest == size);
    assert(recorder_stop(&recorder, 200));
    FILE *avi = fopen(recorder.path, "rb");
    uint8_t chunk[8];
    assert(avi && fseek(avi, 224, SEEK_SET) == 0);
    assert(fread(chunk, 1, 8, avi) == 8 && !memcmp(chunk, "00dc", 4));
    assert(uvc_le32(chunk + 4) == size);
    for (size_t n = 0; n < size; ++n)
        assert(fgetc(avi) == input[n]);
    if (size & 1) assert(fgetc(avi) == 0); /* RIFF alignment is outside JPEG data. */
    assert(fread(chunk, 1, 8, avi) == 8 && !memcmp(chunk, "idx1", 4));
    assert(fclose(avi) == 0);
    if (size == WC04_FRAME_CAPACITY - 7) {
        char path[80];
        snprintf(path, sizeof(path), "build/test-framing-%u.avi", width);
        assert(rename(recorder.path, path) == 0);
    } else
        assert(unlink(recorder.path) == 0);
    FILE *photo = tmpfile();
    assert(photo && photo_encode(photo, pixels, width, width * 3 / 4));
    assert(fclose(photo) == 0);
    preview_finish(&preview, true);
    assert(preview_photo_ready(&preview));
    assert(mailbox.before == 0x3c && mailbox.after == 0xc3);
}

static void test_pipeline(void)
{
    char directory[] = "build/framing-test-XXXXXX";
    assert(mkdtemp(directory));
    for (unsigned width = 320; width <= 640; width *= 2) {
        char *jpeg = NULL;
        size_t size = 0;
        FILE *file = open_memstream(&jpeg, &size);
        for (size_t n = 0; n < (size_t)width * width * 3 / 4 * 3; ++n)
            pixels[n] = (uint8_t)(n / 7);
        assert(file && photo_encode(file, pixels, width, width * 3 / 4) && fclose(file) == 0);
        for (unsigned padding = 0; padding <= 7; ++padding) {
            size_t target = ((size + 18) & ~(size_t)7) - padding;
            extend_jpeg((uint8_t *)jpeg, size, target);
            memset(input + target, 0, padding);
            assert(feed(input, target + padding, 798, 2));
            assert(frame.size == target && frame.stats.padding_bytes == padding);
            save_frame(directory, width, target);
        }
        const size_t bounds[] = {614989, WC04_FRAME_CAPACITY - 7, WC04_FRAME_CAPACITY};
        for (unsigned n = 0; n < sizeof(bounds) / sizeof(bounds[0]); ++n) {
            size_t target = bounds[n], padded = (target + 7) & ~(size_t)7;
            extend_jpeg((uint8_t *)jpeg, size, target);
            memset(input + target, 0, padded - target);
            assert(feed(input, padded, 1024, 2));
            assert(frame.size == target);
            save_frame(directory, width, target);
        }
        extend_jpeg((uint8_t *)jpeg, size, WC04_FRAME_CAPACITY + 1);
        assert(!feed(input, WC04_FRAME_CAPACITY + 1, 1024, 2));
        assert(frame.stats.overflow == 1 && !frame.stats.normalized_frames);
        /* No post-overflow trimming: an otherwise eligible suffix is still discarded. */
        extend_jpeg((uint8_t *)jpeg, size, WC04_FRAME_CAPACITY + 1);
        memset(input + WC04_FRAME_CAPACITY + 1, 0, 7);
        assert(!feed(input, WC04_FRAME_CAPACITY + 8, 1024, 2));
        assert(frame.stats.overflow == 1);
        preview_init(&preview, mailbox.bytes, sizeof(mailbox.bytes));
        assert(!preview_offer(&preview, input, WC04_FRAME_CAPACITY + 1));
        assert(!preview_take(&preview) && !preview.length);
        assert(recorder_start(&recorder, directory, width, width * 3 / 4, 0));
        assert(!recorder_frame(&recorder, input, WC04_FRAME_CAPACITY + 1, 1));
        assert(recorder.limited && !recorder.avi.frames);
        assert(!recorder_stop(&recorder, 2) && !recorder.failed);
        /* A raw EOI pair inside a truncated COM segment is not a valid JPEG. */
        const uint8_t false_eoi[] = {0xff, 0xd8, 0xff, 0xfe, 0, 20, 0xff, 0xd9,
                                     0,    0,    0,    0,    0, 0,  0,    0};
        /* Eight zeros exceed the policy. Seven pass only the boundary gate. */
        assert(!feed(false_eoi, sizeof(false_eoi), 1, 2));
        uint8_t malformed[16] = {0xff, 0xd8, 0xff, 0xfe, 0, 20, 1, 0xff, 0xd9};
        assert(feed(malformed, sizeof(malformed), 1, 2));
        assert(frame.size == 9 && frame.stats.padding_bytes == 7);
        preview_init(&preview, mailbox.bytes, sizeof(mailbox.bytes));
        preview_shutter(&preview);
        assert(preview_offer(&preview, frame.data, frame.size));
        assert(!photo_decode(preview.jpeg, preview.length, pixels, width, width * 3 / 4));
        preview_finish(&preview, false);
        assert(!preview_photo_ready(&preview) && !preview_take(&preview));
        /* Forced terminal EOI after truncated real scan data still fails full decode. */
        memcpy(input, jpeg, size / 2);
        size_t truncated = (size / 2) & ~(size_t)7;
        input[truncated - 4] = 0xff;
        input[truncated - 3] = 0xd9;
        input[truncated - 2] = input[truncated - 1] = 0;
        assert(feed(input, truncated, 798, 2));
        assert(!photo_decode(frame.data, frame.size, pixels, width, width * 3 / 4));
        free(jpeg);
    }
    assert(rmdir(directory) == 0);
    assert(assembly.before == 0x5a && assembly.after == 0xa5);
    assert(mailbox.before == 0x3c && mailbox.after == 0xc3);
}

int main(void)
{
    test_boundaries();
    test_pipeline();
    puts(
        "PASS: bounded aligned zero padding, observed tails, errors/truncation, full JPEG decode/photo/AVI pipeline, shared cap and guards");
}
