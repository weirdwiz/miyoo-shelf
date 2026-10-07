#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "icons.h"
#include "text.h"

// Palettes of the handheld UI; light matches the mockups.
typedef struct {
    uint32_t bg, dot, ink, sub, on_ink, line, dim_dot, key, chip, footer, panel;
    uint8_t footer_a, shadow_a;
} Palette;
static const Palette PALETTES[2] = {
    {0xeceff4, 0xd3d8e2, 0x262a35, 0x6b7184, 0xffffff, 0xd5dae4, 0xc9cfda, 0x3a3f4d, 0xffffff, 0xffffff, 0xffffff, 190, 38},
    {0x14171f, 0x222734, 0xe6e9f0, 0x8d94a7, 0x14171f, 0x2f3544, 0x3a4152, 0x4a5163, 0x262b38, 0x1c2029, 0x20242f, 215, 90},
};
static const Palette *P = &PALETTES[0];
#define C_BG (P->bg)
#define C_DOT (P->dot)
#define C_INK (P->ink)
#define C_SUB (P->sub)
#define C_SEL 0x19a4f0
#define C_SEL2 0x7fd3ff

// Home row geometry.
#define ROW_Y 146
#define TILE 168
#define STEP 184         // tile + gap
#define DIVIDER_GAP 20   // extra space between Recent and Library
#define CENTER_X 236     // (640 - 168) / 2

// Folder view: the grid on the left, the selected game's detail panel on the right.
// Three rows of small tiles, or two rows of big ones when the folder fits in a 2x2 block.
typedef struct { int tile, col, row, rows, vis; } Grid;
static const Grid GRIDS[2] = {{88, 100, 98, 3, 3}, {136, 152, 152, 2, 2}};
#define G_X 24
#define G_Y 100
#define G_EDGE 320       // tiles scrolling right slide under the panel from here
#define PANEL_X 330
#define PANEL_Y 40
#define PANEL_W 290
#define PANEL_H 382
#define FOOTER_H 46

typedef struct { float x, v, t; } Spring;

static void spring_step(Spring *s, float dt, float k, float c)
{
    if (fabsf(s->t - s->x) < .001f && fabsf(s->v) < .01f) {
        s->x = s->t; s->v = 0;
        return;
    }
    while (dt > 0) {
        float h = dt > 1.f / 240 ? 1.f / 240 : dt;
        float a = k * (s->t - s->x) - c * s->v;
        s->v += a * h;
        s->x += s->v * h;
        dt -= h;
    }
}
// Bouncy: ~8% overshoot, settles in ~0.4s. That's the DS feel.
#define BOUNCE 320.f, 22.f
#define SNAPPY 500.f, 40.f

enum { F_ALL, F_FAV, F_SYS };
typedef struct {
    int kind, sys;
    char label[48];
    uint32_t color;
    int *games, ngames;
} Folder;

enum { V_HOME, V_FOLDER };
static const char *SORTS[] = {"A–Z", "Recent first", "Favorites first"};

static struct {
    Library *lib;
    Font *title, *body, *small, *label;
    Image *bg, *layer, *scene;
    int scene_dirty;
    unsigned art_revision, scene_builds;
    Folder folders[MAX_SYSTEMS + 2];
    int nfolders;

    int view;
    float t;                 // seconds since start, drives the cursor pulse
    Spring enter;            // view transition 0..1

    int sel;                 // home row selection
    Spring track;            // home row x offset
    Spring hscale[MAX_RECENT + MAX_SYSTEMS + 2];

    int folder, fsel, sort, fcol;
    const Grid *grid;
    int *fgames, nfg;
    Spring ftrack;
    Spring *fscale;

    int launching;           // game index, -1 when idle
    float launch_t;
    Spring launch_scale;
    int launch_ready;
    int sound;               // pending UI click

    int dark;                // palette index
    int options, osel;       // Options panel open, its selected row
    int boot;                // Shelf is Onion's home screen (boot hook installed)
    int action;              // pending UiAction
    int battery, charging, wifi; // see ui_set_status
} S;

// Options rows, top to bottom.
enum { O_DARK, O_BOOT, O_ONION, O_COUNT };

static void tiles_free(void);
// The game switcher view, at the end of this file.
static int sw_on(void);
static void sw_button(Button b);
static void sw_update(float dt);
static void sw_draw(Image *c);
static void sw_free(void);

static int home_count(void) { return S.lib->nrecent + S.nfolders; }

static void add_folder(int kind, int sys, const char *label, uint32_t color)
{
    Folder *f = &S.folders[S.nfolders];
    memset(f, 0, sizeof *f);
    f->kind = kind;
    f->sys = sys;
    snprintf(f->label, sizeof f->label, "%s", label);
    f->color = color;
    f->games = malloc(sizeof(int) * (S.lib->ngames ? S.lib->ngames : 1));
    for (int i = 0; i < S.lib->ngames; i++) {
        Game *g = &S.lib->games[i];
        if (kind == F_ALL || (kind == F_FAV && g->fav) || (kind == F_SYS && g->sys == sys)) f->games[f->ngames++] = i;
    }
    if (f->ngames || kind == F_ALL) S.nfolders++;
    else free(f->games);
}

static void build_bg(void)
{
    S.bg = img_new(SCREEN_W, SCREEN_H);
    gfx_clear(S.bg, rgb(C_BG));
    for (int y = 8; y < SCREEN_H; y += 16)
        for (int x = 8; x < SCREEN_W; x += 16) gfx_fill_rrect(S.bg, x - 1.3f, y - 1.3f, 2.6f, 2.6f, 1.3f, rgb(C_DOT));
}

int ui_init(Library *lib, const char *font_dir)
{
    memset(&S, 0, sizeof S);
    S.lib = lib;
    char bold[PATH_LEN], xbold[PATH_LEN];
    snprintf(bold, sizeof bold, "%s/Shelf-Bold.ttf", font_dir);
    snprintf(xbold, sizeof xbold, "%s/Shelf-ExtraBold.ttf", font_dir);
    // Sized for a 2.8" 640x480 panel held at arm's length.
    S.title = font_load(xbold, 32);
    S.label = font_load(xbold, 20);
    S.body = font_load(bold, 19);
    S.small = font_load(bold, 17);
    if (!S.title || !S.body || !S.small || !S.label) return -1;
    build_bg();
    S.layer = img_new(SCREEN_W, SCREEN_H);
    S.scene = img_new(SCREEN_W, SCREEN_H);
    S.scene_dirty = 1;

    add_folder(F_ALL, -1, "All games", 0x2b3040);
    add_folder(F_FAV, -1, "Favorites", 0xf0a020);
    for (int i = 0; i < lib->nsys; i++) add_folder(F_SYS, i, lib->sys[i].label, lib->sys[i].color);

    S.launching = -1;
    S.battery = S.wifi = -1;
    S.dark = P == &PALETTES[1];
    S.enter.x = S.enter.t = 1;
    for (int i = 0; i < home_count(); i++) S.hscale[i].x = S.hscale[i].t = 1;
    S.track.x = S.track.t = CENTER_X;
    return 0;
}

void ui_free(void)
{
    sw_free();
    for (int i = 0; i < S.nfolders; i++) free(S.folders[i].games);
    free(S.fgames);
    free(S.fscale);
    img_free(S.bg);
    img_free(S.layer);
    img_free(S.scene);
    tiles_free();
    font_free(S.title); font_free(S.body); font_free(S.small); font_free(S.label);
}

/* ---------- model helpers ---------- */

static float item_x(int i) { return i * STEP + (i >= S.lib->nrecent ? DIVIDER_GAP : 0); }

static void home_select(int i)
{
    S.sel = i;
    S.track.t = CENTER_X - item_x(i);
    for (int k = 0; k < home_count(); k++) S.hscale[k].t = k == i ? 1.08f : 1.f;
}

static int sort_mode;
static int cmp_fg(const void *a, const void *b)
{
    const Game *x = &S.lib->games[*(const int *)a], *y = &S.lib->games[*(const int *)b];
    if (sort_mode == 1) {
        int rx = x->recent < 0 ? 1 << 20 : x->recent, ry = y->recent < 0 ? 1 << 20 : y->recent;
        if (rx != ry) return rx - ry;
    }
    if (sort_mode == 2 && x->fav != y->fav) return y->fav - x->fav;
    return *(const int *)a - *(const int *)b; // library is already A–Z
}

