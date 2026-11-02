// Home row (Recent, then Library folders), the folder grid, Options and the in-game
// switcher view.
#ifndef SHELF_UI_H
#define SHELF_UI_H

#include "gfx.h"
#include "library.h"
#include "platform.h"
#include "settings.h"

// The UI is loaded once and then entered and left, so coming home or opening the in-game
// menu doesn't read the fonts again or lose the glyphs, background and sprites drawn so far.
//   ui_init / ui_init_switcher  load what stays (fonts, frame buffers, background) unless
//                               already loaded, then enter the home screen / the switcher
//   ui_enter_home / _switcher   enter a view of the loaded UI: this visit's state starts
//                               blank (selection, springs, toast, options), the folders are
//                               rebuilt, the settings, theme and battery carry over
//   ui_leave                    a visit ends: drops what it borrowed (the switcher's hooks
//                               and screenshots); everything loaded stays
//   ui_free                     the process is done with the UI: frees it all
// The enter functions need a loaded UI (ui_loaded) and fail with -1 otherwise. A failing
// init or enter frees the UI (and so needs a new ui_init); nothing is left half built.
int ui_init(Library *lib, const char *font_dir);
int ui_loaded(void);
int ui_enter_home(void);
void ui_leave(void);
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
    UI_SETTINGS,   // brightness, volume, screen, theme or colours changed: apply and save
    UI_POWER_OFF,  // power off was confirmed in Options
    UI_SW_RESUME,  // back to the paused game
    UI_SW_PLAY,    // start ui_switch_target()
    UI_SW_HOME,    // back to Shelf
};
int ui_take_action(void);
void ui_set_dark(int dark); // may be called before ui_init; a no-op when the theme is unchanged
int ui_dark(void);
// The Options panel's values. Always call ui_set_settings after ui_init: the panel edits
// its copy and asks for UI_SETTINGS, so main stays the one that applies and saves them.
void ui_set_settings(const Settings *s);
Settings ui_settings(void);
// Status bar: battery percent (-1 hides it) and whether it is charging.
void ui_set_status(int battery, int charging);
// A one-line message over the home screen or the in-game menu ("Couldn't start …"),
// shown for a few seconds.
void ui_set_toast(const char *text);

// The in-game menu: the paused game and the recents as screenshot cards. With `running`,
// recents[0] is the paused game and the view opens by shrinking it from full screen.
// Actions: UI_SW_RESUME (back to the paused game), UI_SW_PLAY (start ui_switch_target()),
// UI_SW_HOME (to Shelf). Each is taken once its animation ends.
int ui_init_switcher(Library *lib, const char *font_dir, int from_game, int running);
int ui_enter_switcher(int from_game, int running);
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
// The paused game's colour row, which START opens under its card: a Game Boy game's
// Palette (one of `npalettes`, cycled with LEFT/RIGHT/A, saved as Settings.palette) or a
// Game Boy Color or Advance game's Colours switch (Settings.colours). A change asks for
// UI_SETTINGS like the Options panel. Call after entering the switcher; UI_COLOURS_NONE
// (the default) offers neither.
enum { UI_COLOURS_NONE, UI_COLOURS_SWITCH, UI_COLOURS_PALETTE };
typedef struct {
    const char *name;
    uint32_t shade[4]; // 0xRRGGBB, lightest first
} UiPalette;
void ui_switch_colours(int row, const UiPalette *palettes, int npalettes);
int ui_switch_colours_open(void); // the row is up: it owns the buttons until B closes it
// Benchmark: play the opening zoom again, from full screen into the row.
void ui_switch_reopen(void);

#endif
