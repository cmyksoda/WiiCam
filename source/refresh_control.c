/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "refresh_control.h"
#include <stdio.h>

void refresh_control_init(RefreshControl *c)
{
    atomic_init(&c->complete, false);
    c->found = false;
    c->cancelled = false;
}

void refresh_control_complete(RefreshControl *c, bool found)
{
    c->found = found;
    atomic_store_explicit(&c->complete, true, memory_order_release);
}

RefreshResult refresh_control_poll(RefreshControl *c, bool cancel)
{
    c->cancelled |= cancel;
    if (!atomic_load_explicit(&c->complete, memory_order_acquire))
        return c->cancelled ? REFRESH_CANCELLING : REFRESH_BUSY;
    return c->cancelled ? REFRESH_CANCELLED : c->found ? REFRESH_FOUND : REFRESH_MISSING;
}

void refresh_status(char *text, size_t capacity, RefreshResult result, unsigned width,
                    unsigned height)
{
    switch (result) {
    case REFRESH_BUSY:
        snprintf(text, capacity, "Refreshing camera source - B cancels");
        break;
    case REFRESH_CANCELLING:
        snprintf(text, capacity, "Finishing camera refresh - please wait");
        break;
    case REFRESH_FOUND:
        snprintf(text, capacity, "MJPEG %ux%u found - A opens viewfinder", width, height);
        break;
    case REFRESH_MISSING:
        snprintf(text, capacity, "No %ux%u mode found - connect camera, press 2", width, height);
        break;
    case REFRESH_CANCELLED:
        snprintf(text, capacity, "Refresh cancelled - 2 retries");
        break;
    }
}
