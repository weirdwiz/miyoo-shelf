// Shelf from boot, on the fake platform: main.c (built with main renamed shelf_main), the
// home screen, the power policy and play sessions with the fake core. Each scenario runs
// in its own process, so main's statics start fresh, and ends by closing the "window"
// (BTN_QUIT) or by powering off.
// Usage: test-main <work dir> <fonts dir> <fake core .so>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/library.h"
#include "../src/text.h"
#include "fake_platform.h"

int shelf_main(void);

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static char sd[PATH_LEN]; // the fake SD card's root

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

// Two GB games, Alpha the most recent: A on the home screen plays it.
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
    snprintf(path, sizeof path, "%s/.shelf/data", sd);
    CHECK(mkpath(path) == 0);
    static const char recents[] = "[\"/mnt/SDCARD/Roms/GB/Alpha.gb\", \"/mnt/SDCARD/Roms/GB/Beta.gb\"]\n";
    snprintf(path, sizeof path, "%s/.shelf/data/recents.json", sd);
    CHECK(write_file(path, recents, sizeof recents - 1) == 0);
}

/* ---------- the resume marker ---------- */

static char data_dir[PATH_LEN * 2], marker[PATH_LEN * 2 + 16];

static void set_marker(const char *rom)
{
    char line[PATH_LEN + 2];
    snprintf(line, sizeof line, "%s\n", rom);
    CHECK(write_file(marker, line, strlen(line)) == 0);
}

static int marked(const char *rom) // the marker names `rom` ("": there is none)
{
    char line[PATH_LEN] = "";
    FILE *f = fopen(marker, "rb");
    if (!f) return !rom[0];
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    line[strcspn(line, "\n")] = 0;
    return rom[0] && !strcmp(line, rom);
}

#define ALPHA "/mnt/SDCARD/Roms/GB/Alpha.gb"

// The fake core paints the low byte of its frame counter into row 3, one bit per pixel:
// a game resumed from its state doesn't start at 0.
static int frame_counter(const Image *f)
{
    if (!f) return -1;
    int v = 0;
    for (int i = 0; i < 8; i++)
        if (((f->px[3 * f->w + i] >> 16) & 0xff) > 127) v |= 1 << i;
    return v;
}

/* ---------- scenarios ---------- */

// When each logged call first happened, for the checks.
static double at_dim, at_undim, at_standby, at_wake, at_off, at_game;
static int marker_at_game = -1; // the marker still existed when the game started
static int game_starts;
static void times(const char *call)
{
    double now = platform_now();
    // A game starting turns its sound on; the CPU clock also goes up for the exit save.
    if (!strcmp(call, "audio_on")) game_starts++;
    if (!strcmp(call, "perf_on") && at_game < 0) marker_at_game = access(marker, F_OK) == 0;
    if (!strcmp(call, "dim_on") && at_dim < 0) at_dim = now;
    if (!strcmp(call, "dim_off") && at_undim < 0) at_undim = now;
    if (!strcmp(call, "standby_on") && at_standby < 0) at_standby = now;
    if (!strcmp(call, "standby_off") && at_wake < 0) at_wake = now;
    if (!strcmp(call, "power_off") && at_off < 0) at_off = now;
    if (!strcmp(call, "perf_on") && at_game < 0) at_game = now;
}

static void start(double limit)
{
    fp_reset();
    fp_limit(limit);
    at_dim = at_undim = at_standby = at_wake = at_off = at_game = -1;
    game_starts = 0;
    fp_on_call(times);
}

// Runs `scenario` in a child process; its failures count here.
static void run(const char *name, void (*scenario)(void))
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); failures++; return; }
    if (pid == 0) {
        failures = 0; // this scenario's own, not the ones before it
        scenario();
        fflush(NULL);
        _exit(failures ? 1 : 0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
        fprintf(stderr, "FAIL scenario: %s\n", name);
        failures++;
    }
}

// Idle on the home screen: dim at 45 s. The press that wakes it does nothing else, the
// next one does.
static void home_dims(void)
{
    start(200);
    fp_press_at(60, BTN_A);     // would play Alpha
    fp_press_at(61, BTN_RIGHT); // moves the selection, with a click
    fp_press_at(70, BTN_QUIT);
    CHECK(shelf_main() == 0);
    CHECK(at_dim == 45);
    CHECK(at_undim == 60);
    CHECK(at_game < 0);         // A only woke the screen
    CHECK(fp()->clicks == 1);   // RIGHT did move
    CHECK(fp()->dims == 1 && !fp()->dimmed);
    CHECK(fp()->standby_on == 0 && fp()->power_off == 0);
}

// Still idle: standby after 3 minutes, and off after 10 minutes asleep.
static void home_sleeps_then_powers_off(void)
{
    start(1000);
    CHECK(shelf_main() == 0);
    CHECK(at_dim == 45);
    CHECK(at_standby == 180);
    CHECK(at_off == 780);
    CHECK(fp()->power_off == 1);
    CHECK(fp()->asleep_presents == 0);
}

