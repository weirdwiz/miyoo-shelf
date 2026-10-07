// Shelf: a DSi/3DS-style launcher that stands in for Onion's MainUI.
// It shows Recent games and Library folders, and when you pick a game it writes
// /tmp/cmd_to_run.sh and exits. Onion's runtime then runs the game and starts us again.
#include <arpa/inet.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../third_party/cJSON.h"
#include "activity.h"
#include "icons.h"
#include "library.h"
#include "loop.h"
#include "platform.h"
#include "switcher.h"
#include "ui.h"

// Exit codes that ask mainui.sh / launch.sh to run boot.sh after Shelf quits.
#define EXIT_BOOT_ONION 10 // boot.sh disable
#define EXIT_BOOT_SHELF 11 // boot.sh enable

// Settings live beside the app: {"dark": true}.
static void settings_path(char *out, int n) { host_path(SD_PREFIX "/App/Shelf/settings.json", out, n); }

static void settings_load(void)
{
    char path[PATH_LEN], buf[256];
    settings_path(path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = 0;
    cJSON *j = cJSON_Parse(buf);
    ui_set_dark(cJSON_IsTrue(cJSON_GetObjectItem(j, "dark")));
    cJSON_Delete(j);
}

static void settings_save(void)
{
    char path[PATH_LEN], tmp[PATH_LEN + 4];
    settings_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { fprintf(stderr, "shelf: can't save %s\n", path); return; }
    fprintf(f, "{\"dark\": %s}\n", ui_dark() ? "true" : "false");
    if (fclose(f) == 0) rename(tmp, path);
}

static int read_small(const char *path, char *buf, int n)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t len = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[len] = 0;
    return (int)len;
}

// Battery from Onion's batmon (/tmp/.axp_result on the Mini Plus, else /tmp/percBat) and
// Wi-Fi from the kernel. SHELF_STATUS="pct,charging,wifi" stands in on the simulator.
static void status_poll(int sim)
{
    int battery = -1, charging = 0, wifi = -1;
    char buf[512];
    const char *fake = getenv("SHELF_STATUS");
    if (fake || sim) {
        if (!fake || sscanf(fake, "%d,%d,%d", &battery, &charging, &wifi) != 3) {
            battery = 76; charging = 0; wifi = 3;
        }
        ui_set_status(battery, charging, wifi);
        return;
    }
    if (read_small("/tmp/.axp_result", buf, sizeof buf) > 0) {
        cJSON *j = cJSON_Parse(buf);
        cJSON *b = cJSON_GetObjectItem(j, "battery"), *ch = cJSON_GetObjectItem(j, "charging");
        if (cJSON_IsNumber(b)) battery = b->valueint;
        charging = cJSON_IsNumber(ch) && ch->valueint;
        cJSON_Delete(j);
    }
    if (battery < 0 && read_small("/tmp/percBat", buf, sizeof buf) > 0) battery = atoi(buf);
    if (battery > 100) battery = 100;

    unsigned flags = 0;
    if (read_small("/sys/class/net/wlan0/flags", buf, sizeof buf) > 0 &&
        sscanf(buf, "%x", &flags) == 1 && (flags & 1)) { // IFF_UP: Onion's Wi-Fi is on
        wifi = 0;
        if (read_small("/sys/class/net/wlan0/operstate", buf, sizeof buf) > 0 && !strncmp(buf, "up", 2) &&
            read_small("/proc/net/wireless", buf, sizeof buf) > 0) {
            const char *line = strstr(buf, "wlan0:");
            float link, level;
            int status;
            if (line && sscanf(line + 6, "%x %f %f", &status, &link, &level) == 3)
                wifi = level >= -60 ? 3 : level >= -70 ? 2 : 1;
            else wifi = 1;
        }
    }
    ui_set_status(battery, charging, wifi);
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
// (s = START).
static int headless(Library *lib, const char *cmd_path, Image *canvas, const char *keys, const char *out)
{
    static const char MAP[] = "UDLRABYlrs";
    static const Button BTN[] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_Y, BTN_L, BTN_R, BTN_START};
    for (const char *k = keys ? keys : ""; ; k++) {
        const char *m = *k ? strchr(MAP, *k) : NULL;
        if (m) ui_button(BTN[m - MAP]);
        for (int f = 0; f < 36; f++) {
            ui_draw(canvas); // requests the icons on screen
            while (icons_pump(64)) {}
            ui_update(1.f / 60);
            int g = ui_take_launch();
            if (g >= 0) { lib_launch(lib, g, cmd_path); ui_reset_home(); }
            ui_take_action(); // a screenshot never saves settings or quits
        }
        if (!*k) break;
    }
    ui_draw(canvas);
    return write_ppm(canvas, out);
}

