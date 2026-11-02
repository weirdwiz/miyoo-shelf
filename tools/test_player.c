// Tests for the libretro host (src/player.c), driven by tools/fake_core.so.
// usage: test-player [core.so] [work-dir]
#include "../src/player.h"
#include "../src/platform.h" // the audio latency budget (constants only)

#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CORE_FPS 59.7275
#define OUT_RATE 48000
#define SRAM_SIZE 8192
#define STATE_SIZE (SRAM_SIZE + 8)

// fake_core pattern rows
#define ROW_COLORS 0
#define ROW_COUNTER 3
#define ROW_INPUT 4
#define ROW_TAGS 5
#define ROW_LIVE 6

static const char *core_path = "build/fake_core.so";
static char work[512], saves[600], saves_ro[600];
static char rom[600], rom_srm[600], rom_refuse[600], rom_missing[600];
static char bad_core[600], missing_core[600], tmp_file[600];
static int check_count;
static int buffer_ms; // what config() asks the player to keep buffered; 0: its default

static void fail_at(int line, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "FAIL line %d: ", line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        check_count++;                                                         \
        if (!(cond)) fail_at(__LINE__, __VA_ARGS__);                           \
    } while (0)

// --- helpers ------------------------------------------------------------------

static void rm_rf(const char *path)
{
    struct stat st;
    DIR *d;
    struct dirent *e;

    if (lstat(path, &st) != 0) return;
    if (!S_ISDIR(st.st_mode)) {
        remove(path);
        return;
    }
    if (!(d = opendir(path))) return;
    while ((e = readdir(d))) {
        char child[1024];
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(child, sizeof child, "%s/%s", path, e->d_name);
        rm_rf(child);
    }
    closedir(d);
    rmdir(path);
}

static void write_file(const char *path, const void *data, size_t n)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "cannot create %s", path);
    if (n) CHECK(fwrite(data, 1, n, f) == n, "cannot write %s", path);
    CHECK(fclose(f) == 0, "cannot close %s", path);
}

static void write_text(const char *path, const char *text)
{
    write_file(path, text, strlen(text));
}

static long file_size(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? (long)st.st_size : -1;
}

static uint32_t pix(int x, int y)
{
    const PlayerFrame *f = player_frame();
    CHECK(x < f->w && y < f->h, "pixel %d,%d is outside the %dx%d frame", x, y, f->w, f->h);
    return f->px[(size_t)y * f->pitch + x] & 0xffffffu;
}

// White/black bit pixels the fake core draws for the tests.
static unsigned bits_at(int row, int x0, int count)
{
    unsigned value = 0;
    for (int i = 0; i < count; i++) {
        uint32_t p = pix(x0 + i, row);
        CHECK(p == 0xffffffu || p == 0, "bit pixel %d,%d is 0x%06x, not black or white", x0 + i, row, p);
        if (p) value |= 1u << i;
    }
    return value;
}

static unsigned format_code(void) { return bits_at(ROW_TAGS, 0, 2); }
static unsigned frame_counter(void) { return bits_at(ROW_COUNTER, 0, 8); }
static unsigned input_mask(void) { return bits_at(ROW_INPUT, 0, 16); }
static unsigned tint_code(void) { return bits_at(ROW_LIVE, 0, 2); }
static unsigned updates_seen(void) { return bits_at(ROW_LIVE, 8, 8); }

static void config(PlayerConfig *cfg, const char *core, const char *rom_path, const char *save_dir,
                   const char *stem, const char *options)
{
    memset(cfg, 0, sizeof *cfg);
    cfg->core_path = core;
    cfg->rom_path = rom_path;
    cfg->system_dir = work;
    cfg->save_dir = save_dir;
    cfg->stem = stem;
    cfg->options_path = options;
    cfg->sample_rate = OUT_RATE;
    cfg->buffer_ms = buffer_ms;
}

static void open_ok(const char *rom_path, const char *save_dir, const char *stem, const char *options)
{
    PlayerConfig cfg;
    char err[256];

    config(&cfg, core_path, rom_path, save_dir, stem, options);
    if (player_open(&cfg, err, sizeof err) != 0) fail_at(__LINE__, "open failed: %s", err);
    CHECK(player_is_open() == 1, "player_is_open() after a successful open");
}

static void expect_open_fail(const char *core, const char *rom_path, const char *what)
{
    PlayerConfig cfg;
    char err[256];

    config(&cfg, core, rom_path, saves, "fail", NULL);
    err[0] = 0;
    CHECK(player_open(&cfg, err, sizeof err) == -1, "%s: open should fail", what);
    CHECK(err[0] != 0, "%s: open should explain itself", what);
    CHECK(player_is_open() == 0, "%s: a failed open must leave nothing open", what);
}

