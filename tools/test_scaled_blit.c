// Differential test for clipping, bilinear sampling, alpha, and fractional geometry.
#include "../src/gfx.c"
#include <assert.h>

static void reference_blit(Image *dst, const Image *src, float x, float y, float w, float h, uint8_t alpha)
{
    if (w <= 0 || h <= 0) return;
    if (fabsf(w - src->w) < .5f && fabsf(h - src->h) < .5f) {
        gfx_blit(dst, src, (int)lroundf(x), (int)lroundf(y), alpha);
        return;
    }
    int x0 = (int)floorf(x), y0 = (int)floorf(y), x1 = (int)ceilf(x + w), y1 = (int)ceilf(y + h);
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 > dst->w) x1 = dst->w; if (y1 > dst->h) y1 = dst->h;
    float kx = src->w / w, ky = src->h / h;
    for (int yy = y0; yy < y1; yy++) {
        float fy = (yy + .5f - y) * ky;
        if (fy < 0 || fy >= src->h) continue;
        for (int xx = x0; xx < x1; xx++) {
            float fx = (xx + .5f - x) * kx;
            if (fx < 0 || fx >= src->w) continue;
            uint32_t p = sample(src, fx, fy);
            uint32_t *d = &dst->px[yy * dst->w + xx];
            *d = over(*d, alpha == 255 ? p : scale_px(p, alpha));
        }
    }
}

static uint32_t seed = 12345;
static uint32_t random_word(void) { seed = seed * 1664525u + 1013904223u; return seed; }

int main(void)
{
    Image *a = img_new(80, 60), *b = img_new(80, 60);
    for (int n = 0; n < 500; n++) {
        Image *src = img_new(1 + random_word() % 50, 1 + random_word() % 50);
        for (int i = 0; i < src->w * src->h; i++) {
            uint32_t r = random_word();
            src->px[i] = argb(r >> 24, r >> 16, r >> 8, r);
        }
        for (int i = 0; i < a->w * a->h; i++) a->px[i] = b->px[i] = random_word() | 0xff000000u;
        float x = (int)(random_word() % 120) - 30 + .13f;
        float y = (int)(random_word() % 100) - 30 + .31f;
        float w = .1f + random_word() % 100, h = .1f + random_word() % 90;
        uint8_t alpha = n % 3 == 0 ? 255 : random_word();
        reference_blit(a, src, x, y, w, h, alpha);
        gfx_blit_scaled(b, src, x, y, w, h, alpha);
        assert(!memcmp(a->px, b->px, (size_t)a->w * a->h * 4));
        img_free(src);
    }
    puts("scaled blit matches reference pixel-for-pixel (500 clipped/alpha cases)");

    // Span-accelerated blits (opaque middles copied) must equal plain blending.
    for (int n = 0; n < 300; n++) {
        Image *src = img_new(1 + random_word() % 60, 1 + random_word() % 60);
        gfx_fill_rrect(src, 1.5f, 2.5f, src->w - 3.f, src->h - 4.f, (random_word() % 12) + .5f,
                       random_word() | 0xff000000u);
        for (int i = 0; i < a->w * a->h; i++) a->px[i] = b->px[i] = random_word() | 0xff000000u;
        int x = (int)(random_word() % 120) - 40, y = (int)(random_word() % 100) - 40;
        gfx_blit(a, src, x, y, 255);
        img_find_spans(src);
        gfx_blit(b, src, x, y, 255);
        assert(!memcmp(a->px, b->px, (size_t)a->w * a->h * 4));
        img_free(src);
    }
    img_free(a); img_free(b);
    puts("span blit matches blended blit (300 clipped cases)");
}
