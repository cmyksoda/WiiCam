/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 cmyksoda
 */

#ifndef WC04_UVC_H
#define WC04_UVC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WC04_FRAME_CAPACITY (640u * 480u * 2u)
#define WC04_MAX_ALTS 16
#define UVC_PROBE_LENGTH 26

enum {
    UVC_SET_CUR = 0x01,
    UVC_GET_CUR = 0x81,
    UVC_GET_MAX = 0x83,
    UVC_GET_DEF = 0x87,
    UVC_VS_PROBE = 0x0100,
    UVC_VS_COMMIT = 0x0200
};

/* Return payload bytes transferred, excluding the USB setup packet. */
typedef int (*UvcControlTransfer)(void *context, uint8_t request,
                                  uint16_t selector, uint8_t data[UVC_PROBE_LENGTH]);

typedef struct {
    uint8_t number, endpoint;
    uint16_t capacity;
} UvcAlternate;

typedef struct {
    uint8_t configuration, interface, format, frame, endpoint;
    uint16_t width, height, version;
    uint32_t interval, max_frame;
    UvcAlternate alts[WC04_MAX_ALTS];
    size_t alt_count;
} UvcMode;

typedef struct {
    unsigned packets, bytes, empty, header_only, payload_packets, payload_bytes;
    unsigned bad_headers, errors, eof, fid_changes, overflow;
    unsigned missing_eof, empty_frames, invalid_jpeg, complete;
} UvcStats;

typedef struct {
    uint8_t *data;
    size_t capacity, size;
    int fid;
    bool discard, ended;
    unsigned dropped;
    UvcStats stats;
} UvcFrame;

uint16_t uvc_le16(const uint8_t *p);
uint32_t uvc_le32(const uint8_t *p);
void uvc_put32(uint8_t *p, uint32_t value);
bool uvc_parse(const uint8_t *data, size_t length, unsigned width,
               unsigned height, UvcMode *mode);
bool uvc_alternate(const UvcMode *mode, uint32_t payload, UvcAlternate *alt);
void uvc_probe_select(uint8_t data[UVC_PROBE_LENGTH], const UvcMode *mode);
bool uvc_negotiate(const UvcMode *mode, UvcAlternate *alt,
                   uint8_t data[UVC_PROBE_LENGTH], UvcControlTransfer transfer,
                   void *context);
void uvc_frame_init(UvcFrame *frame, uint8_t *data, size_t capacity);
bool uvc_packet(UvcFrame *frame, const uint8_t *packet, size_t length);

#endif
