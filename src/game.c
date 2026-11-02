// One play session: the core runs the game inside Shelf while the frame loop feeds it
// input and presents its frames. MENU pauses it into the switcher, POWER sleeps, and
// every way out saves the state, the SRAM and a screenshot before the core is unloaded.
#include "game.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// stb_image_write's Radiance HDR path uses sprintf; it is third-party and Shelf only
// writes PNGs, so its deprecation warning is not ours to fix.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"
#pragma GCC diagnostic pop

#include "core_options.h"
#include "icons.h"
#include "loop.h"
#include "player.h"
#include "platform.h"
#include "power.h"
#include "ui.h"

#define SRAM_SECONDS 30.0 // how often the battery-backed save is written

typedef struct {
    Library *lib;
    int game;
    const GameSettings *set;
    char stem[256];
    char save_dir[PATH_LEN]; // host paths: the simulator maps /mnt/SDCARD to SHELF_ROOT
    char rom[PATH_LEN * 2];
    char core[PATH_LEN * 2];
    char state[PATH_LEN * 2];
    char shot[PATH_LEN * 2];
    double played;           // seconds actually spent playing, menu and standby excluded
    int switch_to;           // the game picked in the in-game menu
    PowerState power;        // dimming, idle standby and the battery, across play and menu
    PowerBattery battery;    // the newest reading
    double last_input;       // the last press, or change in the buttons held while playing
    int undimmed;            // the last power step woke a dimmed menu
    const char *core_name;   // "gambatte", "mgba": which options the choices set
    CoreLcd lcd;             // which colour choice the game gets
    Settings look;           // the settings it started with (set->settings, or the defaults),
                             // kept up to date with the colour row's changes
    int lost;                // the game couldn't be put back after a preview: save nothing
} Session;

static char problem[192]; // see game_error()
static void report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(problem, sizeof problem, fmt, ap);
    va_end(ap);
    fprintf(stderr, "shelf: %s\n", problem);
}

