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
#define UVC_MAX_INTERVALS 32
#define UVC_BOUNDARY_SAMPLES 4
#define UVC_BOUNDARY_HEAD 8
#define UVC_BOUNDARY_TAIL 16

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
    uint8_t configuration, interface, control_interface, format, frame, endpoint;
    uint16_t width, height, version;
    uint32_t interval, max_frame;
    unsigned interval_count;
    uint32_t intervals[UVC_MAX_INTERVALS], interval_min, interval_max, interval_step;
    UvcAlternate alts[WC04_MAX_ALTS];
    size_t alt_count;
    /* Populated only for UVC_LIMIT; does not change mode acceptance. */
    const char *limit_reason;
    uint32_t limit_value, limit_bound;
} UvcMode;

enum { UVC_SHORT_JPEG = 1, UVC_MISSING_SOI = 2, UVC_MISSING_EOI = 4 };

typedef struct {
    uint32_t size;
    uint8_t reasons, header_length, flags, head_length, tail_length;
    /* Raw FF D9 distance from end within last 16 bytes; -1 means not found.
     * This is a diagnostic byte pair, not a parsed or validated JPEG marker. */
    int8_t eoi_distance;
    uint8_t head[UVC_BOUNDARY_HEAD], tail[UVC_BOUNDARY_TAIL];
} UvcBoundarySample;

typedef struct {
    unsigned packets, bytes, empty, header_only, payload_packets, payload_bytes;
    unsigned bad_headers, errors, eof, fid_changes, overflow;
    unsigned missing_eof, empty_frames, invalid_jpeg, complete;
    /* Nonempty, non-discarded EOF frames only; reason counters can overlap. */
    unsigned short_jpeg, missing_soi, missing_eoi;
    unsigned accepted_mod8[8], rejected_mod8[8];
} UvcStats;

typedef struct {
    uint8_t *data;
    size_t capacity, size;
    int fid;
    bool discard, ended;
    unsigned dropped;
    UvcStats stats;
    unsigned boundary_samples;
    UvcBoundarySample boundary[UVC_BOUNDARY_SAMPLES];
} UvcFrame;

uint16_t uvc_le16(const uint8_t *p);
uint32_t uvc_le32(const uint8_t *p);
void uvc_put32(uint8_t *p, uint32_t value);
typedef enum {
    UVC_OK, UVC_MALFORMED, UVC_ASSOCIATION, UVC_VERSION,
    UVC_NO_MODE, UVC_LIMIT, UVC_TRANSPORT
} UvcParseResult;

UvcParseResult uvc_parse(const uint8_t *data, size_t length, unsigned interface,
                         unsigned width, unsigned height, UvcMode *mode);
const char *uvc_parse_description(UvcParseResult result);
int uvc_control_payload_length(int result, unsigned requested);
bool uvc_interval_supported(const UvcMode *mode, uint32_t interval);
bool uvc_alternate(const UvcMode *mode, uint32_t payload, UvcAlternate *alt);
void uvc_probe_select(uint8_t data[UVC_PROBE_LENGTH], const UvcMode *mode);
bool uvc_negotiate(const UvcMode *mode, UvcAlternate *alt,
                   uint8_t data[UVC_PROBE_LENGTH], UvcControlTransfer transfer,
                   void *context);
void uvc_frame_init(UvcFrame *frame, uint8_t *data, size_t capacity);
bool uvc_packet(UvcFrame *frame, const uint8_t *packet, size_t length);

#endif
