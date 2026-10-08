# WC04 Camera

An **experimental Wii photo-capture prototype** for the Ubisoft / Hercules WC04 USB camera, based on [ZodiaKGalXy's GBAtemp request](https://gbatemp.net/threads/homebrew-application-for-ubisoft-hercules-wc04-webcam.683135/).

**Status:** ZodiaKGalXy's physical Wii test of **0.1.3** successfully saved both 320×240 and 640×480 photos and displayed them in the Photo Channel. Its in-app preview showed incorrect colors. **0.2.0-experimental** replaces that display path and adds a live viewfinder. The new build passes compilation and host tests; its TV colors, frame rate, and sustained capture still need a Wii test.

## What is implemented

- WC04-only discovery (`057e:030a`), selecting the video-streaming interface.
- UVC 1.0 MJPEG descriptor parsing and probe/commit negotiation.
- Queued IOS58 isochronous input, frame assembly, error rejection, and cancellation.
- 320×240 or 640×480 live preview and photo capture.
- JPEG decoding followed by baseline JPEG encoding to SD.
- A graphical camera screen, live viewfinder, and immediate saved-photo review.
- Double-buffered color display with 4:3 and widescreen framing.
- Timestamped diagnostic logging, preview timing, and on-screen status messages.

It does not implement motion tracking, audio, movie recording, YUY2 capture, arbitrary webcams, USB-drive storage, or direct Photo Channel/NAND modification.

## Install and use

1. Copy `dist/apps/wc04-camera` to the SD card's `apps` directory, or extract the experimental ZIP to the SD root.
2. Connect the WC04 directly to a Wii USB port and insert a writable FAT-formatted SD card.
3. Launch the app from the Homebrew Channel under **IOS58**. The app logs the running IOS and refuses camera operations under other versions. It never installs or reloads an IOS.
4. Press **A** to open the viewfinder, then **A** again to take a photo. **B** returns to the menu; **HOME** exits.
5. In the menu, **A/B** opens the viewfinder, **1** switches between 320×240 and 640×480. **2 / CAMERA CHECK** checks the connection and records supported camera modes in the log; it does not take a photo.
6. A saved photo appears immediately. **A** reopens the live viewfinder; **B** returns to the menu.

Photos: `sd:/DCIM/100WC04/WC04####.JPG`. Existing files are never overwritten. The 9999-file directory limit follows the chosen four-digit filename scheme; move photos elsewhere when it fills.

Logs: `sd:/wc04-camera/session.log` (appended across runs). Each nonblank line includes the Wii clock and monotonic elapsed seconds with milliseconds since app start. The wall clock depends on the console's setting. Saving needs writable SD; scanning and the viewfinder can run without it. Insert SD before launching to enable saving. Packet prefixes and statistics are collected in memory and logged after reception, avoiding diagnostic SD writes during streaming.

For this build, leave the viewfinder running for at least 30 seconds at each resolution, take a photo, and compare its colors in the app and Photo Channel. Also test B followed by reopening, and HOME from the live view. Include the app version, session log, and whether movement was smooth or choppy. Preview brightness may change as the camera adapts; this build does not explicitly adjust exposure or white balance.

For the Photo Channel, exit to the Wii Menu and open the channel's SD-card photos. The output is baseline JPEG, matching [Nintendo's documented image format](https://www.nintendo.com/en-gb/Support/Legacy-system/File-types-compatible-with-the-Photo-Channel-239918.html). The tester confirmed actual Photo Channel playback for both resolutions in 0.1.3.

Choose **Digital Camera/Cell Phone** in the Photo Channel to read the SD card, as described in [Nintendo's access instructions](https://en-americas-support.nintendo.com/app/answers/detail/a_id/2958/p/604/c/950). These instructions specify the photo source and compatible formats, not a required folder name. The app uses its DCIM subfolder to keep photographs together; it does not register them with a channel database. Nintendo documents [approximately 1,000 visible photos](https://www.nintendo.com/en-gb/Support/Legacy-system/Photo-Channel-More-about-viewing-your-photos-245163.html), distinct from this app's 9999-filename capacity.

The viewfinder runs until a photo, B, HOME, or error. A ten-second deadline applies when no frame has decoded, including at startup or after the stream stalls. An in-flight IOS transfer must finish before its buffers can be reused or the app can exit. Cancellation requests the IOS endpoint cancellation ioctl; **if the screen says it is waiting, unplug the camera**. Control requests and the cancellation ioctl are synchronous and have no app-enforced deadline. A wedged IOS may still require a console restart.

The receiver queues four separate, aligned buffers of sixteen slots each.
It processes them in submission order and reuses a buffer only after its
completion callback. Success, cancellation, timeout, and USB failure all
drain every submitted transfer before returning. Logs include submitted,
consumed, and drained batch counts. The maximum consume gap measures CPU-side
processing intervals; it is not a USB bus timing measurement. Zero invalid
JPEGs can also mean no complete frame reached the decoder.

The USB thread keeps the existing four-transfer pipeline fed. A lower-priority worker decodes copied JPEG frames and presents at most about 15 preview frames per second; actual speed depends on the Wii. Frames arriving while that worker is busy are skipped for preview without overwriting its input. A shutter press selects the next frame accepted by the worker. A failed decode retries; a successful shutter frame remains untouched until USB drains and the SD save finishes. The saved JPEG retains the selected camera resolution, independent of preview size.

The display uses packed 32-bit Y0/Cb/Y1/Cr words in cached framebuffers, flushes them before presentation, and waits for the display to switch before reusing a buffer. The word layout follows [libogc's framebuffer renderer](https://github.com/devkitPro/libogc/blob/master/libogc/console.c). Host tests verify reference colors, luminance ordering, scaling, framebuffer bounds, and concurrent preview ownership. This replaces the old uncached byte stores; it is not yet proof of the reported TV color fix.

## Build

Requires devkitPPC, libogc, and the PowerPC libjpeg-turbo portlib. This checkout built with devkitPPC GCC 16.1.0, libogc 3.1.0, and libjpeg-turbo 3.1.4.1.

```sh
make -j4
make test
make test-photo                 # Host libjpeg-turbo development headers/library
make package
```

`DEVKITPRO` defaults to `/opt/devkitpro`; override it if necessary. `HOST_CC`, `JPEG_CFLAGS`, and `JPEG_LIBS` configure the host tests. The host's test JPEG library is separate from the library linked into the Wii binary.

Negotiation reads a 26-byte UVC 1.0 probe block using `GET_MAX`, with
`GET_DEF` and `GET_CUR` fallbacks. It preserves the returned compression and
timing fields while selecting the requested format, frame, and interval,
following [libuvc's initialization approach](https://github.com/libuvc/libuvc/blob/d07de4fa23905ee8cac06f5d24b2a03d42a3b363/src/stream.c).
Only a complete, validated `GET_CUR` response is committed. These negotiation settings and the four-transfer USB queue are unchanged
from the successful 0.1.3 hardware test.

The build uses a pinned copy of libogc's USB implementation with two added functions. It does **not** modify `/opt/devkitpro` or other projects. See [the vendor note](vendor/libogc/README.md).

`make package` creates `dist/WC04-Camera-0.2.0-experimental.zip`. Build outputs and ZIPs are not tracked in Git. `tools/inspect_wc04.py` optionally collects Linux USB descriptors for the WC04 without starting a video stream.

## Credits and license

Original application code is licensed under the GNU General Public License, version 3 only (`GPL-3.0-only`); see [LICENSE](LICENSE). Copyright (c) 2026 cmyksoda. The modified libogc USB source retains its own license and author credits. Third-party runtime notices are in `dist/apps/wc04-camera/licenses`.

This software is based in part on the work of the Independent JPEG Group and uses libjpeg-turbo. Nintendo, Ubisoft, and Guillemot are not associated with this project.
