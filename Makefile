# Shelf build.
#   make sim       build the desktop simulator (needs SDL2)
#   make run       build + run the simulator against fixture/SDCARD
#   make fixture   (re)build the sample SD card
#   make clean

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers
BUILD := build

CORE_SRC := src/main.c src/gfx.c src/text.c src/library.c src/icons.c src/ui.c third_party/cJSON.c
SIM_SRC := $(CORE_SRC) src/platform_sdl2.c
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS := $(shell sdl2-config --libs)

.PHONY: sim run fixture clean

sim: $(BUILD)/shelf-sim

$(BUILD)/shelf-sim: $(SIM_SRC) $(wildcard src/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $(SIM_SRC) $(SDL_LIBS) -lm

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
