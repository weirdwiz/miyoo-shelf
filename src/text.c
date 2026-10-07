#include "text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb_truetype.h"

#define CACHE_SIZE 512

typedef struct {
    int cp; // 0 = empty slot
    int w, h, xoff, yoff, advance;
    uint8_t *bitmap;
} Glyph;

struct Font {
    unsigned char *data;
    stbtt_fontinfo info;
    float scale;
    int ascent, descent;
    Glyph cache[CACHE_SIZE];
};

// Several Font sizes share one file in memory.
static struct { char path[512]; unsigned char *data; int refs; } files[4];

static unsigned char *file_acquire(const char *path)
{
    for (int i = 0; i < 4; i++)
        if (files[i].data && !strcmp(files[i].path, path)) { files[i].refs++; return files[i].data; }
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    unsigned char *data = malloc(n);
    if (data && fread(data, 1, n, fp) != (size_t)n) { free(data); data = NULL; }
    fclose(fp);
    if (!data) return NULL;
    for (int i = 0; i < 4; i++)
        if (!files[i].data) {
            snprintf(files[i].path, sizeof files[i].path, "%s", path);
            files[i].data = data;
            files[i].refs = 1;
            return data;
        }
    return data; // table full: leaks one file, harmless for a handful of fonts
}

static void file_release(unsigned char *data)
{
    for (int i = 0; i < 4; i++)
        if (files[i].data == data && --files[i].refs == 0) { free(data); files[i].data = NULL; }
}

Font *font_load(const char *path, float px)
{
    Font *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->data = file_acquire(path);
    if (!f->data || !stbtt_InitFont(&f->info, f->data, stbtt_GetFontOffsetForIndex(f->data, 0))) {
        fprintf(stderr, "shelf: cannot load font %s\n", path);
        free(f);
        return NULL;
    }
    f->scale = stbtt_ScaleForPixelHeight(&f->info, px);
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&f->info, &asc, &desc, &gap);
    f->ascent = (int)(asc * f->scale + 0.5f);
    f->descent = (int)(desc * f->scale - 0.5f);
    return f;
}

void font_free(Font *f)
{
    if (!f) return;
    for (int i = 0; i < CACHE_SIZE; i++) free(f->cache[i].bitmap);
    file_release(f->data);
    free(f);
}

int font_height(const Font *f) { return f->ascent - f->descent; }

static Glyph *glyph(Font *f, int cp)
{
    Glyph *g = &f->cache[(unsigned)cp % CACHE_SIZE];
    if (g->cp == cp) return g;
    free(g->bitmap);
    memset(g, 0, sizeof *g);
    g->cp = cp;
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&f->info, cp, &adv, &lsb);
    g->advance = (int)(adv * f->scale + 0.5f);
    g->bitmap = stbtt_GetCodepointBitmap(&f->info, 0, f->scale, cp, &g->w, &g->h, &g->xoff, &g->yoff);
    return g;
}

static int utf8_next(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    int cp, n;
    if (p[0] < 0x80) { cp = p[0]; n = 1; }
    else if ((p[0] & 0xe0) == 0xc0 && p[1]) { cp = ((p[0] & 0x1f) << 6) | (p[1] & 0x3f); n = 2; }
    else if ((p[0] & 0xf0) == 0xe0 && p[1] && p[2]) { cp = ((p[0] & 0x0f) << 12) | ((p[1] & 0x3f) << 6) | (p[2] & 0x3f); n = 3; }
    else if ((p[0] & 0xf8) == 0xf0 && p[1] && p[2] && p[3]) { cp = ((p[0] & 0x07) << 18) | ((p[1] & 0x3f) << 12) | ((p[2] & 0x3f) << 6) | (p[3] & 0x3f); n = 4; }
    else { cp = 0xfffd; n = 1; }
    *s += n;
    return cp;
}

int text_width(Font *f, const char *s)
{
    int w = 0, prev = 0;
    while (*s) {
        int cp = utf8_next(&s);
        if (prev) w += (int)(stbtt_GetCodepointKernAdvance(&f->info, prev, cp) * f->scale);
        w += glyph(f, cp)->advance;
        prev = cp;
    }
    return w;
}

int text_draw(Image *dst, Font *f, const char *s, int x, int y, uint32_t color)
{
    int pen = x, prev = 0, base = y + f->ascent;
    while (*s) {
        int cp = utf8_next(&s);
        if (prev) pen += (int)(stbtt_GetCodepointKernAdvance(&f->info, prev, cp) * f->scale);
        Glyph *g = glyph(f, cp);
        if (g->bitmap) gfx_mask_a8(dst, g->bitmap, g->w, g->h, pen + g->xoff, base + g->yoff, color);
        pen += g->advance;
        prev = cp;
    }
    return pen - x;
}

void text_fit(Font *f, const char *s, int max_w, char *out, int out_size)
{
    snprintf(out, out_size, "%s", s);
    if (text_width(f, out) <= max_w) return;
    int ell = text_width(f, "…");
    int n = (int)strlen(out);
    while (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)out[n] & 0xc0) == 0x80) n--; // keep UTF-8 whole
        out[n] = 0;
        if (text_width(f, out) + ell <= max_w) break;
    }
    while (n > 0 && out[n - 1] == ' ') out[--n] = 0;
    if (n + 4 <= out_size) strcat(out, "…");
}
