#include "core_options.h"

#include <stdio.h>
#include <string.h>

#define COUNT(a) (int)(sizeof a / sizeof *a)

// gambatte (GB, GBC). Game Boy Color games are corrected to look like the GBC's own
// screen with the core's accurate model, and Game Boy games get the Pocket palette (each
// game's choice, below, sets one or the other). No frame blending: it costs a frame copy
// and blend every frame, and only a few games need it. The boot logo is skipped when a
// boot ROM is on the card: A starts the game.
static const PlayerOption GAMBATTE[] = {
    {"gambatte_gb_colorization", "internal"},
    {"gambatte_gb_internal_palette", "GB - Pocket"},
    {"gambatte_gbc_color_correction", "GBC only"},
    {"gambatte_gbc_color_correction_mode", "accurate"},
    {"gambatte_mix_frames", "disabled"},
    {"gambatte_gb_bootloader", "disabled"},
};

// mGBA (GBA). Same reasoning for blending, and the BIOS intro is skipped.
static const PlayerOption MGBA[] = {
    {"mgba_interframe_blending", "OFF"},
    {"mgba_skip_bios", "ON"},
};

const PlayerOption *core_defaults(const char *core, int *n)
{
    const PlayerOption *list = NULL;
    int count = 0;
    if (core && !strcmp(core, "gambatte")) { list = GAMBATTE; count = COUNT(GAMBATTE); }
    else if (core && !strcmp(core, "mgba")) { list = MGBA; count = COUNT(MGBA); }
    if (n) *n = count;
    return list;
}

// Four palettes, rendered through gambatte and picked for a backlit panel. Pocket is the
// default: Game Boy Pocket's soft grey-green keeps a near-black darkest shade for text
// without a glaring white. Green is the bright yellow-green of the original Game Boy as
// people remember it (gambatte's own "GB - DMG" is a murky olive that small text gets lost
// in), Teal the Game Boy Light's backlight, Mono plain greys. Mono is the core's
// colorization off rather than its "GBC - Grayscale", which gambatte treats as a GBC
// palette and colour corrects. The shades are what the core draws, for the menu's swatches.
static const struct {
    CorePalette shown;
    const char *colorization, *internal, *pack_key, *pack_value;
} PALETTES[PALETTE_COUNT] = {
    {{"pocket", "Pocket", {0xa5b29c, 0x84927b, 0x52614a, 0x293021}}, "internal", "GB - Pocket", NULL, NULL},
    {{"green", "Green", {0xfffb94, 0xc6db00, 0x526100, 0x003000}}, "internal", "PixelShift - Pack 1",
     "gambatte_gb_palette_pixelshift_1", "PixelShift 23 - GB Kiosk 2"},
    {{"teal", "Teal", {0x00cbde, 0x00b2d6, 0x299aad, 0x00718c}}, "internal", "GB - Light", NULL, NULL},
    {{"mono", "Mono", {0xffffff, 0xa5a5a5, 0x525252, 0x000000}}, "disabled", NULL, NULL, NULL},
};

const CorePalette *core_palette(int i)
{
    return i >= 0 && i < PALETTE_COUNT ? &PALETTES[i].shown : NULL;
}

CoreLcd core_lcd(const char *core, const char *rom_path)
{
    if (!core) return LCD_NONE;
    if (!strcmp(core, "mgba")) return LCD_GBA;
    if (strcmp(core, "gambatte")) return LCD_NONE;
    // 0x143: 0x80 runs on both and in colour on a GBC, 0xC0 only on a GBC.
    FILE *f = rom_path ? fopen(rom_path, "rb") : NULL;
    int flag = -1;
    if (f) {
        if (fseek(f, 0x143, SEEK_SET) == 0) flag = fgetc(f);
        fclose(f);
    }
    return flag >= 0 && (flag & 0x80) ? LCD_GBC : LCD_GB;
}

static int put(PlayerOption *out, int n, int max, const char *key, const char *value)
{
    if (n < max && key && value) out[n++] = (PlayerOption){key, value};
    return n;
}

int core_choices(const char *core, CoreLcd lcd, int colours, int palette, PlayerOption *out, int max)
{
    int n = 0;
    if (!core || !out) return 0;
    if (!strcmp(core, "gambatte") && lcd == LCD_GB) {
        if (palette < 0 || palette >= PALETTE_COUNT) palette = PALETTE_DEFAULT;
        n = put(out, n, max, "gambatte_gb_colorization", PALETTES[palette].colorization);
        n = put(out, n, max, "gambatte_gb_internal_palette", PALETTES[palette].internal);
        n = put(out, n, max, PALETTES[palette].pack_key, PALETTES[palette].pack_value);
    } else if (!strcmp(core, "gambatte") && lcd == LCD_GBC) {
        n = put(out, n, max, "gambatte_gbc_color_correction", colours ? "GBC only" : "disabled");
    } else if (!strcmp(core, "mgba") && lcd == LCD_GBA) {
        // The GBA's own screen was dark and washed out, so games were drawn oversaturated;
        // correcting for it gives the colours their artists saw.
        n = put(out, n, max, "mgba_color_correction", colours ? "GBA" : "OFF");
    }
    return n;
}
