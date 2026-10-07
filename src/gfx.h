// Software canvas: premultiplied ARGB8888 images and the few drawing ops the UI needs.
#ifndef SHELF_GFX_H
#define SHELF_GFX_H

#include <stdint.h>

typedef struct {
    int w, h;
    uint32_t *px; // premultiplied ARGB, row-major, stride == w
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
void img_blur(Image *img, int radius, int passes);  // separable box blur, in place
void img_round(Image *img, float radius);           // anti-aliased rounded-corner mask
int img_save_raw(const Image *img, const char *path);
Image *img_load_raw(const char *path);

void gfx_clear(Image *dst, uint32_t c);
void gfx_fill_rect(Image *dst, int x, int y, int w, int h, uint32_t c);
void gfx_fill_rrect(Image *dst, float x, float y, float w, float h, float r, uint32_t c);
void gfx_stroke_rrect(Image *dst, float x, float y, float w, float h, float r, float t, uint32_t c);
void gfx_blit(Image *dst, const Image *src, int x, int y, uint8_t alpha);
void gfx_blit_scaled(Image *dst, const Image *src, float x, float y, float w, float h, uint8_t alpha);
void gfx_mask_a8(Image *dst, const uint8_t *mask, int mw, int mh, int x, int y, uint32_t c);

#endif
