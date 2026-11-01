// The only code that differs between the Mac simulator (platform_sdl2.c) and the Miyoo
// Mini / Mini Plus (platform_mm.c: fb0, evdev, MI_AO audio, sysfs; no SDL).
#ifndef SHELF_PLATFORM_H
#define SHELF_PLATFORM_H

#include <stdint.h>
#include <stdio.h>

#include "gfx.h"
#include "scale.h"

#define SCREEN_W 640
#define SCREEN_H 480

typedef enum {
    BTN_NONE, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
    BTN_A, BTN_B, BTN_X, BTN_Y, BTN_L, BTN_R, BTN_L2, BTN_R2,
    BTN_START, BTN_SELECT, BTN_MENU,
    BTN_POWER,     // POWER released after a short press
    BTN_POWER_OFF, // POWER held for 2 s (sent once, while still held)
    BTN_VOLUME,    // the volume keys changed the volume (platform_volume() has it)
    BTN_QUIT,      // simulator window closed
    BTN_REDRAW,    // the screen contents were lost; draw everything again
    BTN_COUNT
} Button;

int platform_init(void);
void platform_quit(void);

// Returns the next button press (with d-pad auto-repeat) or event, BTN_NONE when drained.
// Draining also updates platform_held().
Button platform_poll(void);
// Buttons held down right now, as bits 1u << Button (for gameplay input).
unsigned platform_held(void);

void platform_present(const Image *frame);
// Shows a game frame (XRGB8888, `pitch` in pixels) scaled with `mode`, black around it.
void platform_present_game(const uint32_t *px, int w, int h, int pitch, float aspect, ScaleMode mode);
// Nonzero when presenting waits on vblank (for the previous frame's flip), so frame
// loops needn't sleep to pace.
int platform_vsync_paced(void);

double platform_now(void); // seconds, monotonic
void platform_sleep_until(double t);
// Sleeps until a button event is pending, a held d-pad button is due to repeat,
// platform_wake() is called, or `until` (platform_now() time), whichever comes first.
// Without platform_init it just sleeps until `until`.
void platform_wait(double until);
// Ends a platform_wait early. Safe from any thread (background loaders call it).
void platform_wake(void);

// Short UI navigation click; a no-op when no sound is available.
void platform_click(void);
// Game audio: while a source is set, an audio thread pulls interleaved stereo s16 frames
// at PLATFORM_AUDIO_RATE from it (player_audio_read fits). NULL stops pulling; clicks
// still play. The source must be safe to call from that thread.
#define PLATFORM_AUDIO_RATE 48000
void platform_audio_source(int (*pull)(int16_t *out, int frames));

// Hardware settings. Levels: brightness 0-10, volume 0-20 (0 mutes).
void platform_set_brightness(int level);
void platform_set_volume(int level);
int platform_volume(void);
// Idle dimming: on = backlight at its lowest, off = the brightness set again. Waking from
// standby also ends it.
void platform_dim(int on);
// Battery percent 0-100, or -1 when unknown; *charging set to 0/1.
int platform_battery(int *charging);
// Standby: on = backlight and panel off, audio muted, CPU in powersave. off restores.
void platform_standby(int on);
// CPU policy: nonzero while a game runs (full speed), zero in menus, the paused in-game
// menu included (on demand).
void platform_performance(int game);
// Hardware probe (SHELF_PROBE, src/probe.c): writes what the platform knows about its
// display and input to `out` and logs every raw input event there until called with NULL.
void platform_probe(FILE *out);
// Syncs and powers the device off. Returns only if that failed (simulator: quits).
void platform_power_off(void);

#endif
