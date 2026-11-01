#include "fb_geometry.h"
#include "platform.h"

int fb_canvas_origin(int w, int h, int *x, int *y)
{
    if (w < SCREEN_W || h < SCREEN_H) return -1;
    *x = (w - SCREEN_W) / 2;
    *y = (h - SCREEN_H) / 2;
    return 0;
}
