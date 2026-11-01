#ifndef SHELF_FB_GEOMETRY_H
#define SHELF_FB_GEOMETRY_H

// Where the 640x480 canvas goes on a panel of w x h: centred, rounded down. Returns 0, or
// -1 when the panel is smaller than the canvas. Kept apart from platform_mm.c so the host
// tests can check it.
int fb_canvas_origin(int w, int h, int *x, int *y);

#endif
