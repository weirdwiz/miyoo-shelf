// Host benchmark for how long the home screen and the in-game menu take to come up:
// UI init, then the first frame, then the frames until it settles, over many
// home -> menu -> home cycles with art already decoded (as on the second visit to a screen).
//   bench-ui <SDCARD> <fonts> [cycles]
// Prints medians in milliseconds. Host numbers are not device numbers (a 1.2 GHz Cortex-A7
// is several times slower), but what a cycle redoes is the same, so before/after ratios hold.
//
// Modes: "cold" tears the UI down and brings it up again at every visit, as Shelf used to;
// "resident" (when ui.h has ui_enter_home) keeps fonts, glyphs, background and tiles.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/icons.h"
#include "../src/ui.h"

static Library lib;
static const char *fonts;
static Image *canvas, *card, *full;

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000. + ts.tv_nsec / 1e6;
}

// Linked with -Wl,--wrap=fopen: counts the font files Shelf reads from the card.
FILE *__real_fopen(const char *path, const char *mode);
static long font_opens;
FILE *__wrap_fopen(const char *path, const char *mode)
{
    if (strstr(path, ".ttf")) font_opens++;
    return __real_fopen(path, mode);
}

static double fixed_wall(void) { return 1767225605.0; }

// Screenshots for the switcher: a flat gradient per card size, like a live game frame.
static const Image *hook_card(int i) { return card; }
static const Image *hook_full(int i) { return full; }
static void hook_select(int i) {}
static unsigned hook_rev(void) { return 1; }

static void settle(int frames)
{
    for (int f = 0; f < frames; f++) {
        while (icons_pump(64)) {}
        ui_update(1.f / 60);
        ui_draw(canvas);
    }
}

typedef struct { double init, first, settled; } Visit;

// One visit to a screen: bring it up, time the first frame, run 60 frames of settling.
static Visit visit(int menu, int resident, int first_of_process)
{
    Visit v;
    double t = now();
    int rc;
#ifdef BENCH_RESIDENT
    if (resident && !first_of_process) rc = menu ? ui_enter_switcher(1, 1) : ui_enter_home();
    else
#endif
    {
        ui_set_dark(0);
        rc = menu ? ui_init_switcher(&lib, fonts, 1, 1) : ui_init(&lib, fonts);
    }
    if (rc) { fprintf(stderr, "bench-ui: init failed\n"); exit(1); }
    v.init = now() - t;
    t = now();
    ui_draw(canvas);
    v.first = now() - t;
    t = now();
    settle(60);
    v.settled = now() - t;
    return v;
}

static void leave(int menu, int resident)
{
#ifdef BENCH_RESIDENT
    if (resident) { ui_leave(); return; }
#endif
    ui_free();
}

static int cmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static double median(double *v, int n) { qsort(v, n, sizeof *v, cmp); return v[n / 2]; }

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: %s <SDCARD> <fonts> [cycles]\n", argv[0]); return 2; }
    lib_set_root(argv[1]);
    fonts = argv[2];
    int cycles = argc > 3 ? atoi(argv[3]) : 40;
    ui_set_wall_clock(fixed_wall);
    if (lib_load(&lib)) return 1;
    icons_init(&lib);
    canvas = img_new(SCREEN_W, SCREEN_H);
    int cw, ch;
    ui_switch_card_size(&cw, &ch);
    card = img_new(cw, ch);
    full = img_new(SCREEN_W, SCREEN_H);
    for (int i = 0; i < cw * ch; i++) card->px[i] = 0xff000000u | (i * 7);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) full->px[i] = 0xff000000u | (i * 5);
    ui_switch_hooks(hook_card, hook_full, hook_select, hook_rev);

    // Warm the OS file cache and decode the art once; every mode then sees the same icons.
    visit(0, 0, 1);
    ui_free();
    ui_init(&lib, fonts);
    for (int i = 0; i < 4; i++) { ui_button(BTN_R); settle(30); }
    ui_free();

    int modes = 1;
#ifdef BENCH_RESIDENT
    modes = 2;
#endif
    for (int resident = 0; resident < modes; resident++) {
        long opens0 = font_opens;
        double hi[cycles], hf[cycles], hs[cycles], mi[cycles], mf[cycles], ms[cycles];
        for (int c = 0; c < cycles; c++) {
            Visit h = visit(0, resident, c == 0);
            hi[c] = h.init; hf[c] = h.first; hs[c] = h.settled;
            ui_button(BTN_R); settle(30); // the user moves around a bit
            leave(0, resident);
            ui_switch_hooks(hook_card, hook_full, hook_select, hook_rev);
            Visit m = visit(1, resident, c == 0);
            mi[c] = m.init; mf[c] = m.first; ms[c] = m.settled;
            leave(1, resident);
        }
        printf("%-8s font files read per home+menu cycle: %.1f\n", resident ? "resident" : "cold",
               (double)(font_opens - opens0) / cycles);
        printf("%-8s home: init %6.2f ms, first frame %6.2f ms, 60 frames %7.2f ms | "
               "menu: init %6.2f ms, first frame %6.2f ms, 60 frames %7.2f ms\n",
               resident ? "resident" : "cold",
               median(hi, cycles), median(hf, cycles), median(hs, cycles),
               median(mi, cycles), median(mf, cycles), median(ms, cycles));
    }
    ui_free();
    icons_free();
    lib_free(&lib);
    return 0;
}
