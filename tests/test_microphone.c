/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "microphone.h"
#include "uvc_fixture.h"
#include <stdlib.h>

/* Logged UAC descriptors in a synthetic configuration envelope. */
static Fixture microphone(void)
{
    Fixture f = {0};
    const uint8_t bytes[] = {
        9,    2, 0,    0,    2,    1,    0,    0x80, 100,  9,    4,    2,    0,    0,    1,
        1,    0, 0,    9,    0x24, 1,    0,    1,    41,   0,    1,    3,    12,   0x24, 2,
        1,    1, 2,    0,    1,    0,    0,    0,    0,    11,   0x24, 6,    2,    1,    2,
        1,    0, 2,    0,    0,    9,    0x24, 3,    3,    1,    1,    0,    2,    0,    9,
        4,    3, 0,    0,    1,    2,    0,    0,    9,    4,    3,    1,    1,    1,    2,
        0,    0, 7,    0x24, 1,    3,    1,    1,    0,    29,   0x24, 2,    1,    1,    2,
        16,   7, 0x40, 0x1f, 0,    0x11, 0x2b, 0,    0x80, 0x3e, 0,    0x22, 0x56, 0,    0xc0,
        0x5d, 0, 0x44, 0xac, 0,    0x80, 0xbb, 0,    9,    5,    0x84, 5,    192,  0,    4,
        0,    0, 7,    0x25, 1,    1,    0,    0,    0};
    append(&f, bytes, sizeof(bytes));
    return f;
}

typedef struct {
    unsigned calls, fault, endpoint;
} Control;

static int control(void *context, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                   uint8_t *data, unsigned length)
{
    Control *c = context;
    assert(value == 0x100 && index == (c->endpoint ? c->endpoint : 0x84) && length == 3);
    assert(type == (c->calls ? 0xa2 : 0x22) && request == (c->calls ? 0x81 : 1));
    if (!c->calls)
        assert(data[0] == 0x80 && data[1] == 0x3e && data[2] == 0);
    else {
        assert(!data[0] && !data[1] && !data[2]);
        data[0] = c->fault == 5 ? 0x44 : 0x80;
        data[1] = c->fault == 5 ? 0xac : 0x3e;
    }
    unsigned step = c->calls++;
    if (c->fault == 1 && !step) return -4;
    if (c->fault == 2 && !step) return 2;
    if (c->fault == 3 && step) return -4;
    if (c->fault == 4 && step) return 2;
    if (c->fault == 6 && step) return -7102;
    if (c->fault == 7 && !step) return -7102;
    return 3;
}

