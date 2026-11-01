#include "../src/fb_snapshot.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    // Padded pitch and both possible original pages. RA may draw only the viewport;
    // its borders must not retain pixels from Shelf after resume or switching games.
    for (int page = 0; page <= 1; page++) {
        unsigned char memory[160], original[160];
        for (int i = 0; i < 160; i++) original[i] = memory[i] = (unsigned char)(i * 13);
        FbSnapshot s = {0};
        assert(!fb_snapshot_take(&s, memory, sizeof memory, 20, 4, page * 4));
        memset(memory, 0xff, sizeof memory); // overlay overwrites both pages
        assert(!fb_snapshot_restore(&s, memory, sizeof memory));
        assert(!memcmp(memory + page * 80, original + page * 80, 80));
        assert(fb_snapshot_restore(&s, memory, 79) == -1);
        fb_snapshot_free(&s);
        assert(!s.pixels);
    }
    // One visible page (here the third of three) restores every page an overlay drew on.
    {
        unsigned char memory[240], original[240];
        for (int i = 0; i < 240; i++) original[i] = memory[i] = (unsigned char)(i * 7);
        FbSnapshot one = {0};
        assert(!fb_snapshot_take(&one, memory, sizeof memory, 20, 4, 8));
        memset(memory, 0xff, sizeof memory);
        assert(!fb_snapshot_restore_pages(&one, memory, sizeof memory, 3));
        for (int p = 0; p < 3; p++) assert(!memcmp(memory + p * 80, original + 160, 80));
        assert(fb_snapshot_restore_pages(&one, memory, 239, 3) == -1); // a page past the end
        fb_snapshot_free(&one);
    }
    unsigned char memory[16] = {0};
    FbSnapshot s = {0};
    assert(fb_snapshot_take(&s, memory, 16, 8, 3, 0) == -1);
    assert(fb_snapshot_take(&s, memory, 16, 8, 2, 1) == -1);
    assert(fb_snapshot_take(&s, memory, 16, 0, 2, 0) == -1);
    assert(fb_snapshot_take(&s, memory, 16, 8, 2, -1) == -1);
    // RetroArch triple-buffers (1440 rows): its visible page may be the third, which
    // must stay mapped for the snapshot; Shelf still needs two pages of its own.
    assert(fb_map_len(2560, 480, 1440, 960, 2560u * 1440) == 2560u * 1440);
    assert(fb_map_len(2560, 480, 960, 480, 2560u * 960) == 2560u * 960);
    assert(fb_map_len(2560, 480, 1440, 961, 2560u * 1440) == 0);  // page past the end
    assert(fb_map_len(2560, 480, 480, 0, 2560u * 480) == 0);      // no second page
    assert(fb_map_len(2560, 480, 1440, 960, 2560u * 960) == 0);   // smaller than declared
    puts("framebuffer lease restores original page, padding and borders; bounds checked");
    return 0;
}
