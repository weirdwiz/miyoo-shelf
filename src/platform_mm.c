// Miyoo Mini / Mini Plus: SDL 1.2 with explicit 180-degree panel rotation.
// Buttons arrive as SDL key events using the stock Miyoo mapping (see Onion's keymap_sw.h).
#include <SDL/SDL.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "../third_party/cJSON.h"
#include "platform.h"

static SDL_Surface *video;

// Page flipping straight on /dev/fb0, whose virtual height holds two screens. SDL 1.2
// here only offers the visible page, so drawing there tore mid-scanout; instead each
// frame goes to the hidden page and FBIOPAN_DISPLAY shows it. SDL still owns input.
// The pan blocks until vblank, so a flipper thread issues it: the main thread renders
// the next frame meanwhile and waits only before writing into the page being replaced.
static int fb = -1;
static struct fb_var_screeninfo fb_var;
static unsigned char *fb_mem;
static size_t fb_len;
static int fb_pitch, fb_back;
static pthread_t flipper;
static pthread_mutex_t flip_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t flip_cond = PTHREAD_COND_INITIALIZER;
static int flip_pending, flip_quit;

static void *flip_main(void *ud)
{
    (void)ud;
    pthread_mutex_lock(&flip_lock);
    for (;;) {
        while (!flip_pending && !flip_quit) pthread_cond_wait(&flip_cond, &flip_lock);
        if (!flip_pending) break;
        struct fb_var_screeninfo var = fb_var;
        pthread_mutex_unlock(&flip_lock);
        ioctl(fb, FBIOPAN_DISPLAY, &var);
        pthread_mutex_lock(&flip_lock);
        flip_pending = 0;
        pthread_cond_broadcast(&flip_cond);
    }
    pthread_mutex_unlock(&flip_lock);
    return NULL;
}

// Once this returns the previous flip is on screen and the back page is free.
static void flip_wait(void)
{
    pthread_mutex_lock(&flip_lock);
    while (flip_pending) pthread_cond_wait(&flip_cond, &flip_lock);
    pthread_mutex_unlock(&flip_lock);
}

static void fb_init(void)
{
    struct fb_fix_screeninfo fix;
    if (getenv("SHELF_NO_FLIP")) return;
    if ((fb = open("/dev/fb0", O_RDWR)) < 0) return;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &fb_var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix) ||
        fb_var.xres != SCREEN_W || fb_var.yres != SCREEN_H || fb_var.bits_per_pixel != 32 ||
        fb_var.yres_virtual < 2 * SCREEN_H) goto fail;
    fb_pitch = fix.line_length;
    fb_len = (size_t)fb_pitch * 2 * SCREEN_H;
    fb_mem = mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (fb_mem == MAP_FAILED) { fb_mem = NULL; goto fail; }
    fb_back = fb_var.yoffset >= SCREEN_H ? 0 : 1;
    if (!pthread_create(&flipper, NULL, flip_main, NULL)) return;
    munmap(fb_mem, fb_len);
fail:
    fprintf(stderr, "shelf: no fb page flip; drawing to the visible page\n");
    close(fb);
    fb = -1;
}

static void fb_quit(void)
{
    if (fb < 0) return;
    pthread_mutex_lock(&flip_lock);
    flip_quit = 1;
    pthread_cond_broadcast(&flip_cond);
    pthread_mutex_unlock(&flip_lock);
    pthread_join(flipper, NULL); // finishes a pending flip first
    // Leave page 0 showing what was last presented: the next app draws there.
    int shown = !fb_back;
    if (shown) memcpy(fb_mem, fb_mem + (size_t)fb_pitch * SCREEN_H, (size_t)fb_pitch * SCREEN_H);
    fb_var.yoffset = 0;
    ioctl(fb, FBIOPAN_DISPLAY, &fb_var);
    munmap(fb_mem, fb_len);
    close(fb);
    fb = -1;
}

static Button held = BTN_NONE;
static double held_next;
#define REPEAT_DELAY 0.28
#define REPEAT_RATE 0.075

// UI click: the active Onion theme's change.wav (what MainUI plays), else Onion's default.
// Audio goes through Onion's audioserver via libpadsp.so, preloaded by launch.sh.
static Uint8 *click;
static Uint32 click_len, click_pos;
static int audio_ok;

static void audio_cb(void *ud, Uint8 *out, int len)
{
    (void)ud;
    memset(out, 0, len);
    if (click_pos >= click_len) return;
    Uint32 n = click_len - click_pos < (Uint32)len ? click_len - click_pos : (Uint32)len;
    memcpy(out, click + click_pos, n);
    click_pos += n;
}

static int theme_sound(char *out, size_t n)
{
    FILE *f = fopen("/appconfigs/system.json", "rb");
    if (!f) return -1;
    char buf[4096];
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[len] = 0;
    cJSON *j = cJSON_Parse(buf);
    const char *theme = cJSON_GetStringValue(cJSON_GetObjectItem(j, "theme"));
    int r = theme ? (snprintf(out, n, "%s/sound/change.wav", theme), 0) : -1;
    cJSON_Delete(j);
    return r;
}