/* ---------- game switcher ---------- */

#define SYSDIR SD_PREFIX "/.tmp_update"

static void touch(const char *dev)
{
    char host[PATH_LEN];
    host_path(dev, host, sizeof host);
    FILE *f = fopen(host, "w");
    if (f) fclose(f);
}

static void unlink_dev(const char *dev)
{
    char host[PATH_LEN];
    host_path(dev, host, sizeof host);
    remove(host);
}

// If Shelf's switcher dies over a paused game, the game would stay frozen with MENU
// disabled. Unpause it on the way out (sendto is async-signal-safe).
static void overlay_crash(int sig)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(55355);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sendto(fd, "UNPAUSE", 7, 0, (struct sockaddr *)&to, sizeof to);
    unlink(SYSDIR "/.runGameSwitcher");
    _exit(128 + sig);
}

// Onion's own switcher, copied aside by boot.sh before it mounts Shelf over it.
static void exec_onion_switcher(char **argv)
{
    static const char ONION[] = SD_PREFIX "/App/Shelf/boot/gameSwitcher-onion";
    if (access(ONION, X_OK)) return;
    fprintf(stderr, "shelf: falling back to Onion's switcher\n");
    execv(ONION, argv);
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
    int sim, running, toast;
    double next_status;
} SwitcherLoop;

static double switcher_tick(void *ud, double now)
{
    SwitcherLoop *l = ud;
    if (now >= l->next_status) { status_poll(l->sim); l->next_status = now + 5; }
    icons_trim();
    int save = l->running ? sw_save_state() : SW_SAVE_NONE;
    int t = save == SW_SAVE_BUSY ? SW_TOAST_SAVING : save == SW_SAVE_DONE ? SW_TOAST_SAVED
          : save == SW_SAVE_FAILED ? SW_TOAST_FAILED : SW_TOAST_NONE;
    if (t != l->toast) ui_switch_saving(l->toast = t);
    return l->next_status;
}

static int switcher_after(void *ud) { return ui_take_action(); }

