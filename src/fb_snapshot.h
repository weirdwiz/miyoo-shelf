// An overlay borrows framebuffer memory; restore the game's entire visible page,
// including letterbox borders, rather than copying switcher pixels into its surface.
#ifndef SHELF_FB_SNAPSHOT_H
#define SHELF_FB_SNAPSHOT_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
typedef struct { unsigned char *pixels; size_t bytes, offset; int yoffset; } FbSnapshot;
static inline int fb_snapshot_take(FbSnapshot *s, const unsigned char *mem, size_t len,
                                   int pitch, int height, int yoffset)
{
    if (pitch <= 0 || height <= 0 || yoffset < 0 ||
        (size_t)pitch > SIZE_MAX / (size_t)height ||
        (size_t)pitch > SIZE_MAX / ((size_t)yoffset + 1)) return -1;
    size_t bytes = (size_t)pitch * height, offset = (size_t)pitch * yoffset;
    if (offset > len || bytes > len - offset) return -1;
    unsigned char *pixels = malloc(bytes);
    if (!pixels) return -1;
    memcpy(pixels, mem + offset, bytes);
    *s = (FbSnapshot){pixels, bytes, offset, yoffset};
    return 0;
}
static inline int fb_snapshot_restore(const FbSnapshot *s, unsigned char *mem, size_t len)
{
    if (!s->pixels || s->offset > len || s->bytes > len - s->offset) return -1;
    memcpy(mem + s->offset, s->pixels, s->bytes);
    return 0;
}
// Bytes of framebuffer to map so Shelf can flip between its two pages (0 and 1) and the
// page on screen stays addressable: RetroArch triple-buffers, so that may be page 2.
// 0 when the geometry can't hold both.
static inline size_t fb_map_len(int pitch, int height, unsigned yres_virtual, unsigned yoffset,
                                size_t smem_len)
{
    if (pitch <= 0 || height <= 0 || yres_virtual < 2u * height || yoffset > yres_virtual - height)
        return 0;
    size_t len = (size_t)pitch * yres_virtual;
    if (len / pitch != yres_virtual || len > smem_len) return 0;
    return len;
}
// Copies a snapshot of one page over each of the `pages` pages starting at the top of
// mem (each `s->bytes` long). RetroArch's borders are the same on every page and it
// redraws its viewport each frame, so one page restores any page an overlay drew on.
static inline int fb_snapshot_restore_pages(const FbSnapshot *s, unsigned char *mem, size_t len, int pages)
{
    if (!s->pixels || pages < 0 || (pages && s->bytes > len / (size_t)pages)) return -1;
    for (int p = 0; p < pages; p++) memcpy(mem + (size_t)p * s->bytes, s->pixels, s->bytes);
    return 0;
}
static inline void fb_snapshot_free(FbSnapshot *s)
{
    free(s->pixels);
    *s = (FbSnapshot){0};
}
#endif
