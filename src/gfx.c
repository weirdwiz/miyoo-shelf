#include "gfx.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_GIF
#include "../third_party/stb_image.h"

static inline uint32_t over(uint32_t d, uint32_t s)
{
    uint32_t a = s >> 24;
    if (a == 255) return s;
    if (a == 0) return d;
    uint32_t ia = 255 - a;
    uint32_t rb = ((d & 0x00ff00ffu) * ia + 0x00800080u);
    rb = ((rb + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
    uint32_t ag = (((d >> 8) & 0x00ff00ffu) * ia + 0x00800080u);
    ag = (ag + ((ag >> 8) & 0x00ff00ffu)) & 0xff00ff00u;
    return s + (rb | ag);
}

static inline uint32_t scale_px(uint32_t p, uint32_t k) // k in 0..255
{
    if (k == 255) return p;
    uint32_t rb = ((p & 0x00ff00ffu) * k + 0x00800080u);
    rb = ((rb + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
    uint32_t ag = (((p >> 8) & 0x00ff00ffu) * k + 0x00800080u);
    ag = (ag + ((ag >> 8) & 0x00ff00ffu)) & 0xff00ff00u;
    return rb | ag;
}

Image *img_new(int w, int h)
{
    Image *img = malloc(sizeof *img);
    if (!img) return NULL;
    img->w = w;
    img->h = h;
    img->px = calloc((size_t)w * h, 4);
    img->spans = NULL;
    if (!img->px) { free(img); return NULL; }
    return img;
}

void img_free(Image *img)
{
    if (!img) return;
    free(img->px);
    free(img->spans);
    free(img);
}

void img_find_spans(Image *img)
{
    if (!img->spans) img->spans = malloc(sizeof *img->spans * 2 * img->h);
    if (!img->spans) return;
    for (int y = 0; y < img->h; y++) {
        const uint32_t *row = img->px + y * img->w;
        int a = 0;
        while (a < img->w && row[a] >> 24 != 255) a++;
        int b = a;
        while (b < img->w && row[b] >> 24 == 255) b++;
        img->spans[2 * y] = (int16_t)a;
        img->spans[2 * y + 1] = (int16_t)b;
    }
}

Image *img_load(const char *path)
{
    int w, h, n;
    unsigned char *data = stbi_load(path, &w, &h, &n, 4);
    if (!data) return NULL;
    Image *img = img_new(w, h);
    if (img) {
        for (int i = 0; i < w * h; i++) {
            unsigned char *p = data + i * 4;
            img->px[i] = argb(p[3], p[0], p[1], p[2]);
        }
    }
    stbi_image_free(data);
    return img;
}

// Bilinear sample at continuous source coords (pixel centres at +0.5).
static uint32_t sample(const Image *s, float fx, float fy)
{
    fx -= 0.5f; fy -= 0.5f;
    if (fx < 0) fx = 0;
    if (fy < 0) fy = 0;
    int x0 = (int)fx, y0 = (int)fy;
    int x1 = x0 + 1 < s->w ? x0 + 1 : s->w - 1;
    int y1 = y0 + 1 < s->h ? y0 + 1 : s->h - 1;
    if (x0 >= s->w) x0 = x1 = s->w - 1;
    if (y0 >= s->h) y0 = y1 = s->h - 1;
    uint32_t tx = (uint32_t)((fx - x0) * 256), ty = (uint32_t)((fy - y0) * 256);
    uint32_t p[4] = {s->px[y0 * s->w + x0], s->px[y0 * s->w + x1], s->px[y1 * s->w + x0], s->px[y1 * s->w + x1]};
    uint32_t out = 0;
    for (int c = 0; c < 32; c += 8) {
        uint32_t a = (p[0] >> c) & 255, b = (p[1] >> c) & 255, d = (p[2] >> c) & 255, e = (p[3] >> c) & 255;
        uint32_t top = a * (256 - tx) + b * tx, bot = d * (256 - tx) + e * tx;
        out |= (((top * (256 - ty) + bot * ty) >> 16) & 255) << c;
    }
    return out;
}

Image *img_resize(const Image *src, int w, int h)
{
    Image *dst = img_new(w, h);
    if (!dst) return NULL;
    float sx = (float)src->w / w, sy = (float)src->h / h;
    if (sx <= 1.5f && sy <= 1.5f) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                dst->px[y * w + x] = sample(src, (x + 0.5f) * sx, (y + 0.5f) * sy);
        return dst;
    }
    for (int y = 0; y < h; y++) {
        int y0 = (int)(y * sy), y1 = (int)((y + 1) * sy);
        if (y1 <= y0) y1 = y0 + 1;
        if (y1 > src->h) y1 = src->h;
        for (int x = 0; x < w; x++) {
            int x0 = (int)(x * sx), x1 = (int)((x + 1) * sx);
            if (x1 <= x0) x1 = x0 + 1;
            if (x1 > src->w) x1 = src->w;
            uint32_t acc[4] = {0}, n = 0;
            for (int yy = y0; yy < y1; yy++)
                for (int xx = x0; xx < x1; xx++) {
                    uint32_t p = src->px[yy * src->w + xx];
                    acc[0] += p & 255; acc[1] += (p >> 8) & 255; acc[2] += (p >> 16) & 255; acc[3] += p >> 24;
                    n++;
                }
            dst->px[y * w + x] = (acc[0] / n) | ((acc[1] / n) << 8) | ((acc[2] / n) << 16) | ((acc[3] / n) << 24);
        }
    }
    return dst;
}

// Coverage of pixel (px,py) inside a rounded rect, 0..1.
static float rrect_cov(float px, float py, float x, float y, float w, float h, float r)
{
    float cx = px < x + r ? x + r : (px > x + w - r ? x + w - r : px);
    float cy = py < y + r ? y + r : (py > y + h - r ? y + h - r : py);
    float dx = px - cx, dy = py - cy;
    // Interior strips are fully covered; reserve square roots for corner edges.
    if (r >= .5f && (dx == 0 || dy == 0) &&
        px >= x + 1 && px <= x + w - 1 &&
        py >= y + 1 && py <= y + h - 1)
        return 1;
    float d = sqrtf(dx * dx + dy * dy) - r; // signed distance to edge for corners
    if (px < x || px > x + w || py < y || py > y + h) {
        float ex = px < x ? x - px : (px > x + w ? px - x - w : 0);
        float ey = py < y ? y - py : (py > y + h ? py - y - h : 0);
        float e = ex > ey ? ex : ey;
        if (d < e) d = e;
    }
    float c = 0.5f - d;
    return c < 0 ? 0 : (c > 1 ? 1 : c);
}

void img_round(Image *img, float r)
{
    for (int y = 0; y < img->h; y++)
        for (int x = 0; x < img->w; x++) {
            float c = rrect_cov(x + 0.5f, y + 0.5f, 0, 0, img->w, img->h, r);
            if (c < 1) img->px[y * img->w + x] = scale_px(img->px[y * img->w + x], (uint32_t)(c * 255));
        }
}

int img_save_raw(const Image *img, const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int32_t hdr[3] = {0x464c4853 /* SHLF */, img->w, img->h};
    int ok = fwrite(hdr, sizeof hdr, 1, f) == 1 && fwrite(img->px, 4, (size_t)img->w * img->h, f) == (size_t)img->w * img->h;
    fclose(f);
    return ok ? 0 : -1;
}

Image *img_load_raw(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    int32_t hdr[3];
    Image *img = NULL;
    if (fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == 0x464c4853 && hdr[1] > 0 && hdr[2] > 0 && hdr[1] <= 2048 && hdr[2] <= 2048) {
        img = img_new(hdr[1], hdr[2]);
        if (img && fread(img->px, 4, (size_t)img->w * img->h, f) != (size_t)img->w * img->h) {
            img_free(img);
            img = NULL;
        }
    }
    fclose(f);
    return img;
}

void gfx_clear(Image *dst, uint32_t c)
{
    for (int i = 0; i < dst->w * dst->h; i++) dst->px[i] = c;
}

void gfx_fill_rect(Image *dst, int x, int y, int w, int h, uint32_t c)
{
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > dst->w ? dst->w : x + w, y1 = y + h > dst->h ? dst->h : y + h;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) dst->px[yy * dst->w + xx] = over(dst->px[yy * dst->w + xx], c);
}

void gfx_fill_rrect(Image *dst, float x, float y, float w, float h, float r, uint32_t c)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int x0 = (int)floorf(x), y0 = (int)floorf(y), x1 = (int)ceilf(x + w), y1 = (int)ceilf(y + h);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > dst->w) x1 = dst->w;
    if (y1 > dst->h) y1 = dst->h;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
            float cv = rrect_cov(xx + 0.5f, yy + 0.5f, x, y, w, h, r);
            if (cv <= 0) continue;
            uint32_t *d = &dst->px[yy * dst->w + xx];
            *d = over(*d, cv >= 1 ? c : scale_px(c, (uint32_t)(cv * 255)));
        }
}