int main(void)
{
    Fixture f = microphone();
    UacMode mode;
    assert(microphone_mode(f.data, f.length, &mode));
    assert(mode.control_interface == 2 && mode.interface == 3 && mode.alternate == 1);
    assert(mode.endpoint == 0x84 && mode.packet_capacity == 192 && mode.interval == 4);
    assert(mode.channels == 1 && mode.rate == 16000 && mode.rate_count == 7);
    assert(mode.microphone == 1 && mode.terminal == 3);
    for (size_t n = 0; n < f.length; ++n)
        assert(!microphone_mode(f.data, n, &mode));
    assert(microphone_mode(f.data, f.length, &mode));
    CameraEntry entry = {.vid = 0x0c45, .pid = 0x6366, .interface = 3};
    uint8_t data[32];
    for (unsigned fault = 0; fault <= 7; ++fault) {
        Control state = {.fault = fault};
        bool set_failed = fault == 1 || fault == 2 || fault == 7;
        assert(microphone_set_rate(&entry, &mode, control, &state, data)
               == (set_failed ? MIC_RATE_FAILED : MIC_RATE_UNREADABLE));
        assert(state.calls == 1);
        CameraEntry other = entry;
        other.pid ^= 1;
        state = (Control){.fault = fault};
        assert(microphone_set_rate(&other, &mode, control, &state, data)
               == (fault == 0 ? MIC_RATE_OK : MIC_RATE_FAILED));
        assert(state.calls == (set_failed ? 1u : 2u));
    }
    for (unsigned n = 0; n < 10; ++n) {
        CameraEntry other = entry;
        UacMode changed = mode;
        if (n == 0) other.vid ^= 1;
        if (n == 1) other.pid ^= 1;
        if (n == 2) other.interface ^= 1;
        if (n == 3) changed.control_interface ^= 1;
        if (n == 4) changed.interface ^= 1;
        if (n == 5) changed.alternate ^= 2;
        if (n == 6) changed.endpoint ^= 1;
        if (n == 7) changed.packet_capacity += 2;
        if (n == 8) changed.interval ^= 1;
        Control state = {.fault = 6, .endpoint = changed.endpoint};
        assert(microphone_set_rate(n == 9 ? NULL : &other, &changed, control, &state, data)
               == MIC_RATE_FAILED);
        assert(state.calls == 2);
    }
    mode.frequency_control = false;
    assert(!microphone_set_rate(&entry, &mode, NULL, NULL, NULL));
    mode.rate_count = 1;
    mode.rates[0] = MIC_RATE;
    assert(microphone_set_rate(&entry, &mode, NULL, NULL, NULL));
    mode.channels = 2;
    assert(!microphone_set_rate(&entry, &mode, NULL, NULL, NULL));

    MicEntry entries[3] = {{{(int32_t)0xe71e0042u, 0x0c45, 0x6366, 1}, 0x00420107},
                           {{(int32_t)0xe71c0042u, 0x0c45, 0x6366, 3}, 0x00420302},
                           {{(int32_t)0xe7190043u, 0x1234, 0x5678, 3}, 0x00430302}};
    size_t selected = 999;
    assert(microphone_match(entries, 3, entries[0].camera.id, 1, 3, &selected) && selected == 1);
    MicEntry saved = entries[2];
    entries[2] = entries[1];
    entries[2].camera.id = (int32_t)0xe7190043u;
    entries[2].token = 0x00430302;
    assert(!microphone_match(entries, 3, entries[0].camera.id, 1, 3, &selected));
    entries[2] = entries[0];
    assert(!microphone_match(entries, 3, entries[0].camera.id, 1, 3, &selected));
    entries[2] = saved;
    entries[1].token ^= 0x10000;
    assert(!microphone_match(entries, 3, entries[0].camera.id, 1, 3, &selected));
    entries[1].token ^= 0x10000;
    entries[1].camera.id = -1;
    assert(!microphone_match(entries, 3, entries[0].camera.id, 1, 3, &selected));
    entries[1].camera.id = (int32_t)0xe71c0042u;
    uint8_t parameters[192] = {0};
    parameters[20] = 18;
    parameters[21] = 1;
    parameters[37] = 1;
    parameters[28] = 0x0c;
    parameters[29] = 0x45;
    parameters[30] = 0x63;
    parameters[31] = 0x66;
    parameters[40] = 9;
    parameters[41] = 2;
    parameters[44] = 4;
    parameters[45] = 1;
    parameters[52] = 9;
    parameters[53] = 4;
    parameters[54] = 3;
    parameters[57] = 1;
    parameters[58] = 2;
    assert(microphone_parameters(&entries[1].camera, parameters, 1));
    const unsigned faults[] = {20, 21, 28, 29, 30, 31, 37, 40, 41,
                               45, 52, 53, 54, 55, 56, 57, 58, 59};
    for (unsigned n = 0; n < sizeof(faults) / sizeof(*faults); ++n) {
        parameters[faults[n]] ^= 1;
        assert(!microphone_parameters(&entries[1].camera, parameters, 1));
        parameters[faults[n]] ^= 1;
    }

    uint8_t *samples = malloc(MIC_BYTES + 2);
    assert(samples);
    samples[0] = 0xa5;
    samples[MIC_BYTES + 1] = 0x5a;
    MicPcm pcm;
    uint8_t packet[194] = {0, 0x80, 0xff, 0x7f};
    microphone_pcm_init(&pcm, samples + 1, MIC_BYTES);
    assert(microphone_pcm_packet(&pcm, packet, 0, 192) && pcm.empty == 1 && !pcm.size);
    while (pcm.size < MIC_BYTES)
        assert(microphone_pcm_packet(&pcm, packet, 192, 192));
    assert(pcm.peak == 32768 && pcm.size == MIC_BYTES && !pcm.failed);
    const uint64_t spans[] = {0, 1667, 3333, 4499, 4500, 5000, 5500, 5501, 10000, UINT64_MAX};
    for (unsigned n = 0; n < sizeof(spans) / sizeof(*spans); ++n) {
        assert(microphone_capture_valid(MIC_RATE_UNREADABLE, &pcm, spans[n])
               == (spans[n] >= 4500 && spans[n] <= 5500));
        assert(microphone_capture_valid(MIC_RATE_OK, &pcm, spans[n]));
        assert(!microphone_capture_valid(MIC_RATE_FAILED, &pcm, spans[n]));
    }
    assert(!microphone_capture_valid(MIC_RATE_UNREADABLE, NULL, 5000));
    pcm.failed = true;
    assert(!microphone_capture_valid(MIC_RATE_UNREADABLE, &pcm, 5000));
    pcm.failed = false;
    assert(samples[0] == 0xa5 && samples[MIC_BYTES + 1] == 0x5a);
    FILE *wav = fopen("build/test-microphone.wav", "wb+");
    assert(wav && microphone_wav(wav, &pcm) && fflush(wav) == 0);
    assert(ftell(wav) == MIC_BYTES + 44);
    rewind(wav);
    uint8_t header[44];
    assert(fread(header, 1, 44, wav) == 44);
    assert(!memcmp(header, "RIFF", 4) && !memcmp(header + 8, "WAVEfmt ", 8));
    assert(uvc_le32(header + 4) == MIC_BYTES + 36 && uvc_le32(header + 24) == MIC_RATE);
    assert(uvc_le32(header + 40) == MIC_BYTES && header[22] == 1 && header[34] == 16);
    assert(fclose(wav) == 0);
    for (unsigned length = 1; length <= 194; ++length) {
        microphone_pcm_init(&pcm, samples + 1, MIC_BYTES);
        bool valid = length <= 192 && !(length & 1);
        assert(microphone_pcm_packet(&pcm, packet, length, 192) == valid);
        assert(pcm.failed == !valid);
    }
    /* No payload may be read from an IOS transaction-error slot. */
    const uint8_t *inaccessible = (const uint8_t *)(uintptr_t)1;
    microphone_pcm_init(&pcm, samples + 1, MIC_BYTES);
    pcm.recover_transactions = true;
    memset(pcm.data, 0x55, MIC_BYTES);
    for (unsigned n = 0; n < 500; ++n)
        assert(microphone_pcm_packet(&pcm, inaccessible, 0x1000, 192));
    assert(pcm.rejected == 500 && !pcm.failed && !pcm.size && !pcm.received
           && !pcm.concealed_bytes && !pcm.packets);
    assert(!microphone_capture_valid(MIC_RATE_UNREADABLE, &pcm, 5000));
    for (size_t n = 0; n < MIC_BYTES; ++n)
        assert(pcm.data[n] == 0x55);
    assert(microphone_pcm_packet(&pcm, packet, 0, 192));
    assert(!pcm.received && !pcm.size && pcm.empty == 1);
    for (unsigned n = 0; n < MIC_BYTES / 32; ++n)
        assert(microphone_pcm_packet(&pcm, packet, 32, 192));
    assert(pcm.received == MIC_BYTES && !pcm.concealed_bytes && !pcm.failed);
    assert(microphone_capture_valid(MIC_RATE_UNREADABLE, &pcm, 5000));

    /* Fifty spaced 1 ms losses retain a five-second timeline and bounded silence. */
    microphone_pcm_init(&pcm, samples + 1, MIC_BYTES);
    pcm.recover_transactions = true;
    for (unsigned n = 0; n < MIC_BYTES / 32; ++n) {
        bool lost = n % 100 == 99;
        assert(microphone_pcm_packet(&pcm, lost ? inaccessible : packet,
                                     lost ? 0x1000 : 32, 192));
        for (unsigned j = 0; j < 32; ++j)
            assert(pcm.data[n * 32 + j] == (lost ? 0 : packet[j]));
    }
    assert(pcm.size == MIC_BYTES && pcm.received == MIC_BYTES - 50 * 32
           && pcm.concealed_bytes == 50 * 32 && pcm.rejected == 50 && pcm.peak == 32768);
    assert(microphone_capture_valid(MIC_RATE_UNREADABLE, &pcm, 5000));
    for (unsigned n = 0; n < MIC_MAX_REJECTED_PACKETS; ++n)
        assert(pcm.rejected_lengths[n] == 0x1000 && pcm.rejected_indices[n] == n * 100 + 99);

    microphone_pcm_init(&pcm, samples + 1, MIC_BYTES);
    pcm.recover_transactions = true;
    assert(microphone_pcm_packet(&pcm, packet, 32, 192));
    for (unsigned n = 0; n < 10; ++n)
        assert(microphone_pcm_packet(&pcm, inaccessible, 0x1000, 192));
    assert(microphone_pcm_packet(&pcm, packet, 0, 192));
    assert(!microphone_pcm_packet(&pcm, inaccessible, 0x1000, 192));
    assert(pcm.failed && pcm.received == 32 && pcm.concealed_bytes == 320 && pcm.size == 352);

    /* Short bursts cannot bypass the cumulative one-percent loss allowance. */
    microphone_pcm_init(&pcm, samples + 1, MIC_BYTES);
    pcm.recover_transactions = true;
    for (unsigned n = 0; n < 10; ++n) {
        assert(microphone_pcm_packet(&pcm, packet, 32, 192));
        assert(microphone_pcm_packet(&pcm, inaccessible, 0x1000, 192));
    }
    assert(microphone_pcm_packet(&pcm, packet, 32, 192));
    assert(!microphone_pcm_packet(&pcm, inaccessible, 0x1000, 192));
    assert(pcm.failed && pcm.received == 352 && pcm.concealed_bytes == 320);

    const size_t invalid_lengths[] = {31, 193, 194, 0x1001, 0x2000, 65535};
    for (unsigned n = 0; n < sizeof(invalid_lengths) / sizeof(*invalid_lengths); ++n) {
        for (unsigned started = 0; started < 2; ++started) {
            microphone_pcm_init(&pcm, samples + 1, MIC_BYTES);
            pcm.recover_transactions = true;
            if (started) assert(microphone_pcm_packet(&pcm, packet, 32, 192));
            assert(!microphone_pcm_packet(&pcm, inaccessible, invalid_lengths[n], 192));
            assert(pcm.failed && pcm.rejected == 1 && pcm.size == started * 32);
        }
    }
    microphone_pcm_init(&pcm, samples + 1, MIC_BYTES);
    assert(!microphone_pcm_packet(&pcm, inaccessible, 0x1000, 192));
    microphone_pcm_init(&pcm, samples + 1, MIC_BYTES);
    pcm.recover_transactions = true;
    assert(!microphone_pcm_packet(&pcm, inaccessible, 0x1000, 128));
    microphone_pcm_init(&pcm, samples + 1, MIC_BYTES);
    pcm.recover_transactions = true;
    assert(microphone_pcm_packet(&pcm, packet, 64, 192));
    assert(!microphone_pcm_packet(&pcm, inaccessible, 0x1000, 192));
    assert(pcm.size == 64 && !pcm.concealed_bytes);
    microphone_pcm_init(&pcm, samples + 1, MIC_BYTES);
    pcm.recover_transactions = true;
    assert(!microphone_pcm_packet(&pcm, NULL, 32, 192));
    assert(samples[0] == 0xa5 && samples[MIC_BYTES + 1] == 0x5a);
    microphone_pcm_init(&pcm, samples + 1, MIC_BYTES);
    assert(!microphone_capture_valid(MIC_RATE_OK, &pcm, 5000));
    assert(!microphone_capture_valid(MIC_RATE_UNREADABLE, &pcm, 5000));
    FILE *partial = tmpfile();
    assert(partial && !microphone_wav(partial, &pcm) && ftell(partial) == 0);
    fclose(partial);
    free(samples);
    puts(
        "PASS: physical UAC fields, ambiguous/changed IOS pairing, strict rate controls, exact-profile readback skip and strict readback for other devices, timing guard, bounded transaction recovery, real PCM accounting, packet bounds, PCM/WAV and partial rejection");
}
