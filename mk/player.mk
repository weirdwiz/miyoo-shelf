# Libretro host (src/player.c): host tests against a fake core, a headless CLI for VM runs,
# and the same CLI cross-compiled for the device.
#   make test-player        unit tests with the fake core
#   make test-player-asan   the same under ASan + UBSan
#   make player-cli         headless runner (host)
#   make player-cli-mm      headless runner for the Miyoo (toolchain container)
.PHONY: test-player test-player-asan player-cli player-cli-mm player-cli-mm-inner

PLAYER_LIBS := -ldl -lpthread -lm
PLAYER_ASAN := -fsanitize=address,undefined -fno-omit-frame-pointer -g
PLAYER_MM_CFLAGS := -O2 -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard -std=c11 -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64
PLAYER_DEPS := src/player.c src/player.h src/core_options.c src/core_options.h src/platform.h third_party/libretro.h

$(BUILD)/fake_core.so: tools/fake_core.c third_party/libretro.h
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -fPIC -shared -o $@ tools/fake_core.c

$(BUILD)/test-player: tools/test_player.c $(PLAYER_DEPS) $(BUILD)/fake_core.so
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/test_player.c src/player.c $(PLAYER_LIBS)

test-player: $(BUILD)/test-player
	$(BUILD)/test-player $(BUILD)/fake_core.so $(BUILD)/test-player-work

$(BUILD)/asan/fake_core.so: tools/fake_core.c third_party/libretro.h
	@mkdir -p $(BUILD)/asan
	$(CC) $(CFLAGS) $(PLAYER_ASAN) -fPIC -shared -o $@ tools/fake_core.c

$(BUILD)/asan/test-player: tools/test_player.c $(PLAYER_DEPS) $(BUILD)/asan/fake_core.so
	@mkdir -p $(BUILD)/asan
	$(CC) $(CFLAGS) $(PLAYER_ASAN) -o $@ tools/test_player.c src/player.c $(PLAYER_LIBS)

test-player-asan: $(BUILD)/asan/test-player
	$(BUILD)/asan/test-player $(BUILD)/asan/fake_core.so $(BUILD)/asan/test-player-work

player-cli: $(BUILD)/player-cli

$(BUILD)/player-cli: tools/player_cli.c $(PLAYER_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(CFLAGS) -o $@ tools/player_cli.c src/player.c src/core_options.c $(PLAYER_LIBS)

player-cli-mm:
	$(CONTAINER) run --rm --platform linux/amd64 -v "$(CURDIR)":/root/workspace:z -w /root/workspace $(TOOLCHAIN) \
		/bin/bash -c "source /root/.bashrc && make player-cli-mm-inner"

# Runs inside the toolchain container.
player-cli-mm-inner: $(MM_BUILD)/player-cli

$(MM_BUILD)/player-cli: tools/player_cli.c $(PLAYER_DEPS)
	@mkdir -p $(MM_BUILD)
	$(CROSS_COMPILE)gcc $(PLAYER_MM_CFLAGS) \
		-Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers \
		-o $@ tools/player_cli.c src/player.c src/core_options.c $(PLAYER_LIBS)
