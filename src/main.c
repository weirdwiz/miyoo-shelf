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

    if (platform_init()) return 1;
    const double frame = 1.0 / 60;
    double next = platform_now();
    int running = 1;
    while (running) {
        Button b;
        while ((b = platform_poll()) != BTN_NONE) {
            if (b == BTN_QUIT) running = 0;
            else ui_button(b);
        }
        icons_pump(2);
        ui_update((float)frame);
        ui_draw(canvas);
        platform_present(canvas);

        int g = ui_take_launch();
        if (g >= 0) {
            lib_launch(&lib, g, cmd_path);
            ui_reset_home();
            fprintf(stderr, "shelf: launch -> %s\n", cmd_path);
            if (!sim) running = 0; // on the device Onion takes over from here
        }
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