static void check_reference_colors(void);

static void run_frames(int n)
{
    for (int i = 0; i < n; i++) player_run();
}

// --- tests --------------------------------------------------------------------

static void test_open_close(void)
{
    const PlayerFrame *f;

    open_ok(rom, saves, "game", NULL);
    CHECK(player_fps() > CORE_FPS - 0.01 && player_fps() < CORE_FPS + 0.01,
          "core fps should be %.4f, got %.4f", CORE_FPS, player_fps());
    f = player_frame();
    CHECK(f->aspect > 1.11f && f->aspect < 1.12f, "aspect should be 160/144, got %.4f", f->aspect);
    CHECK(player_audio_queued() == 0, "a fresh player has no buffered audio");
    player_run();
    f = player_frame();
    CHECK(f->w == 160 && f->h == 144, "frame should be 160x144, got %dx%d", f->w, f->h);
    CHECK(f->pitch == 160, "pitch should be 160 pixels, got %d", f->pitch);
    CHECK(f->serial == 1, "the first frame should bump the serial to 1, got %u", f->serial);
    player_close();
    CHECK(player_is_open() == 0, "player_is_open() after close");
    printf("open/close\n");
}

static void test_open_failures(void)
{
    expect_open_fail(missing_core, rom, "missing core");
    expect_open_fail(bad_core, rom, "not a shared library");
    expect_open_fail(core_path, rom_missing, "missing ROM");
    expect_open_fail(core_path, rom_refuse, "game the core refuses");

    open_ok(rom, saves, "busy", NULL);
    {
        PlayerConfig cfg;
        char err[256];
        config(&cfg, core_path, rom, saves, "busy", NULL);
        err[0] = 0;
        CHECK(player_open(&cfg, err, sizeof err) == -1, "a second open must fail");
        CHECK(err[0] != 0, "a second open must explain itself");
        CHECK(player_is_open() == 1, "a refused second open must leave the first one running");
        player_run();
    }
    player_close();
    printf("open failures\n");
}

static void test_rom_too_large(void)
{
    char big[600];
    int fd;

    snprintf(big, sizeof big, "%s/huge.gb", work);
    fd = open(big, O_CREAT | O_WRONLY, 0644);
    CHECK(fd >= 0, "cannot create %s", big);
    CHECK(ftruncate(fd, (off_t)(64u << 20) + 1) == 0, "cannot size %s", big);
    close(fd);
    expect_open_fail(core_path, big, "ROM over 64 MiB");
    remove(big);
    printf("oversized ROM\n");
}

static void test_fullpath(void)
{
    setenv("FAKE_CORE_FULLPATH", "1", 1);
    open_ok(rom, saves, "fullpath", NULL);
    player_run();
    check_reference_colors();
    player_close();
    expect_open_fail(core_path, rom_refuse, "refused game for a fullpath core");
    unsetenv("FAKE_CORE_FULLPATH");
    printf("need_fullpath core\n");
}

static void check_reference_colors(void)
{
    CHECK(pix(0, ROW_COLORS) == 0xff0000u, "red should convert to 0xff0000, got 0x%06x", pix(0, ROW_COLORS));
    CHECK(pix(1, ROW_COLORS) == 0x00ff00u, "green should convert to 0x00ff00, got 0x%06x", pix(1, ROW_COLORS));
    CHECK(pix(2, ROW_COLORS) == 0x0000ffu, "blue should convert to 0x0000ff, got 0x%06x", pix(2, ROW_COLORS));
    CHECK(pix(3, ROW_COLORS) == 0xffffffu, "white should convert to 0xffffff, got 0x%06x", pix(3, ROW_COLORS));
    CHECK(pix(4, ROW_COLORS) == 0x000000u, "black should convert to 0x000000, got 0x%06x", pix(4, ROW_COLORS));
}

