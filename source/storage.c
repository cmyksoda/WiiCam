/* SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include "storage.h"
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

FILE *storage_create(const char *directory, const char *prefix, const char *extension, char *path,
                     size_t capacity)
{
    for (unsigned n = 1; n <= 9999; ++n) {
        int length = snprintf(path, capacity, "%s/%s%04u.%s", directory, prefix, n, extension);
        if (length < 0 || (size_t)length >= capacity) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);
        if (fd < 0) {
            if (errno == EEXIST) continue;
            return NULL;
        }
        FILE *file = fdopen(fd, "wb");
        if (file) return file;
        close(fd);
        unlink(path);
        return NULL;
    }
    errno = ENOSPC;
    return NULL;
}

bool storage_finish(FILE *file, const char *path, bool success)
{
    if (!file) return false;
    if (fflush(file) != 0 || ferror(file)) success = false;
    if (fclose(file) != 0) success = false;
    if (!success) unlink(path);
    return success;
}
