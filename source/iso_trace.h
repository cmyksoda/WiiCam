/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */
#ifndef WC04_ISO_TRACE_H
#define WC04_ISO_TRACE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    ISO_TRACE_REQUESTS = 4,
    ISO_TRACE_SIZES = 32,
    ISO_TRACE_HEADER = 24,
    ISO_TRACE_EVENTS = 2,
    ISO_TRACE_NONE = 4
};

typedef struct {
    int32_t video_id, audio_id;
    unsigned video_interface, audio_interface, endpoint, capacity, interval, tick_hz;
    int ios, revision;
    bool av;
} IsoTraceContext;

typedef struct {
    const void *request, *slot, *header;
    const uint16_t *sizes;
    uintptr_t vector_address[4];
    uint32_t vector_length[4];
    int32_t device;
    unsigned endpoint, packets, length, in_count, io_count;
} IsoTraceWire;

/* Begin/dump require all earlier requests drained; queue_done seals generations. */
void iso_trace_begin(const IsoTraceContext *context, uint64_t (*clock)(void));
bool iso_trace_wants(int32_t device, unsigned endpoint);
unsigned iso_trace_submit(const IsoTraceWire *wire);
void iso_trace_submit_result(unsigned token, int result);
void iso_trace_callback(const void *request, int result);
void iso_trace_consume(const void *slot, const uint16_t *sizes);
void iso_trace_queue_done(void);
void iso_trace_alternate(unsigned alternate, int result);
unsigned iso_trace_cancel_begin(void);
void iso_trace_cancel_end(unsigned token, int result);
void iso_trace_dump(void (*line)(const char *));
size_t iso_trace_storage_bytes(void);
#endif
