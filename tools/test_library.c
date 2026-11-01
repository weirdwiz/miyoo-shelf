// The SD card as Shelf reads it: which files count as games, and the recents, favorites
// and play-time files Shelf keeps. Every case builds a throwaway card in /tmp.
#include "../src/library.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[PATH_LEN];
static int failures;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void host(const char *rel, char *out, int n)
{
    if (snprintf(out, n, "%s/%s", root, rel) >= n) out[0] = 0;
}

// mkdir -p for a host path.
static void mkdirs(const char *path)
{
    char buf[PATH_LEN];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *c = buf + 1;; c++) {
        if (*c && *c != '/') continue;
        char keep = *c;
        *c = 0;
        mkdir(buf, 0700);
        *c = keep;
        if (!keep) return;
    }
}

// Writes a card-relative file, creating its folders.
static void put(const char *rel, const char *contents)
{
    char path[PATH_LEN], dir[PATH_LEN];
    host(rel, path, sizeof path);
    snprintf(dir, sizeof dir, "%s", path);
    *strrchr(dir, '/') = 0;
    mkdirs(dir);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (!f) return;
    fwrite(contents, 1, strlen(contents), f);
    fclose(f);
}

static void rom(const char *rel) { put(rel, ""); } // placeholder ROM

static void mkcard(void)
{
    snprintf(root, sizeof root, "/tmp/shelf-library-XXXXXX");
    if (!mkdtemp(root)) root[0] = 0;
}

