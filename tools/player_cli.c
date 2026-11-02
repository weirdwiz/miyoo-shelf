// Headless player for VM runs and core smoke tests:
//   player-cli <core.so> <rom> <frames> <out.ppm> [frame:key,frame:-key,...]
//
// Keys: a b start select up down left right l r x y l2 r2. SHELF_AUDIO_DUMP=<path> writes
// the resampled 48 kHz stereo output as raw s16le. Optional SHELF_SYSTEM_DIR,
// SHELF_SAVE_DIR and SHELF_OPTIONS pick the frontend directories and option file. Shelf's
// own option defaults for the core (by its file name, <core>_libretro.so) apply as in Shelf,
// and its colour choices with the default settings: SHELF_COLOURS=0 turns correction off,
// SHELF_PALETTE=<0-3> picks a Game Boy palette (core_options.h).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/core_options.h"
#include "../src/player.h"

#define MAX_EVENTS 256

typedef struct {
    int frame;
    unsigned bit;
    int down;
} Event;

typedef struct {
    const char *name;
    unsigned bit;
} KeyName;

static const KeyName keys[] = {
    { "b", PLAYER_B },       { "y", PLAYER_Y },         { "select", PLAYER_SELECT },
    { "start", PLAYER_START }, { "up", PLAYER_UP },     { "down", PLAYER_DOWN },
    { "left", PLAYER_LEFT }, { "right", PLAYER_RIGHT }, { "a", PLAYER_A },
    { "x", PLAYER_X },       { "l", PLAYER_L },         { "r", PLAYER_R },
    { "l2", PLAYER_L2 },     { "r2", PLAYER_R2 },
};

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static unsigned key_bit(const char *name)
{
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        if (!strcmp(keys[i].name, name)) return keys[i].bit;
    return 0;
}

static int parse_events(const char *script, Event *out, int max)
{
    char buf[1024];
    char *save, *item;
    int n = 0;

    snprintf(buf, sizeof buf, "%s", script);
    for (item = strtok_r(buf, ",", &save); item && n < max; item = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(item, ':');
        char *name;
        int down = 1;
        if (!colon) continue;
        *colon = 0;
        name = colon + 1;
        if (*name == '-') {
            down = 0;
            name++;
        }
        out[n].frame = atoi(item);
        out[n].bit = key_bit(name);
        out[n].down = down;
        if (out[n].bit) n++;
    }
    return n;
}

