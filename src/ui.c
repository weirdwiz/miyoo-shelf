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
    {0xeceff4, 0xd3d8e2, 0x262a35, 0x5c6374, 0xffffff, 0xd5dae4, 0xc9cfda, 0x3a3f4d, 0xffffff, 0xffffff, 0xffffff, 190, 38},
    {0x14171f, 0x222734, 0xe6e9f0, 0x8d94a7, 0x14171f, 0x2f3544, 0x3a4152, 0x4a5163, 0x262b38, 0x1c2029, 0x20242f, 215, 90},
};
static const Palette *P = &PALETTES[0];
#define C_BG (P->bg)
#define C_DOT (P->dot)
#define C_INK (P->ink)
#define C_SUB (P->sub)
#define C_SEL 0x1668d8

// Home row geometry.
#define ROW_Y 144
#define TILE 168
#define STEP 184         // tile + gap
#define DIVIDER_GAP 20   // extra space between Recent and Library
#define CENTER_X ((SCREEN_W - TILE) / 2)

// Folder view: the grid on the left, the selected game's detail panel on the right.
// Three rows of small tiles, or two rows of big ones when the folder fits in a 2x2 block.
typedef struct { int tile, col, row, rows, vis; } Grid;
// One gap everywhere: column and row steps derive from the tile size.
#define G_GAP 16
#define GRID(tile, rows, vis) {tile, tile + G_GAP, tile + G_GAP, rows, vis}
static const Grid GRIDS[2] = {GRID(88, 3, 3), GRID(136, 2, 2)};
#define G_X 24
#define G_Y 100
#define G_EDGE 320       // tiles scrolling right slide under the panel from here
#define PANEL_X 332
#define PANEL_Y 40
#define PANEL_W 288
#define PANEL_H 384
#define FOOTER_H 48

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
// Firm but springy: ~1.4% overshoot, settles in ~0.22s. Still a nod to the DS feel.
#define BOUNCE 500.f, 36.f
#define SNAPPY 500.f, 40.f

// How long a home-screen message (ui_set_toast) stays up.
#define TOAST_FOR 4.f

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
    int redraw;              // the next ui_draw would differ from the last one
    long drawn_minute;       // status clock minute, art and card revisions last drawn
    unsigned drawn_art, drawn_cards;
    Spring enter;            // view transition 0..1

    int sel;                 // home row selection
    Spring track;            // home row x offset
    Spring hscale[MAX_RECENT + MAX_SYSTEMS + 2];

    int folder, fsel, sort, fcol;
    const Grid *grid;
    int *fgames, nfg;
    Spring ftrack;
    Spring *fscale;
    // Folder tiles whose zoom spring may be moving (only selection changes move them);
    // when it overflows, every spring is stepped until all settle.
    int factive[32], nfactive, fall;

    int launching;           // game index, -1 when idle
    float launch_t;
    Spring launch_scale;
    int launch_ready;
    int sound;               // pending UI click

    int dark;                // palette index
    Settings set;            // Options panel values; see ui_set_settings
    int options, osel;       // Options panel open, its selected row
    int oconfirm;            // "power off?" is waiting for a second A
    int action;              // pending UiAction
    int battery, charging;   // see ui_set_status
    char toast[128];         // message over the home screen, "" when none
    double toast_set;         // wall seconds when it was set; it lives on wall time
} S;

// Options rows, top to bottom.
enum { O_BRIGHT, O_VOLUME, O_SCREEN, O_DARK, O_OFF, O_COUNT };
static const char *const OPT_TITLE[O_COUNT] = {"Brightness", "Volume", "Screen", "Dark mode", "Power off"};
static const char *const OPT_SUB[O_COUNT] = {
    NULL, NULL, "How games fill the screen",
    "Easier on the eyes at night", "Saves everything first",
};

static void tiles_free(void);
static void status_free(void);
static void panel_invalidate(void);
// The game switcher view, at the end of this file.
static int sw_on(void);
static void sw_button(Button b);
static void sw_update(float dt);
static void sw_draw(Image *c);
static void sw_free(void);

static int home_count(void) { return S.lib->nrecent + S.nfolders; }