// POWER wakes it: the idle clock starts again from the wake.
static void home_wakes(void)
{
    start(1000);
    fp_press_at(300, BTN_POWER);
    fp_press_at(400, BTN_QUIT);
    CHECK(shelf_main() == 0);
    CHECK(at_standby == 180 && at_wake == 300);
    CHECK(fp()->dims == 2 && fp()->dimmed); // again at 345
    CHECK(fp()->standby_on == 1 && fp()->power_off == 0);
}

// Holding POWER in a game saves it, marks it and powers off...
static void game_powers_off(void)
{
    start(60);
    fp_press_at(1, BTN_A); // Alpha, the newest recent
    fp_press_at(8, BTN_POWER_OFF);
    CHECK(shelf_main() == 0);
    CHECK(at_game > 1 && at_game < 8);
    CHECK(at_off >= 8);
    CHECK(marked(ALPHA));
}

// ...and the next boot goes straight back into it, from its saved state, having deleted
// the marker before the game started.
static void boots_into_the_game(void)
{
    start(60);
    fp_press_at(2, BTN_MENU);
    fp_press_at(3, BTN_B); // home
    fp_press_at(6, BTN_QUIT);
    CHECK(shelf_main() == 0);
    CHECK(at_game >= 0 && at_game < .1);
    CHECK(marker_at_game == 0);
    CHECK(frame_counter(fp_first_frame()) > 0);
    CHECK(marked(""));
}

// Home -> game -> menu -> home, three times over: the UI is loaded once (Bold and
// ExtraBold are each read from the card once), however often home and the menu open.
static void cycles_read_fonts_once(void)
{
    start(120);
    for (int i = 0; i < 3; i++) {
        double t = 1 + i * 10;
        fp_press_at(t, BTN_A);        // home: play Alpha
        fp_press_at(t + 4, BTN_MENU); // the in-game menu
        fp_press_at(t + 6, BTN_B);    // back to Shelf
    }
    fp_press_at(40, BTN_QUIT);
    CHECK(shelf_main() == 0);
    CHECK(font_files_read() == 2);
    CHECK(game_starts == 3);      // each visit really played, then came home
}

// No marker: home.
static void boots_home(void)
{
    start(60);
    fp_press_at(2, BTN_QUIT);
    CHECK(shelf_main() == 0);
    CHECK(at_game < 0 && fp()->presents > 0);
}

// Powering off from home leaves no marker.
static void home_powers_off(void)
{
    start(60);
    fp_press_at(2, BTN_POWER_OFF);
    CHECK(shelf_main() == 0);
    CHECK(fp()->power_off == 1 && at_game < 0);
    CHECK(marked(""));
}

// A game that can't start (here: its core is missing) is tried once; the marker is gone,
// so the boot after that starts at home rather than failing the same way forever.
static void bad_resume_tried_once(void)
{
    set_marker(ALPHA);
    char missing[PATH_LEN * 2];
    snprintf(missing, sizeof missing, "%s/missing_libretro.so", sd);
    setenv("SHELF_CORE", missing, 1);
    start(60);
    fp_press_at(3, BTN_QUIT);
    CHECK(shelf_main() == 0);
    CHECK(fp()->presents > 0); // home, with the failure as a toast
    CHECK(marked(""));
}

// A marker naming a game that is no longer on the card: deleted, home.
static void unknown_game(void)
{
    set_marker("/mnt/SDCARD/Roms/GB/Gone.gb");
    start(60);
    fp_press_at(2, BTN_QUIT);
    CHECK(shelf_main() == 0);
    CHECK(at_game < 0);
    CHECK(marked(""));
}

// A marker that can't be deleted could boot into the same game forever: home instead.
static void stuck_marker(void)
{
    set_marker(ALPHA);
    CHECK(chmod(data_dir, 0555) == 0);
    start(60);
    fp_press_at(2, BTN_QUIT);
    CHECK(shelf_main() == 0);
    CHECK(at_game < 0);
    CHECK(chmod(data_dir, 0755) == 0);
    unlink(marker);
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <work dir> <fonts> <fake core .so>\n", argv[0]);
        return 2;
    }
    build_card(argv[1]);
    snprintf(data_dir, sizeof data_dir, "%s/.shelf/data", sd);
    snprintf(marker, sizeof marker, "%s/resume", data_dir);
    setenv("SHELF_ROOT", sd, 1);
    setenv("SHELF_FONTS", argv[2], 1);
    setenv("SHELF_CORE", argv[3], 1);
    unsetenv("SHELF_SHOT");
    unsetenv("SHELF_BENCH");
    unsetenv("SHELF_STATS");

    run("home dims", home_dims);
    run("home sleeps, then powers off", home_sleeps_then_powers_off);
    run("home wakes", home_wakes);
    run("a game powers off", game_powers_off);
    run("the next boot resumes it", boots_into_the_game);
    run("then home again", boots_home);
    run("home powers off", home_powers_off);
    run("and boots home", boots_home);
    run("home and menu cycles load the fonts once", cycles_read_fonts_once);
    run("a resume that fails is tried once", bad_resume_tried_once);
    run("then home", boots_home);
    run("an unknown game isn't resumed", unknown_game);
    if (geteuid() != 0) run("nor a marker that can't be deleted", stuck_marker); // root ignores 0555

    if (failures) return 1;
    puts("main: home dims, sleeps and powers off; a game powered off resumes once on the next boot");
    return 0;
}