void gfx_stroke_rrect(Image *dst, float x, float y, float w, float h, float r, float t, uint32_t c)
{
    int x0 = (int)floorf(x), y0 = (int)floorf(y), x1 = (int)ceilf(x + w), y1 = (int)ceilf(y + h);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > dst->w) x1 = dst->w;
    if (y1 > dst->h) y1 = dst->h;
    float ri = r - t < 0 ? 0 : r - t;
    for (int yy = y0; yy < y1; yy++)
        for (int xx = x0; xx < x1; xx++) {
            float px = xx + 0.5f, py = yy + 0.5f;
            float cv = rrect_cov(px, py, x, y, w, h, r) - rrect_cov(px, py, x + t, y + t, w - 2 * t, h - 2 * t, ri);
            if (cv <= 0) continue;
            uint32_t *d = &dst->px[yy * dst->w + xx];
            *d = over(*d, cv >= 1 ? c : scale_px(c, (uint32_t)(cv * 255)));
        }
}

void gfx_blit(Image *dst, const Image *src, int x, int y, uint8_t alpha)
{
    int sx0 = x < 0 ? -x : 0, sy0 = y < 0 ? -y : 0;
    int sx1 = x + src->w > dst->w ? dst->w - x : src->w;
    int sy1 = y + src->h > dst->h ? dst->h - y : src->h;
    for (int sy = sy0; sy < sy1; sy++) {
        const uint32_t *s = src->px + sy * src->w;
        uint32_t *d = dst->px + (y + sy) * dst->w + x;
        if (alpha == 255 && src->spans) {
            // Blend the soft edges, copy the opaque middle.
            int a = src->spans[2 * sy], b = src->spans[2 * sy + 1];
            if (a < sx0) a = sx0;
            if (b > sx1) b = sx1;
            if (a >= b) a = b = sx1;
            for (int sx = sx0; sx < a; sx++) d[sx] = over(d[sx], s[sx]);
            memcpy(d + a, s + a, (size_t)(b - a) * sizeof *d);
            for (int sx = b; sx < sx1; sx++) d[sx] = over(d[sx], s[sx]);
            continue;
        }
        for (int sx = sx0; sx < sx1; sx++) d[sx] = over(d[sx], alpha == 255 ? s[sx] : scale_px(s[sx], alpha));
    }
}

