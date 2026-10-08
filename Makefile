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
all: dist/apps/wc04-camera/boot.dol

WC04_FONT ?=

build/ui_assets.h: tools/build_ui_assets.py assets/wc04-placeholder.png assets/color-bars.png | build
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

dist/apps/wc04-camera/boot.dol: build/wc04-camera.elf
	$(DEVKITPRO)/tools/bin/elf2dol $< $@

HOST_CC ?= cc
JPEG_CFLAGS ?=
JPEG_LIBS ?= -ljpeg
test: build/ui_assets.h | build
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

.PHONY: test-photo test-recording
test-recording: | build
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror $(JPEG_CFLAGS) -Isource source/photo.c source/avi.c source/storage.c source/recorder.c tests/test_recorder.c $(JPEG_LIBS) -o build/test_recorder
	./build/test_recorder
	python3 tools/check_recordings.py

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
	rm -rf build dist/apps/wc04-camera/boot.dol

-include $(OBJECTS:.o=.d)

.PHONY: preview-ui
preview-ui: build/ui_assets.h
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Ibuild -Isource source/screen.c tests/render_ui.c -o build/render_ui
	./build/render_ui
	python3 tools/preview_ui.py
