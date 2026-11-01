// Home row (Recent, then Library folders), the folder grid, Options and the in-game
// switcher view.
#ifndef SHELF_UI_H
#define SHELF_UI_H

#include "gfx.h"
#include "library.h"
#include "platform.h"
#include "settings.h"

int ui_init(Library *lib, const char *font_dir);
void ui_free(void);
void ui_button(Button b);
void ui_update(float dt);
void ui_draw(Image *canvas);
// Seconds until the screen changes by itself (the status clock's next minute, or a
// toast's fade-out); 0 when ui_draw would now draw something new: after input, while
// anything animates, and when art, screenshots, status or the clock changed. Until then
// frames needn't be drawn.
double ui_idle(void);
// Tests: wall-clock seconds for the status clock (NULL restores the real clock).
void ui_set_wall_clock(double (*now)(void));
// Game index ready to launch once the launch animation finishes, else -1. Clears the request.
int ui_take_launch(void);
// Nonzero once per button press that moved the selection or changed view (UI click sound).
int ui_take_sound(void);
// Call after the library's recents changed: rebuilds the home row with the first item selected.
void ui_reset_home(void);

// Requests from the Options panel and the switcher view, taken once each.
enum {
    UI_NONE,
    UI_SETTINGS,   // brightness, volume, screen or theme changed: apply and save
    UI_POWER_OFF,  // power off was confirmed in Options
    UI_SW_RESUME,  // back to the paused game
    UI_SW_PLAY,    // start ui_switch_target()
    UI_SW_HOME,    // back to Shelf
};
int ui_take_action(void);
void ui_set_dark(int dark); // may be called before ui_init
int ui_dark(void);
// The Options panel's values. Always call ui_set_settings after ui_init: the panel edits
// its copy and asks for UI_SETTINGS, so main stays the one that applies and saves them.
void ui_set_settings(const Settings *s);
Settings ui_settings(void);
// Status bar: battery percent (-1 hides it) and whether it is charging.
void ui_set_status(int battery, int charging);
// A one-line message over the home screen ("Couldn't start …"), shown for a few seconds.
void ui_set_toast(const char *text);

// The in-game menu: the paused game and the recents as screenshot cards. With `running`,
// recents[0] is the paused game and the view opens by shrinking it from full screen.
// Actions: UI_SW_RESUME (back to the paused game), UI_SW_PLAY (start ui_switch_target()),
// UI_SW_HOME (to Shelf). Each is taken once its animation ends.
int ui_init_switcher(Library *lib, const char *font_dir, int from_game, int running);
// Where screenshots come from: a card-size and a full-size image per recent (NULL while
// loading or missing), the selection to load first, and a counter that changes with them.
// ui_switch_card_size() is the size cards must be.
void ui_switch_hooks(const Image *(*card)(int), const Image *(*full)(int), void (*select)(int),
                     unsigned (*revision)(void));
void ui_switch_card_size(int *w, int *h);
// A question over the switcher ("Restart the game?"): the footer then answers A yes, B no.
// NULL clears it.
void ui_switch_notice(const char *text);
int ui_switch_target(void);
// Benchmark: play the opening zoom again, from full screen into the row.
void ui_switch_reopen(void);

#endif
