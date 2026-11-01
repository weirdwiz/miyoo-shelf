// Tests and benchmark for the game-frame scaler.
//   test-scale        exact expectations, then a randomized differential test
//   test-scale bench  ms per 240x160 -> 640x427 rotated blit, one line per mode
//
// The reference below spells the scaler out the slow, obvious way: it rebuilds the tap
// tables, scales every source row and writes one pixel at a time, sharing no code with
// src/scale.c. The differential test is what proves the optimised paths agree with it bit
// for bit, which on the device means the NEON paths against the C ones.
#include "../src/scale.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DW 640
#define DH 480
#define SENTINEL 0xdeadbeefu

static int failures;

static void fail(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    failures++;
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    return p;
}

// A host-side frame, padded rows and all: every pixel starts as a sentinel so that a write
// outside the target rectangle shows up in the comparison.
typedef struct { uint32_t *px; int w, h, pitch; } Frame;

static Frame *frame_new(int w, int h, int pitch)
{
    Frame *f = xmalloc(sizeof *f);
    f->w = w;
    f->h = h;
    f->pitch = pitch;
    f->px = xmalloc(sizeof *f->px * (size_t)pitch * h);
    for (size_t i = 0; i < (size_t)pitch * h; i++) f->px[i] = SENTINEL;
    return f;
}

static void frame_free(Frame *f)
{
    if (!f) return;
    free(f->px);
    free(f);
}

static Frame *frame_copy(const Frame *f)
{
    Frame *c = frame_new(f->w, f->h, f->pitch);
    memcpy(c->px, f->px, sizeof *c->px * (size_t)f->pitch * f->h);
    return c;
}

