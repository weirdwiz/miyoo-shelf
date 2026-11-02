// Shelf: a DSi/3DS-style launcher for Game Boy, Game Boy Color and Game Boy Advance games
// on the Miyoo Mini. It shows recently played games and library folders; picking a game
// starts it inside Shelf, which saves and returns here when the player leaves it.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "game.h"
#include "icons.h"
#include "library.h"
#include "loop.h"
#include "platform.h"
#include "power.h"
#include "probe.h"
#include "settings.h"
#include "ui.h"

// Battery and charging state, for the status bar and the power policy.
static PowerBattery status_poll(void)
{
    int charging = 0;
    int battery = platform_battery(&charging);
    ui_set_status(battery, charging);
    return (PowerBattery){battery, charging, battery >= 0};
}

static int write_ppm(const Image *img, const char *out)
{
    FILE *f = fopen(out, "wb");
    if (!f) return 1;
    fprintf(f, "P6\n%d %d\n255\n", img->w, img->h);
    for (int i = 0; i < img->w * img->h; i++) {
        uint32_t p = img->px[i];
        unsigned char rgb3[3] = {p >> 16, p >> 8, p};
        fwrite(rgb3, 1, 3, f);
    }
    fclose(f);
    return 0;
}

// SHELF_SHOT=out.ppm SHELF_KEYS="RRA..." renders without a window: each key is pressed
// and given 0.6s of animation, then the final frame is written. Keys: U D L R A B Y l r s
// (s = START), and X for favorites.
static int headless(Library *lib, Image *canvas, const char *keys, const char *out)
{
    static const char MAP[] = "UDLRABYXlrs";
    static const Button BTN[] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B,
                                 BTN_Y, BTN_X, BTN_L, BTN_R, BTN_START};
    for (const char *k = keys ? keys : ""; ; k++) {
        const char *m = *k ? strchr(MAP, *k) : NULL;
        if (m) ui_button(BTN[m - MAP]);
        for (int f = 0; f < 36; f++) {
            ui_draw(canvas); // requests the icons on screen
            while (icons_pump(64)) {}
            ui_update(1.f / 60);
            int gi = ui_take_launch(); // as if it had played: the game becomes the newest
            if (gi >= 0) {
                if (lib_mark_played(lib, gi)) fprintf(stderr, "shelf: couldn't save recents\n");
                ui_reset_home();
            }
            ui_take_action(); // a screenshot never saves settings
        }
        if (!*k) break;
    }
    ui_draw(canvas);
    return write_ppm(canvas, out);
}

// SHELF_BENCH frames draws the screen as fast as it can, without a display. Scenes: home,
// grid (a folder), scroll (the home row), transition (in and out of a folder).
static int bench(Library *lib, Image *canvas)
{
    int n = atoi(getenv("SHELF_BENCH"));
    if (n < 1 || n > 10000) return 1;
    const char *scene = getenv("SHELF_BENCH_SCENE");
    if (!scene) scene = "home";
    int grid = !strcmp(scene, "grid"), scroll = !strcmp(scene, "scroll");
    int transitions = !strcmp(scene, "transition");
    if (!grid && !scroll && !transitions && strcmp(scene, "home")) return 1;
    // Grid and transitions start from the first library folder (A on a recent plays it).
    if (grid || transitions) ui_button(BTN_R);
    if (grid) ui_button(BTN_A);
    // Warm the same initial scene in both renderers; navigation still exercises newly
    // visible tiles and art requests inside the measured loop.
    for (int i = 0; i < 60; i++) {
        ui_draw(canvas);
        while (icons_pump(64)) {}
        ui_update(1.f / 60);
    }
    icons_start_worker(); // as in the interactive loop: art decodes off the frame
    double t0 = platform_now(), worst_frame = 0;
    int over = 0;
    for (int i = 0; i < n; i++) {
        double begin = platform_now();
        if (scroll && i % 12 == 0) ui_button((i / 72) % 2 ? BTN_LEFT : BTN_RIGHT);
        if (grid && i % 12 == 0) {
            static const Button moves[] = {BTN_DOWN, BTN_DOWN, BTN_RIGHT, BTN_UP, BTN_UP, BTN_LEFT};
            ui_button(moves[(i / 12) % 6]);
        }
        if (transitions && i % 36 == 0) ui_button((i / 36) % 2 ? BTN_B : BTN_A);
        icons_pump(2);
        ui_update(1.f / 60);
        ui_draw(canvas);
        double elapsed = platform_now() - begin;
        if (elapsed > worst_frame) worst_frame = elapsed;
        if (elapsed > .015) over++; // 16.7 ms vblank less ~1.6 ms rotate into fb
    }
    fprintf(stderr, "shelf: %s benchmark %d frames, %.2f ms/frame, worst %.2f ms, %d over 15 ms\n",
            scene, n, (platform_now() - t0) * 1000 / n, worst_frame * 1000, over);
    return 0;
}

