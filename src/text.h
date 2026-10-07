// TrueType text via stb_truetype with a per-font glyph cache. UTF-8 in.
#ifndef SHELF_TEXT_H
#define SHELF_TEXT_H

#include "gfx.h"

typedef struct Font Font;

Font *font_load(const char *path, float px);
void font_free(Font *f);
int font_height(const Font *f); // ascent - descent, in pixels
int text_width(Font *f, const char *s);
// Draws with the top of the line box at y. Returns the advance.
int text_draw(Image *dst, Font *f, const char *s, int x, int y, uint32_t color);
// Copies s into out, cutting it with an ellipsis so it fits max_w.
void text_fit(Font *f, const char *s, int max_w, char *out, int out_size);

#endif
