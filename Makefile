# Shelf build.
#   make sim       build the desktop simulator (needs SDL2)
#   make run       build + run the simulator against fixture/SDCARD
#   make fixture   (re)build the sample SD card
#   make test      build and run the tests
#   make bench-ui  time the home screen and in-game menu coming up (host)
#   make shots     headless screenshots into docs/screenshots
#   make mm        cross-compile for the Miyoo in its toolchain container (podman or docker)
#   make clean

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers
BUILD := build

CORE_SRC := src/main.c src/loop.c src/gfx.c src/text.c src/library.c src/icons.c src/ui.c src/game.c src/settings.c src/scale.c src/player.c src/probe.c src/power.c third_party/cJSON.c
SIM_SRC := $(CORE_SRC) src/platform_sdl2.c
SDL_CFLAGS = $(shell sdl2-config --cflags)
SDL_LIBS = $(shell sdl2-config --libs)

FIXTURE := fixture/SDCARD
# The fixture's recents file doubles as the "fixture is built" marker dependent targets
# wait for.
FIXTURE_READY := $(FIXTURE)/.shelf/data/recents.json

.PHONY: sim run fixture clean test shots mm mm-inner bench-ui

test: $(BUILD)/test-library $(BUILD)/test-rrect $(BUILD)/test-scaled-blit $(BUILD)/test-scale $(BUILD)/test-fb-geometry \
      $(BUILD)/test-ui-cache $(BUILD)/test-loop $(BUILD)/test-ui-alloc $(BUILD)/test-game $(BUILD)/test-palette $(BUILD)/test-power $(BUILD)/test-hw-mm $(BUILD)/test-main $(FIXTURE_READY)
	$(BUILD)/test-scale
	$(BUILD)/test-fb-geometry
	$(BUILD)/test-power
	$(BUILD)/test-hw-mm
	$(BUILD)/test-library
	$(BUILD)/test-rrect
	$(BUILD)/test-scaled-blit
	$(BUILD)/test-palette "$(CURDIR)/$(FIXTURE)" "$(CURDIR)/assets/fonts"
	$(BUILD)/test-ui-cache "$(CURDIR)/$(FIXTURE)" "$(CURDIR)/assets/fonts"
	$(BUILD)/test-loop "$(CURDIR)/$(FIXTURE)" "$(CURDIR)/assets/fonts"
	$(BUILD)/test-ui-alloc "$(CURDIR)/$(FIXTURE)" "$(CURDIR)/assets/fonts"
	$(BUILD)/test-game "$(CURDIR)/$(BUILD)/test-game-work" "$(CURDIR)/assets/fonts" "$(CURDIR)/$(BUILD)/fake_core.so"
	$(BUILD)/test-main "$(CURDIR)/$(BUILD)/test-main-work" "$(CURDIR)/assets/fonts" "$(CURDIR)/$(BUILD)/fake_core.so"

$(BUILD)/test-library: tools/test_library.c src/library.c src/library.h third_party/cJSON.c third_party/cJSON.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/test_library.c src/library.c third_party/cJSON.c

