/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 cmyksoda
 */

#include "log.h"
#include <inttypes.h>
#include <string.h>

void log_message(FILE *file, time_t wall_time, uint64_t elapsed_ms, const char *text)
{
    char timestamp[32] = "clock unavailable";
    struct tm *local = wall_time == (time_t)-1 ? NULL : localtime(&wall_time);
    if (local)
        strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", local);
    while (*text) {
        const char *end = strchr(text, '\n');
        size_t length = end ? (size_t)(end - text) : strlen(text);
        if (length) {
            fprintf(file, "[%s +%" PRIu64 ".%03u] ", timestamp,
                    elapsed_ms / 1000, (unsigned)(elapsed_ms % 1000));
            fwrite(text, 1, length, file);
        }
        fputc('\n', file);
        text += length + (end != NULL);
    }
}