/* ---------- frame loop screens (see loop.h) ---------- */

static void screen_button(void *ud, Button b)
{
    ui_button(b);
    if (ui_take_sound()) platform_click();
}
static LoopIO loop_platform(void)
{
    return (LoopIO){platform_now, platform_poll, platform_wait, platform_present, platform_vsync_paced()};
}

static void screen_update(void *ud, float dt) { ui_update(dt); }
static double screen_idle(void *ud) { return ui_idle(); }
static void screen_draw(void *ud, Image *canvas) { ui_draw(canvas); }

typedef struct {
    Library *lib;
    Settings *set;
    int quit, launch, power_off;
    PowerState power;
    double last_input;
    PowerBattery battery;
} HomeLoop;

// Saves what the Options panel changed and applies it to the hardware.
static void settings_apply(HomeLoop *l)
{
    Settings s = ui_settings();
    *l->set = s;
    platform_set_brightness(s.brightness);
    platform_set_volume(s.volume);
    if (settings_save(&s)) fprintf(stderr, "shelf: couldn't save the settings\n");
}

// Blanks the panel and sleeps until POWER wakes it, or powers off once the policy's
// limit passes. Returns 1 when Shelf should power off.
static int home_standby(void)
{
    platform_standby(1);
    PowerState asleep;
    power_init(&asleep);
    double since = platform_now();
    int off = 0;
    for (;;) {
        PowerDecision d = power_step(&asleep, platform_now(), since, POWER_ASLEEP, (PowerBattery){0});
        if (d.action == POWER_OFF_RESUMABLE) { off = 1; break; }
        platform_wait(d.next); // no polling: the next input, or the limit, ends the sleep
        Button b;
        while ((b = platform_poll()) != BTN_NONE) {
            if (b == BTN_POWER) goto woke;
            if (b == BTN_POWER_OFF || b == BTN_QUIT) { off = 1; goto woke; }
        }
    }
woke:
    platform_standby(0);
    return off;
}

// Off from the home screen: the next boot starts at home.
static void home_power_off(HomeLoop *l)
{
    settings_save(l->set);
    l->power_off = 1;
    platform_power_off();
}

// Carries out what the power policy asks for until it asks for nothing. Returns when it
// next wants to run; *undimmed says whether this woke a dimmed screen.
static double home_power(HomeLoop *l, int *undimmed)
{
    for (int i = 0; i < 8 && !l->power_off; i++) {
        double now = platform_now();
        if (power_battery_due(&l->power, now)) l->battery = status_poll();
        PowerDecision d = power_step(&l->power, now, l->last_input, POWER_HOME, l->battery);
        switch (d.action) {
        case POWER_NONE:
            return d.next;
        case POWER_DIM:
            platform_dim(1);
            break;
        case POWER_UNDIM:
            platform_dim(0);
            if (undimmed) *undimmed = 1;
            break;
        case POWER_STANDBY:
            if (home_standby()) { home_power_off(l); break; }
            l->last_input = platform_now();
            ui_button(BTN_REDRAW);
            break;
        case POWER_OFF_RESUMABLE: // the battery is nearly flat
            home_power_off(l);
            break;
        case POWER_LOW_BATTERY_WARNING:
            ui_set_toast("Battery low");
            break;
        }
    }
    return platform_now();
}

