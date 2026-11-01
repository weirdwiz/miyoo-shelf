// One play session end to end: game_play() with the real player, game loop and in-game
// menu, driven by the fake core (SHELF_CORE) on the fake platform.
// Usage: test-game <work dir> <fonts dir> <fake core .so>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/game.h"
#include "../src/icons.h"
#include "../src/library.h"
#include "fake_platform.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

// The fake core's state: 8 KiB of SRAM, then the frame counter (see tools/fake_core.c).
#define FAKE_SRAM 8192

static Library lib;
static char sd[PATH_LEN]; // the fake SD card's root
static char fonts[PATH_LEN];
static char core[PATH_LEN];
static GameSettings gs;

/* ---------- the fake SD card ---------- */

static int mkpath(const char *path)
{
    char buf[PATH_LEN];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            if (mkdir(buf, 0755) && errno != EEXIST) return -1;
            *p = '/';
        }
    return mkdir(buf, 0755) && errno != EEXIST ? -1 : 0;
}

static void clear_dir(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[PATH_LEN * 2];
        struct stat st;
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        if (!lstat(child, &st) && S_ISDIR(st.st_mode)) clear_dir(child);
        else unlink(child);
    }
    closedir(d);
    rmdir(path);
}

static int write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int bad = len && fwrite(data, 1, len, f) != len;
    return fclose(f) || bad ? -1 : 0;
}

// Three games in two folders, with Alpha, Beta and Gamma as the recents.
static void build_card(const char *work)
{
    clear_dir(work);
    snprintf(sd, sizeof sd, "%s/SDCARD", work);
    char path[PATH_LEN * 2];
    unsigned char rom[64] = {0};
    snprintf(path, sizeof path, "%s/Roms/GB", sd);
    CHECK(mkpath(path) == 0);
    snprintf(path, sizeof path, "%s/Roms/GB/Alpha.gb", sd);
    CHECK(write_file(path, rom, sizeof rom) == 0);
    snprintf(path, sizeof path, "%s/Roms/GB/Beta.gb", sd);
    CHECK(write_file(path, rom, sizeof rom) == 0);
    snprintf(path, sizeof path, "%s/Roms/GBA", sd);
    CHECK(mkpath(path) == 0);
    snprintf(path, sizeof path, "%s/Roms/GBA/Gamma.gba", sd);
    CHECK(write_file(path, rom, sizeof rom) == 0);
    snprintf(path, sizeof path, "%s/.shelf/data", sd);
    CHECK(mkpath(path) == 0);
    static const char recents[] =
        "[\"/mnt/SDCARD/Roms/GB/Alpha.gb\", \"/mnt/SDCARD/Roms/GB/Beta.gb\", "
        "\"/mnt/SDCARD/Roms/GBA/Gamma.gba\"]\n";
    snprintf(path, sizeof path, "%s/.shelf/data/recents.json", sd);
    CHECK(write_file(path, recents, sizeof recents - 1) == 0);
}

/* ---------- reading what a session left behind ---------- */

static void saved(int game, const char *ext, char *out, int n)
{
    char dev[PATH_LEN], dir[PATH_LEN];
    lib_save_dir(&lib, game, dev, sizeof dev);
    host_path(dev, dir, sizeof dir);
    snprintf(out, n, "%s/%s%s", dir, lib.games[game].stem, ext);
}

static int exists(int game, const char *ext)
{
    char path[PATH_LEN * 2];
    saved(game, ext, path, sizeof path);
    return access(path, F_OK) == 0;
}

static long state_counter(int game) // the fake core's frame counter in a saved state
{
    char path[PATH_LEN * 2];
    unsigned char buf[FAKE_SRAM + 8];
    saved(game, ".state", path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n < sizeof buf) return -1;
    const unsigned char *p = buf + FAKE_SRAM;
    return (long)p[0] | (long)p[1] << 8 | (long)p[2] << 16 | (long)p[3] << 24;
}

// The fake core paints the low byte of its frame counter into row 3, one bit per pixel.
static int frame_counter(const Image *f)
{
    if (!f) return -1;
    int v = 0;
    for (int i = 0; i < 8; i++) {
        uint32_t p = f->px[3 * f->w + i];
        if (((p >> 16) & 0xff) > 127) v |= 1 << i;
    }
    return v;
}

static int game_index(const char *stem)
{
    for (int i = 0; i < lib.ngames; i++)
        if (!strcmp(lib.games[i].stem, stem)) return i;
    return -1;
}