static void test_pixel_formats(void)
{
    static const struct {
        const char *options;
        unsigned code;
        const char *name;
    } cases[] = {
        { NULL, 0, "RGB565 (default)" },
        { "fake_format = \"xrgb\"\n", 1, "XRGB8888" },
        { "fake_format = \"1555\"\n", 2, "0RGB1555" },
    };

    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char path[600];
        snprintf(path, sizeof path, "%s/format%d.opt", work, (int)i);
        if (cases[i].options) write_text(path, cases[i].options);
        else remove(path);
        open_ok(rom, saves, "fmt", cases[i].options ? path : NULL);
        player_run();
        CHECK(format_code() == cases[i].code, "%s: core should report format %u, got %u",
              cases[i].name, cases[i].code, format_code());
        check_reference_colors();
        player_close();
    }
    // The deprecated SET_VARIABLES path has to work too.
    setenv("FAKE_CORE_LEGACY_OPTIONS", "1", 1);
    {
        char path[600];
        snprintf(path, sizeof path, "%s/legacy.opt", work);
        write_text(path, "fake_format = \"xrgb\"\n");
        open_ok(rom, saves, "fmt", path);
        player_run();
        CHECK(format_code() == 1, "SET_VARIABLES path: expected XRGB8888, got format %u", format_code());
        check_reference_colors();
        player_close();
    }
    unsetenv("FAKE_CORE_LEGACY_OPTIONS");
    printf("pixel formats\n");
}

static void test_dupe(void)
{
    unsigned serial;

    open_ok(rom, saves, "dupe", NULL);
    run_frames(3);  // draws counters 0, 1, 2
    serial = player_frame()->serial;
    CHECK(serial == 3, "three draws should give serial 3, got %u", serial);
    player_run();  // counter 3 is the dupe
    CHECK(player_frame()->serial == serial, "a duped frame must not bump the serial");
    CHECK(frame_counter() == 2, "a duped frame keeps the previous pixels, got counter %u",
          frame_counter());
    player_run();  // counter 4
    CHECK(player_frame()->serial == serial + 1, "the frame after a dupe bumps the serial");
    CHECK(frame_counter() == 4, "the frame after a dupe shows counter 4, got %u", frame_counter());
    player_close();
    printf("duped frames\n");
}

static void test_input(void)
{
    unsigned mask = PLAYER_A | PLAYER_START | PLAYER_LEFT;

    open_ok(rom, saves, "input", NULL);
    player_set_input(0);
    player_run();
    CHECK(input_mask() == 0, "no keys held should reach the core, got 0x%04x", input_mask());
    player_set_input(mask);
    player_run();  // counter 1: drawn, not a dupe
    CHECK(input_mask() == mask, "the joypad mask should reach the core: expected 0x%04x, got 0x%04x",
          mask, input_mask());
    player_set_input(0);
    player_run();
    CHECK(input_mask() == 0, "releasing the keys should reach the core, got 0x%04x", input_mask());
    player_close();
    printf("input\n");
}

static void test_options(void)
{
    char path[600];
    snprintf(path, sizeof path, "%s/opts.opt", work);

    // A malformed file, an unknown key and an invalid value: the default wins.
    write_text(path,
               "# a comment\n"
               "this line is malformed\n"
               "unknown_key = \"whatever\"\n"
               "fake_format = \"bogus\"\n");
    open_ok(rom, saves, "opts", path);
    player_run();
    CHECK(format_code() == 0, "an invalid option value should fall back to the default, got format %u",
          format_code());
    player_close();

    // A valid value with stray whitespace is still read.
    write_text(path, "  fake_format   =  \"1555\"   \nnoise\n");
    open_ok(rom, saves, "opts", path);
    player_run();
    CHECK(format_code() == 2, "a valid option value should be used, got format %u", format_code());
    player_close();

    // No file at all: defaults.
    open_ok(rom, saves, "opts", NULL);
    player_run();
    CHECK(format_code() == 0, "no option file means core defaults, got format %u", format_code());
    player_close();
    printf("core options\n");
}

static void open_with(const char *options, const PlayerOption *defs, int ndefs)
{
    PlayerConfig cfg;
    char err[256];

    config(&cfg, core_path, rom, saves, "live", options);
    cfg.defaults = defs;
    cfg.ndefaults = ndefs;
    if (player_open(&cfg, err, sizeof err) != 0) fail_at(__LINE__, "open failed: %s", err);
}