static void folder_select(int i)
{
    const Grid *g = S.grid;
    S.fsel = i;
    int col = i / g->rows, cols = (S.nfg + g->rows - 1) / g->rows;
    if (col < S.fcol) S.fcol = col;
    if (col > S.fcol + g->vis - 1) S.fcol = col - g->vis + 1;
    if (S.fcol > cols - g->vis) S.fcol = cols - g->vis;
    if (S.fcol < 0) S.fcol = 0;
    S.ftrack.t = G_X - S.fcol * g->col;
    for (int k = 0; k < S.nfg; k++) S.fscale[k].t = k == i ? 1.1f : 1.f;
}

static void folder_open(int f)
{
    Folder *fd = &S.folders[f];
    S.folder = f;
    S.view = V_FOLDER;
    free(S.fgames);
    free(S.fscale);
    S.nfg = fd->ngames;
    S.grid = &GRIDS[S.nfg <= 4];
    S.fgames = malloc(sizeof(int) * (S.nfg ? S.nfg : 1));
    memcpy(S.fgames, fd->games, sizeof(int) * S.nfg);
    sort_mode = S.sort;
    qsort(S.fgames, S.nfg, sizeof(int), cmp_fg);
    S.fscale = calloc(S.nfg ? S.nfg : 1, sizeof(Spring));
    for (int k = 0; k < S.nfg; k++) S.fscale[k].x = S.fscale[k].t = 1;
    S.fcol = 0;
    S.ftrack.x = S.ftrack.t = G_X;
    folder_select(0);
    S.enter.x = 0; S.enter.v = 0; S.enter.t = 1;
}

static void start_launch(int gi)
{
    S.launching = gi;
    S.launch_t = 0;
    S.launch_scale.x = .6f; S.launch_scale.v = 0; S.launch_scale.t = 1.15f;
}

static void ui_button_inner(Button b);

void ui_button(Button b)
{
    if (sw_on()) {
        int sel = S.sel;
        sw_button(b);
        if (S.sel != sel) S.sound = 1;
        return;
    }
    if (S.launching >= 0) return;
    int view = S.view, sel = S.sel, fsel = S.fsel, sort = S.sort;
    int options = S.options, osel = S.osel, dark = S.dark;
    ui_button_inner(b);
    if (S.view != view || S.sel != sel || S.fsel != fsel || S.sort != sort || S.launching >= 0 ||
        S.options != options || S.osel != osel || S.dark != dark)
        S.sound = 1;
}

int ui_take_sound(void)
{
    int s = S.sound;
    S.sound = 0;
    return s;
}

int ui_take_action(void)
{
    int a = S.action;
    S.action = UI_NONE;
    return a;
}

void ui_set_dark(int dark)
{
    P = &PALETTES[dark ? 1 : 0];
    S.dark = dark ? 1 : 0;
    if (!S.bg) return; // before ui_init: build_bg picks the palette up
    img_free(S.bg);
    build_bg();
    tiles_free(); // sprites carry the palette's shadow; the footer strip its background
    S.scene_dirty = 1;
}

int ui_dark(void) { return S.dark; }

void ui_set_boot(int on) { S.boot = on; }

void ui_set_status(int battery, int charging, int wifi)
{
    S.battery = battery;
    S.charging = charging;
    S.wifi = wifi;
}

static void options_button(Button b)
{
    if (b == BTN_UP && S.osel > 0) S.osel--;
    if (b == BTN_DOWN && S.osel < O_COUNT - 1) S.osel++;
    if (b == BTN_B || b == BTN_START) S.options = 0;
    int toggle = b == BTN_A || ((b == BTN_LEFT || b == BTN_RIGHT) && S.osel == O_DARK);
    if (!toggle) return;
    if (S.osel == O_DARK) {
        ui_set_dark(!S.dark);
        S.action = UI_THEME;
    } else {
        S.options = 0;
        S.action = S.osel == O_ONION ? UI_EXIT : S.boot ? UI_BOOT_ONION : UI_BOOT_SHELF;
    }
}

static void ui_button_inner(Button b)
{
    S.scene_dirty = 1;
    if (S.options) { options_button(b); return; }
    if (b == BTN_MENU) { S.action = UI_EXIT; return; }
    if (b == BTN_START) { S.options = 1; S.osel = 0; return; }
    if (S.view == V_HOME) {
        int n = home_count();
        if (b == BTN_LEFT && S.sel > 0) home_select(S.sel - 1);
        if (b == BTN_RIGHT && S.sel < n - 1) home_select(S.sel + 1);
        if (b == BTN_L) home_select(0);
        if (b == BTN_R) home_select(S.lib->nrecent < n ? S.lib->nrecent : n - 1);
        if (b == BTN_A) {
            if (S.sel < S.lib->nrecent) start_launch(S.lib->recent[S.sel]);
            else folder_open(S.sel - S.lib->nrecent);
        }
    } else {
        int i = S.fsel, rows = S.grid->rows;
        if (b == BTN_LEFT && i - rows >= 0) folder_select(i - rows);
        if (b == BTN_RIGHT) {
            if (i + rows < S.nfg) folder_select(i + rows);
            else if ((i / rows + 1) * rows < S.nfg) folder_select(S.nfg - 1); // ragged last column
        }
        if (b == BTN_UP && i % rows > 0) folder_select(i - 1);
        if (b == BTN_DOWN && i % rows < rows - 1 && i + 1 < S.nfg) folder_select(i + 1);
        if (b == BTN_L) folder_select(0);
        if (b == BTN_R && S.nfg) folder_select(S.nfg - 1);
        if (b == BTN_Y) { S.sort = (S.sort + 1) % 3; int f = S.folder; folder_open(f); S.enter.x = 1; }
        if (b == BTN_A && S.nfg) start_launch(S.fgames[S.fsel]);
        if (b == BTN_B) { S.view = V_HOME; S.enter.x = 0; S.enter.v = 0; }
    }
}

static int spring_moving(const Spring *s) { return s->x != s->t || s->v != 0; }

// Whether the next update will change the scene (springs snap exactly when they settle).
static int scene_moving(void)
{
    if (spring_moving(&S.track)) return 1;
    for (int i = 0; i < home_count(); i++) if (spring_moving(&S.hscale[i])) return 1;
    if (S.view == V_FOLDER) {
        if (spring_moving(&S.ftrack)) return 1;
        for (int i = 0; i < S.nfg; i++) if (spring_moving(&S.fscale[i])) return 1;
    }
    return 0;
}

static void scene_step(Spring *s, float dt, float k, float damping)
{
    float before = s->x;
    spring_step(s, dt, k, damping);
    if (before != s->x) S.scene_dirty = 1;
}

void ui_update(float dt)
{
    S.t += dt;
    if (sw_on()) { sw_update(dt); return; }
    spring_step(&S.enter, dt, SNAPPY);
    scene_step(&S.track, dt, BOUNCE);
    for (int i = 0; i < home_count(); i++) scene_step(&S.hscale[i], dt, 400.f, 18.f);
    if (S.view == V_FOLDER) {
        scene_step(&S.ftrack, dt, BOUNCE);
        for (int i = 0; i < S.nfg; i++) scene_step(&S.fscale[i], dt, 400.f, 18.f);
    }
    if (S.launching >= 0) {
        S.launch_t += dt;
        spring_step(&S.launch_scale, dt, 260.f, 16.f);
        if (S.launch_t > 0.95f && !S.launch_ready) S.launch_ready = 1;
    }
}

int ui_take_launch(void)
{
    if (!S.launch_ready) return -1;
    int g = S.launching;
    S.launch_ready = 0;
    S.launching = -1;
    return g;
}

void ui_reset_home(void)
{
    S.scene_dirty = 1;
    tiles_free();
    // Back on the home row with the launched game first (simulator keeps running).
    S.view = V_HOME;
    for (int i = 0; i < home_count(); i++) S.hscale[i].x = S.hscale[i].t = 1;
    home_select(0);
    S.track.x = S.track.t;
    S.enter.x = 0;
}

/* ---------- drawing ---------- */

static uint32_t lerp_rgb(uint32_t a, uint32_t b, float t)
{
    int r = ((a >> 16) & 255) + (int)((((int)(b >> 16) & 255) - ((int)(a >> 16) & 255)) * t);
    int g = ((a >> 8) & 255) + (int)((((int)(b >> 8) & 255) - ((int)(a >> 8) & 255)) * t);
    int bl = (a & 255) + (int)((((int)b & 255) - ((int)a & 255)) * t);
    return rgb((uint32_t)(r << 16 | g << 8 | bl));
}

static void text_center(Image *c, Font *f, const char *s, int cx, int y, uint32_t col, int max_w)
{
    char buf[256];
    text_fit(f, s, max_w, buf, sizeof buf);
    text_draw(c, f, buf, cx - text_width(f, buf) / 2, y, col);
}

