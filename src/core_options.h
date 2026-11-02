// The core options Shelf sets itself, so games look right without a settings screen.
//
// Shelf's defaults replace the cores' own; a RetroArch-style .shelf/data/<core>.opt file
// still overrides any of them (see PlayerConfig).
#ifndef SHELF_CORE_OPTIONS_H
#define SHELF_CORE_OPTIONS_H

#include "player.h"

// Shelf's defaults for a core ("gambatte", "mgba"), *n of them; NULL (and *n = 0) for a
// core Shelf sets nothing for.
const PlayerOption *core_defaults(const char *core, int *n);

#endif