// Shelf's defaults sit between the core's own and the options file.
static void test_shelf_defaults(void)
{
    static const PlayerOption red[] = { { "fake_tint", "red" }, { "fake_format", "xrgb" } };
    static const PlayerOption bad[] = { { "fake_tint", "green" }, { NULL, "x" }, { "fake_format", NULL },
                                        { "fake_format", "" } };
    char path[600];
    snprintf(path, sizeof path, "%s/live.opt", work);
    setenv("FAKE_CORE_NO_DUPES", "1", 1); // every frame redraws row 6

    for (int legacy = 0; legacy < 2; legacy++) {
        if (legacy) setenv("FAKE_CORE_LEGACY_OPTIONS", "1", 1);

        open_with(NULL, red, 2);
        player_run();
        CHECK(tint_code() == 1, "Shelf's default should win over the core's, got tint %u", tint_code());
        CHECK(format_code() == 1, "every default applies, got format %u", format_code());
        CHECK(updates_seen() == 0, "nothing changed yet, but the core saw %u updates", updates_seen());
        player_close();

        // The options file wins over Shelf's defaults, for the keys it sets.
        write_text(path, "fake_tint = \"blue\"\n");
        open_with(path, red, 2);
        player_run();
        CHECK(tint_code() == 2, "the options file should win over Shelf's default, got tint %u", tint_code());
        CHECK(format_code() == 1, "keys the file leaves out keep Shelf's default, got format %u", format_code());
        player_close();

        // A default the core doesn't allow, and malformed entries, change nothing.
        open_with(NULL, bad, 4);
        player_run();
        CHECK(tint_code() == 0 && format_code() == 0, "invalid defaults fall back to the core's (tint %u, format %u)",
              tint_code(), format_code());
        player_close();
    }
    unsetenv("FAKE_CORE_LEGACY_OPTIONS");
    unsetenv("FAKE_CORE_NO_DUPES");
    printf("Shelf's option defaults\n");
}

// player_set_option changes a value while the game runs, and the core hears about it once.
static void test_live_options(void)
{
    static const PlayerOption red[] = { { "fake_tint", "red" } };
    char path[600];
    snprintf(path, sizeof path, "%s/live.opt", work);
    setenv("FAKE_CORE_NO_DUPES", "1", 1);

    for (int legacy = 0; legacy < 2; legacy++) {
        if (legacy) setenv("FAKE_CORE_LEGACY_OPTIONS", "1", 1);
        open_with(NULL, red, 1);
        player_run();
        CHECK(player_set_option("fake_tint", "blue") == 1, "a new allowed value is a change");
        player_run();
        CHECK(tint_code() == 2, "the core should re-read the option live, got tint %u", tint_code());
        CHECK(updates_seen() == 1, "one change is one update, got %u", updates_seen());
        player_run();
        CHECK(updates_seen() == 1, "the update is reported once, got %u", updates_seen());

        CHECK(player_set_option("fake_tint", "blue") == 0, "the same value is no change");
        CHECK(player_set_option("fake_tint", "green") == -1, "a value the core doesn't allow is refused");
        CHECK(player_set_option("no_such_key", "on") == -1, "an unknown key is refused");
        CHECK(player_set_option("fake_tint", "") == -1, "an empty value is refused");
        CHECK(player_set_option("fake_tint", NULL) == -1 && player_set_option(NULL, "red") == -1,
              "a missing key or value is refused");
        player_run();
        CHECK(updates_seen() == 1 && tint_code() == 2, "refused changes tell the core nothing (%u, tint %u)",
              updates_seen(), tint_code());

        // Two changes before the next frame are one update with the newest value.
        CHECK(player_set_option("fake_tint", "red") == 1 && player_set_option("fake_tint", "off") == 1,
              "back-to-back changes both apply");
        player_run();
        CHECK(updates_seen() == 2 && tint_code() == 0, "one update for both (%u, tint %u)", updates_seen(),
              tint_code());
        player_close();
        CHECK(player_set_option("fake_tint", "red") == -1, "no player, no options");

        // A live change wins over the options file too.
        write_text(path, "fake_tint = \"blue\"\n");
        open_with(path, red, 1);
        player_run();
        CHECK(tint_code() == 2 && updates_seen() == 0, "the file's value first (tint %u)", tint_code());
        CHECK(player_set_option("fake_tint", "red") == 1, "a key the options file sets can still change");
        player_run();
        CHECK(tint_code() == 1 && updates_seen() == 1, "the live value wins (tint %u, %u updates)", tint_code(),
              updates_seen());
        player_close();

        // A new session starts from the configured values again.
        open_with(NULL, red, 1);
        player_run();
        CHECK(tint_code() == 1 && updates_seen() == 0, "nothing carries over (tint %u, %u updates)", tint_code(),
              updates_seen());
        player_close();
    }
    unsetenv("FAKE_CORE_LEGACY_OPTIONS");
    unsetenv("FAKE_CORE_NO_DUPES");
    printf("live core options\n");
}

