#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "icons.h"
#include "text.h"

// Palette of the handheld UI (matches the mockups).
#define C_BG 0xeceff4
#define C_DOT 0xd3d8e2
#define C_INK 0x262a35
#define C_SUB 0x6b7184
#define C_SEL 0x19a4f0
#define C_SEL2 0x7fd3ff

// Home row geometry.
#define ROW_Y 146
#define TILE 168
#define STEP 184         // tile + gap
#define DIVIDER_GAP 20   // extra space between Recent and Library
#define CENTER_X 236     // (640 - 168) / 2

// Folder grid geometry.
#define G_TILE 88
#define G_COL 104
#define G_ROW 98
#define G_X 24
#define G_Y 100
#define G_ROWS 3
#define G_VIS 5
#define FOOTER_H 46
#define CAPTION_Y 394

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
static const char *SORTS[] = {"A–Z", "Recent", "Favorites"};

static struct {
    Library *lib;
    Font *title, *body, *small, *label;
    Image *bg, *layer, *scene, *caption;
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
    int *fgames, nfg;
    Spring ftrack;
    Spring *fscale;

    int launching;           // game index, -1 when idle
    float launch_t;
    Spring launch_scale;
    int launch_ready;
    int sound;               // pending UI click
} S;

static void tiles_free(void);

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
    S.caption = img_new(SCREEN_W, 40);
    S.scene_dirty = 1;

    add_folder(F_ALL, -1, "All games", 0x2b3040);
    add_folder(F_FAV, -1, "Favorites", 0xf0a020);
    for (int i = 0; i < lib->nsys; i++) add_folder(F_SYS, i, lib->sys[i].label, lib->sys[i].color);

    S.launching = -1;
    S.enter.x = S.enter.t = 1;
    for (int i = 0; i < home_count(); i++) S.hscale[i].x = S.hscale[i].t = 1;
    S.track.x = S.track.t = CENTER_X;
    return 0;
}

void ui_free(void)
{
    for (int i = 0; i < S.nfolders; i++) free(S.folders[i].games);
    free(S.fgames);
    free(S.fscale);
    img_free(S.bg);
    img_free(S.layer);
    img_free(S.scene);
    img_free(S.caption);
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
    S.fsel = i;
    int col = i / G_ROWS, cols = (S.nfg + G_ROWS - 1) / G_ROWS;
    if (col < S.fcol) S.fcol = col;
    if (col > S.fcol + G_VIS - 1) S.fcol = col - G_VIS + 1;
    if (S.fcol > cols - G_VIS) S.fcol = cols - G_VIS;
    if (S.fcol < 0) S.fcol = 0;
    S.ftrack.t = G_X - S.fcol * G_COL;
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
    if (S.launching >= 0) return;
    int view = S.view, sel = S.sel, fsel = S.fsel, sort = S.sort;
    ui_button_inner(b);
    if (S.view != view || S.sel != sel || S.fsel != fsel || S.sort != sort || S.launching >= 0)
        S.sound = 1;
}

int ui_take_sound(void)
{
    int s = S.sound;
    S.sound = 0;
    return s;
}

static void ui_button_inner(Button b)
{
    S.scene_dirty = 1;
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
        int i = S.fsel;
        if (b == BTN_LEFT && i - G_ROWS >= 0) folder_select(i - G_ROWS);
        if (b == BTN_RIGHT) {
            if (i + G_ROWS < S.nfg) folder_select(i + G_ROWS);
            else if ((i / G_ROWS + 1) * G_ROWS < S.nfg) folder_select(S.nfg - 1); // ragged last column
        }
        if (b == BTN_UP && i % G_ROWS > 0) folder_select(i - 1);
        if (b == BTN_DOWN && i % G_ROWS < G_ROWS - 1 && i + 1 < S.nfg) folder_select(i + 1);
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
    gfx_fill_rrect(c, x + 1, y + 4, w - 2, w - 1, w * .085f, argb(38, 20, 30, 60));
    const Image *ic = icon_get(gi, big);
    if (ic) gfx_blit_scaled(c, ic, x, y, w, w, 255);
    else draw_placeholder(c, &S.lib->games[gi], x, y, w);
}