static void draw_placeholder(Image *c, const Game *g, float x, float y, float w)
{
    const System *s = &S.lib->sys[g->sys];
    gfx_fill_rrect(c, x, y, w, w, w * 0.085f, rgb(s->color));
    gfx_fill_rrect(c, x + w * .08f, y + w * .08f, w * .84f, w * .84f, w * .06f, argb(40, 255, 255, 255));
    Font *f = w > 120 ? S.label : S.small;
    text_center(c, f, s->label, (int)(x + w / 2), (int)(y + w * .2f), argb(200, 255, 255, 255), (int)(w * .8f));
    // Title, wrapped onto up to three lines.
    char words[128];
    snprintf(words, sizeof words, "%s", g->title);
    char line[256] = "";
    int ly = (int)(y + w * .42f), lines = 0, lh = font_height(f);
    char *save;
    for (char *wd = strtok_r(words, " ", &save); wd && lines < 3; wd = strtok_r(NULL, " ", &save)) {
        char trial[256];
        snprintf(trial, sizeof trial, "%s%s%s", line, *line ? " " : "", wd);
        if (*line && text_width(f, trial) > w * .84f) {
            text_center(c, f, line, (int)(x + w / 2), ly, rgb(0xffffff), (int)(w * .84f));
            ly += lh; lines++;
            snprintf(line, sizeof line, "%s", wd);
        } else snprintf(line, sizeof line, "%s", trial);
    }
    if (*line && lines < 3) text_center(c, f, line, (int)(x + w / 2), ly, rgb(0xffffff), (int)(w * .84f));
}

static void draw_game_raw(Image *c, int gi, float cx, float cy, float w, int big)
{
    float x = cx - w / 2, y = cy - w / 2;
    gfx_fill_rrect(c, x + 1, y + 4, w - 2, w - 1, w * .085f, argb(P->shadow_a, 20, 30, 60));
    const Image *ic = icon_get(gi, big);
    if (ic) gfx_blit_scaled(c, ic, x, y, w, w, 255);
    else draw_placeholder(c, &S.lib->games[gi], x, y, w);
}

static void draw_folder_raw(Image *c, const Folder *f, float cx, float cy, float w)
{
    float x = cx - w / 2, y = cy - w / 2, pad = w * .085f, gap = w * .036f, foot = w * .21f;
    gfx_fill_rrect(c, x + 1, y + 4, w - 2, w - 1, w * .085f, argb(P->shadow_a, 20, 30, 60));
    gfx_fill_rrect(c, x, y, w, w, w * .085f, rgb(f->color));
    float cell = (w - 2 * pad - gap) / 2, cellh = (w - pad - foot - gap) / 2;
    if (cellh < cell) cell = cellh;
    float gx = x + (w - (2 * cell + gap)) / 2;
    for (int k = 0; k < 4; k++) {
        float mx = gx + (k % 2) * (cell + gap), my = y + pad + (k / 2) * (cell + gap);
        const Image *ic = k < f->ngames ? icon_get(f->games[k], 0) : NULL;
        if (ic) gfx_blit_scaled(c, ic, mx, my, cell, cell, 255);
        else gfx_fill_rrect(c, mx, my, cell, cell, cell * .12f, argb(60, 255, 255, 255));
    }
    Font *lf = w > 120 ? S.label : S.small;
    text_center(c, lf, f->label, (int)cx, (int)(y + w - foot + (foot - font_height(lf)) / 2), rgb(0xffffff), (int)(w - 12));
}

// A bounded working set of reusable sprites, not one surface per ROM.
// Normal/selected sizes are rasterized once. In-between animation frames scale
// the normal sprite instead of rebuilding text, shadows and rounded geometry.
#define TILE_CACHE_CAP 24
#define TILE_PAD 8
typedef struct {
    int kind, id, size, big;
    const Image *sources[4];
    Image *image;
    unsigned used;
} TileSprite;
static TileSprite tiles[TILE_CACHE_CAP];
static Image *footer_img;      // see draw_footer
static char footer_key[160];
static Image *panel_base, *panel_img; // see draw_detail
static int panel_game = -1, panel_art;
static unsigned tile_clock;
// Selection ring masks, rasterized once per tile size (home row, each folder grid) at the
// selected size and 9-sliced to whatever size the zoom spring is at.
typedef struct { uint8_t *outer, *inner; int size, pad, extent, corner; } CursorMask;
static CursorMask cursors[3];

static void tiles_free(void)
{
    for (int i = 0; i < TILE_CACHE_CAP; i++) {
        img_free(tiles[i].image);
        memset(&tiles[i], 0, sizeof tiles[i]);
    }
    img_free(footer_img); footer_img = NULL; footer_key[0] = 0;
    img_free(panel_base); img_free(panel_img); panel_base = panel_img = NULL; panel_game = -1;
    for (int k = 0; k < 3; k++) {
        free(cursors[k].outer);
        free(cursors[k].inner);
        memset(&cursors[k], 0, sizeof cursors[k]);
    }
    tile_clock = 0;
}

static const Image *tile_sprite(int kind, int id, int size, int big)
{
    const Image *sources[4] = {0};
    if (kind == 0) sources[0] = icon_get(id, big);
    else {
        const Folder *f = &S.folders[id];
        for (int k = 0; k < 4 && k < f->ngames; k++)
            sources[k] = icon_get(f->games[k], 0);
    }
    TileSprite *slot = NULL;
    for (int i = 0; i < TILE_CACHE_CAP; i++) {
        TileSprite *t = &tiles[i];
        if (t->image && t->kind == kind && t->id == id &&
            t->size == size && t->big == big) {
            if (!memcmp(t->sources, sources, sizeof sources)) {
                t->used = ++tile_clock;
                return t->image;
            }
            slot = t;
            break;
        }
        if (!slot || !t->image || (slot->image && t->used < slot->used)) slot = t;
    }
    img_free(slot->image);
    slot->image = img_new(size + 2 * TILE_PAD, size + 2 * TILE_PAD);
    if (!slot->image) return NULL;
    slot->kind = kind; slot->id = id; slot->size = size; slot->big = big;
    slot->used = ++tile_clock;
    memcpy(slot->sources, sources, sizeof sources);
    float center = TILE_PAD + size / 2.f;
    if (kind == 0) draw_game_raw(slot->image, id, center, center, size, big);
    else draw_folder_raw(slot->image, &S.folders[id], center, center, size);
    img_find_spans(slot->image);
    return slot->image;
}

// Size of a `base` tile while selected: home tiles zoom 8%, folder tiles 10%.
static int zoomed(int base) { return (int)lroundf(base * (base == TILE ? 1.08f : 1.1f)); }

static void draw_tile(Image *c, int kind, int id, float cx, float cy, float w, int base)
{
    int big = base > ICON_SMALL, selected = zoomed(base);
    int size = fabsf(w - selected) < .6f ? selected : base;
    const Image *sprite = tile_sprite(kind, id, size, big);
    if (!sprite) {
        if (kind == 0) draw_game_raw(c, id, cx, cy, w, big);
        else draw_folder_raw(c, &S.folders[id], cx, cy, w);
        return;
    }
    // Snap settled geometry to pixels; scale only during motion/zoom.
    if (fabsf(w - size) < .6f) {
        gfx_blit(c, sprite, (int)lroundf(cx - size / 2.f) - TILE_PAD,
                 (int)lroundf(cy - size / 2.f) - TILE_PAD, 255);
    } else {
        float pad = TILE_PAD * w / size, extent = w + 2 * pad;
        gfx_blit_scaled(c, sprite, cx - w / 2 - pad, cy - w / 2 - pad,
                        extent, extent, 255);
    }
}

static void draw_game(Image *c, int gi, float cx, float cy, float w, int base)
{
    draw_tile(c, 0, gi, cx, cy, w, base);
}

static void draw_folder(Image *c, const Folder *f, float cx, float cy, float w)
{
    draw_tile(c, 1, (int)(f - S.folders), cx, cy, w, TILE);
}

// Game count bubble on a folder's corner; drawn after the cursor so it stays readable.
static void draw_folder_badge(Image *c, const Folder *f, float cx, float cy, float w)
{
    float x = cx - w / 2, y = cy - w / 2;
    char n[16];
    snprintf(n, sizeof n, "%d", f->ngames);
    int tw = text_width(S.small, n), bw = tw + 16, bh = font_height(S.small) + 4;
    gfx_fill_rrect(c, x + w - bw + 10, y - 10, bw, bh, bh / 2.f, rgb(P->chip));
    gfx_stroke_rrect(c, x + w - bw + 10, y - 10, bw, bh, bh / 2.f, 1, rgb(P->line));
    text_draw(c, S.small, n, (int)(x + w - bw + 18), (int)(y - 8), rgb(C_INK));
}

