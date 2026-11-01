#include "scale.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ARM_NEON) && !defined(SCALE_NO_NEON)
#include <arm_neon.h>
#define SCALE_NEON 1
#else
#define SCALE_NEON 0
#endif

ScaleRect scale_rect(int sw, int sh, float aspect, int dw, int dh, ScaleMode mode)
{
    ScaleRect r = {0, 0, dw, dh};
    if (sw <= 0 || sh <= 0) return r;
    if (mode == SCALE_INTEGER) {
        int k = dw / sw < dh / sh ? dw / sw : dh / sh;
        if (k < 1) k = 1;
        r.w = sw * k;
        r.h = sh * k;
    } else {
        if (aspect <= 0) aspect = (float)sw / sh;
        r.w = dw;
        r.h = (int)lroundf(dw / aspect);
        if (r.h > dh) {
            r.h = dh;
            r.w = (int)lroundf(dh * aspect);
        }
    }
    if (r.w > dw) r.w = dw;
    if (r.h > dh) r.h = dh;
    r.x = (dw - r.w) / 2;
    r.y = (dh - r.h) / 2;
    return r;
}

// Per output column (or row): the two source texels and the second one's weight, 0..256.
typedef struct { int i0, i1, w; } Tap;

static struct {
    Tap *x, *y;
    int sw, sh, rw, rh, mode;
    uint32_t *rows[2]; // horizontally scaled source rows
    int row_of[2];     // which source row each holds, -1 none
} T = {.row_of = {-1, -1}};

void scale_free(void)
{
    free(T.x);
    free(T.y);
    free(T.rows[0]);
    free(T.rows[1]);
    memset(&T, 0, sizeof T);
    T.row_of[0] = T.row_of[1] = -1;
}

static void make_taps(Tap *t, int n, int src, ScaleMode mode)
{
    float scale = (float)n / src;
    for (int i = 0; i < n; i++) {
        float u = (i + .5f) / scale - .5f;
        if (mode == SCALE_INTEGER) { // nearest
            int k = (int)((i + .5f) / scale);
            t[i] = (Tap){k, k, 0};
            continue;
        }
        int k = (int)floorf(u);
        float f = u - k;
        if (mode == SCALE_SHARP && scale > 1) {
            // Sharp bilinear: f is the distance from texel k's centre, so the edge
            // with k+1 is at .5. Stretching around it by the scale keeps each texel's
            // middle solid and blends only the output pixel or so on the edge.
            f = (f - .5f) * scale + .5f;
            f = f < 0 ? 0 : f > 1 ? 1 : f;
        }
        int w = (int)lroundf(f * 256);
        int i0 = k < 0 ? 0 : k >= src ? src - 1 : k;
        int i1 = k + 1 < 0 ? 0 : k + 1 >= src ? src - 1 : k + 1;
        if (w <= 0) { w = 0; i1 = i0; }
        if (w >= 256) { w = 0; i0 = i1; }
        t[i] = (Tap){i0, i1, w};
    }
}

static int prepare(int sw, int sh, int rw, int rh, ScaleMode mode)
{
    if (T.x && T.sw == sw && T.sh == sh && T.rw == rw && T.rh == rh && T.mode == (int)mode)
        return 0;
    scale_free();
    T.x = malloc(sizeof *T.x * rw);
    T.y = malloc(sizeof *T.y * rh);
    T.rows[0] = malloc(sizeof(uint32_t) * rw);
    T.rows[1] = malloc(sizeof(uint32_t) * rw);
    if (!T.x || !T.y || !T.rows[0] || !T.rows[1]) { scale_free(); return -1; }
    make_taps(T.x, rw, sw, mode);
    make_taps(T.y, rh, sh, mode);
    T.sw = sw; T.sh = sh; T.rw = rw; T.rh = rh; T.mode = mode;
    return 0;
}

static inline uint32_t lerp_px(uint32_t a, uint32_t b, int w)
{
    uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8;
    uint32_t g = ((a & 0x00ff00) * (256 - w) + (b & 0x00ff00) * w) >> 8;
    return (rb & 0xff00ff) | (g & 0x00ff00);
}

// The source row scaled to the output width, from the two-row cache. The taps pick a
// different pair of source texels per output column, so this is a gather that ARMv7 NEON
// cannot do in a vector load. It stays scalar: one blend per column is cheap next to the
// vertical pass below, and it only runs when a source row enters the cache, which spreads
// its ~27% of a frame's pixels over the whole frame instead of the hot loop.
static const uint32_t *hrow(const uint32_t *src, int spitch, int sy)
{
    for (int k = 0; k < 2; k++)
        if (T.row_of[k] == sy) return T.rows[k];
    // Rows are consumed in ascending order: replace the older (lower) one.
    int k = T.row_of[0] < T.row_of[1] ? 0 : 1;
    uint32_t *out = T.rows[k];
    const uint32_t *s = src + (size_t)sy * spitch;
    for (int x = 0; x < T.rw; x++) {
        Tap t = T.x[x];
        out[x] = t.w ? lerp_px(s[t.i0], s[t.i1], t.w) : s[t.i0] & 0xffffff;
    }
    T.row_of[k] = sy;
    return out;
}