static int switcher_main(int overlay, int sim, const char *font_dir, char **argv)
{
    static Recent rec[MAX_RECENT];
    static Library lib;
    double t0 = platform_now();
    Image *canvas = img_new(SCREEN_W, SCREEN_H), *frame = img_new(SCREEN_W, SCREEN_H);
    // The paused game's last frame, before anything draws over it.
    int have_frame = overlay && canvas && frame && !platform_grab(frame);
    if (lib_load_recents(&lib, rec, MAX_RECENT) || !canvas) {
        if (overlay && !sim) exec_onion_switcher(argv);
        return 1;
    }
    int running = overlay && lib.ngames && sw_overlay_begin(have_frame ? frame : NULL, &rec[0]);
    if (sim && overlay && getenv("SHELF_FRAME")) running = lib.ngames > 0; // no RetroArch to ask
    if (overlay && !sim) {
        signal(SIGSEGV, overlay_crash);
        signal(SIGBUS, overlay_crash);
        signal(SIGABRT, overlay_crash);
        signal(SIGFPE, overlay_crash);
        setenv("SHELF_MUTE", "1", 1); // RetroArch has the audio device
    }
    settings_load();
    icons_init(&lib);
    sw_cards_start(&lib, rec, 248, 186, running && have_frame ? frame : NULL);
    if (running && have_frame) frame = NULL;
    ui_switch_hooks(sw_card, sw_full, sw_card_select, sw_cards_revision);
    const char *shot = getenv("SHELF_SHOT");
    if (shot) { // headless, as for Shelf: SHELF_FRAMES frames (default 36) after each key
        static const char MAP[] = "LRABlrm";
        static const Button BTN[] = {BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_L, BTN_R, BTN_MENU};
        const char *fr = getenv("SHELF_FRAMES");
        int frames = fr ? atoi(fr) : 36;
        if (ui_init_switcher(&lib, font_dir, overlay, running)) return 1;
        ui_draw(canvas);
        fprintf(stderr, "shelf: switcher first frame after %.0f ms\n", (platform_now() - t0) * 1000);
        ui_set_status(76, 0, 3);
        for (const char *k = getenv("SHELF_KEYS") ? getenv("SHELF_KEYS") : ""; ; k++) {
            const char *m = *k ? strchr(MAP, *k) : NULL;
            if (m) ui_button(BTN[m - MAP]);
            for (int f = 0; f < frames; f++) {
                if (f == 0) usleep(150000); // let the card worker catch up
                ui_switch_saving(running ? SW_TOAST_SAVED : SW_TOAST_NONE);
                ui_update(1.f / 60);
                ui_draw(canvas);
                while (icons_pump(64)) {}
                ui_take_action();
            }
            if (!*k) break;
        }
        ui_draw(canvas);
        int rc = write_ppm(canvas, shot);
        if (running) sw_overlay_resume(); // finishes the save, as leaving does
        sw_cards_stop();
        return rc;
    }
    // Render-only benchmark, as for Shelf (SHELF_SWITCHER=1 keeps RetroArch out of it).
    // Scenes: idle, scroll (left/right every 12 frames), zoom (open, then A, every 36).
    const char *bench = getenv("SHELF_BENCH");
    if (bench) {
        int n = atoi(bench);
        const char *scene = getenv("SHELF_BENCH_SCENE");
        if (!scene) scene = "idle";
        int scroll = !strcmp(scene, "scroll"), zoom = !strcmp(scene, "zoom");
        if (n < 1 || n > 10000 || (!scroll && !zoom && strcmp(scene, "idle"))) return 1;
        if (ui_init_switcher(&lib, font_dir, overlay, running)) return 1;
        fprintf(stderr, "shelf: switcher ready after %.0f ms\n", (platform_now() - t0) * 1000);
        for (int i = 0; i < 60; i++) { // let the cards load
            ui_update(1.f / 60);
            ui_draw(canvas);
            while (icons_pump(64)) {}
            usleep(5000);
        }
        icons_start_worker();
        double start = platform_now(), worst = 0;
        int over = 0;
        for (int i = 0; i < n; i++) {
            double begin = platform_now();
            if (scroll && i % 12 == 0) ui_button((i / 72) % 2 ? BTN_LEFT : BTN_RIGHT);
            if (zoom && i % 36 == 0) {
                if ((i / 36) % 2) ui_button(BTN_A);
                else ui_switch_reopen();
            }
            ui_update(1.f / 60);
            ui_draw(canvas);
            ui_take_action();
            double el = platform_now() - begin;
            if (el > worst) worst = el;
            if (el > .015) over++;
        }
        fprintf(stderr, "shelf: switcher %s benchmark %d frames, %.2f ms/frame, worst %.2f ms, %d over 15 ms\n",
                scene, n, (platform_now() - start) * 1000 / n, worst * 1000, over);
        if (running) sw_overlay_resume();
        sw_cards_stop();
        return 0;
    }
    if (ui_init_switcher(&lib, font_dir, overlay, running) || platform_init()) {
        fprintf(stderr, "shelf: switcher couldn't start\n");
        if (running) sw_overlay_resume();
        if (overlay && !sim) { unlink_dev(SYSDIR "/.runGameSwitcher"); exec_onion_switcher(argv); }
        return 1;
    }
    icons_start_worker();
    status_poll(sim);
    fprintf(stderr, "shelf: switcher up in %.0f ms (%d recents, %s)\n", (platform_now() - t0) * 1000,
            lib.ngames, running ? "game paused" : overlay ? "no game running" : "menu");

    SwitcherLoop sl = {sim, running, SW_TOAST_NONE, platform_now() + 5};
    LoopScreen screen = {&sl, screen_button, switcher_tick, screen_update, screen_idle, screen_draw, switcher_after};
    LoopIO io = loop_platform();
    sw_set_notify(platform_wake);
    icons_set_notify(platform_wake);
    int action = loop_run(&io, &screen, canvas, NULL);
    sw_set_notify(NULL);
    icons_set_notify(NULL);

    int target = ui_switch_target();
    fprintf(stderr, "shelf: switcher %s %s\n", action == UI_SW_RESUME ? "resume" : action == UI_SW_PLAY ? "play" : "home",
            target >= 0 ? rec[target].rompath : "");
    if (action == UI_SW_PLAY) {
        // Onion's resume: queue the game, keep the queued command over the runtime's
        // "back to MainUI" step, and auto-load its state.
        char cmd[PATH_LEN], tmp[PATH_LEN + 4];
        host_path(SYSDIR "/cmd_to_run.sh", cmd, sizeof cmd);
        snprintf(tmp, sizeof tmp, "%s.new", cmd);
        // The runtime's sh may still be reading cmd_to_run.sh (the game being left):
        // replace it by rename rather than rewriting it.
        if (lib_resume(&rec[target], tmp) || rename(tmp, cmd)) {
            fprintf(stderr, "shelf: couldn't queue the game\n");
            action = UI_SW_HOME;
        } else if (!sim) {
            touch("/tmp/quick_switch");
            touch("/tmp/force_auto_load_state");
        }
    }
    if (action == UI_SW_HOME) unlink_dev(SYSDIR "/cmd_to_run.sh");
    unlink_dev(SYSDIR "/.runGameSwitcher");
    sync();
    if (action == UI_SW_RESUME) sw_overlay_resume();
    else if (running) sw_quit_game();
    else if (overlay && !sim) sw_quit_game(); // RetroArch without our game: still close it

    sw_cards_stop();
    ui_free();
    icons_free();
    platform_quit();
    img_free(canvas);
    img_free(frame);
    lib_free(&lib);
    return 0;
}