static int inside_rect(ScaleRect r, int x, int y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// Reports the first pixel that differs, padding of the rows included, which is enough to see
// what went wrong.
static void frame_expect(const Frame *want, const Frame *got, const char *what)
{
    for (int y = 0; y < want->h; y++)
        for (int x = 0; x < want->pitch; x++) {
            size_t i = (size_t)y * want->pitch + x;
            if (want->px[i] != got->px[i]) {
                fail("FAIL %s: pixel (%d,%d) want %08x got %08x\n", what, x, y, want->px[i],
                     got->px[i]);
                return;
            }
        }
}

static uint32_t random_word_state = 12345;

static uint32_t random_word(void)
{
    random_word_state = random_word_state * 1664525u + 1013904223u;
    return random_word_state;
}

// A different colour per texel, top byte zero: the scaler promises the three colour channels
// and always zeroes the fourth, whatever the source has there.
static uint32_t texel(int x, int y)
{
    return ((uint32_t)x * 0x9e3779b1u) ^ ((uint32_t)y * 0x85ebca6bu);
}

// ---------------------------------------------------------------- the slow reference

// Per output column (or row): the two source texels and the second one's weight, 0..256.
// This is the definition of the sampling: one tap per output position, centred on it, with
// the sharp-bilinear shift for SCALE_SHARP.
typedef struct { int i0, i1, w; } Tap;

static void make_taps(Tap *t, int n, int src, ScaleMode mode)
{
    float scale = (float)n / src;
    for (int i = 0; i < n; i++) {
        float u = (i + .5f) / scale - .5f;
        int k = (int)floorf(u);
        float f = u - k;
        if (mode == SCALE_INTEGER) {
            k = (int)((i + .5f) / scale);
            t[i] = (Tap){k, k, 0};
            continue;
        }
        if (mode == SCALE_SHARP && scale > 1) {
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

// Fixed-point blend with 8-bit weights: each channel is (a*(256-w) + b*w) >> 8.
static uint32_t lerp_px(uint32_t a, uint32_t b, int w)
{
    uint32_t rb = ((a & 0xff00ff) * (256 - w) + (b & 0xff00ff) * w) >> 8;
    uint32_t g = ((a & 0x00ff00) * (256 - w) + (b & 0x00ff00) * w) >> 8;
    return (rb & 0xff00ff) | (g & 0x00ff00);
}

// Where the picture lands for a rotated blit: upright (x, y) turns into (dw-1-x, dh-1-y).
static void dest_of(ScaleRect r, int dw, int dh, int x, int y, int rotate, int *dx, int *dy)
{
    *dx = rotate ? dw - 1 - (r.x + x) : r.x + x;
    *dy = rotate ? dh - 1 - (r.y + y) : r.y + y;
}

// Everything scale_blit does, one pixel at a time and with no caching.
static void ref_blit(uint32_t *dst, int dpitch, int dw, int dh, const uint32_t *src, int sw, int sh,
                     int spitch, ScaleRect r, ScaleMode mode, int rotate)
{
    Tap *tx = xmalloc(sizeof *tx * r.w);
    Tap *ty = xmalloc(sizeof *ty * r.h);
    make_taps(tx, r.w, sw, mode);
    make_taps(ty, r.h, sh, mode);
    uint32_t *rows = xmalloc(sizeof *rows * (size_t)r.w * sh); // every source row, scaled up
    for (int sy = 0; sy < sh; sy++) {
        uint32_t *out = rows + (size_t)sy * r.w;
        for (int x = 0; x < r.w; x++) {
            Tap t = tx[x];
            const uint32_t *s = src + (size_t)sy * spitch;
            out[x] = t.w ? lerp_px(s[t.i0], s[t.i1], t.w) : s[t.i0] & 0xffffff;
        }
    }
    for (int y = 0; y < r.h; y++) {
        Tap t = ty[y];
        const uint32_t *a = rows + (size_t)t.i0 * r.w;
        const uint32_t *b = rows + (size_t)t.i1 * r.w;
        for (int x = 0; x < r.w; x++) {
            int dx, dy;
            dest_of(r, dw, dh, x, y, rotate, &dx, &dy);
            dst[(size_t)dy * dpitch + dx] = t.w ? lerp_px(a[x], b[x], t.w) : a[x];
        }
    }
    free(rows);
    free(ty);
    free(tx);
}

// Blits `src` with src/scale.c and with the reference, and compares the whole destination,
// sentinel pixels and padding included.
static void same_as_reference(Frame *dst, const Frame *src, ScaleRect r, ScaleMode mode,
                              int rotate, const char *what)
{
    Frame *want = frame_copy(dst);
    ref_blit(want->px, want->pitch, want->w, want->h, src->px, src->w, src->h, src->pitch, r, mode,
             rotate);
    int rc = scale_blit(dst->px, dst->pitch, dst->w, dst->h, src->px, src->w, src->h, src->pitch, r,
                        mode, rotate);
    if (rc != 0) {
        fail("FAIL %s: scale_blit returned %d\n", what, rc);
        frame_free(want);
        return;
    }
    frame_expect(want, dst, what);
    frame_free(want);
}

// ---------------------------------------------------------------- exact expectations

static int rect_is(ScaleRect got, int x, int y, int w, int h)
{
    return got.x == x && got.y == y && got.w == w && got.h == h;
}

static void rect_case(int sw, int sh, float aspect, ScaleMode mode, int x, int y, int w, int h,
                      const char *what)
{
    ScaleRect got = scale_rect(sw, sh, aspect, DW, DH, mode);
    if (!rect_is(got, x, y, w, h))
        fail("FAIL scale_rect %s: got {%d,%d,%d,%d} want {%d,%d,%d,%d}\n", what, got.x, got.y,
             got.w, got.h, x, y, w, h);
}

static void test_rect(void)
{
    // A Game Boy Advance picture is 3:2, so it fills the width with 427 lines, 26 from the top.
    rect_case(240, 160, 0, SCALE_SHARP, 0, 26, 640, 427, "GBA sharp");
    rect_case(240, 160, 0, SCALE_SMOOTH, 0, 26, 640, 427, "GBA smooth");
    // Whole multiples: 2x fits the width, 3x the height.
    rect_case(240, 160, 0, SCALE_INTEGER, 80, 80, 480, 320, "GBA integer");
    rect_case(160, 144, 0, SCALE_INTEGER, 80, 24, 480, 432, "GB integer");
    // The Game Boy's 10:9 picture fills the height: 480 * 160/144 = 533.3 -> 533 wide.
    rect_case(160, 144, 0, SCALE_SHARP, 53, 0, 533, 480, "GB sharp");
    rect_case(160, 144, 0, SCALE_SMOOTH, 53, 0, 533, 480, "GB smooth");
    // Odd sizes: min(640/101, 480/77) = 6, and the centring keeps the extra pixel on the right.
    rect_case(101, 77, 0, SCALE_INTEGER, 17, 9, 606, 462, "odd integer");
    rect_case(101, 77, 0, SCALE_SMOOTH, 5, 0, 630, 480, "odd smooth");
    // An aspect override wins over sw/sh: square fills the height, 2:1 fills the width.
    rect_case(240, 160, 1.0f, SCALE_SHARP, 80, 0, 480, 480, "aspect 1:1");
    rect_case(240, 160, 2.0f, SCALE_SHARP, 0, 80, 640, 320, "aspect 2:1");
    rect_case(240, 160, 2.0f, SCALE_INTEGER, 80, 80, 480, 320, "aspect ignored by integer");
    // Sources larger than the screen are scaled down, and a rect that cannot fit is clamped
    // to the screen instead of overflowing it.
    rect_case(1000, 800, 0, SCALE_SMOOTH, 20, 0, 600, 480, "1.25:1 downscale");
    rect_case(1000, 800, 0, SCALE_INTEGER, 0, 0, 640, 480, "integer below 1x clamps");
    rect_case(1280, 720, 0, SCALE_SHARP, 0, 60, 640, 360, "720p downscale");
    rect_case(2000, 500, 0, SCALE_SMOOTH, 0, 160, 640, 160, "4:1 downscale");
    // Degenerate sizes cannot be scaled; the whole screen is the answer.
    rect_case(0, 144, 0, SCALE_SHARP, 0, 0, 640, 480, "sw 0");
    rect_case(160, 0, 0, SCALE_INTEGER, 0, 0, 640, 480, "sh 0");
    rect_case(-5, -5, 0, SCALE_SMOOTH, 0, 0, 640, 480, "negative");
    puts("scale_rect matches the expected rectangles (18 cases)");
}

// INTEGER draws whole multiples of source texels, so every output pixel is a source colour.
static void test_integer(void)
{
    static const struct { int sw, sh; ScaleMode mode; int rotate; } cases[] = {
        {240, 160, SCALE_INTEGER, 0}, {240, 160, SCALE_INTEGER, 1}, {160, 144, SCALE_INTEGER, 0},
        {160, 144, SCALE_INTEGER, 1}, {101, 77, SCALE_INTEGER, 0},  {37, 23, SCALE_INTEGER, 1},
    };
    for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
        Frame *src = frame_new(cases[c].sw, cases[c].sh, cases[c].sw);
        for (int y = 0; y < src->h; y++)
            for (int x = 0; x < src->w; x++)
                src->px[y * src->pitch + x] = texel(x, y) | 0xff000000u;
        Frame *dst = frame_new(DW, DH, DW);
        ScaleRect r = scale_rect(src->w, src->h, 0, DW, DH, cases[c].mode);
        if (scale_blit(dst->px, dst->pitch, dst->w, dst->h, src->px, src->w, src->h, src->pitch, r,
                       cases[c].mode, cases[c].rotate) != 0) {
            fail("FAIL integer %dx%d: blit failed\n", src->w, src->h);
            frame_free(src);
            frame_free(dst);
            continue;
        }
        int k = r.w / src->w;
        for (int y = 0; y < dst->h; y++)
            for (int x = 0; x < dst->w; x++) {
                int ux = x, uy = y; // back to upright coordinates
                if (cases[c].rotate) {
                    ux = DW - 1 - x;
                    uy = DH - 1 - y;
                }
                uint32_t want = SENTINEL;
                if (inside_rect(r, ux, uy)) {
                    uint32_t s = src->px[(uy - r.y) / k * src->pitch + (ux - r.x) / k];
                    want = s & 0xffffff;
                }
                uint32_t got = dst->px[y * dst->pitch + x];
                if (got != want) {
                    fail("FAIL integer %dx%d %srotate: pixel (%d,%d) want %08x got %08x\n", src->w,
                         src->h, cases[c].rotate ? "" : "un", x, y, want, got);
                    y = dst->h; // one report per case
                    break;
                }
            }
        frame_free(src);
        frame_free(dst);
    }
    puts("integer scaling reproduces source texels exactly, upright and rotated (6 cases)");
}

// A 50/50 blend of two colours, channel by channel, rounding down.
static uint32_t average(uint32_t a, uint32_t b)
{
    uint32_t out = 0;
    for (int c = 0; c < 24; c += 8) out |= ((((a >> c) & 255) + ((b >> c) & 255)) >> 1) << c;
    return out;
}

static void test_smooth(void)
{
    // 256 columns onto 640 is exactly 2.5x, so output column 2 samples the midpoint between
    // source texels 0 and 1: a plain bilinear 50/50 blend.
    Frame *src = frame_new(256, 160, 256);
    for (int y = 0; y < src->h; y++)
        for (int x = 0; x < src->w; x++) src->px[y * src->pitch + x] = texel(x, 0);
    Frame *dst = frame_new(DW, DH, DW);
    ScaleRect r = scale_rect(src->w, src->h, 0, DW, DH, SCALE_SMOOTH);
    if (!rect_is(r, 0, 40, 640, 400))
        fail("FAIL smooth midpoint: rect {%d,%d,%d,%d}\n", r.x, r.y, r.w, r.h);
    if (scale_blit(dst->px, dst->pitch, dst->w, dst->h, src->px, src->w, src->h, src->pitch, r,
                   SCALE_SMOOTH, 0) != 0) {
        fail("FAIL smooth midpoint: blit failed\n");
    } else {
        uint32_t want = average(src->px[0] & 0xffffff, src->px[1] & 0xffffff);
        for (int y = r.y; y < r.y + r.h; y++)
            if (dst->px[y * dst->pitch + r.x + 2] != want) {
                fail("FAIL smooth midpoint: pixel (%d,%d) want %08x got %08x\n", r.x + 2, y, want,
                     dst->px[y * dst->pitch + r.x + 2]);
                break;
            }
        // ... and it is bilinear, not nearest: the neighbouring column is not a source colour.
        if (dst->px[r.y * dst->pitch + r.x + 3] == (src->px[1] & 0xffffff) ||
            dst->px[r.y * dst->pitch + r.x + 3] == (src->px[0] & 0xffffff))
            fail("FAIL smooth midpoint: column 3 snapped to a texel instead of blending\n");
    }
    frame_free(src);
    frame_free(dst);
    puts("smooth samples bilinearly: a texel midpoint is a 50/50 blend, not a snap");
}

static void test_sharp(void)
{
    // At 1:1 there is nothing to blend, so sharp is an exact copy: this is the crispness the
    // mode promises where it can be had.
    Frame *native = frame_new(640, 427, 640);
    for (int y = 0; y < native->h; y++)
        for (int x = 0; x < native->w; x++) native->px[y * native->pitch + x] = texel(x, y);
    Frame *dst = frame_new(DW, DH, DW);
    ScaleRect r = scale_rect(native->w, native->h, 0, DW, DH, SCALE_SHARP);
    if (!rect_is(r, 0, 26, 640, 427))
        fail("FAIL sharp 1:1: rect {%d,%d,%d,%d}\n", r.x, r.y, r.w, r.h);
    if (scale_blit(dst->px, dst->pitch, dst->w, dst->h, native->px, native->w, native->h,
                   native->pitch, r, SCALE_SHARP, 0) != 0) {
        fail("FAIL sharp 1:1: blit failed\n");
    } else {
        for (int y = 0; y < native->h; y++)
            for (int x = 0; x < native->w; x++) {
                uint32_t want = native->px[y * native->pitch + x] & 0xffffff;
                uint32_t got = dst->px[(r.y + y) * dst->pitch + r.x + x];
                if (got != want) {
                    fail("FAIL sharp 1:1: pixel (%d,%d) want %08x got %08x\n", x, y, want, got);
                    y = native->h;
                    break;
                }
            }
    }
    frame_free(native);
    frame_free(dst);

    // Upright, sharp uses its own weights: on a two-texel ramp its taps differ from plain
    // bilinear even where the texels are furthest from the sample point.
    Frame *ramp = frame_new(2, 1, 2);
    ramp->px[0] = 0x000000;
    ramp->px[1] = 0xffffff;
    Frame *sharp = frame_new(64, 4, 64), *smooth = frame_new(64, 4, 64);
    ScaleRect rr = {0, 1, 64, 2};
    if (scale_blit(sharp->px, 64, 64, 4, ramp->px, 2, 1, 2, rr, SCALE_SHARP, 0) ||
        scale_blit(smooth->px, 64, 64, 4, ramp->px, 2, 1, 2, rr, SCALE_SMOOTH, 0))
        fail("FAIL sharp ramp: blit failed\n");
    else if (!memcmp(sharp->px, smooth->px, sizeof *sharp->px * 64 * 4))
        fail("FAIL sharp ramp: sharp produced the same pixels as smooth\n");
    frame_free(ramp);
    frame_free(smooth);
    frame_free(sharp);

    // Shrinking skips the sharp-bilinear adjustment, so sharp and smooth become the same
    // filter here.
    Frame *big = frame_new(1280, 854, 1280);
    for (int y = 0; y < big->h; y++)
        for (int x = 0; x < big->w; x++) big->px[y * big->pitch + x] = texel(x, y);
    sharp = frame_new(DW, DH, DW);
    smooth = frame_new(DW, DH, DW);
    ScaleRect rd = scale_rect(big->w, big->h, 0, DW, DH, SCALE_SMOOTH);
    if (!rect_is(rd, 0, 26, 640, 427))
        fail("FAIL downscale: rect {%d,%d,%d,%d}\n", rd.x, rd.y, rd.w, rd.h);
    if (scale_blit(sharp->px, DW, DW, DH, big->px, big->w, big->h, big->pitch, rd, SCALE_SHARP,
                   0) ||
        scale_blit(smooth->px, DW, DW, DH, big->px, big->w, big->h, big->pitch, rd, SCALE_SMOOTH,
                   0))
        fail("FAIL downscale: blit failed\n");
    else
        frame_expect(smooth, sharp, "sharp equals smooth when shrinking");
    frame_free(big);
    frame_free(smooth);
    frame_free(sharp);

    // Upscaling a GBA frame to the screen, the middle of every texel is reproduced exactly:
    // only output pixels straddling a texel edge may blend.
    Frame *gba = frame_new(240, 160, 240);
    for (int y = 0; y < gba->h; y++)
        for (int x = 0; x < gba->w; x++) gba->px[y * gba->pitch + x] = texel(x, y);
    Frame *up = frame_new(DW, DH, DW);
    ScaleRect ru = scale_rect(240, 160, 0, DW, DH, SCALE_SHARP);
    int solid = 0, total = 0;
    if (scale_blit(up->px, DW, DW, DH, gba->px, 240, 160, 240, ru, SCALE_SHARP, 0)) {
        fail("FAIL sharp interior: blit failed\n");
    } else {
        for (int y = 0; y < ru.h; y++)
            for (int x = 0; x < ru.w; x++) {
                float u = (x + .5f) * 240 / ru.w, v = (y + .5f) * 160 / ru.h;
                float fu = u - floorf(u), fv = v - floorf(v);
                float edge = .5f * 240 / ru.w; // half an output pixel, in texels
                if (fu < edge || fu > 1 - edge || fv < edge || fv > 1 - edge) continue;
                total++;
                uint32_t want = gba->px[(int)v * 240 + (int)u] & 0xffffff;
                if (up->px[(ru.y + y) * DW + ru.x + x] == want) solid++;
            }
        if (!total || solid != total)
            fail("FAIL sharp interior: %d of %d interior pixels exact\n", solid, total);
    }
    frame_free(gba);
    frame_free(up);

    puts("sharp is exact at 1:1, keeps texel interiors exact upscaling, is plain bilinear shrinking");
}

static void test_rotate(void)
{
    static const ScaleMode modes[] = {SCALE_SHARP, SCALE_SMOOTH, SCALE_INTEGER};
    static const struct { int sw, sh; } sizes[] = {{240, 160}, {160, 144}, {1280, 720}};
    for (size_t m = 0; m < sizeof modes / sizeof *modes; m++)
        for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
            Frame *src = frame_new(sizes[s].sw, sizes[s].sh, sizes[s].sw);
            for (int y = 0; y < src->h; y++)
                for (int x = 0; x < src->w; x++)
                    src->px[y * src->pitch + x] = texel(x, y) | 0xff000000u;
            Frame *up = frame_new(DW, DH, DW), *down = frame_new(DW, DH, DW);
            ScaleRect r = scale_rect(src->w, src->h, 0, DW, DH, modes[m]);
            int rc = scale_blit(up->px, DW, DW, DH, src->px, src->w, src->h, src->pitch, r,
                                modes[m], 0);
            rc |= scale_blit(down->px, DW, DW, DH, src->px, src->w, src->h, src->pitch, r, modes[m],
                             1);
            if (rc) {
                fail("FAIL rotate %dx%d mode %d: blit failed\n", src->w, src->h, (int)modes[m]);
            } else {
                for (int y = 0; y < DH; y++)
                    for (int x = 0; x < DW; x++) {
                        uint32_t want = up->px[y * up->pitch + x];
                        uint32_t got = down->px[(DH - 1 - y) * down->pitch + DW - 1 - x];
                        if (want != got) {
                            fail("FAIL rotate %dx%d mode %d: (%d,%d) is %08x but mirrored %08x\n",
                                 src->w, src->h, (int)modes[m], x, y, want, got);
                            y = DH;
                            break;
                        }
                    }
            }
            frame_free(src);
            frame_free(up);
            frame_free(down);
        }
    puts("rotate=1 maps upright (x,y) to (dw-1-x, dh-1-y) in all three modes (9 cases)");
}

static void test_clear_outside(void)
{
    static const ScaleRect rects[] = {{0, 26, 640, 427}, {53, 0, 533, 480}, {80, 80, 480, 320}};
    for (size_t i = 0; i < sizeof rects / sizeof *rects; i++)
        for (int rotate = 0; rotate < 2; rotate++) {
            Frame *f = frame_new(DW, DH, DW + 8); // padded rows: the padding must survive
            ScaleRect r = rects[i];
            // Upright rectangle of the picture on screen; rotation mirrors it.
            ScaleRect on = rotate ? (ScaleRect){DW - r.x - r.w, DH - r.y - r.h, r.w, r.h} : r;
            for (int y = 0; y < f->h; y++)
                for (int x = 0; x < f->w; x++)
                    f->px[y * f->pitch + x] = inside_rect(on, x, y) ? 0x11223344u : SENTINEL;
            Frame *want = frame_copy(f);
            for (int y = 0; y < want->h; y++)
                for (int x = 0; x < want->w; x++)
                    if (!inside_rect(on, x, y)) want->px[y * want->pitch + x] = 0x0000ff00u;
            scale_clear_outside(f->px, f->pitch, f->w, f->h, r, 0x0000ff00u, rotate);
            frame_expect(want, f, "clear_outside");
            frame_free(want);
            frame_free(f);
        }
    puts("clear_outside paints only outside the rectangle, upright and rotated (6 cases)");
}

static void test_invalid(void)
{
    static const ScaleRect bad[] = {
        {-1, 0, 10, 10}, {0, -1, 10, 10}, {0, 0, 0, 10},  {0, 0, 10, 0}, {0, 0, -5, 10},
        {DW - 9, 0, 10, 10}, {0, DH - 9, 10, 10}, {DW, 0, 10, 10}, {0, 0, DW + 1, 10},
    };
    Frame *src = frame_new(8, 8, 8);
    for (int y = 0; y < src->h; y++)
        for (int x = 0; x < src->w; x++) src->px[y * src->pitch + x] = texel(x, y);
    Frame *dst = frame_new(DW, DH, DW);
    Frame *before = frame_copy(dst);
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        int rc = scale_blit(dst->px, dst->pitch, dst->w, dst->h, src->px, src->w, src->h,
                            src->pitch, bad[i], SCALE_SMOOTH, (int)i % 2);
        if (rc != -1)
            fail("FAIL invalid rect %zu {%d,%d,%d,%d}: returned %d, want -1\n", i, bad[i].x,
                 bad[i].y, bad[i].w, bad[i].h, rc);
    }
    frame_expect(before, dst, "an invalid rectangle wrote pixels");
    // The largest rectangle that still fits is fine.
    ScaleRect edge = {DW - 10, DH - 10, 10, 10};
    if (scale_blit(dst->px, dst->pitch, dst->w, dst->h, src->px, src->w, src->h, src->pitch, edge,
                   SCALE_SMOOTH, 0) != 0)
        fail("FAIL rect at the bottom-right corner was rejected\n");
    frame_free(before);
    frame_free(dst);
    frame_free(src);
    puts("invalid rectangles return -1 and draw nothing (9 cases)");
}

static void test_rebuild(void)
{
    // Different source and rectangle sizes between calls: the tap tables and the row cache
    // have to be rebuilt each time the geometry changes.
    static const struct { int sw, sh; ScaleMode mode; int rotate; } cases[] = {
        {160, 144, SCALE_SHARP, 0},  {240, 160, SCALE_SMOOTH, 1}, {101, 77, SCALE_SHARP, 0},
        {1, 1, SCALE_SMOOTH, 0},     {256, 240, SCALE_INTEGER, 1}, {3, 200, SCALE_SHARP, 0},
        {200, 3, SCALE_SMOOTH, 0},   {240, 160, SCALE_SHARP, 1},  {160, 144, SCALE_SMOOTH, 0},
    };
    for (int pass = 0; pass < 2; pass++)
        for (size_t c = 0; c < sizeof cases / sizeof *cases; c++) {
            Frame *src = frame_new(cases[c].sw, cases[c].sh, cases[c].sw + 1);
            for (int y = 0; y < src->h; y++)
                for (int x = 0; x < src->w; x++)
                    src->px[y * src->pitch + x] = texel(x + pass, y);
            Frame *dst = frame_new(DW, DH, DW + 2);
            ScaleRect r = scale_rect(src->w, src->h, 0, DW, DH, cases[c].mode);
            same_as_reference(dst, src, r, cases[c].mode, cases[c].rotate, "rebuild");
            frame_free(src);
            frame_free(dst);
        }
    puts("changing sizes and mode between calls rebuilds the tables (18 blits)");
}

static void test_differential(void)
{
    // Random geometry against the reference: sizes, modes, rectangles, rotation and pitches,
    // including rectangles that touch the screen edges and sources bigger than the screen.
    const int cases = 400;
    for (int n = 0; n < cases; n++) {
        int sw = 1 + (int)(random_word() % 256), sh = 1 + (int)(random_word() % 256);
        ScaleMode mode = (ScaleMode)(random_word() % SCALE_MODES);
        int rotate = (int)(random_word() & 1);
        int spitch = sw + (int)(random_word() % 5);
        int dpitch = DW + (int)(random_word() % 8);
        ScaleRect r;
        if (n % 3 == 0) {
            r = scale_rect(sw, sh, 0, DW, DH, mode);
        } else {
            r.w = 1 + (int)(random_word() % DW);
            r.h = 1 + (int)(random_word() % DH);
            r.x = (int)(random_word() % (uint32_t)(DW - r.w + 1));
            r.y = (int)(random_word() % (uint32_t)(DH - r.h + 1));
        }
        Frame *src = frame_new(sw, sh, spitch);
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                uint32_t v = texel(x, y) | (random_word() & 0xff000000u);
                src->px[y * spitch + x] = v;
            }
        Frame *dst = frame_new(DW, DH, dpitch);
        char what[96];
        snprintf(what, sizeof what, "differential %d (%dx%d mode %d rot %d)", n, sw, sh, (int)mode,
                 rotate);
        same_as_reference(dst, src, r, mode, rotate, what);
        frame_free(src);
        frame_free(dst);
    }
    printf("optimised blit matches the reference pixel for pixel (%d random cases); on the "
           "NEON build this is the proof that the vector paths equal the C ones\n", cases);
}

