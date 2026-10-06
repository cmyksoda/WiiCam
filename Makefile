DEVKITPRO ?= /opt/devkitpro
DEVKITPPC ?= $(DEVKITPRO)/devkitPPC
export DEVKITPRO DEVKITPPC
include $(DEVKITPPC)/wii_rules

.DEFAULT_GOAL := all
CC := $(DEVKITPPC)/bin/powerpc-eabi-gcc
LD := $(CC)
CFLAGS := -O2 -g -std=gnu11 -Wall -Wextra -Werror $(MACHDEP) \
          -I$(LIBOGC_INC) -I$(DEVKITPRO)/portlibs/ppc/include -MMD -MP
LDFLAGS := $(MACHDEP) -Wl,-Map,build/wc04-camera.map
LIBPATHS := -L$(LIBOGC_LIB) -L$(DEVKITPRO)/portlibs/ppc/lib
LIBS := -ljpeg -lfat -lwiiuse -lbte -logc -lm
OBJECTS := $(patsubst source/%.c,build/%.o,$(wildcard source/*.c)) build/usb.o

.PHONY: all clean test package
all: dist/apps/wc04-camera/boot.dol

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
test: | build
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Isource source/uvc.c tests/test_uvc.c -o build/test_uvc
	./build/test_uvc
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -Isource source/log.c tests/test_log.c -o build/test_log
	./build/test_log

.PHONY: test-photo
test-photo: | build
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror $(JPEG_CFLAGS) -Isource source/photo.c tests/test_photo.c $(JPEG_LIBS) -o build/test_photo
	./build/test_photo

package: all
	python3 tools/package.py

clean:
	rm -rf build dist/apps/wc04-camera/boot.dol

-include $(OBJECTS:.o=.d)
