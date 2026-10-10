/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _GNU_SOURCE
#include "recorder.h"
#include "photo.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static Recorder r;
static uint8_t pixels[640 * 480 * 3];

static size_t without_dht(const uint8_t *src, size_t size, uint8_t *dest)
{
    size_t in = 2, out = 2;
    memcpy(dest, src, 2);
    while (in + 4 <= size) {
        assert(src[in] == 0xff);
        if (src[in + 1] == 0xda) {
            memcpy(dest + out, src + in, size - in);
            return out + size - in;
        }
        size_t length = ((size_t)src[in + 2] << 8 | src[in + 3]) + 2;
        assert(length >= 4 && length <= size - in);
        if (src[in + 1] != 0xc4) {
            memcpy(dest + out, src + in, length);
            out += length;
        }
        in += length;
    }
    abort();
}

static ssize_t broken_write(void *p, const char *data, size_t size)
{
    (void)p;
    (void)data;
    (void)size;
    errno = ENOSPC;
    return -1;
}

typedef struct {
    FILE *backing;
    unsigned writes, fail_write, closes;
    bool fail_seek;
} FaultFile;

static ssize_t fault_write(void *context, const char *data, size_t size)
{
    FaultFile *fault = context;
    if (++fault->writes == fault->fail_write) {
        errno = ENOSPC;
        return -1;
    }
    return (ssize_t)fwrite(data, 1, size, fault->backing);
}

static int fault_seek(void *context, off64_t *offset, int whence)
{
    FaultFile *fault = context;
    if (fault->fail_seek || fseeko(fault->backing, *offset, whence) != 0) {
        errno = EIO;
        return -1;
    }
    *offset = ftello(fault->backing);
    return 0;
}

static int fault_close(void *context)
{
    FaultFile *fault = context;
    ++fault->closes;
    return fclose(fault->backing);
}

int main(void)
{
    char directory[] = "build/recorder-test-XXXXXX";
    assert(mkdtemp(directory));
    recorder_init(&r);
    assert(!recorder_stop(&r, 10));
    assert(!recorder_start(&r, "/no-such-wc04-test-directory", 320, 240, 0));
    assert(r.failed);
    for (unsigned w = 320; w <= 640; w *= 2) {
        unsigned h = w * 3 / 4;
        for (size_t n = 0; n < (size_t)w * h * 3; ++n)
            pixels[n] = (uint8_t)(n / 3 / w + n / 3 % w);
        char *jpeg = NULL;
        size_t size = 0;
        FILE *f = open_memstream(&jpeg, &size);
        assert(f && photo_encode(f, pixels, w, h) && fclose(f) == 0);
        uint8_t *abbrev = malloc(size);
        assert(abbrev);
        size_t short_size = without_dht((uint8_t *)jpeg, size, abbrev);
        for (unsigned repeat = 0; repeat < 3; ++repeat) {
            assert(recorder_start(&r, directory, w, h, 1000));
            assert(!recorder_start(&r, directory, w, h, 1000));
            for (unsigned n = 0; n < 10; ++n)
                assert(recorder_frame(&r, n % 2 ? abbrev : (uint8_t *)jpeg,
                                      n % 2 ? short_size : size, 1000 + n * 200));
            assert(recorder_stop(&r, 3000));
            assert(r.saved && r.avi.frames == 10 && r.duration_ms == 2000);
            assert(!recorder_stop(&r, 3001));
            if (!repeat) {
                char path[80];
                snprintf(path, sizeof(path), "build/test-video-%u.avi", w);
                assert(rename(r.path, path) == 0);
            } else
                assert(unlink(r.path) == 0);
        }
        assert(recorder_start(&r, directory, w, h, 0));
        assert(recorder_frame(&r, (uint8_t *)jpeg, size, 0));
        assert(!recorder_frame(&r, (uint8_t *)jpeg, size, AVI_MAX_MS));
        assert(r.limited && recorder_stop(&r, AVI_MAX_MS + 20));
        assert(r.duration_ms == AVI_MAX_MS);
        assert(unlink(r.path) == 0);
        assert(recorder_start(&r, directory, w, h, 0));
        assert(!recorder_stop(&r, 5));
        assert(access(r.path, F_OK) != 0);
        assert(recorder_start(&r, directory, w, h, 0));
        assert(recorder_frame(&r, (uint8_t *)jpeg, size, 0));
        assert(fclose(r.avi.file) == 0);
        r.avi.file = fopencookie(NULL, "w", (cookie_io_functions_t){.write = broken_write});
        assert(r.avi.file);
        setbuf(r.avi.file, NULL);
        assert(!recorder_frame(&r, (uint8_t *)jpeg, size, 20));
        assert(r.failed && !recorder_stop(&r, 30));
        assert(!r.saved && !r.active && access(r.path, F_OK) != 0);
        /* Fail idx1 header, index entry, rewritten AVI header, and header seek. */
        for (unsigned fault_at = 1; fault_at <= 4; ++fault_at) {
            assert(recorder_start(&r, directory, w, h, 0));
            assert(recorder_frame(&r, (uint8_t *)jpeg, size, 10));
            assert(fflush(r.avi.file) == 0);
            FaultFile fault = {
                .backing = r.avi.file, .fail_write = fault_at, .fail_seek = fault_at == 4};
            r.avi.file =
                fopencookie(&fault, "w",
                            (cookie_io_functions_t){
                                .write = fault_write, .seek = fault_seek, .close = fault_close});
            assert(r.avi.file);
            setbuf(r.avi.file, NULL);
            assert(!recorder_stop(&r, 200));
            assert(r.failed && !r.saved && !r.active && fault.closes == 1);
            assert(access(r.path, F_OK) != 0);
        }
        assert(recorder_start(&r, directory, w, h, 1000));
        assert(!recorder_stop(&r, 1100));
        assert(!r.saved && !r.failed && !r.active);
        free(abbrev);
        free(jpeg);
    }
    assert(rmdir(directory) == 0);
    puts(
        "PASS: recorder repeated start/stop, both sizes, missing DHT, 60 s limit, empty/cancel, SD failure/recovery, index/seek/header failure, single close");
}
