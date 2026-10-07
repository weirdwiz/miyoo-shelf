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

int main(void)
{
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
