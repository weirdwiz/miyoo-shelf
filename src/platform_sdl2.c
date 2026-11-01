// Mac/Linux simulator: a 640x480 canvas in an SDL2 window, keyboard as the Miyoo buttons.
//
// Keys: arrows = d-pad, Z / space / return = A, X / backspace = B, A = X, Y / S = Y,
// Q = L1, W = R1, Tab = SELECT, O = START, M = MENU, P = POWER (tap: standby, hold it for
// two seconds: power off), Esc = close the window (Shelf quits and saves).
#include <SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "library.h"
#include "platform.h"
#include "settings.h"

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static Image *game_canvas; // platform_present_game's scaled frame
static Uint32 wake_event;  // platform_wake's user event
static int standby_on;     // the panel is blank while asleep

// D-pad auto-repeat, same feel as on the device.
static Button repeat_button = BTN_NONE;
static double repeat_next;
#define REPEAT_DELAY 0.28
#define REPEAT_RATE 0.075

// Buttons held right now, for gameplay input.
static unsigned held_mask;

// POWER is a tap (standby) unless it is held: then it is a power-off, sent once.
#define POWER_HOLD 2.0
static int power_down, power_off_sent;
static double power_down_at;

/* ---------- audio ---------- */

// Game audio comes from the player's pull function; the UI click is mixed on top of it.
static int (*audio_source)(int16_t *out, int frames);
static SDL_AudioDeviceID audio_dev;
static SDL_AudioSpec audio_have;
static int16_t *click;       // at the device's rate, stereo s16
static int click_frames;
static volatile int click_pos; // played frames of the click
static int volume_level = VOLUME_MAX;

static int16_t clamp16(int v) { return (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v); }

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    int16_t *out = (int16_t *)stream;
    int frames = len / (int)(2 * sizeof *out), got = 0;
    if (audio_source) got = audio_source(out, frames);
    if (got < 0) got = 0;
    if (got < frames) memset(out + 2 * got, 0, (size_t)(frames - got) * 2 * sizeof *out);
    int pos = click_pos;
    if (click && pos < click_frames) {
        int n = click_frames - pos < frames ? click_frames - pos : frames;
        for (int i = 0; i < n; i++) {
            out[2 * i] = clamp16(out[2 * i] + click[2 * (pos + i)]);
            out[2 * i + 1] = clamp16(out[2 * i + 1] + click[2 * (pos + i) + 1]);
        }
        click_pos = pos + n;
    }
    if (volume_level < VOLUME_MAX) { // the system volume scales everything, clicks included
        int gain = volume_level * 32768 / VOLUME_MAX;
        for (int i = 0; i < 2 * frames; i++) out[i] = (int16_t)((int)out[i] * gain / 32768);
    }
}

// The click on the SD card, converted to the device's format. Missing is fine: no clicks.
static void click_load(void)
{
    char dev[PATH_LEN], path[PATH_LEN];
    snprintf(dev, sizeof dev, SD_PREFIX "/.shelf/sound/click.wav");
    host_path(dev, path, sizeof path);
    SDL_AudioSpec wav;
    Uint8 *buf = NULL;
    Uint32 len = 0;
    if (!SDL_LoadWAV(path, &wav, &buf, &len)) return;
    SDL_AudioCVT cvt;
    if (SDL_BuildAudioCVT(&cvt, wav.format, wav.channels, wav.freq, audio_have.format,
                          audio_have.channels, audio_have.freq) < 0) {
        SDL_FreeWAV(buf);
        return;
    }
    cvt.len = (int)len;
    cvt.buf = malloc((size_t)len * (cvt.len_mult > 0 ? cvt.len_mult : 1));
    if (!cvt.buf) { SDL_FreeWAV(buf); return; }
    memcpy(cvt.buf, buf, len);
    SDL_FreeWAV(buf);
    if (cvt.needed) SDL_ConvertAudio(&cvt);
    click = (int16_t *)cvt.buf;
    click_frames = cvt.len_cvt / (int)(2 * sizeof *click);
    click_pos = click_frames;
}

static void audio_init(void)
{
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    SDL_AudioSpec want;
    memset(&want, 0, sizeof want);
    want.freq = PLATFORM_AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audio_cb;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &audio_have, 0);
    if (!audio_dev) return;
    click_load();
    SDL_PauseAudioDevice(audio_dev, 0);
}

