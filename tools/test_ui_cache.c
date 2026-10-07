// Exercise the real UI and cache with a fixture library (no SDL/window required).
#include "../src/ui.c"
#include <assert.h>

static unsigned checksum(const Image *im)
{
    unsigned hash = 2166136261u;
    for (int i = 0; i < im->w * im->h; i++) hash = (hash ^ im->px[i]) * 16777619u;
    return hash;
}

int main(int argc, char **argv)
{
    assert(argc == 3);
    lib_set_root(argv[1]);
    Library lib;
    assert(lib_load(&lib) == 0);
    assert(lib.ngames && lib.nrecent);
    icons_init(&lib);
    assert(ui_init(&lib, argv[2]) == 0);
    Image *canvas = img_new(SCREEN_W, SCREEN_H);
    assert(canvas);

    // A sprite built before its art loads must be replaced when art arrives.
    int gi = lib.recent[0];
    const Image *initial = tile_sprite(0, gi, TILE, 1);
    assert(initial);
    assert(icon_get(gi, 1) == NULL);
    while (icons_pump(64)) {}
    assert(icon_get(gi, 1));
    assert(tile_sprite(0, gi, TILE, 1));
    int updated = 0;
    for (int i = 0; i < TILE_CACHE_CAP; i++)
        if (tiles[i].image && tiles[i].kind == 0 && tiles[i].id == gi &&
            tiles[i].size == TILE && tiles[i].sources[0] == icon_get(gi, 1))
            updated = 1;
    assert(updated);

    // Settle requests/animation. The cursor must animate without rebuilding content.
    for (int i = 0; i < 120; i++) {
        icons_pump(64); ui_update(1.f / 60); ui_draw(canvas);
    }
    unsigned builds = S.scene_builds, scene = checksum(S.scene), frame = checksum(canvas);
    for (int i = 0; i < 60; i++) { ui_update(1.f / 60); ui_draw(canvas); }
    assert(S.scene_builds == builds);
    assert(checksum(S.scene) == scene);
    assert(checksum(canvas) != frame);

    // Navigation, folder entry, and sort changes must invalidate the scene.
    ui_button(BTN_R); ui_update(1.f / 60); ui_draw(canvas);
    assert(S.scene_builds > builds);
    ui_button(BTN_A); ui_update(1.f / 60); ui_draw(canvas);
    assert(S.view == V_FOLDER);
    builds = S.scene_builds;
    ui_button(BTN_Y); ui_draw(canvas);
    assert(S.scene_builds > builds);
    ui_button(BTN_B); ui_draw(canvas);
    assert(S.view == V_HOME);
    // While springs move, frames are drawn directly; once they settle the scene is retained.
    ui_reset_home(); ui_draw(canvas);
    assert(scene_moving() == !!S.scene_dirty);
    for (int i = 0; i < 120; i++) { ui_update(1.f / 60); ui_draw(canvas); }
    assert(!scene_moving() && !S.scene_dirty);
    builds = S.scene_builds;
    ui_update(1.f / 60); ui_draw(canvas);
    assert(S.scene_builds == builds);

    // Options: dark mode repaints with the dark palette and asks to be saved; the
    // panel stays retained while open; MENU and the boot row hand actions to main.
    uint32_t light_bg = S.scene->px[SCREEN_W * 2 + 2];
    ui_button(BTN_START); ui_draw(canvas);
    assert(S.options && ui_take_action() == UI_NONE);
    ui_button(BTN_A); ui_draw(canvas);
    assert(ui_dark() && ui_take_action() == UI_THEME);
    for (int i = 0; i < 5; i++) { ui_update(1.f / 60); ui_draw(canvas); }
    assert(S.scene->px[SCREEN_W * 2 + 2] != light_bg);
    builds = S.scene_builds;
    ui_update(1.f / 60); ui_draw(canvas);
    assert(S.scene_builds == builds);
    ui_set_boot(1);
    ui_button(BTN_DOWN); ui_button(BTN_A);
    assert(!S.options && ui_take_action() == UI_BOOT_ONION);
    ui_button(BTN_MENU);
    assert(ui_take_action() == UI_EXIT);
    ui_set_dark(0);

    // Decoded icons stay within budget across a long browse: everything in "All games",
    // trimmed between frames as the loop does, without dangling sprites or lost art.
    size_t budget = (size_t)ICON_BIG * ICON_BIG * 4 * 2;
    icons_set_budget(budget);
    ui_button(BTN_START); ui_button(BTN_B); // close any sheet
    ui_reset_home();
    ui_button(BTN_R); ui_button(BTN_A);
    assert(S.view == V_FOLDER && S.nfg == lib.ngames);
    for (int step = 0; step < 3 * lib.ngames; step++) {
        ui_button(step < lib.ngames ? BTN_DOWN : BTN_UP);
        if (step % 3 == 2) ui_button(step < lib.ngames ? BTN_RIGHT : BTN_LEFT);
        for (int f = 0; f < 4; f++) {
            icons_trim();
            ui_update(1.f / 60); ui_draw(canvas);
            while (icons_pump(64)) {}
        }
        // Over budget only by what the last frame drew: a grid of small tiles and the panel.
        assert(icons_bytes() <= budget + (size_t)ICON_SMALL * ICON_SMALL * 4 * 16 + (size_t)ICON_BIG * ICON_BIG * 4);
    }
    for (int f = 0; f < 4; f++) { icons_trim(); ui_update(1.f / 60); ui_draw(canvas); while (icons_pump(64)) {} }
    assert(icon_get(S.fgames[S.fsel], 1)); // the panel's art came back after eviction
    icons_set_budget(0);

    ui_free(); img_free(canvas); icons_free(); lib_free(&lib);
    puts("UI cache: art invalidation, retained scene, cursor animation, navigation, options and icon budget passed");
    return 0;
}
