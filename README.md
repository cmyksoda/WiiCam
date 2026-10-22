# ![WiiCam logo](assets/logo.png)

A **Wii USB camera app**, inspired by [ZodiaKGalXy's GBAtemp request](https://gbatemp.net/threads/homebrew-application-for-ubisoft-hercules-wc04-webcam.683135/).

## Tested cameras

| Camera | Photo | Video | Sound |
|---|---|---|---|
| Ubisoft / Hercules WC04 | Yes | Yes | Not tested |
| Generic W2 1080p webcam (USB ID 0c45:6366) | Yes | Yes | Yes, at every size |
| Logitech C920 PRO HD (USB ID 046d:08e5) | Not detected yet | Not detected yet | Not detected yet |

The Wii's USB system (IOS58) doesn't list the C920 at all, so WiiCam can't see it yet. Work on supporting it is tracked in [issue #1](https://github.com/cmyksoda/WiiCam/issues/1).

Other UVC 1.0 MJPEG cameras may work. If you try one, please open an issue with the camera's name, its USB ID, and whether photos, video and sound worked. The `wc04-camera/session.log` file on your SD card helps a lot.

## Features

- Live preview and JPEG photos, from 160×120 up to 848×480 depending on the camera.
- AVI video, silent or with sound, up to 60 seconds or 128 MiB.
- Instant photos or 3-, 5-, and 10-second timers.
- Review after each photo or video: play videos back or delete them.
- Camera refresh and 4:3 or widescreen display.
- Photo Channel compatibility: photos ~~and videos~~ are saved in folders the Photo Channel can see.

*Videos were supported in pre-release versions, they are currently unreliable, with different cameras posting varying results. this is a known regression and is tracked in [issue #2](https://github.com/cmyksoda/WiiCam/issues/2).*

## Install

Copy the ZIP’s `apps/wiicam` folder onto your SD card.

Launch WiiCam from the Homebrew Channel with IOS58.

Photos are saved to `SD:/DCIM/100WC04` and videos to `SD:/wc04-camera/videos`.

## Recording with sound

In testing, sound works at every resolution. However, it only stays available at the size the camera first opened with. To record sound at another size, restart WiiCam; the menu tells you when.

Sound needs a camera with a USB microphone that records 16 kHz mono.

## Controls

- **A:** open the viewfinder, take a photo, or start/stop video.
- **B:** return to the menu, cancel the timer, or stop a video and go straight to the menu.
- **HOME:** exit.
- **Menu:** **+** changes the mode (Photo, Video, Video + audio), **1** changes the size, **Left/Right** sets the timer, and **2** refreshes the camera.
- **Review:** **1** plays the video, **Minus** twice deletes the photo or video, **A** returns to the viewfinder, and **B** returns to the menu.

## Credits

- **libogc**, **libfat**, and **devkitPPC** by devkitPro, with **libdvm** and ChaN's **FatFs** for the SD card
- **libasnd** by Hermes, for sound playback
- **libjpeg-turbo**, based in part on the work of the Independent JPEG Group
- **wiiuse**, for the Wii Remote
- The interface font, FOT-RodinNTLG Pro DB, © 2003 Fontworks Japan, Inc. RodinNTLGPro is a trademark of Fontworks Japan, Inc.
- The HOME button glyph, from Nintendo's Wii Operations Manual

## License

WiiCam is free software under the [GNU General Public License, version 3 or later](LICENSE). Some parts use other licenses: the modified libogc USB source in `vendor/libogc` and the libraries keep their own licenses, and the interface font and HOME button glyph belong to Fontworks and Nintendo and are not covered by the GPL. All the texts are in [`dist/apps/wiicam/licenses/`](dist/apps/wiicam/licenses/).

WiiCam isn't affiliated with Nintendo, Fontworks, Ubisoft, or Guillemot.

---

*This project was made with AI assistance. For more information, see [my AI usage statement](https://github.com/cmyksoda/cmyksoda/blob/main/AI_USAGE.md).*
