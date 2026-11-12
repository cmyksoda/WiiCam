/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WIICAM_MJPEG_H
#define WIICAM_MJPEG_H
#include <stddef.h>
#include <stdint.h>

#define MJPEG_DHT_BYTES 420u

/* The JPEG standard's Huffman tables as one DHT segment, the ones UVC MJPEG implies when absent. */
extern const uint8_t mjpeg_dht[MJPEG_DHT_BYTES];

/* Offset of the SOS marker when no DHT comes before it, else 0. Unparseable frames also give 0. */
size_t mjpeg_dht_insert_at(const uint8_t *jpeg, size_t length);

/* Where the FFD9 belongs: the Photo Channel refuses bytes between the last MCU and it. Unsure
 * walks cut nothing (length - 2). The frame must end in FFD9. */
size_t mjpeg_scan_end(const uint8_t *jpeg, size_t length);

enum { MJPEG_PARTS = 4 };

typedef struct {
    const uint8_t *data;
    size_t length;
} MjpegPart;

/* Pieces to store for a frame: the DHT when missing, the scan up to mjpeg_scan_end, then FFD9. */
size_t mjpeg_frame_parts(const uint8_t *jpeg, size_t length, MjpegPart parts[MJPEG_PARTS]);
#endif
