// Switcher: Onion's screenshot hash, the recents-only load, and resume's recents rewrite.
// Usage: test-switcher <fixture SD card>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../src/library.h"
#include "../src/switcher.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

// Expected values from Onion's own FNV1A_Pippip_Yurii (src/common/utils/hash.h).
static const struct { const char *s; uint32_t h; } HASHES[] = {
    {"a", 579392569u},
    {"abcdefgh", 622653775u},
    {"abcdefghi", 2300094295u},
    {"0123456789abcdef", 2519323961u},
    {"0123456789abcdefg", 3638872414u},
    {"/mnt/SDCARD/Roms/PORTS/x.port", 2005797617u},
    {"/mnt/SDCARD/Emu/GBA/../../Roms/GBA/Pokemon - Emerald Version (USA, Europe).gba", 2349991472u},
};

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    static char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <SDCARD>\n", argv[0]); return 2; }
    for (size_t i = 0; i < sizeof HASHES / sizeof *HASHES; i++) CHECK(sw_hash(HASHES[i].s) == HASHES[i].h);

    // Work on a copy of the recents file so the fixture stays put.
    char cmd[1024], dir[] = "/tmp/shelf-switcher-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    snprintf(cmd, sizeof cmd, "cp -R '%s/.' '%s/'", argv[1], dir);
    CHECK(system(cmd) == 0);
    lib_set_root(dir);

    static Recent rec[MAX_RECENT];
    Library lib;
    CHECK(lib_load_recents(&lib, rec, MAX_RECENT) == 0);
    CHECK(lib.ngames == 5 && lib.nrecent == 5);
    CHECK(!strcmp(lib.games[0].title, "Pokemon - Emerald Version"));
    CHECK(!strcmp(rec[1].rompath, "/mnt/SDCARD/Emu/SFC/../../Roms/SFC/Super Metroid (Japan, USA) (En,Ja).sfc"));
    CHECK(!strcmp(rec[1].launch, "/mnt/SDCARD/Emu/SFC/launch.sh"));
    CHECK(!strcmp(lib.sys[lib.games[1].sys].label, "SNES"));
    CHECK(!strcmp(lib.games[3].title, "The Legend of Zelda - The Minish Cap"));

    // Resuming the third recent: command in Onion's layout, and that line moves to the top.
    char out[600], recents[600];
    snprintf(out, sizeof out, "%s/cmd.sh", dir);
    CHECK(lib_resume(&rec[2], out) == 0);
    char *c = slurp(out);
    CHECK(c && !strcmp(c, "LD_PRELOAD=/mnt/SDCARD/miyoo/app/../lib/libpadsp.so \"/mnt/SDCARD/Emu/PS/launch.sh\" "
                          "\"/mnt/SDCARD/Emu/PS/../../Roms/PS/Crash Bandicoot (USA).cue\"\n"));
    lib_free(&lib);
    CHECK(lib_load_recents(&lib, rec, MAX_RECENT) == 0);
    CHECK(lib.ngames == 5 && !strcmp(lib.games[0].title, "Crash Bandicoot"));
    CHECK(!strcmp(lib.games[1].title, "Pokemon - Emerald Version"));
    snprintf(recents, sizeof recents, "%s/Roms/recentlist.json", dir);
    char *r = slurp(recents);
    int lines = 0;
    for (char *p = r; p && *p; p++) lines += *p == '\n';
    CHECK(lines == 5);

    // The card worker can be started again after a stop: the first card (Crash Bandicoot,
    // which has a screenshot) loads both times.
    for (int round = 0; round < 2; round++) {
        sw_cards_start(&lib, rec, 32, 24, NULL);
        const Image *card = NULL;
        for (int i = 0; i < 200 && !card; i++) { card = sw_card(0); if (!card) usleep(5000); }
        CHECK(card && card->w == 32 && card->h == 24);
        sw_cards_stop();
        CHECK(sw_card(0) == NULL);
    }

    // Borrowing: walking the selection loads full-size screenshots past the worker's keep
    // limit, so it evicts. The selection's full image and every card stay put while held.
    sw_cards_start(&lib, rec, 32, 24, NULL);
    const Image *cards[MAX_RECENT] = {0};
    int fulls = 0;
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < lib.ngames; i++) {
            sw_card_select(i);
            const Image *full = NULL;
            for (int t = 0; t < 100 && !full; t++) { full = sw_full(i); if (!full) usleep(2000); }
            if (!full) continue; // no screenshot for this recent
            fulls++;
            unsigned sum = 0;
            for (int p = 0; p < full->w * full->h; p++) sum = sum * 31 + full->px[p];
            usleep(20000); // the worker loads and evicts around the selection meanwhile
            unsigned again = 0;
            for (int p = 0; p < full->w * full->h; p++) again = again * 31 + full->px[p];
            CHECK(sw_full(i) == full && again == sum);
            for (int k = 0; k < lib.ngames; k++) {
                const Image *card = sw_card(k);
                if (cards[k]) CHECK(card == cards[k]); // published once, never replaced
                else cards[k] = card;
            }
        }
    CHECK(fulls >= 8); // both passes, past FULL_KEEP: eviction happened
    sw_cards_stop();

    // A recents file that can't be rewritten still queues the game, but says so, and the
    // file keeps its old order.
    char blocker[620];
    snprintf(blocker, sizeof blocker, "%s.tmp", recents);
    CHECK(mkdir(blocker, 0700) == 0); // the temp file can't be created over a directory
    CHECK(lib_resume(&rec[1], out) == LIB_RECENTS_UNSAVED);
    c = slurp(out);
    CHECK(c && strstr(c, "Pokemon - Emerald"));
    lib_free(&lib);
    CHECK(lib_load_recents(&lib, rec, MAX_RECENT) == 0);
    CHECK(!strcmp(lib.games[0].title, "Crash Bandicoot"));
    rmdir(blocker);
    lib_free(&lib);

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd)) {}
    if (failures) return 1;
    printf("switcher tests passed\n");
    return 0;
}