static void stem_of(const char *path, char *out, size_t n)
{
    const char *base = strrchr(path, '/');
    char *dot;
    base = base ? base + 1 : path;
    snprintf(out, n, "%s", base);
    dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

// "gambatte" from ".../gambatte_libretro.so".
static void core_name(const char *path, char *out, size_t n)
{
    const char *base = strrchr(path, '/');
    char *end;
    snprintf(out, n, "%s", base ? base + 1 : path);
    end = strstr(out, "_libretro");
    if (end) *end = 0;
}

static int write_ppm(const char *path, const PlayerFrame *f)
{
    FILE *out = fopen(path, "wb");
    if (!out) return -1;
    fprintf(out, "P6\n%d %d\n255\n", f->w, f->h);
    for (int y = 0; y < f->h; y++) {
        const uint32_t *row = f->px + (size_t)y * f->pitch;
        for (int x = 0; x < f->w; x++) {
            unsigned char rgb[3] = { (unsigned char)(row[x] >> 16), (unsigned char)(row[x] >> 8),
                                     (unsigned char)row[x] };
            if (fwrite(rgb, 1, sizeof rgb, out) != sizeof rgb) {
                fclose(out);
                return -1;
            }
        }
    }
    return fclose(out) == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    PlayerConfig cfg;
    Event events[MAX_EVENTS];
    char stem[256], err[256], core[64];
    PlayerOption choices[CORE_CHOICES_MAX];
    const char *colours = getenv("SHELF_COLOURS"), *palette = getenv("SHELF_PALETTE");
    const char *dump_path;
    FILE *dump = NULL;
    int16_t *audio;
    int nev, frames, audio_frames = 0, max_audio;
    double audio_acc = 0.0, total_ms, run_sum = 0.0, run_max = 0.0;
    unsigned mask = 0;
    int i;

    if (argc < 5) {
        fprintf(stderr, "usage: %s <core.so> <rom> <frames> <out.ppm> [frame:key,...]\n", argv[0]);
        return 2;
    }
    frames = atoi(argv[3]);
    if (frames <= 0) {
        fprintf(stderr, "%s: bad frame count '%s'\n", argv[0], argv[3]);
        return 2;
    }
    nev = argc > 5 ? parse_events(argv[5], events, MAX_EVENTS) : 0;

    memset(&cfg, 0, sizeof cfg);
    stem_of(argv[2], stem, sizeof stem);
    cfg.core_path = argv[1];
    cfg.rom_path = argv[2];
    cfg.system_dir = getenv("SHELF_SYSTEM_DIR");
    cfg.save_dir = getenv("SHELF_SAVE_DIR");
    cfg.stem = stem;
    cfg.options_path = getenv("SHELF_OPTIONS");
    core_name(argv[1], core, sizeof core);
    cfg.defaults = core_defaults(core, &cfg.ndefaults);
    cfg.choices = choices;
    cfg.nchoices = core_choices(core, colours ? atoi(colours) != 0 : 1,
                                palette ? atoi(palette) : PALETTE_DEFAULT, choices, CORE_CHOICES_MAX);
    cfg.sample_rate = 48000;
    if (!cfg.system_dir) cfg.system_dir = ".";
    if (!cfg.save_dir) cfg.save_dir = ".";

    if (player_open(&cfg, err, sizeof err) != 0) {
        fprintf(stderr, "%s: %s\n", argv[0], err);
        return 1;
    }
    max_audio = cfg.sample_rate / 30;  // a frame's worth, with room to spare
    audio = malloc((size_t)max_audio * 2 * sizeof *audio);
    dump_path = getenv("SHELF_AUDIO_DUMP");
    if (dump_path && dump_path[0]) dump = fopen(dump_path, "wb");
    if (!audio || (dump_path && dump_path[0] && !dump)) {
        fprintf(stderr, "%s: cannot allocate the audio buffer or open %s\n", argv[0],
                dump_path ? dump_path : "(dump)");
        player_close();
        free(audio);
        return 1;
    }

    double started = now_ms();
    for (i = 0; i < frames; i++) {
        for (int e = 0; e < nev; e++) {
            if (events[e].frame != i) continue;
            if (events[e].down) mask |= events[e].bit;
            else mask &= ~events[e].bit;
        }
        player_set_input(mask);
        double t0 = now_ms();
        player_run();
        double dt = now_ms() - t0;
        run_sum += dt;
        if (dt > run_max) run_max = dt;

        // Read what this frame owed the output at cfg->sample_rate.
        audio_acc += (double)cfg.sample_rate / player_fps();
        int due = (int)audio_acc;
        audio_acc -= due;
        if (due > max_audio) due = max_audio;
        if (due > 0) {
            int got = player_audio_read(audio, due);
            audio_frames += got;
            if (dump && got > 0 &&
                fwrite(audio, sizeof(int16_t) * 2, (size_t)got, dump) != (size_t)got) {
                fprintf(stderr, "%s: audio dump write failed\n", argv[0]);
                fclose(dump);
                dump = NULL;
            }
        }
    }
    total_ms = now_ms() - started;

    if (dump) fclose(dump);
    if (write_ppm(argv[4], player_frame()) != 0) {
        fprintf(stderr, "%s: cannot write %s\n", argv[0], argv[4]);
        player_close();
        free(audio);
        return 1;
    }
    printf("frames %d\n", frames);
    printf("core_fps %.4f\n", player_fps());
    printf("wall_fps %.2f\n", total_ms > 0 ? frames * 1000.0 / total_ms : 0.0);
    printf("run_avg_ms %.3f\n", run_sum / frames);
    printf("run_max_ms %.3f\n", run_max);
    printf("audio_frames %d\n", audio_frames);
    printf("frame %dx%d aspect %.4f\n", player_frame()->w, player_frame()->h,
           (double)player_frame()->aspect);
    player_close();
    free(audio);
    return 0;
}
