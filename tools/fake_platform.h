// A fake platform for host tests and headless screenshots: a clock that only moves when
// Shelf sleeps or waits, scripted button presses, and counters for what the platform was
// asked to do. See fake_platform.c.
#ifndef SHELF_FAKE_PLATFORM_H
#define SHELF_FAKE_PLATFORM_H

#include "../src/platform.h"

typedef struct {
    int presents;        // platform_present calls (UI frames)
    int game_presents;   // platform_present_game calls (game frames)
    int asleep_presents; // frames presented while platform_standby(1) was in effect
    int standby_on;      // platform_standby(1) calls
    int standby_off;     // platform_standby(0) calls
    int power_off;       // platform_power_off calls
    int performance_on;  // platform_performance(1) calls
    int audio_on;        // platform_audio_source(pull) calls
    int audio_off;       // platform_audio_source(NULL) calls
    int performance_off; // platform_performance(0) calls
    int battery_reads;   // platform_battery calls
    int dims;            // platform_dim(1) calls
    int dimmed;          // dimmed now (standby ends it, as on the device)
    int waits;           // platform_wait calls
    int watched_waits;   // of those, the ones begun inside fp_watch_waits' window
    int clicks;
    int brightness, volume;
} FakePlatform;

void fp_reset(void); // forget the clock, the script and the counters
const FakePlatform *fp(void);
// The calls that change the device's state, in order, space separated: audio_on,
// audio_off, standby_on, standby_off, perf_on, perf_off, dim_on, dim_off, power_off.
const char *fp_log(void);
// Calls `hook` with each logged call as it happens (NULL: none); fp_reset clears it.
void fp_on_call(void (*hook)(const char *call));
// Queues a press the fake delivers once, when the clock reaches `at`; the test scripts
// presses in time order (fp_press_at sorts them).
void fp_press_at(double at, Button b);
// From clock time `at` on the battery reads `percent` (-1: unknown) and `charging`;
// until the first such change it is 76 %, not charging.
void fp_battery_at(double at, int percent, int charging);
// Buttons platform_held() reports.
void fp_hold(unsigned mask);
// Counts the platform_wait calls begun at clock times in [from, to) as watched_waits.
void fp_watch_waits(double from, double to);
// Fails (exit 1) instead of letting a stuck loop run past `at`.
void fp_limit(double at);
// The next presented canvas from clock time `at` on, as a copy for screenshots.
void fp_capture_at(double at);
const Image *fp_capture(void);
// The last game frame presented, and the first one after fp_reset() (borrowed copies).
const Image *fp_frame(void);
const Image *fp_first_frame(void);

#endif
