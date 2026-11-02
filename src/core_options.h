// The core options Shelf sets itself, so games look right without a settings screen.
//
// Three layers, weakest first: Shelf's defaults replace the cores' own; a RetroArch-style
// .shelf/data/<core>.opt file overrides any of them; and the player's two choices in the
// in-game menu (Colours, Palette) override the few keys they control, whatever the file
// says (see PlayerConfig).
#ifndef SHELF_CORE_OPTIONS_H
#define SHELF_CORE_OPTIONS_H

#include <stdint.h>

#include "player.h"

// Shelf's defaults for a core ("gambatte", "mgba"), *n of them; NULL (and *n = 0) for a
// core Shelf sets nothing for.
const PlayerOption *core_defaults(const char *core, int *n);

// The palettes the in-game menu offers Game Boy games, in the order it cycles them.
enum { PALETTE_POCKET, PALETTE_GREEN, PALETTE_TEAL, PALETTE_MONO, PALETTE_COUNT };
#define PALETTE_DEFAULT PALETTE_POCKET

typedef struct {
    const char *id;     // in settings.json: "pocket"
    const char *name;   // in the menu: "Pocket"
    uint32_t shade[4];  // 0xRRGGBB as the core draws them, lightest first
} CorePalette;

// NULL outside 0..PALETTE_COUNT-1.
const CorePalette *core_palette(int i);

// The screen a game is drawn for, which decides the colour choice it gets: a Game Boy
// game a palette, a Game Boy Color or Advance game colour correction.
typedef enum { LCD_NONE, LCD_GB, LCD_GBC, LCD_GBA } CoreLcd;
// From the core and, for gambatte, the cartridge header's colour flag (a GBC cart in the
// GB folder is still a GBC game). A ROM that can't be read counts as a Game Boy game.
CoreLcd core_lcd(const char *core, const char *rom_path);

// The options the choices set for a core: colour correction on or off, and the Game Boy
// palette (an index above). Writes up to `max` into `out`; returns how many.
#define CORE_CHOICES_MAX 4
int core_choices(const char *core, int colours, int palette, PlayerOption *out, int max);

#endif
