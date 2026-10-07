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

    ui_free(); img_free(canvas); icons_free(); lib_free(&lib);
    puts("UI cache: art invalidation, retained scene, cursor animation and navigation passed");
    return 0;
}
