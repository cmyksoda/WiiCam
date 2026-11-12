#pragma once

#include <gctypes.h>

typedef enum {
    IOS_PATCH_APPLIED,       // the gate and buffer were changed in MEM2
    IOS_PATCH_NO_AHBPROT,    // no hardware access; nothing touched
    IOS_PATCH_WRONG_BUILD,   // the expected v6176 bytes were absent; nothing touched
    IOS_PATCH_BUF_NOT_EMPTY, // the target buffer region held data; nothing touched
} IosPatchResult;

// Lifts IOS58 v6176's 2,048-byte configuration-descriptor limit in memory so a
// larger camera (the C920, 2,409 bytes) is listed. RAM only, gone on IOS reload.
IosPatchResult ios_patch_config_limit(char *detail, u32 detail_len);

// Puts the gate and buffer pointers back to pristine if we patched. The patch
// aims IOS at WiiCam's own buffer, so this must run before WiiCam leaves, or a
// later app meets IOS still pointing at freed memory.
void ios_patch_restore(void);
