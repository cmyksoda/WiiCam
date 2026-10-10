/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WIICAM_UAC_H
#define WIICAM_UAC_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UAC_MAX_ENTITIES 16u
#define UAC_MAX_RATES 32u
#define UAC_MAX_ALTERNATES 16u

typedef struct {
    uint8_t control_interface, interface, alternate, endpoint, interval;
    uint8_t channels, terminal, microphone, endpoint_controls, lock_units;
    uint16_t packet_capacity, lock_delay;
    uint32_t rate, rates[UAC_MAX_RATES], minimum_rate, maximum_rate;
    unsigned rate_count;
    bool frequency_control;
} UacMode;

typedef enum {
    UAC_OK,
    UAC_MALFORMED,
    UAC_ASSOCIATION,
    UAC_VERSION,
    UAC_LIMIT,
    UAC_NO_MODE
} UacResult;

/* Validate one advertised UAC1 PCM16 alternate without opening devices or issuing controls.
 * Bus speed, transfer timing and physical interface ownership require separate validation. */
UacResult uac_parse(const uint8_t *data, size_t length, unsigned interface, unsigned alternate,
                    UacMode *mode);
bool uac_rate_supported(const UacMode *mode, uint32_t rate);
#endif
