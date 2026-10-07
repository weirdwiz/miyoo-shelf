// Shelf: a DSi/3DS-style launcher that stands in for Onion's MainUI.
// It shows Recent games and Library folders, and when you pick a game it writes
// /tmp/cmd_to_run.sh and exits. Onion's runtime then runs the game and starts us again.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "icons.h"
#include "library.h"
#include "platform.h"
#include "ui.h"

// SHELF_SHOT=out.ppm SHELF_KEYS="RRA..." renders without a window: each key is pressed
// and given 0.6s of animation, then the final frame is written. Keys: U D L R A B Y l r.
static int headless(Library *lib, const char *cmd_path, Image *canvas, const char *keys, const char *out)
{
    static const char MAP[] = "UDLRABYlr";
    static const Button BTN[] = {BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT, BTN_A, BTN_B, BTN_Y, BTN_L, BTN_R};
    for (const char *k = keys ? keys : ""; ; k++) {
        const char *m = *k ? strchr(MAP, *k) : NULL;
        if (m) ui_button(BTN[m - MAP]);
        for (int f = 0; f < 36; f++) {
            ui_draw(canvas); // requests the icons on screen
            while (icons_pump(64)) {}
            ui_update(1.f / 60);
            int g = ui_take_launch();
            if (g >= 0) { lib_launch(lib, g, cmd_path); ui_reset_home(); }
        }
        if (!*k) break;
    }
    ui_draw(canvas);
    FILE *f = fopen(out, "wb");
    if (!f) return 1;
    fprintf(f, "P6\n%d %d\n255\n", canvas->w, canvas->h);
    for (int i = 0; i < canvas->w * canvas->h; i++) {
        uint32_t p = canvas->px[i];
        unsigned char rgb3[3] = {p >> 16, p >> 8, p};
        fwrite(rgb3, 1, 3, f);
    }
    fclose(f);
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    const char *root = getenv("SHELF_ROOT");      // simulator: folder standing in for /mnt/SDCARD
    const char *fonts = getenv("SHELF_FONTS");    // defaults to the installed app folder
    const char *cmd = getenv("SHELF_CMD");        // where to write the launch command
    int sim = root != NULL;
    if (root) lib_set_root(root);

    char font_dir[PATH_LEN], cmd_path[PATH_LEN];
    if (fonts) snprintf(font_dir, sizeof font_dir, "%s", fonts);
    else host_path(SD_PREFIX "/App/Shelf/fonts", font_dir, sizeof font_dir);
    snprintf(cmd_path, sizeof cmd_path, "%s", cmd ? cmd : "/tmp/cmd_to_run.sh");

    Library lib;
    if (lib_load(&lib)) return 1;
    fprintf(stderr, "shelf: %d systems, %d games, %d recent\n", lib.nsys, lib.ngames, lib.nrecent);

    icons_init(&lib);
    if (ui_init(&lib, font_dir)) { fprintf(stderr, "shelf: fonts missing in %s\n", font_dir); return 1; }
    Image *canvas = img_new(SCREEN_W, SCREEN_H);

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
    const double frame = 1.0 / 60;
    double next = platform_now(), last_update = next;
    int running = 1;
    int stats = getenv("SHELF_STATS") != NULL;
    int paced = platform_vsync_paced();
    double stat_t0 = platform_now(), work = 0, worst = 0;
    double draw_work = 0, present_work = 0;
    int frames = 0;
    while (running) {
        Button b;
        while ((b = platform_poll()) != BTN_NONE) {
            if (b == BTN_QUIT || b == BTN_MENU) running = 0; // MENU returns to Onion
            else {
                ui_button(b);
                if (ui_take_sound()) platform_click();
            }
        }
        double t0 = platform_now();
        icons_pump(2);
        double update_now = platform_now();
        float dt = (float)(update_now - last_update);
        last_update = update_now;
        if (dt > .1f) dt = .1f; // bound catch-up after a long stall
        ui_update(dt);
        ui_draw(canvas);
        double drawn = platform_now();
        platform_present(canvas);
        if (stats) {
            draw_work += drawn - t0;
            present_work += platform_now() - drawn;
            double w = platform_now() - t0;
            work += w;
            if (w > worst) worst = w;
            if (++frames == 120) {
                double el = platform_now() - stat_t0;
                fprintf(stderr, "shelf: %.1f fps, frame work avg %.1f ms, worst %.1f ms\n",
                        frames / el, work / frames * 1000, worst * 1000);
                fprintf(stderr, "shelf: render %.2f ms, present %.2f ms\n",
                        draw_work * 1000 / frames, present_work * 1000 / frames);
                frames = 0; work = worst = draw_work = present_work = 0; stat_t0 = platform_now();
            }
        }

        int g = ui_take_launch();
        if (g >= 0) {
            int launched = lib_launch(&lib, g, cmd_path) == 0;
            ui_reset_home();
            if (launched) {
                fprintf(stderr, "shelf: launch -> %s\n", cmd_path);
                if (!sim) running = 0; // on the device Onion takes over from here
            } else {
                fprintf(stderr, "shelf: launch failed; staying in Shelf\n");
            }
        }
        // Present already waits on vblank; sleeping as well would push most frames
        // past the next one.
        if (paced) continue;
        next += frame;
        double now = platform_now();
        if (next < now - 0.1) next = now; // fell behind; don't spiral
        platform_sleep_until(next);
    }

    img_free(canvas);
    ui_free();
    icons_free();
    lib_free(&lib);
    platform_quit();
    return 0;
}
