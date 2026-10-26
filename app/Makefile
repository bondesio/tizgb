# TizGB native TPK. Same toolchain, freestanding build and TV-pulled link
# libraries as TizNES (nes-emulator/app/Makefile).
CC  := $(HOME)/tizen-studio/tools/arm-linux-gnueabi-gcc-9.2/bin/arm-tizen-linux-gnueabi-gcc-9.2.0
TIZEN := $(HOME)/tizen-studio/tools/ide/bin/tizen
LIBGCC := $(shell $(CC) -print-libgcc-file-name)

VERSION := 1.2.0
PACKAGE := org.pablo.tizgb-$(VERSION)-arm.tpk
EXE := tizgb
VEN := ../vendor/Walnut-CGB

# Read-only link inputs copied from the TV firmware (not packaged).
TV_LIBS := libcapi-appfw-application.so.0 libelementary.so.1 libevas.so.1 \
           libecore.so.1 libecore_input.so.1 libeina.so.1 \
           libcapi-media-audio-io.so.0 libstorage.so.0.1 libc.so.6

CFLAGS := -nostdinc -I include -std=gnu11 -O2 -fPIE -fno-strict-aliasing -c -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable

.PHONY: all clean refresh-tv-libs probe library-probe
all: out/$(PACKAGE)

build/tv-libs/.ready: Makefile
	mkdir -p build/tv-libs
	ssh q60a-tv-root 'tar -chf - $(addprefix /usr/lib/,$(TV_LIBS)) 2>/dev/null' | tar -xf - -C build/tv-libs --strip-components=2
	touch $@

bin/platform.o: platform.c $(VEN)/walnut_cgb.h $(VEN)/minigb_apu/minigb_apu.c $(VEN)/minigb_apu/minigb_apu.h $(wildcard include/*.h)
	mkdir -p bin
	$(CC) $(CFLAGS) $< -o $@

bin/gb_main.o: gb_main.c library.h
	mkdir -p bin
	$(CC) $(CFLAGS) $< -o $@

bin/library.o: library.c library.h
	mkdir -p bin
	$(CC) $(CFLAGS) $< -o $@

bin/start.o: start.S
	mkdir -p bin
	$(CC) -nostdinc -c $< -o $@

bin/$(EXE): bin/start.o bin/gb_main.o bin/library.o bin/platform.o build/tv-libs/.ready
	$(CC) -nostdlib -pie -Wl,-e,_start -Wl,--dynamic-linker=/lib/ld-linux.so.3 -Wl,--allow-shlib-undefined \
	  -o $@ bin/start.o bin/gb_main.o bin/library.o bin/platform.o \
	  $(addprefix build/tv-libs/,$(TV_LIBS)) $(LIBGCC)

out/$(PACKAGE): bin/$(EXE) tizen-manifest.xml shared/res/icon.png
	rm -rf stage
	mkdir -p stage/bin stage/shared/res out
	cp bin/$(EXE) stage/bin/$(EXE)
	cp tizen-manifest.xml stage/tizen-manifest.xml
	cp shared/res/icon.png stage/shared/res/icon.png
	rm -f $@
	cd stage && zip -q -r ../$@ bin shared tizen-manifest.xml
	$(TIZEN) package -t tpk -s tvdev -- $@

# Disposable EVIOCGRAB probe for tools/deferred-tv-checks.sh (not packaged).
probe: bin/grab-probe
bin/grab-probe: ../tools/grab-probe.c bin/start.o build/tv-libs/.ready
	$(CC) -nostdinc -O2 -fPIE -c $< -o bin/grab-probe.o
	$(CC) -nostdlib -pie -Wl,-e,_start -Wl,--dynamic-linker=/lib/ld-linux.so.3 -Wl,--allow-shlib-undefined \
	  -o $@ bin/start.o bin/grab-probe.o build/tv-libs/libc.so.6 $(LIBGCC)

# Read-only on-TV check of the real library.c (tools/library-probe.c): the
# Storage API roots, the stat64 layout and a synthetic deep/looping tree.
# Its "internal" root is /tmp/tizgb-libprobe, never the app's data folder.
library-probe: bin/library-probe
bin/library-probe: ../tools/library-probe.c library.c library.h bin/start.o build/tv-libs/.ready
	$(CC) $(CFLAGS) -I . ../tools/library-probe.c -o bin/library-probe.o
	$(CC) $(CFLAGS) -DTIZGB_DATA_DIR='"/tmp/tizgb-libprobe"' library.c -o bin/library-probe-lib.o
	$(CC) -nostdlib -pie -Wl,-e,_start -Wl,--dynamic-linker=/lib/ld-linux.so.3 -Wl,--allow-shlib-undefined \
	  -o $@ bin/start.o bin/library-probe.o bin/library-probe-lib.o \
	  build/tv-libs/libstorage.so.0.1 build/tv-libs/libc.so.6 $(LIBGCC)

clean:
	rm -rf bin stage out

refresh-tv-libs:
	rm -rf build/tv-libs
	$(MAKE) build/tv-libs/.ready