static void audio_init(void)
{
    if (getenv("SHELF_MUTE") || SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
    char themed[512];
    const char *paths[] = {themed, "/mnt/SDCARD/miyoo/app/sound/change.wav",
                           "/customer/app/sound/change.wav"};
    if (theme_sound(themed, sizeof themed)) paths[0] = paths[1];
    SDL_AudioSpec wav;
    Uint8 *buf = NULL;
    Uint32 len = 0;
    for (size_t i = 0; i < sizeof paths / sizeof *paths && !buf; i++)
        if (!SDL_LoadWAV(paths[i], &wav, &buf, &len)) buf = NULL;
    if (!buf) { fprintf(stderr, "shelf: no click sound\n"); return; }
    SDL_AudioSpec want = wav, got;
    want.samples = 512; // ~12 ms at 44.1 kHz: the click lands with the frame
    want.callback = audio_cb;
    want.userdata = NULL;
    if (SDL_OpenAudio(&want, &got)) { SDL_FreeWAV(buf); return; }
    SDL_AudioCVT cvt;
    if (SDL_BuildAudioCVT(&cvt, wav.format, wav.channels, wav.freq, got.format, got.channels, got.freq) < 0) {
        SDL_CloseAudio(); SDL_FreeWAV(buf); return;
    }
    cvt.len = (int)len;
    cvt.buf = malloc((size_t)len * (cvt.len_mult > 0 ? cvt.len_mult : 1));
    if (!cvt.buf) { SDL_CloseAudio(); SDL_FreeWAV(buf); return; }
    memcpy(cvt.buf, buf, len);
    SDL_FreeWAV(buf);
    if (cvt.needed) SDL_ConvertAudio(&cvt);
    click = cvt.buf;
    click_len = cvt.needed ? (Uint32)cvt.len_cvt : len;
    click_pos = click_len;
    audio_ok = 1;
    SDL_PauseAudio(0);
}

void platform_click(void)
{
    if (!audio_ok) return;
    SDL_LockAudio();
    click_pos = 0; // restart: rapid scrolling retriggers rather than queueing
    SDL_UnlockAudio();
}

int platform_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return -1; }
    SDL_ShowCursor(SDL_DISABLE);
    video = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 32, SDL_HWSURFACE);
    if (!video) { fprintf(stderr, "SDL_SetVideoMode: %s\n", SDL_GetError()); return -1; }
    fb_init();
    audio_init();
    return 0;
}

void platform_quit(void)
{
    if (audio_ok) SDL_CloseAudio();
    free(click);
    fb_quit();
    SDL_Quit();
}

static Button map_key(SDLKey k)
{
    switch (k) {
    case SDLK_UP: return BTN_UP;
    case SDLK_DOWN: return BTN_DOWN;
    case SDLK_LEFT: return BTN_LEFT;
    case SDLK_RIGHT: return BTN_RIGHT;
    case SDLK_SPACE: return BTN_A;
    case SDLK_LCTRL: return BTN_B;
    case SDLK_LSHIFT: return BTN_X;
    case SDLK_LALT: return BTN_Y;
    case SDLK_e: return BTN_L;
    case SDLK_t: return BTN_R;
    case SDLK_RETURN: return BTN_START;
    case SDLK_RCTRL: return BTN_SELECT;
    case SDLK_ESCAPE: return BTN_MENU;
    default: return BTN_NONE;
    }
}

static int is_dpad(Button b) { return b >= BTN_UP && b <= BTN_RIGHT; }

Button platform_poll(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return BTN_QUIT;
        if (e.type == SDL_KEYDOWN) {
            Button b = map_key(e.key.keysym.sym);
            if (is_dpad(b)) { held = b; held_next = platform_now() + REPEAT_DELAY; }
            if (b != BTN_NONE) return b;
        }
        if (e.type == SDL_KEYUP && map_key(e.key.keysym.sym) == held) held = BTN_NONE;
    }
    if (held != BTN_NONE && platform_now() >= held_next) {
        held_next += REPEAT_RATE;
        return held;
    }
    return BTN_NONE;
}

// The Miyoo panel is mounted upside down. Rotate only presentation, not input
// or the shared canvas. Respect the row pitch rather than assuming packed rows.
static void rotate_into(unsigned char *pixels, int pitch, const Image *frame)
{
    for (int y = 0; y < frame->h; y++) {
        uint32_t *dst = (uint32_t *)(pixels + y * pitch);
        const uint32_t *src = frame->px + (frame->h - 1 - y) * frame->w;
        for (int x = 0; x < frame->w; x++)
            dst[x] = src[frame->w - 1 - x];
    }
}

void platform_present(const Image *frame)
{
    if (fb >= 0) {
        flip_wait();
        rotate_into(fb_mem + (size_t)fb_pitch * SCREEN_H * fb_back, fb_pitch, frame);
        pthread_mutex_lock(&flip_lock);
        fb_var.yoffset = SCREEN_H * fb_back;
        flip_pending = 1;
        pthread_cond_signal(&flip_cond);
        pthread_mutex_unlock(&flip_lock);
        fb_back = !fb_back;
        return;
    }
    if (SDL_MUSTLOCK(video) && SDL_LockSurface(video) < 0) return;
    rotate_into(video->pixels, video->pitch, frame);
    if (SDL_MUSTLOCK(video)) SDL_UnlockSurface(video);
    SDL_Flip(video);
}

int platform_vsync_paced(void) { return fb >= 0; }

double platform_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void platform_sleep_until(double t)
{
    double d = t - platform_now();
    if (d <= 0) return;
    struct timespec ts = {(time_t)d, (long)((d - (time_t)d) * 1e9)};
    nanosleep(&ts, NULL);
}
