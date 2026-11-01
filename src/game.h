// One play session: the core running one game, its saves, and the in-game menu. Shelf
// hands game_play() a game and gets back where the player went.
#ifndef SHELF_GAME_H
#define SHELF_GAME_H

#include "library.h"
#include "scale.h"

enum {
    GAME_FAILED = -1,   // nothing was loaded; game_error() says why
    GAME_HOME = 0,      // the player went back to Shelf's home screen
    GAME_SWITCH = 1,    // another game was picked in the in-game menu: *switch_to names it
    GAME_POWER_OFF = 2, // everything was saved and Shelf powered off
};

// What a session needs from Shelf's settings. Brightness and volume live on the
// platform, which main sets when they change.
typedef struct {
    ScaleMode scale;      // how game frames fill the screen
    const char *font_dir; // fonts for the in-game menu
} GameSettings;

// Plays `game` (an index into lib->games) until the player leaves it. Saves the state,
// SRAM and a screenshot on every way out, including switching, and records the play
// time. On GAME_SWITCH *switch_to (when not NULL) is the index of the game picked in the
// menu. GAME_FAILED leaves nothing loaded and nothing written.
int game_play(Library *lib, int game, const GameSettings *set, int *switch_to);

// Why the last session failed to start, or what went wrong saving it ("" when nothing
// did). The home screen shows it as a toast.
const char *game_error(void);

#endif
