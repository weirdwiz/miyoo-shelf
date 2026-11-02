// A tiny libretro core for player tests and the CLI: 160x144 "Game Boy" screen, three
// pixel formats, a deterministic pattern, audio and 8 KiB of SRAM. No emulation.
//
// FAKE_CORE_LEGACY_OPTIONS=1 makes it register options through the deprecated
// SET_VARIABLES call, so both frontend option paths get exercised. FAKE_CORE_NO_DUPES=1
// makes every frame a new one. `fake_tint` (off|red|blue) is re-read whenever the frontend
// reports a variable update, like the real cores' colour options; row 6 shows it and how
// many updates the core has seen. The core also declares, free-form, the gambatte and mGBA
// options Shelf sets, and with FAKE_CORE_OPTION_LOG=<path> writes their values there as
// `key=value` lines after a `load` or `update` line, so session tests can see what Shelf sent.
// FAKE_CORE_FAIL_UNSERIALIZE=<n> makes the nth retro_unserialize since the game loaded fail.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../third_party/libretro.h"

#define SCREEN_W 160
#define SCREEN_H 144
#define MAX_W 256
#define MAX_H 224
#define FPS 59.7275
#define CORE_RATE 32768.0
#define SRAM_SIZE 8192
#define STATE_SIZE (SRAM_SIZE + 8)  // SRAM plus the frame counter and input mask

static retro_environment_t env;
static retro_video_refresh_t video;
static retro_audio_sample_t sample;
static retro_audio_sample_batch_t batch;
static retro_input_poll_t poll;
static retro_input_state_t input;

static enum retro_pixel_format format = RETRO_PIXEL_FORMAT_RGB565;
static uint8_t sram[SRAM_SIZE];
static uint8_t rtc[8];
static int fullpath;
static unsigned frame_counter;
static unsigned last_input;
static double audio_acc;
static uint16_t audio_pos;
static uint8_t screen[MAX_W * MAX_H * 4];
static unsigned tint, updates;

