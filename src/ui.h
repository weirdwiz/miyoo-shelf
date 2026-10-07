// Home row (Recent, then Library folders) and the folder grid.
#ifndef SHELF_UI_H
#define SHELF_UI_H

#include "gfx.h"
#include "library.h"
#include "platform.h"

int ui_init(Library *lib, const char *font_dir);
void ui_free(void);
void ui_button(Button b);
void ui_update(float dt);
void ui_draw(Image *canvas);
// Seconds until the screen changes by itself (the status clock's next minute); 0 when
// ui_draw would now draw something new: after input, while anything animates, and when
// art, screenshots, status or the clock changed. Until then frames needn't be drawn.
// The cursor pulses for a few seconds after each button, then rests.
double ui_idle(void);
// Tests: wall-clock seconds for the status clock (NULL restores the real clock).
void ui_set_wall_clock(double (*now)(void));
// Game index ready to launch once the launch animation finishes, else -1. Clears the request.
int ui_take_launch(void);
// Nonzero once per button press that moved the selection or changed view (UI click sound).
int ui_take_sound(void);
// Call after the library's recents changed: rebuilds the home row with the first item selected.
void ui_reset_home(void);

// Requests from the Options panel (START) and MENU, taken once each.
enum { UI_NONE, UI_EXIT, UI_THEME, UI_BOOT_ONION, UI_BOOT_SHELF,
       UI_SW_RESUME, UI_SW_PLAY, UI_SW_HOME };
int ui_take_action(void);
void ui_set_dark(int dark);   // may be called before ui_init
int ui_dark(void);
// Whether Shelf is Onion's home screen; picks the Options row that switches it.
void ui_set_boot(int on);
// Status bar: battery percent (-1 hides it), charging, Wi-Fi bars 1..3 (0 = on but not
// connected, -1 = off, hidden).
void ui_set_status(int battery, int charging, int wifi);

// Game switcher view (see switcher.h): recents as screenshot cards. With `overlay` and
// `running`, recents[0] is the paused game and the view opens by shrinking it from full
// screen. Actions: UI_SW_RESUME (back to the paused game), UI_SW_PLAY (start
// ui_switch_target()), UI_SW_HOME (to Shelf). Each is taken once its animation ends.
int ui_init_switcher(Library *lib, const char *font_dir, int overlay, int running);
// Where screenshots come from: a card-size and a full-size image per recent (NULL while
// loading or missing), the selection to load first, and a counter that changes with them.
void ui_switch_hooks(const Image *(*card)(int), const Image *(*full)(int), void (*select)(int),
                     unsigned (*revision)(void));
enum { SW_TOAST_NONE, SW_TOAST_SAVING, SW_TOAST_SAVED, SW_TOAST_FAILED };
void ui_switch_saving(int toast);
int ui_switch_target(void);
// Benchmark: play the opening zoom again, from full screen into the row.
void ui_switch_reopen(void);

#endif