typedef struct {
    Library *lib;
    const char *cmd_path;
    int sim, status;
    double next_status;
} HomeLoop;

static void home_button(void *ud, Button b)
{
    if (b == BTN_QUIT) ((HomeLoop *)ud)->status = -1;
    else screen_button(ud, b);
}

static double home_tick(void *ud, double now)
{
    HomeLoop *l = ud;
    if (now >= l->next_status) { status_poll(l->sim); l->next_status = now + 5; }
    icons_pump(2); // only without the worker; new art makes ui_idle draw
    icons_trim();
    return l->next_status;
}

// Returns nonzero to leave Shelf, with l->status as the exit code.
static int home_after(void *ud)
{
    HomeLoop *l = ud;
    if (l->status < 0) { l->status = 0; return 1; } // window closed
    switch (ui_take_action()) {
    case UI_EXIT: return 1; // to Onion's menu
    case UI_THEME: settings_save(); break;
    case UI_BOOT_ONION: l->status = EXIT_BOOT_ONION; break;
    case UI_BOOT_SHELF: l->status = EXIT_BOOT_SHELF; break;
    }
    if (l->status && l->sim) {
        fprintf(stderr, "shelf: would run boot.sh %s\n", l->status == EXIT_BOOT_ONION ? "disable" : "enable");
        l->status = 0;
    }
    if (l->status) return 1;
    int g = ui_take_launch();
    if (g >= 0) {
        int launched = lib_launch(l->lib, g, l->cmd_path) == 0;
        ui_reset_home();
        if (launched) {
            fprintf(stderr, "shelf: launch -> %s\n", l->cmd_path);
            if (!l->sim) return 1; // on the device Onion takes over from here
        } else {
            fprintf(stderr, "shelf: launch failed; staying in Shelf\n");
        }
    }
    return 0;
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

int main(int argc, char **argv)
{
    const char *root = getenv("SHELF_ROOT");      // simulator: folder standing in for /mnt/SDCARD
    const char *fonts = getenv("SHELF_FONTS");    // defaults to the installed app folder
    const char *cmd = getenv("SHELF_CMD");        // where to write the launch command
    int sim = root != NULL;
    if (root) lib_set_root(root);

    char font_dir[PATH_LEN], cmd_path[PATH_LEN];
    if (fonts) snprintf(font_dir, sizeof font_dir, "%s", fonts);
    else host_path(SD_PREFIX "/App/Shelf/fonts", font_dir, sizeof font_dir);
    snprintf(cmd_path, sizeof cmd_path, "%s", cmd ? cmd : "/tmp/cmd_to_run.sh");

    // Run as Onion's gameSwitcher (boot.sh mounts Shelf there), or SHELF_SWITCHER=1|overlay.
    const char *base = strrchr(argv[0], '/');
    const char *sw = getenv("SHELF_SWITCHER");
    if (!strcmp(base ? base + 1 : argv[0], "gameSwitcher") || sw) {
        int overlay = (argc > 1 && !strcmp(argv[1], "--overlay")) || (sw && !strcmp(sw, "overlay"));
        return switcher_main(overlay, sim, font_dir, argv);
    }

    Library lib;
    if (lib_load(&lib)) return 1;
    fprintf(stderr, "shelf: %d systems, %d games, %d recent, %d with play time\n", lib.nsys, lib.ngames,
            lib.nrecent, activity_load(&lib));

    // SHELF_THEME=light|dark overrides the saved setting (screenshots).
    const char *theme = getenv("SHELF_THEME");
    if (theme) ui_set_dark(!strcmp(theme, "dark"));
    else settings_load();
    char hook[PATH_LEN];
    host_path(SD_PREFIX "/.tmp_update/startup/shelf.sh", hook, sizeof hook);
    ui_set_boot(access(hook, F_OK) == 0);

    icons_init(&lib);
    if (ui_init(&lib, font_dir)) { fprintf(stderr, "shelf: fonts missing in %s\n", font_dir); return 1; }
    Image *canvas = img_new(SCREEN_W, SCREEN_H);

    status_poll(sim);
    const char *shot = getenv("SHELF_SHOT");
    if (shot) return headless(&lib, cmd_path, canvas, getenv("SHELF_KEYS"), shot);

    // Render-only benchmark: safe to run over SSH while Onion owns the screen.
    const char *bench = getenv("SHELF_BENCH");
    if (bench) {
        int n = atoi(bench);
        if (n < 1 || n > 10000) return 1;
        const char *scene = getenv("SHELF_BENCH_SCENE");
        if (!scene) scene = "home";
        int grid = !strcmp(scene, "grid"), scroll = !strcmp(scene, "scroll");
        int transitions = !strcmp(scene, "transition");
        if (!grid && !scroll && !transitions && strcmp(scene, "home")) return 1;
        // Grid and transitions start from the first library folder (A on a recent launches).
        if (grid || transitions) ui_button(BTN_R);
        if (grid) ui_button(BTN_A);
        // Warm the same initial scene in both renderers; navigation still exercises
        // newly visible tiles and art requests inside the measured loop.
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
            if (scroll && i % 12 == 0)
                ui_button((i / 72) % 2 ? BTN_LEFT : BTN_RIGHT);
            if (grid && i % 12 == 0) {
                static const Button moves[] = {BTN_DOWN, BTN_DOWN, BTN_RIGHT, BTN_UP, BTN_UP, BTN_LEFT};
                ui_button(moves[(i / 12) % 6]);
            }
            if (transitions && i % 36 == 0)
                ui_button((i / 36) % 2 ? BTN_B : BTN_A);
            icons_pump(2);
            ui_update(1.f / 60);
            ui_draw(canvas);
            double elapsed = platform_now() - begin;
            if (elapsed > worst_frame) worst_frame = elapsed;
            if (elapsed > .015) over++; // 16.7 ms vblank less ~1.6 ms rotate into fb
        }
        fprintf(stderr, "shelf: %s benchmark %d frames, %.2f ms/frame, worst %.2f ms, %d over 15 ms\n",
                scene, n, (platform_now() - t0) * 1000 / n, worst_frame * 1000, over);
        img_free(canvas); ui_free(); icons_free(); lib_free(&lib);
        return 0;
    }

    if (platform_init()) return 1;
    if (icons_start_worker()) fprintf(stderr, "shelf: icon worker unavailable; loading art per frame\n");
    HomeLoop hl = {&lib, cmd_path, sim, 0, platform_now() + 5};
    LoopScreen screen = {&hl, home_button, home_tick, screen_update, screen_idle, screen_draw, home_after};
    LoopIO io = loop_platform();
    if (getenv("SHELF_STATS")) {
        stats.t0 = platform_now();
        screen.draw = stats_draw;
        io.present = stats_present;
    }
    icons_set_notify(platform_wake);
    loop_run(&io, &screen, canvas, &stats.loop);
    icons_set_notify(NULL);
    int status = hl.status;

    img_free(canvas);
    ui_free();
    icons_free();
    lib_free(&lib);
    platform_quit();
    return status;
}