static void test_free_reuse(void)
{
    Frame *src = frame_new(240, 160, 240);
    for (int y = 0; y < src->h; y++)
        for (int x = 0; x < src->w; x++) src->px[y * src->pitch + x] = texel(x, y);
    ScaleRect r = scale_rect(src->w, src->h, 0, DW, DH, SCALE_SMOOTH);
    Frame *first = frame_new(DW, DH, DW);
    Frame *second = frame_new(DW, DH, DW);
    int rc = scale_blit(first->px, DW, DW, DH, src->px, src->w, src->h, src->pitch, r, SCALE_SMOOTH,
                        1);
    scale_free();
    rc |= scale_blit(second->px, DW, DW, DH, src->px, src->w, src->h, src->pitch, r,
                     SCALE_SMOOTH, 1);
    if (rc) {
        fail("FAIL free then reuse: blit failed\n");
    } else {
        frame_expect(first, second, "free then reuse");
    }
    scale_free(); // freeing twice must be harmless
    // And a different geometry after a free still works.
    ScaleRect r2 = scale_rect(src->w, src->h, 0, DW, DH, SCALE_SHARP);
    if (scale_blit(second->px, DW, DW, DH, src->px, src->w, src->h, src->pitch, r2, SCALE_SHARP, 0))
        fail("FAIL free then reuse: blit after free failed\n");
    frame_free(first);
    frame_free(second);
    frame_free(src);
    puts("scale_free then reuse gives the same pixels, twice in a row and after a free");
}

