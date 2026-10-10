/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */

#ifndef WC04_PHOTO_H
#define WC04_PHOTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

bool photo_decode(const uint8_t *jpeg, size_t length, uint8_t *rgb, unsigned width,
                  unsigned height);
bool photo_encode(FILE *file, const uint8_t *rgb, unsigned width, unsigned height);

#endif
