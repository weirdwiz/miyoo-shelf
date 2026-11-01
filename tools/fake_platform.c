// The platform.h implementation the host tests and the screenshot tool run on: no window,
// no hardware. Time only moves when Shelf sleeps, so a session plays out deterministically
// and scripted presses land on exact frame numbers.
#include "fake_platform.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FP_MAX_EVENTS 32
#define FP_LOG 4096

typedef struct {
    double at;
    Button button;
} Event;

typedef struct {
    double at;
    int percent, charging;
} Battery;

static struct {
    double clock;
    Event events[FP_MAX_EVENTS];
    int nevents, next;
    unsigned held;
    double limit;
    double watch_from, watch_to;
    double vsync_hz, flip_done; // the panel, and when the pending pan finishes
    double cost_min, cost_max;  // per game frame, charged in platform_held
    unsigned rng;
    Battery battery[FP_MAX_EVENTS];
    int nbattery;
    int asleep;
    FakePlatform counts;
    Image *frame, *first; // the last and the first game frame presented
    Image *captured;      // the canvas asked for with fp_capture_at
    double capture_at;    // < 0: not capturing
    char log[FP_LOG];     // see fp_log
    size_t loglen;
    void (*hook)(const char *call);
} F;

// Records a call that changes the device's state, in order, and tells the test's hook.
static void note(const char *call)
{
    size_t n = strlen(call);
    if (F.loglen + n + 2 < FP_LOG) {
        if (F.loglen) F.log[F.loglen++] = ' ';
        memcpy(F.log + F.loglen, call, n + 1);
        F.loglen += n;
    }
    if (F.hook) F.hook(call);
}

void fp_reset(void)
{
    img_free(F.frame);
    img_free(F.first);
    img_free(F.captured);
    memset(&F, 0, sizeof F);
    F.limit = 600;
    F.capture_at = -1;
}

const FakePlatform *fp(void) { return &F.counts; }
const char *fp_log(void) { return F.log; }
void fp_on_call(void (*hook)(const char *call)) { F.hook = hook; }

void fp_press_at(double at, Button b)
{
    if (F.nevents == FP_MAX_EVENTS) {
        fprintf(stderr, "fake platform: the press script is full\n");
        exit(2);
    }
    int at_i = F.nevents;
    while (at_i > 0 && F.events[at_i - 1].at > at) { // keep the script in time order
        F.events[at_i] = F.events[at_i - 1];
        at_i--;
    }
    F.events[at_i].at = at;
    F.events[at_i].button = b;
    F.nevents++;
}

void fp_battery_at(double at, int percent, int charging)
{
    if (F.nbattery == FP_MAX_EVENTS) {
        fprintf(stderr, "fake platform: the battery script is full\n");
        exit(2);
    }
    int i = F.nbattery++;
    while (i > 0 && F.battery[i - 1].at > at) { // in time order, like the presses
        F.battery[i] = F.battery[i - 1];
        i--;
    }
    F.battery[i] = (Battery){at, percent, charging};
}

void fp_hold(unsigned mask) { F.held = mask; }
void fp_vsync(double hz) { F.vsync_hz = hz > 0 ? hz : 0; F.flip_done = 0; }
void fp_frame_cost(double min, double max) { F.cost_min = min; F.cost_max = max > min ? max : min; F.rng = 12345; }
void fp_limit(double at) { F.limit = at; }
void fp_watch_waits(double from, double to) { F.watch_from = from; F.watch_to = to; }
void fp_capture_at(double at) { F.capture_at = at; }
const Image *fp_capture(void) { return F.captured; }
const Image *fp_frame(void) { return F.frame; }
const Image *fp_first_frame(void) { return F.first; }

// A session that never returns would hang the test: fail loudly instead.
static void fp_check_limit(void)
{
    if (F.clock < F.limit) return;
    fprintf(stderr, "fake platform: still running at %.1f s\n", F.clock);
    exit(1);
}

int platform_init(void) { return 0; }
void platform_quit(void) {}

double platform_now(void) { return F.clock; }

void platform_sleep_until(double t)
{
    if (t > F.clock) F.clock = t;
    fp_check_limit();
}

