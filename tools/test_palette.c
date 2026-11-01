// WCAG contrast checks on the colours Shelf actually uses, pulled from src/ui.c and
// src/library.c so the test fails when someone darkens a fill under white text.
#include "../src/library.c"
#include "../src/ui.c"

#include <assert.h>
#include <math.h>
#include <stdio.h>

// WCAG 2 relative luminance of an opaque 0xRRGGBB colour (sRGB, linearised).
static double luminance(uint32_t c)
{
    static const double W[3] = {0.2126, 0.7152, 0.0722}; // red, green, blue weights
    double lum = 0;
    for (int i = 2; i >= 0; i--, c >>= 8) {
        double v = (c & 0xff) / 255.0;
        lum += W[i] * (v <= 0.04045 ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4));
    }
    return lum;
}

static double contrast(uint32_t a, uint32_t b)
{
    double la = luminance(a) + 0.05, lb = luminance(b) + 0.05;
    return la > lb ? la / lb : lb / la;
}

static void require(double ratio, const char *what)
{
    printf("palette: white-on-colour %-28s %.2f\n", what, ratio);
    assert(ratio >= 4.5);
}

int main(int argc, char **argv)
{
    assert(argc == 3);

    // Secondary text must stay readable on the background in both themes.
    for (int i = 0; i < (int)(sizeof PALETTES / sizeof *PALETTES); i++) {
        char what[64];
        snprintf(what, sizeof what, "palette[%d] sub on bg", i);
        printf("palette: %-34s %.2f\n", what, contrast(PALETTES[i].sub, PALETTES[i].bg));
        assert(contrast(PALETTES[i].sub, PALETTES[i].bg) >= 4.5);
    }

    lib_set_root(argv[1]);
    Library lib;
    assert(lib_load(&lib) == 0);
    icons_init(&lib);
    assert(ui_init(&lib, argv[2]) == 0);

    // White text sits on every system colour and on both fixed folder colours.
    for (int i = 0; i < lib.nsys; i++)
        require(contrast(0xffffff, lib.sys[i].color), lib.sys[i].label);
    int all = 0, fav = 0;
    for (int i = 0; i < S.nfolders; i++) {
        if (S.folders[i].kind == F_ALL) all = 1;
        if (S.folders[i].kind == F_FAV) fav = 1;
        if (S.folders[i].kind == F_ALL || S.folders[i].kind == F_FAV)
            require(contrast(0xffffff, S.folders[i].color), S.folders[i].label);
    }
    assert(all && fav); // the fixture must exercise both fixed folders

    ui_free();
    puts("palette: WCAG contrast of text, system and folder colours passed");
    return 0;
}
