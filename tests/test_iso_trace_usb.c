/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */
#include "iso_trace.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int32_t s32;
typedef uint32_t u32;
typedef uint16_t u16;
typedef uint8_t u8;
typedef s32 (*usbcallback)(s32, void *);

typedef struct {
    void *data;
    u32 len;
} ioctlv;

enum {
    IPC_OK = 0,
    IPC_EINVAL = -4,
    IPC_ENOMEM = -22,
    USB_ENDPOINT_IN = 0x80,
    USBV0_IOCTL_ISOMSG = 9,
    USBV5_IOCTL_ISOMSG = 20
};

static struct {
    s32 fd;
} ven = {42}, hid = {43};

static void *ven_host_unused;
#define ven_host (&ven)
#define hid_host (&hid)
static int hId;
static unsigned allocations, frees, requests, callbacks;
static bool immediate, reject;

static void *iosAlloc(int heap, size_t size)
{
    (void)heap;
    ++allocations;
    return calloc(1, size);
}

static void iosFree(int heap, void *ptr)
{
    (void)heap;
    ++frees;
    free(ptr);
}

static s32 IOS_IoctlvAsync(s32 fd, unsigned code, unsigned in, unsigned io, ioctlv *v,
                           usbcallback cb, void *context);

static s32 IOS_Ioctlv(s32 fd, unsigned code, unsigned in, unsigned io, ioctlv *v)
{
    (void)fd;
    (void)code;
    (void)in;
    (void)io;
    (void)v;
    return 512;
}

static s32 __usbv0_messageCB(s32 result, void *context)
{
    (void)context;
    return result;
}

#include "usb_under_test.h"

static uint64_t ticks;

static uint64_t clock_tick(void)
{
    return ++ticks;
}

static char output[16384];
static unsigned used;

static void log_line(const char *line)
{
    size_t n = strlen(line);
    assert(used + n < sizeof(output));
    memcpy(output + used, line, n + 1);
    used += n;
}

typedef struct {
    u16 *sizes;
    int result;
} Slot;

static struct {
    usbcallback cb;
    void *context;
    ioctlv *v;
} pending[4];

static s32 app_callback(s32 result, void *context)
{
    Slot *slot = context;
    ++callbacks;
    slot->result = result;
    /* If the diagnostic hook runs later, it will incorrectly record these bytes. */
    for (unsigned i = 0; i < 16; ++i)
        slot->sizes[i] = 4096;
    return 123;
}

static s32 IOS_IoctlvAsync(s32 fd, unsigned code, unsigned in, unsigned io, ioctlv *v,
                           usbcallback cb, void *context)
{
    assert(fd == 42 && code == 20 && in == 1 && io == 3);
    assert(v[0].data == context && v[0].len == 64);
    assert(v[1].data == v[3].data && v[1].len == 32 && v[3].len == 32);
    assert(v[2].len == 3072);
    struct _usb_msg *msg = context;
    assert(msg->fd == -12 && msg->iso.bEndpoint == 0x84 && msg->iso.bPackets == 16);
    assert(msg->iso.rpPacketSizes == v[1].data && msg->iso.rpData == v[2].data);
    u16 *sizes = v[1].data;
    for (unsigned i = 0; i < 16; ++i)
        assert(sizes[i] == 192);
    assert(requests < 4);
    pending[requests].cb = cb;
    pending[requests].context = context;
    pending[requests++].v = v;
    if (reject) return -5;
    if (immediate) {
        for (unsigned i = 0; i < 16; ++i)
            sizes[i] = 32;
        cb(512, context);
    }
    return 0;
}

static void reset(void)
{
    assert(allocations == frees);
    requests = callbacks = used = 0;
    output[0] = 0;
    IsoTraceContext context = {.audio_id = -12, .endpoint = 0x84, .capacity = 192};
    iso_trace_begin(&context, clock_tick);
}

int main(void)
{
    (void)ven_host_unused;
    _Alignas(32) u16 sizes[4][16];
    _Alignas(32) u8 data[4][3072];
    Slot slots[4];
    memset(data, 0x7f, sizeof(data));
    reset();
    for (unsigned i = 0; i < 4; ++i) {
        for (unsigned j = 0; j < 16; ++j)
            sizes[i][j] = 192;
        slots[i] = (Slot){sizes[i], -999};
        assert(__usb_isochronous_message(-12, 0x84, 16, sizes[i], data[i], app_callback, &slots[i])
               == 0);
    }
    const unsigned order[] = {3, 1, 0, 2};
    for (unsigned n = 0; n < 4; ++n) {
        unsigned i = order[n];
        for (unsigned j = 0; j < 16; ++j)
            sizes[i][j] = 32;
        int result = i == 2 ? -7022 : i == 1 ? 512 : 0;
        assert(pending[i].cb(result, pending[i].context) == 0);
        assert(slots[i].result == result);
        if (result >= 0) iso_trace_consume(&slots[i], sizes[i]);
    }
    assert(callbacks == 4 && allocations == frees);
    for (unsigned i = 0; i < sizeof(data); ++i)
        assert(((u8 *)data)[i] == 0x7f);
    iso_trace_queue_done();
    iso_trace_dump(log_line);
    assert(strstr(output, "requests=4"));
    assert(strstr(output, "callback_result=512 consumed=1 equal_callback_consumer=0"));
    assert(strstr(output, "callback_result=-7022 consumed=0"));
    u16 before = 32, after = 4096;
    char expected[100];
    snprintf(expected, sizeof(expected), "callback=%02x%02x", ((u8 *)&before)[0],
             ((u8 *)&before)[1]);
    assert(strstr(output, expected));
    snprintf(expected, sizeof(expected), "consume=%02x%02x", ((u8 *)&after)[0], ((u8 *)&after)[1]);
    assert(strstr(output, expected));

    reset();
    immediate = true;
    for (unsigned j = 0; j < 16; ++j)
        sizes[0][j] = 192;
    assert(__usb_isochronous_message(-12, 0x84, 16, sizes[0], data[0], app_callback, &slots[0])
           == 0);
    assert(callbacks == 1 && allocations == frees);
    iso_trace_consume(&slots[0], sizes[0]);
    iso_trace_queue_done();
    iso_trace_dump(log_line);
    assert(strstr(output, "submit_returned=1 submit_result=0 callback=1 callback_result=512"));

    reset();
    immediate = false;
    reject = true;
    for (unsigned j = 0; j < 16; ++j)
        sizes[0][j] = 192;
    assert(__usb_isochronous_message(-12, 0x84, 16, sizes[0], data[0], app_callback, &slots[0])
           == -5);
    /* Existing libogc leaves a rejected async allocation to its backend contract. */
    iosFree(hId, pending[0].context);
    iso_trace_queue_done();
    iso_trace_dump(log_line);
    assert(strstr(output, "submit_result=-5 callback=0"));
    assert(allocations == frees);
    puts(
        "PASS: actual vendor ISO builder/callback, identical vectors/results, snapshot before app mutation/free, out-of-order and immediate callbacks, negative completion/submission, no payload change");
}