static void draw_cursor(Image *c, float cx, float cy, float w, int base)
{
    float p = .5f + .5f * sinf(S.t * 5.7f);
    int size = zoomed(base);
    CursorMask *m = NULL;
    for (int k = 0; k < 3 && !m; k++)
        if (!cursors[k].outer || cursors[k].size == size) m = &cursors[k];
    if (!m) { // a fourth size: reuse the first slot
        m = &cursors[0];
        free(m->outer); free(m->inner);
        memset(m, 0, sizeof *m);
    }
    if (!m->outer) {
        float o = 7.f * size / TILE + 1;
        int pad = (int)ceilf(o) + 2, extent = size + 2 * pad;
        Image *outer = img_new(extent, extent), *inner = img_new(extent, extent);
        uint8_t *om = malloc((size_t)extent * extent), *im = malloc((size_t)extent * extent);
        if (!outer || !inner || !om || !im) {
            img_free(outer); img_free(inner); free(om); free(im);
            return;
        }
        float offset = pad - o;
        gfx_stroke_rrect(outer, offset, offset, size + 2 * o, size + 2 * o,
                         size * .085f + o, 4.5f, rgb(0xffffff));
        gfx_stroke_rrect(inner, offset + 4, offset + 4, size + 2 * o - 8,
                         size + 2 * o - 8, size * .085f + o - 4, 2.5f, rgb(0xffffff));
        for (int i = 0; i < extent * extent; i++) {
            om[i] = outer->px[i] >> 24;
            im[i] = inner->px[i] >> 24;
        }
        img_free(outer); img_free(inner);
        *m = (CursorMask){om, im, size, pad, extent, pad + (int)ceilf(size * .085f) + 2};
    }
    int ring = (int)lroundf(w), extent = ring + 2 * m->pad;
    int x = (int)lroundf(cx - ring / 2.f) - m->pad;
    int y = (int)lroundf(cy - ring / 2.f) - m->pad;
    gfx_mask_a8_9slice(c, m->outer, m->extent, m->extent, m->corner, x, y, extent, extent,
                       lerp_rgb(C_SEL, C_SEL2, p));
    gfx_mask_a8_9slice(c, m->inner, m->extent, m->extent, m->corner, x, y, extent, extent,
                       argb(230, 255, 255, 255));
}

// Status glyphs are coverage masks, 4x4 supersampled once from a shape test.
#define WIFI_W 23
#define WIFI_H 17
#define BOLT_W 9
#define BOLT_H 13
static uint8_t wifi_mask[4][WIFI_W * WIFI_H], bolt_mask[BOLT_W * BOLT_H];

// Wi-Fi fan: part 0 is the dot, 1..3 the arcs, within 45° of vertical.
static int in_wifi(float x, float y, int part)
{
    float dx = x - WIFI_W / 2.f, dy = WIFI_H - .5f - y, r = sqrtf(dx * dx + dy * dy);
    if (dy < 0 || fabsf(dx) > dy + .5f) return 0;
    static const float BAND[4][2] = {{0, 2.6f}, {5, 7.6f}, {10, 12.6f}, {15, 17.6f}};
    return r >= BAND[part][0] && r < BAND[part][1];
}

static int in_bolt(float x, float y)
{
    static const float PT[][2] = {{5.5f, 0}, {0.5f, 7.5f}, {4.2f, 7.5f}, {3.2f, 13}, {8.5f, 5.2f}, {4.8f, 5.2f}};
    int n = sizeof PT / sizeof PT[0], in = 0;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if ((PT[i][1] > y) != (PT[j][1] > y) &&
            x < (PT[j][0] - PT[i][0]) * (y - PT[i][1]) / (PT[j][1] - PT[i][1]) + PT[i][0])
            in = !in;
    return in;
}

static void build_status_masks(void)
{
    static int built;
    if (built) return;
    built = 1;
    for (int part = 0; part < 4; part++)
        for (int y = 0; y < WIFI_H; y++)
            for (int x = 0; x < WIFI_W; x++) {
                int n = 0;
                for (int s = 0; s < 16; s++) n += in_wifi(x + (s % 4 + .5f) / 4, y + (s / 4 + .5f) / 4, part);
                wifi_mask[part][y * WIFI_W + x] = (uint8_t)(n * 255 / 16);
            }
    for (int y = 0; y < BOLT_H; y++)
        for (int x = 0; x < BOLT_W; x++) {
            int n = 0;
            for (int s = 0; s < 16; s++) n += in_bolt(x + (s % 4 + .5f) / 4, y + (s / 4 + .5f) / 4);
            bolt_mask[y * BOLT_W + x] = (uint8_t)(n * 255 / 16);
        }
}

// Right-aligned at `right`; returns the left edge.
static int draw_battery(Image *c, int right, int y)
{
    const int bw = 27, bh = 14;
    int x = right - bw - 3;
    uint32_t ink = rgb(C_SUB);
    uint32_t fill = S.charging ? rgb(0x30a46c) : S.battery <= 15 ? rgb(0xe5484d) : ink;
    gfx_stroke_rrect(c, x, y, bw, bh, 4, 1.6f, ink);
    gfx_fill_rrect(c, x + bw + .5f, y + 4, 2.5f, bh - 8, 1, ink);
    float w = (bw - 6) * (S.battery < 0 ? 0 : S.battery > 100 ? 100 : S.battery) / 100.f;
    if (w > .5f) gfx_fill_rrect(c, x + 3, y + 3, w < 2 ? 2 : w, bh - 6, 1.5f, fill);
    if (S.charging) gfx_mask_a8(c, bolt_mask, BOLT_W, BOLT_H, x - BOLT_W - 5, y, fill);
    char pct[8];
    snprintf(pct, sizeof pct, "%d%%", S.battery);
    int tx = x - (S.charging ? BOLT_W + 9 : 6) - text_width(S.small, pct);
    text_draw(c, S.small, pct, tx, y - 4, rgb(C_SUB));
    return tx;
}

static void draw_status(Image *c)
{
    char buf[64];
    time_t now = time(NULL);
    strftime(buf, sizeof buf, "%a %d %b · %H:%M", localtime(&now));
    text_draw(c, S.small, buf, 16, 7, rgb(C_SUB));
    build_status_masks();
    int x = SCREEN_W - 16, y = 11;
    if (S.battery >= 0) x = draw_battery(c, x, y) - 14;
    if (S.wifi >= 0) { // bars lit up to the signal; all dim while not connected
        for (int part = 0; part < 4; part++) {
            int lit = S.wifi > 0 && (part == 0 || part <= S.wifi);
            gfx_mask_a8(c, wifi_mask[part], WIFI_W, WIFI_H, x - WIFI_W, y - 3,
                        lit ? rgb(C_SUB) : rgb(P->line));
        }
    }
}

// Hints name the Miyoo's printed labels: A B X Y, L1 R1, SELECT START MENU.
// Face buttons are circles; shoulder and system buttons are pills.
static void draw_footer_raw(Image *c, int top, const char *const *hints, int n)
{
    gfx_fill_rect(c, 0, top, SCREEN_W, FOOTER_H, argb(P->footer_a, P->footer >> 16, P->footer >> 8 & 255, P->footer & 255));
    gfx_fill_rect(c, 0, top, SCREEN_W, 1, rgb(P->line));
    int th = font_height(S.small), bh = th + 4;
    int x = 14, y = top + (FOOTER_H - bh) / 2;
    for (int i = 0; i < n; i += 2) {
        int bw = hints[i][1] ? text_width(S.small, hints[i]) + 14 : bh;
        gfx_fill_rrect(c, x, y, bw, bh, bh / 2.f, rgb(P->key));
        text_center(c, S.small, hints[i], x + bw / 2, y + 2, rgb(0xffffff), bw);
        x += bw + 7;
        x += text_draw(c, S.small, hints[i + 1], x, y + 2, rgb(C_SUB)) + 18;
    }
}

// The footer sits on the static background, so keep it pre-composited as an opaque strip
// per hint set and copy it in rather than re-blending text on every rebuilt frame.
static void draw_footer(Image *c, const char *const *hints, int n)
{
    char key[sizeof footer_key] = "";
    for (int i = 0; i < n; i++) {
        strncat(key, hints[i], sizeof key - strlen(key) - 2);
        strcat(key, "\x1f");
    }
    if (!footer_img || strcmp(key, footer_key)) {
        if (!footer_img) footer_img = img_new(SCREEN_W, FOOTER_H);
        if (!footer_img) { draw_footer_raw(c, SCREEN_H - FOOTER_H, hints, n); return; }
        memcpy(footer_img->px, S.bg->px + (SCREEN_H - FOOTER_H) * SCREEN_W,
               (size_t)SCREEN_W * FOOTER_H * sizeof *footer_img->px);
        draw_footer_raw(footer_img, 0, hints, n);
        img_find_spans(footer_img);
        snprintf(footer_key, sizeof footer_key, "%s", key);
    }
    gfx_blit(c, footer_img, 0, SCREEN_H - FOOTER_H, 255);
}

