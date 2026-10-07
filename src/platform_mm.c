// Miyoo Mini / Mini Plus: SDL 1.2 with explicit 180-degree panel rotation.
// Buttons arrive as SDL key events using the stock Miyoo mapping (see Onion's keymap_sw.h).
#include <SDL/SDL.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <poll.h>
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
// SDL 1.2 keeps writing silence while paused, which keeps the audioserver busy too, so
// the device is closed once a click has drained and reopened for the next one.
static Uint8 *click;
static Uint32 click_len, click_pos;
static int audio_ok, audio_open;
static SDL_AudioSpec audio_want, audio_got;
static double audio_close_at;
#define AUDIO_LINGER 2.0 // seconds open after a click: rapid scrolling doesn't reopen it

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

// Opens the device in the format the click was converted to. Returns 0 on success.
static int audio_reopen(void)
{
    SDL_AudioSpec got;
    if (SDL_OpenAudio(&audio_want, &got)) return -1;
    if (got.format != audio_got.format || got.channels != audio_got.channels || got.freq != audio_got.freq) {
        SDL_CloseAudio();
        return -1;
    }
    audio_open = 1;
    return 0;
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
    audio_want = wav;
    audio_want.samples = 512; // ~12 ms at 44.1 kHz: the click lands with the frame
    audio_want.callback = audio_cb;
    audio_want.userdata = NULL;
    if (SDL_OpenAudio(&audio_want, &audio_got)) { SDL_FreeWAV(buf); return; }
    SDL_AudioCVT cvt;
    if (SDL_BuildAudioCVT(&cvt, wav.format, wav.channels, wav.freq, audio_got.format, audio_got.channels,
                          audio_got.freq) < 0) {
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
    SDL_CloseAudio(); // silent until the first click
}

void platform_click(void)
{
    if (!audio_ok) return;
    if (!audio_open && audio_reopen()) return;
    SDL_LockAudio();
    click_pos = 0; // restart: rapid scrolling retriggers rather than queueing
    SDL_UnlockAudio();
    SDL_PauseAudio(0);
    audio_close_at = platform_now() + AUDIO_LINGER;
}

// Closes the device once the last click has played and lingered.
static void audio_idle(double now)
{
    if (!audio_open || now < audio_close_at) return;
    SDL_LockAudio();
    int drained = click_pos >= click_len;
    SDL_UnlockAudio();
    if (!drained) { audio_close_at = now + .1; return; }
    SDL_CloseAudio();
    audio_open = 0;
}

/* ---------- waiting for input ---------- */

// SDL 1.2 reads the keys from the console tty (fbcon keyboard). Waiting polls that same
// descriptor without reading it, so SDL still gets every byte; a pipe carries wakeups
// from background threads.
static int tty_fd = -1, wake_pipe[2] = {-1, -1};

static int find_tty_fd(void)
{
    DIR *d = opendir("/proc/self/fd");
    if (!d) return -1;
    int found = -1;
    for (struct dirent *e; found < 0 && (e = readdir(d));) {
        char link[64], target[64];
        snprintf(link, sizeof link, "/proc/self/fd/%s", e->d_name);
        ssize_t n = readlink(link, target, sizeof target - 1);
        if (n <= 0) continue;
        target[n] = 0;
        if (!strncmp(target, "/dev/tty", 8) && strcmp(target, "/dev/tty")) found = atoi(e->d_name);
    }
    closedir(d);
    return found;
}

static void wait_init(void)
{
    tty_fd = find_tty_fd();
    if (pipe(wake_pipe)) wake_pipe[0] = wake_pipe[1] = -1;
    for (int i = 0; i < 2; i++)
        if (wake_pipe[i] >= 0) fcntl(wake_pipe[i], F_SETFL, fcntl(wake_pipe[i], F_GETFL) | O_NONBLOCK);
    if (tty_fd < 0) fprintf(stderr, "shelf: no input tty to wait on; idling in 10 ms steps\n");
}

static void wait_quit(void)
{
    int fds[2] = {wake_pipe[0], wake_pipe[1]};
    wake_pipe[0] = wake_pipe[1] = -1;
    for (int i = 0; i < 2; i++) if (fds[i] >= 0) close(fds[i]);
    tty_fd = -1;
}

void platform_wake(void)
{
    int fd = wake_pipe[1];
    if (fd >= 0 && write(fd, "w", 1) < 0) {} // full pipe: a wakeup is pending anyway
}

int platform_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return -1; }
    SDL_ShowCursor(SDL_DISABLE);
    video = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 32, SDL_HWSURFACE);
    if (!video) { fprintf(stderr, "SDL_SetVideoMode: %s\n", SDL_GetError()); return -1; }
    fb_init();
    audio_init();
    wait_init();
    return 0;
}

void platform_quit(void)
{
    if (audio_open) SDL_CloseAudio();
    audio_open = audio_ok = 0;
    free(click);
    click = NULL;
    wait_quit();
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

int platform_grab(Image *out)
{
    struct fb_var_screeninfo var;
    struct fb_fix_screeninfo fix;
    int fd = open("/dev/fb0", O_RDONLY);
    if (fd < 0) return -1;
    int rc = -1;
    if (!ioctl(fd, FBIOGET_VSCREENINFO, &var) && !ioctl(fd, FBIOGET_FSCREENINFO, &fix) &&
        var.xres == SCREEN_W && var.yres == SCREEN_H && var.bits_per_pixel == 32 &&
        (size_t)fix.line_length * (var.yoffset + SCREEN_H) <= fix.smem_len) {
        unsigned char *mem = mmap(NULL, fix.smem_len, PROT_READ, MAP_SHARED, fd, 0);
        if (mem != MAP_FAILED) {
            // The visible page, turned the right way up (the panel is mounted rotated).
            const unsigned char *page = mem + (size_t)fix.line_length * var.yoffset;
            for (int y = 0; y < SCREEN_H; y++) {
                const uint32_t *src = (const uint32_t *)(page + (size_t)(SCREEN_H - 1 - y) * fix.line_length);
                uint32_t *dst = out->px + y * SCREEN_W;
                for (int x = 0; x < SCREEN_W; x++) dst[x] = src[SCREEN_W - 1 - x] | 0xff000000u;
            }
            munmap(mem, fix.smem_len);
            rc = 0;
        }
    }
    close(fd);
    return rc;
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

void platform_wait(double until)
{
    double now = platform_now();
    audio_idle(now);
    if (audio_open && audio_close_at < until) until = audio_close_at;
    if (held != BTN_NONE && held_next < until) until = held_next;
    if (until <= now) return;
    if (wake_pipe[0] < 0) { platform_sleep_until(until); return; }
    struct pollfd fds[2] = {{wake_pipe[0], POLLIN, 0}, {tty_fd, POLLIN, 0}};
    int nfds = tty_fd >= 0 ? 2 : 1;
    // Bytes SDL left unread would keep the tty readable; don't spin on them.
    if (tty_fd >= 0 && poll(fds + 1, 1, 0) > 0) {
        platform_sleep_until(now + .005 < until ? now + .005 : until);
        return;
    }
    double step = tty_fd >= 0 ? until - now : .01; // without the tty, check input every 10 ms
    if (step > until - now) step = until - now;
    int ms = (int)(step * 1000) + 1;
    if (poll(fds, nfds, ms) > 0 && (fds[0].revents & POLLIN)) {
        char drain[64];
        while (read(wake_pipe[0], drain, sizeof drain) > 0) {}
    }
}
