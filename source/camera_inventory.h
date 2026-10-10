/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef WC04_CAMERA_INVENTORY_H
#define WC04_CAMERA_INVENTORY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CAMERA_INVENTORY_LINES 128u
#define CAMERA_INVENTORY_LINE_BYTES 192u

typedef void (*CameraInventoryReport)(void *context, const char *message);

typedef struct {
    unsigned descriptors, mjpeg_frames, audio_formats, lines;
    bool malformed, truncated;
} CameraInventory;

/* Numeric descriptor inventory only. Never selects a mode or issues USB controls. */
bool camera_inventory(const uint8_t *data, size_t length, CameraInventoryReport report,
                      void *context, CameraInventory *inventory);
#endif
