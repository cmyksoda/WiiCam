/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WIICAM_MICROPHONE_H
#define WIICAM_MICROPHONE_H
#include "camera.h"
#include "uac.h"
#include <stdio.h>

#define MIC_RATE 16000u
#define MIC_SECONDS 5u
#define MIC_BYTES (MIC_RATE * MIC_SECONDS * 2u)

typedef struct {
    CameraEntry camera;
    uint32_t token;
} MicEntry;

typedef struct {
    uint8_t *data;
    size_t size, capacity;
    unsigned packets, empty, smallest, largest, peak;
    bool failed;
} MicPcm;

/* IDs are IOS interface handles. Require one unambiguous video/audio pair. */
bool microphone_match(const MicEntry *entries, size_t count, int32_t video_id,
                      unsigned video_interface, unsigned audio_interface, size_t *index);
bool microphone_parameters(const CameraEntry *entry, const uint8_t *parameters,
                           unsigned configuration);
bool microphone_mode(const uint8_t *config, size_t length, UacMode *mode);
/* Callback returns payload bytes; the Wii adapter checks IOS setup-byte counts. */
typedef int (*MicControl)(void *, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                          uint8_t *data, unsigned length);

typedef enum { MIC_RATE_FAILED, MIC_RATE_OK, MIC_RATE_UNREADABLE } MicRateResult;

MicRateResult microphone_set_rate(const CameraEntry *entry, const UacMode *mode, MicControl control,
                                  void *context, uint8_t *data);
bool microphone_capture_valid(MicRateResult rate, const MicPcm *pcm, uint64_t span_ms);
void microphone_pcm_init(MicPcm *pcm, uint8_t *data, size_t capacity);
bool microphone_pcm_packet(MicPcm *pcm, const uint8_t *data, size_t length, unsigned packet_cap);
bool microphone_wav(FILE *file, const MicPcm *pcm);
#endif
