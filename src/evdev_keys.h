// Linux input-event key codes used by Onion/Miyoo. Kept independent of linux/input.h
// so the overlay's raw-input mapping can be regression-tested on the simulator host.
#ifndef SHELF_EVDEV_KEYS_H
#define SHELF_EVDEV_KEYS_H
#include "platform.h"
static inline Button evdev_button(int code)
{
    switch (code) {
    case 103: return BTN_UP;     // KEY_UP
    case 108: return BTN_DOWN;   // KEY_DOWN
    case 105: return BTN_LEFT;   // KEY_LEFT
    case 106: return BTN_RIGHT;  // KEY_RIGHT
    case 57:  return BTN_A;      // KEY_SPACE
    case 29:  return BTN_B;      // KEY_LEFTCTRL
    case 42:  return BTN_X;      // KEY_LEFTSHIFT
    case 56:  return BTN_Y;      // KEY_LEFTALT
    case 18:  return BTN_L;      // KEY_E
    case 20:  return BTN_R;      // KEY_T
    case 28:  return BTN_START;  // KEY_ENTER
    case 97:  return BTN_SELECT; // KEY_RIGHTCTRL
    case 1:   return BTN_MENU;   // KEY_ESC
    default:  return BTN_NONE;   // Power/sleep is handled by keymon.
    }
}
#endif
