// Software canvas: premultiplied ARGB8888 images and the few drawing ops the UI needs.
#ifndef SHELF_GFX_H
#define SHELF_GFX_H

#include <stdint.h>

typedef struct {
    int w, h;
    uint32_t *px; // premultiplied ARGB, row-major, stride == w
    int16_t *spans; // optional per-row [begin, end) fully-opaque run, from img_find_spans
} Image;

// Build a premultiplied ARGB colour from straight RGB + alpha.
static inline uint32_t argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
    return ((uint32_t)a << 24) | ((uint32_t)(r * a / 255) << 16) |
           ((uint32_t)(g * a / 255) << 8) | (uint32_t)(b * a / 255);
}
static inline uint32_t rgb(uint32_t hex) { return 0xff000000u | hex; }

Image *img_new(int w, int h);
void img_free(Image *img);
Image *img_load(const char *path); // PNG/JPG/GIF via stb_image, NULL on failure
Image *img_resize(const Image *src, int w, int h); // area-average down, bilinear up
void img_round(Image *img, float radius);           // anti-aliased rounded-corner mask
int img_save_raw(const Image *img, const char *path);
Image *img_load_raw(const char *path);
// Record each row's opaque run so gfx_blit can copy it instead of blending.
// Call again after changing the pixels.
void img_find_spans(Image *img);

void gfx_clear(Image *dst, uint32_t c);
void gfx_fill_rect(Image *dst, int x, int y, int w, int h, uint32_t c);
void gfx_fill_rrect(Image *dst, float x, float y, float w, float h, float r, uint32_t c);
// gfx_fill_rrect leaving the pixels [hx0, hx1) x [hy0, hy1) alone, for a frame around
// something drawn there.
void gfx_fill_rrect_around(Image *dst, float x, float y, float w, float h, float r,
                           int hx0, int hy0, int hx1, int hy1, uint32_t c);
void gfx_stroke_rrect(Image *dst, float x, float y, float w, float h, float r, float t, uint32_t c);
void gfx_blit(Image *dst, const Image *src, int x, int y, uint8_t alpha);
void gfx_blit_scaled(Image *dst, const Image *src, float x, float y, float w, float h, uint8_t alpha);
// Writes all of dst: `under` with the opaque `src` faded in over it, nearest-neighbour
// scaled into (x, y, w, h). For short, fading motion where filtering is invisible.
void gfx_fade_scaled_nearest(Image *dst, const Image *under, const Image *src,
                             float x, float y, float w, float h, uint8_t alpha);
// Bilinear resize of src to fill dst, for upscaling smooth images (blurred backdrops).
void gfx_upscale(Image *dst, const Image *src);
// Nearest-neighbour scaled blit (alpha 255), for short motion where filtering is invisible.
void gfx_blit_scaled_nearest(Image *dst, const Image *src, float x, float y, float w, float h);
// dst = a faded to b by t (0..255); all three opaque and the same size.
void gfx_lerp(Image *dst, const Image *a, const Image *b, uint8_t t);
void gfx_mask_a8(Image *dst, const uint8_t *mask, int mw, int mh, int x, int y, uint32_t c);
// Draw a w x h mask stretched from an mw x mh one: corner x corner blocks stay fixed,
// edges repeat the middle row/column, and the (empty) interior is skipped.
void gfx_mask_a8_9slice(Image *dst, const uint8_t *mask, int mw, int mh, int corner,
                        int x, int y, int w, int h, uint32_t c);

#endif
