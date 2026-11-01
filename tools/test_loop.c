// The frame loop on a fake clock, driving the real UI: settled screens must stop
// presenting, and input, art, screenshots, status and the clock must bring frames back.
// Usage: test-loop <fixture SD card> <fonts dir>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "../src/icons.h"
#include "../src/loop.h"
#include "../src/ui.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

// Fake platform: a clock that only moves when the loop waits, scripted presses, and a
// presenter that records when frames were shown.
#define WALL0 1767225605.0 // 00:00:05 UTC: the status clock's minute turns at t = 55
static double clock_now;
static struct { double at; Button b; } script[16];
static int nscript, next_press;
static int presents;
static double last_present, first_present_after;
static double stop_at;

static double fake_now(void) { return clock_now; }
static double fake_wall(void) { return WALL0 + clock_now; }

static Button fake_poll(void)
{
    if (next_press < nscript && script[next_press].at <= clock_now) return script[next_press++].b;
    return BTN_NONE;
}

static void fake_wait(double until)
{
    double t = until;
    if (next_press < nscript && script[next_press].at < t) t = script[next_press].at;
    if (stop_at < t) t = stop_at;
    if (t > clock_now) clock_now = t;
}

static void fake_present(const Image *frame)
{
    (void)frame;
    presents++;
    last_present = clock_now;
    if (first_present_after < 0) first_present_after = clock_now;
    clock_now += 1.0 / 60; // paced: present waits for vblank
}

// Screen glue, as main.c does it, with hooks for scripted outside changes.
static unsigned card_rev;
static double card_at = INFINITY, status_at = INFINITY;
static unsigned card_revision(void) { return card_rev; }
static const Image *no_image(int i) { (void)i; return NULL; }
static void no_select(int i) { (void)i; }
static Image *shot; // every recent's screenshot card once cards "load"
static const Image *fake_card(int i) { (void)i; return card_rev ? shot : NULL; }

static void s_button(void *ud, Button b) { ui_button(b); ui_take_sound(); }
static double s_tick(void *ud, double now)
{
    icons_pump(2);
    if (now >= card_at) { card_rev++; card_at = INFINITY; }
    if (now >= status_at) { ui_set_status(50, 1); status_at = INFINITY; }
    double next = card_at < status_at ? card_at : status_at;
    return next < now + 5 ? next : now + 5;
}
static void s_update(void *ud, float dt) { ui_update(dt); }
static double s_idle(void *ud) { return ui_idle(); }
static void s_draw(void *ud, Image *c) { ui_draw(c); }
static int s_after(void *ud)
{
    ui_take_action();
    int g = ui_take_launch();
    if (g >= 0) ui_reset_home();
    return clock_now >= stop_at;
}

static const LoopIO IO = {fake_now, fake_poll, fake_wait, fake_present, 1};
static const LoopScreen SCREEN = {NULL, s_button, s_tick, s_update, s_idle, s_draw, s_after};

// Runs the loop until `until`; returns frames presented.
static int run_until(Image *canvas, double until, LoopStats *st)
{
    presents = 0;
    first_present_after = -1;
    stop_at = until;
    loop_run(&IO, &SCREEN, canvas, st);
    return presents;
}

static void press(double at, Button b)
{
    script[nscript].at = at;
    script[nscript++].b = b;
}

