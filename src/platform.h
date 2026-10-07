// The only code that differs between the Mac simulator and the Miyoo Mini Plus.
#ifndef SHELF_PLATFORM_H
#define SHELF_PLATFORM_H

#include "gfx.h"

#define SCREEN_W 640
#define SCREEN_H 480

typedef enum {
    BTN_NONE, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
    BTN_A, BTN_B, BTN_X, BTN_Y, BTN_L, BTN_R,
    BTN_START, BTN_SELECT, BTN_MENU, BTN_QUIT,
} Button;

int platform_init(void);
void platform_quit(void);
// Returns the next button press (including d-pad auto-repeat), BTN_NONE when drained.
Button platform_poll(void);
void platform_present(const Image *frame);
// Copies what the screen shows now (the paused game, for the switcher) into a
// SCREEN_W x SCREEN_H image, upright. Call before platform_init. Simulator: SHELF_FRAME
// names a picture to use. Returns 0 on success.
int platform_grab(Image *out);
// Nonzero when present waits on vblank (for the previous frame's flip), so the frame
// loop needn't sleep to pace.
int platform_vsync_paced(void);
// Short UI navigation click; a no-op when no sound is available.
void platform_click(void);
double platform_now(void); // seconds, monotonic
void platform_sleep_until(double t);

#endif
