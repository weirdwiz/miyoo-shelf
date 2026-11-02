// Runs one libretro core and game in-process. See player.h.
//
// The libretro callbacks are process-global, so everything here is file-local static
// state for a single open player. player_audio_read is the only entry point that may
// run on another thread; it talks to the game thread through a lock-free ring.
#include "player.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../third_party/libretro.h"

#define PLAYER_PATH_LEN 512
#define FILE_MAX (64u << 20)  // ROM and save state cap
#define AUDIO_MS 32           // buffered output when the caller sets no buffer_ms
#define DRC_MAX 0.005         // resample ratio swing at full correction
#define DRC_KP 2.0            // full correction at half a target from it
#define DRC_SMOOTH 0.15       // seconds the fill is averaged over

typedef struct {
    void *dl;
    void (*init)(void);
    void (*deinit)(void);
    unsigned (*api_version)(void);
    void (*get_system_info)(struct retro_system_info *);
    void (*get_system_av_info)(struct retro_system_av_info *);
    void (*set_controller_port_device)(unsigned, unsigned);
    void (*reset)(void);
    void (*run)(void);
    size_t (*serialize_size)(void);
    bool (*serialize)(void *, size_t);
    bool (*unserialize)(const void *, size_t);
    bool (*load_game)(const struct retro_game_info *);
    void (*unload_game)(void);
    void *(*get_memory_data)(unsigned);
    size_t (*get_memory_size)(unsigned);
    void (*set_environment)(retro_environment_t);
    void (*set_video_refresh)(retro_video_refresh_t);
    void (*set_audio_sample)(retro_audio_sample_t);
    void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
    void (*set_input_poll)(retro_input_poll_t);
    void (*set_input_state)(retro_input_state_t);
} Core;

// Core options are collected from whichever SET_* call the core makes. Every string
// lives in the arena so a pointer handed out by GET_VARIABLE stays valid until close.
typedef struct {
    const char *key;
    const char *value;
    const char **allowed;
    int nallowed;
} Option;

// Where a configured value came from; a stronger source replaces a weaker one's value,
// whichever is read first. FROM_CHOICE: changed while the game runs.
enum { FROM_DEFAULT, FROM_FILE, FROM_CHOICE };

typedef struct {
    const char *key;
    const char *value;
    int from;
} RawOption;

typedef struct {
    unsigned id;
    const char *ext;
    size_t size;
    uint8_t *last;  // what the file (or the core, after loading) holds
    char path[PLAYER_PATH_LEN * 2 + 8];  // save_dir + stem + extension
} Mem;

static Core core;
static int open_flag, inited, game_loaded;

static char system_dir[PLAYER_PATH_LEN], save_dir[PLAYER_PATH_LEN], stem[PLAYER_PATH_LEN];
static char rom_path[PLAYER_PATH_LEN];
static uint8_t *rom;
static size_t rom_size;
static char **arena;
static int narena, arena_cap;
static Option *opts;
static int nopts, opts_cap;
static RawOption *raw;
static int nraw;
static int options_updated; // a change the core hasn't been told about yet
static int audio_muted;     // a peek is running: its sound is dropped
static int lost;            // a peek couldn't put the game back: it must not be saved

static uint32_t *fb;
static size_t fb_px;
static PlayerFrame frame;
static struct retro_game_geometry geo;
static struct retro_system_timing timing;
static enum retro_pixel_format pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
static unsigned input_mask;

static int16_t *ring;
static uint32_t ring_cap, ring_target;
static _Atomic uint32_t ring_head, ring_tail;
static _Atomic int primed; // the reader has had ring_target frames to start from
static int out_rate;
static double core_rate;
static double drc_err, drc_sum, step_cur;
static unsigned step_since;
static double phase;
static int16_t prev_l, prev_r;
static int have_prev;

static Mem mems[2] = {
    { RETRO_MEMORY_SAVE_RAM, "srm" },
    { RETRO_MEMORY_RTC, "rtc" },
};

static void reset_state(void);
static void teardown(void);