int main(int argc, char **argv)
{
    if (argc != 3) { fprintf(stderr, "usage: %s <SDCARD> <fonts>\n", argv[0]); return 2; }
    lib_set_root(argv[1]);
    Library lib;
    CHECK(lib_load(&lib) == 0);
    icons_init(&lib);
    ui_set_wall_clock(fake_wall);
    CHECK(ui_init(&lib, argv[2]) == 0);
    Image *canvas = img_new(SCREEN_W, SCREEN_H);
    LoopStats st = {0};

    // Start-up: the first frame plus the art it asks for, pumped in at the loop's tick.
    // With the cursor pulse gone that is a handful of frames, not 60 fps, and then the
    // home screen stops presenting until the clock's minute turns.
    int n = run_until(canvas, 11, &st);
    CHECK(n >= 1 && n < 66);
    n = run_until(canvas, 54.9, &st);
    CHECK(n == 0);
    st.waits = 0;
    n = run_until(canvas, 56, &st);
    CHECK(n == 1 && fabs(last_present - 55) < .05);
    CHECK(st.waits < 5); // sleeps, doesn't spin

    // Input wakes at once; the retuned springs settle a 184 px hop in ~0.7 s (the tail
    // down to the spring's rest threshold), then the screen is idle again.
    press(60, BTN_RIGHT);
    n = run_until(canvas, 70, &st);
    CHECK(first_present_after >= 60 && first_present_after < 60.01);
    CHECK(n > 20 && last_present < 60.8);

    // Status changes draw one frame; unchanged polls none.
    status_at = 75;
    n = run_until(canvas, 80, &st);
    CHECK(n == 1 && first_present_after >= 75 && first_present_after < 75.01);

    // Options: the sheet hides the cursor, so once drawn nothing animates.
    press(90, BTN_START);
    n = run_until(canvas, 100, &st);
    CHECK(n >= 1 && n <= 2 && last_present < 90.1);
    press(100, BTN_B);
    run_until(canvas, 110, &st);

    // Folder view and back: springs and the zoom animate, then stop.
    press(110, BTN_R);
    press(111, BTN_A);
    n = run_until(canvas, 119, &st);
    CHECK(n > 30 && last_present < 119);
    press(119, BTN_B);
    run_until(canvas, 128, &st);
    n = run_until(canvas, 170, &st);
    CHECK(n == 0); // the clock's minutes turn at 115 and 175

    // A toast fades in and out but is otherwise static: the middle of its stay presents
    // nothing, and the whole thing costs roughly the fade frames, not 60 fps x TOAST_FOR.
    ui_set_toast("Couldn't start Gamma");
    n = run_until(canvas, 171, &st);
    CHECK(n > 3 && n < 20); // the ~0.12 s fade-in
    n = run_until(canvas, 173.3, &st);
    CHECK(n == 0); // the static middle: ui_idle sleeps through it
    n = run_until(canvas, 174.7, &st);
    CHECK(n > 10 && n < 45); // the ~0.45 s fade-out and the frame that clears it
    ui_free();
    icons_free();

    // Switcher (the in-game menu to come): a screenshot arriving later redraws; a settled
    // switcher stops.
    icons_init(&lib);
    ui_switch_hooks(no_image, no_image, no_select, card_revision);
    CHECK(ui_init_switcher(&lib, argv[2], 0, 0) == 0);
    double t0 = clock_now;
    n = run_until(canvas, t0 + 10, &st);
    CHECK(n >= 1 && n <= 3 && last_present < t0 + 8); // one frame, then the clock's minute
    card_at = t0 + 12;
    n = run_until(canvas, t0 + 14, &st);
    CHECK(n == 1 && first_present_after >= t0 + 12 && first_present_after < t0 + 12.01);
    press(t0 + 20, BTN_RIGHT);
    n = run_until(canvas, t0 + 30, &st);
    CHECK(n > 30 && first_present_after < t0 + 20.01 && last_present < t0 + 28);
    ui_free();

    // Screenshots arriving while settled: the cards redraw with them, the nearby sprites
    // are built between frames (no presents), and the loop goes back to sleep.
    shot = img_new(248, 186);
    CHECK(shot != NULL);
    card_rev = 0;
    ui_switch_hooks(fake_card, no_image, no_select, card_revision);
    CHECK(ui_init_switcher(&lib, argv[2], 0, 0) == 0);
    t0 = clock_now;
    run_until(canvas, t0 + 10, &st);
    card_at = t0 + 12;
    n = run_until(canvas, t0 + 14, &st);
    CHECK(n > 0 && n < 30 && first_present_after >= t0 + 12 && first_present_after < t0 + 12.01);
    CHECK(last_present < t0 + 12.5);
    st.waits = 0;
    n = run_until(canvas, t0 + 40, &st);
    CHECK(n <= 1 && st.waits < 10); // at most the clock's minute; sleeping, not polling
    ui_free();
    img_free(shot);
    icons_free();
    lib_free(&lib);
    img_free(canvas);
    if (failures) return 1;
    puts("frame loop: idle screens stop presenting; input, art, cards, status and clock wake them");
    return 0;
}
