// The UI is loaded once and entered per visit: across many home <-> in-game menu cycles the
// fonts are read from the card once, and a visit to a screen draws exactly what a freshly
// loaded UI draws, while the caches (glyphs, background, sprites) are kept and are dropped
// when the theme or the data under them changes.
// Usage: test-ui-resident <fixture SD card> <fonts>
#include "../src/ui.c"
#include <assert.h>
#include <string.h>

static Library lib;
static const char *fonts;
static Image *canvas, *card, *full;
static double fixed_wall(void) { return 1767225605.0; }

static const Image *hook_card(int i) { return card; }
static const Image *hook_full(int i) { return full; }
static void hook_select(int i) {}
static unsigned hook_rev(void) { return 1; }
static void hooks(void) { ui_switch_hooks(hook_card, hook_full, hook_select, hook_rev); }

static unsigned checksum(const Image *im)
{
    unsigned hash = 2166136261u;
    for (int i = 0; i < im->w * im->h; i++) hash = (hash ^ im->px[i]) * 16777619u;
    return hash;
}

static void settle(int frames)
{
    for (int f = 0; f < frames; f++) {
        while (icons_pump(64)) {}
        ui_update(1.f / 60);
        ui_draw(canvas);
    }
}

// What the home screen shows after a fixed script of presses, once it has settled.
static unsigned home_picture(void)
{
    static const Button KEYS[] = {BTN_RIGHT, BTN_R, BTN_A, BTN_DOWN, BTN_Y, BTN_B};
    settle(2);
    for (size_t k = 0; k < sizeof KEYS / sizeof *KEYS; k++) { ui_button(KEYS[k]); settle(40); }
    return checksum(canvas);
}