void platform_audio_source(int (*pull)(int16_t *out, int frames))
{
    if (audio_dev) SDL_LockAudioDevice(audio_dev);
    audio_source = pull;
    if (audio_dev) SDL_UnlockAudioDevice(audio_dev);
}

void platform_click(void)
{
    if (!audio_dev || !click) return;
    SDL_LockAudioDevice(audio_dev);
    click_pos = 0;
    SDL_UnlockAudioDevice(audio_dev);
}

/* ---------- platform ---------- */

int platform_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) { SDL_Log("SDL_Init: %s", SDL_GetError()); return -1; }
    const char *s = getenv("SHELF_SCALE");
    int scale = s ? atoi(s) : 2;
    if (scale < 1) scale = 1;
    win = SDL_CreateWindow("Shelf (simulator)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           SCREEN_W * scale, SCREEN_H * scale, SDL_WINDOW_ALLOW_HIGHDPI);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!win || !ren) { SDL_Log("SDL window: %s", SDL_GetError()); return -1; }
    SDL_RenderSetLogicalSize(ren, SCREEN_W, SCREEN_H);
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    wake_event = SDL_RegisterEvents(1);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);
    game_canvas = img_new(SCREEN_W, SCREEN_H);
    if (!tex || !game_canvas) return -1;
    audio_init();
    return 0;
}

int platform_vsync_paced(void) { return 0; } // desktop panels may refresh faster than 60 Hz

void platform_quit(void)
{
    platform_audio_source(NULL);
    if (audio_dev) SDL_CloseAudioDevice(audio_dev);
    audio_dev = 0;
    free(click);
    click = NULL;
    img_free(game_canvas);
    game_canvas = NULL;
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    win = NULL;
    SDL_Quit();
}

static Button map_key(SDL_Keycode k)
{
    switch (k) {
    case SDLK_UP: return BTN_UP;
    case SDLK_DOWN: return BTN_DOWN;
    case SDLK_LEFT: return BTN_LEFT;
    case SDLK_RIGHT: return BTN_RIGHT;
    case SDLK_z: case SDLK_SPACE: case SDLK_RETURN: return BTN_A;
    case SDLK_x: case SDLK_BACKSPACE: return BTN_B;
    case SDLK_a: return BTN_X;
    case SDLK_y: case SDLK_s: return BTN_Y;
    case SDLK_q: return BTN_L;
    case SDLK_w: return BTN_R;
    case SDLK_TAB: return BTN_SELECT;
    case SDLK_o: return BTN_START;
    case SDLK_m: return BTN_MENU;
    case SDLK_ESCAPE: return BTN_QUIT;
    default: return BTN_NONE;
    }
}

static int is_dpad(Button b) { return b >= BTN_UP && b <= BTN_RIGHT; }
// The buttons a core can read: everything the panel has, without the system keys.
static int is_game_button(Button b) { return b >= BTN_UP && b <= BTN_SELECT; }

unsigned platform_held(void) { return held_mask; }

Button platform_poll(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return BTN_QUIT;
        if (e.type != SDL_KEYDOWN && e.type != SDL_KEYUP) continue;
        SDL_Keycode key = e.key.keysym.sym;
        Button b = map_key(key);
        if (is_game_button(b)) {
            if (e.type == SDL_KEYDOWN) held_mask |= 1u << b;
            else held_mask &= ~(1u << b);
        }
        if (key == SDLK_p) { // POWER: a tap sleeps, holding it powers off
            if (e.type == SDL_KEYDOWN && !e.key.repeat) {
                power_down = 1;
                power_down_at = platform_now();
                power_off_sent = 0;
            } else if (e.type == SDL_KEYUP) {
                power_down = 0;
                if (!power_off_sent) return BTN_POWER;
            }
            continue;
        }
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
            if (b == BTN_NONE) continue;
            if (is_dpad(b)) { repeat_button = b; repeat_next = platform_now() + REPEAT_DELAY; }
            return b;
        }
        if (e.type == SDL_KEYUP && b == repeat_button) repeat_button = BTN_NONE;
    }
    if (power_down && !power_off_sent && platform_now() - power_down_at >= POWER_HOLD) {
        power_off_sent = 1;
        return BTN_POWER_OFF; // once, while still held
    }
    if (repeat_button != BTN_NONE && platform_now() >= repeat_next) {
        repeat_next += REPEAT_RATE;
        return repeat_button;
    }
    return BTN_NONE;
}