$(BUILD)/test-loop: tools/test_loop.c src/loop.c src/ui.c src/icons.c src/library.c src/settings.c src/text.c src/gfx.c $(wildcard src/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/test_loop.c src/loop.c src/ui.c src/icons.c src/library.c src/settings.c src/text.c src/gfx.c third_party/cJSON.c -lm -lpthread

$(BUILD)/test-palette: tools/test_palette.c src/ui.c src/library.c src/icons.c src/settings.c src/text.c src/gfx.c third_party/cJSON.c third_party/cJSON.h $(wildcard src/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/test_palette.c src/icons.c src/settings.c src/text.c src/gfx.c third_party/cJSON.c -lm

$(BUILD)/test-ui-cache: tools/test_ui_cache.c src/ui.c src/icons.c src/library.c src/settings.c src/text.c src/gfx.c $(wildcard src/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/test_ui_cache.c src/icons.c src/library.c src/settings.c src/text.c src/gfx.c third_party/cJSON.c -lm

# The UI's sources with every allocation routed through the fault injector.
FAILALLOC_SRC := src/ui.c src/icons.c src/text.c src/gfx.c
$(BUILD)/test-ui-alloc: tools/test_ui_alloc.c tools/failalloc.c tools/failalloc.h $(FAILALLOC_SRC) src/library.c $(wildcard src/*.h)
	@mkdir -p $(BUILD)/failalloc
	for f in $(FAILALLOC_SRC); do $(CC) $(CFLAGS) -include tools/failalloc.h -c $$f -o $(BUILD)/failalloc/$$(basename $$f .c).o || exit 1; done
	$(CC) $(CFLAGS) -o $@ tools/test_ui_alloc.c tools/failalloc.c $(patsubst src/%.c,$(BUILD)/failalloc/%.o,$(FAILALLOC_SRC)) src/library.c src/settings.c third_party/cJSON.c -lm -lpthread

# How long the home screen and the in-game menu take to come up, cycle after cycle.
$(BUILD)/bench-ui: tools/bench_ui.c src/ui.c src/icons.c src/library.c src/settings.c src/text.c src/gfx.c $(wildcard src/*.h) Makefile
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(BENCH_FLAGS) -Wl,--wrap=fopen -o $@ tools/bench_ui.c src/ui.c src/icons.c src/library.c src/settings.c src/text.c src/gfx.c third_party/cJSON.c -lm -lpthread

bench-ui: $(BUILD)/bench-ui $(FIXTURE_READY)
	$(BUILD)/bench-ui "$(CURDIR)/$(FIXTURE)" "$(CURDIR)/assets/fonts" $(CYCLES)

$(BUILD)/test-scaled-blit: tools/test_scaled_blit.c src/gfx.c src/gfx.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/test_scaled_blit.c -lm

$(BUILD)/test-scale: tools/test_scale.c src/scale.c src/scale.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/test_scale.c src/scale.c -lm

$(BUILD)/test-fb-geometry: tools/test_fb_geometry.c src/fb_geometry.c src/fb_geometry.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/test_fb_geometry.c src/fb_geometry.c

$(BUILD)/test-power: tools/test_power.c src/power.c src/power.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/test_power.c src/power.c

# The device's hardware settings against a throwaway sysfs tree (also run in the VM).
$(BUILD)/test-hw-mm: tools/test_hw_mm.c src/hw_mm.c src/platform.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Wno-format-truncation -o $@ tools/test_hw_mm.c src/hw_mm.c -lm

$(BUILD)/test-rrect: tools/test_rrect.c src/gfx.c src/gfx.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/test_rrect.c -lm

# One play session end to end: the real player, game loop and UI drive the fake core
# through a fake platform, so the in-game menu, standby and saving can be scripted.
SESSION_SRC := src/game.c src/player.c src/loop.c src/scale.c src/ui.c src/icons.c src/text.c \
               src/gfx.c src/library.c src/settings.c src/power.c third_party/cJSON.c tools/fake_platform.c
SESSION_DEPS := $(wildcard src/*.h) tools/fake_platform.h

$(BUILD)/test-game: tools/test_game.c $(SESSION_SRC) $(SESSION_DEPS) $(BUILD)/fake_core.so
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/test_game.c $(SESSION_SRC) -lm -lpthread -ldl

# Shelf from boot: main.c on the fake platform, with the session's sources.
$(BUILD)/test-main: tools/test_main.c src/main.c src/probe.c $(SESSION_SRC) $(SESSION_DEPS) $(BUILD)/fake_core.so
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -Dmain=shelf_main -c -o $(BUILD)/test-main-shelf.o src/main.c
	$(CC) $(CFLAGS) -o $@ tools/test_main.c $(BUILD)/test-main-shelf.o src/probe.c $(SESSION_SRC) -lm -lpthread -ldl

# The in-game menu over a running fake-core game, headless, for docs/screenshots.
$(BUILD)/shot-ingame: tools/shot_ingame.c $(SESSION_SRC) $(SESSION_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/shot_ingame.c $(SESSION_SRC) -lm -lpthread -ldl

sim: $(BUILD)/shelf-sim

$(BUILD)/shelf-sim: $(SIM_SRC) $(wildcard src/*.h)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) $(SDL_CFLAGS) -o $@ $(SIM_SRC) $(SDL_LIBS) -lm -lpthread

$(FIXTURE_READY): tools/make_fixture.sh
	tools/make_fixture.sh

fixture:
	tools/make_fixture.sh

run: sim $(FIXTURE_READY)
	SHELF_ROOT=$(CURDIR)/$(FIXTURE) SHELF_FONTS=$(CURDIR)/assets/fonts $(BUILD)/shelf-sim

clean:
	rm -rf $(BUILD)

# Headless screenshots into docs/screenshots (keys: U D L R A B Y X l r s, where l/r are L1/R1
# and s is START; names ending -dark use the dark theme). Playing a game rewrites the
# fixture's recents, so restore them after. 7- comes from the in-game menu over a running
# fake-core game (tools/shot_ingame.c), the rest from the simulator without a window.
.PHONY: shots
shots: sim $(FIXTURE_READY) $(BUILD)/shot-ingame $(BUILD)/fake_core.so
	@mkdir -p docs/screenshots $(BUILD)/shots
	@cp $(FIXTURE_READY) $(BUILD)/shots/recents.bak
	@for spec in "1-home:" "2-library:r" "3-folder:rA" "4-after-launch:rAAA" "5-dark:r" "6-options-dark:s" "8-options:s"; do \
		name=$${spec%%:*}; keys=$${spec#*:}; \
		theme=light; case $$name in *-dark) theme=dark;; esac; \
		cp $(BUILD)/shots/recents.bak $(FIXTURE_READY); \
		SHELF_THEME=$$theme SHELF_ROOT=$(CURDIR)/$(FIXTURE) SHELF_FONTS=$(CURDIR)/assets/fonts \
		SHELF_SHOT=$(BUILD)/shots/$$name.ppm SHELF_KEYS="$$keys" $(BUILD)/shelf-sim 2>/dev/null; \
		python3 -I tools/img.py $(BUILD)/shots/$$name.ppm docs/screenshots/$$name.png; \
	done
	@cp $(BUILD)/shots/recents.bak $(FIXTURE_READY)
	@SHELF_ROOT=$(CURDIR)/$(FIXTURE) SHELF_FONTS=$(CURDIR)/assets/fonts $(BUILD)/shot-ingame \
		$(CURDIR)/$(FIXTURE) $(CURDIR)/assets/fonts $(CURDIR)/$(BUILD)/fake_core.so \
		$(BUILD)/shots/7-ingame-menu.ppm
	@python3 -I tools/img.py $(BUILD)/shots/7-ingame-menu.ppm docs/screenshots/7-ingame-menu.png
	@ls docs/screenshots

# ---------- Miyoo Mini Plus ----------
TOOLCHAIN := docker.io/aemiii91/miyoomini-toolchain:latest
CONTAINER ?= $(shell command -v podman || command -v docker)
MM_BUILD := $(BUILD)/mm
MM_SRC := $(CORE_SRC) src/platform_mm.c src/audio_mm.c src/hw_mm.c src/fb_geometry.c

mm:
	$(CONTAINER) run --rm --platform linux/amd64 -v "$(CURDIR)":/root/workspace:z -w /root/workspace $(TOOLCHAIN) \
		/bin/bash -c "source /root/.bashrc && make mm-inner"

mm-inner: $(MM_BUILD)/shelf

$(MM_BUILD)/shelf: $(MM_SRC) $(wildcard src/*.h) Makefile
	@mkdir -p $(MM_BUILD)
	$(CROSS_COMPILE)gcc -O3 -fno-math-errno -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -std=c11 -D_DEFAULT_SOURCE \
		-Wall -Wno-unused-parameter -Wno-sign-compare -Wno-format-truncation -o $@ $(MM_SRC) -lm -lpthread -ldl

# Self-contained parts (VM harness, cores, player tests) add their own rules here.
-include $(wildcard mk/*.mk)
