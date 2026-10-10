/* SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include "storage.h"
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static ssize_t write_failed(void *p, const char *data, size_t size)
{
    (void)p;
    (void)data;
    (void)size;
    errno = ENOSPC;
    return -1;
}

static int close_failed(void *p)
{
    (void)p;
    errno = EIO;
    return -1;
}

int main(void)
{
    char directory[] = "build/storage-test-XXXXXX", paths[10][120];
    assert(mkdtemp(directory));
    for (unsigned n = 0; n < 10; ++n) {
        FILE *f = storage_create(directory, "WC04", "JPG", paths[n], sizeof(paths[n]));
        assert(f);
        assert(fputs("existing photo", f) >= 0);
        assert(storage_finish(f, paths[n], true));
        for (unsigned j = 0; j < n; ++j)
            assert(strcmp(paths[n], paths[j]));
    }
    char path[120];
    FILE *f = storage_create(directory, "WC04", "JPG", path, sizeof(path));
    assert(f && !storage_finish(f, path, false) && access(path, F_OK) != 0);
    assert(!storage_create("/no-such-wc04-test-directory", "WC04", "JPG", path, sizeof(path)));
    assert(!storage_create(directory, "WC04", "JPG", path, 2));
    assert(errno == ENAMETOOLONG);
    f = storage_create(directory, "VID", "AVI", path, sizeof(path));
    assert(f && fclose(f) == 0);
    f = fopencookie(NULL, "w", (cookie_io_functions_t){.close = close_failed});
    assert(f && !storage_finish(f, path, true) && access(path, F_OK) != 0);
    f = storage_create(directory, "VID", "AVI", path, sizeof(path));
    assert(f && fclose(f) == 0);
    f = fopencookie(NULL, "w", (cookie_io_functions_t){.write = write_failed});
    assert(f && fputs("buffered SD write", f) >= 0);
    assert(!storage_finish(f, path, true) && access(path, F_OK) != 0);
    for (unsigned n = 0; n < 10; ++n) {
        f = fopen(paths[n], "rb");
        char content[30] = {0};
        assert(f && fread(content, 1, 14, f) == 14 && !strcmp(content, "existing photo"));
        fclose(f);
        unlink(paths[n]);
    }
    rmdir(directory);
    puts(
        "PASS: repeated exclusive captures, existing file preservation, creation/encode/flush/close errors");
}
