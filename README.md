# WC04 Camera

An **experimental Wii photo-capture prototype** for the Ubisoft / Hercules WC04 USB camera, based on [ZodiaKGalXy's GBAtemp request](https://gbatemp.net/threads/homebrew-application-for-ubisoft-hercules-wc04-webcam.683135/).

**Status:** physical Wii testing of 0.1.1 received valid UVC headers but **no image payload** at either resolution. Version **0.1.2-experimental** initializes negotiation from camera-provided parameters and logs the full probe/commit exchange. Wii compilation and host tests pass; successful photo capture and Photo Channel playback remain unverified.

## What is implemented

- WC04-only discovery (`057e:030a`), selecting the video-streaming interface.
- UVC 1.0 MJPEG descriptor parsing and probe/commit negotiation.
- IOS58 isochronous input, frame assembly, error rejection, and cancellation.
- 320×240 or 640×480 single-photo capture.
- JPEG decoding followed by baseline JPEG encoding to SD.
- Preview of the captured photograph after saving; **no live viewfinder**.
- Timestamped diagnostic logging, packet summaries, and on-screen USB errors.

It does not implement motion tracking, audio, movie recording, YUY2 capture, arbitrary webcams, USB-drive storage, or direct Photo Channel/NAND modification.

## Install and use

1. Copy `dist/apps/wc04-camera` to the SD card's `apps` directory, or extract the experimental ZIP to the SD root.
2. Connect the WC04 directly to a Wii USB port and insert a writable FAT-formatted SD card.
3. Launch the app from the Homebrew Channel under **IOS58**. The app prints the running IOS and refuses camera operations under other versions. It never installs or reloads an IOS.
4. Press **2** to scan and record descriptors. Press **A** to take a photo. **1** switches between 320×240 and 640×480. **B** cancels capture. **HOME** exits.
5. After a successful save, press **A** to view the photograph; **A/B** returns to the menu.

Photos: `sd:/DCIM/100WC04/WC04####.JPG`. Existing files are never overwritten. The 9999-file directory limit follows the chosen four-digit filename scheme; move photos elsewhere when it fills.

Logs: `sd:/wc04-camera/session.log` (appended across runs). Each nonblank line includes the Wii clock and monotonic elapsed seconds with milliseconds since app start. The wall clock depends on the console's setting. Capture needs writable SD; scanning can still run without it. Packet prefixes and statistics are collected in memory and logged after reception, avoiding diagnostic SD writes during streaming.

When reporting capture problems, try A at 320×240 without pressing B, repeat at 640×480, then cancel a separate attempt with B. Include the app version, steps taken, and session log.

For the Photo Channel, exit to the Wii Menu and open the channel's SD-card photos. The output is baseline JPEG, matching [Nintendo's documented image format](https://www.nintendo.com/en-gb/Support/Legacy-system/File-types-compatible-with-the-Photo-Channel-239918.html). **Compatibility with the actual channel still needs testing.**

Choose **Digital Camera/Cell Phone** in the Photo Channel to read the SD card, as described in [Nintendo's access instructions](https://en-americas-support.nintendo.com/app/answers/detail/a_id/2958/p/604/c/950). These instructions specify the photo source and compatible formats, not a required folder name. The app uses its DCIM subfolder to keep photographs together; it does not register them with a channel database. Nintendo documents [approximately 1,000 visible photos](https://www.nintendo.com/en-gb/Support/Legacy-system/Photo-Channel-More-about-viewing-your-photos-245163.html), distinct from this app's 9999-filename capacity.

Capture attempts have a ten-second application deadline. An in-flight IOS transfer must finish before its buffers can be reused or the app can exit. Cancellation requests the IOS endpoint cancellation ioctl; **if the screen says it is waiting, unplug the camera**. Control requests and the cancellation ioctl are synchronous and have no app-enforced deadline. A wedged IOS may still require a console restart.

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
Only a complete, validated `GET_CUR` response is committed. This is a
compatibility change to test; receiving headers alone does not establish
which camera or host behavior prevents image delivery.

The build uses a pinned copy of libogc's USB implementation with two added functions. It does **not** modify `/opt/devkitpro` or other projects. See [the vendor note](vendor/libogc/README.md).

`make package` creates `dist/WC04-Camera-0.1.2-experimental.zip`. Build outputs and ZIPs are not tracked in Git. `tools/inspect_wc04.py` optionally collects Linux USB descriptors for the WC04 without starting a video stream.

## Credits and license

Original application code is licensed under the GNU General Public License, version 3 only (`GPL-3.0-only`); see [LICENSE](LICENSE). Copyright (c) 2026 cmyksoda. The modified libogc USB source retains its own license and author credits. Third-party runtime notices are in `dist/apps/wc04-camera/licenses`.

This software is based in part on the work of the Independent JPEG Group and uses libjpeg-turbo. Nintendo, Ubisoft, and Guillemot are not associated with this project.
