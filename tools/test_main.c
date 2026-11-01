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

/* ---------- scenarios ---------- */

// When each logged call first happened, for the checks.
static double at_dim, at_undim, at_standby, at_wake, at_off, at_game;
static void times(const char *call)
{
    double now = platform_now();
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
    fp_on_call(times);
}

// Runs `scenario` in a child process; its failures count here.
static void run(const char *name, void (*scenario)(void))
{
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); failures++; return; }
    if (pid == 0) {
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

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "usage: %s <work dir> <fonts> <fake core .so>\n", argv[0]);
        return 2;
    }
    build_card(argv[1]);
    setenv("SHELF_ROOT", sd, 1);
    setenv("SHELF_FONTS", argv[2], 1);
    setenv("SHELF_CORE", argv[3], 1);
    unsetenv("SHELF_SHOT");
    unsetenv("SHELF_BENCH");
    unsetenv("SHELF_STATS");

    run("home dims", home_dims);
    run("home sleeps, then powers off", home_sleeps_then_powers_off);
    run("home wakes", home_wakes);

    if (failures) return 1;
    puts("main: the home screen dims, sleeps, wakes and powers off on its own");
    return 0;
}