static void test_state(void)
{
    char state[600];
    unsigned char head[STATE_SIZE];

    snprintf(state, sizeof state, "%s/state.bin", work);
    open_ok(rom, saves, "state", NULL);
    run_frames(6);
    CHECK(frame_counter() == 5, "six runs show counter 5, got %u", frame_counter());
    CHECK(player_save_state(state) == 0, "saving a state should succeed");
    CHECK(file_size(state) == STATE_SIZE, "state should be %d bytes, got %ld", STATE_SIZE,
          file_size(state));

    run_frames(3);
    CHECK(frame_counter() == 8, "nine runs show counter 8, got %u", frame_counter());
    CHECK(player_load_state(state) == 0, "loading a state should succeed");
    player_run();
    CHECK(frame_counter() == 6, "a loaded state restores the frame counter, got %u", frame_counter());

    head[0] = 1;
    write_file(tmp_file, head, 4);  // too short for the core
    CHECK(player_load_state(tmp_file) == -1, "a truncated state file must be rejected");
    CHECK(player_load_state("/nonexistent/state.bin") == -1, "a missing state file must be rejected");
    player_close();
    printf("save states\n");
}

static void test_sram(void)
{
    char srm[600], rtc[600];
    unsigned char data[SRAM_SIZE];
    FILE *f;

    snprintf(srm, sizeof srm, "%s/srmgame.srm", saves);
    snprintf(rtc, sizeof rtc, "%s/srmgame.rtc", saves);
    remove(srm);
    remove(rtc);
    open_ok(rom_srm, saves, "srmgame", NULL);
    run_frames(3);
    CHECK(player_save_sram() == 0, "saving SRAM should succeed");
    f = fopen(srm, "rb");
    CHECK(f != NULL, "SRAM should be written to %s", srm);
    CHECK(fread(data, 1, sizeof data, f) == sizeof data, "SRAM file should be %d bytes", SRAM_SIZE);
    fclose(f);
    CHECK(data[0] == 2, "the core wrote frame counter 2 to SRAM, got %u", data[0]);
    CHECK(file_size(rtc) == 8, "the core RTC should be saved too, got %ld bytes", file_size(rtc));

    // Nothing changed: the writer must not touch the file again.
    CHECK(remove(srm) == 0, "cannot remove %s", srm);
    CHECK(player_save_sram() == 0, "an unchanged save should succeed");
    CHECK(file_size(srm) == -1, "an unchanged save must not rewrite the file");

    run_frames(1);
    CHECK(player_save_sram() == 0, "saving changed SRAM should succeed");
    CHECK(file_size(srm) == SRAM_SIZE, "changed SRAM should be written, got %ld bytes", file_size(srm));
    player_close();

    // The next session starts from the file on disk.
    open_ok(rom_srm, saves, "srmgame", NULL);
    player_run();
    CHECK(bits_at(ROW_TAGS, 8, 8) == 3, "the next session should load SRAM byte 3, got %u",
          bits_at(ROW_TAGS, 8, 8));
    player_close();
    printf("SRAM save and load\n");
}

static void test_sram_write_failure(void)
{
    char srm[600];
    unsigned char before[SRAM_SIZE], after[SRAM_SIZE];
    long size;
    FILE *f;

    snprintf(srm, sizeof srm, "%s/ro.srm", saves_ro);
    remove(srm);
    open_ok(rom_srm, saves_ro, "ro", NULL);
    run_frames(2);
    CHECK(player_save_sram() == 0, "saving SRAM before the read-only test should succeed");
    size = file_size(srm);
    CHECK(size == SRAM_SIZE, "the original SRAM file should exist, got %ld bytes", size);
    f = fopen(srm, "rb");
    CHECK(f && fread(before, 1, sizeof before, f) == sizeof before, "cannot read %s", srm);
    if (f) fclose(f);

    CHECK(chmod(saves_ro, 0555) == 0, "cannot make %s read-only", saves_ro);
    run_frames(1);
    CHECK(player_save_sram() == -1, "a write to a read-only directory must fail");
    CHECK(file_size(srm) == size, "a failed write must leave the old SRAM file intact");
    f = fopen(srm, "rb");
    CHECK(f && fread(after, 1, sizeof after, f) == sizeof after, "cannot re-read %s", srm);
    if (f) fclose(f);
    CHECK(memcmp(before, after, sizeof before) == 0, "a failed write must not corrupt the old SRAM");
    {
        char temp[700];
        snprintf(temp, sizeof temp, "%s.tmp", srm);
        CHECK(file_size(temp) == -1, "a failed write must not leave %s behind", temp);
    }

    CHECK(chmod(saves_ro, 0755) == 0, "cannot restore write access on %s", saves_ro);
    player_close();  // saves the changed SRAM now that the directory is writable again
    printf("SRAM write failure\n");
}

static void test_reopen_cycles(void)
{
    for (int i = 0; i < 3; i++) {
        open_ok(rom, saves, "cycle", NULL);
        run_frames(2);
        CHECK(frame_counter() == 1, "cycle %d: expected counter 1, got %u", i, frame_counter());
        CHECK(player_audio_queued() > 0, "cycle %d: the core should have produced audio", i);
        player_close();
        CHECK(player_is_open() == 0, "cycle %d: player_is_open() after close", i);
    }
    printf("three open/close cycles\n");
}

