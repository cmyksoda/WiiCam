/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WIICAM_MICROPHONE_H
#define WIICAM_MICROPHONE_H
#include "camera.h"
#include "uac.h"

#define MIC_RATE 16000u
#define MIC_MAX_CHANNELS 2u
#define MIC_MAX_DECIMATION 3u

typedef struct {
    CameraEntry camera;
    uint32_t token;
} MicEntry;

/* IDs are IOS interface handles. Require one unambiguous video/audio pair. */
bool microphone_match(const MicEntry *entries, size_t count, int32_t video_id,
                      unsigned video_interface, unsigned audio_interface, size_t *index);
bool microphone_parameters(const CameraEntry *entry, const uint8_t *parameters,
                           unsigned configuration);
/* Mono or stereo at a whole multiple of MIC_RATE; the cheapest qualifying alternate wins. */
bool microphone_mode(const uint8_t *config, size_t length, UacMode *mode);
/* One line per advertised audio streaming alternate, for logs when no mode qualifies. */
size_t microphone_summary(const uint8_t *config, size_t length, char *text, size_t size);
/* Callback returns payload bytes; the Wii adapter checks IOS setup-byte counts. */
typedef int (*MicControl)(void *, uint8_t type, uint8_t request, uint16_t value, uint16_t index,
                          uint8_t *data, unsigned length);

typedef enum { MIC_RATE_FAILED, MIC_RATE_OK, MIC_RATE_UNREADABLE } MicRateResult;

MicRateResult microphone_set_rate(const CameraEntry *entry, const UacMode *mode, MicControl control,
                                  void *context, uint8_t *data);
#endif
