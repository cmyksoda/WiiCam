/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "avi.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

static unsigned le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
}

static ssize_t broken_write(void *p, const char *data, size_t size)
{
    (void)p;
    (void)data;
    (void)size;
    errno = ENOSPC;
    return -1;
}

int main(void)
{
    Avi a;
    const uint8_t jpeg[] = {255, 216, 42, 255, 217};
    for (unsigned repeat = 0; repeat < 10; ++repeat) {
        FILE *f = tmpfile();
        assert(f && avi_begin(&a, f, 320, 240));
        assert(!avi_finish(&a, 1000));
        assert(!avi_append(&a, jpeg, 3));
        assert(avi_append(&a, jpeg, sizeof(jpeg)));
        assert(avi_append(&a, jpeg, sizeof(jpeg)));
        assert(avi_finish(&a, 2000));
        assert(!avi_finish(&a, 2000));
        assert(!avi_append(&a, jpeg, sizeof(jpeg)));
        rewind(f);
        uint8_t data[292];
        assert(fread(data, 1, sizeof(data), f) == sizeof(data));
        assert(le32(data + 4) == 284 && le32(data + 48) == 2);
        assert(le32(data + 128) == 2000 && le32(data + 132) == 2000);
        assert(le32(data + 216) == 32);
        assert(!memcmp(data + 252, "idx1", 4));
        assert(le32(data + 268) == 4 && le32(data + 284) == 18);
        assert(data[237] == 0 && data[251] == 0);
        fclose(f);
    }
    FILE *f = tmpfile();
    assert(f && avi_begin(&a, f, 640, 480));
    a.bytes = AVI_MAX_BYTES - 250;
    assert(!avi_has_space(&a, 100));
    a.bytes = 0;
    a.frames = AVI_MAX_FRAMES;
    assert(!avi_has_space(&a, 100));
    fclose(f);
    f = fopencookie(NULL, "w", (cookie_io_functions_t){.write = broken_write});
    assert(f);
    setbuf(f, NULL);
    assert(!avi_begin(&a, f, 320, 240));
    assert(!avi_finish(&a, 1000));
    fclose(f);
    puts(
        "PASS: AVI repeated start/stop, empty clip, bounds, odd padding, index, size limit, SD write failure");
}
