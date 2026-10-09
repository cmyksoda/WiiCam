/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (c) 2026 cmyksoda
 */
#include "camera.h"
#include <string.h>

static void report(const CameraOps *ops, const CameraEntry *entry, const char *message)
{
    if (ops->report)
        ops->report(ops->context, entry, message);
}

static bool describe(const CameraEntry *entry, const uint8_t *data, uint8_t *configuration)
{
    /* USBV5 GETDEVPARAMS: 20-byte IOS prefix, then word-aligned standard
     * device/config/interface descriptors. Read only the bounded first view,
     * unlike the SDK parser which follows device counts through this reply. */
    const uint8_t *device = data + 20, *config = data + 40, *interface = data + 52;
    if (device[0] != 18 || device[1] != 1 || device[17] != 1 ||
        uvc_le16(device + 8) != entry->vid || uvc_le16(device + 10) != entry->pid ||
        config[0] != 9 || config[1] != 2 || !config[4] || !config[5] ||
        interface[0] != 9 || interface[1] != 4 || interface[2] != entry->interface ||
        interface[5] != 14 || interface[6] != 2)
        return false;
    *configuration = config[5];
    return true;
}

bool camera_find(const CameraEntry *entries, size_t count, unsigned width, unsigned height,
                  uint8_t *scratch, size_t capacity, const CameraOps *ops,
                  int32_t *handle, UvcMode *mode)
{
    if (!entries || count > CAMERA_MAX_DEVICES || !scratch || capacity < CAMERA_CONFIG_CAPACITY ||
        !ops || !ops->open || !ops->parameters || !ops->read || !ops->close || !handle || !mode)
        return false;
    /* Deterministic preference for WC04, without excluding other identities. */
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (size_t i = 0; i < count; ++i) {
            const CameraEntry *entry = &entries[i];
            bool wc04 = entry->vid == 0x057e && entry->pid == 0x030a;
            if (wc04 != (pass == 0))
                continue;
            int32_t opened = 0;
            if (ops->open(ops->context, entry, &opened) < 0) {
                report(ops, entry, "open failed");
                continue;
            }
            memset(scratch, 0, CAMERA_IOS_PARAMETERS);
            uint8_t configuration = 0;
            if (ops->parameters(ops->context, opened, scratch) != 0 ||
                !describe(entry, scratch, &configuration)) {
                report(ops, entry, "not a valid single-configuration VideoStreaming interface");
                ops->close(ops->context, opened);
                continue;
            }
            memset(scratch, 0, CAMERA_CONFIG_CAPACITY);
            int result = ops->read(ops->context, opened, 8, 0, 1, scratch);
            if (uvc_control_payload_length(result, 1) != 1 || scratch[0] != configuration) {
                report(ops, entry, "configuration inactive or unreadable; no configuration change attempted");
                ops->close(ops->context, opened);
                continue;
            }
            memset(scratch, 0, CAMERA_CONFIG_CAPACITY);
            result = ops->read(ops->context, opened, 6, 0x0200, 9, scratch);
            unsigned length = uvc_le16(scratch + 2);
            if (uvc_control_payload_length(result, 9) != 9 || scratch[0] != 9 || scratch[1] != 2 ||
                length < 9 || length > CAMERA_CONFIG_CAPACITY || scratch[5] != configuration) {
                report(ops, entry, "configuration header short, malformed or beyond 4096 bytes");
                ops->close(ops->context, opened);
                continue;
            }
            memset(scratch, 0, CAMERA_CONFIG_CAPACITY);
            result = ops->read(ops->context, opened, 6, 0x0200, (uint16_t)length, scratch);
            if (uvc_control_payload_length(result, length) != (int)length || scratch[5] != configuration) {
                report(ops, entry, "configuration read short or changed");
                ops->close(ops->context, opened);
                continue;
            }
            UvcParseResult parsed = uvc_parse(scratch, length, entry->interface, width, height, mode);
            report(ops, entry, uvc_parse_description(parsed));
            if (parsed != UVC_OK) {
                ops->close(ops->context, opened);
                continue;
            }
            *handle = opened;
            return true;
        }
    }
    memset(mode, 0, sizeof(*mode));
    return false;
}
