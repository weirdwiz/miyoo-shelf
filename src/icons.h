// Square "blur fill" icons built from Onion's Imgs/ art, cached on the SD card.
#ifndef SHELF_ICONS_H
#define SHELF_ICONS_H

#include "gfx.h"
#include "library.h"

#define ICON_BIG 168
#define ICON_SMALL 92

void icons_init(Library *lib);
void icons_free(void);
// Returns the icon if ready, NULL while pending. Requests generation for later.
const Image *icon_get(int game, int big);
// Generates up to `budget` pending icons (call once per frame). Returns how many it made.
int icons_pump(int budget);

#endif