static void draw_home(Image *c)
{
    Library *L = S.lib;
    int n = home_count(), inlib = S.sel >= L->nrecent;
    char meta[160];
    if (!inlib) {
        const Game *g = &L->games[L->recent[S.sel]];
        snprintf(meta, sizeof meta, "%s · Recently played", L->sys[g->sys].name);
        text_center(c, S.title, g->title, SCREEN_W / 2, 44, rgb(C_INK), SCREEN_W - 60);
    } else {
        const Folder *f = &S.folders[S.sel - L->nrecent];
        snprintf(meta, sizeof meta, "Folder · %d game%s", f->ngames, f->ngames == 1 ? "" : "s");
        text_center(c, S.title, f->label, SCREEN_W / 2, 44, rgb(C_INK), SCREEN_W - 60);
    }
    text_center(c, S.body, meta, SCREEN_W / 2, 86, rgb(C_SUB), SCREEN_W - 60);
    text_draw(c, S.small, L->nrecent && !inlib ? "RECENT" : "LIBRARY", 18, 114, rgb(C_SUB));

    float tx = S.track.x, cy = ROW_Y + TILE / 2.f;
    if (L->nrecent) {
        float dx = tx + item_x(L->nrecent - 1) + TILE + (STEP - TILE + DIVIDER_GAP) / 2.f - 2;
        if (dx > -10 && dx < SCREEN_W + 10) gfx_fill_rrect(c, dx, cy - 60, 4, 120, 2, rgb(P->line));
    }
    for (int pass = 0; pass < 2; pass++) // selected tile last so it sits on top
        for (int i = 0; i < n; i++) {
            if ((i == S.sel) != pass) continue;
            float cx = tx + item_x(i) + TILE / 2.f, w = TILE * S.hscale[i].x;
            if (cx + w < -20 || cx - w > SCREEN_W + 20) continue;
            if (i < L->nrecent) draw_game(c, L->recent[i], cx, cy, w, TILE);
            else draw_folder(c, &S.folders[i - L->nrecent], cx, cy, w);
            if (i >= L->nrecent) draw_folder_badge(c, &S.folders[i - L->nrecent], cx, cy, w);
        }

    // Position dots: round for Recent, square for Library.
    float dw = 15, x0 = SCREEN_W / 2.f - (n - 1) * dw / 2;
    for (int i = 0; i < n; i++) {
        float s = i == S.sel ? 12 : 8, x = x0 + i * dw - s / 2, y = 362 - s / 2;
        gfx_fill_rrect(c, x, y, s, s, i < L->nrecent ? s / 2 : 2.5f, rgb(i == S.sel ? C_SEL : P->dim_dot));
    }
    const char *const H[] = {"A", inlib ? "Open" : "Play", "L1", "Recent", "R1", "Library", "START", "Options"};
    draw_footer(c, H, 8);
}

// Splits s at spaces into up to `max` lines no wider than max_w; the last line takes the
// rest, cut with an ellipsis. Returns the line count.
static int wrap_text(Font *f, const char *s, int max_w, char out[][128], int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ') s++;
        int fit = 0;
        for (int i = 1; n < max - 1; i++) { // longest run of whole words that fits
            if (s[i] != ' ' && s[i]) continue;
            char t[128];
            snprintf(t, sizeof t, "%.*s", i, s);
            if (text_width(f, t) > max_w) break;
            fit = i;
            if (!s[i]) break;
        }
        if (!fit) { text_fit(f, s, max_w, out[n++], 128); break; }
        snprintf(out[n++], 128, "%.*s", fit, s);
        s += fit;
    }
    return n;
}

static void format_play_time(const Game *g, char *out, int n)
{
    int m = g->play_time / 60;
    if (!g->play_count) snprintf(out, n, "None yet");
    else if (m < 1) snprintf(out, n, "< 1 m");
    else if (m < 60) snprintf(out, n, "%d m", m);
    else snprintf(out, n, "%d h %d m", m / 60, m % 60);
}

static time_t day_start(time_t t)
{
    struct tm d;
    localtime_r(&t, &d);
    d.tm_hour = d.tm_min = d.tm_sec = 0;
    d.tm_isdst = -1;
    return mktime(&d);
}

static void format_last_played(const Game *g, char *out, int n)
{
    // Without network time the clock may start near 1970, and so may Onion's records;
    // only trust dates after 2020. Otherwise fall back to the recents list.
    const time_t sane = 1577836800;
    time_t now = time(NULL), t = (time_t)g->last_played;
    if (t > sane && now > sane) {
        long days = lround(difftime(day_start(now), day_start(t)) / 86400);
        struct tm d;
        localtime_r(&t, &d);
        static const char *const MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                          "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        if (days <= 0) snprintf(out, n, "Today");
        else if (days == 1) snprintf(out, n, "Yesterday");
        else if (days < 7) snprintf(out, n, "%ld days ago", days);
        else if (days < 300) snprintf(out, n, "%d %s", d.tm_mday, MON[d.tm_mon]);
        else snprintf(out, n, "%s %d", MON[d.tm_mon], d.tm_year + 1900);
    } else if (g->recent == 0) snprintf(out, n, "Most recent");
    else if (g->recent > 0) snprintf(out, n, "Recently");
    else snprintf(out, n, g->play_count ? "A while ago" : "Never");
}

// The selected game's art, title, system and play stats. The panel is opaque over the
// background, so it's kept as one pre-composited image per selection and copied in; the
// empty card is rasterized once. Its left margin also covers grid tiles that have
// scrolled under it.
static void draw_detail(Image *c, int gi)
{
    const int x0 = G_EDGE, y0 = PANEL_Y - 8;
    const int iw = PANEL_X + PANEL_W + 8 - x0, ih = PANEL_H + 16;
    const float px = PANEL_X - x0, py = PANEL_Y - y0;
    const Image *icon = icon_get(gi, 1);
    if (!panel_base) {
        panel_base = img_new(iw, ih);
        if (!panel_base) return;
        for (int y = 0; y < ih; y++)
            memcpy(panel_base->px + y * iw, S.bg->px + (y0 + y) * SCREEN_W + x0, iw * sizeof *panel_base->px);
        gfx_fill_rrect(panel_base, px + 1, py + 3, PANEL_W - 2, PANEL_H - 1, 16, argb(P->shadow_a / 2, 20, 30, 60));
        gfx_fill_rrect(panel_base, px, py, PANEL_W, PANEL_H, 16, rgb(P->panel));
    }
    if (!panel_img || panel_game != gi || panel_art != (icon != NULL)) {
        if (!panel_img) panel_img = img_new(iw, ih);
        if (!panel_img) return;
        Image *p = panel_img;
        memcpy(p->px, panel_base->px, (size_t)iw * ih * sizeof *p->px);
        const Game *g = &S.lib->games[gi];
        float ax = px + (PANEL_W - TILE) / 2.f, ay = py + 16;
        if (icon) gfx_blit(p, icon, (int)ax, (int)ay, 255); // already TILE square, rounded
        else draw_placeholder(p, g, ax, ay, TILE);

        int tx = (int)px + 18, tw = PANEL_W - 36, ty = (int)py + TILE + 30;
        char lines[2][128];
        int nl = wrap_text(S.label, g->title, tw, lines, 2);
        for (int i = 0; i < nl; i++, ty += font_height(S.label))
            text_draw(p, S.label, lines[i], tx, ty, rgb(C_INK));
        char buf[128];
        text_fit(S.small, S.lib->sys[g->sys].name, tw, buf, sizeof buf);
        text_draw(p, S.small, buf, tx, ty + 2, rgb(C_SUB));

        char play[32], last[32], sessions[16];
        format_play_time(g, play, sizeof play);
        format_last_played(g, last, sizeof last);
        snprintf(sessions, sizeof sessions, "%d", g->play_count);
        const char *const LABEL[4] = {"PLAY TIME", "LAST PLAYED", "SESSIONS", "FAVORITE"};
        const char *value[4] = {play, last, sessions, g->fav ? "Yes" : "No"};
        for (int i = 0; i < 4; i++) {
            int sx = tx + (i % 2) * (tw / 2 + 4), sy = (int)py + 282 + (i / 2) * 50;
            text_draw(p, S.small, LABEL[i], sx, sy, rgb(C_SUB));
            uint32_t ink = i == 3 && g->fav ? rgb(0xf0a020) : rgb(C_INK);
            text_fit(S.body, value[i], tw / 2 - 4, buf, sizeof buf);
            text_draw(p, S.body, buf, sx, sy + 21, ink);
        }
        img_find_spans(p);
        panel_game = gi;
        panel_art = icon != NULL;
    }
    gfx_blit(c, panel_img, x0, y0, 255);
}