static void home_button(void *ud, Button b)
{
    HomeLoop *l = ud;
    if (b == BTN_QUIT) { l->quit = 1; return; } // the window closed
    if (b != BTN_REDRAW) {
        int undimmed = 0;
        l->last_input = platform_now();
        home_power(l, &undimmed);
        if (l->power_off) return;
        // A dimmed screen: the press only wakes it, unless it was POWER or the volume.
        if (undimmed && b != BTN_POWER && b != BTN_POWER_OFF && b != BTN_VOLUME) return;
    }
    if (b == BTN_POWER) {
        if (!home_standby()) {
            l->last_input = platform_now();
            ui_button(BTN_REDRAW);
            return;
        }
        home_power_off(l);
        return;
    }
    if (b == BTN_POWER_OFF) { // held: save everything and go
        home_power_off(l);
        return;
    }
    if (b == BTN_VOLUME) { // the hardware keys moved the volume: remember it
        Settings s = ui_settings();
        s.volume = platform_volume();
        ui_set_settings(&s);
        *l->set = s;
        settings_save(&s);
        return;
    }
    screen_button(ud, b);
}

static double home_tick(void *ud, double now)
{
    HomeLoop *l = ud;
    double next = home_power(l, NULL); // battery, dimming and sleeping
    icons_pump(2); // only without the worker; new art makes ui_idle draw
    icons_trim();
    return icons_pending() && now < next ? now : next;
}

static int home_after(void *ud)
{
    HomeLoop *l = ud;
    if (l->quit || l->power_off) return 1;
    int action = ui_take_action();
    if (action == UI_SETTINGS) {
        settings_apply(l);
    } else if (action == UI_POWER_OFF) {
        home_power_off(l);
        return 1;
    }
    int gi = ui_take_launch();
    if (gi < 0) return 0;
    l->launch = gi;
    return 1;
}

// SHELF_STATS: draw and present times, frames and idle waits, every 120 frames.
static struct {
    double t0, draw, present, worst;
    int frames;
    LoopStats loop;
} stats;

static void stats_draw(void *ud, Image *canvas)
{
    double t = platform_now();
    ui_draw(canvas);
    double d = platform_now() - t;
    stats.draw += d;
    if (d > stats.worst) stats.worst = d;
}

static void stats_present(const Image *frame)
{
    double t = platform_now();
    platform_present(frame);
    stats.present += platform_now() - t;
    if (++stats.frames < 120) return;
    double el = t - stats.t0;
    fprintf(stderr, "shelf: %d frames in %.1f s, %u idle waits; render %.2f ms (worst %.1f), present %.2f ms\n",
            stats.frames, el, stats.loop.waits, stats.draw * 1000 / stats.frames, stats.worst * 1000,
            stats.present * 1000 / stats.frames);
    stats.frames = 0;
    stats.loop.waits = 0;
    stats.draw = stats.present = stats.worst = 0;
    stats.t0 = platform_now();
}

