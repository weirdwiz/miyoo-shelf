// The platform.h implementation the host tests and the screenshot tool run on: no window,
// no hardware. Time only moves when Shelf sleeps, so a session plays out deterministically
// and scripted presses land on exact frame numbers.
#include "fake_platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FP_MAX_EVENTS 32
#define FP_LOG 4096

typedef struct {
    double at;
    Button button;
} Event;

static struct {
    double clock;
    Event events[FP_MAX_EVENTS];
    int nevents, next;
    unsigned held;
    double limit;
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

void fp_hold(unsigned mask) { F.held = mask; }
void fp_limit(double at) { F.limit = at; }
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

unsigned platform_held(void) { return F.held; }

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
    F.counts.presents++;
    if (F.asleep) F.counts.asleep_presents++;
    if (F.capture_at >= 0 && !F.captured && F.clock >= F.capture_at)
        copy(&F.captured, frame->px, frame->w, frame->h, frame->w);
}

void platform_present_game(const uint32_t *px, int w, int h, int pitch, float aspect, ScaleMode mode)
{
    (void)aspect;
    (void)mode;
    F.counts.game_presents++;
    if (F.asleep) F.counts.asleep_presents++;
    copy(&F.frame, px, w, h, pitch);
    if (!F.first) copy(&F.first, px, w, h, pitch);
}

int platform_vsync_paced(void) { return 0; } // the clock moves in platform_sleep_until

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
    if (charging) *charging = 0;
    return 76;
}

void platform_standby(int on)
{
    F.asleep = on != 0;
    if (F.asleep) F.counts.standby_on++;
    else F.counts.standby_off++;
    note(F.asleep ? "standby_on" : "standby_off");
}

void platform_performance(int game)
{
    if (game) F.counts.performance_on++;
    note(game ? "perf_on" : "perf_off");
}

void platform_power_off(void)
{
    F.counts.power_off++;
    note("power_off");
}
