// Reads an Onion SD card: Emu/*/config.json, Roms/<SYS>/, Imgs/, recentlist.json, favourite.json.
// Paths in Onion files always start with /mnt/SDCARD; on the simulator that prefix maps to
// SHELF_ROOT so the same files work on both.
#ifndef SHELF_LIBRARY_H
#define SHELF_LIBRARY_H

#include <stdint.h>

#define SD_PREFIX "/mnt/SDCARD"
#define MAX_SYSTEMS 64
#define MAX_RECENT 24
#define PATH_LEN 512

typedef struct {
    char dir[64];          // Emu folder name, e.g. "GBA", "SFC"
    char label[48];        // shown on folders, e.g. "SNES"
    char name[64];         // long name, e.g. "Super Nintendo"
    uint32_t color;        // folder/badge colour
    char emu_dir[PATH_LEN]; // /mnt/SDCARD/Emu/GBA
    char launch[PATH_LEN];  // /mnt/SDCARD/Emu/GBA/launch.sh
    char rom_dir[PATH_LEN]; // /mnt/SDCARD/Roms/GBA (normalised)
    char img_dir[PATH_LEN]; // /mnt/SDCARD/Roms/GBA/Imgs
    int ngames;
} System;

typedef struct {
    int sys;
    char title[128];       // display title, tags stripped
    char stem[256];        // file name without extension (matches Imgs/<stem>.png)
    char path[PATH_LEN];   // normalised device path of the ROM
    int fav;
    int recent;            // rank in recents, -1 if not recent
} Game;

typedef struct {
    System sys[MAX_SYSTEMS];
    int nsys;
    Game *games;
    int ngames;
    int recent[MAX_RECENT]; // game indices, most recent first
    int nrecent;
} Library;

void lib_set_root(const char *root);          // host path that stands in for /mnt/SDCARD
void host_path(const char *dev, char *out, int n); // /mnt/SDCARD/x -> <root>/x
void path_normalise(char *p);                 // collapse "/./" and "/x/../"

int lib_load(Library *lib);
void lib_free(Library *lib);
int lib_img_path(const Library *lib, const Game *g, char *out, int n); // host path, 0 if exists

// Writes /tmp/cmd_to_run.sh (or `cmd_path`) and moves the game to the top of recentlist.json.
int lib_launch(Library *lib, int game, const char *cmd_path);

#endif
