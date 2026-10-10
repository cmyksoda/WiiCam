/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "av_recorder.h"
#include "frame_limits.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static AudioStream stream;
static AvRecorder recorder;
static uint8_t cached[WC04_FRAME_CAPACITY];
static uint8_t pcm[AUDIO_BLOCK_BYTES];
static const uint8_t first[] = {255, 216, 1, 255, 217};
static const uint8_t second[] = {255, 216, 2, 3, 255, 217};

static uint32_t le32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void feed(unsigned bytes)
{
    assert(!(bytes % 32));
    for (unsigned n = 0; n < bytes; n += 32)
        assert(audio_stream_packet(&stream, pcm, 32, 192, stream.bytes / 32));
}

static void inspect(FILE *file, unsigned frames, const uint8_t *tags)
{
    assert(!fflush(file));
    assert(!fseek(file, 0, SEEK_END));
    long size = ftell(file);
    assert(size >= 324);
    uint8_t *data = malloc((size_t)size);
    assert(data);
    rewind(file);
    assert(fread(data, 1, (size_t)size, file) == (size_t)size);
    assert(!memcmp(data, "RIFF", 4) && le32(data + 4) == (unsigned)size - 8);
    assert(!memcmp(data + 8, "AVI ", 4) && le32(data + 48) == frames && le32(data + 56) == 2);
    assert(le32(data + 32) == 100000 && le32(data + 128) == 1 && le32(data + 132) == 10);
    assert(!memcmp(data + 232, "auds", 4) && le32(data + 252) == 2 && le32(data + 256) == 32000);
    assert(le32(data + 264) == frames * 1600 && le32(data + 276) == 2);
    assert(le32(data + 300) == 16000 && le32(data + 304) == 32000 && data[308] == 2
           && data[310] == 16);
    unsigned index = 320 + le32(data + 316), at = 324;
    assert(index + 8 + frames * 32 == (unsigned)size && !memcmp(data + index, "idx1", 4));
    for (unsigned n = 0; n < frames * 2; n++) {
        assert(at + 8 < index);
        const uint8_t *entry = data + index + 8 + n * 16;
        const char *id = n % 2 ? "01wb" : "00dc";
        assert(!memcmp(data + at, id, 4) && !memcmp(entry, id, 4));
        unsigned length = le32(data + at + 4);
        assert(le32(entry + 8) + 320 == at && le32(entry + 12) == length);
        if (n % 2) {
            assert(length == 3200);
            assert(!memcmp(data + at + 8, pcm, 3200));
        } else {
            assert(length == 5 || length == 6);
            if (tags) assert(data[at + 10] == tags[n / 2]);
        }
        at += 8 + length + (length & 1);
    }
    assert(at == index);
    free(data);
}

static void *producer(void *unused)
{
    (void)unused;
    uint8_t data[32];
    for (unsigned n = 0; n < 100000; n++) {
        while (audio_stream_position(&stream) - atomic_load(&stream.consumed)
               > AUDIO_RING_BYTES - 32)
            sched_yield();
        memset(data, (uint8_t)n, sizeof(data));
        assert(audio_stream_packet(&stream, data, 32, 192, n));
    }
    return NULL;
}

static void ring_tests(void)
{
    audio_stream_init(&stream, MIC_RATE_UNREADABLE);
    for (unsigned n = 0; n < 4; n++)
        assert(audio_stream_packet(&stream, (void *)1, 4096, 192, n));
    assert(!audio_stream_position(&stream) && stream.rejected == 4);
    assert(!audio_stream_packet(&stream, (void *)1, 4096, 192, 4));
    audio_stream_init(&stream, MIC_RATE_OK);
    assert(!audio_stream_packet(&stream, pcm, 31, 192, 0));
    audio_stream_init(&stream, MIC_RATE_OK);
    assert(audio_stream_packet(&stream, NULL, 0, 192, 0));
    assert(!audio_stream_packet(&stream, NULL, 2, 192, 0));
    audio_stream_init(&stream, MIC_RATE_OK);
    feed(AUDIO_RING_BYTES);
    assert(!audio_stream_packet(&stream, pcm, 2, 192, 0)
           && audio_stream_position(&stream) == AUDIO_RING_BYTES);
    audio_stream_init(&stream, MIC_RATE_OK);
    atomic_store(&stream.produced, UINT32_MAX - 31);
    atomic_store(&stream.consumed, UINT32_MAX - 31);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, producer, NULL));
    uint8_t data[32];
    for (unsigned n = 0; n < 100000; n++) {
        while (!audio_stream_read(&stream, data, sizeof(data)))
            sched_yield();
        for (unsigned b = 0; b < 32; b++)
            assert(data[b] == (uint8_t)n);
    }
    assert(!pthread_join(thread, NULL));
    assert(audio_stream_position(&stream) == atomic_load(&stream.consumed));
    assert(!atomic_load(&stream.failed) && audio_stream_valid(&stream));
    stream.last_ms *= 2;
    assert(!audio_stream_valid(&stream));
    audio_stream_init(&stream, MIC_RATE_FAILED);
    assert(!audio_stream_packet(&stream, pcm, 32, 192, 0));
}

typedef struct {
    FILE *backing;
    unsigned writes, fail, closes;
    bool seek, close;
} Fault;

