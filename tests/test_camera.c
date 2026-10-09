/* SPDX-License-Identifier: GPL-3.0-only */
#include "camera.h"
#include "uvc_fixture.h"
#include <stdio.h>

enum { OK, OPEN, PARAMETERS, MULTI_CONFIG, CLASS, INTERFACE, IDENTITY,
       DEVICE_HEADER, CONFIG_HEADER, IFACE_HEADER, SWAPPED_ID,
       CURRENT_SHORT, INACTIVE, HEADER_SHORT, CONFIG_LARGE, FULL_SHORT,
       CHANGED_CONFIG, BAD_VERSION, NO_MODE };
typedef struct {
    CameraEntry entries[2];
    Fixture configs[2];
    unsigned faults[2], opens[2], closes[2], reads[2], messages;
    unsigned order[2], attempts;
    char last_message[160], ios_message[160];
} Fake;

static unsigned index_of(int32_t handle)
{
    assert(handle == -100 || handle == -101);
    return (unsigned)(-handle - 100);
}

static int open_camera(void *context, const CameraEntry *entry, int32_t *handle)
{
    Fake *f = context;
    unsigned i = (unsigned)entry->id;
    assert(i < 2 && f->attempts < 2);
    f->order[f->attempts++] = i;
    ++f->opens[i];
    if (f->faults[i] == OPEN)
        return -1;
    *handle = -100 - (int32_t)i;
    return 0;
}

static int parameters(void *context, int32_t handle, uint8_t *data)
{
    Fake *f = context;
    unsigned i = index_of(handle), fault = f->faults[i];
    for (unsigned n = 0; n < CAMERA_IOS_PARAMETERS; ++n) assert(data[n] == 0);
    if (fault == PARAMETERS) return -4;
    uint8_t *d = data + 20, *c = data + 40, *in = data + 52;
    d[0] = 18; d[1] = 1; d[17] = fault == MULTI_CONFIG ? 2 : 1;
    /* Compact IOS reply is big-endian; raw config fixture below remains USB LE. */
    d[8] = f->entries[i].vid >> 8; d[9] = f->entries[i].vid;
    d[10] = f->entries[i].pid >> 8; d[11] = f->entries[i].pid;
    if (fault == SWAPPED_ID) {
        uint8_t tmp = d[8]; d[8] = d[9]; d[9] = tmp;
        tmp = d[10]; d[10] = d[11]; d[11] = tmp;
    }
    if (fault == IDENTITY) ++d[8];
    c[0] = 9; c[1] = 2; c[4] = 2; c[5] = 1;
    in[0] = 9; in[1] = 4; in[2] = f->entries[i].interface;
    if (fault == INTERFACE) ++in[2];
    in[5] = fault == CLASS ? 3 : 14; in[6] = 2;
    if (fault == DEVICE_HEADER) d[0] = 17;
    if (fault == CONFIG_HEADER) c[0] = 8;
    if (fault == IFACE_HEADER) in[0] = 8;
    return 0;
}

static int read_control(void *context, int32_t handle, uint8_t request,
                        uint16_t value, uint16_t length, uint8_t *data)
{
    Fake *f = context;
    unsigned i = index_of(handle), fault = f->faults[i], step = f->reads[i]++;
    for (unsigned n = 0; n < CAMERA_CONFIG_CAPACITY; ++n) assert(data[n] == 0);
    assert(step < 3);
    if (step == 0) {
        assert(request == 8 && value == 0 && length == 1);
        data[0] = fault == INACTIVE ? 2 : 1;
        return fault == CURRENT_SHORT ? 8 : 9;
    }
    assert(request == 6 && value == 0x0200);
    assert(length == (step == 1 ? 9 : f->configs[i].length));
    memcpy(data, f->configs[i].data, length);
    if (step == 1 && fault == HEADER_SHORT) return 9; /* only one payload byte */
    if (step == 1 && fault == CONFIG_LARGE) data[3] = 0x20;
    if (step == 2 && fault == FULL_SHORT) return length + 7;
    if (step == 2 && fault == CHANGED_CONFIG) data[5] = 2;
    if (step == 2 && fault == BAD_VERSION) data[f->configs[i].vc + 3] = 0x50;
    if (step == 2 && fault == NO_MODE) data[f->configs[i].frame + 5] = 0x80;
    return length + 8;
}

static void close_camera(void *context, int32_t handle)
{
    Fake *f = context;
    unsigned i = index_of(handle);
    assert(++f->closes[i] == 1);
}