void gfx_blit_scaled(Image *dst, const Image *src, float x, float y, float w, float h, uint8_t alpha)
{
    if (w <= 0 || h <= 0) return;
    if (fabsf(w - src->w) < 0.5f && fabsf(h - src->h) < 0.5f) {
        gfx_blit(dst, src, (int)lroundf(x), (int)lroundf(y), alpha);
        return;
    }
    int x0 = (int)floorf(x), y0 = (int)floorf(y), x1 = (int)ceilf(x + w), y1 = (int)ceilf(y + h);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > dst->w) x1 = dst->w;
    if (y1 > dst->h) y1 = dst->h;
    if (x1 <= x0 || y1 <= y0) return;
    float kx = src->w / w, ky = src->h / h;
    // Separable bilinear sampling: compute horizontal coordinates once per column,
    // and vertical coordinates once per row. UI canvases bound this stack map.
    struct Column { int a, b; uint32_t weight; int valid; } columns[x1 - x0];
    for (int xx = x0; xx < x1; xx++) {
        struct Column *col = &columns[xx - x0];
        float fx = (xx + .5f - x) * kx;
        col->valid = fx >= 0 && fx < src->w;
        if (!col->valid) continue;
        fx -= .5f;
        if (fx < 0) fx = 0;
        col->a = (int)fx;
        col->b = col->a + 1 < src->w ? col->a + 1 : src->w - 1;
        if (col->a >= src->w) col->a = col->b = src->w - 1;
        col->weight = (uint32_t)((fx - col->a) * 256);
    }
    for (int yy = y0; yy < y1; yy++) {
        float fy = (yy + .5f - y) * ky;
        if (fy < 0 || fy >= src->h) continue;
        fy -= .5f;
        if (fy < 0) fy = 0;
        int ya = (int)fy, yb = ya + 1 < src->h ? ya + 1 : src->h - 1;
        if (ya >= src->h) ya = yb = src->h - 1;
        uint32_t ty = (uint32_t)((fy - ya) * 256), iy = 256 - ty;
        const uint32_t *top = src->px + ya * src->w, *bottom = src->px + yb * src->w;
        uint32_t *row = dst->px + yy * dst->w;
        for (int xx = x0; xx < x1; xx++) {
            const struct Column *col = &columns[xx - x0];
            if (!col->valid) continue;
            uint32_t tx = col->weight, ix = 256 - tx;
            uint32_t a = top[col->a], b = top[col->b], d = bottom[col->a], e = bottom[col->b];
            // Two 16-bit lanes retain the original sampler's precision/rounding.
            uint32_t trb = (a & 0x00ff00ffu) * ix + (b & 0x00ff00ffu) * tx;
            uint32_t brb = (d & 0x00ff00ffu) * ix + (e & 0x00ff00ffu) * tx;
            uint32_t tag = ((a >> 8) & 0x00ff00ffu) * ix + ((b >> 8) & 0x00ff00ffu) * tx;
            uint32_t bag = ((d >> 8) & 0x00ff00ffu) * ix + ((e >> 8) & 0x00ff00ffu) * tx;
            uint32_t blue = ((trb & 65535) * iy + (brb & 65535) * ty) >> 16;
            uint32_t red = ((trb >> 16) * iy + (brb >> 16) * ty) >> 16;
            uint32_t green = ((tag & 65535) * iy + (bag & 65535) * ty) >> 16;
            uint32_t opacity = ((tag >> 16) * iy + (bag >> 16) * ty) >> 16;
            uint32_t p = blue | (green << 8) | (red << 16) | (opacity << 24);
            row[xx] = over(row[xx], alpha == 255 ? p : scale_px(p, alpha));
        }
    }
}