static ssize_t fault_write(void *p, const char *data, size_t size)
{
    Fault *f = p;
    if (++f->writes == f->fail) {
        errno = ENOSPC;
        return -1;
    }
    return (ssize_t)fwrite(data, 1, size, f->backing);
}

static int fault_seek(void *p, off64_t *offset, int whence)
{
    Fault *f = p;
    if (f->seek || fseeko(f->backing, *offset, whence)) {
        errno = EIO;
        return -1;
    }
    *offset = ftello(f->backing);
    return 0;
}

static int fault_close(void *p)
{
    Fault *f = p;
    ++f->closes;
    int result = fclose(f->backing);
    return f->close ? -1 : result;
}

static void start(const char *directory)
{
    audio_stream_init(&stream, MIC_RATE_OK);
    av_recorder_init(&recorder, &stream, cached);
    assert(av_recorder_start(&recorder, directory, 320, 240, first, sizeof(first)));
}

int main(void)
{
    memset(pcm, 0x21, sizeof(pcm));
    ring_tests();
    char directory[] = "build/av-test-XXXXXX";
    assert(mkdtemp(directory));
    start(directory);
    feed(3200);
    assert(av_recorder_frame(&recorder, second, sizeof(second), audio_stream_position(&stream)));
    feed(9600);
    assert(av_recorder_pump(&recorder, audio_stream_position(&stream)));
    assert(recorder.avi.frames == 4 && recorder.repeated == 2);
    feed(1600);
    assert(av_recorder_stop(&recorder, audio_stream_position(&stream), true));
    FILE *f = fopen(recorder.path, "rb");
    assert(f);
    uint8_t tags[] = {1, 2, 2, 2};
    inspect(f, 4, tags);
    fclose(f);
    unlink(recorder.path);
    start(directory);
    feed(6400);
    assert(av_recorder_pump(&recorder, audio_stream_position(&stream)));
    assert(av_recorder_frame(&recorder, second, sizeof(second), 3200));
    feed(3200);
    assert(av_recorder_stop(&recorder, audio_stream_position(&stream), true));
    f = fopen(recorder.path, "rb");
    assert(f);
    uint8_t stale[] = {1, 1, 1};
    inspect(f, 3, stale);
    fclose(f);
    unlink(recorder.path);
    start(directory);
    feed(3200);
    assert(!av_recorder_stop(&recorder, audio_stream_position(&stream), false));
    assert(access(recorder.path, F_OK) != 0);
    start(directory);
    assert(!av_recorder_stop(&recorder, 0, true) && access(recorder.path, F_OK) != 0);
    start(directory);
    for (unsigned n = 0; n < 600; n++) {
        feed(3200);
        assert(av_recorder_pump(&recorder, audio_stream_position(&stream)));
    }
    feed(3200);
    assert(!av_recorder_pump(&recorder, audio_stream_position(&stream)) && recorder.limited);
    assert(av_recorder_stop(&recorder, audio_stream_position(&stream), true)
           && recorder.avi.frames == 600);
    unlink(recorder.path);
    start(directory);
    recorder.avi.bytes = AVI_MAX_BYTES - 324 - 8 - 32 - 16 - 3200 - 6;
    assert(av_avi_has_space(&recorder.avi, 5));
    assert(!av_avi_has_space(&recorder.avi, 7));
    assert(!av_avi_has_space(&recorder.avi, WC04_FRAME_CAPACITY + 1));
    assert(!av_recorder_stop(&recorder, 0, false));
    for (unsigned point = 1; point <= 7; point++) {
        start(directory);
        feed(3200);
        assert(av_recorder_pump(&recorder, audio_stream_position(&stream)));
        assert(!fflush(recorder.avi.file));
        Fault fault = {
            .backing = recorder.avi.file, .fail = point, .seek = point == 5, .close = point == 6};
        recorder.avi.file =
            fopencookie(&fault, "w",
                        (cookie_io_functions_t){
                            .write = fault_write, .seek = fault_seek, .close = fault_close});
        assert(recorder.avi.file);
        setbuf(recorder.avi.file, NULL);
        if (point == 7) {
            atomic_store(&stream.failed, true);
        }
        assert(!av_recorder_stop(&recorder, audio_stream_position(&stream), true));
        assert(recorder.failed && !recorder.saved && fault.closes == 1
               && access(recorder.path, F_OK) != 0);
    }
    for (unsigned point = 1; point <= 5; point++) {
        start(directory);
        assert(!fflush(recorder.avi.file));
        Fault fault = {.backing = recorder.avi.file, .fail = point};
        recorder.avi.file =
            fopencookie(&fault, "w",
                        (cookie_io_functions_t){
                            .write = fault_write, .seek = fault_seek, .close = fault_close});
        assert(recorder.avi.file);
        setbuf(recorder.avi.file, NULL);
        feed(3200);
        assert(!av_recorder_pump(&recorder, audio_stream_position(&stream)));
        assert(!av_recorder_stop(&recorder, audio_stream_position(&stream), true)
               && fault.closes == 1);
        assert(access(recorder.path, F_OK) != 0);
    }
    assert(!rmdir(directory));
    puts(
        "PASS: A/V ring bounds, rejected packets, concurrent unsigned wrap, rate guard, sample-clock holds/stale frames, interleaving/indexes, 60s/128MiB limits and write/index/header/seek/close cleanup");
}
