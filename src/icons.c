#include "icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CACHE_VERSION 1

typedef struct {
    Image *img[2];   // [0] small, [1] big
    char want[2];    // requested this session
    char missing;    // no source art: caller draws a placeholder
} Entry;

static Library *L;
static Entry *E;
static char cache_dir[PATH_LEN];

static void mkdirs(const char *path)
{
    char p[PATH_LEN];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(p, 0755); *c = '/'; }
    mkdir(p, 0755);
}

void icons_init(Library *lib)
{
    L = lib;
    E = calloc(lib->ngames ? lib->ngames : 1, sizeof *E);
    host_path(SD_PREFIX "/App/Shelf/cache", cache_dir, sizeof cache_dir);
    mkdirs(cache_dir);
}

void icons_free(void)
{
    if (!E) return;
    for (int i = 0; i < L->ngames; i++) { img_free(E[i].img[0]); img_free(E[i].img[1]); }
    free(E);
    E = NULL;
}

const Image *icon_get(int game, int big)
{
    Entry *e = &E[game];
    if (!e->img[big] && !e->missing) e->want[big] = 1;
    return e->img[big];
}

// Cover-crop src into a w*h image.
static Image *cover(const Image *src, int w, int h)
{
    float k = (float)w / src->w > (float)h / src->h ? (float)w / src->w : (float)h / src->h;
    int cw = (int)(w / k), ch = (int)(h / k);
    int ox = (src->w - cw) / 2, oy = (src->h - ch) / 2;
    Image crop = {cw, ch, malloc((size_t)cw * ch * 4)};
    if (!crop.px) return NULL;
    for (int y = 0; y < ch; y++) memcpy(crop.px + y * cw, src->px + (oy + y) * src->w + ox, (size_t)cw * 4);
    Image *out = img_resize(&crop, w, h);
    free(crop.px);
    return out;
}

static Image *blur_fill(const Image *src, int size)
{
    // Background: tiny cover crop, blurred, scaled back up, darkened a touch.
    int small = size / 6;
    Image *bg_small = cover(src, small, small);
    if (!bg_small) return NULL;
    img_blur(bg_small, 2, 3);
    Image *out = img_resize(bg_small, size, size);
    img_free(bg_small);
    if (!out) return NULL;
    gfx_fill_rect(out, 0, 0, size, size, argb(40, 0, 0, 0));

    // Foreground: whole box art, contained.
    float k = (float)size / src->w < (float)size / src->h ? (float)size / src->w : (float)size / src->h;
    int fw = (int)(src->w * k + 0.5f), fh = (int)(src->h * k + 0.5f);
    Image *fg = img_resize(src, fw, fh);
    if (fg) {
        gfx_blit(out, fg, (size - fw) / 2, (size - fh) / 2, 255);
        img_free(fg);
    }
    img_round(out, size * 0.085f);
    return out;
}

static void cache_path(const Game *g, int size, char *out, int n)
{
    uint32_t h = 2166136261u;
    for (const char *c = g->path; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
    snprintf(out, n, "%s/%08x-%d-v%d.raw", cache_dir, h, size, CACHE_VERSION);
}

static int newer(const char *a, const char *b) // a modified after b, or b missing
{
    struct stat sa, sb;
    if (stat(b, &sb)) return 1;
    if (stat(a, &sa)) return 0;
    return sa.st_mtime > sb.st_mtime;
}

static void build(int gi, int big)
{
    Entry *e = &E[gi];
    Game *g = &L->games[gi];
    int size = big ? ICON_BIG : ICON_SMALL;
    char src_path[PATH_LEN], raw[PATH_LEN];
    if (lib_img_path(L, g, src_path, sizeof src_path)) { e->missing = 1; return; }
    cache_path(g, size, raw, sizeof raw);
    if (!newer(src_path, raw) && (e->img[big] = img_load_raw(raw))) return;
    Image *src = img_load(src_path);
    if (!src) { e->missing = 1; return; }
    e->img[big] = blur_fill(src, size);
    img_free(src);
    if (e->img[big]) img_save_raw(e->img[big], raw);
}

int icons_pump(int budget)
{
    int made = 0;
    for (int i = 0; i < L->ngames && made < budget; i++)
        for (int b = 1; b >= 0 && made < budget; b--)
            if (E[i].want[b] && !E[i].img[b] && !E[i].missing) {
                build(i, b);
                E[i].want[b] = 0;
                made++;
            }
    return made;
}