static void draw_folder_view(Image *c)
{
    const Folder *f = &S.folders[S.folder];
    const Grid *g = S.grid;
    gfx_fill_rrect(c, 24, 36, 48, 48, 12, rgb(f->color));
    text_draw(c, S.label, f->label, 84, 34, rgb(C_INK));
    char n[64];
    snprintf(n, sizeof n, "%d game%s · %s", f->ngames, f->ngames == 1 ? "" : "s", SORTS[S.sort]);
    text_draw(c, S.small, n, 84, 60, rgb(C_SUB));
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < S.nfg; k++) {
            if ((k == S.fsel) != pass) continue;
            float cx = S.ftrack.x + (k / g->rows) * g->col + g->tile / 2.f, cy = G_Y + (k % g->rows) * g->row + g->tile / 2.f;
            float w = g->tile * S.fscale[k].x;
            if (cx + w / 2 < -8 || cx - w / 2 > G_EDGE) continue;
            draw_game(c, S.fgames[k], cx, cy, w, g->tile);
        }
    if (S.nfg) draw_detail(c, S.fgames[S.fsel]);
    else text_center(c, S.body, "Nothing here yet", SCREEN_W / 2, 220, rgb(C_SUB), SCREEN_W);
    static const char *const H[] = {"A", "Play", "B", "Back", "Y", "Sort", "L1", "First", "R1", "Last"};
    draw_footer(c, H, 10);
}

// Options sheet over the dimmed view; drawn into the retained scene since it's static.
static void draw_options(Image *c)
{
    gfx_fill_rect(c, 0, 0, SCREEN_W, SCREEN_H - FOOTER_H, argb(S.dark ? 150 : 110, 10, 12, 18));
    const int w = 452, row = 66, h = 62 + O_COUNT * row + 12;
    int x = (SCREEN_W - w) / 2, y = (SCREEN_H - FOOTER_H - h) / 2;
    gfx_fill_rrect(c, x + 2, y + 6, w - 4, h - 2, 18, argb(60, 0, 0, 0));
    gfx_fill_rrect(c, x, y, w, h, 18, rgb(P->panel));
    text_draw(c, S.label, "Options", x + 24, y + 20, rgb(C_INK));
    static const char *const TITLE[O_COUNT] = {"Dark mode", NULL, "Open Onion's menu"};
    const char *sub[O_COUNT] = {
        "Easier on the eyes at night",
        S.boot ? "Shelf comes back from Apps › Shelf" : "Start in Shelf and return to it after games",
        "Just this once; games still come back here",
    };
    for (int i = 0; i < O_COUNT; i++) {
        int ry = y + 60 + i * row, sel = i == S.osel;
        if (sel) gfx_fill_rrect(c, x + 10, ry, w - 20, row - 6, 12, rgb(C_SEL));
        uint32_t ink = sel ? rgb(0xffffff) : rgb(C_INK), sub_ink = sel ? argb(220, 255, 255, 255) : rgb(C_SUB);
        const char *title = TITLE[i] ? TITLE[i] : S.boot ? "Use Onion's menu instead" : "Make Shelf the home screen";
        text_draw(c, S.body, title, x + 24, ry + 7, ink);
        text_draw(c, S.small, sub[i], x + 24, ry + 32, sub_ink);
        if (i == O_DARK) { // switch
            float sw = 52, sh = 30, sx = x + w - 24 - sw, sy = ry + (row - 6 - sh) / 2;
            uint32_t track = S.dark ? (sel ? argb(90, 255, 255, 255) : rgb(C_SEL)) : (sel ? argb(60, 0, 0, 0) : rgb(P->line));
            gfx_fill_rrect(c, sx, sy, sw, sh, sh / 2, track);
            gfx_fill_rrect(c, sx + (S.dark ? sw - sh : 0) + 3, sy + 3, sh - 6, sh - 6, (sh - 6) / 2, rgb(0xffffff));
        }
    }
}

static void draw_launch(Image *c)
{
    float a = S.launch_t / .3f;
    if (a > 1) a = 1;
    gfx_fill_rect(c, 0, 0, SCREEN_W, SCREEN_H, argb((uint8_t)(a * 235), 20, 22, 30));
    draw_game(c, S.launching, SCREEN_W / 2.f, SCREEN_H / 2.f - 20, TILE * S.launch_scale.x, TILE);
    if (S.launch_t > .15f) {
        char cap[200];
        snprintf(cap, sizeof cap, "Starting %s…", S.lib->games[S.launching].title);
        text_center(c, S.label, cap, SCREEN_W / 2, SCREEN_H / 2 + 100, rgb(0xffffff), SCREEN_W - 48);
    }
}

void ui_draw(Image *c)
{
    if (sw_on()) { sw_draw(c); return; }
    float e = S.enter.x;
    // Compose straight into the final canvas except when a zoom needs a source layer.
    Image *composed = e > .995f ? c : S.layer;
    unsigned revision = icons_revision();
    if (S.scene_dirty || S.art_revision != revision) {
        // While springs move the scene changes every frame, so draw it straight into
        // the frame; retain it only once it settles (saves two 1.2 MB copies a frame).
        int retain = !scene_moving();
        Image *target = retain ? S.scene : composed;
        memcpy(target->px, S.bg->px, (size_t)SCREEN_W * SCREEN_H * sizeof *c->px);
        if (S.view == V_HOME) draw_home(target); else draw_folder_view(target);
        if (S.options) {
            draw_options(target);
            static const char *const H[] = {"A", "Select", "B", "Close"};
            draw_footer(target, H, 4);
        }
        S.scene_dirty = !retain;
        S.scene_builds++;
        S.art_revision = revision;
        if (retain) memcpy(composed->px, S.scene->px, (size_t)SCREEN_W * SCREEN_H * sizeof *c->px);
    } else {
        memcpy(composed->px, S.scene->px, (size_t)SCREEN_W * SCREEN_H * sizeof *c->px);
    }
    if (S.options) {
        // The sheet covers the cursor; nothing animates under it.
    } else if (S.view == V_HOME) {
        float cx = S.track.x + item_x(S.sel) + TILE / 2.f;
        float w = TILE * S.hscale[S.sel].x;
        draw_cursor(composed, cx, ROW_Y + TILE / 2.f, w, TILE);
        if (S.sel >= S.lib->nrecent)
            draw_folder_badge(composed, &S.folders[S.sel - S.lib->nrecent], cx, ROW_Y + TILE / 2.f, w);
    } else if (S.nfg) {
        const Grid *g = S.grid;
        float cx = S.ftrack.x + (S.fsel / g->rows) * g->col + g->tile / 2.f;
        float cy = G_Y + (S.fsel % g->rows) * g->row + g->tile / 2.f;
        draw_cursor(composed, cx, cy, g->tile * S.fscale[S.fsel].x, g->tile);
    }
    if (e <= .995f) {
        float k = .92f + .08f * e, w = SCREEN_W * k, h = SCREEN_H * k;
        float a = e < 0 ? 0 : (e > 1 ? 1 : e);
        gfx_fade_scaled_nearest(c, S.bg, S.layer, (SCREEN_W - w) / 2, (SCREEN_H - h) / 2, w, h, (uint8_t)(a * 255));
    }
    draw_status(c);
    if (S.launching >= 0) draw_launch(c);
}

/* ---------- game switcher ---------- */

// Cards: a 4:3 screenshot in a white frame. The selected card is full size, the others
// SW_SMALL of it.
#define SW_SHOT_W 248
#define SW_SHOT_H 186
#define SW_FRAME 8
#define SW_W (SW_SHOT_W + 2 * SW_FRAME)
#define SW_H (SW_SHOT_H + 2 * SW_FRAME)
#define SW_STEP 280
#define SW_CY 250
#define SW_SMALL .86f
#define SW_SPRITE_PAD 8

