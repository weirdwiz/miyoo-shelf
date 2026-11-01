// SHELF_PROBE: a one-off report for the first boot on a device Shelf hasn't run on yet. The
// boot script collects what the system says about itself (docs/hardware.md); this adds what
// Shelf sees: the panel's geometry and real frame rate, the raw key codes, the battery.
// The test pattern shows orientation and tearing: red is top left, green top right, blue
// bottom left, yellow bottom right, a bar sweeps across, and the squares in the middle count
// frames in binary for a photo to time against.
#include <stdio.h>

#include "gfx.h"
#include "platform.h"
#include "probe.h"

#define PRESENT_FRAMES 120
#define BUTTON_SECONDS 15.0

static const char *const BUTTON_NAMES[BTN_COUNT] = {
    "none", "up", "down", "left", "right", "a", "b", "x", "y", "l1", "r1", "l2", "r2",
    "start", "select", "menu", "power", "power-off", "volume", "quit", "redraw",
};

static void pattern(Image *c, int n)
{
    const int s = 40;
    gfx_clear(c, rgb(0x202020));
    gfx_fill_rect(c, (n * 8) % SCREEN_W, 0, 8, SCREEN_H, rgb(0x808080));
    gfx_fill_rect(c, 0, 0, SCREEN_W, 1, rgb(0xffffff));
    gfx_fill_rect(c, 0, SCREEN_H - 1, SCREEN_W, 1, rgb(0xffffff));
    gfx_fill_rect(c, 0, 0, 1, SCREEN_H, rgb(0xffffff));
    gfx_fill_rect(c, SCREEN_W - 1, 0, 1, SCREEN_H, rgb(0xffffff));
    gfx_fill_rect(c, 1, 1, s, s, rgb(0xff0000));
    gfx_fill_rect(c, SCREEN_W - 1 - s, 1, s, s, rgb(0x00ff00));
    gfx_fill_rect(c, 1, SCREEN_H - 1 - s, s, s, rgb(0x0000ff));
    gfx_fill_rect(c, SCREEN_W - 1 - s, SCREEN_H - 1 - s, s, s, rgb(0xffff00));
    for (int bit = 0; bit < 12; bit++) // most significant on the left
        gfx_fill_rect(c, SCREEN_W / 2 + (5 - bit) * 28 + 2, SCREEN_H / 2 - 12, 24, 24,
                      n >> bit & 1 ? rgb(0xffffff) : rgb(0x404040));
}

static void battery_line(FILE *out, double t0)
{
    int charging = 0, pct = platform_battery(&charging);
    fprintf(out, "%8.3f battery %d%% charging %d\n", platform_now() - t0, pct, charging);
}

int probe_run(const char *path)
{
    FILE *out = fopen(path, "a");
    if (!out) return -1;
    setvbuf(out, NULL, _IOLBF, 0); // a crash part way through still leaves the lines so far
    Image *c = img_new(SCREEN_W, SCREEN_H);
    if (!c) {
        fclose(out);
        return -1;
    }
    double t0 = platform_now();
    fprintf(out, "\n== shelf probe\n");
    platform_probe(out);
    battery_line(out, t0);

    // How fast frames really go out: with vsync this is the panel's refresh rate.
    double prev = platform_now(), lo = 1e9, hi = 0, sum = 0;
    for (int i = 0; i <= PRESENT_FRAMES; i++) {
        pattern(c, i);
        platform_present(c);
        double now = platform_now(), d = now - prev;
        prev = now;
        if (!i) continue;
        sum += d;
        if (d < lo) lo = d;
        if (d > hi) hi = d;
    }
    double avg = sum / PRESENT_FRAMES;
    fprintf(out, "present: %d frames, ms min %.2f avg %.3f max %.2f, %.3f Hz, vsync paced %d\n",
            PRESENT_FRAMES, lo * 1e3, avg * 1e3, hi * 1e3, avg > 0 ? 1 / avg : 0, platform_vsync_paced());

    fprintf(out, "buttons: press every button once within %.0f s (raw events, then decoded)\n", BUTTON_SECONDS);
    double end = platform_now() + BUTTON_SECONDS, next_battery = platform_now() + 5;
    int quit = 0;
    for (int i = PRESENT_FRAMES + 1; !quit && platform_now() < end; i++) {
        pattern(c, i);
        platform_present(c);
        if (!platform_vsync_paced()) platform_sleep_until(platform_now() + 1 / 60.0);
        for (Button b; (b = platform_poll()) != BTN_NONE;) {
            fprintf(out, "%8.3f button %s\n", platform_now() - t0, b < BTN_COUNT ? BUTTON_NAMES[b] : "?");
            if (b == BTN_QUIT) quit = 1;
        }
        if (platform_now() >= next_battery) {
            battery_line(out, t0);
            next_battery += 5;
        }
    }
    platform_probe(NULL);
    fprintf(out, "== end\n");
    fclose(out);
    img_free(c);
    return 0;
}
