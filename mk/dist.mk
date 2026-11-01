# The SD card image: boot script, device binary, cores and licences.
#   make dist          build/dist/SDCARD, ready to copy onto a FAT32 card
#   make dist-zip      build/shelf-<git describe>.zip holding the folder's contents
#   make test-updater  host tests for the boot script
#   make check-dist    verify the image is complete, clean and really ARM
DIST_DIR := $(BUILD)/dist/SDCARD
SHELF_DIR := $(DIST_DIR)/.shelf
DIST_VERSION := $(shell git describe --always --dirty)
DIST_ZIP := $(BUILD)/shelf-$(DIST_VERSION).zip
DIST_LICENSES = LICENSE assets/fonts/OFL.txt third_party/cJSON.LICENSE \
	$(CORES_DIR)/mgba_libretro.LICENSE $(CORES_DIR)/gambatte_libretro.LICENSE

.PHONY: dist dist-zip test-updater check-dist

# $(MM_BUILD)/shelf is cross-compiled by `make mm` in the toolchain container and only
# packaged here. It is checked in the recipe rather than listed as a prerequisite, because
# its rule compiles for ARM and that fails on the host even though the binary is there.
dist: cores
	@test -f $(MM_BUILD)/shelf || { echo "error: $(MM_BUILD)/shelf is missing; run 'make mm' first" >&2; exit 1; }
	rm -rf $(DIST_DIR)
	mkdir -p $(DIST_DIR)/.tmp_update $(SHELF_DIR)/bin $(SHELF_DIR)/cores $(SHELF_DIR)/data \
		$(SHELF_DIR)/fonts $(SHELF_DIR)/sound $(SHELF_DIR)/LICENSES \
		$(DIST_DIR)/Roms/GB $(DIST_DIR)/Roms/GBC $(DIST_DIR)/Roms/GBA $(DIST_DIR)/BIOS $(DIST_DIR)/Saves
	install -m 755 assets/sdcard/.tmp_update/updater $(DIST_DIR)/.tmp_update/updater
	install -m 755 $(MM_BUILD)/shelf $(SHELF_DIR)/bin/shelf
	install -m 644 $(CORES_DIR)/mgba_libretro.so $(CORES_DIR)/gambatte_libretro.so $(SHELF_DIR)/cores/
	install -m 644 assets/fonts/*.ttf $(SHELF_DIR)/fonts/
	install -m 644 assets/sound/click.wav $(SHELF_DIR)/sound/
	for f in $(DIST_LICENSES); do install -m 644 $$f $(SHELF_DIR)/LICENSES/$$(basename $$f); done
	install -m 644 $(CORES_DIR)/VERSIONS $(SHELF_DIR)/LICENSES/VERSIONS
	@{ echo "# Where the bundled libretro cores come from (GPL-2.0 and MPL-2.0 sources)."; \
	   awk '!/^#/ && NF >= 3 { print $$1 " " $$2 " " $$3 }' $(CORES_DIR)/VERSIONS; } \
		> $(SHELF_DIR)/LICENSES/SOURCES.txt
	for d in Roms/GB Roms/GBC Roms/GBA BIOS Saves .shelf/data; do : > $(DIST_DIR)/$$d/.keep; done
	printf '%s\n' \
		'Shelf - a Game Boy, Game Boy Color and Game Boy Advance player for the' \
		'Miyoo Mini.' \
		'' \
		'Install' \
		'  Copy everything in this folder to the root of a FAT32 microSD card, put' \
		'  the card back in the device and boot it. Shelf starts in place of the' \
		'  stock firmware.' \
		'' \
		'Games' \
		'  Put .gb ROMs in Roms/GB, .gbc ROMs in Roms/GBC and .gba ROMs in Roms/GBA.' \
		'  Subfolders are fine. Box art goes next to the ROMs, in' \
		'  Roms/<system>/Imgs/<game name>.png, where <game name> is the ROM file' \
		'  name without its extension.' \
		'' \
		'BIOS' \
		'  Game Boy Advance games run without a BIOS. For better accuracy, put a' \
		'  gba_bios.bin image in BIOS.' \
		'' \
		'Saves' \
		'  Battery saves and save states are written to Saves/<system>.' \
		'' \
		'Back to the stock firmware' \
		'  Remove (or rename) the hidden .tmp_update folder and reboot. Nothing' \
		'  else on the card is touched.' \
		> $(DIST_DIR)/README.txt
	@echo "dist: $(DIST_DIR)"

dist-zip: dist
	@rm -f $(DIST_ZIP)
	cd $(DIST_DIR) && zip -qr -X "$(CURDIR)/$(DIST_ZIP)" .
	@ls -l $(DIST_ZIP)

test-updater:
	sh tools/test_updater.sh

check-dist:
	sh tools/check_dist.sh
