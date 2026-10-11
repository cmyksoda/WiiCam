/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */
#include "iso_trace.h"
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

_Static_assert(ATOMIC_INT_LOCK_FREE == 2, "callback trace needs lock-free publication");

typedef struct {
    IsoTraceWire wire;
    unsigned bytes;
    uint8_t header[ISO_TRACE_HEADER], submitted[ISO_TRACE_SIZES];
    uint8_t callback[ISO_TRACE_SIZES], consumed[ISO_TRACE_SIZES];
    uintptr_t consumer_sizes;
    uint64_t submit_tick, callback_tick, consume_tick;
    uint64_t submit_cost, callback_cost, consume_cost;
    int submit_result, callback_result;
    bool submit_returned, was_consumed, sealed;
    atomic_uint completed;
} TraceRequest;

typedef struct {
    unsigned value;
    int result;
    uint64_t start, end;
    bool returned;
} TraceEvent;

static struct {
    IsoTraceContext context;
    uint64_t (*clock)(void);
    uint64_t start;
    unsigned count, alternate_count, cancel_count;
    bool accepting;
    atomic_uint enabled, pending;
    TraceRequest request[ISO_TRACE_REQUESTS];
    TraceEvent alternate[ISO_TRACE_EVENTS], cancel[ISO_TRACE_EVENTS];
} trace;

void iso_trace_begin(const IsoTraceContext *context, uint64_t (*clock)(void))
{
    memset(&trace, 0, sizeof(trace));
    atomic_init(&trace.enabled, 0);
    atomic_init(&trace.pending, 0);
    for (unsigned i = 0; i < ISO_TRACE_REQUESTS; ++i)
        atomic_init(&trace.request[i].completed, 0);
    trace.context = *context;
    trace.clock = clock;
    trace.start = clock();
    trace.accepting = true;
    atomic_store_explicit(&trace.enabled, 1, memory_order_release);
}

bool iso_trace_wants(int32_t device, unsigned endpoint)
{
    return atomic_load_explicit(&trace.enabled, memory_order_acquire) && trace.accepting
           && trace.count < ISO_TRACE_REQUESTS && device == trace.context.audio_id
           && endpoint == trace.context.endpoint;
}

unsigned iso_trace_submit(const IsoTraceWire *wire)
{
    if (!iso_trace_wants(wire->device, wire->endpoint) || !wire->sizes || !wire->header)
        return ISO_TRACE_NONE;
    uint64_t start = trace.clock();
    unsigned token = trace.count++;
    TraceRequest *r = &trace.request[token];
    r->wire = *wire;
    r->bytes = wire->packets < ISO_TRACE_SIZES / 2 ? wire->packets * 2 : ISO_TRACE_SIZES;
    memcpy(r->header, wire->header, sizeof(r->header));
    memcpy(r->submitted, wire->sizes, r->bytes);
    r->submit_tick = start - trace.start;
    r->submit_cost = trace.clock() - start;
    /* A callback can preempt the next submission, so publish each entry separately. */
    atomic_fetch_or_explicit(&trace.pending, 1u << token, memory_order_release);
    return token;
}

void iso_trace_submit_result(unsigned token, int result)
{
    if (token >= ISO_TRACE_REQUESTS) return;
    TraceRequest *r = &trace.request[token];
    r->submit_result = result;
    r->submit_returned = true;
    if (result < 0) atomic_fetch_and_explicit(&trace.pending, ~(1u << token), memory_order_release);
}

void iso_trace_callback(const void *request, int result)
{
    unsigned pending = atomic_load_explicit(&trace.pending, memory_order_acquire);
    if (!pending) return;
    for (unsigned i = 0; i < ISO_TRACE_REQUESTS; ++i) {
        if (!(pending & (1u << i))) continue;
        TraceRequest *r = &trace.request[i];
        if (r->wire.request != request) continue;
        uint64_t start = trace.clock();
        memcpy(r->callback, r->wire.sizes, r->bytes);
        r->callback_result = result;
        r->callback_tick = start - trace.start;
        r->callback_cost = trace.clock() - start;
        atomic_store_explicit(&r->completed, 1, memory_order_release);
        atomic_fetch_and_explicit(&trace.pending, ~(1u << i), memory_order_release);
        return;
    }
}

void iso_trace_consume(const void *slot, const uint16_t *sizes)
{
    if (!atomic_load_explicit(&trace.enabled, memory_order_acquire) || !trace.accepting) return;
    for (unsigned i = 0; i < trace.count; ++i) {
        TraceRequest *r = &trace.request[i];
        if (r->wire.slot != slot || r->sealed || r->was_consumed
            || !atomic_load_explicit(&r->completed, memory_order_acquire) || r->callback_result < 0)
            continue;
        uint64_t start = trace.clock();
        r->consumer_sizes = (uintptr_t)sizes;
        memcpy(r->consumed, sizes, r->bytes);
        r->was_consumed = true;
        r->consume_tick = start - trace.start;
        r->consume_cost = trace.clock() - start;
        return;
    }
}

void iso_trace_queue_done(void)
{
    trace.accepting = false;
    for (unsigned i = 0; i < trace.count; ++i)
        trace.request[i].sealed = true;
}

void iso_trace_alternate(unsigned alternate, int result)
{
    if (!atomic_load_explicit(&trace.enabled, memory_order_acquire)
        || trace.alternate_count >= ISO_TRACE_EVENTS)
        return;
    TraceEvent *e = &trace.alternate[trace.alternate_count++];
    e->value = alternate;
    e->result = result;
    e->end = trace.clock() - trace.start;
    e->returned = true;
}