#if SCALE_NEON
// Four lanes in reverse order, for the 180 degree rotation: the destination row is written
// back to front, so a vector computed in ascending order has to be stored descending.
static inline uint32x4_t rev4(uint32x4_t v)
{
    uint32x4_t r = vrev64q_u32(v); // [1,0,3,2]
    return vcombine_u32(vget_high_u32(r), vget_low_u32(r)); // [3,2,1,0]
}

// The same fixed-point blend as lerp_px, four pixels at a time. Each masked product needs 16
// bits, so blue's carry and red's leftover low byte both land in the green byte: the shift
// does not separate the channels, the masks do. That makes lerp_px's `& 0xff00ff` and
// `& 0x00ff00` load-bearing rather than tidying up.
static inline uint32x4_t lerp4(uint32x4_t a, uint32x4_t b, uint32x4_t w, uint32x4_t iw)
{
    const uint32x4_t mrb = vdupq_n_u32(0x00ff00ff), mg = vdupq_n_u32(0x0000ff00);
    uint32x4_t rb = vmlaq_u32(vmulq_u32(vandq_u32(a, mrb), iw), vandq_u32(b, mrb), w);
    uint32x4_t g = vmlaq_u32(vmulq_u32(vandq_u32(a, mg), iw), vandq_u32(b, mg), w);
    return vorrq_u32(vandq_u32(vshrq_n_u32(rb, 8), mrb), vandq_u32(vshrq_n_u32(g, 8), mg));
}
#endif

// out[x] = lerp_px(a[x], b[x], w) for x < n.
static void lerp_row(uint32_t *out, const uint32_t *a, const uint32_t *b, int n, int w)
{
    int x = 0;
#if SCALE_NEON
    const uint32x4_t vw = vdupq_n_u32((uint32_t)w), viw = vdupq_n_u32(256u - (uint32_t)w);
    for (; x + 4 <= n; x += 4)
        vst1q_u32(out + x, lerp4(vld1q_u32(a + x), vld1q_u32(b + x), vw, viw));
#endif
    for (; x < n; x++) out[x] = lerp_px(a[x], b[x], w);
}

// The rotated row: a[x] lands on out[-x], so `out` points at the last destination pixel.
static void lerp_row_back(uint32_t *out, const uint32_t *a, const uint32_t *b, int n, int w)
{
    int x = 0;
#if SCALE_NEON
    const uint32x4_t vw = vdupq_n_u32((uint32_t)w), viw = vdupq_n_u32(256u - (uint32_t)w);
    for (; x + 4 <= n; x += 4)
        vst1q_u32(out - x - 3, rev4(lerp4(vld1q_u32(a + x), vld1q_u32(b + x), vw, viw)));
#endif
    for (; x < n; x++) out[-x] = lerp_px(a[x], b[x], w);
}

// The rotated copy: a[x] lands on out[-x].
static void copy_row_back(uint32_t *out, const uint32_t *a, int n)
{
    int x = 0;
#if SCALE_NEON
    for (; x + 4 <= n; x += 4) vst1q_u32(out - x - 3, rev4(vld1q_u32(a + x)));
#endif
    for (; x < n; x++) out[-x] = a[x];
}

int scale_blit(uint32_t *dst, int dpitch, int dw, int dh, const uint32_t *src, int sw, int sh,
               int spitch, ScaleRect r, ScaleMode mode, int rotate)
{
    if (r.w <= 0 || r.h <= 0 || r.x < 0 || r.y < 0 || r.x + r.w > dw || r.y + r.h > dh) return -1;
    if (prepare(sw, sh, r.w, r.h, mode)) return -1;
    T.row_of[0] = T.row_of[1] = -1; // a new frame
    for (int y = 0; y < r.h; y++) {
        Tap t = T.y[y];
        const uint32_t *a = hrow(src, spitch, t.i0);
        uint32_t *out;
        if (!rotate) {
            out = dst + (size_t)(r.y + y) * dpitch + r.x;
            if (!t.w) { memcpy(out, a, sizeof *out * r.w); continue; }
            lerp_row(out, a, hrow(src, spitch, t.i1), r.w, t.w);
        } else {
            // Upright (x, y) lands on (dw-1-x, dh-1-y): rows from the bottom, written backwards.
            out = dst + (size_t)(dh - 1 - (r.y + y)) * dpitch + (dw - 1 - r.x);
            if (!t.w) { copy_row_back(out, a, r.w); continue; }
            lerp_row_back(out, a, hrow(src, spitch, t.i1), r.w, t.w);
        }
    }
    return 0;
}

static void fill(uint32_t *dst, int dpitch, int x, int y, int w, int h, uint32_t color)
{
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) dst[(size_t)(y + j) * dpitch + x + i] = color;
}

void scale_clear_outside(uint32_t *dst, int dpitch, int dw, int dh, ScaleRect r, uint32_t color,
                         int rotate)
{
    if (rotate) r = (ScaleRect){dw - r.x - r.w, dh - r.y - r.h, r.w, r.h};
    fill(dst, dpitch, 0, 0, dw, r.y, color);
    fill(dst, dpitch, 0, r.y + r.h, dw, dh - r.y - r.h, color);
    fill(dst, dpitch, 0, r.y, r.x, r.h, color);
    fill(dst, dpitch, r.x + r.w, r.y, dw - r.x - r.w, r.h, color);
}
