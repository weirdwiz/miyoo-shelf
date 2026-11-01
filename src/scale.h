// Scales a game frame (XRGB8888) onto the screen: integer, sharp-bilinear or bilinear,
// optionally rotated 180 degrees for the Miyoo's upside-down panel.
#ifndef SHELF_SCALE_H
#define SHELF_SCALE_H

#include <stdint.h>

typedef enum {
    SCALE_SHARP,   // fill the screen keeping aspect; pixels stay crisp, only edges blend
    SCALE_SMOOTH,  // fill the screen keeping aspect, plain bilinear
    SCALE_INTEGER, // largest whole multiple that fits; nearest neighbour
    SCALE_MODES
} ScaleMode;

typedef struct { int x, y, w, h; } ScaleRect;

// Where a `sw` x `sh` frame with display aspect `aspect` (<= 0: sw/sh) lands on a
// dw x dh screen, centred, in upright screen coordinates.
ScaleRect scale_rect(int sw, int sh, float aspect, int dw, int dh, ScaleMode mode);

// Draws `src` into rectangle `r` of `dst` (dw x dh, pitches in pixels). With `rotate`, the
// picture is turned 180 degrees: upright `r` maps to the mirrored rectangle. Only pixels
// inside the rectangle are written. Returns -1 when out of memory (nothing drawn).
int scale_blit(uint32_t *dst, int dpitch, int dw, int dh, const uint32_t *src, int sw, int sh,
               int spitch, ScaleRect r, ScaleMode mode, int rotate);

// Paints everything outside `r` with `color` (same coordinates and rotation as scale_blit).
void scale_clear_outside(uint32_t *dst, int dpitch, int dw, int dh, ScaleRect r, uint32_t color,
                         int rotate);

// Frees the cached lookup tables (they are rebuilt on the next blit).
void scale_free(void);

#endif
