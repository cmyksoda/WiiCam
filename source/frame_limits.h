/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef WC04_FRAME_LIMITS_H
#define WC04_FRAME_LIMITS_H

/* Observed 848x480 descriptor requests 814669 bytes; retain bounded overhead. */
#define WC04_MAX_WIDTH 848u
#define WC04_MAX_HEIGHT 480u
#define WC04_RGB_CAPACITY (WC04_MAX_WIDTH * WC04_MAX_HEIGHT * 3u)
#define WC04_FRAME_HEADROOM 1024u
#define WC04_FRAME_CAPACITY (WC04_MAX_WIDTH * WC04_MAX_HEIGHT * 2u + WC04_FRAME_HEADROOM)

#endif
