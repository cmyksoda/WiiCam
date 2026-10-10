/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WIICAM_AUDIO_STREAM_H
#define WIICAM_AUDIO_STREAM_H
#include "microphone.h"
#include <stdatomic.h>

#define AUDIO_RING_BYTES 65536u
#define AUDIO_BLOCK_BYTES 3200u
#define AUDIO_FRAME_MS 100u
#define AUDIO_FRAME_RATE (1000u / AUDIO_FRAME_MS)

typedef struct {
    uint8_t data[AUDIO_RING_BYTES];
    atomic_uint produced, consumed;
    atomic_bool failed;
    MicRateResult rate;
    unsigned packets, empty, rejected, smallest, largest, peak, first_length;
    unsigned rejected_lengths[MIC_MAX_REJECTED_PACKETS];
    unsigned rejected_indices[MIC_MAX_REJECTED_PACKETS];
    uint64_t bytes, first_ms, last_ms;
} AudioStream;

/* One USB producer, one writer/consumer. Positions wrap modulo unsigned. */
void audio_stream_init(AudioStream *stream, MicRateResult rate);
bool audio_stream_packet(AudioStream *stream, const uint8_t *data, unsigned length,
                         unsigned capacity, uint64_t now_ms);
unsigned audio_stream_position(const AudioStream *stream);
void audio_stream_discard(AudioStream *stream);
bool audio_stream_read(AudioStream *stream, uint8_t *data, unsigned length);
/* Read statistics only after the producer joins. */
bool audio_stream_valid(const AudioStream *stream);
#endif
