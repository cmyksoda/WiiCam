/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 cmyksoda
 */

#ifndef WC04_ISO_QUEUE_H
#define WC04_ISO_QUEUE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#define ISO_PACKETS 16
#define ISO_MAX_PAYLOAD 3072
/* Eight milliseconds of slots cover the observed roughly six-ms batches. */
#define ISO_QUEUE_DEPTH 4
#define ISO_DATA_BYTES (ISO_PACKETS * ISO_MAX_PAYLOAD)

typedef enum {
    ISO_RUNNING,
    ISO_PHOTO_READY,
    ISO_CANCELLED,
    ISO_EXIT,
    ISO_TIMEOUT,
    ISO_SUBMIT_FAILED,
    ISO_COMPLETION_FAILED
} IsoStop;

typedef struct {
    uint8_t *data;
    uint16_t *sizes;
    atomic_uint completed;
    int result;
    bool pending;
} IsoSlot;

typedef struct {
    IsoSlot slots[ISO_QUEUE_DEPTH];
    unsigned capacity, submitted, consumed, drained;
    int failure, drain_results[ISO_QUEUE_DEPTH];
} IsoQueue;

typedef struct {
    int (*submit)(void *context, IsoSlot *slot);
    bool (*consume)(void *context, const IsoSlot *slot);
    IsoStop (*poll)(void *context);
    void (*idle)(void *context);
    void (*stopping)(void *context, IsoStop reason, int failure);
    void (*cancel)(void *context);
} IsoHooks;

/* Banks must be aligned for IOS and remain alive until iso_queue_run returns. */
void iso_queue_init(IsoQueue *queue, uint8_t *data, uint16_t *sizes, unsigned capacity);
void iso_queue_complete(IsoSlot *slot, int result);
IsoStop iso_queue_run(IsoQueue *queue, const IsoHooks *hooks, void *context);

#endif
