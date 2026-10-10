/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */

#ifndef WC04_LOG_H
#define WC04_LOG_H

#include <stdint.h>
#include <stdio.h>
#include <time.h>

void log_message(FILE *file, time_t wall_time, uint64_t elapsed_ms, const char *text);

#endif
