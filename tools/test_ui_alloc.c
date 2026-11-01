// Every allocation in the UI's startup, first frames, folder view and shutdown fails in
// turn: ui_init/ui_init_switcher either succeed or fail cleanly, nothing crashes on a
// missing cache, and ui_free (or a failed init) leaves nothing allocated.
// Usage: test-ui-alloc <fixture SD card> <fonts>
#include <stdio.h>

#include "../src/icons.h"
#include "../src/ui.h"
#include "failalloc.h"

static Library lib;
static const char *fonts;
// A fixed clock: a minute turning mid-run would redraw the status and change the count.
static double fixed_wall(void) { return 1767225605.0; }

static int run(int switcher, long fail_at, Image *canvas)
{
    long before = fa_live();
    fa_fail_from(fail_at);
    icons_init(&lib);
    if (!(switcher ? ui_init_switcher(&lib, fonts, 0, 0) : ui_init(&lib, fonts))) {
        static const Button KEYS[] = {BTN_R, BTN_A, BTN_DOWN, BTN_Y, BTN_B, BTN_START, BTN_B};
        for (int k = 0; k < (int)(sizeof KEYS / sizeof *KEYS); k++) {
            ui_button(KEYS[k]);
            for (int f = 0; f < 3; f++) { ui_update(1.f / 60); ui_draw(canvas); while (icons_pump(64)) {} }
        }
        ui_free();
    }
    int failed = fa_failed();
    fa_fail_from(-1);
    icons_free();
    long leaked = fa_live() - before;
    if (leaked) {
        fprintf(stderr, "FAIL %s: %ld allocations leaked when allocation %ld failed\n",
                switcher ? "switcher" : "home", leaked, fail_at);
        return -1;
    }
    return failed;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s <SDCARD> <fonts>\n", argv[0]); return 2; }
    lib_set_root(argv[1]);
    fonts = argv[2];
    ui_set_wall_clock(fixed_wall);
    Image *canvas = img_new(640, 480);
    if (lib_load(&lib) || !canvas) return 1;
    for (int switcher = 0; switcher < 2; switcher++) {
        long n = 0, total;
        int rc;
        if (run(switcher, -1, canvas)) return 1; // warms the SD card's icon cache
        total = fa_total();
        if (run(switcher, -1, canvas)) return 1;
        total = fa_total() - total; // allocations in one clean run
        while ((rc = run(switcher, n, canvas)) == 1) n++;
        if (rc < 0) return 1;
        if (n != total) { // each allocation of a clean run was failed once
            fprintf(stderr, "FAIL: a clean run makes %ld allocations; %ld were failed\n", total, n);
            return 1;
        }
        printf("UI allocation failures: %s survived each of its %ld allocations failing\n",
               switcher ? "switcher" : "home", n);
    }
    img_free(canvas);
    lib_free(&lib);
    return 0;
}