// The player-facing message drops detail like the file name; the log keeps it.
static void report_as(const char *toast, const char *fmt, ...)
{
    snprintf(problem, sizeof problem, "%s", toast);
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "shelf: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

const char *game_error(void) { return problem; }

/* ---------- files ---------- */

// The core (SHELF_CORE for tests and the simulator), the ROM, and the game's folder in
// Saves, where the player keeps <stem>.srm/.rtc and Shelf keeps <stem>.state/.png.
static void session_paths(Session *s)
{
    const Game *g = &s->lib->games[s->game];
    const System *sys = &s->lib->sys[g->sys];
    const char *core_file = getenv("SHELF_CORE");
    const char *core_dir = getenv("SHELF_CORE_DIR");
    if (core_file) {
        snprintf(s->core, sizeof s->core, "%s", core_file);
    } else if (core_dir) {
        snprintf(s->core, sizeof s->core, "%s/%s_libretro.so", core_dir, sys->core);
    } else {
        char dev[PATH_LEN];
        snprintf(dev, sizeof dev, SD_PREFIX "/.shelf/cores/%s_libretro.so", sys->core);
        host_path(dev, s->core, sizeof s->core);
    }
    host_path(g->path, s->rom, sizeof s->rom);
    char dev[PATH_LEN];
    lib_save_dir(s->lib, s->game, dev, sizeof dev);
    host_path(dev, s->save_dir, sizeof s->save_dir);
    snprintf(s->stem, sizeof s->stem, "%s", g->stem);
    snprintf(s->state, sizeof s->state, "%s/%s.state", s->save_dir, s->stem);
    snprintf(s->shot, sizeof s->shot, "%s/%s.png", s->save_dir, s->stem);
}

// The player creates the save folder itself but not its parents ("Saves"): a card that
// has never held a save needs them made first.
static void make_dirs(char *path)
{
    for (char *p = path + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            if (mkdir(path, 0755) && errno != EEXIST) break;
            *p = '/';
        }
    if (mkdir(path, 0755) && errno != EEXIST) {} // reported by the first save
}

// The last frame at native size: the card the in-game menu shows for this game.
static int save_shot(const Session *s)
{
    const PlayerFrame *f = player_frame();
    if (!f || !f->px || f->w <= 0 || f->h <= 0) return 0; // nothing drawn: keep any old one
    size_t bytes = (size_t)f->w * f->h * 3;
    uint8_t *rgb = malloc(bytes);
    if (!rgb) return -1;
    for (int y = 0; y < f->h; y++)
        for (int x = 0; x < f->w; x++) {
            uint32_t p = f->px[(size_t)y * f->pitch + x];
            uint8_t *o = rgb + ((size_t)y * f->w + x) * 3;
            o[0] = (uint8_t)(p >> 16);
            o[1] = (uint8_t)(p >> 8);
            o[2] = (uint8_t)p;
        }
    int ok = stbi_write_png(s->shot, f->w, f->h, 3, rgb, f->w * 3);
    free(rgb);
    return ok ? 0 : -1;
}

// Saves everything the next session needs. Failures are reported and leave the previous
// file in place: the player writes through a temp file, and a screenshot is only opened
// for writing once the new one can be written.
static void save_session(Session *s)
{
    // Each is attempted whatever happened to the others; the cartridge save first, as
    // it's the one a player can't get back. The first failure is the one reported.
    problem[0] = 0;
    if (player_save_sram()) report_as("Couldn't save the game", "couldn't save %s's saved game", s->stem);
    if (player_save_state(s->state) && !problem[0]) report_as("Couldn't save the game state", "couldn't save %s's state", s->stem);
    if (save_shot(s) && !problem[0]) report_as("Couldn't save the screenshot", "couldn't save %s's screenshot", s->stem);
}

/* ---------- input ---------- */

static unsigned player_input(void)
{
    static const struct { Button button; unsigned bit; } MAP[] = {
        {BTN_A, PLAYER_A},           {BTN_B, PLAYER_B},       {BTN_X, PLAYER_X},
        {BTN_Y, PLAYER_Y},           {BTN_L, PLAYER_L},       {BTN_R, PLAYER_R},
        {BTN_L2, PLAYER_L2},         {BTN_R2, PLAYER_R2},     {BTN_START, PLAYER_START},
        {BTN_SELECT, PLAYER_SELECT}, {BTN_UP, PLAYER_UP},     {BTN_DOWN, PLAYER_DOWN},
        {BTN_LEFT, PLAYER_LEFT},     {BTN_RIGHT, PLAYER_RIGHT},
    };
    unsigned held = platform_held(), mask = 0;
    for (int i = 0; i < (int)(sizeof MAP / sizeof *MAP); i++)
        if (held & (1u << MAP[i].button)) mask |= MAP[i].bit;
    return mask;
}

/* ---------- standby ---------- */

// Blanks the panel, saves and sleeps until POWER wakes it. Returns 0 when it woke up and 1
// when Shelf should power off (POWER held, or the sleep limit). The panel and sound go
// first so the press feels instant; the save is written before anything can power off.
// The caller resumes audio: the menu stays paused across a standby.
static int standby(Session *s)
{
    platform_audio_source(NULL);
    player_audio_flush();
    platform_standby(1);
    save_session(s);
    PowerState asleep;
    power_init(&asleep);
    double since = platform_now();
    int off = 0;
    for (;;) {
        PowerDecision d = power_step(&asleep, platform_now(), since, POWER_ASLEEP, (PowerBattery){0});
        if (d.action == POWER_OFF_RESUMABLE) { off = 1; break; }
        platform_wait(d.next); // no polling: the next input, or the limit, ends the sleep
        Button b;
        while ((b = platform_poll()) != BTN_NONE) {
            if (b == BTN_POWER) goto woke;
            if (b == BTN_POWER_OFF || b == BTN_QUIT) { off = 1; goto woke; }
        }
    }
woke:
    if (!off) platform_standby(0); // powering off: the panel stays dark to the end
    return off;
}

// Reads the battery when the policy wants it and takes one decision, carrying out the
// ones that need no more than the platform: dimming and the low-battery message. The
// caller handles standby and powering off.
static PowerAction session_power(Session *s, PowerContext ctx, double *next)
{
    double now = platform_now();
    if (power_battery_due(&s->power, now)) {
        int charging = 0, percent = platform_battery(&charging);
        s->battery = (PowerBattery){percent, charging, percent >= 0};
        ui_set_status(percent, charging); // the menu's status bar; ignored while playing
    }
    PowerDecision d = power_step(&s->power, now, s->last_input, ctx, s->battery);
    if (next) *next = d.next;
    switch (d.action) {
    case POWER_DIM: platform_dim(1); break;
    case POWER_UNDIM: platform_dim(0); s->undimmed = 1; break;
    case POWER_LOW_BATTERY_WARNING: ui_set_toast("Battery low"); break;
    case POWER_OFF_RESUMABLE:
        if (s->battery.valid && s->battery.percent <= POWER_OFF_PERCENT)
            fprintf(stderr, "shelf: battery at %d%%; saving %s and powering off\n", s->battery.percent, s->stem);
        break;
    default: break;
    }
    return d.action;
}

/* ---------- the in-game menu's screenshots ---------- */

// The paused game's live frame plus each recent's <stem>.png, read a few at a time while
// the menu animates so a card that isn't there yet shows its box art instead.
typedef struct {
    Library *lib;
    int running;
    Image *live;      // the paused game on a whole screen (the zoom's full-size source)
    Image *live_card; // and at card size
    Image *card[MAX_RECENT];
    int tried[MAX_RECENT];   // a load was attempted; a missing screenshot isn't retried
    int pending[MAX_RECENT]; // asked for by the view, loaded between frames
    int card_w, card_h;
    unsigned rev;
} Cards;

static Cards *menu_cards; // the switcher's hooks carry no context of their own

static void build_live(Cards *c, ScaleMode mode)
{
    const PlayerFrame *f = player_frame();
    if (!f || !f->px || f->w <= 0 || f->h <= 0) return;
    if (!c->live) c->live = img_new(SCREEN_W, SCREEN_H);
    if (!c->live) return;
    ScaleRect r = scale_rect(f->w, f->h, f->aspect, SCREEN_W, SCREEN_H, mode);
    gfx_clear(c->live, 0xff000000);
    scale_blit(c->live->px, SCREEN_W, SCREEN_W, SCREEN_H, f->px, f->w, f->h, f->pitch, r, mode, 0);
    // scale_blit leaves display pixels (alpha 0); an Image is premultiplied ARGB.
    for (size_t i = 0; i < (size_t)SCREEN_W * SCREEN_H; i++) c->live->px[i] |= 0xff000000u;
    c->live_card = img_resize(c->live, c->card_w, c->card_h);
}

static const Image *menu_card(int i)
{
    Cards *c = menu_cards;
    if (!c || i < 0 || i >= c->lib->nrecent) return NULL;
    if (i == 0 && c->running) return c->live_card;
    if (c->card[i] || c->tried[i]) return c->card[i];
    c->pending[i] = 1;
    return NULL;
}

static const Image *menu_full(int i)
{
    Cards *c = menu_cards;
    return c && i == 0 && c->running ? c->live : NULL;
}

static unsigned menu_rev(void) { return menu_cards ? menu_cards->rev : 0; }
static void menu_select(int i) { (void)i; }

static int cards_pending(const Cards *c)
{
    for (int i = 0; i < c->lib->nrecent; i++)
        if (c->pending[i]) return 1;
    return 0;
}

// Reads one requested screenshot into its card, letterboxed like the game was on screen.
static void cards_pump(Cards *c)
{
    for (int i = 0; i < c->lib->nrecent; i++) {
        if (!c->pending[i]) continue;
        c->pending[i] = 0;
        c->tried[i] = 1;
        int gi = c->lib->recent[i];
        char dev[PATH_LEN], dir[PATH_LEN], path[PATH_LEN * 2];
        lib_save_dir(c->lib, gi, dev, sizeof dev);
        host_path(dev, dir, sizeof dir);
        snprintf(path, sizeof path, "%s/%s.png", dir, c->lib->games[gi].stem);
        Image *shot = img_load(path);
        if (!shot) continue;
        Image *card = img_new(c->card_w, c->card_h);
        if (card) {
            ScaleRect r = scale_rect(shot->w, shot->h, 0, c->card_w, c->card_h, SCALE_SHARP);
            gfx_clear(card, 0xff000000);
            if (scale_blit(card->px, c->card_w, c->card_w, c->card_h, shot->px, shot->w, shot->h,
                           shot->w, r, SCALE_SHARP, 0) == 0)
                c->card[i] = card;
            else
                img_free(card);
        }
        img_free(shot);
        if (c->card[i]) c->rev++;
    }
}

static void cards_free(Cards *c)
{
    for (int i = 0; i < MAX_RECENT; i++) img_free(c->card[i]);
    img_free(c->live);
    img_free(c->live_card);
    memset(c, 0, sizeof *c);
}

/* ---------- colours ---------- */

static UiPalette menu_palettes[PALETTE_COUNT];

// The menu's colour row for this game: a Game Boy game's palette, or colour correction.
static void colours_row(const Session *s)
{
    for (int i = 0; i < PALETTE_COUNT; i++) {
        const CorePalette *p = core_palette(i);
        menu_palettes[i].name = p->name;
        memcpy(menu_palettes[i].shade, p->shade, sizeof p->shade);
    }
    ui_switch_colours(s->lcd == LCD_GB ? UI_COLOURS_PALETTE
                      : s->lcd == LCD_GBC || s->lcd == LCD_GBA ? UI_COLOURS_SWITCH : UI_COLOURS_NONE,
                      menu_palettes, PALETTE_COUNT);
}

// The colour row changed a setting: the core gets it now, the paused game's card shows it,
// and it's saved. Returns 1 when the game can't go on (see player_peek).
static int colours_apply(Session *s, Cards *c)
{
    Settings want = ui_settings();
    PlayerOption opts[CORE_CHOICES_MAX];
    int changed = 0;
    if (want.colours == s->look.colours && want.palette == s->look.palette) return 0;
    s->look.colours = want.colours;
    s->look.palette = want.palette;
    int n = core_choices(s->core_name, want.colours, want.palette, opts, CORE_CHOICES_MAX);
    for (int i = 0; i < n; i++)
        if (player_set_option(opts[i].key, opts[i].value) > 0) changed = 1;
    if (s->set->settings) {
        s->set->settings->colours = want.colours;
        s->set->settings->palette = want.palette;
        settings_save(s->set->settings);
    }
    // gambatte re-reads its options at the end of a frame, so the second frame shows them.
    if (!changed) return 0;
    int peek = player_peek(2);
    if (peek == PLAYER_PEEK_LOST) {
        // The game is a preview's frames ahead of where it was paused: saving it now could
        // keep a state that never was. It ends here, as it was last saved.
        s->lost = 1;
        report_as("Couldn't change the colours; the game wasn't saved",
                  "the core couldn't take %s's state back after a colour preview; not saving it", s->stem);
        return 1;
    }
    if (peek) return 0;
    Image *old = c->live_card;
    c->live_card = NULL;
    build_live(c, s->set->scale); // a new card image, so the switcher redraws its sprite
    img_free(old);
    c->rev++;
    return 0;
}

/* ---------- the in-game menu ---------- */

enum { MENU_RESUME = 1, MENU_RESTART, MENU_SWITCH, MENU_HOME, MENU_POWER_OFF }; // 0: still open

typedef struct {
    Session *s;
    Cards *cards;
    Image *canvas;
    int confirming; // SELECT was pressed: A restarts, B cancels
    int done;       // a MENU_* result, 0 while the menu is open
} Menu;

// The power policy while the menu is up; returns when it next wants to run.
static double menu_power(Menu *m)
{
    Session *s = m->s;
    for (int i = 0; i < 8 && !m->done; i++) {
        double next;
        switch (session_power(s, POWER_MENU, &next)) {
        case POWER_NONE:
            return next;
        case POWER_STANDBY: // as if POWER was pressed
            if (standby(s)) { m->done = MENU_POWER_OFF; break; }
            platform_audio_source(NULL);
            s->last_input = platform_now();
            ui_button(BTN_REDRAW);
            break;
        case POWER_OFF_RESUMABLE:
            m->done = MENU_POWER_OFF;
            break;
        default:
            break;
        }
    }
    return platform_now();
}

static void menu_button(void *ud, Button b)
{
    Menu *m = ud;
    Session *s = m->s;
    if (m->done) return;
    if (b != BTN_REDRAW) {
        s->last_input = platform_now();
        s->undimmed = 0;
        menu_power(m);
        if (m->done) return;
        // A dimmed screen: the press only wakes it, unless it was POWER or the volume.
        if (s->undimmed && b != BTN_POWER && b != BTN_POWER_OFF && b != BTN_VOLUME) return;
    }
    switch (b) {
    case BTN_POWER: // asleep and back: the game stays paused
        if (standby(s)) { m->done = MENU_POWER_OFF; return; }
        platform_audio_source(NULL);
        s->last_input = platform_now();
        ui_button(BTN_REDRAW);
        return;
    case BTN_POWER_OFF:
    case BTN_QUIT:
        m->done = MENU_POWER_OFF;
        return;
    case BTN_SELECT:
        if (ui_switch_colours_open()) return; // the colour row has the buttons
        m->confirming = 1;
        ui_switch_notice("Restart the game?");
        return;
    case BTN_A:
        if (m->confirming) {
            m->confirming = 0;
            ui_switch_notice(NULL);
            m->done = MENU_RESTART;
            return;
        }
        break;
    case BTN_B:
        if (m->confirming) {
            m->confirming = 0;
            ui_switch_notice(NULL);
            return;
        }
        break;
    default:
        break;
    }
    if (m->confirming) return; // the question owns the row until it is answered
    ui_button(b);
    if (ui_take_sound()) platform_click();
}

static double menu_tick(void *ud, double now)
{
    double next = menu_power(ud); // battery, dimming and sleeping
    icons_pump(2);
    icons_trim();
    // Art made here (no worker) needs the next pass. Otherwise nothing outside the screen
    // changes: the worker's art wakes the loop itself and ui_idle has the clock's minute.
    return icons_pending() && now < next ? now : next;
}

static void menu_update(void *ud, float dt)
{
    Menu *m = ud;
    cards_pump(m->cards);
    ui_update(dt);
}

static double menu_idle(void *ud)
{
    Menu *m = ud;
    if (cards_pending(m->cards)) return 0; // a screenshot is still being read
    return ui_idle();
}

static void menu_draw(void *ud, Image *canvas)
{
    (void)ud;
    ui_draw(canvas);
}

static int menu_after(void *ud)
{
    Menu *m = ud;
    if (m->done) return 1;
    switch (ui_take_action()) {
    case UI_SETTINGS:
        if (colours_apply(m->s, m->cards)) m->done = MENU_HOME;
        break;
    case UI_SW_RESUME: m->done = MENU_RESUME; break;
    case UI_SW_HOME: m->done = MENU_HOME; break;
    case UI_SW_PLAY: {
        int target = ui_switch_target();
        m->done = MENU_SWITCH;
        if (target >= 0 && target < m->s->lib->nrecent)
            m->s->switch_to = m->s->lib->recent[target];
        break;
    }
    default: break;
    }
    return m->done != 0;
}

static int run_menu(Session *s)
{
    Cards cards = {0};
    Image *canvas = img_new(SCREEN_W, SCREEN_H);
    cards.lib = s->lib;
    cards.running = 1;
    ui_switch_card_size(&cards.card_w, &cards.card_h);
    build_live(&cards, s->set->scale);
    menu_cards = &cards;
    ui_switch_hooks(menu_card, menu_full, menu_select, menu_rev);
    if (!canvas || ui_init_switcher(s->lib, s->set->font_dir, 1, 1)) {
        report("Couldn't open the in-game menu");
        menu_cards = NULL;
        cards_free(&cards);
        img_free(canvas);
        return MENU_RESUME;
    }

    ui_set_settings(&s->look); // the rest of the settings carry over: only colours change here
    colours_row(s);
    platform_performance(0); // a paused game needs no more than the home screen
    ui_set_status(s->battery.valid ? s->battery.percent : -1, s->battery.charging);
    s->last_input = platform_now(); // MENU was just pressed
    Menu menu = {s, &cards, canvas, 0, 0};
    LoopIO io = {platform_now, platform_poll, platform_wait, platform_present, platform_vsync_paced()};
    LoopScreen screen = {&menu, menu_button, menu_tick, menu_update, menu_idle, menu_draw, menu_after};
    loop_run(&io, &screen, canvas, NULL);

    ui_leave(); // the UI stays loaded for the next menu and for the home screen
    menu_cards = NULL;
    cards_free(&cards);
    img_free(canvas);
    return menu.done;
}

/* ---------- playing ---------- */

// Back from the menu or standby: sound on, and the time away isn't idle or play time.
static void play_resume(Session *s, double *last, double *next)
{
    player_audio_flush(); // stale sound out; reading starts once the ring is full again
    platform_audio_source(player_audio_read);
    *last = *next = s->last_input = platform_now();
}

// Runs frames until the player leaves the game with the menu or POWER, or the power
// policy ends it.
//
// Pacing: emulation runs on the monotonic clock, one frame every 1 / player_fps() seconds
// against an absolute schedule, whether or not the frame was presented and whether or not
// presenting waits for vblank. The game's speed is the core's (59.7275 fps for GB and
// GBA), not the panel's: pacing by vblank would tie it to a refresh rate Shelf has never
// measured on hardware and that differs between panel revisions. At 60.00 Hz that is
// 0.46 % fast, all the headroom the audio ring's rate control has (player.c), and any
// panel further off would overflow or starve it; worse, a frame the core dupes presents
// nothing, so nothing waited for vblank and the game raced. Instead the newest frame goes
// to the flipper, which pans at the next vblank: a 60 Hz panel shows one frame in ~220
// twice (every ~3.7 s), the price of correct speed and sound. Presenting can still wait
// for the previous pan, so a frame that started over a quarter frame late, or finished
// only after the next one was due, isn't presented (never two in a row): neither a panel
// slower than the game nor a slow frame holds the emulation back for long, and the
// sound each frame makes never waits on the panel.
static int play(Session *s)
{
    double last = platform_now(), next = last, flush_at = last + SRAM_SECONDS;
    unsigned shown = 0, held = 0;
    int redraw = 1, skipped = 0;
    s->last_input = last;
    for (;;) {
        Button b;
        while ((b = platform_poll()) != BTN_NONE) {
            if (b != BTN_REDRAW) s->last_input = platform_now();
            switch (b) {
            case BTN_MENU: {
                platform_audio_source(NULL);
                player_audio_flush();
                int result = run_menu(s);
                if (result == MENU_SWITCH) return GAME_SWITCH;
                if (result == MENU_HOME) return GAME_HOME;
                if (result == MENU_POWER_OFF) return GAME_POWER_OFF;
                if (result == MENU_RESTART) player_reset();
                platform_performance(1);
                play_resume(s, &last, &next);
                redraw = 1; // the menu's last frame is on screen
                break;
            }
            case BTN_POWER:
                if (standby(s)) return GAME_POWER_OFF;
                play_resume(s, &last, &next);
                redraw = 1;
                break;
            case BTN_POWER_OFF:
            case BTN_QUIT:
                return GAME_POWER_OFF;
            case BTN_REDRAW:
                redraw = 1;
                break;
            default:
                break;
            }
        }

        double begun = platform_now(), frame = 1.0 / player_fps();
        unsigned input = player_input();
        if (input != held) { // holding the same buttons for ten minutes is idle too
            held = input;
            s->last_input = platform_now();
        }
        player_set_input(input);
        player_run();
        int on_time = begun - next < frame / 4 && platform_now() < next + frame;
        next += frame; // when the next frame is due
        const PlayerFrame *f = player_frame();
        if (redraw || f->serial != shown) { // a core that dupes a frame keeps the last one
            if (on_time || skipped) {
                platform_present_game(f->px, f->w, f->h, f->pitch, f->aspect, s->set->scale);
                shown = f->serial;
                redraw = skipped = 0;
            } else {
                skipped = 1; // late: the next frame, newer anyway, is shown instead
            }
        }
        double now = platform_now();
        s->played += now - last;
        last = now;
        if (now >= flush_at) {
            if (player_save_sram()) report_as("Couldn't save the game", "couldn't save %s's saved game", s->stem);
            flush_at = now + SRAM_SECONDS;
        }
        for (PowerAction a; (a = session_power(s, POWER_GAMEPLAY, NULL)) != POWER_NONE;) {
            if (a == POWER_OFF_RESUMABLE) return GAME_POWER_OFF; // the battery is nearly flat
            if (a == POWER_STANDBY) { // idle: as if POWER was pressed
                platform_audio_source(NULL);
                if (standby(s)) return GAME_POWER_OFF;
                play_resume(s, &last, &next);
                redraw = 1;
            }
        }
        now = platform_now();
        if (next > now) platform_sleep_until(next);
        // Behind by a frame or two (a slow frame, a late wake): run on at once and catch up,
        // which refills the short audio ring. Further behind (a stall, a slow card write):
        // drop the lost time and carry on from now, rather than a visible burst of frames.
        else if (now - next > 2 * frame) next = now;
    }
}

int game_play(Library *lib, int game, const GameSettings *set, int *switch_to)
{
    problem[0] = 0;
    if (switch_to) *switch_to = -1;
    if (!lib || !set || !set->font_dir || game < 0 || game >= lib->ngames) {
        report("no such game to play");
        return GAME_FAILED;
    }

    Session s;
    memset(&s, 0, sizeof s);
    s.lib = lib;
    s.game = game;
    s.set = set;
    s.switch_to = -1;
    power_init(&s.power);
    session_paths(&s);
    make_dirs(s.save_dir);

    // The menu shows the running game as recents[0]; main records it before launching, so
    // this only repairs a caller that didn't.
    if (lib->nrecent < 1 || lib->recent[0] != game) lib_mark_played(lib, game);

    const char *core = lib->sys[lib->games[game].sys].core;
    s.core_name = core;
    s.lcd = core_lcd(core, s.rom);
    if (set->settings) s.look = *set->settings;
    else settings_defaults(&s.look);
    char dev[PATH_LEN], bios[PATH_LEN], options[PATH_LEN];
    host_path(SD_PREFIX "/BIOS", bios, sizeof bios);
    snprintf(dev, sizeof dev, SD_PREFIX "/.shelf/data/%s.opt", core);
    host_path(dev, options, sizeof options);

    int ndefaults;
    const PlayerOption *defaults = core_defaults(core, &ndefaults);
    PlayerOption choices[CORE_CHOICES_MAX];
    int nchoices = core_choices(core, s.look.colours, s.look.palette, choices, CORE_CHOICES_MAX);
    PlayerConfig cfg = {.core_path = s.core, .rom_path = s.rom, .system_dir = bios,
                        .save_dir = s.save_dir, .stem = s.stem, .options_path = options,
                        .defaults = defaults, .ndefaults = ndefaults,
                        .choices = choices, .nchoices = nchoices,
                        .sample_rate = PLATFORM_AUDIO_RATE, .buffer_ms = PLATFORM_AUDIO_RING_MS};
    char err[160];
    // Full speed from here to the save on the way out: reading the ROM, loading the core
    // and restoring the state are part of the wait for the game to start.
    platform_performance(1);
    if (player_open(&cfg, err, sizeof err)) {
        report("%s", err);
        platform_performance(0);
        return GAME_FAILED;
    }
    // No state yet is a first start; a broken one must never keep the game from running.
    if (access(s.state, F_OK) == 0 && player_load_state(s.state)) {
        fprintf(stderr, "shelf: couldn't restore %s; starting it fresh\n", s.state);
        player_reset(); // a half-applied state must not run
    }
    platform_audio_source(player_audio_read);

    int result = play(&s);

    platform_audio_source(NULL);
    // Powering off: dark and silent first, like standby, so the press feels instant and
    // a slow card doesn't leave the screen lit while the save is written.
    if (result == GAME_POWER_OFF) platform_standby(1);
    // The menu lowered the clock and standby lowered it further; the save is the last
    // wait before home (or the power going), so it runs at full speed again.
    platform_performance(1);
    if (!s.lost) save_session(&s); // lost: game_error() already says why
    // Powering off from a game: the next boot comes straight back to it. Not when the save
    // failed; then home, with the previous state still there to resume by hand.
    if (result == GAME_POWER_OFF && !problem[0] && lib_set_resume(lib, game))
        fprintf(stderr, "shelf: couldn't mark %s to resume\n", s.stem);
    player_close();
    platform_performance(0);
    if (lib_add_play(lib, game, (int)s.played, (long long)time(NULL)))
        fprintf(stderr, "shelf: couldn't record the play time\n");
    if (switch_to) *switch_to = s.switch_to;
    if (result == GAME_POWER_OFF) platform_power_off();
    return result;
}
