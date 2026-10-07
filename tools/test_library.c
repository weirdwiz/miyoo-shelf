// Regression fixture mirrors the card: 8 ROMs, a root port shortcut,
// and three ports stored in category subfolders. No ROM data is needed.
#include "../src/library.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[PATH_LEN];
static const char *dirs[] = {
    "Emu", "Emu/GBA", "Emu/GBC", "Emu/SFC", "Emu/PORTS", "Emu/GB",
    "Roms", "Roms/GBA", "Roms/GBC", "Roms/SFC", "Roms/GB", "Roms/PORTS",
    "Roms/PORTS/Shortcuts", "Roms/PORTS/Shortcuts/Action",
    "Roms/PORTS/Shortcuts/Puzzle games", "Roms/PORTS/Imgs",
    "Roms/PORTS/Shortcuts/.hidden", "Roms/PORTS/Shortcuts/Imgs"
};
static const char *files[] = {
    "Emu/GBA/config.json", "Emu/GBC/config.json", "Emu/SFC/config.json",
    "Emu/PORTS/config.json", "Emu/GB/config.json",
    "Roms/GBA/Apotris.gba", "Roms/GBA/Zelda.gba", "Roms/GBA/Pokemon.gba",
    "Roms/GBA/Castlevania.gba", "Roms/GBA/Street Fighter.GBA",
    "Roms/GBC/Wario.gbc", "Roms/SFC/Chrono.sfc", "Roms/SFC/Donkey Kong.sfc",
    "Roms/PORTS/Shortcuts/~Import ports.miyoocmd",
    "Roms/PORTS/Shortcuts/Action/Pong (Gong).port",
    "Roms/PORTS/Shortcuts/Action/Bomberman (mr.boom).port",
    "Roms/PORTS/Shortcuts/Puzzle games/2048.port",
    "Roms/PORTS/Shortcuts/.hidden/Hidden.port",
    "Roms/PORTS/Shortcuts/Imgs/Artwork.port",
    "Roms/PORTS/Shortcuts/Action/cache.db", "Roms/GB/GB_cache6.db",
    "Roms/PORTS/Imgs/2048.png", "Roms/recentlist.json", "Roms/recentlist-hidden.json",
    "Roms/favourite.json"
};

static void host(const char *rel, char *out)
{
    assert(snprintf(out, PATH_LEN, "%s/%s", root, rel) < PATH_LEN);
}

static void put(const char *rel, const char *contents)
{
    char path[PATH_LEN];
    host(rel, path);
    FILE *f = fopen(path, "w");
    assert(f);
    assert(fputs(contents, f) >= 0);
    assert(fclose(f) == 0);
}

static void config(const char *sys, const char *rom, const char *img, const char *ext)
{
    char rel[128], json[1024];
    snprintf(rel, sizeof rel, "Emu/%s/config.json", sys);
    snprintf(json, sizeof json,
        "{\"launch\":\"launch.sh\",\"rompath\":\"../../Roms/%s\","
        "\"imgpath\":\"../../Roms/%s\",\"extlist\":\"%s\"}", rom, img, ext);
    put(rel, json);
}

static const Game *game(const Library *lib, const char *rel)
{
    char path[PATH_LEN];
    snprintf(path, sizeof path, SD_PREFIX "/%s", rel);
    for (int i = 0; i < lib->ngames; i++)
        if (!strcmp(lib->games[i].path, path)) return &lib->games[i];
    return NULL;
}

int main(void)
{
    strcpy(root, "/tmp/shelf-library-XXXXXX");
    assert(mkdtemp(root));
    char path[PATH_LEN];
    for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++) {
        host(dirs[i], path);
        assert(mkdir(path, 0700) == 0);
    }
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) put(files[i], "");
    config("GBA", "GBA", "GBA/Imgs", "gba|zip|7z");
    config("GBC", "GBC", "GBC/Imgs", "gbc|zip|7z");
    config("SFC", "SFC", "SFC/Imgs", "sfc|smc|zip|7z");
    config("GB", "GB", "GB/Imgs", "gb|zip|7z");
    config("PORTS", "PORTS/Shortcuts", "PORTS/Imgs", "port|miyoocmd");
    put("Roms/recentlist.json", "{\"rompath\":\"/mnt/SDCARD/Emu/PORTS/../../Roms/PORTS/Shortcuts/Puzzle games/2048.port\"}\n");
    put("Roms/recentlist-hidden.json",
        "{\"rompath\":\"/mnt/SDCARD/Emu/GBA/../../Roms/GBA/Apotris.gba\"}\n"
        "{\"label\":\"Shelf\",\"launch\":\"/mnt/SDCARD/App/Shelf/launch.sh\",\"type\":3}\n"
        "{\"rompath\":\"/mnt/SDCARD/Emu/PORTS/../../Roms/PORTS/Shortcuts/Puzzle games/2048.port\"}\n");
    put("Roms/favourite.json", "{\"rompath\":\"/mnt/SDCARD/Roms/PORTS/Shortcuts/Puzzle games/2048.port\"}\n");
    // A directory symlink back to the root must not cause recursion or duplicates.
    host("Roms/PORTS/Shortcuts/Action/loop", path);
    assert(symlink("..", path) == 0);

    lib_set_root(root);
    Library lib;
    assert(lib_load(&lib) == 0);
    fprintf(stderr, "library discovery: %d systems, %d games (expected 4 / 12)\n", lib.nsys, lib.ngames);
    int ok = lib.nsys == 4 && lib.ngames == 12;
    const Game *g = game(&lib, "Roms/PORTS/Shortcuts/Puzzle games/2048.port");
    ok = ok && g && g->fav && g->recent == 0 && lib.nrecent == 2;
    // Hidden recents follow the visible ones; apps and duplicates are skipped.
    const Game *hidden = game(&lib, "Roms/GBA/Apotris.gba");
    ok = ok && hidden && hidden->recent == 1;
    if (g) ok = ok && lib_img_path(&lib, g, path, sizeof path) == 0;
    ok = ok && game(&lib, "Roms/PORTS/Shortcuts/Action/Pong (Gong).port");
    ok = ok && game(&lib, "Roms/PORTS/Shortcuts/Action/Bomberman (mr.boom).port");
    lib_free(&lib);

    host("Roms/PORTS/Shortcuts/Action/loop", path);
    assert(unlink(path) == 0);
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        host(files[i], path);
        assert(unlink(path) == 0);
    }
    for (size_t i = sizeof dirs / sizeof *dirs; i > 0; i--) {
        host(dirs[i - 1], path);
        assert(rmdir(path) == 0);
    }
    assert(rmdir(root) == 0);
    if (!ok) { fprintf(stderr, "FAIL: nested ports, metadata, or artwork missing\n"); return 1; }
    puts("library discovery tests passed");
    return 0;
}
