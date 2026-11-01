// Every allocation in the UI's startup, first frames, folder view and shutdown fails in
// turn: ui_init/ui_init_switcher either succeed or fail cleanly, nothing crashes on a
// missing cache, and ui_free (or a failed init) leaves nothing allocated. A third run
// does the same over a whole life of the resident UI: load, visit the switcher, leave,
// come home again, with every one of the enter functions failing in turn.
// Usage: test-ui-alloc <fixture SD card> <fonts>
#include <stdio.h>

#include "../src/icons.h"
#include "../src/ui.h"
#include "failalloc.h"

static Library lib;
static const char *fonts;
// A fixed clock: a minute turning mid-run would redraw the status and change the count.
static double fixed_wall(void) { return 1767225605.0; }

static const Button KEYS[] = {BTN_R, BTN_A, BTN_DOWN, BTN_Y, BTN_B, BTN_START, BTN_B};
static void use(Image *canvas)
{
    for (int k = 0; k < (int)(sizeof KEYS / sizeof *KEYS); k++) {
        ui_button(KEYS[k]);
        for (int f = 0; f < 3; f++) { ui_update(1.f / 60); ui_draw(canvas); while (icons_pump(64)) {} }
    }
}

static const Image *no_card(int i) { return NULL; }
static void no_select(int i) {}
static unsigned no_rev(void) { return 0; }

// A failing init or enter must have freed the UI; nothing may be left to use.
static int freed(void) { return !ui_loaded(); }

// The resident UI's life: loaded for the home screen, then the switcher by each way in,
// home again by each way in, then freed. Returns 0 when it completed, 1 when a step failed
// (cleanly: the UI was freed by the failing step).
static int life(Image *canvas)
{
    if (ui_init(&lib, fonts)) return freed() ? 1 : -1;
    use(canvas);
    ui_switch_hooks(no_card, no_card, no_select, no_rev);
    if (ui_enter_switcher(1, 1)) return freed() ? 1 : -1;
    use(canvas);
    ui_leave();
    if (ui_enter_home()) return freed() ? 1 : -1;
    use(canvas);
    ui_leave();
    ui_switch_hooks(no_card, no_card, no_select, no_rev);
    if (ui_init_switcher(&lib, fonts, 0, 0)) return freed() ? 1 : -1; // already loaded
    use(canvas);
    ui_leave();
    if (ui_init(&lib, fonts)) return freed() ? 1 : -1;                  // already loaded
    use(canvas);
    return 0;
}

static const char *const NAME[] = {"home", "switcher", "resident life"};

static int run(int mode, long fail_at, Image *canvas)
{
    long before = fa_live();
    fa_fail_from(fail_at);
    icons_init(&lib);
    int rc = 0;
    if (mode == 2) {
        rc = life(canvas);
        if (rc < 0) { fprintf(stderr, "FAIL %s: a failed step left the UI loaded\n", NAME[mode]); return -1; }
    } else if (!(mode ? ui_init_switcher(&lib, fonts, 0, 0) : ui_init(&lib, fonts))) {
        use(canvas);
    }
    ui_free(); // safe after a failure freed everything already
    int failed = fa_failed();
    fa_fail_from(-1);
    icons_free();
    long leaked = fa_live() - before;
    if (leaked) {
        fprintf(stderr, "FAIL %s: %ld allocations leaked when allocation %ld failed\n", NAME[mode], leaked, fail_at);
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
    for (int mode = 0; mode < 3; mode++) {
        long n = 0, total;
        int rc;
        if (run(mode, -1, canvas)) return 1; // warms the SD card's icon cache
        total = fa_total();
        if (run(mode, -1, canvas)) return 1;
        total = fa_total() - total; // allocations in one clean run
        while ((rc = run(mode, n, canvas)) == 1) n++;
        if (rc < 0) return 1;
        if (n != total) { // each allocation of a clean run was failed once
            fprintf(stderr, "FAIL: a clean run makes %ld allocations; %ld were failed\n", total, n);
            return 1;
        }
        printf("UI allocation failures: %s survived each of its %ld allocations failing\n", NAME[mode], n);
    }
    img_free(canvas);
    lib_free(&lib);
    return 0;
}