static unsigned menu_picture(void)
{
    static const Button KEYS[] = {BTN_RIGHT, BTN_LEFT};
    settle(90); // the opening zoom
    for (size_t k = 0; k < sizeof KEYS / sizeof *KEYS; k++) { ui_button(KEYS[k]); settle(40); }
    return checksum(canvas);
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    lib_set_root(argv[1]);
    fonts = argv[2];
    assert(lib_load(&lib) == 0 && lib.nrecent > 1 && lib.ngames > 4);
    ui_set_wall_clock(fixed_wall);
    icons_init(&lib);
    canvas = img_new(SCREEN_W, SCREEN_H);
    int cw, ch;
    ui_switch_card_size(&cw, &ch);
    card = img_new(cw, ch);
    full = img_new(SCREEN_W, SCREEN_H);
    assert(canvas && card && full);
    for (int i = 0; i < cw * ch; i++) card->px[i] = 0xff000000u | (i * 7);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) full->px[i] = 0xff000000u | (i * 5);

    // Nothing is loaded: entering fails, and leaving and freeing are harmless.
    assert(!ui_loaded() && ui_enter_home() == -1 && ui_enter_switcher(0, 0) == -1);
    ui_leave(); ui_free();

    // The theme chosen before the first ui_init is the one it builds in.
    ui_set_dark(1);
    assert(ui_init(&lib, fonts) == 0 && ui_dark() == 1);
    assert(S.bg->px[0] == (0xff000000u | PALETTES[1].bg));
    ui_free();
    ui_set_dark(0);

    // Reference pictures from a freshly loaded UI, with the art decoded.
    unsigned reads = font_files_read();
    assert(ui_init(&lib, fonts) == 0);
    assert(font_files_read() == reads + 2); // Bold and ExtraBold, once
    settle(2);
    for (int i = 0; i < 4; i++) { ui_button(BTN_R); settle(30); } // decode the art
    ui_free();
    assert(ui_init(&lib, fonts) == 0);
    unsigned home_ref = home_picture();
    ui_free();
    hooks();
    assert(ui_init_switcher(&lib, fonts, 1, 1) == 0);
    unsigned menu_ref = menu_picture();
    ui_free();
    assert(home_ref != menu_ref);

    // Many cycles, one load: the fonts aren't read again, and every visit draws what the
    // reference did (the home row, the folder grid and the switcher all start fresh).
    reads = font_files_read();
    assert(ui_init(&lib, fonts) == 0);
    assert(font_files_read() == reads + 2);
    Font *title = S.title, *label = S.label, *body = S.body;
    Image *bg = S.bg;
    assert(S.small == S.body); // the same file and size is one Font, not two glyph caches
    for (int cycle = 0; cycle < 6; cycle++) {
        if (cycle) assert(ui_enter_home() == 0);
        assert(home_picture() == home_ref);
        ui_leave();
        hooks();
        assert((cycle % 2 ? ui_init_switcher(&lib, fonts, 1, 1) : ui_enter_switcher(1, 1)) == 0);
        assert(menu_picture() == menu_ref);
        ui_leave();
        // The visit's borrowings are gone: hooks, screenshots, sprites and the dark palette.
        assert(!W.on && !W.card && !W.full && !W.sprites[0] && !W.small[0]);
        assert(P == &PALETTES[0] && !ui_dark());
    }
    assert(font_files_read() == reads + 2);
    assert(S.title == title && S.label == label && S.body == body && S.bg == bg);

    // The sprite caches survive a visit, and a visit that finds the data changed rebuilds
    // what that data fed: a game's play time in the detail panel, a folder's game count.
    assert(ui_enter_home() == 0);
    settle(60);
    unsigned builds = 0;
    for (int i = 0; i < TILE_CACHE_CAP; i++) builds += tiles[i].image != NULL;
    assert(builds > 0);
    ui_leave();
    assert(ui_enter_home() == 0);
    int kept = 0;
    for (int i = 0; i < TILE_CACHE_CAP; i++) kept += tiles[i].image != NULL;
    assert(kept == (int)builds);
    assert(panel_game == -1);

    int fav_folder = -1;
    for (int k = 0; k < S.nfolders; k++) if (S.folders[k].kind == F_FAV) fav_folder = k;
    assert(fav_folder >= 0);
    int before = S.folders[fav_folder].ngames;
    const Image *tile = tile_sprite(1, fav_folder, TILE, 1);
    assert(tile);
    unsigned tile_before = checksum(tile);
    const Image *thumb = folder_thumb(&S.folders[fav_folder]);
    // The last game, so the folder's thumbnail (its first game with art) stays the same
    // and only the count on the tile can tell the sprite is stale.
    int gi = -1;
    for (int i = 0; i < lib.ngames; i++) if (!lib.games[i].fav) gi = i;
    assert(gi >= 0 && lib_set_fav(&lib, gi, 1) == 0); // changed while the UI was away
    ui_leave();
    assert(ui_enter_home() == 0);
    int fav_now = -1;
    for (int k = 0; k < S.nfolders; k++) if (S.folders[k].kind == F_FAV) fav_now = k;
    assert(fav_now == fav_folder && S.folders[fav_now].ngames == before + 1);
    assert(folder_thumb(&S.folders[fav_now]) == thumb);
    tile = tile_sprite(1, fav_now, TILE, 1);
    assert(tile && checksum(tile) != tile_before);
    assert(lib_set_fav(&lib, gi, 0) == 0);

    // The theme: unchanged is a no-op, a change rebuilds the background and drops the
    // sprites (they carry the palette's shadows), and survives visits to the switcher.
    ui_leave();
    assert(ui_enter_home() == 0);
    bg = S.bg;
    ui_set_dark(0);
    assert(S.bg == bg);
    ui_set_dark(1);
    assert(ui_dark() && S.bg != bg && S.bg->px[0] == (0xff000000u | PALETTES[1].bg));
    for (int i = 0; i < TILE_CACHE_CAP; i++) assert(!tiles[i].image);
    ui_leave();
    hooks();
    assert(ui_enter_switcher(0, 0) == 0);
    assert(P == &PALETTES[1]);
    ui_set_dark(0); // the settings changed under the switcher: the dots follow, the row stays dark
    assert(P == &PALETTES[1] && !ui_dark() && S.bg->px[0] == (0xff000000u | PALETTES[0].bg));
    ui_leave();
    assert(P == &PALETTES[0]);
    assert(ui_enter_home() == 0);
    assert(home_picture() == home_ref);

    // Settings, the battery and the toast: the first two carry over a visit, the toast doesn't.
    Settings set = ui_settings();
    set.volume = set.volume ? 0 : 3;
    ui_set_settings(&set);
    ui_set_status(77, 1);
    ui_set_toast("Hello");
    ui_leave();
    assert(ui_enter_home() == 0);
    assert(ui_settings().volume == set.volume && S.battery == 77 && S.charging == 1);
    assert(!S.toast[0] && S.sel == 0 && !S.options && S.view == V_HOME);

    // Another font folder reloads the fonts; the same one never does.
    reads = font_files_read();
    assert(ui_init(&lib, fonts) == 0 && font_files_read() == reads);
    ui_free();
    assert(!ui_loaded());

    img_free(canvas); img_free(card); img_free(full);
    icons_free();
    lib_free(&lib);
    puts("UI resident: fonts read once across cycles; visits draw as a fresh load; theme and data invalidate caches");
    return 0;
}