typedef struct {
    double skew;  // consumer clock multiplier
    int warmup;   // game frames before underruns and production count
    _Atomic unsigned vframes;
    _Atomic long long requested;
    _Atomic int done;
    long long got, got_warm, padded, maxq;
    int underruns;
} AudioRun;

// Reads audio in device-sized chunks at a fixed output rate. The game loop waits for
// each frame's worth of audio to be taken, so frames stand in for wall time and the
// test never sleeps.
static void *audio_consumer(void *arg)
{
    AudioRun *a = arg;
    int16_t *buf = malloc(sizeof(int16_t) * 2 * 1024);
    long long got = 0, got_warm = 0, padded = 0, maxq = 0;
    int underruns = 0;

    if (!buf) return NULL;
    for (;;) {
        unsigned f = atomic_load_explicit(&a->vframes, memory_order_acquire);
        // Two frames of lookahead: a real audio device runs a fixed latency behind the
        // video, which is what keeps its ring from starving between game frames.
        double clock = f > 2 ? (double)(f - 2) : 0.0;
        long long want = (long long)((double)OUT_RATE * a->skew * clock / CORE_FPS);
        long long need = want - atomic_load_explicit(&a->requested, memory_order_relaxed);
        if (need > 0) {
            int n = need > 1024 ? 1024 : (int)need;
            int real = player_audio_read(buf, n);
            int q = player_audio_queued();
            if (q > maxq) maxq = q;
            got += real;
            padded += n - real;
            if (got_warm == 0 && f > (unsigned)a->warmup) got_warm = got;
            if (real < n && f > (unsigned)a->warmup) underruns++;
            atomic_store_explicit(&a->requested, want, memory_order_release);
            continue;
        }
        if (atomic_load_explicit(&a->done, memory_order_acquire)) break;
        sched_yield();
    }
    a->got = got;
    a->got_warm = got_warm;
    a->padded = padded;
    a->maxq = maxq;
    a->underruns = underruns;
    free(buf);
    return NULL;
}

// Runs `frames` game frames with the audio consumer pacing them. Returns the ring fill
// at the end of the warm-up and at the end of the run.
static void audio_run(double skew, int frames, int warmup, AudioRun *out, int *fill_warm,
                      int *fill_end)
{
    pthread_t thread;

    memset(out, 0, sizeof *out);
    out->skew = skew;
    out->warmup = warmup;
    atomic_store(&out->vframes, 0);
    atomic_store(&out->requested, 0);
    atomic_store(&out->done, 0);
    *fill_warm = *fill_end = 0;
    CHECK(pthread_create(&thread, NULL, audio_consumer, out) == 0, "cannot start the audio consumer");
    for (int i = 0; i < frames; i++) {
        long long want;
        player_run();
        atomic_store_explicit(&out->vframes, (unsigned)i + 1, memory_order_release);
        want = (long long)((double)OUT_RATE * skew * (double)(i > 1 ? i - 1 : 0) / CORE_FPS);
        while (atomic_load_explicit(&out->requested, memory_order_acquire) < want) sched_yield();
        if (i + 1 == warmup) *fill_warm = player_audio_queued();
    }
    *fill_end = player_audio_queued();
    atomic_store_explicit(&out->done, 1, memory_order_release);
    CHECK(pthread_join(thread, NULL) == 0, "cannot join the audio consumer");
}

static void test_audio_timing(void)
{
    const int frames = 600, warmup = 120;
    const double expected = (double)OUT_RATE * (frames - warmup) / CORE_FPS;
    AudioRun run;
    int fill_warm, fill_end;
    int16_t buf[256 * 2];

    open_ok(rom, saves, "audio", NULL);
    audio_run(1.0, frames, warmup, &run, &fill_warm, &fill_end);
    {
        double produced = (double)(run.got - run.got_warm) + (double)(fill_end - fill_warm);
        CHECK(run.underruns == 0, "the ring underran %d times after warm-up", run.underruns);
        CHECK(fabs(produced - expected) < expected * 0.01,
              "produced %.0f audio frames over %d game frames, expected %.0f (%.2f%% off)",
              produced, frames - warmup, expected, 100.0 * (produced - expected) / expected);
        CHECK(fill_end > 0, "the ring should hold audio at the end of the run");
    }

    // Flushing leaves silence: an empty ring pads and reports no real frames.
    player_audio_flush();
    CHECK(player_audio_queued() == 0, "flush should empty the ring, %d left", player_audio_queued());
    memset(buf, 0x7f, sizeof buf);
    CHECK(player_audio_read(buf, 256) == 0, "an empty ring reports no real frames");
    for (int i = 0; i < 256 * 2; i++) {
        CHECK(buf[i] == 0, "an empty ring must pad with silence, sample %d is %d", i, buf[i]);
    }
    player_close();
    printf("audio timing and flush\n");
}

