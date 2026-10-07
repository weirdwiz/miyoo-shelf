// Square icons framing Onion's Imgs/ art (aspect preserved), cached on the SD card.
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
// Generates up to `budget` pending icons on the caller's thread. Returns how many it
// made; always 0 once icons_start_worker() has taken over.
int icons_pump(int budget);
// Decode and resize requested art on a background thread so frames never wait on the
// SD card or PNG decoding. Returns 0 on success; on failure keep calling icons_pump.
int icons_start_worker(void);
// Changes when a requested image becomes available (invalidates UI composites).
unsigned icons_revision(void);

#endif
