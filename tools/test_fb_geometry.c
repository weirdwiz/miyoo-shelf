// Where the canvas lands on each panel the Miyoo Mini family has shipped with.
#include <stdio.h>
#include <stdlib.h>

#include "../src/fb_geometry.h"

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); exit(1); } } while (0)

int main(void)
{
    int x = -1, y = -1;
    CHECK(fb_canvas_origin(640, 480, &x, &y) == 0 && x == 0 && y == 0);   // Mini v1-v3, early Plus
    CHECK(fb_canvas_origin(752, 560, &x, &y) == 0 && x == 56 && y == 40); // Mini v4, later Plus
    CHECK(fb_canvas_origin(750, 559, &x, &y) == 0 && x == 55 && y == 39); // its visible area
    CHECK(fb_canvas_origin(639, 480, &x, &y) == -1);
    CHECK(fb_canvas_origin(640, 479, &x, &y) == -1);
    printf("fb geometry: 640x480 fills the panel, larger panels centre the canvas\n");
    return 0;
}
