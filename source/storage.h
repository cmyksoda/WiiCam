/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WC04_STORAGE_H
#define WC04_STORAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
FILE *storage_create(const char *directory, const char *prefix, const char *extension, char *path,
                     size_t capacity);
/* Only pass a path just created by storage_create. Removal of failed partial output is
 * best-effort. */
bool storage_finish(FILE *file, const char *path, bool success);
#endif