static void fail(char *err, int errlen, const char *fmt, ...)
{
    if (!err || errlen <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    err[errlen - 1] = 0;
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

static char *arena_keep(char *s)
{
    if (!s) return NULL;
    if (narena == arena_cap) {
        int cap = arena_cap ? arena_cap * 2 : 16;
        char **p = realloc(arena, (size_t)cap * sizeof *p);
        if (!p) { free(s); return NULL; }
        arena = p;
        arena_cap = cap;
    }
    arena[narena++] = s;
    return s;
}

static char *arena_str(const char *s) { return arena_keep(dup_str(s)); }

static int copy_str(char *dst, const char *src)
{
    int n;
    if (!src) return -1;
    n = snprintf(dst, PLAYER_PATH_LEN, "%s", src);
    return n < 0 || n >= PLAYER_PATH_LEN ? -1 : 0;
}

// --- core options -------------------------------------------------------------

static RawOption *raw_find(const char *key)
{
    for (int i = 0; i < nraw; i++)
        if (!strcmp(raw[i].key, key)) return &raw[i];
    return NULL;
}

static const char *raw_get(const char *key)
{
    RawOption *r = raw_find(key);
    return r ? r->value : NULL;
}

static char *trim(char *s)
{
    size_t n;
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n'))
        s[--n] = 0;
    return s;
}

// Records `value` for `key` unless a stronger source already set it.
static int raw_set(const char *key, const char *value, int from)
{
    RawOption *r = raw_find(key);
    if (r) {
        const char *v;
        if (r->from > from) return 0;
        if (!(v = arena_str(value))) return -1;
        r->value = v;
        r->from = from;
        return 0;
    }
    RawOption *p = realloc(raw, (size_t)(nraw + 1) * sizeof *p);
    if (!p) return -1;
    raw = p;
    raw[nraw].key = arena_str(key);
    raw[nraw].value = arena_str(value);
    raw[nraw].from = from;
    if (!raw[nraw].key || !raw[nraw].value) return -1;
    nraw++;
    return 0;
}

// Shelf's own values for some options; malformed entries are skipped.
static int raw_set_list(const PlayerOption *list, int n, int from)
{
    for (int i = 0; list && i < n; i++)
        if (list[i].key && list[i].key[0] && list[i].value && list[i].value[0] &&
            raw_set(list[i].key, list[i].value, from))
            return -1;
    return 0;
}

// RetroArch .opt file: `key = "value"` per line. Unknown keys and malformed lines are
// ignored, so a hand-edited file can never stop a game from starting.
static void options_load(const char *path)
{
    FILE *f;
    char line[PLAYER_PATH_LEN];

    if (!path) return;
    f = fopen(path, "r");
    if (!f) return;
    while (fgets(line, sizeof line, f)) {
        char *s = trim(line), *eq, *key, *value;
        size_t n;
        if (!s[0] || s[0] == '#' || !(eq = strchr(s, '='))) continue;
        *eq = 0;
        key = trim(s);
        value = trim(eq + 1);
        n = strlen(value);
        if (n >= 2 && value[0] == '"' && value[n - 1] == '"') {
            value[n - 1] = 0;
            value++;
        }
        if (key[0] && value[0]) raw_set(key, value, FROM_FILE);
    }
    fclose(f);
}

static Option *opt_slot(const char *key)
{
    for (int i = 0; i < nopts; i++)
        if (!strcmp(opts[i].key, key)) return &opts[i];
    if (nopts == opts_cap) {
        int cap = opts_cap ? opts_cap * 2 : 8;
        Option *p = realloc(opts, (size_t)cap * sizeof *p);
        if (!p) return NULL;
        opts = p;
        opts_cap = cap;
    }
    memset(&opts[nopts], 0, sizeof opts[nopts]);
    opts[nopts].key = arena_str(key);
    if (!opts[nopts].key) return NULL;
    return &opts[nopts++];
}

static int opt_allows(const Option *o, const char *value)
{
    if (o->nallowed == 0) return 1;  // free-form option
    for (int i = 0; i < o->nallowed; i++)
        if (!strcmp(o->allowed[i], value)) return 1;
    return 0;
}

static int opt_set(const char *key, const char *def, const char *const *values, int nvalues)
{
    Option *o;
    const char *chosen = NULL;
    const char *want;

    if (!key || !key[0]) return 0;
    o = opt_slot(key);
    if (!o) return -1;

    free(o->allowed);
    o->allowed = NULL;
    o->nallowed = 0;
    if (nvalues > 0) {
        const char **a = calloc((size_t)nvalues, sizeof *a);
        if (!a) return -1;
        for (int i = 0; i < nvalues; i++) a[i] = arena_str(values[i]);
        o->allowed = a;
        o->nallowed = nvalues;
    }

    // The configured value (Shelf's or the options file's) wins when the core allows it,
    // otherwise the core's default.
    want = raw_get(o->key);
    if (want && opt_allows(o, want)) chosen = want;
    if (!chosen && def && opt_allows(o, def)) chosen = def;
    if (!chosen && o->nallowed) chosen = o->allowed[0];
    o->value = chosen ? arena_str(chosen) : NULL;
    return 0;
}

// A deprecated SET_VARIABLES entry is "Label; value|value|...".
static int opt_set_legacy(const char *key, const char *desc)
{
    const char *values[RETRO_NUM_CORE_OPTION_VALUES_MAX];
    char buf[1024];
    int nvalues = 0;
    char *semi, *tok, *save;

    if (!key || !desc) return 0;
    snprintf(buf, sizeof buf, "%s", desc);
    semi = strchr(buf, ';');
    if (!semi) return opt_set(key, NULL, NULL, 0);
    for (tok = strtok_r(semi + 1, "|", &save); tok && nvalues < RETRO_NUM_CORE_OPTION_VALUES_MAX;
         tok = strtok_r(NULL, "|", &save)) {
        tok = trim(tok);
        if (tok[0]) values[nvalues++] = tok;
    }
    return opt_set(key, nvalues ? values[0] : NULL, values, nvalues);
}

static int set_variables(const struct retro_variable *vars)
{
    if (!vars) return 0;
    for (; vars->key; vars++)
        if (opt_set_legacy(vars->key, vars->value)) return -1;
    return 0;
}

static int opt_set_defined(const char *key, const char *def,
                           const struct retro_core_option_value *values)
{
    const char *list[RETRO_NUM_CORE_OPTION_VALUES_MAX];
    int nvalues = 0;

    for (int i = 0; i < RETRO_NUM_CORE_OPTION_VALUES_MAX && values[i].value; i++)
        list[nvalues++] = values[i].value;
    return opt_set(key, def, list, nvalues);
}

static int set_core_options(const struct retro_core_option_definition *defs)
{
    if (!defs) return 0;
    for (; defs->key; defs++)
        if (opt_set_defined(defs->key, defs->default_value, defs->values)) return -1;
    return 0;
}

static int set_core_options_v2(const struct retro_core_options_v2 *v2)
{
    const struct retro_core_option_v2_definition *defs;

    if (!v2 || !(defs = v2->definitions)) return 0;
    for (; defs->key; defs++)
        if (opt_set_defined(defs->key, defs->default_value, defs->values)) return -1;
    return 0;
}

// --- audio --------------------------------------------------------------------

// Linear resampling into a single-producer/single-consumer ring. The producer adjusts
// its ratio by up to DRC_MAX to keep the ring around ring_target frames, so a consumer
// with its own clock never starves the ring or overflows it over minutes of play, and
// sound is no later than it needs to be. The ring holds three targets; it fills past one
// only when the consumer stalls, and then drops the oldest sound.
static void audio_push(int16_t l, int16_t r)
{
    uint32_t mask = ring_cap - 1;
    uint32_t h = atomic_load_explicit(&ring_head, memory_order_relaxed);
    uint32_t t = atomic_load_explicit(&ring_tail, memory_order_acquire);

    if (h - t >= ring_cap)  // full: drop the oldest rather than stall the game thread
        atomic_fetch_add_explicit(&ring_tail, h - t - ring_cap + 1, memory_order_release);
    ring[2 * (h & mask)] = l;
    ring[2 * (h & mask) + 1] = r;
    atomic_store_explicit(&ring_head, h + 1, memory_order_release);
}

// A PI controller on the fill, as a fraction of the target away from it. The integral
// learns the steady clock difference between the producer and the consumer, so the fill
// settles on the target whatever that difference is (up to DRC_MAX): with a proportional
// term alone it would settle short of the target by an amount that grows with the skew,
// which a ring of a few frames can't spare. Its gain is set for critical damping, from
// how fast full correction moves the fill (out_rate * DRC_MAX against the target).
static double drc_step(double dt)
{
    double fill, err, u, step, k;

    if (!ring_cap) return 1.0;
    fill = (double)(atomic_load_explicit(&ring_head, memory_order_relaxed) -
                    atomic_load_explicit(&ring_tail, memory_order_relaxed));
    err = (fill - (double)ring_target) / (double)ring_target;
    if (err > 1.0) err = 1.0;
    if (err < -1.0) err = -1.0;
    // The fill saws up with each frame's burst of sound and down with each read: average
    // that out first (over ~0.15 s), so the controller doesn't clip on it.
    drc_err += (err - drc_err) * (dt / (dt + DRC_SMOOTH));
    k = (double)out_rate * DRC_MAX / (double)ring_target; // per second
    drc_sum += drc_err * k * DRC_KP * DRC_KP / 4.0 * dt;
    if (drc_sum > 1.0) drc_sum = 1.0;
    if (drc_sum < -1.0) drc_sum = -1.0;
    u = DRC_KP * drc_err + drc_sum;
    if (u > 1.0) u = 1.0;
    if (u < -1.0) u = -1.0;
    step = core_rate / (double)out_rate * (1.0 + DRC_MAX * u);
    if (!(step > 0.01) || step > 100.0) step = 1.0;  // never spin on a broken rate
    return step;
}

// The ratio only needs to follow a slow clock difference, so update it every few
// hundred input frames whatever call rate the core uses (batch or one sample at a time).
static double resample_step(size_t frames)
{
    step_since += (unsigned)frames;
    if (step_since < 256) return step_cur;
    step_cur = drc_step(core_rate > 0 ? step_since / core_rate : 0);
    step_since = 0;
    return step_cur;
}

static void audio_batch(const int16_t *data, size_t frames)
{
    double step;
    if (!ring || !data || audio_muted) return;
    step = resample_step(frames);
    for (size_t i = 0; i < frames; i++) {
        int16_t l = data[2 * i], r = data[2 * i + 1];
        if (!have_prev) {
            prev_l = l;
            prev_r = r;
            have_prev = 1;
            continue;
        }
        while (phase < 1.0) {
            double t = phase;
            audio_push((int16_t)(prev_l + (l - prev_l) * t), (int16_t)(prev_r + (r - prev_r) * t));
            phase += step;
        }
        phase -= 1.0;
        prev_l = l;
        prev_r = r;
    }
}

static void RETRO_CALLCONV audio_sample_cb(int16_t left, int16_t right)
{
    int16_t frame[2] = { left, right };
    audio_batch(frame, 1);
}

static size_t RETRO_CALLCONV audio_batch_cb(const int16_t *data, size_t frames)
{
    audio_batch(data, frames);
    return frames;
}

static int audio_open(int rate, double core_hz, int ms)
{
    unsigned cap = 1024;

    if (rate < 8000 || rate > 192000) rate = 48000;
    if (ms <= 0 || ms > 1000) ms = AUDIO_MS;
    out_rate = rate;
    core_rate = core_hz > 0 ? core_hz : (double)rate;
    ring_target = (unsigned)rate * (unsigned)ms / 1000;
    if (ring_target < 1) ring_target = 1;
    while (cap < 3 * ring_target) cap <<= 1;
    ring = malloc((size_t)cap * 2 * sizeof *ring);
    if (!ring) return -1;
    ring_cap = cap;
    atomic_store(&ring_head, 0);
    atomic_store(&ring_tail, 0);
    atomic_store(&primed, 0);
    step_cur = 1.0;
    drc_err = drc_sum = 0.0;
    step_since = 0;
    phase = 0.0;
    prev_l = prev_r = 0;
    have_prev = 0;
    return 0;
}

// --- video --------------------------------------------------------------------

static void video_cb(const void *data, unsigned w, unsigned h, size_t pitch)
{
    const uint8_t *src = data;
    size_t need;

    if (!data || !w || !h) return;  // dupe: keep the frame we already have
    need = (size_t)w * h;
    if (need > fb_px) {
        uint32_t *p = realloc(fb, need * sizeof *p);
        if (!p) return;
        fb = p;
        fb_px = need;
        frame.px = fb;
    }
    for (unsigned y = 0; y < h; y++) {
        const uint8_t *row = src + (size_t)y * pitch;
        uint32_t *dst = fb + (size_t)y * w;
        if (pixel_format == RETRO_PIXEL_FORMAT_XRGB8888) {
            for (unsigned x = 0; x < w; x++) {
                const uint8_t *p = row + 4 * (size_t)x;
                dst[x] = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
            }
        } else if (pixel_format == RETRO_PIXEL_FORMAT_RGB565) {
            for (unsigned x = 0; x < w; x++) {
                const uint8_t *p = row + 2 * (size_t)x;
                uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8;
                uint32_t r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, b = v & 0x1f;
                dst[x] = ((r << 3) | (r >> 2)) << 16 | ((g << 2) | (g >> 4)) << 8 | ((b << 3) | (b >> 2));
            }
        } else {
            for (unsigned x = 0; x < w; x++) {
                const uint8_t *p = row + 2 * (size_t)x;
                uint32_t v = (uint32_t)p[0] | (uint32_t)p[1] << 8;
                uint32_t r = (v >> 10) & 0x1f, g = (v >> 5) & 0x1f, b = v & 0x1f;
                dst[x] = ((r << 3) | (r >> 2)) << 16 | ((g << 3) | (g >> 2)) << 8 | ((b << 3) | (b >> 2));
            }
        }
    }
    frame.px = fb;
    frame.w = (int)w;
    frame.h = (int)h;
    frame.pitch = (int)w;
    frame.aspect = geo.aspect_ratio > 0 ? geo.aspect_ratio : (float)w / (float)h;
    frame.serial++;
}

static void input_poll_cb(void) {}
static int16_t input_state_cb(unsigned port, unsigned device, unsigned index, unsigned id)
{
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return (int16_t)(input_mask & 0xffffu);
    if (id < 16) return (int16_t)((input_mask >> id) & 1u);
    return 0;
}

// --- environment --------------------------------------------------------------

static void RETRO_CALLCONV log_cb(enum retro_log_level level, const char *fmt, ...)
{
    va_list ap;
    if (level == RETRO_LOG_DEBUG || !fmt) return;
    fputs("core: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static bool rumble_cb(unsigned port, enum retro_rumble_effect effect, uint16_t strength)
{
    return false;
}

static void grow_video(void)
{
    size_t need = (size_t)(geo.max_width ? geo.max_width : geo.base_width) *
                  (size_t)(geo.max_height ? geo.max_height : geo.base_height);
    if (!need || need <= fb_px) return;
    if (need > (size_t)1 << 26) return;  // 64 Mpx is already absurd for a handheld
    uint32_t *p = realloc(fb, need * sizeof *p);
    if (!p) return;
    fb = p;
    fb_px = need;
    frame.px = fb;
}

static void set_av_info(const struct retro_system_av_info *av)
{
    geo = av->geometry;
    timing = av->timing;
    if (timing.sample_rate > 0) core_rate = timing.sample_rate;
    if (!geo.max_width) geo.max_width = geo.base_width ? geo.base_width : 640;
    if (!geo.max_height) geo.max_height = geo.base_height ? geo.base_height : 480;
    grow_video();
}

static float default_aspect(void)
{
    if (geo.aspect_ratio > 0) return geo.aspect_ratio;
    if (!geo.base_width || !geo.base_height) return 1.0f;
    return (float)geo.base_width / (float)geo.base_height;
}

static bool env_cb(unsigned command, void *data)
{
    unsigned cmd = command & 0xffffu;  // drop the EXPERIMENTAL / PRIVATE flag bits

    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        if (!system_dir[0] || !data) return false;
        *(const char **)data = system_dir;
        return true;
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        if (!save_dir[0] || !data) return false;
        *(const char **)data = save_dir;
        return true;
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        enum retro_pixel_format fmt = data ? *(const enum retro_pixel_format *)data
                                           : RETRO_PIXEL_FORMAT_UNKNOWN;
        if (fmt != RETRO_PIXEL_FORMAT_0RGB1555 && fmt != RETRO_PIXEL_FORMAT_RGB565 &&
            fmt != RETRO_PIXEL_FORMAT_XRGB8888)
            return false;
        pixel_format = fmt;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        struct retro_variable *var = data;
        const char *value = NULL;
        if (!var || !var->key) return false;
        for (int i = 0; i < nopts; i++)
            if (!strcmp(opts[i].key, var->key)) value = opts[i].value;
        var->value = value;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
        if (data) *(bool *)data = options_updated != 0;
        options_updated = 0; // told once: the core re-reads every option it cares about
        return true;
    case RETRO_ENVIRONMENT_SET_VARIABLES:
        return set_variables(data) == 0;
    case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
        if (data) *(unsigned *)data = 2;
        return true;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
        return set_core_options(data) == 0;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL: {
        const struct retro_core_options_intl *intl = data;
        return intl && set_core_options(intl->us) == 0;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
        return set_core_options_v2(data) == 0;
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL: {
        const struct retro_core_options_v2_intl *intl = data;
        return intl && set_core_options_v2(intl->us) == 0;
    }
    case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        return true;
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
        if (!data) return false;
        ((struct retro_log_callback *)data)->log = log_cb;
        return true;
    case RETRO_ENVIRONMENT_GET_CAN_DUPE:
        if (data) *(bool *)data = true;
        return true;
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
        return true;
    case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
    case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
    case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
    case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
    case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
    case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
    case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
    case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        return true;
    case RETRO_ENVIRONMENT_SET_GEOMETRY:
        if (!data) return false;
        geo = *(const struct retro_game_geometry *)data;
        grow_video();
        return true;
    case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        if (!data) return false;
        set_av_info(data);
        return true;
    case RETRO_ENVIRONMENT_GET_LANGUAGE:
        if (data) *(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
        return true;
    case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
        if (data) *(int *)data = RETRO_AV_ENABLE_VIDEO | RETRO_AV_ENABLE_AUDIO;
        return true;
    case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
        if (data) *(bool *)data = false;
        return true;
    case RETRO_ENVIRONMENT_SET_MESSAGE: {
        const struct retro_message *msg = data;
        if (msg && msg->msg) fprintf(stderr, "core: %s\n", msg->msg);
        return true;
    }
    case RETRO_ENVIRONMENT_SET_MESSAGE_EXT: {
        const struct retro_message_ext *msg = data;
        if (msg && msg->msg) fprintf(stderr, "core: %s\n", msg->msg);
        return true;
    }
    case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
        if (data) ((struct retro_rumble_interface *)data)->set_rumble_state = rumble_cb;
        return false;  // no motors here
    default:
        return false;
    }
}

// --- files --------------------------------------------------------------------

enum { READ_OK, READ_MISSING, READ_TOO_BIG, READ_IO };

static int read_all(const char *path, size_t max, uint8_t **out, size_t *len)
{
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *buf;

    if (!f) return READ_MISSING;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return READ_IO; }
    n = ftell(f);
    if (n < 0) { fclose(f); return READ_IO; }
    if ((unsigned long)n > max) { fclose(f); return READ_TOO_BIG; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return READ_IO; }
    buf = malloc(n ? (size_t)n : 1);
    if (!buf) { fclose(f); return READ_IO; }
    if (n && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return READ_IO;
    }
    fclose(f);
    *out = buf;
    *len = (size_t)n;
    return READ_OK;
}

static void sync_dir(const char *path)
{
    char dir[PLAYER_PATH_LEN];
    char *slash;
    int fd;

    snprintf(dir, sizeof dir, "%s", path);
    slash = strrchr(dir, '/');
    if (!slash) snprintf(dir, sizeof dir, ".");
    else if (slash == dir) dir[1] = 0;
    else *slash = 0;
    fd = open(dir, O_RDONLY);
    if (fd < 0) return;
    fsync(fd);  // the rename has landed; a directory fsync failure is not fatal
    close(fd);
}

// Temp file, fsync, rename. On any error the old file is left untouched.
static int write_atomic(const char *path, const void *data, size_t n)
{
    char tmp[PLAYER_PATH_LEN + 8];
    FILE *f;
    int ok;

    if (snprintf(tmp, sizeof tmp, "%s.tmp", path) >= (int)sizeof tmp) return -1;
    f = fopen(tmp, "wb");
    if (!f) return -1;
    ok = fwrite(data, 1, n, f) == n;
    if (ok && fflush(f) != 0) ok = 0;
    if (ok && fsync(fileno(f)) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) {
        remove(tmp);
        return -1;
    }
    sync_dir(path);
    return 0;
}

// --- SRAM and RTC -------------------------------------------------------------

static void mem_open(Mem *m)
{
    size_t size = core.get_memory_size(m->id);
    void *data = core.get_memory_data(m->id);
    FILE *f;

    if (!size || !data) return;
    m->size = size;
    m->last = malloc(size);
    if (!m->last) return;
    snprintf(m->path, sizeof m->path, "%s/%s.%s", save_dir, stem, m->ext);
    f = fopen(m->path, "rb");
    if (f) {
        fread(data, 1, size, f);  // min(file, size): a short save file is fine
        fclose(f);
    }
    memcpy(m->last, data, size);
}

static int mem_save(Mem *m)
{
    void *data;

    if (!m->size || !m->last) return 0;
    data = core.get_memory_data(m->id);
    if (!data) return 0;
    if (!memcmp(m->last, data, m->size)) return 0;
    if (write_atomic(m->path, data, m->size) != 0) return -1;
    memcpy(m->last, data, m->size);
    return 0;
}

// --- lifecycle ----------------------------------------------------------------

static void reset_state(void)
{
    memset(&core, 0, sizeof core);
    memset(&geo, 0, sizeof geo);
    memset(&timing, 0, sizeof timing);
    memset(&frame, 0, sizeof frame);
    memset(mems, 0, sizeof mems);
    mems[0].id = RETRO_MEMORY_SAVE_RAM;
    mems[0].ext = "srm";
    mems[1].id = RETRO_MEMORY_RTC;
    mems[1].ext = "rtc";
    open_flag = inited = game_loaded = 0;
    system_dir[0] = save_dir[0] = stem[0] = rom_path[0] = 0;
    rom = NULL;
    rom_size = 0;
    fb = NULL;
    fb_px = 0;
    pixel_format = RETRO_PIXEL_FORMAT_0RGB1555;
    input_mask = 0;
    ring = NULL;
    ring_cap = ring_target = 0;
    out_rate = 0;
    core_rate = 0.0;
    step_cur = 1.0;
    drc_err = drc_sum = 0.0;
    step_since = 0;
    phase = 0.0;
    have_prev = 0;
    opts = NULL;
    nopts = opts_cap = 0;
    raw = NULL;
    nraw = 0;
    options_updated = 0;
    audio_muted = lost = 0;
    arena = NULL;
    narena = arena_cap = 0;
}

static void teardown(void)
{
    if (game_loaded) core.unload_game();
    if (inited) core.deinit();
    game_loaded = inited = 0;
    if (core.dl) dlclose(core.dl);

    free(fb);
    free(rom);
    free(ring);
    for (int i = 0; i < 2; i++) free(mems[i].last);
    for (int i = 0; i < nopts; i++) free(opts[i].allowed);
    free(opts);
    free(raw);
    for (int i = 0; i < narena; i++) free(arena[i]);
    free(arena);
    reset_state();
}

#define LOAD(name)                                                                    \
    do {                                                                              \
        *(void **)(&core.name) = dlsym(core.dl, "retro_" #name);                      \
        if (!core.name) {                                                             \
            fail(err, errlen, "%s: no retro_" #name, cfg->core_path);                 \
            goto fail;                                                                \
        }                                                                             \
    } while (0)

int player_open(const PlayerConfig *cfg, char *err, int errlen)
{
    struct retro_system_info sysinfo;
    struct retro_game_info info;
    struct retro_system_av_info av;
    int rc;

    if (err && errlen > 0) err[0] = 0;
    if (open_flag) {
        fail(err, errlen, "a player is already open");
        return -1;
    }
    if (!cfg || !cfg->core_path || !cfg->rom_path || !cfg->save_dir || !cfg->stem) {
        fail(err, errlen, "incomplete player configuration");
        return -1;
    }
    reset_state();
    if (copy_str(system_dir, cfg->system_dir ? cfg->system_dir : "") ||
        copy_str(save_dir, cfg->save_dir) || copy_str(stem, cfg->stem) ||
        copy_str(rom_path, cfg->rom_path)) {
        fail(err, errlen, "player path too long");
        goto fail;
    }
    options_load(cfg->options_path);
    if (raw_set_list(cfg->defaults, cfg->ndefaults, FROM_DEFAULT) ||
        raw_set_list(cfg->choices, cfg->nchoices, FROM_CHOICE)) {
        fail(err, errlen, "out of memory for the core options");
        goto fail;
    }
    if (mkdir(save_dir, 0755) != 0 && errno != EEXIST) {
        // Not fatal: the game still runs, its saves will report write errors.
    }

    core.dl = dlopen(cfg->core_path, RTLD_NOW | RTLD_LOCAL);
    if (!core.dl) {
        fail(err, errlen, "cannot load %s: %s", cfg->core_path, dlerror());
        goto fail;
    }
    LOAD(init);
    LOAD(deinit);
    LOAD(api_version);
    LOAD(get_system_info);
    LOAD(get_system_av_info);
    LOAD(set_controller_port_device);
    LOAD(reset);
    LOAD(run);
    LOAD(serialize_size);
    LOAD(serialize);
    LOAD(unserialize);
    LOAD(load_game);
    LOAD(unload_game);
    LOAD(get_memory_data);
    LOAD(get_memory_size);
    LOAD(set_environment);
    LOAD(set_video_refresh);
    LOAD(set_audio_sample);
    LOAD(set_audio_sample_batch);
    LOAD(set_input_poll);
    LOAD(set_input_state);

    if (core.api_version() != RETRO_API_VERSION) {
        fail(err, errlen, "%s: unsupported libretro API version %u", cfg->core_path,
             core.api_version());
        goto fail;
    }

    core.set_environment(env_cb);
    core.set_video_refresh(video_cb);
    core.set_audio_sample(audio_sample_cb);
    core.set_audio_sample_batch(audio_batch_cb);
    core.set_input_poll(input_poll_cb);
    core.set_input_state(input_state_cb);
    core.init();
    inited = 1;

    memset(&sysinfo, 0, sizeof sysinfo);
    core.get_system_info(&sysinfo);

    memset(&info, 0, sizeof info);
    info.path = rom_path;
    if (!sysinfo.need_fullpath) {
        rc = read_all(rom_path, FILE_MAX, &rom, &rom_size);
        if (rc != READ_OK) {
            fail(err, errlen, "%s: %s", rom_path,
                 rc == READ_TOO_BIG ? "file too large" : "cannot read game");
            goto fail;
        }
        info.data = rom;
        info.size = rom_size;
    }
    if (!core.load_game(&info)) {
        fail(err, errlen, "%s: core refused the game", rom_path);
        goto fail;
    }
    game_loaded = 1;
    core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    // Timing and geometry are only valid once a game is loaded (mGBA dereferences its
    // emulator, which load_game creates).
    memset(&av, 0, sizeof av);
    core.get_system_av_info(&av);
    set_av_info(&av);
    if (!(timing.fps > 0)) timing.fps = 60.0;
    if (!fb) {
        fail(err, errlen, "out of memory for a %ux%u frame", geo.max_width, geo.max_height);
        goto fail;
    }
    frame.px = fb;
    frame.aspect = default_aspect();
    if (audio_open(cfg->sample_rate, timing.sample_rate, cfg->buffer_ms) != 0) {
        fail(err, errlen, "out of memory for the audio buffer");
        goto fail;
    }
    for (int i = 0; i < 2; i++) {
        mem_open(&mems[i]);
        if (mems[i].size && !mems[i].last) {
            fail(err, errlen, "out of memory for saved game data");
            goto fail;
        }
    }
    open_flag = 1;
    return 0;

fail:
    teardown();
    return -1;
}

int player_is_open(void) { return open_flag; }

void player_close(void)
{
    if (!open_flag) return;
    player_save_sram();
    open_flag = 0;
    teardown();
}

void player_set_input(unsigned mask) { input_mask = mask & 0xffffu; }

void player_run(void)
{
    if (!open_flag) return;
    core.run();
}

const PlayerFrame *player_frame(void) { return &frame; }

int player_set_option(const char *key, const char *value)
{
    Option *o = NULL;
    const char *v;

    if (!open_flag || !key || !value || !value[0]) return -1;
    for (int i = 0; i < nopts && !o; i++)
        if (!strcmp(opts[i].key, key)) o = &opts[i];
    if (!o || !opt_allows(o, value)) return -1;
    // Kept as the strongest source too, so it survives the core declaring its options anew.
    if (raw_set(key, value, FROM_CHOICE)) return -1;
    if (o->value && !strcmp(o->value, value)) return 0;
    if (!(v = arena_str(value))) return -1;
    o->value = v;
    options_updated = 1;
    return 1;
}

int player_peek(int frames)
{
    size_t size;
    uint8_t *state, *mem[2] = {NULL, NULL};
    int rc = -1;

    if (!open_flag || lost || frames < 1 || frames > 8) return -1;
    size = core.serialize_size();
    if (!size || size > FILE_MAX || !(state = malloc(size))) return -1;
    if (!core.serialize(state, size)) goto done;
    // Save states don't always carry the cartridge save (mGBA's leave it out), and a frame
    // may write to it: it goes back by hand.
    for (int i = 0; i < 2; i++) {
        void *data = mems[i].size ? core.get_memory_data(mems[i].id) : NULL;
        if (!data) continue;
        if (!(mem[i] = malloc(mems[i].size))) goto done;
        memcpy(mem[i], data, mems[i].size);
    }
    // The frames' sound never happened: the core's output isn't taken while they run, so
    // what was buffered before plays on untouched.
    audio_muted = 1;
    for (int i = 0; i < frames; i++) core.run();
    audio_muted = 0;
    if (!core.unserialize(state, size)) {
        // The game is frames ahead of where it was paused, and its saves may not match its
        // state: nothing of it may be saved from now on (see player_save_*).
        lost = 1;
        rc = PLAYER_PEEK_LOST;
        goto done;
    }
    for (int i = 0; i < 2; i++) {
        void *data = mem[i] ? core.get_memory_data(mems[i].id) : NULL;
        if (data) memcpy(data, mem[i], mems[i].size);
    }
    rc = 0;
done:
    free(mem[0]);
    free(mem[1]);
    free(state);
    return rc;
}

double player_fps(void) { return timing.fps > 0 ? timing.fps : 60.0; }

void player_reset(void)
{
    if (open_flag) core.reset();
}

int player_save_state(const char *path)
{
    size_t size;
    uint8_t *buf;
    int rc = -1;

    if (!open_flag || lost || !path) return -1;
    size = core.serialize_size();
    if (!size || size > FILE_MAX) return -1;
    buf = malloc(size);
    if (!buf) return -1;
    if (core.serialize(buf, size) && write_atomic(path, buf, size) == 0) rc = 0;
    free(buf);
    return rc;
}

int player_load_state(const char *path)
{
    uint8_t *buf;
    size_t size;
    int rc;

    if (!open_flag || !path) return -1;
    if (read_all(path, FILE_MAX, &buf, &size) != READ_OK) return -1;
    rc = core.unserialize(buf, size) ? 0 : -1;
    free(buf);
    return rc;
}

int player_save_sram(void)
{
    int rc = 0;
    if (!open_flag || lost) return -1;
    for (int i = 0; i < 2; i++)
        if (mem_save(&mems[i]) != 0) rc = -1;
    return rc;
}

int player_audio_read(int16_t *out, int frames)
{
    uint32_t mask, t, h;
    int n;

    if (!out || frames <= 0 || !ring) return 0;
    mask = ring_cap - 1;
    t = atomic_load_explicit(&ring_tail, memory_order_relaxed);
    h = atomic_load_explicit(&ring_head, memory_order_acquire);
    n = h - t;
    if (!atomic_load_explicit(&primed, memory_order_relaxed)) {
        if ((uint32_t)n < ring_target) n = 0; // not yet: silence, and nothing taken
        else atomic_store_explicit(&primed, 1, memory_order_relaxed);
    }
    if (n > frames) n = frames;
    for (int i = 0; i < n; i++) {
        uint32_t at = (t + (uint32_t)i) & mask;
        out[2 * i] = ring[2 * at];
        out[2 * i + 1] = ring[2 * at + 1];
    }
    // fetch_add: the producer may drop the oldest frames by advancing the tail too.
    atomic_fetch_add_explicit(&ring_tail, (uint32_t)n, memory_order_release);
    if (n < frames) memset(out + 2 * n, 0, (size_t)(frames - n) * 2 * sizeof *out);
    return n;
}

void player_audio_flush(void)
{
    if (!ring) return;
    atomic_store_explicit(&primed, 0, memory_order_relaxed);
    atomic_store_explicit(&ring_tail,
                          atomic_load_explicit(&ring_head, memory_order_acquire),
                          memory_order_release);
    have_prev = 0;
    phase = 0.0;
}

int player_audio_queued(void)
{
    if (!ring) return 0;
    return (int)(atomic_load_explicit(&ring_head, memory_order_acquire) -
                 atomic_load_explicit(&ring_tail, memory_order_acquire));
}