// Wall-clock seconds, for the status clock; tests substitute their own.
static double wall_real(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static double (*wall)(void) = wall_real;
void ui_set_wall_clock(double (*now)(void)) { wall = now ? now : wall_real; }

static int add_folder(int kind, int sys, const char *label, uint32_t color)
{
    Folder *f = &S.folders[S.nfolders];
    memset(f, 0, sizeof *f);
    f->kind = kind;
    f->sys = sys;
    snprintf(f->label, sizeof f->label, "%s", label);
    f->color = color;
    f->games = malloc(sizeof(int) * (S.lib->ngames ? S.lib->ngames : 1));
    if (!f->games) return -1;
    for (int i = 0; i < S.lib->ngames; i++) {
        Game *g = &S.lib->games[i];
        if (kind == F_ALL || (kind == F_FAV && g->fav) || (kind == F_SYS && g->sys == sys)) f->games[f->ngames++] = i;
    }
    if (f->ngames || kind == F_ALL) S.nfolders++;
    else free(f->games);
    return 0;
}

// Re-derives each folder's games after a favorite changed.
static void folders_refresh(void)
{
    for (int k = 0; k < S.nfolders; k++) {
        Folder *f = &S.folders[k];
        f->ngames = 0;
        for (int i = 0; i < S.lib->ngames; i++) {
            Game *g = &S.lib->games[i];
            if (f->kind == F_ALL || (f->kind == F_FAV && g->fav) || (f->kind == F_SYS && g->sys == f->sys))
                f->games[f->ngames++] = i;
        }
    }
}

static int build_bg(void)
{
    S.bg = img_new(SCREEN_W, SCREEN_H);
    if (!S.bg) return -1;
    gfx_clear(S.bg, rgb(C_BG));
    for (int y = 8; y < SCREEN_H; y += 16)
        for (int x = 8; x < SCREEN_W; x += 16) gfx_fill_rrect(S.bg, x - 1.3f, y - 1.3f, 2.6f, 2.6f, 1.3f, rgb(C_DOT));
    return 0;
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
    S.label = font_load(bold, 20);
    S.body = font_load(bold, 18);
    S.small = font_load(bold, 18);
    S.layer = img_new(SCREEN_W, SCREEN_H);
    S.scene = img_new(SCREEN_W, SCREEN_H);
    S.scene_dirty = 1;
    int bad = !S.title || !S.body || !S.small || !S.label || !S.layer || !S.scene || build_bg() ||
              add_folder(F_ALL, -1, "All games", 0x2b3040) || add_folder(F_FAV, -1, "Favorites", 0xa8610a);
    for (int i = 0; !bad && i < lib->nsys; i++) bad = add_folder(F_SYS, i, lib->sys[i].label, lib->sys[i].color);
    if (bad) { ui_free(); return -1; } // ui_free undoes any part of this

    S.launching = -1;
    S.battery = -1;
    S.dark = P == &PALETTES[1];
    settings_defaults(&S.set);
    S.set.dark = S.dark;
    S.enter.x = S.enter.t = 1;
    for (int i = 0; i < home_count(); i++) S.hscale[i].x = S.hscale[i].t = 1;
    S.track.x = S.track.t = CENTER_X;
    S.redraw = 1;
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
    status_free();
    font_free(S.title); font_free(S.body); font_free(S.small); font_free(S.label);
    memset(&S, 0, sizeof S);
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

static void factive_add(int k)
{
    for (int i = 0; i < S.nfactive; i++) if (S.factive[i] == k) return;
    if (S.nfactive < (int)(sizeof S.factive / sizeof *S.factive)) S.factive[S.nfactive++] = k;
    else S.fall = 1;
}

static void folder_select(int i)
{
    const Grid *g = S.grid;
    if (S.fsel < S.nfg) { S.fscale[S.fsel].t = 1.f; factive_add(S.fsel); }
    S.fsel = i;
    int col = i / g->rows, cols = (S.nfg + g->rows - 1) / g->rows;
    if (col < S.fcol) S.fcol = col;
    if (col > S.fcol + g->vis - 1) S.fcol = col - g->vis + 1;
    if (S.fcol > cols - g->vis) S.fcol = cols - g->vis;
    if (S.fcol < 0) S.fcol = 0;
    S.ftrack.t = G_X - S.fcol * g->col;
    if (i < S.nfg) { S.fscale[i].t = 1.1f; factive_add(i); }
}

// Out of memory leaves the view (and any open folder) as it was.
static void folder_open(int f)
{
    Folder *fd = &S.folders[f];
    int n = fd->ngames, *games = malloc(sizeof(int) * (n ? n : 1));
    Spring *scale = calloc(n ? n : 1, sizeof(Spring));
    if (!games || !scale) { free(games); free(scale); return; }
    S.folder = f;
    S.view = V_FOLDER;
    free(S.fgames);
    free(S.fscale);
    S.nfg = n;
    S.grid = &GRIDS[S.nfg <= 4];
    S.fgames = games;
    memcpy(S.fgames, fd->games, sizeof(int) * S.nfg);
    sort_mode = S.sort;
    qsort(S.fgames, S.nfg, sizeof(int), cmp_fg);
    S.fscale = scale;
    for (int k = 0; k < S.nfg; k++) S.fscale[k].x = S.fscale[k].t = 1;
    S.nfactive = S.fall = 0;
    S.fsel = 0;
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

// X in a folder: favorite the selected game, and show the new state in the detail panel.
static void favorite_toggle(void)
{
    if (!S.nfg) return;
    int gi = S.fgames[S.fsel];
    if (lib_set_fav(S.lib, gi, !S.lib->games[gi].fav)) {
        ui_set_toast("Couldn't save the favorites");
        return;
    }
    folders_refresh();
    panel_invalidate();
    int folder = S.folder;
    folder_open(folder); // the folder and its sort may now list other games
    for (int k = 0; k < S.nfg; k++)
        if (S.fgames[k] == gi) { folder_select(k); break; }
    S.enter.x = 1; // the view itself didn't change, so don't replay the zoom
}

void ui_button(Button b)
{
    S.redraw = 1;
    if (b == BTN_REDRAW) return; // exposure/wake, not navigation
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
    S.set.dark = S.dark;
    if (!S.bg) return; // before ui_init: build_bg picks the palette up
    Image *old = S.bg;
    if (build_bg()) S.bg = old; // out of memory: keep the old dots rather than none
    else img_free(old);
    tiles_free(); // sprites carry the palette's shadow; the footer strip its background
    S.scene_dirty = S.redraw = 1;
}

int ui_dark(void) { return S.dark; }

void ui_set_settings(const Settings *s)
{
    if (!s) return;
    S.set = *s;
    if (s->dark != S.dark) ui_set_dark(s->dark);
    S.redraw = 1;
}

Settings ui_settings(void)
{
    Settings s = S.set;
    s.dark = S.dark;
    return s;
}

void ui_set_toast(const char *text)
{
    snprintf(S.toast, sizeof S.toast, "%s", text ? text : "");
    S.toast_set = wall();
    S.redraw = 1;
}

void ui_set_status(int battery, int charging)
{
    if (S.battery != battery || S.charging != charging) S.redraw = 1;
    S.battery = battery;
    S.charging = charging;
}

static int clamp_level(int v, int hi) { return v < 0 ? 0 : v > hi ? hi : v; }

static void options_button(Button b)
{
    if (b == BTN_UP && S.osel > 0) S.osel--;
    if (b == BTN_DOWN && S.osel < O_COUNT - 1) S.osel++;
    if (b == BTN_B || b == BTN_START || b == BTN_MENU) {
        if (S.oconfirm) S.oconfirm = 0; // answers the power-off question first
        else S.options = 0;
        return;
    }
    if (S.osel == O_OFF) { // never powers off on a single press
        if (b != BTN_A) return;
        if (!S.oconfirm) { S.oconfirm = 1; return; }
        S.oconfirm = 0;
        S.action = UI_POWER_OFF;
        return;
    }
    int adjust = b == BTN_LEFT ? -1 : (b == BTN_RIGHT || b == BTN_A) ? 1 : 0;
    if (!adjust) return;
    switch (S.osel) {
    case O_BRIGHT:
        S.set.brightness = clamp_level(S.set.brightness + adjust, BRIGHTNESS_MAX);
        break;
    case O_VOLUME:
        S.set.volume = clamp_level(S.set.volume + adjust, VOLUME_MAX);
        break;
    case O_SCREEN:
        S.set.scale = (ScaleMode)((S.set.scale + adjust + SCALE_MODES) % SCALE_MODES);
        break;
    case O_DARK:
        ui_set_dark(!S.dark);
        break;
    }
    S.action = UI_SETTINGS;
}

static void ui_button_inner(Button b)
{
    S.scene_dirty = 1;
    if (S.options) { options_button(b); return; }
    if (b == BTN_MENU || b == BTN_START) { S.options = 1; S.osel = 0; S.oconfirm = 0; return; }
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
        if (b == BTN_X) favorite_toggle();
        if (b == BTN_A && S.nfg) start_launch(S.fgames[S.fsel]);
        if (b == BTN_B) { S.view = V_HOME; S.enter.x = 0; S.enter.v = 0; }
    }
}

static int spring_moving(const Spring *s) { return s->x != s->t || s->v != 0; }

// Whether the next update will change the scene (springs snap exactly when they settle).
static int scene_moving(void)
{
    if (S.view == V_HOME) { // hidden behind a folder, the row's springs just settle
        if (spring_moving(&S.track)) return 1;
        for (int i = 0; i < home_count(); i++) if (spring_moving(&S.hscale[i])) return 1;
    } else {
        if (spring_moving(&S.ftrack)) return 1;
        if (S.fall) return 1;
        for (int i = 0; i < S.nfactive; i++) if (spring_moving(&S.fscale[S.factive[i]])) return 1;
    }
    return 0;
}

static void scene_step(Spring *s, float dt, float k, float damping)
{
    float before = s->x;
    spring_step(s, dt, k, damping);
    if (before != s->x) S.scene_dirty = 1;
}

static int sw_animating(void);
static int sw_asking(void);
static unsigned sw_revision(void);
static int sw_prebuild_next(void);
static void sw_prebuild(void);

// The toast's life runs on wall time: the frame loop sleeps through its static middle,
// so animation time (dt) stands still while it is up.
static double toast_age(void) { return wall() - S.toast_set; }

// The toast only moves while fading in or out; in between the screen is idle.
static int toast_fading(void)
{
    double age = toast_age();
    return S.toast[0] && (age < .12 || age > TOAST_FOR - .45);
}

// Whether the next update changes what's on screen.
static int animating(void)
{
    if (toast_fading()) return 1; // the message fades in and out on its own
    if (sw_on()) return sw_animating();
    return scene_moving() || spring_moving(&S.enter) || S.launching >= 0;
}

void ui_update(float dt)
{
    if (S.toast[0] && toast_age() > TOAST_FOR) {
        S.toast[0] = 0;
        S.redraw = 1; // the frame that takes it away
    }
    if (animating()) S.redraw = 1;
    if (sw_on()) { sw_update(dt); return; }
    spring_step(&S.enter, dt, SNAPPY);
    // Opening or closing a folder rebuilds the scene anyway (any button dirties it), so
    // the home row moving out of sight mustn't redraw the folder view every frame.
    void (*home_step)(Spring *, float, float, float) = S.view == V_HOME ? scene_step : spring_step;
    home_step(&S.track, dt, BOUNCE);
    for (int i = 0; i < home_count(); i++) home_step(&S.hscale[i], dt, 480.f, 40.f);
    if (S.view == V_FOLDER) {
        scene_step(&S.ftrack, dt, BOUNCE);
        if (S.fall) {
            S.fall = 0;
            for (int i = 0; i < S.nfg; i++) {
                scene_step(&S.fscale[i], dt, 480.f, 40.f);
                S.fall |= spring_moving(&S.fscale[i]);
            }
            if (!S.fall) S.nfactive = 0;
        } else {
            for (int i = 0; i < S.nfactive; i++) {
                Spring *sp = &S.fscale[S.factive[i]];
                scene_step(sp, dt, 480.f, 40.f);
                if (!spring_moving(sp)) S.factive[i--] = S.factive[--S.nfactive];
            }
        }
    }
    if (S.launching >= 0) {
        S.launch_t += dt;
        spring_step(&S.launch_scale, dt, 260.f, 26.f);
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
    S.scene_dirty = S.redraw = 1;
    tiles_free();
    // Back on the home row with the launched game first (simulator keeps running).
    S.view = V_HOME;
    for (int i = 0; i < home_count(); i++) S.hscale[i].x = S.hscale[i].t = 1;
    home_select(0);
    S.track.x = S.track.t;
    S.enter.x = 0;
}

/* ---------- drawing ---------- */

// A message over the home screen: a dark pill that fades in and out. Over the in-game
// menu it takes the light pill a question uses, at the top, unless a question is up.
static void draw_toast(Image *c)
{
    if (!S.toast[0]) return;
    float t = (float)toast_age(), a = 1;
    if (t < .12f) a = t / .12f;
    else if (t > TOAST_FOR - .45f) a = (TOAST_FOR - t) / .45f;
    if (a <= 0) return;
    if (sw_on()) {
        if (sw_asking()) return;
        int tw = text_width(S.small, S.toast), th = font_height(S.small) + 8;
        float x = SCREEN_W / 2.f - (tw + 24) / 2.f;
        gfx_fill_rrect(c, x, 8, tw + 24, th, th / 2.f, argb((uint8_t)(a * 255), 255, 255, 255));
        text_draw(c, S.small, S.toast, (int)x + 12, 8 + (th - font_height(S.small)) / 2,
                  argb((uint8_t)(a * 255), 0x26, 0x2a, 0x35));
        return;
    }
    char text[128];
    text_fit(S.body, S.toast, SCREEN_W - 120, text, sizeof text);
    int tw = text_width(S.body, text), th = font_height(S.body) + 16;
    float x = (SCREEN_W - tw - 32) / 2.f, y = SCREEN_H - FOOTER_H - th - 24;
    gfx_fill_rrect(c, x, y, tw + 32, th, th / 2.f, argb((uint8_t)(a * 235), 22, 25, 33));
    text_draw(c, S.body, text, (int)x + 16, (int)y + 8, argb((uint8_t)(a * 255), 255, 255, 255));
}

static void text_center(Image *c, Font *f, const char *s, int cx, int y, uint32_t col, int max_w)
{
    char buf[256];
    text_fit(f, s, max_w, buf, sizeof buf);
    text_draw(c, f, buf, cx - text_width(f, buf) / 2, y, col);
}

static int wrap_text(Font *f, const char *s, int max_w, char out[][128], int max);

// Cartridge silhouettes for games without art, as unit-square polygons: GB/GBC carts are
// tall with the top-right corner cut diagonally, GBA carts wider and shorter.
static const float CART_GB[][2] = {{.04f, 0}, {.8f, 0}, {1, .18f}, {1, 1}, {0, 1}, {0, .04f}};
static const float CART_GBA[][2] = {{.05f, 0}, {.95f, 0}, {1, .1f}, {1, .9f}, {.95f, 1}, {.05f, 1}, {0, .9f}, {0, .1f}};

// Fills a polygon scaled to a w x h box, supersampled 4x4 per pixel like the battery
// bolt's mask so the edges anti-alias at any tile size.
static void fill_cart(Image *c, const float (*pt)[2], int npt, float x, float y, float w, float h, uint32_t col)
{
    int mw = (int)(w + .5f), mh = (int)(h + .5f);
    if (mw < 1 || mh < 1) return;
    uint8_t *mask = malloc((size_t)mw * mh);
    if (!mask) return;
    for (int py = 0; py < mh; py++)
        for (int px = 0; px < mw; px++) {
            int n = 0;
            for (int s = 0; s < 16; s++) {
                float u = (px + (s % 4 + .5f) / 4) / w, v = (py + (s / 4 + .5f) / 4) / h;
                int in = 0;
                for (int i = 0, j = npt - 1; i < npt; j = i++)
                    if ((pt[i][1] > v) != (pt[j][1] > v) &&
                        u < (pt[j][0] - pt[i][0]) * (v - pt[i][1]) / (pt[j][1] - pt[i][1]) + pt[i][0])
                        in = !in;
                n += in;
            }
            mask[py * mw + px] = (uint8_t)(n * 255 / 16);
        }
    gfx_mask_a8(c, mask, mw, mh, (int)x, (int)y, col);
    free(mask);
}

// A game without box art: a neutral tile (so real art stays the brightest thing on screen)
// carrying a grey cartridge with the system's label on it, and the title wrapped below.
static void draw_placeholder(Image *c, const Game *g, float x, float y, float w)
{
    const System *s = &S.lib->sys[g->sys];
    gfx_fill_rrect(c, x, y, w, w, w * .085f, rgb(C_DOT));
    Font *f = w > 120 ? S.label : S.small;
    int lh = font_height(f);
    // The title block is pinned to the tile's bottom; the cart centres in the room left
    // above it, which keeps a workable cart at every size despite the fixed-height fonts.
    char lines[2][128];
    int n = wrap_text(f, g->title, (int)(w * .9f), lines, 2);
    float top = y + w * .06f, bot = y + w * .95f - n * lh - w * .03f;
    int gba = !strcmp(s->dir, "GBA");
    float ch = gba ? (bot - top) * .72f : bot - top; // cart height
    if (ch > w * (gba ? .42f : .55f)) ch = w * (gba ? .42f : .55f);
    float cw = gba ? ch * 1.5f : ch * .82f;
    float cy = top + (bot - top - ch) / 2;
    if (ch >= 8) {
        fill_cart(c, gba ? CART_GBA : CART_GB, gba ? 8 : 6, x + (w - cw) / 2, cy, cw, ch, rgb(0x9aa1b0));
        // The label window on the cart: the system colour with its short label in white.
        float lab_h = lh + w * .05f;
        if (lab_h < ch - 4) {
            float lab_w = cw * .8f, lx = x + (w - lab_w) / 2;
            gfx_fill_rrect(c, lx, cy + (ch - lab_h) / 2, lab_w, lab_h, lab_h * .18f, rgb(s->color));
            text_center(c, f, s->label, (int)(lx + lab_w / 2),
                        (int)(cy + (ch - lab_h) / 2 + (lab_h - lh) / 2), rgb(0xffffff), (int)lab_w);
        }
    }
    int ty = (int)(y + w * .95f) - n * lh;
    for (int i = 0; i < n; i++, ty += lh)
        text_center(c, f, lines[i], (int)(x + w / 2), ty, rgb(C_INK), (int)(w * .9f));
}

static void draw_game_raw(Image *c, int gi, float cx, float cy, float w, int big)
{
    float x = cx - w / 2, y = cy - w / 2;
    gfx_fill_rrect(c, x + 1, y + 4, w - 2, w - 1, w * .085f, argb(P->shadow_a, 0, 0, 0));
    const Image *ic = icon_get(gi, big);
    if (ic) gfx_blit_scaled(c, ic, x, y, w, w, 255);
    else draw_placeholder(c, &S.lib->games[gi], x, y, w);
}

// The folder tile's one art source: its first game with art. Scanning in order also
// drives sprite invalidation — an earlier game's art arriving changes the answer.
static const Image *folder_thumb(const Folder *f)
{
    for (int k = 0; k < f->ngames; k++) {
        const Image *ic = icon_get(f->games[k], 0);
        if (ic) return ic;
    }
    return NULL;
}

// A folder: a neutral tile (real art stays the brightest thing on screen) with its first
// game's art centred up top, a thin system-colour stripe over the footer band, and the
// name with the game count as quiet text after it.
static void draw_folder_raw(Image *c, const Folder *f, float cx, float cy, float w)
{
    float x = cx - w / 2, y = cy - w / 2, pad = w * .085f, foot = w * .21f, band = y + w - foot;
    gfx_fill_rrect(c, x + 1, y + 4, w - 2, w - 1, w * .085f, argb(P->shadow_a, 0, 0, 0));
    gfx_fill_rrect(c, x, y, w, w, w * .085f, rgb(C_DOT));
    const Image *ic = folder_thumb(f);
    if (ic) { // one large thumb, centred in the room above the footer band
        float side = band - pad - (y + pad) - pad * .5f;
        if (side > w - 2 * pad) side = w - 2 * pad;
        gfx_blit_scaled(c, ic, x + (w - side) / 2, y + pad + (band - pad - (y + pad) - side) / 2,
                        side, side, 255);
    }
    gfx_fill_rect(c, x, band, w, 4, rgb(f->color));
    Font *lf = w > 120 ? S.label : S.small;
    char count[16], name[128];
    snprintf(count, sizeof count, "%d", f->ngames);
    int lh = font_height(lf), avail = (int)(w - 2 * pad), gap = 8;
    int cw = text_width(lf, count);
    text_fit(lf, f->label, avail - gap - cw, name, sizeof name);
    int tw = text_width(lf, name) + gap + cw;
    int tx = (int)cx - tw / 2, ty = (int)(band + 4 + (foot - 4 - lh) / 2);
    text_draw(c, lf, name, tx, ty, rgb(C_INK));
    text_draw(c, lf, count, tx + tw - cw, ty, rgb(C_SUB));
}

// A bounded working set of reusable sprites, not one surface per ROM.
// Normal/selected sizes are rasterized once. In-between animation frames scale
// the normal sprite instead of rebuilding text, shadows and rounded geometry.
#define TILE_CACHE_CAP 24
#define TILE_PAD 8
typedef struct {
    int kind, id, size, big;
    const Image *source; // icon the sprite was built from: the game's art, or a folder's first with art
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
    const Image *source = kind == 0 ? icon_get(id, big) : folder_thumb(&S.folders[id]);
    TileSprite *slot = NULL;
    for (int i = 0; i < TILE_CACHE_CAP; i++) {
        TileSprite *t = &tiles[i];
        if (t->image && t->kind == kind && t->id == id &&
            t->size == size && t->big == big) {
            if (t->source == source) {
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
    slot->source = source;
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

static void draw_cursor(Image *c, float cx, float cy, float w, int base)
{
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
                       rgb(C_SEL));
    gfx_mask_a8_9slice(c, m->inner, m->extent, m->extent, m->corner, x, y, extent, extent,
                       argb(230, 255, 255, 255));
}

// Status glyphs are coverage masks, 4x4 supersampled once from a shape test.
#define BOLT_W 9
#define BOLT_H 13
static uint8_t bolt_mask[BOLT_W * BOLT_H];

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
    for (int y = 0; y < BOLT_H; y++)
        for (int x = 0; x < BOLT_W; x++) {
            int n = 0;
            for (int s = 0; s < 16; s++) n += in_bolt(x + (s % 4 + .5f) / 4, y + (s / 4 + .5f) / 4);
            bolt_mask[y * BOLT_W + x] = (uint8_t)(n * 255 / 16);
        }
}

// Right-aligned at `right`, with the percentage text's top at `ty`: the body hangs
// 4px below it so text and body share a bottom edge.
static void draw_battery(Image *c, int right, int ty)
{
    const int bw = 27, bh = 14;
    int y = ty + 4, x = right - bw - 3;
    uint32_t ink = rgb(C_SUB);
    uint32_t fill = S.charging ? rgb(0x30a46c) : S.battery <= 15 ? rgb(0xe5484d) : ink;
    gfx_stroke_rrect(c, x, y, bw, bh, 4, 1.6f, ink);
    gfx_fill_rrect(c, x + bw + .5f, y + 4, 2.5f, bh - 8, 1, ink);
    float w = (bw - 6) * (S.battery < 0 ? 0 : S.battery > 100 ? 100 : S.battery) / 100.f;
    if (w > .5f) gfx_fill_rrect(c, x + 3, y + 3, w < 2 ? 2 : w, bh - 6, 1.5f, fill);
    if (S.charging) gfx_mask_a8(c, bolt_mask, BOLT_W, BOLT_H, x - BOLT_W - 4, y, fill);
    char pct[8];
    snprintf(pct, sizeof pct, "%d%%", S.battery);
    int tx = x - (S.charging ? BOLT_W + 8 : 4) - text_width(S.small, pct);
    text_draw(c, S.small, pct, tx, ty, rgb(C_SUB));
}

static void status_clock(char *buf, int n)
{
    time_t now = (time_t)wall();
    struct tm tm;
    strftime(buf, n, "%a %d %b · %H:%M", localtime_r(&now, &tm));
}

// What the status strip shows; equal keys draw identical strips.
static void status_key(char *out, int n)
{
    char clock[64];
    status_clock(clock, sizeof clock);
    snprintf(out, n, "%s|%d|%d|%p", clock, S.battery, S.charging, (const void *)P);
}

// Clock, battery and Wi-Fi along the top of a STATUS_H-tall band.
#define STATUS_H 32
static void draw_status_raw(Image *c)
{
    char buf[64];
    status_clock(buf, sizeof buf);
    int ty = (STATUS_H - font_height(S.small)) / 2;
    text_draw(c, S.small, buf, 16, ty, rgb(C_SUB));
    build_status_masks();
    if (S.battery >= 0) draw_battery(c, SCREEN_W - 16, ty);
}

// Chrome that changes rarely: drawn once onto a transparent band of the screen and blended
// in each frame until what it shows (its key) changes.
typedef struct {
    Image *img;
    char key[256];
} Band;
static Band status_band; // see draw_status

static void band_free(Band *b)
{
    img_free(b->img);
    b->img = NULL;
    b->key[0] = 0;
}

// Returns the band's image cleared for redrawing when `key` changed, else NULL.
static Image *band_begin(Band *b, int h, const char *key)
{
    if (b->img && !strcmp(b->key, key)) return NULL;
    if (!b->img) b->img = img_new(SCREEN_W, h);
    if (!b->img) return NULL;
    memset(b->img->px, 0, (size_t)SCREEN_W * h * sizeof *b->img->px);
    snprintf(b->key, sizeof b->key, "%s", key);
    return b->img;
}

static void band_end(Band *b) { img_find_runs(b->img); }

static void status_free(void) { band_free(&status_band); }

// The status band, re-rendered only when what it shows changes.
static void draw_status(Image *c)
{
    char key[256];
    status_key(key, sizeof key);
    Image *band = band_begin(&status_band, STATUS_H, key);
    if (band) { draw_status_raw(band); band_end(&status_band); }
    if (status_band.img) gfx_blit(c, status_band.img, 0, 0, 255);
    else draw_status_raw(c);
}

// Hints name the Miyoo's printed labels: A B X Y, L1 R1, SELECT START MENU.
// Face buttons are circles; shoulder and system buttons are pills.
static void draw_footer_raw(Image *c, int top, const char *const *hints, int n)
{
    gfx_fill_rect(c, 0, top, SCREEN_W, FOOTER_H, argb(P->footer_a, P->footer >> 16, P->footer >> 8 & 255, P->footer & 255));
    gfx_fill_rect(c, 0, top, SCREEN_W, 1, rgb(P->line));
    int th = font_height(S.small), bh = th + 4;
    int x = 16, y = top + (FOOTER_H - bh) / 2;
    for (int i = 0; i < n; i += 2) {
        int bw = hints[i][1] ? text_width(S.small, hints[i]) + 16 : bh;
        gfx_fill_rrect(c, x, y, bw, bh, bh / 2.f, rgb(P->key));
        text_center(c, S.small, hints[i], x + bw / 2, y + (bh - th) / 2, rgb(0xffffff), bw);
        x += bw + 8;
        x += text_draw(c, S.small, hints[i + 1], x, y + (bh - th) / 2, rgb(C_SUB)) + 16;
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

static void format_play_time(const Game *g, char *out, int n);
static void format_last_played(const Game *g, char *out, int n);

// The quiet line under the home page dots, describing the selection: a game's system,
// when it was last played and for how long, or a folder's size and favorite count.
// Parts the library doesn't know (never played, no favorites) drop out.
static void format_home_facts(char *out, int n)
{
    const Library *L = S.lib;
    if (S.sel < L->nrecent) {
        const Game *g = &L->games[L->recent[S.sel]];
        if (!g->play_count) { snprintf(out, n, "%s", L->sys[g->sys].name); return; }
        char last[32], play[32];
        format_last_played(g, last, sizeof last);
        format_play_time(g, play, sizeof play);
        snprintf(out, n, "%s · %s · %s", L->sys[g->sys].name, last, play);
    } else {
        const Folder *f = &S.folders[S.sel - L->nrecent];
        int fav = 0;
        for (int k = 0; k < f->ngames; k++) fav += L->games[f->games[k]].fav != 0;
        if (fav) snprintf(out, n, "%d game%s · %d favorite%s", f->ngames, f->ngames == 1 ? "" : "s",
                          fav, fav == 1 ? "" : "s");
        else snprintf(out, n, "%d game%s", f->ngames, f->ngames == 1 ? "" : "s");
    }
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
    text_center(c, S.body, meta, SCREEN_W / 2, 88, rgb(C_SUB), SCREEN_W - 60);
    text_draw(c, S.small, L->nrecent && !inlib ? "Recent" : "Library", 16, 116, rgb(C_SUB));

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
        }

    // Position dots: round for Recent, square for Library.
    float dw = 16, x0 = SCREEN_W / 2.f - (n - 1) * dw / 2;
    for (int i = 0; i < n; i++) {
        float s = i == S.sel ? 12 : 8, x = x0 + i * dw - s / 2, y = 360 - s / 2;
        gfx_fill_rrect(c, x, y, s, s, i < L->nrecent ? s / 2 : 2.5f, rgb(i == S.sel ? C_INK : P->dim_dot));
    }
    // One quiet facts line in the empty band between the dots and the footer. Like the
    // meta line at the top it lives in the retained scene, so it changes with S.sel.
    char facts[160];
    format_home_facts(facts, sizeof facts);
    text_center(c, S.small, facts, SCREEN_W / 2, 392, rgb(C_SUB), SCREEN_W - 60);
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
    else if (m < 1) snprintf(out, n, "<1 m");
    else if (m < 60) snprintf(out, n, "%d m", m);
    else if (m % 60) snprintf(out, n, "%d h %d m", m / 60, m % 60);
    else snprintf(out, n, "%d h", m / 60);
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
    // Without network time the clock may start near 1970, and so may the play times Shelf
    // recorded; only trust dates after 2020. Otherwise fall back to the recents list.
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

// A five-pointed star in unit-square coordinates, for the favorite mark in the detail
// panel (it replaced the old grid's Favorite cell).
static const float STAR[][2] = {
    {.5f, 0}, {.612f, .346f}, {.976f, .346f}, {.682f, .559f}, {.794f, .904f},
    {.5f, .691f}, {.206f, .904f}, {.318f, .559f}, {.024f, .346f}, {.388f, .346f},
};

// The selected game's art, title, system and play stats. The panel is opaque over the
// background, so it's kept as one pre-composited image per selection and copied in; the
// empty card is rasterized once. Its left margin also covers grid tiles that have
// scrolled under it.
static void panel_invalidate(void) { panel_game = -1; }

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
        gfx_fill_rrect(panel_base, px + 1, py + 3, PANEL_W - 2, PANEL_H - 1, 16, argb(P->shadow_a / 2, 0, 0, 0));
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

        int tx = (int)px + 16, tw = PANEL_W - 32, ty = (int)py + TILE + 32;
        char lines[2][128];
        int nl = wrap_text(S.label, g->title, tw, lines, 2);
        for (int i = 0; i < nl; i++, ty += font_height(S.label))
            text_draw(p, S.label, lines[i], tx, ty, rgb(C_INK));
        char buf[128];
        text_fit(S.small, S.lib->sys[g->sys].name, tw, buf, sizeof buf);
        text_draw(p, S.small, buf, tx, ty + 4, rgb(C_SUB));
        if (g->fav) // favorite star, right after the system name
            fill_cart(p, STAR, 10, tx + text_width(S.small, buf) + 8,
                      ty + 4 + (font_height(S.small) - 12) / 2.f, 12, 12, rgb(0xa8610a));

        // The play stats as one quiet block instead of the old 2x2 grid.
        char facts[128];
        int fy = (int)py + 284, fh = (font_height(S.small) + 3) & ~3; // line spacing on the 4px grid
        if (g->play_count) {
            char play[32], last[32];
            format_play_time(g, play, sizeof play);
            format_last_played(g, last, sizeof last);
            snprintf(facts, sizeof facts, "Played %s · %d session%s", play, g->play_count,
                     g->play_count == 1 ? "" : "s");
            text_fit(S.small, facts, tw, buf, sizeof buf);
            text_draw(p, S.small, buf, tx, fy, rgb(C_INK));
            // Words continue the sentence in lower case; dates ("22 Sep") keep theirs.
            if (!strcmp(last, "Most recent")) snprintf(last, sizeof last, "most recently");
            else if (!strcmp(last, "Today") || !strcmp(last, "Yesterday") ||
                     !strcmp(last, "Recently") || !strcmp(last, "A while ago"))
                last[0] = (char)(last[0] - 'A' + 'a');
            snprintf(facts, sizeof facts, "Last played %s", last);
            text_fit(S.small, facts, tw, buf, sizeof buf);
            text_draw(p, S.small, buf, tx, fy + fh, rgb(C_SUB));
        } else {
            text_draw(p, S.small, "Never played", tx, fy, rgb(C_SUB));
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
    text_draw(c, S.label, f->label, 84, 36, rgb(C_INK));
    char n[64];
    snprintf(n, sizeof n, "%d game%s · %s", f->ngames, f->ngames == 1 ? "" : "s", SORTS[S.sort]);
    text_draw(c, S.small, n, 84, 60, rgb(C_SUB));
    // Only the columns that can reach the visible strip (zoomed tiles are up to 10% wider).
    float reach = g->tile * .55f;
    int c0 = (int)floorf((-8 - reach - S.ftrack.x - g->tile / 2.f) / g->col);
    int c1 = (int)ceilf((G_EDGE + reach - S.ftrack.x - g->tile / 2.f) / g->col);
    int k0 = c0 < 0 ? 0 : c0 * g->rows, k1 = (c1 + 1) * g->rows;
    if (k1 > S.nfg) k1 = S.nfg;
    for (int pass = 0; pass < 2; pass++)
        for (int k = k0; k < k1; k++) {
            if ((k == S.fsel) != pass) continue;
            float cx = S.ftrack.x + (k / g->rows) * g->col + g->tile / 2.f, cy = G_Y + (k % g->rows) * g->row + g->tile / 2.f;
            float w = g->tile * S.fscale[k].x;
            if (cx + w / 2 < -8 || cx - w / 2 > G_EDGE) continue;
            draw_game(c, S.fgames[k], cx, cy, w, g->tile);
        }
    if (S.nfg) draw_detail(c, S.fgames[S.fsel]);
    else text_center(c, S.body, "Nothing here yet", SCREEN_W / 2, 220, rgb(C_SUB), SCREEN_W);
    static const char *const H[] = {"A", "Play", "B", "Back", "X", "Favorite", "Y", "Sort",
                                   "L1", "First", "R1", "Last"};
    draw_footer(c, H, 12);
}

// Options sheet over the dimmed view; drawn into the retained scene since it's static.
static void draw_level(Image *c, int right, int cy, int value, int max)
{
    char n[8];
    snprintf(n, sizeof n, "%d", value);
    const int bar_w = 132, h = 8, gap = 4;
    int segs = max > 12 ? 10 : max; // one segment per step, grouped when there are many
    int lit = value * segs / max;
    float sw = (bar_w - (segs - 1) * gap) / (float)segs;
    int bx = right - bar_w, by = cy - h / 2;
    for (int i = 0; i < segs; i++)
        gfx_fill_rrect(c, bx + i * (sw + gap), by, sw, h, 2,
                       i < lit ? rgb(C_INK) : rgb(P->line));
    text_draw(c, S.small, n, right - bar_w - 8 - text_width(S.small, n),
              cy - font_height(S.small) / 2, rgb(C_INK));
}

// The three scale modes as one joined segmented control, the chosen segment filled.
static void draw_scale_pills(Image *c, int right, int cy)
{
    static const char *const NAME[SCALE_MODES] = {"Sharp", "Smooth", "Integer"};
    int width[SCALE_MODES], total = 0, h = font_height(S.small) + 8;
    for (int i = 0; i < SCALE_MODES; i++) {
        width[i] = text_width(S.small, NAME[i]) + 16;
        total += width[i];
    }
    int x = right - total, y = cy - h / 2;
    gfx_fill_rrect(c, x, y, total, h, 8, rgb(P->line));
    for (int i = 0; i < SCALE_MODES; i++) {
        int on = i == (int)S.set.scale;
        if (on) gfx_fill_rrect(c, x + 2, y + 2, width[i] - 4, h - 4, 6, rgb(C_INK));
        if (i) gfx_fill_rect(c, x, y + 4, 1, h - 8, rgb(P->panel));
        text_center(c, S.small, NAME[i], x + width[i] / 2, y + 4,
                    on ? rgb(P->on_ink) : rgb(C_SUB), width[i]);
        x += width[i];
    }
}

static void draw_options(Image *c)
{
    gfx_fill_rect(c, 0, 0, SCREEN_W, SCREEN_H - FOOTER_H, argb(S.dark ? 150 : 110, 10, 12, 18));
    const int w = 452, row = 64, h = 56 + O_COUNT * row + 12;
    int x = (SCREEN_W - w) / 2, y = (SCREEN_H - FOOTER_H - h) / 2;
    gfx_fill_rrect(c, x + 2, y + 6, w - 4, h - 2, 18, argb(60, 0, 0, 0));
    gfx_fill_rrect(c, x, y, w, h, 18, rgb(P->panel));
    text_draw(c, S.label, "Options", x + 24, y + 16, rgb(C_INK));
    for (int i = 0; i < O_COUNT; i++) {
        int ry = y + 56 + i * row, sel = i == S.osel;
        if (sel) { // quiet fill plus an accent bar; inks stay as they are when unselected
            gfx_fill_rrect(c, x + 10, ry, w - 20, row - 8, 8, rgb(P->dim_dot));
            gfx_fill_rrect(c, x + 10, ry, 4, row - 8, 2, rgb(C_SEL));
        }
        uint32_t ink = rgb(C_INK), sub_ink = rgb(C_SUB);
        int cy = ry + (row - 8) / 2, right = x + w - 24;
        const char *sub = OPT_SUB[i];
        if (i == O_OFF && S.oconfirm) sub = "Press A again to power off";
        text_draw(c, S.body, OPT_TITLE[i], x + 24, sub ? ry + 8 : cy - font_height(S.body) / 2, ink);
        if (sub) text_draw(c, S.small, sub, x + 24, ry + 32, sub_ink);
        if (i == O_BRIGHT) draw_level(c, right, cy, S.set.brightness, BRIGHTNESS_MAX);
        if (i == O_VOLUME) draw_level(c, right, cy, S.set.volume, VOLUME_MAX);
        if (i == O_SCREEN) draw_scale_pills(c, right, cy);
        if (i == O_DARK) { // switch
            float sw = 52, sh = 32, sx = right - sw, sy = cy - sh / 2;
            gfx_fill_rrect(c, sx, sy, sw, sh, sh / 2, S.dark ? rgb(C_INK) : rgb(P->line));
            gfx_fill_rrect(c, sx + (S.dark ? sw - sh : 0) + 4, sy + 4, sh - 8, sh - 8, (sh - 8) / 2, rgb(S.dark ? P->on_ink : 0xffffff));
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

double ui_idle(void)
{
    if (S.redraw || animating()) return 0;
    if (icons_revision() != S.drawn_art) return 0;
    if (sw_on() && sw_revision() != S.drawn_cards) return 0;
    if (sw_on() && sw_prebuild_next() >= 0) return .001; // update, not redraw, builds it
    double now = wall();
    if ((long)(now / 60) != S.drawn_minute) return 0;
    double rest = 60 - fmod(now, 60) + .01; // the status clock's next minute
    if (S.toast[0]) { // asleep through the static middle, awake again for the fade-out
        double fade = TOAST_FOR - .45 - toast_age();
        if (fade < rest) rest = fade;
    }
    return rest;
}

void ui_draw(Image *c)
{
    S.redraw = 0;
    S.drawn_art = icons_revision();
    S.drawn_minute = (long)(wall() / 60);
    if (sw_on()) { S.drawn_cards = sw_revision(); sw_draw(c); draw_toast(c); return; }
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
            static const char *const H[] = {"A", "Change", "B", "Close"};
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
    draw_toast(c);
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
#define SW_CY 248
#define SW_SMALL .86f
#define SW_SPRITE_PAD 8

static struct {
    int on, running;           // switcher view; over a paused game, recents[0] is it
    const Image *(*card)(int), *(*full)(int);
    void (*select)(int);
    unsigned (*revision)(void);
    Image *sprites[MAX_RECENT];
    Image *small[MAX_RECENT];  // sprites[i] at SW_SMALL, for cards at rest
    uint8_t *ring;             // selection ring coverage at full card size, 9-sliced
    const Image *sprite_src[MAX_RECENT], *sprite_icon[MAX_RECENT];
    Image *home_bg;           // Shelf's own background: B fades to it, as Shelf starts on it
    Spring zoom;              // 0: selected card fills the screen, 1: in the row
    int closing, close_to;    // UI_SW_* being animated to, and the recent
    float home_t;             // B: fade-out progress
    int fired;                // the closing action was handed out; hold the last frame
    int layer_ok;             // S.layer holds the scene behind a zoom; reused while it's still
    unsigned layer_rev;       // card + art revisions it was drawn with
    char notice[64];          // a question over the row ("Restart the game?"); "" when none
    Band top, bottom;         // status, notice and titles; position dots and footer
} W;

void ui_switch_hooks(const Image *(*card)(int), const Image *(*full)(int), void (*select)(int),
                     unsigned (*revision)(void))
{
    W.card = card; W.full = full; W.select = select; W.revision = revision;
}

static int sw_asking(void) { return W.notice[0] != 0; }

void ui_switch_notice(const char *text)
{
    snprintf(W.notice, sizeof W.notice, "%s", text ? text : "");
    W.layer_ok = 0;
    S.redraw = 1;
}

void ui_switch_card_size(int *w, int *h)
{
    if (w) *w = SW_SHOT_W;
    if (h) *h = SW_SHOT_H;
}

int ui_switch_target(void) { return W.close_to; }

static int sw_on(void) { return W.on; }

static unsigned sw_revision(void) { return W.revision ? W.revision() : 0; }

static int sw_animating(void)
{
    for (int i = 0; i < S.lib->nrecent; i++) if (spring_moving(&S.hscale[i])) return 1;
    return spring_moving(&W.zoom) || spring_moving(&S.track) || (W.closing && !W.fired);
}

void ui_switch_reopen(void)
{
    W.closing = W.fired = 0;
    W.zoom.x = 0; W.zoom.v = 0; W.zoom.t = 1;
}

static void sw_select(int i)
{
    S.sel = i;
    S.track.t = SCREEN_W / 2.f - i * SW_STEP;
    for (int k = 0; k < S.lib->nrecent; k++) S.hscale[k].t = k == i ? 1.f : SW_SMALL;
    if (W.select) W.select(i);
}

int ui_init_switcher(Library *lib, const char *font_dir, int from_game, int running)
{
    if (ui_init(lib, font_dir)) return -1;
    W.on = 1;
    W.running = running;
    W.home_bg = S.bg;     // in the user's palette
    P = &PALETTES[1];     // the switcher is always dark: it sits over a dimmed game
    S.bg = NULL;
    if (build_bg()) { ui_free(); return -1; }
    for (int k = 0; k < lib->nrecent; k++) S.hscale[k].x = S.hscale[k].t = SW_SMALL;
    sw_select(0);
    S.track.x = S.track.t;
    S.hscale[0].x = 1;
    // Over a game, open from its frame filling the screen.
    W.zoom.x = from_game && running ? 0 : 1;
    W.zoom.t = 1;
    return 0;
}

static void sw_free(void)
{
    for (int i = 0; i < MAX_RECENT; i++) { img_free(W.sprites[i]); img_free(W.small[i]); }
    free(W.ring);
    img_free(W.home_bg);
    band_free(&W.top); band_free(&W.bottom);
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
    if (b == BTN_QUIT) { // the window closed: leave by the nearest exit
        if (W.running) { S.action = UI_SW_RESUME; W.close_to = 0; }
        else S.action = UI_SW_HOME;
    }
}

static void sw_update(float dt)
{
    spring_step(&W.zoom, dt, SNAPPY);
    spring_step(&S.track, dt, BOUNCE);
    for (int i = 0; i < S.lib->nrecent; i++) spring_step(&S.hscale[i], dt, 480.f, 40.f);
    if (W.closing == UI_SW_HOME) {
        W.home_t += dt / .25f;
        if (W.home_t >= 1 && !W.fired) { S.action = UI_SW_HOME; W.fired = 1; }
    } else if (W.closing && W.zoom.x < .004f && !W.fired) {
        S.action = W.closing;
        W.fired = 1;
    }
    sw_prebuild();
}

static void sw_draw_backdrop(Image *c)
{
    gfx_clear(c, rgb(0x14171f)); // one flat scrim: the screenshot shows in the focused card
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
    img_free(W.small[i]);
    W.small[i] = NULL;
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
        float a = 152, ax = sx + (SW_SHOT_W - a) / 2, ay = sy + (SW_SHOT_H - a) / 2;
        if (icon) gfx_blit_scaled(s, icon, ax, ay, a, a, 255);
        else draw_placeholder(s, g, ax, ay, a);
    }
    if (shot && sys->label[0]) { // system chip, bottom left
        int tw = text_width(S.small, sys->label), ch = font_height(S.small) + 4;
        float chy = sy + SW_SHOT_H - ch - 8;
        gfx_fill_rrect(s, sx + 8, chy, tw + 16, ch, ch / 2.f, rgb(sys->color));
        text_draw(s, S.small, sys->label, (int)sx + 16, (int)(chy + (ch - font_height(S.small)) / 2), rgb(0xffffff));
    }
    if (i == 0 && W.running) { // the paused game, chip on the top edge
        const char *t = "Paused";
        int tw = text_width(S.small, t), ch = font_height(S.small) + 8;
        float cx = x + SW_W / 2.f - (tw + 24) / 2.f, cy = y - 4;
        gfx_fill_rrect(s, cx - 4, cy - 4, tw + 32, ch + 8, (ch + 8) / 2.f, rgb(0xffffff));
        gfx_fill_rrect(s, cx, cy, tw + 24, ch, ch / 2.f, rgb(0x262a35));
        text_draw(s, S.small, t, (int)cx + 12, (int)(cy + (ch - font_height(S.small)) / 2), rgb(0xffffff));
    }
    img_find_spans(s);
    return s;
}

// A card at rest in the row (SW_SMALL), area-averaged once instead of scaled per frame.
static const Image *sw_small(int i, const Image *sprite)
{
    if (!W.small[i] && (W.small[i] = img_resize(sprite, (int)lroundf(sprite->w * SW_SMALL),
                                                (int)lroundf(sprite->h * SW_SMALL))))
        img_find_spans(W.small[i]);
    return W.small[i];
}

// A nearby card whose screenshot has loaded but whose sprite isn't built yet, or -1.
// Building one takes ~30 ms on the device, so it's done while the row is settled rather
// than in the first frame that scrolls the card into view.
#define SW_PREBUILD 3
static int sw_prebuild_next(void)
{
    if (!W.card || W.closing || sw_animating()) return -1;
    for (int d = 0; d <= SW_PREBUILD; d++)
        for (int side = -1; side <= 1; side += 2) {
            int i = S.sel + side * d;
            if (i < 0 || i >= S.lib->nrecent) continue;
            const Image *shot = W.card(i);
            if (shot && W.sprite_src[i] != shot) return i;
        }
    return -1;
}

static void sw_prebuild(void)
{
    int i = sw_prebuild_next();
    const Image *sprite = i >= 0 ? sw_sprite(i) : NULL;
    if (sprite) sw_small(i, sprite);
}

// The selection ring around a w x h card: stroked once at full card size, then 9-sliced
// to the size the card is animating through.
#define SW_RING_OUT 6  // ring's outer edge beyond the card
#define SW_RING_PAD 2
#define SW_RING_W (SW_W + 2 * (SW_RING_OUT + SW_RING_PAD))
#define SW_RING_H (SW_H + 2 * (SW_RING_OUT + SW_RING_PAD))
#define SW_RING_CORNER (SW_RING_PAD + 19 + 3)
static void sw_ring(Image *c, float x, float y, float w, float h, uint32_t color)
{
    if (!W.ring) {
        Image *m = img_new(SW_RING_W, SW_RING_H);
        W.ring = malloc(SW_RING_W * SW_RING_H);
        if (!m || !W.ring) { img_free(m); free(W.ring); W.ring = NULL; return; }
        gfx_stroke_rrect(m, SW_RING_PAD, SW_RING_PAD, SW_W + 2 * SW_RING_OUT, SW_H + 2 * SW_RING_OUT, 19, 4,
                         rgb(0xffffff));
        for (int i = 0; i < SW_RING_W * SW_RING_H; i++) W.ring[i] = m->px[i] >> 24;
        img_free(m);
    }
    const int edge = SW_RING_OUT + SW_RING_PAD;
    int x0 = (int)lroundf(x) - edge, y0 = (int)lroundf(y) - edge;
    int x1 = (int)lroundf(x + w) + edge, y1 = (int)lroundf(y + h) + edge;
    gfx_mask_a8_9slice(c, W.ring, SW_RING_W, SW_RING_H, SW_RING_CORNER, x0, y0, x1 - x0, y1 - y0, color);
}

static void sw_card_rect(int i, float *x, float *y, float *w, float *h)
{
    float k = S.hscale[i].x, cx = S.track.x + i * SW_STEP;
    *w = SW_W * k; *h = SW_H * k;
    *x = cx - *w / 2; *y = SW_CY - *h / 2;
}

// Everything over the backdrop but the cards, kept in two bands: along the top the
// status, save notification and the selection's title; along the bottom the position
// dots and the footer. Each is redrawn only when what it shows changes.
#define SW_TOP_H 120
#define SW_BOTTOM_Y 368
static void sw_draw_chrome(Image *c)
{
    Library *L = S.lib;
    int resume = S.sel == 0 && W.running;
    char key[256], status[200];
    status_key(status, sizeof status);
    snprintf(key, sizeof key, "%s|%d|%d|%s", status, S.sel, resume, W.notice);
    Image *b = band_begin(&W.top, SW_TOP_H, key);
    if (b) {
        if (L->nrecent) {
            const Game *g = &L->games[L->recent[S.sel]];
            char meta[160];
            snprintf(meta, sizeof meta, "%s%s", L->sys[g->sys].name, resume ? " · Paused" : "");
            text_center(b, S.title, g->title, SCREEN_W / 2, 44, rgb(0xffffff), SCREEN_W - 60);
            text_center(b, S.body, meta, SCREEN_W / 2, 88, argb(255, 0xb8, 0xbf, 0xd0), SCREEN_W - 60);
        }
        draw_status_raw(b);
        if (W.notice[0]) { // the question a confirming press answers
            int tw = text_width(S.small, W.notice), th = font_height(S.small) + 8;
            float tx = SCREEN_W / 2.f - (tw + 24) / 2.f;
            gfx_fill_rrect(b, tx, 8, tw + 24, th, th / 2.f, rgb(0xffffff));
            text_draw(b, S.small, W.notice, (int)tx + 12, 8 + (th - font_height(S.small)) / 2, rgb(0x262a35));
        }
        band_end(&W.top);
    }
    snprintf(key, sizeof key, "%d|%d|%d|%d|%s", S.sel, L->nrecent, resume, W.running, W.notice);
    if ((b = band_begin(&W.bottom, SCREEN_H - SW_BOTTOM_Y, key))) {
        const int y0 = SW_BOTTOM_Y;
        if (L->nrecent > 1) {
            float dw = 16, x0 = SCREEN_W / 2.f - (L->nrecent - 1) * dw / 2;
            for (int i = 0; i < L->nrecent; i++) {
                float sz = i == S.sel ? 12 : 8;
                gfx_fill_rrect(b, x0 + i * dw - sz / 2, 380 - y0 - sz / 2, sz, sz, sz / 2,
                               i == S.sel ? rgb(0xffffff) : argb(70, 255, 255, 255));
            }
        }
        // A question owns the footer while it is up: A answers yes, B no.
        const char *const ASK[] = {"A", "Yes", "B", "No"};
        const char *const GAME[] = {"A", resume ? "Resume" : "Play", "B", "Shelf", "SELECT", "Restart",
                                    "MENU", "Back"};
        const char *const OTHER[] = {"A", "Play", "B", "Shelf"};
        if (W.notice[0]) draw_footer_raw(b, SCREEN_H - FOOTER_H - y0, ASK, 4);
        else if (L->nrecent) draw_footer_raw(b, SCREEN_H - FOOTER_H - y0, GAME, W.running ? 8 : 4);
        else draw_footer_raw(b, SCREEN_H - FOOTER_H - y0, OTHER, 4);
        band_end(&W.bottom);
    }
    if (W.top.img) gfx_blit(c, W.top.img, 0, 0, 255);
    if (W.bottom.img) gfx_blit(c, W.bottom.img, 0, SW_BOTTOM_Y, 255);
}

// The row, titles and chrome, without the selected card when `skip_sel`.
static void sw_draw_scene(Image *c, int skip_sel)
{
    sw_draw_backdrop(c);
    Library *L = S.lib;
    if (!L->nrecent) {
        text_center(c, S.title, "Nothing played yet", SCREEN_W / 2, 200, rgb(C_INK), SCREEN_W - 60);
        text_center(c, S.body, "Games you play show up here", SCREEN_W / 2, 244, rgb(C_SUB), SCREEN_W - 60);
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
            const Image *rest = fabsf(k - SW_SMALL) < .004f ? sw_small(i, s) : NULL;
            if (fabsf(k - 1) < .004f) gfx_blit(c, s, (int)lroundf(x) - SW_SPRITE_PAD, (int)lroundf(y) - SW_SPRITE_PAD, 255);
            else if (rest) gfx_blit(c, rest, (int)lroundf(x + w / 2 - rest->w / 2.f), (int)lroundf(y + h / 2 - rest->h / 2.f), 255);
            else gfx_blit_scaled_nearest(c, s, x - pad, y - pad, s->w * k, s->h * k); // moving
            if (pass) sw_ring(c, x, y, w, h, rgb(C_SEL));
        }
    sw_draw_chrome(c);
}

static void sw_draw(Image *c)
{
    if (W.closing == UI_SW_HOME) {
        sw_draw_scene(S.layer, 0);
        W.layer_ok = 0;
        float a = W.home_t > 1 ? 1 : W.home_t;
        gfx_lerp(c, S.layer, W.home_bg, (uint8_t)(a * 255));
        return;
    }
    float z = W.zoom.x;
    if (z > .996f || !S.lib->nrecent) { sw_draw_scene(c, 0); W.layer_ok = 0; return; }
    // Zooming between the selected card and the full screen: the scene without that card
    // (kept while nothing in it moves), its white frame, and the screenshot scaled into it.
    int still = !spring_moving(&S.track);
    for (int i = 0; i < S.lib->nrecent && still; i++) still = !spring_moving(&S.hscale[i]);
    unsigned rev = (W.revision ? W.revision() : 0) * 31u + icons_revision();
    if (!W.layer_ok || !still || rev != W.layer_rev) {
        sw_draw_scene(S.layer, 1);
        W.layer_ok = 1;
        W.layer_rev = rev;
    }
    float x, y, w, h;
    sw_card_rect(S.sel, &x, &y, &w, &h);
    if (z < 0) z = 0;
    float rx = x * z, ry = y * z, rw = SCREEN_W + (w - SCREEN_W) * z, rh = SCREEN_H + (h - SCREEN_H) * z;
    float fr = SW_FRAME * (w / SW_W) * z;
    const Image *src = W.full ? W.full(S.sel) : NULL;
    if (!src && W.card) src = W.card(S.sel);
    if (src) {
        float sx = rx + fr, sy = ry + fr, sw = rw - 2 * fr, sh = rh - 2 * fr;
        gfx_fade_scaled_nearest(c, S.layer, src, sx, sy, sw, sh, 255);
        // The frame goes around the pixels the screenshot covered.
        if (z > .02f)
            gfx_fill_rrect_around(c, rx, ry, rw, rh, 14 * z, (int)floorf(sx), (int)floorf(sy),
                                  (int)ceilf(sx + sw), (int)ceilf(sy + sh), rgb(0xffffff));
    } else { // no screenshot: grow the card itself
        memcpy(c->px, S.layer->px, (size_t)SCREEN_W * SCREEN_H * 4);
        if (z > .02f) gfx_fill_rrect(c, rx, ry, rw, rh, 14 * z, rgb(0xffffff));
        const Image *s = sw_sprite(S.sel);
        float kx = rw / SW_W, ky = rh / SW_H;
        if (s) gfx_blit_scaled(c, s, rx - SW_SPRITE_PAD * kx, ry - SW_SPRITE_PAD * ky, s->w * kx, s->h * ky, 255);
    }
}