// ---------------------------------------------------------------- benchmark

static uint32_t checksum(const Frame *f)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < (size_t)f->pitch * f->h; i++) h = (h ^ f->px[i]) * 16777619u;
    return h;
}

// 600 blits of a Game Boy Advance frame onto the full 640x427 box, rotated 180 degrees like
// the Miyoo's upside-down panel is, so every blit rebuilds the row cache from the bottom.
// Same source, same rectangle and same rotation for all three modes, so they are comparable.
// Each mode is timed over three passes and the best pass is reported: the VM's emulated CPU
// is noisy, and a best-of-three is what makes two builds comparable.
static int bench(void)
{
    static const char *const NAME[SCALE_MODES] = {"sharp", "smooth", "integer"};
    enum { SW = 240, SH = 160, BLITS = 600, PASSES = 3 };
    ScaleRect r = {0, 26, 640, 427};
    Frame *src = frame_new(SW, SH, SW);
    for (int y = 0; y < src->h; y++)
        for (int x = 0; x < src->w; x++) src->px[y * src->pitch + x] = texel(x, y);
    Frame *dst = frame_new(DW, DH, DW);
    printf("%d rotated blits of %dx%d onto %dx%d, best of %d passes, ms per blit\n", BLITS, SW,
           SH, r.w, r.h, PASSES);
    for (int m = 0; m < SCALE_MODES; m++) {
        double best = 0;
        int rc = scale_blit(dst->px, DW, DW, DH, src->px, SW, SH, src->pitch, r, (ScaleMode)m, 1);
        for (int pass = 0; pass < PASSES; pass++) {
            clock_t t0 = clock();
            for (int i = 0; i < BLITS; i++)
                rc |= scale_blit(dst->px, DW, DW, DH, src->px, SW, SH, src->pitch, r,
                                 (ScaleMode)m, 1);
            double ms = (double)(clock() - t0) * 1000.0 / CLOCKS_PER_SEC / BLITS;
            if (!pass || ms < best) best = ms;
        }
        if (rc) {
            printf("%s: blit failed\n", NAME[m]);
            frame_free(src);
            frame_free(dst);
            return 1;
        }
        printf("%-8s %8.3f ms/blit (sum %08x)\n", NAME[m], best, checksum(dst));
    }
    frame_free(src);
    frame_free(dst);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "bench")) return bench();

    test_rect();
    test_integer();
    test_smooth();
    test_sharp();
    test_rotate();
    test_clear_outside();
    test_invalid();
    test_rebuild();
    test_differential();
    test_free_reuse();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    puts("all scaler tests passed");
    return 0;
}
