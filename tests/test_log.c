/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 cmyksoda
 */

#include "log.h"
#include <assert.h>
#include <string.h>

int main(void)
{
    FILE *file = tmpfile();
    assert(file);
    log_message(file, (time_t)-1, 12345, "\nCapture started\nSecond line\n");
    log_message(file, (time_t)-1, 12346, "B pressed");
    rewind(file);
    char data[512] = {0};
    assert(fread(data, 1, sizeof(data) - 1, file) > 0);
    assert(strcmp(data, "\n[clock unavailable +12.345] Capture started\n"
                        "[clock unavailable +12.345] Second line\n"
                        "[clock unavailable +12.346] B pressed\n")
           == 0);
    fclose(file);

    file = tmpfile();
    assert(file);
    time_t now = time(NULL);
    assert(now != (time_t)-1);
    char timestamp[32], expected[128];
    assert(strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now)));
    snprintf(expected, sizeof(expected), "[%s +0.007] event\n", timestamp);
    log_message(file, now, 7, "event\n");
    rewind(file);
    memset(data, 0, sizeof(data));
    assert(fread(data, 1, sizeof(data) - 1, file) > 0);
    assert(strcmp(data, expected) == 0);
    fclose(file);
    puts("PASS: wall clock, elapsed milliseconds, multiline logs, unavailable clock");
    return 0;
}
