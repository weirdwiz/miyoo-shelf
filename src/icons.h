// Square blur-fill icons from Onion's Imgs/ art (whole box, aspect kept), cached on the SD card.
#ifndef SHELF_ICONS_H
#define SHELF_ICONS_H

#include <stddef.h>

#include "gfx.h"
#include "library.h"

#define ICON_BIG 168
#define ICON_SMALL 92

void icons_init(Library *lib);
void icons_free(void);
// Returns the icon if ready, NULL while pending. Requests generation for later.
// The image is borrowed: it stays valid until the next icons_trim() or icons_free(), so
// draw with it but don't keep it. Comparing a kept pointer is fine only to notice that
// this same game's icon changed (a reloaded icon may reuse an address, but it shows the
// same art). Main thread only.
const Image *icon_get(int game, int big);
// Frees the least recently requested icons while decoded icons exceed the budget. Icons
// asked for since the previous trim are kept (they are on screen), even over budget.
// Call between frames on the main thread.
void icons_trim(void);
// Memory budget for decoded icons (0 restores the default, 8 MiB), and current use.
void icons_set_budget(size_t max_bytes);
size_t icons_bytes(void);
// Generates up to `budget` pending icons on the caller's thread. Returns how many it
// made; always 0 once icons_start_worker() has taken over.
int icons_pump(int budget);
// Decode and resize requested art on a background thread so frames never wait on the
// SD card or PNG decoding. Returns 0 on success; on failure keep calling icons_pump.
int icons_start_worker(void);
// Called from the worker after each image it publishes (wakes an idle frame loop).
void icons_set_notify(void (*fn)(void));
// Changes when a requested image becomes available (invalidates UI composites).
unsigned icons_revision(void);

#endif
