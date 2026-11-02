#include "core_options.h"

#include <string.h>

#define COUNT(a) (int)(sizeof a / sizeof *a)

// gambatte (GB, GBC). Game Boy games get a palette instead of the core's plain grey:
// Game Boy Pocket's soft grey-green has the best contrast of the tinted palettes on a
// backlit panel (its darkest shade is near black, its lightest isn't glaring white), where
// the core's "GB - DMG" is a murky olive that small text gets lost in. Game Boy Color
// games are corrected to look like the GBC's own screen (the core's accurate model). No
// frame blending: it costs a frame copy and blend every frame, and only a few games need
// it. The boot logo is skipped when a boot ROM is on the card: A starts the game.
static const PlayerOption GAMBATTE[] = {
    {"gambatte_gb_colorization", "internal"},
    {"gambatte_gb_internal_palette", "GB - Pocket"},
    {"gambatte_gbc_color_correction", "GBC only"},
    {"gambatte_gbc_color_correction_mode", "accurate"},
    {"gambatte_mix_frames", "disabled"},
    {"gambatte_gb_bootloader", "disabled"},
};

// mGBA (GBA). The GBA's own screen was dark and washed out, so games were drawn with
// oversaturated colours; correcting for it gives the colours the artists saw. Same
// reasoning as above for blending, and the BIOS intro is skipped.
static const PlayerOption MGBA[] = {
    {"mgba_color_correction", "GBA"},
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
