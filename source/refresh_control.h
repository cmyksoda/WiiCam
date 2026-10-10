/* SPDX-License-Identifier: GPL-3.0-only */
#ifndef WC04_REFRESH_CONTROL_H
#define WC04_REFRESH_CONTROL_H
#include <stdbool.h>
#include <stdatomic.h>
#include <stddef.h>

typedef enum {
    REFRESH_BUSY,
    REFRESH_CANCELLING,
    REFRESH_FOUND,
    REFRESH_MISSING,
    REFRESH_CANCELLED
} RefreshResult;

typedef struct {
    atomic_bool complete;
    bool found, cancelled;
} RefreshControl;

void refresh_control_init(RefreshControl *control);
/* Worker publishes only after discovery has returned and its handle is closed. */
void refresh_control_complete(RefreshControl *control, bool found);
/* UI thread owns cancellation; a late success cannot undo it. */
RefreshResult refresh_control_poll(RefreshControl *control, bool cancel);
void refresh_status(char *text, size_t capacity, RefreshResult result, unsigned width,
                    unsigned height);
#endif
