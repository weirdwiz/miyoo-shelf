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
#define G_TILE 92
#define G_COL 108
#define G_ROW 104
#define G_X 24
#define G_Y 108
#define G_ROWS 3
#define G_VIS 5

typedef struct { float x, v, t; } Spring;

static void spring_step(Spring *s, float dt, float k, float c)
{
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
    Image *bg, *layer;
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
} S;

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
    snprintf(bold, sizeof bold, "%s/MPLUSRounded1c-Bold.ttf", font_dir);
    snprintf(xbold, sizeof xbold, "%s/MPLUSRounded1c-ExtraBold.ttf", font_dir);
    S.title = font_load(xbold, 27);
    S.label = font_load(xbold, 15);
    S.body = font_load(bold, 15);
    S.small = font_load(bold, 13);
    if (!S.title || !S.body || !S.small || !S.label) return -1;
    build_bg();
    S.layer = img_new(SCREEN_W, SCREEN_H);

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

void ui_button(Button b)
{
    if (S.launching >= 0) return;
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

void ui_update(float dt)
{
    S.t += dt;
    spring_step(&S.enter, dt, SNAPPY);
    spring_step(&S.track, dt, BOUNCE);
    for (int i = 0; i < home_count(); i++) spring_step(&S.hscale[i], dt, 400.f, 18.f);
    if (S.view == V_FOLDER) {
        spring_step(&S.ftrack, dt, BOUNCE);
        for (int i = 0; i < S.nfg; i++) spring_step(&S.fscale[i], dt, 400.f, 18.f);
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
    char line[128] = "";
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

static void draw_game(Image *c, int gi, float cx, float cy, float w, int big)
{
    float x = cx - w / 2, y = cy - w / 2;
    gfx_fill_rrect(c, x + 1, y + 4, w - 2, w - 1, w * .085f, argb(38, 20, 30, 60));
    const Image *ic = icon_get(gi, big);
    if (ic) gfx_blit_scaled(c, ic, x, y, w, w, 255);
    else draw_placeholder(c, &S.lib->games[gi], x, y, w);
}

static void draw_folder(Image *c, const Folder *f, float cx, float cy, float w)
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
    float o = 7 * w / TILE + 1;
    gfx_stroke_rrect(c, cx - w / 2 - o, cy - w / 2 - o, w + 2 * o, w + 2 * o, w * .085f + o, 4.5f, lerp_rgb(C_SEL, C_SEL2, p));
    gfx_stroke_rrect(c, cx - w / 2 - o + 4, cy - w / 2 - o + 4, w + 2 * o - 8, w + 2 * o - 8, w * .085f + o - 4, 2.5f, argb(230, 255, 255, 255));
}

static void draw_status(Image *c)
{
    char buf[64];
    time_t now = time(NULL);
    strftime(buf, sizeof buf, "%a %d %b · %H:%M", localtime(&now));
    text_draw(c, S.small, buf, 16, 7, rgb(C_SUB));
}

static void draw_footer(Image *c, const char *const *hints, int n)
{
    gfx_fill_rect(c, 0, SCREEN_H - 38, SCREEN_W, 38, argb(190, 255, 255, 255));
    gfx_fill_rect(c, 0, SCREEN_H - 38, SCREEN_W, 1, rgb(0xdde1ea));
    int x = 16, y = SCREEN_H - 29;
    for (int i = 0; i < n; i += 2) {
        gfx_fill_rrect(c, x, y, 20, 20, 10, rgb(0x3a3f4d));
        text_center(c, S.small, hints[i], x + 10, y + 1, rgb(0xffffff), 20);
        x += 26;
        x += text_draw(c, S.small, hints[i + 1], x, y + 1, rgb(C_SUB)) + 20;
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
        text_center(c, S.title, g->title, SCREEN_W / 2, 50, rgb(C_INK), SCREEN_W - 60);
    } else {
        const Folder *f = &S.folders[S.sel - L->nrecent];
        snprintf(meta, sizeof meta, "Folder · %d games", f->ngames);
        text_center(c, S.title, f->label, SCREEN_W / 2, 50, rgb(C_INK), SCREEN_W - 60);
    }
    text_center(c, S.body, meta, SCREEN_W / 2, 88, rgb(C_SUB), SCREEN_W - 60);
    text_draw(c, S.small, L->nrecent && !inlib ? "RECENT" : "LIBRARY", 18, 116, rgb(C_SUB));

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
            if (i == S.sel) draw_cursor(c, cx, cy, w);
            if (i >= L->nrecent) draw_folder_badge(c, &S.folders[i - L->nrecent], cx, cy, w);
        }

    // Position dots: round for Recent, square for Library.
    float dw = 15, x0 = SCREEN_W / 2.f - (n - 1) * dw / 2;
    for (int i = 0; i < n; i++) {
        float s = i == S.sel ? 12 : 8, x = x0 + i * dw - s / 2, y = 354 - s / 2;
        gfx_fill_rrect(c, x, y, s, s, i < L->nrecent ? s / 2 : 2.5f, rgb(i == S.sel ? C_SEL : 0xc9cfda));
    }
    static const char *const H[] = {"A", "Open", "L", "Recent", "R", "Library"};
    draw_footer(c, H, 6);
}

static void draw_folder_view(Image *c)
{
    const Folder *f = &S.folders[S.folder];
    gfx_fill_rrect(c, 24, 40, 44, 44, 11, rgb(f->color));
    text_draw(c, S.label, f->label, 80, 40, rgb(C_INK));
    char n[48];
    snprintf(n, sizeof n, "%d games", f->ngames);
    text_draw(c, S.small, n, 80, 62, rgb(C_SUB));
    int x = SCREEN_W - 24;
    for (int i = 2; i >= 0; i--) {
        int w = text_width(S.small, SORTS[i]) + 20;
        x -= w;
        gfx_fill_rrect(c, x, 50, w, 24, 12, rgb(i == S.sort ? C_INK : 0xffffff));
        text_draw(c, S.small, SORTS[i], x + 10, 53, rgb(i == S.sort ? 0xffffff : C_SUB));
        x -= 6;
    }
    for (int pass = 0; pass < 2; pass++)
        for (int k = 0; k < S.nfg; k++) {
            if ((k == S.fsel) != pass) continue;
            float cx = S.ftrack.x + (k / G_ROWS) * G_COL + G_TILE / 2.f, cy = G_Y + (k % G_ROWS) * G_ROW + G_TILE / 2.f;
            if (cx < -G_TILE || cx > SCREEN_W + G_TILE) continue;
            float w = G_TILE * S.fscale[k].x;
            draw_game(c, S.fgames[k], cx, cy, w, 0);
            if (k == S.fsel) draw_cursor(c, cx, cy, w);
        }
    if (S.nfg) {
        const Game *g = &S.lib->games[S.fgames[S.fsel]];
        char name[200];
        snprintf(name, sizeof name, "%s · %s", g->title, S.lib->sys[g->sys].label);
        text_center(c, S.label, name, SCREEN_W / 2, 416, rgb(C_INK), SCREEN_W - 48);
    } else {
        text_center(c, S.body, "Nothing here yet", SCREEN_W / 2, 220, rgb(C_SUB), SCREEN_W);
    }
    static const char *const H[] = {"A", "Play", "B", "Back", "Y", "Sort"};
    draw_footer(c, H, 6);
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
    gfx_blit(c, S.bg, 0, 0, 255);
    float e = S.enter.x;
    if (e > 0.995f) {
        if (S.view == V_HOME) draw_home(c); else draw_folder_view(c);
    } else {
        // Zoom the new view in from 92%.
        memcpy(S.layer->px, S.bg->px, (size_t)SCREEN_W * SCREEN_H * 4);
        if (S.view == V_HOME) draw_home(S.layer); else draw_folder_view(S.layer);
        float k = .92f + .08f * e, w = SCREEN_W * k, h = SCREEN_H * k;
        float a = e < 0 ? 0 : (e > 1 ? 1 : e);
        gfx_blit_scaled(c, S.layer, (SCREEN_W - w) / 2, (SCREEN_H - h) / 2, w, h, (uint8_t)(a * 255));
    }
    draw_status(c);
    if (S.launching >= 0) draw_launch(c);
}
