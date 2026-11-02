// Shelf's saved preferences: the palette, the panel's backlight level, the volume, how
// game screens are scaled and how their colours look. Stored on the card as
// .shelf/data/settings.json.
#ifndef SHELF_SETTINGS_H
#define SHELF_SETTINGS_H

#include "core_options.h"
#include "scale.h"

typedef struct {
    int dark;       // 1: dark palette
    int brightness; // 0..10
    int volume;     // 0..20 (0 mutes)
    ScaleMode scale;
    int colours;    // 1: GBC and GBA games colour corrected for their consoles' screens
    int palette;    // Game Boy games' palette, PALETTE_* (core_options.h)
} Settings;

#define BRIGHTNESS_MAX 10
#define VOLUME_MAX 20

// dark off, brightness 7, volume 12, sharp pixels, colours corrected, the Pocket palette.
void settings_defaults(Settings *s);
// Reads settings.json. A missing or unreadable file, and any value that is missing or out
// of range, keeps the default. Returns 0 when the file was read.
int settings_load(Settings *s);
// Writes settings.json atomically. Returns 0, or -1 when it couldn't be saved.
int settings_save(const Settings *s);

#endif