static void tint_read(void)
{
    struct retro_variable var = { "fake_tint", NULL };
    tint = 0;
    if (!env || !env(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value) return;
    if (!strcmp(var.value, "red")) tint = 1;
    else if (!strcmp(var.value, "blue")) tint = 2;
}

// The real cores' options Shelf sets, declared here without values (any value goes).
static const char *const REAL_KEYS[] = {
    "gambatte_gb_colorization", "gambatte_gb_internal_palette", "gambatte_gb_palette_twb64_1",
    "gambatte_gb_palette_pixelshift_1", "gambatte_gbc_color_correction",
    "gambatte_gbc_color_correction_mode", "gambatte_mix_frames", "gambatte_gb_bootloader",
    "mgba_color_correction", "mgba_interframe_blending", "mgba_skip_bios",
};
#define NREAL (sizeof REAL_KEYS / sizeof *REAL_KEYS)

static void options_log(const char *mode, const char *event)
{
    const char *path = getenv("FAKE_CORE_OPTION_LOG");
    FILE *f;
    if (!path || !path[0] || !env || !(f = fopen(path, mode))) return;
    fprintf(f, "%s\n", event);
    for (size_t i = 0; i < NREAL; i++) {
        struct retro_variable var = { REAL_KEYS[i], NULL };
        if (env(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
            fprintf(f, "%s=%s\n", var.key, var.value);
    }
    fclose(f);
}

static void option_read(void)
{
    struct retro_variable var = { "fake_format", NULL };

    if (!env || !env(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value) return;
    if (!strcmp(var.value, "xrgb"))
        format = RETRO_PIXEL_FORMAT_XRGB8888;
    else if (!strcmp(var.value, "1555"))
        format = RETRO_PIXEL_FORMAT_0RGB1555;
    else
        format = RETRO_PIXEL_FORMAT_RGB565;
}

static void set_options_legacy(void)
{
    struct retro_variable vars[] = {
        { "fake_format", "Output format; rgb565|xrgb|1555" },
        { "fake_tint", "Tint; off|red|blue" },
        { NULL, NULL },
    };
    struct retro_variable all[3 + NREAL];
    size_t n = 0;
    for (; vars[n].key; n++) all[n] = vars[n];
    for (size_t i = 0; i < NREAL; i++) all[n++] = (struct retro_variable){ REAL_KEYS[i], "Shelf" };
    all[n] = (struct retro_variable){ NULL, NULL };
    if (env) env(RETRO_ENVIRONMENT_SET_VARIABLES, all);
}

static void set_options(void)
{
    if (getenv("FAKE_CORE_LEGACY_OPTIONS")) {
        set_options_legacy();
        return;
    }
    {
        struct retro_core_option_v2_definition defs[] = {
            {
                "fake_format",
                "Output format",
                "Output format",
                "Pixel format the fake core reports",
                NULL,
                NULL,
                { { "rgb565", "RGB565" }, { "xrgb", "XRGB8888" }, { "1555", "0RGB1555" }, { NULL, NULL } },
                "rgb565",
            },
            {
                "fake_tint",
                "Tint",
                "Tint",
                "Changes live, like a colour correction option",
                NULL,
                NULL,
                { { "off", NULL }, { "red", NULL }, { "blue", NULL }, { NULL, NULL } },
                "off",
            },
        };
        struct retro_core_option_v2_definition all[sizeof defs / sizeof *defs + NREAL + 1];
        size_t n = 0;
        memset(all, 0, sizeof all);
        for (; n < sizeof defs / sizeof *defs; n++) all[n] = defs[n];
        for (size_t i = 0; i < NREAL; i++, n++) {
            all[n].key = REAL_KEYS[i];
            all[n].desc = "Shelf";
        }
        struct retro_core_options_v2 options = { NULL, all };
        if (!env) return;
        if (env(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options)) return;
        set_options_legacy();  // frontend refused V2
    }
}

static unsigned pixel_bytes(void) { return format == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2; }

// The colours are exact after the frontend expands them back, so tests can assert on
// red/green/blue/white/black whatever the format.
static void put_pixel(unsigned x, unsigned y, unsigned r, unsigned g, unsigned b)
{
    size_t off = ((size_t)y * SCREEN_W + x) * pixel_bytes();
    uint16_t v;

    if (format == RETRO_PIXEL_FORMAT_XRGB8888) {
        // Little-endian 0x00RRGGBB.
        screen[off + 0] = (uint8_t)b;
        screen[off + 1] = (uint8_t)g;
        screen[off + 2] = (uint8_t)r;
        screen[off + 3] = 0;
        return;
    }
    if (format == RETRO_PIXEL_FORMAT_RGB565)
        v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    else
        v = (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
    screen[off + 0] = (uint8_t)v;
    screen[off + 1] = (uint8_t)(v >> 8);
}

static void put_bits(unsigned row, unsigned x0, unsigned count, unsigned value)
{
    for (unsigned bit = 0; bit < count; bit++) {
        unsigned on = (value >> bit) & 1 ? 255 : 0;
        put_pixel(x0 + bit, row, on, on, on);
    }
}

static void draw_frame(void)
{
    size_t row_bytes = (size_t)SCREEN_W * pixel_bytes();
    unsigned code = format == RETRO_PIXEL_FORMAT_XRGB8888 ? 1
                    : format == RETRO_PIXEL_FORMAT_0RGB1555 ? 2
                                                            : 0;

    // One computed test-pattern row, repeated down the screen.
    for (unsigned x = 0; x < SCREEN_W; x++)
        put_pixel(x, 0, (x * 3 + frame_counter) & 0xff, (x + frame_counter * 2) & 0xff,
                  (x * 7 + frame_counter * 3) & 0xff);
    for (unsigned y = 1; y < SCREEN_H; y++) memcpy(screen + (size_t)y * row_bytes, screen, row_bytes);

    // Row 0: the reference colours tests look at.
    put_pixel(0, 0, 255, 0, 0);
    put_pixel(1, 0, 0, 255, 0);
    put_pixel(2, 0, 0, 0, 255);
    put_pixel(3, 0, 255, 255, 255);
    put_pixel(4, 0, 0, 0, 0);
    // Row 3: the low byte of the frame counter, one bit per pixel.
    put_bits(3, 0, 8, frame_counter);
    // Row 4: the joypad mask the core saw, bits 0-15.
    put_bits(4, 0, 16, last_input);
    // Row 5: the active pixel format (bits 0-1) and the SRAM byte loaded from disk.
    put_bits(5, 0, 2, code);
    put_bits(5, 8, 8, sram[0]);
    // Row 6: the tint option (bits 0-1) and how many variable updates were seen (8-15).
    put_bits(6, 0, 2, tint);
    put_bits(6, 8, 8, updates);
    // Every fourth frame is a dupe, like a core that skips an idle update.
    int dupe = (frame_counter % 4) == 3 && !getenv("FAKE_CORE_NO_DUPES");
    video(dupe ? NULL : screen, SCREEN_W, SCREEN_H, (size_t)SCREEN_W * pixel_bytes());
}

static void emit_audio(void)
{
    int16_t buf[1024 * 2];
    double per_frame = CORE_RATE / FPS;
    int total, first, second;

    audio_acc += per_frame;
    total = (int)audio_acc;
    audio_acc -= total;
    first = total / 2;
    second = total - first;
    for (int pass = 0; pass < 2; pass++) {
        int n = pass ? second : first;
        if (n <= 0) continue;
        if (n > 1024) n = 1024;
        for (int i = 0; i < n; i++) {
            buf[2 * i] = (int16_t)(audio_pos * 64);
            buf[2 * i + 1] = (int16_t)-(audio_pos * 64);
            audio_pos++;
        }
        batch(buf, (size_t)n);
    }
}

// --- libretro API --------------------------------------------------------------

RETRO_API void retro_set_environment(retro_environment_t cb)
{
    env = cb;
    if (cb && cb(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, NULL)) set_options();
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t cb) { video = cb; }
RETRO_API void retro_set_audio_sample(retro_audio_sample_t cb) { sample = cb; }
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { batch = cb; }
RETRO_API void retro_set_input_poll(retro_input_poll_t cb) { poll = cb; }
RETRO_API void retro_set_input_state(retro_input_state_t cb) { input = cb; }

RETRO_API void retro_init(void) {}
RETRO_API void retro_deinit(void) {}

RETRO_API unsigned retro_api_version(void) { return RETRO_API_VERSION; }

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
    fullpath = getenv("FAKE_CORE_FULLPATH") != NULL;
    info->library_name = "fake";
    info->library_version = "1";
    info->valid_extensions = "gb|gbc|gba";
    info->need_fullpath = fullpath != 0;
    info->block_extract = false;
}

// Like mGBA, timing is only available once a game is loaded: asking earlier is a
// frontend bug that crashes real cores, so the fake one aborts loudly.
static int game_loaded;

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
    if (!game_loaded) {
        fprintf(stderr, "fake_core: retro_get_system_av_info before retro_load_game\n");
        abort();
    }
    info->geometry.base_width = SCREEN_W;
    info->geometry.base_height = SCREEN_H;
    info->geometry.max_width = MAX_W;
    info->geometry.max_height = MAX_H;
    info->geometry.aspect_ratio = 0.0f;  // frontend uses w/h
    info->timing.fps = FPS;
    info->timing.sample_rate = CORE_RATE;
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
    (void)port;
    (void)device;
}

RETRO_API void retro_reset(void)
{
    frame_counter = 0;
    last_input = 0;
}

RETRO_API void retro_run(void)
{
    unsigned mask = 0;

    if (poll) poll();
    bool updated = false;
    if (env && env(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated) {
        updates++;
        tint_read();
        options_log("a", "update");
    }
    if (input) {
        for (unsigned id = 0; id < 16; id++)
            if (input(0, RETRO_DEVICE_JOYPAD, 0, id)) mask |= 1u << id;
        last_input = mask;
    }
    draw_frame();
    emit_audio();
    sram[0] = (uint8_t)frame_counter;
    sram[1] = (uint8_t)(frame_counter >> 8);
    sram[2] = (uint8_t)(frame_counter >> 16);
    sram[3] = (uint8_t)(frame_counter >> 24);
    sram[4] = (uint8_t)mask;
    sram[5] = (uint8_t)(mask >> 8);
    rtc[0] = (uint8_t)frame_counter;
    rtc[1] = (uint8_t)(frame_counter >> 8);
    frame_counter++;
}

RETRO_API size_t retro_serialize_size(void) { return STATE_SIZE; }

RETRO_API bool retro_serialize(void *data, size_t len)
{
    uint8_t *out = data;
    if (!data || len < STATE_SIZE) return false;
    memcpy(out, sram, SRAM_SIZE);
    out[SRAM_SIZE + 0] = (uint8_t)frame_counter;
    out[SRAM_SIZE + 1] = (uint8_t)(frame_counter >> 8);
    out[SRAM_SIZE + 2] = (uint8_t)(frame_counter >> 16);
    out[SRAM_SIZE + 3] = (uint8_t)(frame_counter >> 24);
    out[SRAM_SIZE + 4] = (uint8_t)last_input;
    out[SRAM_SIZE + 5] = (uint8_t)(last_input >> 8);
    out[SRAM_SIZE + 6] = 0;
    out[SRAM_SIZE + 7] = 0;
    return true;
}

static int unserialized; // calls since the game loaded

RETRO_API bool retro_unserialize(const void *data, size_t len)
{
    const uint8_t *in = data;
    const char *fail = getenv("FAKE_CORE_FAIL_UNSERIALIZE");
    unserialized++;
    if (fail && atoi(fail) == unserialized) return false;
    if (!data || len < STATE_SIZE) return false;
    memcpy(sram, in, SRAM_SIZE);
    frame_counter = (unsigned)in[SRAM_SIZE + 0] | (unsigned)in[SRAM_SIZE + 1] << 8 |
                    (unsigned)in[SRAM_SIZE + 2] << 16 | (unsigned)in[SRAM_SIZE + 3] << 24;
    last_input = (unsigned)in[SRAM_SIZE + 4] | (unsigned)in[SRAM_SIZE + 5] << 8;
    return true;
}

RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
    enum retro_pixel_format fmt;
    int first = 0;

    if (!game) return false;
    if (fullpath) {
        FILE *f = game->path ? fopen(game->path, "rb") : NULL;
        if (!f) return false;
        first = fgetc(f);
        fclose(f);
    } else {
        if (!game->data || !game->size) return false;
        first = ((const unsigned char *)game->data)[0];
    }
    if (first == 'X') return false;  // the "refused" fixture
    option_read();
    tint_read();
    updates = 0;
    options_log("w", "load");
    unserialized = 0;
    fmt = format;
    if (!env || !env(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) return false;
    memset(sram, 0, sizeof sram);
    memset(rtc, 0, sizeof rtc);
    frame_counter = 0;
    last_input = 0;
    audio_acc = 0.0;
    audio_pos = 0;
    game_loaded = 1;
    return true;
}

RETRO_API void retro_unload_game(void) { game_loaded = 0; }

RETRO_API unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

RETRO_API void *retro_get_memory_data(unsigned id)
{
    if (id == RETRO_MEMORY_SAVE_RAM) return sram;
    if (id == RETRO_MEMORY_RTC) return rtc;
    return NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
    if (id == RETRO_MEMORY_SAVE_RAM) return SRAM_SIZE;
    if (id == RETRO_MEMORY_RTC) return sizeof rtc;
    return 0;
}