static void message(void *context, const CameraEntry *entry, const char *text)
{
    Fake *f = context;
    assert(entry && text && text[0]);
    ++f->messages;
    snprintf(f->last_message, sizeof(f->last_message), "%s", text);
    if (strstr(text, "IOS descriptor fields:"))
        snprintf(f->ios_message, sizeof(f->ios_message), "%s", text);
}

static Fake make_fake(void)
{
    Fake f = {0};
    f.entries[0] = (CameraEntry){0, 0x0c45, 0x6366, 5};
    f.entries[1] = (CameraEntry){1, 0x057e, 0x030a, 1};
    f.configs[0] = fixture(5, 3, 0x83, false);
    f.configs[1] = fixture(1, 0, 0x81, true);
    return f;
}

static bool find(Fake *f, size_t count, int32_t *handle, UvcMode *mode)
{
    struct { uint8_t before, data[4096], after; } memory = {.before=0x5a, .after=0xa5};
    CameraOps ops = {f, open_camera, parameters, read_control, close_camera, message};
    bool found = camera_find(f->entries, count, 320, 240, memory.data, sizeof(memory.data),
                             &ops, handle, mode);
    assert(memory.before == 0x5a && memory.after == 0xa5);
    return found;
}

int main(void)
{
    int32_t handle;
    UvcMode mode;
    Fake f = make_fake();
    assert(find(&f, 1, &handle, &mode) && handle == -100 && mode.interface == 5);
    assert(f.opens[0] == 1 && f.closes[0] == 0 && f.reads[0] == 3);
    assert(strstr(f.ios_message, "id=0c45:6366 configs=1 config=1 interface=5 class=0e subclass=02"));
    f = make_fake();
    /* Full discovery with the retained real WC04 raw configuration fixture. */
    FILE *file = fopen("tests/fixtures/wc04-configuration.bin", "rb");
    assert(file);
    f.configs[1].length = fread(f.configs[1].data, 1, 4096, file);
    assert(f.configs[1].length == 777 && fclose(file) == 0);
    assert(find(&f, 2, &handle, &mode) && handle == -101 && mode.interface == 1);
    assert(f.order[0] == 1 && f.opens[0] == 0 && f.closes[1] == 0);
    assert(strstr(f.ios_message, "id=057e:030a"));
    const char *const expected[] = {
        [OPEN]="open failed", [PARAMETERS]="IOS GETDEVPARAMS failed: -4",
        [MULTI_CONFIG]="unsupported IOS configuration count",
        [CLASS]="not a VideoStreaming interface",
        [INTERFACE]="IOS interface number differs",
        [IDENTITY]="IOS vendor/product identity differs",
        [DEVICE_HEADER]="invalid IOS device descriptor header",
        [CONFIG_HEADER]="invalid IOS configuration descriptor",
        [IFACE_HEADER]="invalid IOS interface descriptor header",
        [SWAPPED_ID]="IOS vendor/product identity differs",
        [CURRENT_SHORT]="configuration inactive or unreadable",
        [INACTIVE]="configuration inactive or unreadable",
        [HEADER_SHORT]="configuration header short",
        [CONFIG_LARGE]="configuration header short",
        [FULL_SHORT]="configuration read short",
        [CHANGED_CONFIG]="configuration read short",
        [BAD_VERSION]="only UVC 1.0 controls supported",
        [NO_MODE]="no progressive MJPEG mode"
    };
    for (unsigned fault = OPEN; fault <= NO_MODE; ++fault) {
        f = make_fake(); f.faults[0] = fault;
        assert(!find(&f, 1, &handle, &mode));
        assert(f.opens[0] == 1 && f.closes[0] == (fault == OPEN ? 0u : 1u));
        assert(strstr(f.last_message, expected[fault]) && mode.alt_count == 0);
        if (fault == OPEN || fault == PARAMETERS)
            assert(f.messages == 1 && !f.ios_message[0]);
        else
            assert(f.messages == 2 && f.ios_message[0]);
        if (fault <= SWAPPED_ID)
            assert(f.reads[0] == 0);
        if (fault == SWAPPED_ID)
            assert(strstr(f.ios_message, "id=450c:6663"));
        f = make_fake(); f.faults[1] = fault;
        assert(find(&f, 2, &handle, &mode) && handle == -100);
        assert(f.order[0] == 1 && f.order[1] == 0);
        assert(f.closes[1] == (fault == OPEN ? 0u : 1u) && !f.closes[0]);
    }
    f = make_fake();
    assert(!find(&f, 33, &handle, &mode) && !f.attempts);
    assert(!find(&f, 0, &handle, &mode) && !f.attempts);
    puts("PASS: IOS big-endian IDs, raw USB little-endian config, swapped-ID rejection, precise diagnostics, discovery ownership/failures");
    return 0;
}
