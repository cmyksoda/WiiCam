/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef WC04_FRAME_LIMITS_H
#define WC04_FRAME_LIMITS_H

/* Bounded MJPEG overhead: observed 640x480 descriptor requests 614989 bytes. */
#define WC04_FRAME_HEADROOM 1024u
#define WC04_FRAME_CAPACITY (640u * 480u * 2u + WC04_FRAME_HEADROOM)

#endif
