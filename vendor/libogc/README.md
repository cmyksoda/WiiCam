# Local libogc USB adaptation

`usb.c` is from devkitPro/libogc commit `ed9b8fcf77a696a3c0e9ee727cc18e5d9e221b7c`. The original file's SHA-256 is `1c2e69df6e5a55d802d4095c8d3b807aff910bf3f605b670d803cff4e6dc78b7`; its source URL is below.

Changes are limited to an altered-version notice and two appended functions:

- `WC04_SetAlternate`: vendor ioctl 7, using libogc's existing vendor handle and an aligned IPC input allocation.
- `WC04_CancelIso`: vendor ioctl 17 with the actual endpoint address.

Existing USB functions are unchanged. The original license header is retained. The two additional functions are supplied under the same permissive license as this file.

Why vendor this file: the vendor host handle is private to libogc; its public alternate-setting helper sends a USB control request, and its public halt helper does not expose cancellation of an arbitrary isochronous endpoint. Keeping the wrappers beside the private handle avoids an extra competing IOS client or a system-wide devkitPro modification.

The Makefile compiles this translation unit into the executable before the static libraries. It suppresses an existing signed-comparison warning only for this upstream file. All original application files build with `-Wall -Wextra -Werror`.

Reference: [upstream source](https://github.com/devkitPro/libogc/blob/ed9b8fcf77a696a3c0e9ee727cc18e5d9e221b7c/libogc/usb.c).
