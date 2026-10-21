/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (c) 2026 cmyksoda
 */
#include "audio_stream.h"
#include <limits.h>
#include <string.h>

_Static_assert(UINT_MAX == UINT32_MAX && ATOMIC_INT_LOCK_FREE == 2,
               "audio positions require lock-free 32-bit unsigned atomics");
_Static_assert((AUDIO_RING_BYTES & (AUDIO_RING_BYTES - 1)) == 0,
               "ring wrap must match unsigned position wrap");

void audio_stream_init(AudioStream *s, MicRateResult rate)
{
    memset(s, 0, sizeof(*s));
    atomic_init(&s->produced, 0);
    atomic_init(&s->consumed, 0);
    atomic_init(&s->received, 0);
    atomic_init(&s->failed, rate == MIC_RATE_FAILED);
    s->rate = rate;
}

bool audio_stream_packet(AudioStream *s, const uint8_t *data, unsigned length, unsigned capacity,
                         uint64_t now)
{
    if (atomic_load(&s->failed)) return false;
    unsigned index = s->packets + s->empty + s->rejected;
    if (!capacity || capacity > 1024 || (!data && length)) goto fail;
    bool conceal = false;
    if (length > capacity || (length & 1)) {
        unsigned n = s->rejected++;
        if (n < MIC_MAX_REJECTED_PACKETS) {
            s->rejected_lengths[n] = length;
            s->rejected_indices[n] = index;
        }
        /* IOS58's high-speed ISO completion preserves iTD status in bits 15:12.
         * 0x1000 is a transaction error with no payload. Only the known camera's
         * 16 kHz / 1 ms mode opts in; never read the failed slot or mask it into PCM. */
        if (s->rate != MIC_RATE_UNREADABLE || capacity != 192 || length != 0x1000) goto fail;
        if (!s->packets) return true;
        if (s->smallest != 32 || s->largest != 32) goto fail;
        /* Preserve the audio clock with one millisecond of silence per lost slot.
         * Limit bursts to 10 ms and loss to 1%, with one second of startup allowance. */
        if (++s->gap_packets > 10
            || (s->concealed_bytes + 32) * 100 > s->bytes + s->concealed_bytes + MIC_RATE * 2u)
            goto fail;
        length = 32;
        conceal = true;
    }
    if (!length) {
        ++s->empty;
        return true;
    }
    unsigned write = atomic_load_explicit(&s->produced, memory_order_relaxed);
    unsigned read = atomic_load_explicit(&s->consumed, memory_order_acquire);
    unsigned used = write - read;
    if (used > AUDIO_RING_BYTES || length > AUDIO_RING_BYTES - used) goto fail;
    for (unsigned n = 0; !conceal && n < length; n += 2) {
        int sample = (int16_t)((unsigned)data[n] | ((unsigned)data[n + 1] << 8));
        unsigned magnitude = sample < 0 ? (unsigned)-sample : (unsigned)sample;
        if (magnitude > s->peak) s->peak = magnitude;
    }
    for (unsigned n = 0; n < length; ++n)
        s->data[(write + n) & (AUDIO_RING_BYTES - 1)] = conceal ? 0 : data[n];
    if (conceal) {
        s->concealed_bytes += length;
        atomic_store_explicit(&s->produced, write + length, memory_order_release);
        return true;
    }
    if (!s->packets) {
        s->first_ms = now;
        s->first_length = length;
        s->smallest = length;
    }
    if (length < s->smallest) s->smallest = length;
    if (length > s->largest) s->largest = length;
    ++s->packets;
    s->bytes += length;
    s->gap_packets = 0;
    s->last_ms = now;
    atomic_store_explicit(&s->produced, write + length, memory_order_release);
    atomic_fetch_add_explicit(&s->received, length, memory_order_release);
    return true;
fail:
    atomic_store(&s->failed, true);
    return false;
}

unsigned audio_stream_position(const AudioStream *s)
{
    return atomic_load_explicit(&s->produced, memory_order_acquire);
}

unsigned audio_stream_received(const AudioStream *s)
{
    return atomic_load_explicit(&s->received, memory_order_acquire);
}

void audio_stream_discard(AudioStream *s)
{
    atomic_store_explicit(&s->consumed, audio_stream_position(s), memory_order_release);
}

bool audio_stream_read(AudioStream *s, uint8_t *data, unsigned length)
{
    unsigned read = atomic_load_explicit(&s->consumed, memory_order_relaxed);
    unsigned available = audio_stream_position(s) - read;
    if (!data || (length & 1) || length > AUDIO_RING_BYTES || available > AUDIO_RING_BYTES
        || length > available)
        return false;
    for (unsigned n = 0; n < length; ++n)
        data[n] = s->data[(read + n) & (AUDIO_RING_BYTES - 1)];
    atomic_store_explicit(&s->consumed, read + length, memory_order_release);
    return true;
}

bool audio_stream_valid(const AudioStream *s)
{
    if (atomic_load(&s->failed) || s->bytes < MIC_RATE * 2u || s->last_ms < s->first_ms)
        return false;
    uint64_t span = s->last_ms - s->first_ms;
    uint64_t samples_ms = (s->bytes + s->concealed_bytes - s->first_length) * 1000 / (MIC_RATE * 2u);
    uint64_t difference = span > samples_ms ? span - samples_ms : samples_ms - span;
    return difference <= 100 + span / 50;
}
