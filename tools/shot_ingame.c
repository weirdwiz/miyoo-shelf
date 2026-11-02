// docs/screenshots/7-ingame-menu.png: the in-game menu over a running fake-core game.
// It plays the newest recent through the real game loop on the fake platform, pauses into
// the switcher, and writes the settled menu. With `colours`, START opens the paused game's
// colour row first (9-ingame-colours.png). Usage:
//   shot-ingame <SDCARD> <fonts> <fake core .so> <out.ppm> [colours]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/game.h"
#include "../src/icons.h"
#include "../src/library.h"
#include "fake_platform.h"

static int write_ppm(const Image *img, const char *out)
{
    FILE *f = fopen(out, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", img->w, img->h);
    for (int i = 0; i < img->w * img->h; i++) {
        uint32_t p = img->px[i];
        unsigned char rgb3[3] = {p >> 16, p >> 8, p};
        fwrite(rgb3, 1, 3, f);
    }
    return fclose(f);
}

int main(int argc, char **argv)
{
    if (argc != 5 && !(argc == 6 && !strcmp(argv[5], "colours"))) {
        fprintf(stderr, "usage: %s <SDCARD> <fonts> <fake core .so> <out.ppm> [colours]\n", argv[0]);
        return 2;
    }
    setenv("SHELF_CORE", argv[3], 1);
    lib_set_root(argv[1]);
    Library lib;
    if (lib_load(&lib) || lib.nrecent < 1) {
        fprintf(stderr, "shot-ingame: no recently played games in %s\n", argv[1]);
        return 1;
    }
    icons_init(&lib);
    GameSettings set = {SCALE_SHARP, argv[2], NULL};
    int colours = argc == 6;

    fp_reset();
    fp_limit(30);
    fp_press_at(1.5, BTN_MENU);                // pause into the switcher
    if (colours) fp_press_at(2.6, BTN_START);  // the paused game's colour row, drawn at once
    fp_capture_at(2.6);                        // once its cards have all arrived
    if (colours) fp_press_at(3.2, BTN_B);
    fp_press_at(3.4, BTN_B);                   // leave again so the tool can finish
    int next = -1;
    int result = game_play(&lib, lib.recent[0], &set, &next);

    const Image *shot = fp_capture();
    int rc = shot ? write_ppm(shot, argv[4]) : -1;
    if (!shot) fprintf(stderr, "shot-ingame: the menu never came up (%d)\n", result);
    icons_free();
    lib_free(&lib);
    return rc;
}
