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
// Short UI navigation click; a no-op when no sound is available.
void platform_click(void);
double platform_now(void); // seconds, monotonic
void platform_sleep_until(double t);

#endif
