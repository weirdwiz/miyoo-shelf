# Shelf build.
#   make sim       build the desktop simulator (needs SDL2)
#   make run       build + run the simulator against fixture/SDCARD
#   make fixture   (re)build the sample SD card
#   make mm        cross-compile for the Miyoo in Onion's toolchain (podman or docker)
#   make push      copy the build to the device over SSH: MM_HOST=<device-ip> make push
#   make log       show the device log from the last run
#   make clean

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers
BUILD := build

CORE_SRC := src/main.c src/gfx.c src/text.c src/library.c src/icons.c src/ui.c third_party/cJSON.c
SIM_SRC := $(CORE_SRC) src/platform_sdl2.c
SDL_CFLAGS = $(shell sdl2-config --cflags)
SDL_LIBS = $(shell sdl2-config --libs)

.PHONY: sim run fixture clean mm mm-inner push log test

test: $(BUILD)/test-library $(BUILD)/test-rrect $(BUILD)/test-scaled-blit $(BUILD)/test-ui-cache fixture/SDCARD/Emu
	$(BUILD)/test-library
	$(BUILD)/test-rrect
	$(BUILD)/test-scaled-blit
	$(BUILD)/test-ui-cache "$(CURDIR)/fixture/SDCARD" "$(CURDIR)/assets/fonts"

$(BUILD)/test-library: tools/test_library.c src/library.c src/library.h third_party/cJSON.c third_party/cJSON.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/test_library.c src/library.c third_party/cJSON.c

$(BUILD)/test-ui-cache: tools/test_ui_cache.c src/ui.c src/icons.c src/library.c src/text.c src/gfx.c $(wildcard src/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/test_ui_cache.c src/icons.c src/library.c src/text.c src/gfx.c third_party/cJSON.c -lm

$(BUILD)/test-scaled-blit: tools/test_scaled_blit.c src/gfx.c src/gfx.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/test_scaled_blit.c -lm

$(BUILD)/test-rrect: tools/test_rrect.c src/gfx.c src/gfx.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/test_rrect.c -lm

sim: $(BUILD)/shelf-sim

$(BUILD)/shelf-sim: $(SIM_SRC) $(wildcard src/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $(SIM_SRC) $(SDL_LIBS) -lm -lpthread

fixture/SDCARD/Emu:
	tools/make_fixture.sh

fixture:
	tools/make_fixture.sh

run: sim fixture/SDCARD/Emu
	SHELF_ROOT=$(CURDIR)/fixture/SDCARD SHELF_FONTS=$(CURDIR)/assets/fonts SHELF_CMD=$(CURDIR)/$(BUILD)/cmd_to_run.sh $(BUILD)/shelf-sim

clean:
	rm -rf $(BUILD)

# Headless screenshots into docs/screenshots (keys: U D L R A B Y l r).
.PHONY: shots
shots: sim fixture/SDCARD/Emu
	@mkdir -p docs/screenshots $(BUILD)/shots
	@cp fixture/SDCARD/Roms/recentlist.json $(BUILD)/shots/recent.bak
	@for spec in "1-home:" "2-library:RRRRRR" "3-folder:RRRRRRRAD" "4-after-launch:RRRRRRRRARA"; do \
		name=$${spec%%:*}; keys=$${spec#*:}; \
		SHELF_ROOT=$(CURDIR)/fixture/SDCARD SHELF_FONTS=$(CURDIR)/assets/fonts SHELF_CMD=$(BUILD)/shots/cmd.sh \
		SHELF_SHOT=$(BUILD)/shots/$$name.ppm SHELF_KEYS="$$keys" $(BUILD)/shelf-sim 2>/dev/null; \
		sips -s format png $(BUILD)/shots/$$name.ppm --out docs/screenshots/$$name.png >/dev/null; \
	done
	@cp $(BUILD)/shots/recent.bak fixture/SDCARD/Roms/recentlist.json
	@ls docs/screenshots

# ---------- Miyoo Mini Plus ----------
TOOLCHAIN := docker.io/aemiii91/miyoomini-toolchain:latest
CONTAINER ?= $(shell command -v podman || command -v docker)
MM_BUILD := $(BUILD)/mm
MM_SRC := $(CORE_SRC) src/platform_mm.c

mm:
	$(CONTAINER) run --rm --platform linux/amd64 -v "$(CURDIR)":/root/workspace -w /root/workspace $(TOOLCHAIN) \
		/bin/bash -c "source /root/.bashrc && make mm-inner"

# Runs inside the toolchain container.
mm-inner: $(MM_BUILD)/shelf

$(MM_BUILD)/shelf: $(MM_SRC) $(wildcard src/*.h) Makefile
	@mkdir -p $(MM_BUILD)
	$(CROSS_COMPILE)gcc -O3 -fno-math-errno -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -std=c11 -D_DEFAULT_SOURCE \
		-Wall -Wno-unused-parameter -Wno-sign-compare -Wno-format-truncation -o $@ $(MM_SRC) -lSDL -lm -lpthread

APP_DIR := /mnt/SDCARD/App/Shelf

push: $(MM_BUILD)/shelf
	tools/mm.sh 'mkdir -p $(APP_DIR)/fonts'
	tools/mm.sh 'cat > $(APP_DIR)/shelf.new && chmod +x $(APP_DIR)/shelf.new && mv $(APP_DIR)/shelf.new $(APP_DIR)/shelf' < $(MM_BUILD)/shelf
	@for f in assets/fonts/*.ttf; do \
		tools/mm.sh "[ -f $(APP_DIR)/fonts/$$(basename $$f) ] || cat > $(APP_DIR)/fonts/$$(basename $$f)" < $$f; done
	tools/mm.sh 'cat > $(APP_DIR)/config.json' < assets/app/config.json
	tools/mm.sh 'cat > $(APP_DIR)/launch.sh && chmod +x $(APP_DIR)/launch.sh' < assets/app/launch.sh
	@echo "pushed: open Apps > Shelf on the device. Log: make log"

log:
	tools/mm.sh 'cat /tmp/shelf.log; [ -f /tmp/shelf_cmd.sh ] && echo "--- last command:" && cat /tmp/shelf_cmd.sh'
