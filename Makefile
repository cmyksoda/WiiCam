DEVKITPRO ?= /opt/devkitpro
DEVKITPPC ?= $(DEVKITPRO)/devkitPPC
export DEVKITPRO DEVKITPPC
include $(DEVKITPPC)/wii_rules

.DEFAULT_GOAL := all
CC := $(DEVKITPPC)/bin/powerpc-eabi-gcc
LD := $(CC)
CFLAGS := -O2 -g -std=gnu11 -Wall -Wextra -Werror $(MACHDEP) \
          -Ibuild -I$(LIBOGC_INC) -I$(DEVKITPRO)/portlibs/ppc/include -MMD -MP
LDFLAGS := $(MACHDEP) -Wl,-Map,build/wc04-camera.map
LIBPATHS := -L$(LIBOGC_LIB) -L$(DEVKITPRO)/portlibs/ppc/lib
LIBS := -ljpeg -lfat -lwiiuse -lbte -logc -lm
OBJECTS := $(patsubst source/%.c,build/%.o,$(wildcard source/*.c)) build/usb.o

.PHONY: all clean test package
all: dist/apps/wiicam/boot.dol

WC04_FONT ?=

build/ui_assets.h: tools/build_ui_assets.py assets/wii-home-glyph-mask.png assets/color-bars.png | build
	python3 tools/build_ui_assets.py --font "$(WC04_FONT)"

build/screen.o: build/ui_assets.h

build/%.o: source/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/usb.o: vendor/libogc/usb.c | build
	$(CC) $(CFLAGS) -Wno-sign-compare -I$(LIBOGC_INC)/ogc -I$(LIBOGC_INC)/ogc/machine -c $< -o $@

build:
	mkdir -p $@

build/wc04-camera.elf: $(OBJECTS)
	$(LD) $^ $(LDFLAGS) $(LIBPATHS) $(LIBS) -o $@

dist/apps/wiicam/boot.dol: build/wc04-camera.elf
	$(DEVKITPRO)/tools/bin/elf2dol $< $@

HOST_CC ?= cc
JPEG_CFLAGS ?=
JPEG_LIBS ?= -ljpeg
test: build/ui_assets.h | build
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pthread -Isource source/audio_stream.c source/av_avi.c source/av_recorder.c source/storage.c tests/test_av.c -o build/test_av
	./build/test_av
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Isource source/uvc.c source/uac.c source/microphone.c tests/test_microphone.c -o build/test_microphone
	./build/test_microphone
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Isource source/uvc.c source/uac.c tests/test_uac.c -o build/test_uac
	./build/test_uac
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Isource source/uvc.c source/uac.c source/camera_inventory.c tests/test_camera_inventory.c -o build/test_camera_inventory
	./build/test_camera_inventory
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pthread -Isource source/refresh_control.c tests/test_refresh_control.c -o build/test_refresh_control
	./build/test_refresh_control
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Isource source/uvc.c tests/test_uvc_diagnostics.c -o build/test_uvc_diagnostics
	./build/test_uvc_diagnostics
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Isource source/uvc.c tests/test_uvc_generic.c -o build/test_uvc_generic
	./build/test_uvc_generic
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Isource source/uvc.c source/uac.c source/camera_inventory.c source/camera.c tests/test_camera.c -o build/test_camera
	./build/test_camera
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Isource source/avi.c tests/test_avi.c -o build/test_avi
	./build/test_avi
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Isource source/storage.c tests/test_storage.c -o build/test_storage
	./build/test_storage
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Isource source/capture_control.c tests/test_capture_control.c -o build/test_capture_control
	./build/test_capture_control
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Ibuild -Isource source/screen.c tests/test_screen.c -o build/test_screen
	./build/test_screen
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -pthread -Isource source/preview.c tests/test_preview.c -o build/test_preview
	./build/test_preview
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Isource source/uvc.c tests/test_uvc.c -o build/test_uvc
	./build/test_uvc
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Isource source/uvc.c tests/test_probe.c -o build/test_probe
	./build/test_probe
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Isource source/iso_queue.c tests/test_iso_queue.c -o build/test_iso_queue
	./build/test_iso_queue
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Isource source/log.c tests/test_log.c -o build/test_log
	./build/test_log

.PHONY: test-photo test-recording test-framing
test-recording: | build
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror $(JPEG_CFLAGS) -Isource source/photo.c source/avi.c source/storage.c source/recorder.c tests/test_recorder.c $(JPEG_LIBS) -o build/test_recorder
	./build/test_recorder
	python3 tools/check_recordings.py

test-framing: | build
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror $(JPEG_CFLAGS) -Isource source/uvc.c source/preview.c source/photo.c source/avi.c source/storage.c source/recorder.c tests/test_frame_pipeline.c $(JPEG_LIBS) -o build/test_frame_pipeline
	./build/test_frame_pipeline

test-photo: | build
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror $(JPEG_CFLAGS) -Isource source/photo.c tests/test_photo.c $(JPEG_LIBS) -o build/test_photo
	./build/test_photo

package:
	@echo "This build contains local font-derived assets. Use make package-local for a private test only."
	@exit 1

.PHONY: package-local ui-assets
ui-assets: | build
	python3 tools/build_ui_assets.py --font "$(WC04_FONT)"

package-local: all
	python3 tools/package.py --local-test

clean:
	rm -rf build dist/apps/wiicam/boot.dol

-include $(OBJECTS:.o=.d)

.PHONY: preview-ui
preview-ui: build/ui_assets.h
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Ibuild -Isource source/screen.c source/refresh_control.c tests/render_ui.c -o build/render_ui
	./build/render_ui
	python3 tools/preview_ui.py

.PHONY: test-export
test-export: test-recording
	python3 tests/test_export_mp4.py

.PHONY: test-modes
test-modes: build/ui_assets.h | build
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror $(JPEG_CFLAGS) -Ibuild -Isource source/uvc.c source/photo.c source/avi.c source/screen.c tests/test_video_modes.c $(JPEG_LIBS) -o build/test_video_modes
	./build/test_video_modes

.PHONY: test-av-media
test-av-media: | build
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror $(JPEG_CFLAGS) -Isource source/photo.c source/av_avi.c tests/test_av_media.c $(JPEG_LIBS) -o build/test_av_media
	./build/test_av_media
	python3 tools/check_av.py