static void test_audio_drc(void)
{
    const int frames = 36000;
    const int warmup = 600;
    const long max_fill = 2L * OUT_RATE * 32 / 1000;  // twice the default 32 ms target
    static const double skews[] = { 1.0045, 0.9955 };

    for (size_t i = 0; i < sizeof skews / sizeof *skews; i++) {
        AudioRun run;
        int fill_warm, fill_end;
        open_ok(rom, saves, "drc", NULL);
        audio_run(skews[i], frames, warmup, &run, &fill_warm, &fill_end);
        printf("  skew %.4f: %lld served, %lld padded, max fill %lld, end fill %d, underruns %d\n",
               skews[i], run.got, run.padded, run.maxq, fill_end, run.underruns);
        CHECK(run.underruns == 0, "skew %.4f: the ring underran %d times after warm-up", skews[i],
              run.underruns);
        CHECK(run.maxq <= max_fill, "skew %.4f: fill reached %lld frames, past %ld",
              skews[i], run.maxq, max_fill);
        CHECK(fill_end > 0, "skew %.4f: the ring should still hold audio", skews[i]);
        player_close();
    }
    printf("dynamic rate control\n");
}

// The latency budget in platform.h, played out on a simulated clock: the audio thread
// pulls PLATFORM_AUDIO_PERIOD frames each time the device frees a period, while game
// frames are due every 1/fps on the game's clock and each finishes, making its sound in
// one burst, 2-14 ms after it was due. The device's clock runs `skew` faster than the
// game's. Single-threaded, so it is exact and repeatable. After a warm-up the ring must
// never run dry and must hold about PLATFORM_AUDIO_RING_MS, not more.
static void test_audio_latency(void)
{
    static const struct { double skew, cost_min, cost_max; } RUN[] = {
        {1.0, .002, .006},    {1.0, .002, .014},    {1.0003, .002, .014},
        {0.9997, .002, .014}, {1.0045, .002, .014}, {0.9955, .002, .014},
    };
    const double secs = 300, warm = 60, frame = 1.0 / CORE_FPS;
    const int period = PLATFORM_AUDIO_PERIOD;
    int16_t buf[PLATFORM_AUDIO_PERIOD * 2];

    for (size_t r = 0; r < sizeof RUN / sizeof *RUN; r++) {
        unsigned rng = 1;
        long frames = 0, pulls = 0, underruns = 0, max_fill = 0, min_fill = 1L << 30;
        double area = 0, since = warm, frame_at = RUN[r].cost_min, pull_every = period / (OUT_RATE * RUN[r].skew);
        int started = 0;

        buffer_ms = PLATFORM_AUDIO_RING_MS;
        open_ok(rom, saves, "latency", NULL);
        buffer_ms = 0;
        for (;;) {
            double pull_at = pulls * pull_every;
            if (pull_at > secs) break;
            if (frame_at <= pull_at) {
                if (frame_at >= warm) { // the fill over time, for the average
                    area += player_audio_queued() * (frame_at - since);
                    since = frame_at;
                }
                player_run();
                frames++;
                rng = rng * 1103515245u + 12345u;
                double u = (double)((rng >> 8) & 0xffff) / 65535.0;
                frame_at = frames * frame + RUN[r].cost_min + (RUN[r].cost_max - RUN[r].cost_min) * u;
                continue;
            }
            int queued = player_audio_queued();
            if (pull_at >= warm) {
                area += queued * (pull_at - since);
                since = pull_at;
            }
            int real = player_audio_read(buf, period);
            if (real > 0) started = 1;
            if (started && real < period) underruns++;
            if (pull_at >= warm) {
                if (queued > max_fill) max_fill = queued;
                if (queued - real < min_fill) min_fill = queued - real;
            }
            pulls++;
        }
        double mean_ms = 1000.0 * area / (since - warm) / OUT_RATE;
        double total_ms = mean_ms + 1000.0 * PLATFORM_AUDIO_PERIODS * period / OUT_RATE;
        printf("  skew %.4f, frames done %2.0f-%2.0f ms late: ring %.1f ms on average, %.1f-%.1f, "
               "~%.0f ms to the speaker, %ld underruns\n",
               RUN[r].skew, RUN[r].cost_min * 1e3, RUN[r].cost_max * 1e3, mean_ms,
               1000.0 * min_fill / OUT_RATE, 1000.0 * max_fill / OUT_RATE, total_ms, underruns);
        CHECK(started, "skew %.4f: no sound was ever read", RUN[r].skew);
        CHECK(underruns == 0, "skew %.4f: the ring ran dry %ld times", RUN[r].skew, underruns);
        CHECK(mean_ms < PLATFORM_AUDIO_RING_MS + 8, "skew %.4f: the ring held %.1f ms on average, want ~%d",
              RUN[r].skew, mean_ms, PLATFORM_AUDIO_RING_MS);
        CHECK(total_ms < 70, "skew %.4f: ~%.0f ms from the game to the speaker", RUN[r].skew, total_ms);
        player_close();
    }
    printf("audio latency\n");
}

