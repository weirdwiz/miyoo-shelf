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
// Game index ready to launch once the launch animation finishes, else -1. Clears the request.
int ui_take_launch(void);
// Nonzero once per button press that moved the selection or changed view (UI click sound).
int ui_take_sound(void);
// Call after the library's recents changed: rebuilds the home row with the first item selected.
void ui_reset_home(void);

// Requests from the Options panel (START) and MENU, taken once each.
enum { UI_NONE, UI_EXIT, UI_THEME, UI_BOOT_ONION, UI_BOOT_SHELF };
int ui_take_action(void);
void ui_set_dark(int dark);   // may be called before ui_init
int ui_dark(void);
// Whether Shelf is Onion's home screen; picks the Options row that switches it.
void ui_set_boot(int on);
// Status bar: battery percent (-1 hides it), charging, Wi-Fi bars 1..3 (0 = on but not
// connected, -1 = off, hidden).
void ui_set_status(int battery, int charging, int wifi);

#endif
