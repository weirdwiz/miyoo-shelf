// Differential regression test: optimized coverage must match the original renderer.
#include "../src/gfx.c"
#include <assert.h>

static float reference(float px, float py, float x, float y, float w, float h, float r)
{
    float cx = px < x + r ? x + r : (px > x + w - r ? x + w - r : px);
    float cy = py < y + r ? y + r : (py > y + h - r ? y + h - r : py);
    float dx = px - cx, dy = py - cy;
    float d = sqrtf(dx * dx + dy * dy) - r;
    if (px < x || px > x + w || py < y || py > y + h) {
        float ex = px < x ? x - px : (px > x + w ? px - x - w : 0);
        float ey = py < y ? y - py : (py > y + h ? py - y - h : 0);
        float e = ex > ey ? ex : ey;
        if (d < e) d = e;
    }
    float c = 0.5f - d;
    return c < 0 ? 0 : (c > 1 ? 1 : c);
}

// The fill as it was: rrect_cov for every pixel.
static void reference_fill(Image *dst, float x, float y, float w, float h, float r,
                           int hx0, int hy0, int hx1, int hy1, uint32_t c)
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
            if (yy >= hy0 && yy < hy1 && xx == hx0 && hx1 > hx0) { xx = hx1 - 1; continue; }
            float cv = rrect_cov(xx + 0.5f, yy + 0.5f, x, y, w, h, r);
            if (cv <= 0) continue;
            uint32_t *d = &dst->px[yy * dst->w + xx];
            *d = over(*d, cv >= 1 ? c : scale_px(c, (uint32_t)(cv * 255)));
        }
}

static uint32_t seed = 12345;
static uint32_t rnd(void) { seed = seed * 1664525u + 1013904223u; return seed >> 8; }

int main(void)
{
    // Filled interiors skip rrect_cov; every pixel must still match filling per pixel.
    Image *a = img_new(80, 70), *b = img_new(80, 70);
    for (int n = 0; n < 20000; n++) {
        for (int i = 0; i < a->w * a->h; i++) a->px[i] = b->px[i] = rnd() | 0xff000000u;
        float x = (int)(rnd() % 90) - 15 + (rnd() % 100) / 100.f, y = (int)(rnd() % 80) - 15 + (rnd() % 100) / 100.f;
        float w = (rnd() % 900) / 10.f, h = (rnd() % 800) / 10.f;
        float r = n % 5 == 0 ? (rnd() % 20) / 10.f : (rnd() % 400) / 10.f;
        int hx0 = 0, hy0 = 0, hx1 = 0, hy1 = 0;
        if (n % 3 == 0) {
            hx0 = (int)(rnd() % 90) - 5, hy0 = (int)(rnd() % 80) - 5;
            hx1 = hx0 + (int)(rnd() % 40), hy1 = hy0 + (int)(rnd() % 40);
        }
        uint32_t c = n % 2 ? rnd() | 0xff000000u : argb(rnd() % 256, rnd(), rnd(), rnd());
        reference_fill(a, x, y, w, h, r, hx0, hy0, hx1, hy1, c);
        gfx_fill_rrect_around(b, x, y, w, h, r, hx0, hy0, hx1, hy1, c);
        assert(!memcmp(a->px, b->px, (size_t)a->w * a->h * 4));
    }
    img_free(a);
    img_free(b);
    puts("rounded-rectangle fill matches per-pixel coverage (20000 cases)");

    const float radii[] = {0, .1f, .49f, .5f, 1.3f, 8, 20};
    for (int k = 0; k < 7; k++)
        for (int offset = 0; offset < 10; offset++)
            for (int y = -8; y < 240; y++)
                for (int x = -8; x < 240; x++) {
                    float px = x * .25f, py = y * .25f;
                    float ox = offset * .13f, oy = offset * .17f;
                    float a = reference(px, py, ox, oy, 47.7f, 42.3f, radii[k]);
                    float b = rrect_cov(px, py, ox, oy, 47.7f, 42.3f, radii[k]);
                    assert(a == b);
                }
    puts("rounded-rectangle coverage matches reference");
    return 0;
}
