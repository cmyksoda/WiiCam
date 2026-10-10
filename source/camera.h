/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WC04_CAMERA_H
#define WC04_CAMERA_H
#include "uvc.h"

#define CAMERA_MAX_DEVICES 32
#define CAMERA_IOS_PARAMETERS 192
#define CAMERA_CONFIG_CAPACITY 4096

typedef struct {
    int32_t id;
    uint16_t vid, pid;
    uint8_t interface;
} CameraEntry;

typedef struct {
    void *context;
    int (*open)(void *, const CameraEntry *, int32_t *handle);
    int (*parameters)(void *, int32_t handle, uint8_t data[CAMERA_IOS_PARAMETERS]);
    /* IOS58 raw completion counts include eight setup bytes. */
    int (*read)(void *, int32_t handle, uint8_t request, uint16_t value, uint16_t length,
                uint8_t *data);
    void (*close)(void *, int32_t handle);
    void (*report)(void *, const CameraEntry *, const char *message);
} CameraOps;

/* A successful return transfers ownership of one handle to the caller.
 * All rejected, successfully opened candidates are closed exactly once.
 * scratch must be IOS-aligned MEM2 on Wii; host tests use ordinary memory. */
bool camera_find(const CameraEntry *entries, size_t count, unsigned width, unsigned height,
                 uint8_t *scratch, size_t capacity, const CameraOps *ops, int32_t *handle,
                 UvcMode *mode);
#endif
