/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */
#include "iso_trace.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char output[16384];
static unsigned used, lines;
static uint64_t ticks;

static uint64_t clock_tick(void)
{
    return ++ticks;
}

static void line(const char *text)
{
    size_t n = strlen(text);
    assert(n < 512 && used + n < sizeof(output));
    memcpy(output + used, text, n + 1);
    used += n;
    ++lines;
}

static void begin(void)
{
    IsoTraceContext context = {.video_id = -11,
                               .audio_id = -12,
                               .video_interface = 1,
                               .audio_interface = 3,
                               .endpoint = 0x84,
                               .capacity = 192,
                               .interval = 4,
                               .ios = 58,
                               .revision = 6176,
                               .tick_hz = 1000,
                               .av = true};
    used = lines = 0;
    output[0] = 0;
    iso_trace_begin(&context, clock_tick);
}

static void sizes_bytes(uint16_t *sizes, unsigned first, unsigned second)
{
    unsigned char *raw = (unsigned char *)sizes;
    for (unsigned i = 0; i < 16; ++i) {
        raw[2 * i] = first;
        raw[2 * i + 1] = second;
    }
}

int main(void)
{
    uint8_t header[24], data[3072], saved_data[3072];
    uint16_t sizes[4][16];
    unsigned request[4], slot[4];
    memset(header, 0x45, sizeof(header));
    memset(data, 0x9a, sizeof(data));
    memcpy(saved_data, data, sizeof(data));
    begin();
    assert(!iso_trace_wants(-11, 0x84) && !iso_trace_wants(-12, 0x81));
    iso_trace_alternate(0, 0);
    iso_trace_alternate(1, 7);
    iso_trace_alternate(9, -999);
    for (unsigned i = 0; i < 4; ++i) {
        sizes_bytes(sizes[i], 0, 192);
        IsoTraceWire wire = {.request = &request[i],
                             .slot = &slot[i],
                             .header = header,
                             .sizes = sizes[i],
                             .device = -12,
                             .endpoint = 0x84,
                             .packets = 16,
                             .length = 3072,
                             .in_count = 1,
                             .io_count = 3,
                             .vector_address = {(uintptr_t)header, (uintptr_t)sizes[i],
                                                (uintptr_t)data, (uintptr_t)sizes[i]},
                             .vector_length = {64, 32, 3072, 32}};
        unsigned token = iso_trace_submit(&wire);
        assert(token == i);
        iso_trace_submit_result(token, 0);
    }
    assert(!iso_trace_wants(-12, 0x84));
    iso_trace_dump(line);
    assert(!lines); /* No logging while a retained callback can still arrive. */
    sizes_bytes(sizes[1], 0, 32);
    iso_trace_callback(&request[1], 512);
    iso_trace_consume(&slot[1], sizes[1]);
    sizes_bytes(sizes[0], 0, 32);
    iso_trace_callback(&request[0], 0);
    sizes_bytes(sizes[0], 16, 0);
    iso_trace_consume(&slot[0], sizes[0]);
    unsigned cancel = iso_trace_cancel_begin();
    sizes_bytes(sizes[2], 16, 0);
    iso_trace_callback(&request[2], -7022);
    iso_trace_consume(&slot[2], sizes[2]);
    sizes_bytes(sizes[3], 16, 0);
    iso_trace_callback(&request[3], 0);
    iso_trace_cancel_end(cancel, 0);
    iso_trace_queue_done();
    /* A later queue generation reusing slots cannot claim an old drained entry. */
    sizes_bytes(sizes[3], 0, 32);
    iso_trace_callback(&request[3], 999);
    iso_trace_consume(&slot[3], sizes[3]);
    iso_trace_cancel_end(iso_trace_cancel_begin(), -4);
    assert(iso_trace_cancel_begin() == ISO_TRACE_NONE);
    iso_trace_dump(line);
    assert(lines == 27 && used < 10000);
    assert(strstr(output, "mode=AV IOS=58 revision=6176 requests=4"));
    assert(strstr(output, "alternate=1 result=7") && !strstr(output, "alternate=9"));
    assert(strstr(
        output,
        "request=1 submit_returned=1 submit_result=0 callback=1 callback_result=0 consumed=1 equal_callback_consumer=0"));
    assert(strstr(
        output,
        "request=2 submit_returned=1 submit_result=0 callback=1 callback_result=512 consumed=1 equal_callback_consumer=1"));
    assert(strstr(output, "callback_result=-7022 consumed=0"));
    assert(strstr(
        output,
        "request=4 submit_returned=1 submit_result=0 callback=1 callback_result=0 consumed=0"));
    assert(strstr(output, "submit=00c000c0") && strstr(output, "callback=00200020"));
    assert(strstr(output, "consume=10001000"));
    assert(!memcmp(data, saved_data, sizeof(data)) && !strstr(output, "9a9a9a9a"));
    unsigned once = lines;
    iso_trace_dump(line);
    assert(lines == once);

    begin();
    sizes_bytes(sizes[0], 0, 192);
    IsoTraceWire wire = {.request = request,
                         .slot = slot,
                         .header = header,
                         .sizes = sizes[0],
                         .device = -12,
                         .endpoint = 0x84,
                         .packets = 16};
    unsigned token = iso_trace_submit(&wire);
    iso_trace_submit_result(token, -5);
    iso_trace_queue_done();
    iso_trace_dump(line);
    assert(strstr(output, "submit_result=-5 callback=0") && !strstr(output, "callback=00"));

    begin();
    wire.packets = 1;
    token = iso_trace_submit(&wire);
    iso_trace_callback(request, 0); /* Synchronous callback before submission returns. */
    iso_trace_submit_result(token, 0);
    iso_trace_consume(slot, sizes[0]);
    iso_trace_queue_done();
    iso_trace_dump(line);
    assert(strstr(output, "copied_size_bytes=2")
           && strstr(output, "sizes submit=00c0 callback=00c0 consume=00c0"));
    assert(iso_trace_storage_bytes() < 4096);
    puts(
        "PASS: bounded trace, out-of-order callbacks, mutations, raw bytes, negative drain, reuse, immediate callback, submit failure, caps, no payload access, deferred log");
}