unsigned iso_trace_cancel_begin(void)
{
    if (!atomic_load_explicit(&trace.enabled, memory_order_acquire)
        || trace.cancel_count >= ISO_TRACE_EVENTS)
        return ISO_TRACE_NONE;
    unsigned token = trace.cancel_count++;
    trace.cancel[token].start = trace.clock() - trace.start;
    return token;
}

void iso_trace_cancel_end(unsigned token, int result)
{
    if (token >= ISO_TRACE_EVENTS) return;
    TraceEvent *e = &trace.cancel[token];
    e->end = trace.clock() - trace.start;
    e->result = result;
    e->returned = true;
}

static void emit(void (*line)(const char *), const char *format, ...)
{
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    line(buffer);
}

static void hex(char *out, const uint8_t *bytes, unsigned count)
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < count; ++i) {
        out[2 * i] = digits[bytes[i] >> 4];
        out[2 * i + 1] = digits[bytes[i] & 15];
    }
    out[2 * count] = 0;
}

void iso_trace_dump(void (*line)(const char *))
{
    /* Refuse to log while IOS can still own any retained request. */
    if (atomic_load_explicit(&trace.pending, memory_order_acquire)
        || !atomic_exchange_explicit(&trace.enabled, 0, memory_order_acq_rel))
        return;
    const IsoTraceContext *c = &trace.context;
    emit(line,
         "ISO trace diagnostic only: mode=%s IOS=%d revision=%d requests=%u BSS=%u; "
         "raw metadata only, no audio payload; ticks relative to trace start; tick_hz=%u.\n",
         c->av ? "AV" : "mic", c->ios, c->revision, trace.count, (unsigned)sizeof(trace),
         c->tick_hz);
    emit(line,
         "ISO trace binding: video=%08lx if=%u audio=%08lx if=%u endpoint=%02x "
         "cap=%u interval=%u; first queue only, four requests maximum.\n",
         (unsigned long)(uint32_t)c->video_id, c->video_interface,
         (unsigned long)(uint32_t)c->audio_id, c->audio_interface, c->endpoint, c->capacity,
         c->interval);
    for (unsigned i = 0; i < trace.alternate_count; ++i)
        emit(line, "ISO trace AS setter: alternate=%u result=%d return_tick=%llu\n",
             trace.alternate[i].value, trace.alternate[i].result,
             (unsigned long long)trace.alternate[i].end);
    for (unsigned i = 0; i < trace.count; ++i) {
        TraceRequest *r = &trace.request[i];
        bool complete = atomic_load_explicit(&r->completed, memory_order_acquire);
        char h[2 * ISO_TRACE_HEADER + 1], s[2 * ISO_TRACE_SIZES + 1];
        char b[2 * ISO_TRACE_SIZES + 1], u[2 * ISO_TRACE_SIZES + 1];
        hex(h, r->header, sizeof(r->header));
        hex(s, r->submitted, r->bytes);
        hex(b, r->callback, complete ? r->bytes : 0);
        hex(u, r->consumed, r->was_consumed ? r->bytes : 0);
        emit(line,
             "ISO trace request=%u key=%lx slot=%lx device=%08lx ep=%02x packets=%u "
             "length=%u in=%u io=%u copied_size_bytes=%u header24=%s\n",
             i + 1, (unsigned long)(uintptr_t)r->wire.request,
             (unsigned long)(uintptr_t)r->wire.slot, (unsigned long)(uint32_t)r->wire.device,
             r->wire.endpoint, r->wire.packets, r->wire.length, r->wire.in_count, r->wire.io_count,
             r->bytes, h);
        emit(line,
             "ISO trace request=%u vectors=%lx/%lu,%lx/%lu,%lx/%lu,%lx/%lu consumer_sizes=%lx\n",
             i + 1, (unsigned long)r->wire.vector_address[0],
             (unsigned long)r->wire.vector_length[0], (unsigned long)r->wire.vector_address[1],
             (unsigned long)r->wire.vector_length[1], (unsigned long)r->wire.vector_address[2],
             (unsigned long)r->wire.vector_length[2], (unsigned long)r->wire.vector_address[3],
             (unsigned long)r->wire.vector_length[3], (unsigned long)r->consumer_sizes);
        emit(line,
             "ISO trace request=%u submit_returned=%u submit_result=%d callback=%u "
             "callback_result=%d consumed=%u equal_callback_consumer=%u\n",
             i + 1, r->submit_returned, r->submit_result, complete, r->callback_result,
             r->was_consumed,
             complete && r->was_consumed && !memcmp(r->callback, r->consumed, r->bytes));
        emit(line,
             "ISO trace request=%u ticks submit=%llu callback=%llu consume=%llu "
             "copy_cost_ticks=%llu/%llu/%llu\n",
             i + 1, (unsigned long long)r->submit_tick, (unsigned long long)r->callback_tick,
             (unsigned long long)r->consume_tick, (unsigned long long)r->submit_cost,
             (unsigned long long)r->callback_cost, (unsigned long long)r->consume_cost);
        emit(line, "ISO trace request=%u sizes submit=%s callback=%s consume=%s\n", i + 1, s, b, u);
    }
    for (unsigned i = 0; i < trace.cancel_count; ++i)
        emit(line, "ISO trace cancel=%u begin_tick=%llu end_tick=%llu returned=%u result=%d\n",
             i + 1, (unsigned long long)trace.cancel[i].start,
             (unsigned long long)trace.cancel[i].end, trace.cancel[i].returned,
             trace.cancel[i].result);
    emit(line, "ISO trace end: measured copy costs exclude lookup/call/publication overhead; "
               "instrumentation can perturb timing.\n");
}

size_t iso_trace_storage_bytes(void)
{
    return sizeof(trace);
}