int main(void)
{
    const char *root = getenv("SHELF_ROOT");   // simulator: folder standing in for /mnt/SDCARD
    const char *fonts = getenv("SHELF_FONTS"); // defaults to Shelf's own fonts folder
    if (root) lib_set_root(root);

    char font_dir[PATH_LEN];
    if (fonts) snprintf(font_dir, sizeof font_dir, "%s", fonts);
    else host_path(SD_PREFIX "/.shelf/fonts", font_dir, sizeof font_dir);

    Library lib;
    if (lib_load(&lib)) return 1;
    fprintf(stderr, "shelf: %d systems, %d games, %d recent\n", lib.nsys, lib.ngames, lib.nrecent);

    Settings set;
    settings_load(&set);
    const char *theme = getenv("SHELF_THEME"); // screenshots override the saved theme
    if (theme) set.dark = !strcmp(theme, "dark");

    if (icons_init(&lib)) fprintf(stderr, "shelf: no memory for artwork; showing none\n");
    Image *canvas = img_new(SCREEN_W, SCREEN_H);
    if (!canvas) return 1;
    status_poll();

    const char *shot = getenv("SHELF_SHOT");
    if (shot || getenv("SHELF_BENCH")) {
        ui_set_dark(set.dark); // before ui_init: the background is built in this palette
        if (ui_init(&lib, font_dir)) { fprintf(stderr, "shelf: fonts missing in %s\n", font_dir); return 1; }
        ui_set_settings(&set);
        int rc = shot ? headless(&lib, canvas, getenv("SHELF_KEYS"), shot) : bench(&lib, canvas);
        ui_free();
        icons_free();
        lib_free(&lib);
        img_free(canvas);
        return rc;
    }

    // Powered off from a game last time: straight back into it. Taking the marker deletes
    // it, first thing, so a game that can't start doesn't come back on every boot.
    int resume = lib_take_resume(&lib);

    if (platform_init()) return 1;
    platform_set_brightness(set.brightness);
    platform_set_volume(set.volume);
    const char *probe = getenv("SHELF_PROBE"); // the boot script sets it for one boot
    if (probe && *probe) {
        char path[PATH_LEN];
        host_path(DATA_DIR "/probe.txt", path, sizeof path);
        if (probe_run(path)) fprintf(stderr, "shelf: couldn't write %s\n", path);
    }
    if (icons_start_worker()) fprintf(stderr, "shelf: icon worker unavailable; loading art per frame\n");
    icons_set_notify(platform_wake);

    int quit = 0, power_off = 0;
    char toast[192] = ""; // a failure the home screen should show
    while (!quit && !power_off) {
        int want = resume;
        resume = -1;
        if (want < 0) { // the home screen, until it launches a game
            ui_set_dark(set.dark);
            // The first visit loads the UI; every later one (the menu may have loaded it
            // already) only enters it, so coming home reads nothing from the card.
            if (ui_loaded() ? ui_enter_home() : ui_init(&lib, font_dir)) {
                fprintf(stderr, "shelf: fonts missing in %s\n", font_dir);
                break;
            }
            ui_set_settings(&set);
            if (toast[0]) {
                ui_set_toast(toast);
                toast[0] = 0;
            }
            HomeLoop hl = {&lib, &set, 0, -1, 0};
            power_init(&hl.power);
            hl.last_input = platform_now();
            LoopScreen screen = {&hl, home_button, home_tick, screen_update, screen_idle, screen_draw, home_after};
            LoopIO io = loop_platform();
            if (getenv("SHELF_STATS")) {
                stats.t0 = platform_now();
                screen.draw = stats_draw;
                io.present = stats_present;
            }
            loop_run(&io, &screen, canvas, &stats.loop);
            set = ui_settings();
            ui_leave();
            if (hl.quit || hl.power_off) break;
            want = hl.launch;
        }

        // A game, and then whichever game its in-game menu asked for.
        for (;;) {
            if (want < 0) break;
            if (lib_mark_played(&lib, want)) fprintf(stderr, "shelf: couldn't save recents\n");
            GameSettings gs = {set.scale, font_dir, &set}; // the colour row saves into set
            int next = -1;
            int result = game_play(&lib, want, &gs, &next);
            if (game_error()[0]) snprintf(toast, sizeof toast, "%s", game_error());
            if (result == GAME_SWITCH && next >= 0 && next != want) {
                want = next;
                continue;
            }
            if (result == GAME_POWER_OFF) power_off = 1;
            break;
        }
    }

    ui_free();
    icons_set_notify(NULL);
    img_free(canvas);
    icons_free();
    lib_free(&lib);
    platform_quit();
    return 0;
}