static struct {
    int on, overlay, running;  // switcher view; over a paused game; recents[0] is that game
    const Image *(*card)(int), *(*full)(int);
    void (*select)(int);
    unsigned (*revision)(void);
    Image *sprites[MAX_RECENT];
    const Image *sprite_src[MAX_RECENT], *sprite_icon[MAX_RECENT];
    Image *home_bg;           // Shelf's own background: B fades to it, as Shelf starts on it
    Image *backdrop, *old_backdrop;
    int backdrop_for;         // recent the backdrop was made from, -1 none
    const Image *backdrop_src;
    float fade;               // 0..1 cross-fade from old_backdrop
    Spring zoom;              // 0: selected card fills the screen, 1: in the row
    int closing, close_to;    // UI_SW_* being animated to, and the recent
    float home_t;             // B: fade-out progress
    int fired;                // the closing action was handed out; hold the last frame
    int saving;               // SW_TOAST_*
} W;

void ui_switch_hooks(const Image *(*card)(int), const Image *(*full)(int), void (*select)(int),
                     unsigned (*revision)(void))
{
    W.card = card; W.full = full; W.select = select; W.revision = revision;
}

void ui_switch_saving(int state) { W.saving = state; }

int ui_switch_target(void) { return W.close_to; }

static int sw_on(void) { return W.on; }

static void sw_select(int i)
{
    S.sel = i;
    S.track.t = SCREEN_W / 2.f - i * SW_STEP;
    for (int k = 0; k < S.lib->nrecent; k++) S.hscale[k].t = k == i ? 1.f : SW_SMALL;
    if (W.select) W.select(i);
}

int ui_init_switcher(Library *lib, const char *font_dir, int overlay, int running)
{
    if (ui_init(lib, font_dir)) return -1;
    W.on = 1;
    W.overlay = overlay;
    W.running = running;
    W.backdrop_for = -1;
    W.home_bg = S.bg;     // in the user's palette
    P = &PALETTES[1];     // the switcher is always dark: it sits over a dimmed game
    S.bg = NULL;
    build_bg();
    for (int k = 0; k < lib->nrecent; k++) S.hscale[k].x = S.hscale[k].t = SW_SMALL;
    sw_select(0);
    S.track.x = S.track.t;
    S.hscale[0].x = 1;
    // Over a game, open from its frame filling the screen.
    W.zoom.x = overlay && running ? 0 : 1;
    W.zoom.t = 1;
    return 0;
}

static void sw_free(void)
{
    for (int i = 0; i < MAX_RECENT; i++) img_free(W.sprites[i]);
    img_free(W.home_bg); img_free(W.backdrop); img_free(W.old_backdrop);
    memset(&W, 0, sizeof W);
}

static void sw_close(int action, int i)
{
    W.closing = action;
    W.close_to = i;
    W.home_t = 0;
    if (action != UI_SW_HOME) W.zoom.t = 0; // the card grows back to full screen
    else W.zoom.x = W.zoom.t = 1;           // the row fades to Shelf
}

static void sw_button(Button b)
{
    if (W.closing) return;
    int n = S.lib->nrecent;
    if (b == BTN_LEFT && S.sel > 0) sw_select(S.sel - 1);
    if (b == BTN_RIGHT && S.sel < n - 1) sw_select(S.sel + 1);
    if (b == BTN_L) sw_select(0);
    if (b == BTN_R && n) sw_select(n - 1);
    if (b == BTN_A && n) sw_close(S.sel == 0 && W.running ? UI_SW_RESUME : UI_SW_PLAY, S.sel);
    if (b == BTN_MENU) {
        if (W.running) { sw_select(0); sw_close(UI_SW_RESUME, 0); }
        else sw_close(UI_SW_HOME, -1);
    }
    if (b == BTN_B) sw_close(UI_SW_HOME, -1);
    if (b == BTN_QUIT) { // power off: get back to the game at once, as Onion does
        if (W.running) { S.action = UI_SW_RESUME; W.close_to = 0; }
        else S.action = UI_SW_HOME;
    }
}

static void sw_update(float dt)
{
    spring_step(&W.zoom, dt, SNAPPY);
    spring_step(&S.track, dt, BOUNCE);
    for (int i = 0; i < S.lib->nrecent; i++) spring_step(&S.hscale[i], dt, 400.f, 22.f);
    if (W.fade < 1) { W.fade += dt / .22f; if (W.fade > 1) W.fade = 1; }
    if (W.closing == UI_SW_HOME) {
        W.home_t += dt / .25f;
        if (W.home_t >= 1 && !W.fired) { S.action = UI_SW_HOME; W.fired = 1; }
    } else if (W.closing && W.zoom.x < .004f && !W.fired) {
        S.action = W.closing;
        W.fired = 1;
    }
}

// One box-blur pass along a row or column of opaque pixels, edges clamped.
static void blur_line(uint32_t *buf, uint32_t *tmp, int n, int stride, int r)
{
    for (int i = 0; i < n; i++) tmp[i] = buf[i * stride];
    int win = 2 * r + 1, sum[3] = {0};
    for (int i = -r; i <= r; i++) {
        uint32_t p = tmp[i < 0 ? 0 : (i >= n ? n - 1 : i)];
        for (int c = 0; c < 3; c++) sum[c] += (p >> (c * 8)) & 255;
    }
    for (int i = 0; i < n; i++) {
        buf[i * stride] = 0xff000000u | (sum[0] / win) | ((sum[1] / win) << 8) | ((sum[2] / win) << 16);
        int out = i - r, in = i + r + 1;
        uint32_t po = tmp[out < 0 ? 0 : out], pi = tmp[in >= n ? n - 1 : in];
        for (int c = 0; c < 3; c++) sum[c] += (int)((pi >> (c * 8)) & 255) - (int)((po >> (c * 8)) & 255);
    }
}

static void box_blur(Image *img, int r)
{
    int n = img->w > img->h ? img->w : img->h;
    uint32_t *tmp = malloc((size_t)n * 4);
    if (!tmp) return;
    for (int pass = 0; pass < 3; pass++) {
        for (int y = 0; y < img->h; y++) blur_line(img->px + y * img->w, tmp, img->w, 1, r);
        for (int x = 0; x < img->w; x++) blur_line(img->px + x, tmp, img->h, img->w, r);
    }
    free(tmp);
}

// The selected game's screenshot, blurred and dimmed. Blurring a small copy and scaling
// it up is both cheaper and smoother than blurring at full size.
static Image *make_backdrop(const Image *shot)
{
    Image *small = img_resize(shot, 64, 48);
    if (!small) return NULL;
    box_blur(small, 3);
    for (int i = 0; i < 64 * 48; i++) {
        uint32_t p = small->px[i];
        int r = (p >> 16 & 255) * 2 / 5 + 10, g = (p >> 8 & 255) * 2 / 5 + 12, b = (p & 255) * 2 / 5 + 20;
        small->px[i] = rgb((uint32_t)(r << 16 | g << 8 | b));
    }
    Image *big = img_resize(small, SCREEN_W, SCREEN_H);
    img_free(small);
    return big;
}

static void sw_update_backdrop(void)
{
    const Image *src = W.card ? W.card(S.sel) : NULL;
    if (W.backdrop_for == S.sel && W.backdrop_src == src) return;
    if (!src && W.backdrop_for >= 0 && !W.backdrop_src) { W.backdrop_for = S.sel; return; }
    Image *next = src ? make_backdrop(src) : NULL;
    if (!next) { // no screenshot: Shelf's dark background
        next = img_new(SCREEN_W, SCREEN_H);
        if (!next) return;
        memcpy(next->px, S.bg->px, (size_t)SCREEN_W * SCREEN_H * 4);
    }
    img_free(W.old_backdrop);
    W.old_backdrop = W.backdrop;
    W.backdrop = next;
    W.fade = W.old_backdrop ? 0 : 1;
    W.backdrop_for = S.sel;
    W.backdrop_src = src;
}