static void rmcard(void)
{
    char cmd[PATH_LEN + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
    if (system(cmd)) fprintf(stderr, "could not remove %s\n", root);
}

// The game at a card-relative path, or NULL.
static const Game *game(const Library *lib, const char *rel)
{
    char path[PATH_LEN];
    snprintf(path, sizeof path, SD_PREFIX "/%s", rel);
    for (int i = 0; i < lib->ngames; i++)
        if (!strcmp(lib->games[i].path, path)) return &lib->games[i];
    return NULL;
}

static int index_of(const Library *lib, const char *rel)
{
    const Game *g = game(lib, rel);
    return g ? (int)(g - lib->games) : -1;
}

/* ---------- discovery ---------- */

static void test_discovery(void)
{
    static const char *const ROMS[] = {
        // GB: .gb, .sgb, an uppercase extension, a subfolder, and a MinUI-style folder name.
        "Roms/GB/Tetris (World) (Rev 1).gb",
        "Roms/GB/Pokemon - Red Version (USA, Europe) (SGB Enhanced).sgb",
        "Roms/GB/Kirby's Dream Land (USA, Europe).GB",
        "Roms/GB/Deep/Nested/Homebrew (World).gb",
        "Roms/Game Boy (GB)/Alleyway (World).gb",
        // GBC: .gbc, .dmg, all under a MinUI-style folder name.
        "Roms/Game Boy Color (GBC)/Pokemon - Crystal Version (USA, Europe) (Rev 1).gbc",
        "Roms/Game Boy Color (GBC)/Wario Land 3 (World) (En,Fr,De,Es,It).gbc",
        "Roms/Game Boy Color (GBC)/Tetris DX (World).gbc",
        "Roms/Game Boy Color (GBC)/Homebrew Color Demo (World).dmg",
        // GBA: only .gba counts, in a lowercase folder, a subfolder and a MinUI-style one.
        "Roms/gba/Pokemon - Emerald Version (USA, Europe).gba",
        "Roms/gba/Advance Wars (USA).gba",
        "Roms/gba/Homebrew/Puzzle Thing (World).gba",
        "Roms/Game Boy Advance (GBA)/MinUI Game (USA).gba",
    };
    static const char *const IGNORED[] = {
        "Roms/GB/README.txt",
        "Roms/GB/Pokemon - Blue Version.zip",              // no archives
        "Roms/gba/Pokemon - Emerald Version (USA, Europe).zip",
        "Roms/gba/notes.db",
        "Roms/GB/.hidden/Secret.gb",                       // hidden folder
        "Roms/GB/Imgs/Stray.gb",                           // Imgs is art, not games
        "Roms/gba/Wrong System (USA).gbc",                 // GBA takes .gba only
        "Roms/SFC/Chrono Trigger (USA).sfc",               // not a system Shelf runs
    };
    mkcard();
    for (size_t i = 0; i < sizeof ROMS / sizeof *ROMS; i++) rom(ROMS[i]);
    for (size_t i = 0; i < sizeof IGNORED / sizeof *IGNORED; i++) rom(IGNORED[i]);
    put("Roms/GB/Imgs/Tetris (World) (Rev 1).png", "art");
    put("Roms/Game Boy Advance (GBA)/Imgs/MinUI Game (USA).png", "art");
    // A folder symlink back up the tree must not recurse or duplicate.
    char link[PATH_LEN];
    host("Roms/GB/loop", link, sizeof link);
    CHECK(symlink("..", link) == 0);

    lib_set_root(root);
    Library lib;
    CHECK(lib_load(&lib) == 0);
    CHECK(lib.nsys == 3);
    CHECK(lib.ngames == 13 && lib.sys[0].ngames == 5 && lib.sys[1].ngames == 4 && lib.sys[2].ngames == 4);
    // The fixed system table: names, cores and distinct folder colours.
    CHECK(!strcmp(lib.sys[0].dir, "GB") && !strcmp(lib.sys[0].name, "Game Boy"));
    CHECK(!strcmp(lib.sys[1].dir, "GBC") && !strcmp(lib.sys[1].name, "Game Boy Color"));
    CHECK(!strcmp(lib.sys[2].dir, "GBA") && !strcmp(lib.sys[2].name, "Game Boy Advance"));
    CHECK(!strcmp(lib.sys[0].core, "gambatte") && !strcmp(lib.sys[1].core, "gambatte"));
    CHECK(!strcmp(lib.sys[2].core, "mgba"));
    CHECK(lib.sys[0].color != lib.sys[1].color && lib.sys[1].color != lib.sys[2].color &&
          lib.sys[0].color != lib.sys[2].color);

    for (size_t i = 0; i < sizeof ROMS / sizeof *ROMS; i++) CHECK(game(&lib, ROMS[i]));
    for (size_t i = 0; i < sizeof IGNORED / sizeof *IGNORED; i++) CHECK(!game(&lib, IGNORED[i]));

    // Art: the system's Imgs folder, an Imgs folder beside the ROM, and no art at all.
    char out[PATH_LEN];
    const Game *tetris = game(&lib, "Roms/GB/Tetris (World) (Rev 1).gb");
    CHECK(tetris && lib_img_path(&lib, tetris, out, sizeof out) == 0 && strstr(out, "/Roms/GB/Imgs/"));
    const Game *minui = game(&lib, "Roms/Game Boy Advance (GBA)/MinUI Game (USA).gba");
    CHECK(minui && lib_img_path(&lib, minui, out, sizeof out) == 0 &&
          strstr(out, "Game Boy Advance (GBA)/Imgs/"));
    const Game *puzzle = game(&lib, "Roms/gba/Homebrew/Puzzle Thing (World).gba");
    CHECK(puzzle && lib_img_path(&lib, puzzle, out, sizeof out) == -1);

    if (tetris && puzzle) {
        lib_save_dir(&lib, (int)(tetris - lib.games), out, sizeof out);
        CHECK(!strcmp(out, "/mnt/SDCARD/Saves/GB"));
        lib_save_dir(&lib, (int)(puzzle - lib.games), out, sizeof out);
        CHECK(!strcmp(out, "/mnt/SDCARD/Saves/GBA"));
    }
    lib_free(&lib);
    rmcard(); // symlinks are removed with the card, never followed
}

/* ---------- reading the data files ---------- */

static void make_three_games(void)
{
    rom("Roms/GB/Tetris (World) (Rev 1).gb");
    rom("Roms/Game Boy Color (GBC)/Wario Land 3 (World) (En,Fr,De,Es,It).gbc");
    rom("Roms/gba/Advance Wars (USA).gba");
}

static void test_reading(void)
{
    mkcard();
    make_three_games();
    // Recents: cleaned up on the way in (a duplicate, a path with "./", a missing ROM and
    // a value that isn't a path are all skipped).
    put(".shelf/data/recents.json",
        "[\"/mnt/SDCARD/Roms/gba/Advance Wars (USA).gba\",\n"
        " \"/mnt/SDCARD/Roms/./GB/Tetris (World) (Rev 1).gb\",\n"
        " \"/mnt/SDCARD/Roms/GB/Tetris (World) (Rev 1).gb\",\n"
        " \"/mnt/SDCARD/Roms/gba/Gone (USA).gba\",\n"
        " 7]\n");
    put(".shelf/data/favorites.json",
        "[\"/mnt/SDCARD/Roms/Game Boy Color (GBC)/Wario Land 3 (World) (En,Fr,De,Es,It).gbc\"]\n");
    put(".shelf/data/playtime.json",
        "{\"/mnt/SDCARD/Roms/gba/Advance Wars (USA).gba\": [3725, 4, 1767225600],\n"
        " \"/mnt/SDCARD/Roms/gba/Gone (USA).gba\": [60, 1, 1767225600]}\n");

    lib_set_root(root);
    Library lib;
    CHECK(lib_load(&lib) == 0);
    const Game *advance = game(&lib, "Roms/gba/Advance Wars (USA).gba");
    const Game *tetris = game(&lib, "Roms/GB/Tetris (World) (Rev 1).gb");
    const Game *wario = game(&lib, "Roms/Game Boy Color (GBC)/Wario Land 3 (World) (En,Fr,De,Es,It).gbc");
    CHECK(lib.nrecent == 2 && advance && tetris && wario);
    if (lib.nrecent == 2 && advance && tetris && wario) {
        CHECK(lib.recent[0] == advance - lib.games && lib.recent[1] == tetris - lib.games);
        CHECK(advance->recent == 0 && tetris->recent == 1 && wario->recent == -1);
        CHECK(wario->fav == 1 && advance->fav == 0 && tetris->fav == 0);
        CHECK(advance->play_time == 3725 && advance->play_count == 4 && advance->last_played == 1767225600);
        CHECK(tetris->play_time == 0 && tetris->play_count == 0 && tetris->last_played == 0);
    }
    lib_free(&lib);
    rmcard();
}

static void test_corrupt(void)
{
    mkcard();
    make_three_games();
    put(".shelf/data/recents.json", "not json at all\n");
    put(".shelf/data/favorites.json", "{\"not\": \"an array\"}\n");
    put(".shelf/data/playtime.json", "[1, 2, 3]\n");

    lib_set_root(root);
    Library lib;
    CHECK(lib_load(&lib) == 0);
    CHECK(lib.nrecent == 0);
    CHECK(lib.ngames == 3);
    for (int i = 0; i < lib.ngames; i++)
        CHECK(!lib.games[i].fav && lib.games[i].recent == -1 && lib.games[i].play_time == 0);
    // Writing over the broken files works and gives them the right shape.
    int gi = index_of(&lib, "Roms/GB/Tetris (World) (Rev 1).gb");
    CHECK(gi >= 0);
    CHECK(lib_mark_played(&lib, gi) == 0);
    lib_free(&lib);
    CHECK(lib_load(&lib) == 0);
    CHECK(lib.nrecent == 1 && lib.recent[0] == index_of(&lib, "Roms/GB/Tetris (World) (Rev 1).gb"));
    lib_free(&lib);
    rmcard();
}

/* ---------- writing the data files ---------- */

// A directory where the temp file goes makes that one save fail, which is how a full or
// read-only card looks from here.
static void block_save(const char *file, int blocked)
{
    char rel[PATH_LEN], path[PATH_LEN];
    snprintf(rel, sizeof rel, ".shelf/data/%s.json.tmp", file);
    host(rel, path, sizeof path);
    if (blocked) mkdir(path, 0700);
    else rmdir(path);
}

static void test_writing(void)
{
    mkcard();
    make_three_games();
    lib_set_root(root);
    Library lib;
    CHECK(lib_load(&lib) == 0);
    int tetris = index_of(&lib, "Roms/GB/Tetris (World) (Rev 1).gb");
    int advance = index_of(&lib, "Roms/gba/Advance Wars (USA).gba");
    int wario = index_of(&lib, "Roms/Game Boy Color (GBC)/Wario Land 3 (World) (En,Fr,De,Es,It).gbc");
    CHECK(tetris >= 0 && advance >= 0 && wario >= 0);
    if (tetris < 0 || advance < 0 || wario < 0) { lib_free(&lib); rmcard(); return; }

    // Recents: newest first, and the file (not just the library) holds that order.
    CHECK(lib_mark_played(&lib, tetris) == 0);
    CHECK(lib.nrecent == 1 && lib.recent[0] == tetris && lib.games[tetris].recent == 0);
    CHECK(lib_mark_played(&lib, advance) == 0);
    CHECK(lib.nrecent == 2 && lib.recent[0] == advance && lib.games[tetris].recent == 1);
    lib_free(&lib);
    CHECK(lib_load(&lib) == 0);
    CHECK(lib.nrecent == 2 && lib.recent[0] == advance && lib.recent[1] == tetris);
    // Playing an older recent again moves it up without duplicating it.
    CHECK(lib_mark_played(&lib, tetris) == 0);
    CHECK(lib.nrecent == 2 && lib.recent[0] == tetris && lib.recent[1] == advance);
    lib_free(&lib);
    CHECK(lib_load(&lib) == 0 && lib.nrecent == 2 && lib.recent[0] == tetris);

    // Favorites.
    CHECK(lib_set_fav(&lib, wario, 1) == 0);
    CHECK(lib.games[wario].fav == 1 && lib.games[tetris].fav == 0);
    lib_free(&lib);
    CHECK(lib_load(&lib) == 0 && lib.games[wario].fav == 1 && lib.games[tetris].fav == 0);
    CHECK(lib_set_fav(&lib, wario, 0) == 0);
    lib_free(&lib);
    CHECK(lib_load(&lib) == 0 && lib.games[wario].fav == 0);

    // Play time: sessions add up and the last session's start time replaces the old one.
    CHECK(lib_add_play(&lib, advance, 90, 1767225600) == 0);
    CHECK(lib.games[advance].play_time == 90 && lib.games[advance].play_count == 1);
    CHECK(lib_add_play(&lib, advance, 30, 1767225660) == 0);
    CHECK(lib.games[advance].play_time == 120 && lib.games[advance].play_count == 2);
    CHECK(lib.games[advance].last_played == 1767225660);
    CHECK(lib.games[tetris].play_count == 0);
    lib_free(&lib);
    CHECK(lib_load(&lib) == 0);
    CHECK(lib.games[advance].play_time == 120 && lib.games[advance].play_count == 2);
    CHECK(lib.games[advance].last_played == 1767225660 && lib.games[tetris].play_count == 0);

    // Nothing is written when the file can't be saved: the library keeps what it had and
    // the old file stays as it was.
    int nrecent = lib.nrecent, first = lib.recent[0];
    block_save("recents", 1);
    CHECK(lib_mark_played(&lib, wario) == -1);
    CHECK(lib.nrecent == nrecent && lib.recent[0] == first && lib.games[wario].recent == -1);
    block_save("recents", 0);
    CHECK(lib_mark_played(&lib, wario) == 0);

    block_save("favorites", 1);
    CHECK(lib_set_fav(&lib, tetris, 1) == -1);
    CHECK(lib.games[tetris].fav == 0);
    block_save("favorites", 0);
    CHECK(lib_set_fav(&lib, tetris, 1) == 0);

    int play_time = lib.games[advance].play_time, plays = lib.games[advance].play_count;
    long long last = lib.games[advance].last_played;
    block_save("playtime", 1);
    CHECK(lib_add_play(&lib, advance, 60, 1767225720) == -1);
    CHECK(lib.games[advance].play_time == play_time && lib.games[advance].play_count == plays);
    CHECK(lib.games[advance].last_played == last);
    block_save("playtime", 0);
    CHECK(lib_add_play(&lib, advance, 60, 1767225720) == 0);
    lib_free(&lib);

    // Settings use the same writer: a first save creates Shelf's data folder.
    char cmd[PATH_LEN + 16];
    snprintf(cmd, sizeof cmd, "rm -rf '%s/.shelf'", root);
    CHECK(system(cmd) == 0);
    CHECK(lib_write_json(DATA_DIR "/settings.json", "{\"dark\": true}\n") == 0);
    char path[PATH_LEN];
    host_path(DATA_DIR "/settings.json", path, sizeof path);
    FILE *f = fopen(path, "rb");
    CHECK(f != NULL);
    if (f) {
        char buf[64] = "";
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[n] = 0;
        CHECK(!strcmp(buf, "{\"dark\": true}\n"));
    }
    rmcard();
}

static void test_max_recents(void)
{
    mkcard();
    for (int i = 0; i < MAX_RECENT + 6; i++) {
        char rel[64];
        snprintf(rel, sizeof rel, "Roms/GB/Game %02d (USA).gb", i);
        rom(rel);
    }
    // Newest first, so Game 00 is the most recent and the tail is what falls off.
    char json[4096];
    size_t len = snprintf(json, sizeof json, "[");
    for (int i = 0; i < MAX_RECENT + 6; i++)
        len += snprintf(json + len, sizeof json - len, "%s\"/mnt/SDCARD/Roms/GB/Game %02d (USA).gb\"", i ? "," : "", i);
    snprintf(json + len, sizeof json - len, "]");
    put(".shelf/data/recents.json", json);

    lib_set_root(root);
    Library lib;
    CHECK(lib_load(&lib) == 0);
    CHECK(lib.ngames == MAX_RECENT + 6);
    CHECK(lib.nrecent == MAX_RECENT);
    CHECK(lib.recent[0] == index_of(&lib, "Roms/GB/Game 00 (USA).gb"));
    char rel[64];
    snprintf(rel, sizeof rel, "Roms/GB/Game %02d (USA).gb", MAX_RECENT - 1);
    int oldest = index_of(&lib, rel);
    CHECK(oldest >= 0 && lib.games[oldest].recent == MAX_RECENT - 1);

    // Playing a game the list doesn't hold yet makes it the most recent and drops the
    // oldest entry, so the list stays at MAX_RECENT.
    snprintf(rel, sizeof rel, "Roms/GB/Game %02d (USA).gb", MAX_RECENT + 5);
    int newest = index_of(&lib, rel);
    CHECK(newest >= 0 && oldest >= 0);
    CHECK(lib_mark_played(&lib, newest) == 0);
    CHECK(lib.nrecent == MAX_RECENT && lib.recent[0] == newest);
    CHECK(lib.games[oldest].recent == -1);
    lib_free(&lib);
    CHECK(lib_load(&lib) == 0);
    CHECK(lib.nrecent == MAX_RECENT && lib.recent[0] == index_of(&lib, rel));
    CHECK(lib.games[oldest].recent == -1);
    lib_free(&lib);
    rmcard();
}

static void test_missing_roms_folder(void)
{
    mkcard();
    lib_set_root(root);
    Library lib;
    // No Roms folder at all: a card Shelf can't work with, not an empty library.
    CHECK(lib_load(&lib) == -1);
    lib_free(&lib);
    rmcard();
}

int main(void)
{
    test_discovery();
    test_reading();
    test_corrupt();
    test_writing();
    test_max_recents();
    test_missing_roms_folder();
    if (failures) return 1;
    puts("library: ROM folders, recents, favorites and play time passed");
    return 0;
}
