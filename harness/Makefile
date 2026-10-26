# Host (x86-64) harness. app/platform.c and app/gb_main.c are compiled
# exactly as for the TV: -nostdinc against app/include, hand-declared libc.
CC ?= gcc
APPFLAGS := -nostdinc -I ../app/include -std=gnu11 -O2 -fno-strict-aliasing -w -DTIZGB_DATA_DIR='"out/data"'
B := build

.PHONY: all core-tests apu-tests ui-tests clean
all: $(B)/core_test $(B)/core_test_df $(B)/apu_test $(B)/ui_test

$(B)/platform.o: ../app/platform.c ../vendor/Walnut-CGB/walnut_cgb.h ../vendor/Walnut-CGB/minigb_apu/minigb_apu.c
	mkdir -p $(B) out/data/saves
	$(CC) $(APPFLAGS) -c $< -o $@
$(B)/platform_df.o: ../app/platform.c
	mkdir -p $(B)
	$(CC) $(APPFLAGS) -DTIZGB_DUALFETCH -c $< -o $@
$(B)/core_test: core_test.c $(B)/platform.o
	$(CC) -O2 -o $@ $^
$(B)/core_test_df: core_test.c $(B)/platform_df.o
	$(CC) -O2 -o $@ $^

$(B)/apu_test: apu_test.c $(B)/platform.o
	$(CC) -O2 -o $@ $^ -lm

UIFLAGS := -nostdinc -I ../app/include -std=gnu11 -O2 -fno-strict-aliasing -w \
  -DTIZGB_DATA_DIR='"out/ui/data"' -DTIZGB_DEV_INPUT='"out/ui/dev"' -DTIZGB_SYS_INPUT='"out/ui/sys"' \
  -DTIZGB_MOUNTS='"out/ui/mounts"' -DTIZGB_MEDIA_FALLBACK1='"out/ui/media"' -DTIZGB_MEDIA_FALLBACK2='"out/ui/no-such-media"'
# system edges of the app, renamed to the harness fakes
FAKES := -Dopen=fk_open -Dread=fk_read -Dclose=fk_close -Dioctl=fk_ioctl \
  -Dclock_gettime=fk_clock_gettime -Dstorage_foreach_device_supported=fk_storage
$(B)/ui_platform.o: ../app/platform.c ../vendor/Walnut-CGB/minigb_apu/minigb_apu.c
	mkdir -p $(B)
	$(CC) $(UIFLAGS) -c $< -o $@
# gb_main.c exactly as on the TV, with only the system edges renamed to fakes
$(B)/ui_gb_main.o: ../app/gb_main.c ../app/library.h
	mkdir -p $(B)
	$(CC) $(UIFLAGS) $(FAKES) -Dgbp_run_frame=fk_run_frame -Dmain=tizgb_app_main -c $< -o $@
# library.c exactly as on the TV, same renames (storage API -> fake drives)
$(B)/ui_library.o: ../app/library.c ../app/library.h
	mkdir -p $(B)
	$(CC) $(UIFLAGS) $(FAKES) -c $< -o $@
$(B)/ui_test: ui_test.c $(B)/ui_gb_main.o $(B)/ui_library.o $(B)/ui_platform.o
	$(CC) -O1 -I ../app -o $@ $^

ui-tests: all
	./fetch_test_roms.sh >/dev/null
	python3 -I ../tools/make_test_rom.py out/roms >/dev/null
	./$(B)/ui_test

apu-tests: all
	python3 -I ../tools/make_test_rom.py out/roms >/dev/null
	@for r in fade jump sweep0 sweep0-ovf midframe wave noise; do \
	  ./$(B)/apu_test out/roms/apu-$$r.gb 1.2 out/apu-$$r.wav 47; done

core-tests: all
	./fetch_test_roms.sh
	@for t in core_test core_test_df; do \
	  echo "== $$t"; \
	  ./$(B)/$$t serial out/roms/cpu_instrs.gb 7200 | tail -1; \
	  ./$(B)/$$t serial out/roms/instr_timing.gb 600 | tail -1; \
	  ./$(B)/$$t frame out/roms/dmg-acid2.gb 100 out/$$t-dmg-acid2.ppm 1 0 >/dev/null; \
	  python3 -I compare_acid.py dmg out/$$t-dmg-acid2.ppm out/roms/dmg-acid2-reference.png; \
	  ./$(B)/$$t frame out/roms/cgb-acid2.gbc 100 out/$$t-cgb-acid2.ppm 1 0 >/dev/null; \
	  python3 -I compare_acid.py cgb out/$$t-cgb-acid2.ppm out/roms/cgb-acid2-reference.png; \
	done

clean:
	rm -rf $(B) out
