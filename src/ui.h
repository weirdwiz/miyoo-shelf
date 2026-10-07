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
// Call after the library's recents changed: rebuilds the home row with the first item selected.
void ui_reset_home(void);

#endif