void platform_wait(double until)
{
    F.counts.waits++;
    if (F.clock >= F.watch_from && F.clock < F.watch_to) F.counts.watched_waits++;
    double t = until;
    if (F.next < F.nevents && F.events[F.next].at < t) t = F.events[F.next].at;
    if (F.limit < t) t = F.limit;
    if (t > F.clock) F.clock = t;
    fp_check_limit();
}

void platform_wake(void) {}

Button platform_poll(void)
{
    if (F.next < F.nevents && F.events[F.next].at <= F.clock) return F.events[F.next++].button;
    return BTN_NONE;
}

unsigned platform_held(void)
{
    if (F.cost_max > 0) {
        F.rng = F.rng * 1103515245u + 12345u; // the same jitter every run
        double r = (double)((F.rng >> 8) & 0xffff) / 65535.0;
        F.clock += F.cost_min + (F.cost_max - F.cost_min) * r;
        fp_check_limit();
    }
    return F.held;
}

// The device's flipper: a frame waits for the previous pan, then pans at the next vblank.
static void flip(void)
{
    if (!F.vsync_hz) return;
    if (F.clock < F.flip_done) F.clock = F.flip_done;
    F.flip_done = (floor(F.clock * F.vsync_hz + 1e-6) + 1) / F.vsync_hz; // strictly after now
    fp_check_limit();
}

static void copy(Image **dst, const uint32_t *px, int w, int h, int pitch)
{
    if (w <= 0 || h <= 0) return;
    if (!*dst || (*dst)->w != w || (*dst)->h != h) {
        img_free(*dst);
        *dst = img_new(w, h);
        if (!*dst) return;
    }
    for (int y = 0; y < h; y++)
        memcpy((*dst)->px + (size_t)y * w, px + (size_t)y * pitch, (size_t)w * sizeof *px);
}

void platform_present(const Image *frame)
{
    flip();
    F.counts.presents++;
    if (F.asleep) F.counts.asleep_presents++;
    if (F.capture_at >= 0 && !F.captured && F.clock >= F.capture_at)
        copy(&F.captured, frame->px, frame->w, frame->h, frame->w);
}

void platform_present_game(const uint32_t *px, int w, int h, int pitch, float aspect, ScaleMode mode)
{
    (void)aspect;
    (void)mode;
    flip();
    F.counts.game_presents++;
    if (F.asleep) F.counts.asleep_presents++;
    copy(&F.frame, px, w, h, pitch);
    if (!F.first) copy(&F.first, px, w, h, pitch);
}

int platform_vsync_paced(void) { return F.vsync_hz > 0; }

void platform_click(void) { F.counts.clicks++; }

void platform_audio_source(int (*pull)(int16_t *out, int frames))
{
    if (pull) F.counts.audio_on++;
    else F.counts.audio_off++;
    note(pull ? "audio_on" : "audio_off");
}

void platform_set_brightness(int level) { F.counts.brightness = level; }
void platform_set_volume(int level) { F.counts.volume = level; }
int platform_volume(void) { return F.counts.volume; }

int platform_battery(int *charging)
{
    int percent = 76, plugged = 0;
    for (int i = 0; i < F.nbattery && F.battery[i].at <= F.clock; i++) {
        percent = F.battery[i].percent;
        plugged = F.battery[i].charging;
    }
    F.counts.battery_reads++;
    if (charging) *charging = plugged;
    return percent;
}

void platform_dim(int on)
{
    if (on) F.counts.dims++;
    F.counts.dimmed = on != 0;
    note(on ? "dim_on" : "dim_off");
}

void platform_standby(int on)
{
    if ((on != 0) == F.asleep) return; // as on the device: already in that state
    F.asleep = on != 0;
    if (!F.asleep) F.counts.dimmed = 0;
    if (F.asleep) F.counts.standby_on++;
    else F.counts.standby_off++;
    note(F.asleep ? "standby_on" : "standby_off");
}

void platform_performance(int game)
{
    if (game) F.counts.performance_on++;
    else F.counts.performance_off++;
    note(game ? "perf_on" : "perf_off");
}

void platform_probe(FILE *out) { (void)out; }

void platform_power_off(void)
{
    F.counts.power_off++;
    note("power_off");
}