static void draw_folder_raw(Image *c, const Folder *f, float cx, float cy, float w)
{
    float x = cx - w / 2, y = cy - w / 2, pad = w * .085f, gap = w * .036f, foot = w * .21f;
    gfx_fill_rrect(c, x + 1, y + 4, w - 2, w - 1, w * .085f, argb(38, 20, 30, 60));
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
static unsigned tile_clock;
// Selection ring masks, rasterized once per kind (home row, folder grid) at the selected
// size and 9-sliced to whatever size the zoom spring is at.
typedef struct { uint8_t *outer, *inner; int size, pad, extent, corner; } CursorMask;
static CursorMask cursors[2];

static void tiles_free(void)
{
    for (int i = 0; i < TILE_CACHE_CAP; i++) {
        img_free(tiles[i].image);
        memset(&tiles[i], 0, sizeof tiles[i]);
    }
    img_free(footer_img); footer_img = NULL; footer_key[0] = 0;
    for (int k = 0; k < 2; k++) {
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

static void draw_tile(Image *c, int kind, int id, float cx, float cy, float w, int big)
{
    int base = big ? TILE : G_TILE;
    int selected = (int)lroundf(base * (big ? 1.08f : 1.1f));
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

static void draw_game(Image *c, int gi, float cx, float cy, float w, int big)
{
    draw_tile(c, 0, gi, cx, cy, w, big);
}

static void draw_folder(Image *c, const Folder *f, float cx, float cy, float w)
{
    draw_tile(c, 1, (int)(f - S.folders), cx, cy, w, 1);
}

// Game count bubble on a folder's corner; drawn after the cursor so it stays readable.
static void draw_folder_badge(Image *c, const Folder *f, float cx, float cy, float w)
{
    float x = cx - w / 2, y = cy - w / 2;
    char n[16];
    snprintf(n, sizeof n, "%d", f->ngames);
    int tw = text_width(S.small, n), bw = tw + 16, bh = font_height(S.small) + 4;
    gfx_fill_rrect(c, x + w - bw + 10, y - 10, bw, bh, bh / 2.f, rgb(0xffffff));
    gfx_stroke_rrect(c, x + w - bw + 10, y - 10, bw, bh, bh / 2.f, 1, rgb(0xd5dae4));
    text_draw(c, S.small, n, (int)(x + w - bw + 18), (int)(y - 8), rgb(C_INK));
}

static void draw_cursor(Image *c, float cx, float cy, float w)
{
    float p = .5f + .5f * sinf(S.t * 5.7f);
    int big = w > 120;
    CursorMask *m = &cursors[big];
    if (!m->outer) {
        int size = (int)lroundf((big ? TILE : G_TILE) * (big ? 1.08f : 1.1f));
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
    int size = (int)lroundf(w), extent = size + 2 * m->pad;
    int x = (int)lroundf(cx - size / 2.f) - m->pad;
    int y = (int)lroundf(cy - size / 2.f) - m->pad;
    gfx_mask_a8_9slice(c, m->outer, m->extent, m->extent, m->corner, x, y, extent, extent,
                       lerp_rgb(C_SEL, C_SEL2, p));
    gfx_mask_a8_9slice(c, m->inner, m->extent, m->extent, m->corner, x, y, extent, extent,
                       argb(230, 255, 255, 255));
}

static void draw_status(Image *c)
{
    char buf[64];
    time_t now = time(NULL);
    strftime(buf, sizeof buf, "%a %d %b · %H:%M", localtime(&now));
    text_draw(c, S.small, buf, 16, 7, rgb(C_SUB));
}

// Hints name the Miyoo's printed labels: A B X Y, L1 R1, SELECT START MENU.
// Face buttons are circles; shoulder and system buttons are pills.
static void draw_footer_raw(Image *c, int top, const char *const *hints, int n)
{
    gfx_fill_rect(c, 0, top, SCREEN_W, FOOTER_H, argb(190, 255, 255, 255));
    gfx_fill_rect(c, 0, top, SCREEN_W, 1, rgb(0xdde1ea));
    int th = font_height(S.small), bh = th + 4;
    int x = 14, y = top + (FOOTER_H - bh) / 2;
    for (int i = 0; i < n; i += 2) {
        int bw = hints[i][1] ? text_width(S.small, hints[i]) + 14 : bh;
        gfx_fill_rrect(c, x, y, bw, bh, bh / 2.f, rgb(0x3a3f4d));
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
        snprintf(meta, sizeof meta, "Folder · %d games", f->ngames);
        text_center(c, S.title, f->label, SCREEN_W / 2, 44, rgb(C_INK), SCREEN_W - 60);
    }
    text_center(c, S.body, meta, SCREEN_W / 2, 86, rgb(C_SUB), SCREEN_W - 60);
    text_draw(c, S.small, L->nrecent && !inlib ? "RECENT" : "LIBRARY", 18, 114, rgb(C_SUB));

    float tx = S.track.x, cy = ROW_Y + TILE / 2.f;
    if (L->nrecent) {
        float dx = tx + item_x(L->nrecent - 1) + TILE + (STEP - TILE + DIVIDER_GAP) / 2.f - 2;
        if (dx > -10 && dx < SCREEN_W + 10) gfx_fill_rrect(c, dx, cy - 60, 4, 120, 2, rgb(0xcfd5df));
    }
    for (int pass = 0; pass < 2; pass++) // selected tile last so it sits on top
        for (int i = 0; i < n; i++) {
            if ((i == S.sel) != pass) continue;
            float cx = tx + item_x(i) + TILE / 2.f, w = TILE * S.hscale[i].x;
            if (cx + w < -20 || cx - w > SCREEN_W + 20) continue;
            if (i < L->nrecent) draw_game(c, L->recent[i], cx, cy, w, 1);
            else draw_folder(c, &S.folders[i - L->nrecent], cx, cy, w);
            if (i >= L->nrecent) draw_folder_badge(c, &S.folders[i - L->nrecent], cx, cy, w);
        }

    // Position dots: round for Recent, square for Library.
    float dw = 15, x0 = SCREEN_W / 2.f - (n - 1) * dw / 2;
    for (int i = 0; i < n; i++) {
        float s = i == S.sel ? 12 : 8, x = x0 + i * dw - s / 2, y = 362 - s / 2;
        gfx_fill_rrect(c, x, y, s, s, i < L->nrecent ? s / 2 : 2.5f, rgb(i == S.sel ? C_SEL : 0xc9cfda));
    }
    const char *const H[] = {"A", inlib ? "Open" : "Play", "L1", "Recent", "R1", "Library", "MENU", "Exit"};
    draw_footer(c, H, 8);
}

static void draw_folder_caption(Image *c, int y)
{
    const Game *g = &S.lib->games[S.fgames[S.fsel]];
    char name[200];
    snprintf(name, sizeof name, "%s · %s", g->title, S.lib->sys[g->sys].label);
    text_center(c, S.label, name, SCREEN_W / 2, y, rgb(C_INK), SCREEN_W - 48);
}

static void draw_folder_view(Image *c)
{
    const Folder *f = &S.folders[S.folder];
    gfx_fill_rrect(c, 24, 36, 48, 48, 12, rgb(f->color));
    text_draw(c, S.label, f->label, 84, 34, rgb(C_INK));
    char n[48];
    snprintf(n, sizeof n, "%d games", f->ngames);
    text_draw(c, S.small, n, 84, 60, rgb(C_SUB));
    // Sort chips cycle with Y; the hint sits beside them so they don't read as tappable.
    int x = SCREEN_W - 24, ch = font_height(S.small) + 8;
    for (int i = 2; i >= 0; i--) {
        int w = text_width(S.small, SORTS[i]) + 20;
        x -= w;
        gfx_fill_rrect(c, x, 46, w, ch, ch / 2.f, rgb(i == S.sort ? C_INK : 0xffffff));
        text_draw(c, S.small, SORTS[i], x + 10, 50, rgb(i == S.sort ? 0xffffff : C_SUB));
        x -= 6;
    }
    int yb = ch - 4;
    gfx_fill_rrect(c, x - yb, 48, yb, yb, yb / 2.f, rgb(0x3a3f4d));
    text_center(c, S.small, "Y", x - yb / 2, 50, rgb(0xffffff), yb);
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < S.nfg; k++) {
            if ((k == S.fsel) != pass) continue;
            float cx = S.ftrack.x + (k / G_ROWS) * G_COL + G_TILE / 2.f, cy = G_Y + (k % G_ROWS) * G_ROW + G_TILE / 2.f;
            if (cx < -G_TILE || cx > SCREEN_W + G_TILE) continue;
            float w = G_TILE * S.fscale[k].x;
            draw_game(c, S.fgames[k], cx, cy, w, 0);
        }
    if (!S.nfg) text_center(c, S.body, "Nothing here yet", SCREEN_W / 2, 220, rgb(C_SUB), SCREEN_W);
    static const char *const H[] = {"A", "Play", "B", "Back", "Y", "Sort", "L1", "First", "R1", "Last"};
    draw_footer(c, H, 10);
}

static void draw_launch(Image *c)
{
    float a = S.launch_t / .3f;
    if (a > 1) a = 1;
    gfx_fill_rect(c, 0, 0, SCREEN_W, SCREEN_H, argb((uint8_t)(a * 235), 20, 22, 30));
    draw_game(c, S.launching, SCREEN_W / 2.f, SCREEN_H / 2.f - 20, TILE * S.launch_scale.x, 1);
    if (S.launch_t > .15f) {
        char cap[200];
        snprintf(cap, sizeof cap, "Starting %s…", S.lib->games[S.launching].title);
        text_center(c, S.label, cap, SCREEN_W / 2, SCREEN_H / 2 + 100, rgb(0xffffff), SCREEN_W - 48);
    }
}

void ui_draw(Image *c)
{
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
        gfx_clear(S.caption, 0);
        if (S.view == V_FOLDER && S.nfg) draw_folder_caption(S.caption, 0);
        S.scene_dirty = !retain;
        S.scene_builds++;
        S.art_revision = revision;
        if (retain) memcpy(composed->px, S.scene->px, (size_t)SCREEN_W * SCREEN_H * sizeof *c->px);
    } else {
        memcpy(composed->px, S.scene->px, (size_t)SCREEN_W * SCREEN_H * sizeof *c->px);
    }
    if (S.view == V_HOME) {
        float cx = S.track.x + item_x(S.sel) + TILE / 2.f;
        float w = TILE * S.hscale[S.sel].x;
        draw_cursor(composed, cx, ROW_Y + TILE / 2.f, w);
        if (S.sel >= S.lib->nrecent)
            draw_folder_badge(composed, &S.folders[S.sel - S.lib->nrecent], cx, ROW_Y + TILE / 2.f, w);
    } else if (S.nfg) {
        float cx = S.ftrack.x + (S.fsel / G_ROWS) * G_COL + G_TILE / 2.f;
        float cy = G_Y + (S.fsel % G_ROWS) * G_ROW + G_TILE / 2.f;
        draw_cursor(composed, cx, cy, G_TILE * S.fscale[S.fsel].x);
        gfx_blit(composed, S.caption, 0, CAPTION_Y, 255);
    }
    if (e <= .995f) {
        memcpy(c->px, S.bg->px, (size_t)SCREEN_W * SCREEN_H * sizeof *c->px);
        float k = .92f + .08f * e, w = SCREEN_W * k, h = SCREEN_H * k;
        float a = e < 0 ? 0 : (e > 1 ? 1 : e);
        gfx_blit_scaled_nearest(c, S.layer, (SCREEN_W - w) / 2, (SCREEN_H - h) / 2, w, h, (uint8_t)(a * 255));
    }
    draw_status(c);
    if (S.launching >= 0) draw_launch(c);
}