// What the saved state held at the moments the platform blanked, woke and powered off.
static int watched;
static long at_blank, at_wake, at_off;
static void watch_saves(const char *call)
{
    if (!strcmp(call, "standby_on")) at_blank = state_counter(watched);
    if (!strcmp(call, "standby_off")) at_wake = state_counter(watched);
    if (!strcmp(call, "power_off")) at_off = state_counter(watched);
}

// One session with the script the caller queued; returns game_play's result.
static int session(int game, int *next)
{
    return game_play(&lib, game, &gs, next);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <work dir> <fonts> <fake core .so>\n", argv[0]);
        return 2;
    }
    snprintf(fonts, sizeof fonts, "%s", argv[2]);
    snprintf(core, sizeof core, "%s", argv[3]);
    setenv("SHELF_CORE", core, 1);

    build_card(argv[1]);
    lib_set_root(sd);
    CHECK(lib_load(&lib) == 0);
    CHECK(lib.ngames == 3 && lib.nrecent == 3);
    icons_init(&lib);
    gs = (GameSettings){SCALE_SHARP, fonts};

    int alpha = game_index("Alpha"), beta = game_index("Beta"), gamma = game_index("Gamma");
    CHECK(alpha >= 0 && beta >= 0 && gamma >= 0);
    CHECK(lib.nrecent == 3 && lib.recent[0] == alpha && lib.recent[1] == beta);

    // Play a while, pause with MENU, and leave for Shelf: state, SRAM and screenshot are
    // written and the play time is recorded.
    fp_reset();
    fp_limit(20);
    fp_press_at(1.2, BTN_MENU);
    fp_press_at(2.4, BTN_B);
    int next = -1;
    CHECK(session(alpha, &next) == GAME_HOME);
    CHECK(next == -1);
    CHECK(exists(alpha, ".state") && exists(alpha, ".srm") && exists(alpha, ".png"));
    CHECK(lib.games[alpha].play_count == 1);
    CHECK(lib.games[alpha].play_time >= 1);
    CHECK(fp()->game_presents > 20);
    CHECK(fp()->presents > 20); // the menu's frames
    CHECK(fp()->performance_on >= 1);

    // The next session resumes from the state instead of a fresh boot.
    long counter = state_counter(alpha);
    CHECK(counter > 0);
    fp_reset();
    fp_limit(20);
    fp_press_at(0.4, BTN_MENU);
    fp_press_at(1.4, BTN_B);
    CHECK(session(alpha, &next) == GAME_HOME);
    CHECK(frame_counter(fp_first_frame()) == counter);

    // MENU, right, A: the running game is saved and the menu hands over the next one.
    long before = state_counter(alpha);
    fp_reset();
    fp_limit(25);
    fp_press_at(0.5, BTN_MENU);
    fp_press_at(1.3, BTN_RIGHT);
    fp_press_at(1.9, BTN_A);
    CHECK(session(alpha, &next) == GAME_SWITCH);
    CHECK(next == beta);
    CHECK(state_counter(alpha) > before);

    // SELECT asks before restarting: B cancels (the menu stays open), A restarts the
    // running game, so the state it leaves behind starts counting from zero again.
    fp_reset();
    fp_limit(25);
    fp_press_at(1.2, BTN_MENU);
    fp_press_at(1.6, BTN_SELECT);
    fp_press_at(2.0, BTN_B);   // answers "no"
    fp_press_at(2.4, BTN_B);   // and now leaves
    CHECK(session(alpha, &next) == GAME_HOME);
    long played = state_counter(alpha);
    CHECK(played > 120); // two and a bit seconds of frames, never restarted

    fp_reset();
    fp_limit(25);
    fp_press_at(1.2, BTN_MENU);
    fp_press_at(1.6, BTN_SELECT);
    fp_press_at(2.0, BTN_A);   // answers "yes": the game restarts and resumes
    fp_press_at(3.0, BTN_MENU);
    fp_press_at(3.4, BTN_B);
    CHECK(session(alpha, &next) == GAME_HOME);
    CHECK(state_counter(alpha) < 100 && state_counter(alpha) < played);

    // A in the menu resumes the game for real: it runs on and MENU opens the menu again.
    fp_reset();
    fp_limit(25);
    fp_press_at(1.0, BTN_MENU);
    fp_press_at(1.4, BTN_A);   // resume
    fp_press_at(2.6, BTN_MENU); // a second menu after resuming
    fp_press_at(3.0, BTN_B);
    long resumed_from = state_counter(alpha);
    CHECK(session(alpha, &next) == GAME_HOME);
    CHECK(state_counter(alpha) - resumed_from > 100); // ~1 s before the menu + ~1.2 s after resuming

    // POWER sleeps (nothing presented, no player running) and POWER wakes it again. The
    // panel goes dark and the sound stops before the save is written, so the press feels
    // instant; the save is done before the device wakes.
    long beta_unsaved = state_counter(beta);
    fp_reset();
    fp_limit(25);
    watched = beta;
    at_blank = at_wake = -2;
    fp_on_call(watch_saves);
    fp_press_at(0.5, BTN_POWER);
    fp_press_at(2.0, BTN_POWER);
    fp_press_at(2.4, BTN_MENU);
    fp_press_at(3.4, BTN_B);
    CHECK(session(beta, &next) == GAME_HOME);
    CHECK(fp()->standby_on == 1 && fp()->standby_off == 1);
    CHECK(fp()->asleep_presents == 0);
    CHECK(fp()->audio_off >= 2 && fp()->audio_on >= 2);
    CHECK(strstr(fp_log(), "audio_off standby_on standby_off") != NULL);
    CHECK(at_blank == beta_unsaved);
    CHECK(at_wake > 0 && at_wake != beta_unsaved);

    // POWER held while asleep: the save made on the way into standby is on the card
    // before the power goes.
    fp_reset();
    fp_limit(25);
    at_blank = at_off = -2;
    fp_on_call(watch_saves);
    fp_press_at(0.5, BTN_POWER);
    fp_press_at(2.0, BTN_POWER_OFF);
    long beta_before_sleep = state_counter(beta);
    CHECK(session(beta, &next) == GAME_POWER_OFF);
    CHECK(fp()->power_off == 1);
    CHECK(at_blank == beta_before_sleep);
    CHECK(at_off > beta_before_sleep);

    // POWER held: everything is saved before Shelf powers off.
    long beta_before = state_counter(beta);
    fp_reset();
    fp_limit(20);
    fp_press_at(0.6, BTN_POWER_OFF);
    CHECK(session(beta, &next) == GAME_POWER_OFF);
    CHECK(fp()->power_off == 1);
    CHECK(state_counter(beta) > beta_before && exists(beta, ".png"));

    // A missing core fails with a message, and writes nothing.
    char bad[PATH_LEN * 2];
    snprintf(bad, sizeof bad, "%s/missing_libretro.so", argv[1]);
    setenv("SHELF_CORE", bad, 1);
    CHECK(!exists(gamma, ".state"));
    fp_reset();
    fp_limit(5);
    CHECK(session(gamma, &next) == GAME_FAILED);
    CHECK(game_error()[0] != 0);
    CHECK(!exists(gamma, ".state") && !exists(gamma, ".srm") && !exists(gamma, ".png"));
    setenv("SHELF_CORE", core, 1);

    // A corrupt state is reported and the game starts fresh rather than failing.
    char path[PATH_LEN * 2];
    saved(gamma, ".state", path, sizeof path);
    CHECK(write_file(path, "bad", 3) == 0);
    fp_reset();
    fp_limit(20);
    fp_press_at(0.4, BTN_MENU);
    fp_press_at(1.4, BTN_B);
    CHECK(session(gamma, &next) == GAME_HOME);
    CHECK(frame_counter(fp_first_frame()) == 0);

    // A save that can't be written is reported and leaves the previous state alone.
    char dev[PATH_LEN], dir[PATH_LEN];
    lib_save_dir(&lib, gamma, dev, sizeof dev);
    host_path(dev, dir, sizeof dir);
    long good = state_counter(gamma);
    CHECK(chmod(dir, 0555) == 0);
    fp_reset();
    fp_limit(20);
    fp_press_at(0.4, BTN_MENU);
    fp_press_at(1.4, BTN_B);
    CHECK(session(gamma, &next) == GAME_HOME);
    CHECK(game_error()[0] != 0);
    CHECK(state_counter(gamma) == good);
    CHECK(chmod(dir, 0755) == 0);

    icons_free();
    lib_free(&lib);
    if (failures) return 1;
    puts("game: play, resume, switch, standby, power off, failures and saving all behave");
    return 0;
}