void gfx_blit_scaled_nearest(Image *dst, const Image *src, float x, float y, float w, float h, uint8_t alpha)
{
    if (w <= 0 || h <= 0) return;
    int x0 = (int)floorf(x), y0 = (int)floorf(y), x1 = (int)ceilf(x + w), y1 = (int)ceilf(y + h);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > dst->w) x1 = dst->w;
    if (y1 > dst->h) y1 = dst->h;
    if (x1 <= x0 || y1 <= y0) return;
    float kx = src->w / w, ky = src->h / h;
    int columns[x1 - x0];
    for (int xx = x0; xx < x1; xx++) {
        int sx = (int)((xx + .5f - x) * kx);
        columns[xx - x0] = sx < 0 ? 0 : sx >= src->w ? src->w - 1 : sx;
    }
    for (int yy = y0; yy < y1; yy++) {
        int sy = (int)((yy + .5f - y) * ky);
        if (sy < 0 || sy >= src->h) continue;
        const uint32_t *s = src->px + sy * src->w;
        uint32_t *row = dst->px + yy * dst->w;
        for (int xx = x0; xx < x1; xx++) {
            uint32_t p = s[columns[xx - x0]];
            row[xx] = over(row[xx], alpha == 255 ? p : scale_px(p, alpha));
        }
    }
}

void gfx_mask_a8(Image *dst, const uint8_t *mask, int mw, int mh, int x, int y, uint32_t c)
{
    for (int my = 0; my < mh; my++) {
        int yy = y + my;
        if (yy < 0 || yy >= dst->h) continue;
        for (int mx = 0; mx < mw; mx++) {
            int xx = x + mx;
            if (xx < 0 || xx >= dst->w) continue;
            uint8_t a = mask[my * mw + mx];
            if (!a) continue;
            uint32_t *d = &dst->px[yy * dst->w + xx];
            *d = over(*d, scale_px(c, a));
        }
    }
}

void gfx_mask_a8_9slice(Image *dst, const uint8_t *mask, int mw, int mh, int corner,
                        int x, int y, int w, int h, uint32_t c)
{
    if (w < 2 * corner || h < 2 * corner) return;
    for (int yy = 0; yy < h; yy++) {
        int dy = y + yy;
        if (dy < 0 || dy >= dst->h) continue;
        int edge = yy < corner || yy >= h - corner;
        int sy = yy < corner ? yy : yy >= h - corner ? mh - (h - yy) : corner;
        const uint8_t *row = mask + sy * mw;
        uint32_t *d = dst->px + dy * dst->w;
        for (int xx = 0; xx < w; xx++) {
            if (!edge && xx == corner) xx = w - corner; // empty middle of a side row
            int dx = x + xx;
            if (dx < 0 || dx >= dst->w) continue;
            int sx = xx < corner ? xx : xx >= w - corner ? mw - (w - xx) : corner;
            uint8_t a = row[sx];
            if (a) d[dx] = over(d[dx], scale_px(c, a));
        }
    }
}