// One card at full size with its frame, shadow and system chip, rebuilt when its
// screenshot or art arrives.
static const Image *sw_sprite(int i)
{
    int gi = S.lib->recent[i];
    const Image *shot = W.card ? W.card(i) : NULL;
    const Image *icon = shot ? NULL : icon_get(gi, 1);
    if (W.sprites[i] && W.sprite_src[i] == shot && W.sprite_icon[i] == icon) return W.sprites[i];
    img_free(W.sprites[i]);
    Image *s = W.sprites[i] = img_new(SW_W + 2 * SW_SPRITE_PAD, SW_H + 2 * SW_SPRITE_PAD);
    W.sprite_src[i] = shot;
    W.sprite_icon[i] = icon;
    if (!s) return NULL;
    const float x = SW_SPRITE_PAD, y = SW_SPRITE_PAD;
    gfx_fill_rrect(s, x + 1, y + 5, SW_W - 2, SW_H - 1, 14, argb(110, 0, 0, 0));
    gfx_fill_rrect(s, x, y, SW_W, SW_H, 14, rgb(0xffffff));
    const Game *g = &S.lib->games[gi];
    const System *sys = &S.lib->sys[g->sys];
    float sx = x + SW_FRAME, sy = y + SW_FRAME;
    if (shot) {
        Image *r = img_new(SW_SHOT_W, SW_SHOT_H);
        if (r) {
            memcpy(r->px, shot->px, (size_t)SW_SHOT_W * SW_SHOT_H * 4);
            img_round(r, 7);
            gfx_blit(s, r, (int)sx, (int)sy, 255);
            img_free(r);
        }
    } else {
        // Never left through the switcher, so no screenshot: its box art on its colour.
        gfx_fill_rrect(s, sx, sy, SW_SHOT_W, SW_SHOT_H, 7, rgb(sys->color));
        float a = 150, ax = sx + (SW_SHOT_W - a) / 2, ay = sy + (SW_SHOT_H - a) / 2;
        if (icon) gfx_blit_scaled(s, icon, ax, ay, a, a, 255);
        else draw_placeholder(s, g, ax, ay, a);
    }
    if (shot && sys->label[0]) { // system chip, bottom left
        int tw = text_width(S.small, sys->label), ch = font_height(S.small) + 4;
        gfx_fill_rrect(s, sx + 8, sy + SW_SHOT_H - ch - 8, tw + 16, ch, ch / 2.f, rgb(sys->color));
        text_draw(s, S.small, sys->label, (int)sx + 16, (int)(sy + SW_SHOT_H - ch - 6), rgb(0xffffff));
    }
    if (i == 0 && W.running) { // the paused game, chip on the top edge
        const char *t = "PAUSED";
        int tw = text_width(S.small, t), ch = font_height(S.small) + 6;
        float cx = x + SW_W / 2.f - (tw + 22) / 2.f, cy = y - 4;
        gfx_fill_rrect(s, cx - 3, cy - 3, tw + 28, ch + 6, (ch + 6) / 2.f, rgb(0xffffff));
        gfx_fill_rrect(s, cx, cy, tw + 22, ch, ch / 2.f, rgb(0x262a35));
        text_draw(s, S.small, t, (int)cx + 11, (int)cy + 3, rgb(0xffffff));
    }
    img_find_spans(s);
    return s;
}

static void sw_card_rect(int i, float *x, float *y, float *w, float *h)
{
    float k = S.hscale[i].x, cx = S.track.x + i * SW_STEP;
    *w = SW_W * k; *h = SW_H * k;
    *x = cx - *w / 2; *y = SW_CY - *h / 2;
}

// The row, titles and chrome, without the selected card when `skip_sel`.
static void sw_draw_scene(Image *c, int skip_sel)
{
    if (W.backdrop) {
        if (W.fade < 1 && W.old_backdrop) {
            memcpy(c->px, W.old_backdrop->px, (size_t)SCREEN_W * SCREEN_H * 4);
            gfx_blit(c, W.backdrop, 0, 0, (uint8_t)(W.fade * 255));
        } else memcpy(c->px, W.backdrop->px, (size_t)SCREEN_W * SCREEN_H * 4);
    } else memcpy(c->px, S.bg->px, (size_t)SCREEN_W * SCREEN_H * 4);

    Library *L = S.lib;
    if (!L->nrecent) {
        text_center(c, S.title, "Nothing played yet", SCREEN_W / 2, 200, rgb(C_INK), SCREEN_W - 60);
        text_center(c, S.body, "Games you play show up here", SCREEN_W / 2, 244, rgb(C_SUB), SCREEN_W - 60);
    } else {
        const Game *g = &L->games[L->recent[S.sel]];
        char meta[160];
        snprintf(meta, sizeof meta, "%s%s", L->sys[g->sys].name,
                 S.sel == 0 && W.running ? " · Paused" : "");
        text_center(c, S.title, g->title, SCREEN_W / 2, 44, rgb(0xffffff), SCREEN_W - 60);
        text_center(c, S.body, meta, SCREEN_W / 2, 86, argb(255, 0xb8, 0xbf, 0xd0), SCREEN_W - 60);
    }
    for (int pass = 0; pass < 2; pass++)
        for (int i = 0; i < L->nrecent; i++) {
            if ((i == S.sel) != pass || (pass && skip_sel)) continue;
            float x, y, w, h;
            sw_card_rect(i, &x, &y, &w, &h);
            if (x + w < -20 || x > SCREEN_W + 20) continue;
            const Image *s = sw_sprite(i);
            if (!s) continue;
            float k = w / SW_W, pad = SW_SPRITE_PAD * k;
            if (fabsf(k - 1) < .004f) gfx_blit(c, s, (int)lroundf(x) - SW_SPRITE_PAD, (int)lroundf(y) - SW_SPRITE_PAD, 255);
            else gfx_blit_scaled(c, s, x - pad, y - pad, s->w * k, s->h * k, 255);
            if (pass) { // selection ring
                float p = .5f + .5f * sinf(S.t * 5.7f);
                gfx_stroke_rrect(c, x - 6, y - 6, w + 12, h + 12, 19, 4, lerp_rgb(C_SEL, C_SEL2, p));
            }
        }
    if (L->nrecent > 1) {
        float dw = 15, x0 = SCREEN_W / 2.f - (L->nrecent - 1) * dw / 2;
        for (int i = 0; i < L->nrecent; i++) {
            float s = i == S.sel ? 12 : 8;
            gfx_fill_rrect(c, x0 + i * dw - s / 2, 380 - s / 2, s, s, s / 2,
                           i == S.sel ? rgb(C_SEL) : argb(70, 255, 255, 255));
        }
    }
    int resume = S.sel == 0 && W.running;
    const char *const H[] = {"A", resume ? "Resume" : "Play", "B", "Shelf", "MENU", "Back to game"};
    if (L->nrecent) draw_footer_raw(c, SCREEN_H - FOOTER_H, H, W.running ? 6 : 4);
    else draw_footer_raw(c, SCREEN_H - FOOTER_H, H + 2, 2);
    draw_status(c);
    if (W.saving) {
        const char *t = W.saving == SW_TOAST_SAVING ? "Saving…" : W.saving == SW_TOAST_SAVED ? "✓ Saved" : "Couldn't save";
        int tw = text_width(S.small, t), th = font_height(S.small) + 8;
        float tx = SCREEN_W / 2.f - (tw + 24) / 2.f;
        gfx_fill_rrect(c, tx, 6, tw + 24, th, th / 2.f, rgb(0xffffff));
        text_draw(c, S.small, t, (int)tx + 12, 10, rgb(W.saving == SW_TOAST_FAILED ? 0xc0392b : 0x2f8a4c));
    }
}

static void sw_draw(Image *c)
{
    sw_update_backdrop();
    if (W.closing == UI_SW_HOME) {
        sw_draw_scene(S.layer, 0);
        float a = W.home_t > 1 ? 1 : W.home_t;
        memcpy(c->px, S.layer->px, (size_t)SCREEN_W * SCREEN_H * 4);
        gfx_blit(c, W.home_bg, 0, 0, (uint8_t)(a * 255));
        return;
    }
    float z = W.zoom.x;
    if (z > .996f || !S.lib->nrecent) { sw_draw_scene(c, 0); return; }
    // Zooming between the selected card and the full screen: the scene without that card,
    // its white frame growing in, and the screenshot scaled over it.
    sw_draw_scene(S.layer, 1);
    float x, y, w, h;
    sw_card_rect(S.sel, &x, &y, &w, &h);
    if (z < 0) z = 0;
    float rx = x * z, ry = y * z, rw = SCREEN_W + (w - SCREEN_W) * z, rh = SCREEN_H + (h - SCREEN_H) * z;
    float fr = SW_FRAME * (w / SW_W) * z;
    if (z > .02f) gfx_fill_rrect(S.layer, rx, ry, rw, rh, 14 * z, rgb(0xffffff));
    const Image *src = W.full ? W.full(S.sel) : NULL;
    if (!src && W.card) src = W.card(S.sel);
    if (src) gfx_fade_scaled_nearest(c, S.layer, src, rx + fr, ry + fr, rw - 2 * fr, rh - 2 * fr, 255);
    else { // no screenshot: grow the card itself
        memcpy(c->px, S.layer->px, (size_t)SCREEN_W * SCREEN_H * 4);
        const Image *s = sw_sprite(S.sel);
        float kx = rw / SW_W, ky = rh / SW_H;
        if (s) gfx_blit_scaled(c, s, rx - SW_SPRITE_PAD * kx, ry - SW_SPRITE_PAD * ky, s->w * kx, s->h * ky, 255);
    }
}