void platform_present(const Image *frame)
{
    if (!tex || !ren) return;
    if (standby_on) { // the panel is off
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderPresent(ren);
        return;
    }
    SDL_UpdateTexture(tex, NULL, frame->px, frame->w * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

void platform_present_game(const uint32_t *px, int w, int h, int pitch, float aspect, ScaleMode mode)
{
    if (!game_canvas && !(game_canvas = img_new(SCREEN_W, SCREEN_H))) return;
    ScaleRect r = scale_rect(w, h, aspect, SCREEN_W, SCREEN_H, mode);
    if (standby_on) {
        gfx_clear(game_canvas, 0xff000000);
    } else {
        scale_clear_outside(game_canvas->px, SCREEN_W, SCREEN_W, SCREEN_H, r, 0xff000000, 0);
        if (scale_blit(game_canvas->px, SCREEN_W, SCREEN_W, SCREEN_H, px, w, h, pitch, r, mode, 0))
            gfx_clear(game_canvas, 0xff000000);
    }
    platform_present(game_canvas);
}

double platform_now(void) { return SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

void platform_sleep_until(double t)
{
    double d = t - platform_now();
    if (d > 0.001) SDL_Delay((Uint32)(d * 1000));
}

void platform_wait(double until)
{
    if (!win) { platform_sleep_until(until); return; }
    if (repeat_button != BTN_NONE && repeat_next < until) until = repeat_next;
    if (power_down && !power_off_sent && power_down_at + POWER_HOLD < until)
        until = power_down_at + POWER_HOLD;
    double d = until - platform_now();
    if (d <= 0) return;
    SDL_WaitEventTimeout(NULL, (int)ceil(d * 1000)); // leaves the event for platform_poll
}

void platform_wake(void)
{
    if (!win) return;
    SDL_Event e = {0};
    e.type = wake_event;
    SDL_PushEvent(&e); // thread-safe; platform_poll discards it
}

// A desktop has no backlight to dim: the level lives in settings.json.
void platform_set_brightness(int level) { (void)level; }

void platform_set_volume(int level)
{
    if (level < 0) level = 0;
    if (level > VOLUME_MAX) level = VOLUME_MAX;
    if (audio_dev) SDL_LockAudioDevice(audio_dev);
    volume_level = level;
    if (audio_dev) SDL_UnlockAudioDevice(audio_dev);
}

int platform_volume(void) { return volume_level; }

// SHELF_STATUS="<percent>,<charging>" stands in for the battery: 76%, not charging.
int platform_battery(int *charging)
{
    int percent = 76, plugged = 0;
    const char *fake = getenv("SHELF_STATUS");
    if (fake && sscanf(fake, "%d,%d", &percent, &plugged) == 2) {
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        plugged = plugged != 0;
    } else {
        percent = 76;
        plugged = 0;
    }
    if (charging) *charging = plugged;
    return percent;
}

void platform_standby(int on)
{
    on = on != 0;
    if (on == standby_on) return;
    standby_on = on;
    if (audio_dev) SDL_PauseAudioDevice(audio_dev, on ? 1 : 0);
    if (on && ren) { // the panel goes dark
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderPresent(ren);
    }
}

void platform_probe(FILE *out)
{
    if (!out) return;
    int w = 0, h = 0;
    if (win) SDL_GetWindowSize(win, &w, &h);
    fprintf(out, "platform: SDL2 simulator, video driver %s, window %dx%d, vsync paced %d\n",
            SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "none", w, h, platform_vsync_paced());
}

// Nothing to ask of a desktop CPU; kept for parity with the device's governor.
void platform_performance(int game) { (void)game; }

void platform_power_off(void)
{
    // The device cuts power; here the window closes and the loops end on BTN_QUIT.
    SDL_Event e = {0};
    e.type = SDL_QUIT;
    SDL_PushEvent(&e);
}