// Priming: after a flush nothing is read until the ring holds the target, so playback
// restarts with its cushion rather than running dry on the first period.
static void test_audio_priming(void)
{
    int16_t buf[PLATFORM_AUDIO_PERIOD * 2];
    const int target = OUT_RATE * PLATFORM_AUDIO_RING_MS / 1000;

    buffer_ms = PLATFORM_AUDIO_RING_MS;
    open_ok(rom, saves, "priming", NULL);
    buffer_ms = 0;
    for (int pass = 0; pass < 2; pass++) {
        player_run(); // ~804 frames: less than the 24 ms target
        CHECK(player_audio_queued() > 0 && player_audio_queued() < target, "one frame should be under the target");
        CHECK(player_audio_read(buf, PLATFORM_AUDIO_PERIOD) == 0, "pass %d: read before the ring is primed", pass);
        CHECK(player_audio_queued() > 0, "pass %d: an unprimed read must not take anything", pass);
        player_run();
        CHECK(player_audio_queued() >= target, "two frames should reach the target");
        CHECK(player_audio_read(buf, PLATFORM_AUDIO_PERIOD) == PLATFORM_AUDIO_PERIOD, "pass %d: primed read", pass);
        player_run(); // once primed it reads whatever there is
        while (player_audio_read(buf, PLATFORM_AUDIO_PERIOD) == PLATFORM_AUDIO_PERIOD) {}
        CHECK(player_audio_queued() == 0, "a primed ring reads to the end");
        player_audio_flush();
    }
    player_close();
    printf("audio priming\n");
}

int main(int argc, char **argv)
{
    unsigned char rom_data[1024];
    unsigned char refuse_data[64];

    if (argc > 1) core_path = argv[1];
    if (argc > 2) snprintf(work, sizeof work, "%s", argv[2]);
    else snprintf(work, sizeof work, "build/test-player-work");

    rm_rf(work);
    CHECK(mkdir(work, 0755) == 0, "cannot create %s", work);
    snprintf(saves, sizeof saves, "%s/saves", work);
    snprintf(saves_ro, sizeof saves_ro, "%s/saves-ro", work);
    CHECK(mkdir(saves, 0755) == 0, "cannot create %s", saves);
    CHECK(mkdir(saves_ro, 0755) == 0, "cannot create %s", saves_ro);
    snprintf(rom, sizeof rom, "%s/game.gb", work);
    snprintf(rom_srm, sizeof rom_srm, "%s/srm.gb", work);
    snprintf(rom_refuse, sizeof rom_refuse, "%s/refuse.gb", work);
    snprintf(rom_missing, sizeof rom_missing, "%s/not-here.gb", work);
    snprintf(bad_core, sizeof bad_core, "%s/not-a-core.so", work);
    snprintf(missing_core, sizeof missing_core, "%s/no-such-core.so", work);
    snprintf(tmp_file, sizeof tmp_file, "%s/leftover.tmp", work);

    memset(rom_data, 0x42, sizeof rom_data);
    write_file(rom, rom_data, sizeof rom_data);
    write_file(rom_srm, rom_data, sizeof rom_data);
    memset(refuse_data, 0, sizeof refuse_data);
    refuse_data[0] = 'X';
    write_file(rom_refuse, refuse_data, sizeof refuse_data);
    write_text(bad_core, "this is not a shared library\n");

    test_open_close();
    test_open_failures();
    test_rom_too_large();
    test_fullpath();
    test_pixel_formats();
    test_dupe();
    test_input();
    test_options();
    test_shelf_defaults();
    test_live_options();
    test_state();
    test_sram();
    test_sram_write_failure();
    test_reopen_cycles();
    test_audio_timing();
    test_audio_drc();
    test_audio_latency();
    test_audio_priming();

    rm_rf(work);
    printf("all %d checks passed\n", check_count);
    return 0;
}
