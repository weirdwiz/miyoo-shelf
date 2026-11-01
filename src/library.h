// Shelf's view of the SD card: the Game Boy, Game Boy Color and Game Boy Advance ROM
// folders, plus Shelf's own recents, favorites and play-time files.
// Paths in those files always start with /mnt/SDCARD; on the simulator that prefix maps to
// SHELF_ROOT (see lib_set_root) so the same files work on both.
#ifndef SHELF_LIBRARY_H
#define SHELF_LIBRARY_H

#include <stdint.h>

#define SD_PREFIX "/mnt/SDCARD"
#define DATA_DIR SD_PREFIX "/.shelf/data" // recents, favorites, play time and settings
#define MAX_SYSTEMS 3                      // GB, GBC, GBA: the systems Shelf runs
#define MAX_RECENT 24
#define PATH_LEN 512

typedef struct {
    char dir[64];           // ROM folder name, e.g. "GBA"
    char label[48];         // shown on folders, e.g. "GBA"
    char name[64];          // long name, e.g. "Game Boy Advance"
    char core[16];          // libretro core that runs it, e.g. "mgba"
    uint32_t color;         // folder/badge colour
    char rom_dir[PATH_LEN]; // /mnt/SDCARD/Roms/GBA (the first folder that matched)
    char img_dir[PATH_LEN]; // /mnt/SDCARD/Roms/GBA/Imgs
    int ngames;
} System;

typedef struct {
    int sys;
    char title[128];       // display title, tags stripped
    char stem[256];        // file name without extension (matches Imgs/<stem>.png)
    char path[PATH_LEN];   // device path of the ROM
    int fav;
    int recent;            // rank in recents, -1 if not recent
    int play_time;         // seconds, from playtime.json
    int play_count;        // sessions
    long long last_played; // unix time the last session started, 0 if unknown
} Game;

typedef struct {
    System sys[MAX_SYSTEMS];
    int nsys;
    Game *games;            // sorted by title
    int ngames;
    int recent[MAX_RECENT]; // game indices, most recent first
    int nrecent;
} Library;

void lib_set_root(const char *root);              // host path that stands in for /mnt/SDCARD
void host_path(const char *dev, char *out, int n); // /mnt/SDCARD/x -> <root>/x
void path_normalise(char *p);                     // collapse "/./" and "/x/../"

// Scans Roms/GB, Roms/GBC and Roms/GBA (also MinUI-style "Game Boy Advance (GBA)" names,
// case-insensitively) and reads Shelf's data files. Missing or corrupt data files read as
// empty; a card without a Roms folder is an error (-1).
int lib_load(Library *lib);
void lib_free(Library *lib);
int lib_img_path(const Library *lib, const Game *g, char *out, int n); // host path, 0 if exists

// All three save: the file is written first (temp file, fsync, rename) and the library
// fields only change once it is on disk. -1 means nothing changed and the old file is
// intact; the caller may show an error and carry on.
//
// Most recent first, at most MAX_RECENT entries.
int lib_mark_played(Library *lib, int game);
int lib_set_fav(Library *lib, int game, int fav);
// Adds one session's play time and sets last_played to `now` (unix time).
int lib_add_play(Library *lib, int game, int seconds, long long now);

// Where the player keeps this game's <stem>.srm/.state/.png: /mnt/SDCARD/Saves/<SYS>.
void lib_save_dir(const Library *lib, int game, char *out, int n);

// The game to resume on the next boot: .shelf/data/resume names its ROM. Shelf sets it
// when it powers off from a game, after the save. lib_take_resume deletes the marker
// (and syncs) before it answers, so a game that can't start doesn't boot again: the game's
// index, or -1 for none, an unknown game, or a marker that couldn't be deleted.
int lib_set_resume(const Library *lib, int game);
int lib_take_resume(const Library *lib);

// Writes `json` to a device path atomically (creating its folder); 0 or -1. Shelf keeps
// its own files this way, settings.json included.
int lib_write_json(const char *dev_path, const char *json);

#endif
