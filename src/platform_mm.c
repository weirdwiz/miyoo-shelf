// Miyoo Mini / Mini Plus: /dev/fb0 page flipping, evdev input and CLOCK_MONOTONIC timing,
// with no SDL. Audio lives in audio_mm.c, the hardware settings in hw_mm.c.
//
// The panel is mounted upside down, so frames are rotated 180 degrees on the way out. When
// presenting waits for vblank, a frame goes into the hidden fb page and a flipper thread
// issues FBIOPAN_DISPLAY once it is complete: the pan blocks until vblank, so the main
// thread renders the next frame meanwhile and waits only before touching that page again.
// Without vblank pacing there is nothing to hide a flip behind, so frames go to the page on
// screen.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

// linux/input.h names the gamepad and mouse codes BTN_*, the names platform.h gives the
// panel's buttons; the panel sends only KEY_* codes, so those macros go before the
// interface is read.
#undef BTN_A
#undef BTN_B
#undef BTN_X
#undef BTN_Y
#undef BTN_LEFT
#undef BTN_RIGHT
#undef BTN_START
#undef BTN_SELECT

#include "platform.h"
#include "settings.h"
#include "fb_geometry.h"

/* ---------- display ---------- */

static int fb = -1;
static struct fb_var_screeninfo fb_var;
static unsigned char *fb_mem;
static size_t fb_len;
static int fb_pitch;  // bytes per row
static int fb_dpitch; // pixels per row
static int fb_pages;  // 2 when the driver gives us a hidden page, 1 while drawing on screen
static int fb_back;   // the page the next frame is drawn into
static int fb_h;      // rows per page: the panel's height, which can exceed the canvas's
static int fb_ox, fb_oy; // where the 640x480 canvas sits on a larger panel (centred)
static int vsync_paced;
static size_t fb_smem;  // what the driver can map, for the probe
static FILE *probe_out; // raw input events go here while the probe runs

static pthread_t flipper;
static pthread_mutex_t flip_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t flip_cond = PTHREAD_COND_INITIALIZER;
static int flip_pending, flip_quit, flip_target;

// Pans the panel to the page at `yoffset`. On the device this blocks until the next vblank.
static int fb_pan(unsigned yoffset)
{
    struct fb_var_screeninfo var = fb_var;
    var.yoffset = yoffset;
    return ioctl(fb, FBIOPAN_DISPLAY, &var);
}

static void *flip_main(void *ud)
{
    (void)ud;
    pthread_mutex_lock(&flip_lock);
    for (;;) {
        while (!flip_pending && !flip_quit) pthread_cond_wait(&flip_cond, &flip_lock);
        if (!flip_pending) break;
        int page = flip_target;
        pthread_mutex_unlock(&flip_lock);
        fb_pan((unsigned)fb_h * page);
        pthread_mutex_lock(&flip_lock);
        flip_pending = 0;
        pthread_cond_broadcast(&flip_cond);
    }
    pthread_mutex_unlock(&flip_lock);
    return NULL;
}

// Once this returns the previous pan is done and the page about to be written is off screen.
static void flip_wait(void)
{
    if (fb_pages < 2) return;
    pthread_mutex_lock(&flip_lock);
    while (flip_pending) pthread_cond_wait(&flip_cond, &flip_lock);
    pthread_mutex_unlock(&flip_lock);
}

// Shows the page just drawn and makes the other one the drawing target.
static void flip_request(void)
{
    if (fb_pages < 2) return; // drawing straight to the page that is on screen
    pthread_mutex_lock(&flip_lock);
    flip_target = fb_back;
    flip_pending = 1;
    pthread_cond_signal(&flip_cond);
    pthread_mutex_unlock(&flip_lock);
    fb_back ^= 1;
}

// The canvas area of the page the next frame goes into, or NULL without a usable panel.
// On a panel larger than the canvas the border around it stays black: it is painted once
// in fb_init and never drawn over.
static uint32_t *fb_page(void)
{
    if (fb < 0) return NULL;
    return (uint32_t *)(fb_mem + (size_t)fb_pitch * fb_h * fb_back) + (size_t)fb_oy * fb_dpitch + fb_ox;
}

// The canvas area in black; rotation makes no difference to a solid colour.
static void page_black(uint32_t *page)
{
    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++) page[(size_t)y * fb_dpitch + x] = 0xff000000;
}

// The device's FBIOPAN_DISPLAY waits for vblank, so it takes up to a frame time; the test
// VM's returns at once. Three pans tell the two apart.
static void fb_measure_pacing(void)
{
    struct timespec a, b;
    double total = 0;
    for (int i = 0; i < 3; i++) {
        clock_gettime(CLOCK_MONOTONIC, &a);
        if (fb_pan((unsigned)fb_h * (i % 2 ? 0 : 1))) return;
        clock_gettime(CLOCK_MONOTONIC, &b);
        total += (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
    }
    vsync_paced = total / 3 >= .005;
    fb_pan(0); // start from page 0
}

// Returns 0 with page 0 on screen.
static int fb_init(void)
{
    struct fb_fix_screeninfo fix;
    int two;
    if ((fb = open("/dev/fb0", O_RDWR)) < 0) return -1;
    if (ioctl(fb, FBIOGET_VSCREENINFO, &fb_var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix)) goto fail;
    // The original Mini and Mini Plus panels are 640x480. Later units (Mini v4, newer Plus)
    // have a 752x560 panel, which may come up at either size: a larger one gets the canvas
    // centred, so Shelf runs everywhere instead of refusing to start.
    if (fb_var.bits_per_pixel != 32 || fb_canvas_origin((int)fb_var.xres, (int)fb_var.yres, &fb_ox, &fb_oy)) {
        fprintf(stderr, "shelf: fb0 is %ux%u at %u bpp; Shelf needs at least 640x480 at 32\n",
                fb_var.xres, fb_var.yres, fb_var.bits_per_pixel);
        goto fail;
    }
    fb_h = (int)fb_var.yres;
    if (fb_ox || fb_oy)
        fprintf(stderr, "shelf: fb0 is %ux%u; canvas centred at %d,%d\n", fb_var.xres, fb_var.yres, fb_ox, fb_oy);
    // Ask for the hidden page when the driver did not hand one out, then believe whatever
    // geometry it reports back (some kernels keep the visible page only).
    if (fb_var.yres_virtual < 2u * fb_h) {
        fb_var.yres_virtual = 2 * fb_h;
        ioctl(fb, FBIOPUT_VSCREENINFO, &fb_var);
        if (ioctl(fb, FBIOGET_VSCREENINFO, &fb_var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix))
            goto fail;
    }
    fb_pitch = (int)fix.line_length;
    fb_smem = fix.smem_len;
    if (fb_pitch < (int)fb_var.xres * 4 || fb_pitch % 4) goto fail;
    fb_dpitch = fb_pitch / 4;
    two = fb_var.yres_virtual >= 2u * fb_h;
    fb_len = (size_t)fb_pitch * fb_h * (two ? 2 : 1);
    if (fb_len > fix.smem_len) { // what we can map, not the geometry, is the real limit
        two = 0;
        fb_len = (size_t)fb_pitch * fb_h;
        if (fb_len > fix.smem_len) goto fail;
    }
    fb_mem = mmap(NULL, fb_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
    if (fb_mem == MAP_FAILED) { fb_mem = NULL; goto fail; }
    for (size_t i = 0; i < fb_len / 4; i++) ((uint32_t *)fb_mem)[i] = 0xff000000; // border included
    fb_pages = 1;
    fb_back = 0;
    if (!two) {
        fprintf(stderr, "shelf: one fb page only; drawing to the visible page\n");
        return 0;
    }
    fb_measure_pacing(); // leaves page 0 on screen
    if (!vsync_paced) {
        // Nothing to hide a flip behind, and a framebuffer that is a lazily copied shadow
        // (a virtual GPU) keeps showing the page that was on screen.
        fprintf(stderr, "shelf: no vblank to flip behind; drawing to the visible page\n");
    } else if (pthread_create(&flipper, NULL, flip_main, NULL)) {
        fprintf(stderr, "shelf: no flipper thread; drawing to the visible page\n");
    } else {
        fb_pages = 2;
        fb_back = 1;
        return 0;
    }
    // Drawing straight to the page on screen, so there is no vblank wait in presenting.
    vsync_paced = 0;
    return 0;
fail:
    if (fb >= 0) close(fb);
    fb = -1;
    return -1;
}

static void fb_quit(void)
{
    if (fb < 0) return;
    if (fb_pages > 1) {
        pthread_mutex_lock(&flip_lock);
        flip_quit = 1;
        pthread_cond_broadcast(&flip_cond);
        pthread_mutex_unlock(&flip_lock);
        pthread_join(flipper, NULL); // finishes a pending pan first
    }
    // Leave the last frame on page 0 for whatever runs next.
    if (fb_pages > 1 && !fb_back)
        memcpy(fb_mem, fb_mem + (size_t)fb_pitch * fb_h, (size_t)fb_pitch * fb_h);
    if (fb_pages > 1) fb_pan(0);
    munmap(fb_mem, fb_len);
    fb_mem = NULL;
    close(fb);
    fb = -1;
}

// The Miyoo panel is mounted upside down. Rotate only presentation, not input or the shared
// canvas. Respect the row pitch rather than assuming packed rows.
static void rotate_into(uint32_t *dst, int dpitch, const Image *frame)
{
    for (int y = 0; y < frame->h; y++) {
        uint32_t *row = dst + (size_t)y * dpitch;
        const uint32_t *src = frame->px + (size_t)(frame->h - 1 - y) * frame->w;
        for (int x = 0; x < frame->w; x++) row[x] = src[frame->w - 1 - x];
    }
}

// What each page was last given, so a game frame only repaints the border when the game's
// rectangle changes (the usual case is the same rect every frame).
static ScaleRect page_rect[2];
static int page_rect_set[2];

void platform_present(const Image *frame)
{
    uint32_t *page = fb_page();
    if (!page) return;
    flip_wait();
    rotate_into(page, fb_dpitch, frame);
    page_rect_set[fb_back] = 0; // a whole-page draw: the game's border has to be repainted
    flip_request();
}

void platform_present_game(const uint32_t *px, int w, int h, int pitch, float aspect, ScaleMode mode)
{
    uint32_t *page = fb_page();
    if (!page) return;
    ScaleRect r = scale_rect(w, h, aspect, SCREEN_W, SCREEN_H, mode);
    flip_wait();
    if (!page_rect_set[fb_back] || memcmp(&page_rect[fb_back], &r, sizeof r)) {
        scale_clear_outside(page, fb_dpitch, SCREEN_W, SCREEN_H, r, 0xff000000, 1);
        page_rect[fb_back] = r;
        page_rect_set[fb_back] = 1;
    }
    if (scale_blit(page, fb_dpitch, SCREEN_W, SCREEN_H, px, w, h, pitch, r, mode, 1)) {
        page_black(page);
        page_rect_set[fb_back] = 0;
    }
    flip_request();
}

int platform_vsync_paced(void) { return vsync_paced; }

/* ---------- console ---------- */

// The kernel console would draw its text over Shelf; graphics mode stops it. tty1 is the
// fallback when tty0 is not there (the device's console has moved between the two).
static int tty_fd = -1;

static void console_mode(int mode)
{
    if (tty_fd >= 0) ioctl(tty_fd, KDSETMODE, mode);
}

static void console_graphics(void)
{
    static const char *const TTY[] = {"/dev/tty0", "/dev/tty1"};
    for (int i = 0; i < 2 && tty_fd < 0; i++) {
        int fd = open(TTY[i], O_RDWR);
        if (fd < 0) continue;
        if (ioctl(fd, KDSETMODE, KD_GRAPHICS)) {
            close(fd);
            continue;
        }
        tty_fd = fd;
    }
    if (tty_fd < 0) fprintf(stderr, "shelf: no console to put in graphics mode\n");
}

static void console_quit(void)
{
    if (tty_fd < 0) return;
    console_mode(KD_TEXT);
    close(tty_fd);
    tty_fd = -1;
}

/* ---------- input ---------- */

#define INPUT_MAX 8
static int in_fd[INPUT_MAX];
static int in_n;

// The panel's keys are the PC ones (docs/hardware.md); POWER and the volume keys come from
// a second device on the Mini Plus.
static const struct {
    unsigned short code;
    Button button;
} KEYMAP[] = {
    {KEY_UP, BTN_UP},        {KEY_DOWN, BTN_DOWN},
    {KEY_LEFT, BTN_LEFT},    {KEY_RIGHT, BTN_RIGHT},
    {KEY_SPACE, BTN_A},      {KEY_LEFTCTRL, BTN_B},
    {KEY_LEFTSHIFT, BTN_X},  {KEY_LEFTALT, BTN_Y},
    {KEY_E, BTN_L},          {KEY_T, BTN_R},
    {KEY_TAB, BTN_L2},       {KEY_BACKSPACE, BTN_R2},
    {KEY_ENTER, BTN_START},  {KEY_RIGHTCTRL, BTN_SELECT},
    {KEY_ESC, BTN_MENU},
    {KEY_POWER, BTN_POWER},
    {KEY_VOLUMEUP, BTN_VOLUME}, {KEY_VOLUMEDOWN, BTN_VOLUME},
};

static Button map_code(unsigned short code)
{
    for (size_t i = 0; i < sizeof KEYMAP / sizeof *KEYMAP; i++)
        if (KEYMAP[i].code == code) return KEYMAP[i].button;
    return BTN_NONE;
}

// Does this device report any of the panel's keys? (POWER and volume are on their own
// device, and so is anything else the kernel exposes.)
static int has_panel_keys(int fd)
{
    unsigned long bits[(KEY_CNT + 8 * sizeof(long) - 1) / (8 * sizeof(long))] = {0};
    if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof bits), bits) < 0) return 0;
    for (size_t i = 0; i < sizeof KEYMAP / sizeof *KEYMAP; i++) {
        unsigned long code = KEYMAP[i].code;
        if (bits[code / (8 * sizeof(long))] & (1ul << (code % (8 * sizeof(long))))) return 1;
    }
    return 0;
}

static void input_open(void)
{
    DIR *d = opendir("/dev/input");
    if (!d) return;
    for (struct dirent *e; in_n < INPUT_MAX && (e = readdir(d));) {
        char path[64];
        if (strncmp(e->d_name, "event", 5)) continue;
        snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
        int fd = open(path, O_RDONLY | O_NONBLOCK);
        // Grab it so the kernel console and anything else behind us never see these keys.
        if (fd < 0 || !has_panel_keys(fd) || ioctl(fd, EVIOCGRAB, 1)) {
            if (fd >= 0) close(fd);
            continue;
        }
        in_fd[in_n++] = fd;
    }
    closedir(d);
    if (!in_n) fprintf(stderr, "shelf: no input device with the panel's keys\n");
}

static void input_close(void)
{
    for (int i = 0; i < in_n; i++) {
        ioctl(in_fd[i], EVIOCGRAB, 0);
        close(in_fd[i]);
    }
    in_n = 0;
}

// Buttons held right now, for gameplay input.
static unsigned held_mask;

// D-pad auto-repeat: the kernel's own repeat (value 2) is ignored so the timing is ours.
static Button repeat_button = BTN_NONE;
static double repeat_next;
#define REPEAT_DELAY 0.28
#define REPEAT_RATE 0.075

// POWER is a tap (standby) unless it is held: then it is a power-off, sent once.
#define POWER_HOLD 2.0
static int power_down, power_off_sent;
static double power_down_at;

// Decoded events wait here for platform_poll; one read can produce several.
#define QUEUE_LEN 16
static Button queue[QUEUE_LEN];
static int q_head, q_tail;

static void queue_push(Button b)
{
    int next = (q_head + 1) % QUEUE_LEN;
    if (next == q_tail) return; // full: input outran the frame loop, drop the oldest news
    queue[q_head] = b;
    q_head = next;
}

static Button queue_pop(void)
{
    if (q_head == q_tail) return BTN_NONE;
    Button b = queue[q_tail];
    q_tail = (q_tail + 1) % QUEUE_LEN;
    return b;
}

// The hardware keys move the volume themselves; the UI is only told to remember it.
static void volume_step(int delta)
{
    int level = platform_volume() + delta;
    if (level < 0) level = 0;
    if (level > VOLUME_MAX) level = VOLUME_MAX;
    platform_set_volume(level);
}

static int is_dpad(Button b) { return b >= BTN_UP && b <= BTN_RIGHT; }

static void key_event(unsigned short code, int value)
{
    Button b = map_code(code);
    if (b == BTN_NONE) return;
    if (value == 2) return; // kernel autorepeat
    if (value == 0) held_mask &= ~(1u << b);
    else held_mask |= 1u << b;

    if (b == BTN_POWER) {
        if (value == 1) {
            power_down = 1;
            power_down_at = platform_now();
            power_off_sent = 0;
        } else {
            power_down = 0;
            if (!power_off_sent) queue_push(BTN_POWER); // a long hold already powered off
        }
    } else if (b == BTN_VOLUME) {
        if (value == 1) {
            volume_step(code == KEY_VOLUMEUP ? 1 : -1);
            queue_push(BTN_VOLUME);
        }
    } else if (value == 0) {
        if (b == repeat_button) repeat_button = BTN_NONE;
    } else {
        if (is_dpad(b)) {
            repeat_button = b;
            repeat_next = platform_now() + REPEAT_DELAY;
        }
        queue_push(b);
    }
}

static void input_read(void)
{
    struct input_event buf[32];
    for (int i = 0; i < in_n; i++) {
        for (;;) {
            ssize_t n = read(in_fd[i], buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            for (ssize_t j = 0; j < n / (ssize_t)sizeof buf[0]; j++) {
                if (probe_out && buf[j].type != EV_SYN)
                    fprintf(probe_out, "%8.3f input %d type %u code %u value %d\n", platform_now(), i,
                            buf[j].type, buf[j].code, buf[j].value);
                if (buf[j].type == EV_KEY) key_event(buf[j].code, buf[j].value);
            }
        }
    }
}

unsigned platform_held(void) { return held_mask; }

Button platform_poll(void)
{
    if (q_head == q_tail) input_read();
    Button b = queue_pop();
    if (b != BTN_NONE) return b;
    double now = platform_now();
    if (power_down && !power_off_sent && now - power_down_at >= POWER_HOLD) {
        power_off_sent = 1;
        return BTN_POWER_OFF; // once, while still held
    }
    if (repeat_button != BTN_NONE && now >= repeat_next) {
        repeat_next += REPEAT_RATE;
        return repeat_button;
    }
    return BTN_NONE;
}

/* ---------- time ---------- */

double platform_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void platform_sleep_until(double t)
{
    double d = t - platform_now();
    if (d <= 0) return;
    struct timespec ts = {(time_t)d, (long)((d - (double)(time_t)d) * 1e9)};
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}

/* ---------- waiting ---------- */

// Background threads (the icon loader) end a wait through this pipe.
static int wake_pipe[2] = {-1, -1};

void platform_wake(void)
{
    int fd = wake_pipe[1];
    if (fd >= 0 && write(fd, "w", 1) < 0) {} // full pipe: a wakeup is pending anyway
}

void platform_wait(double until)
{
    if (q_head != q_tail) return; // platform_poll has events to hand out first
    double now = platform_now();
    if (repeat_button != BTN_NONE && repeat_next < until) until = repeat_next;
    if (power_down && !power_off_sent && power_down_at + POWER_HOLD < until)
        until = power_down_at + POWER_HOLD;
    if (until <= now) return;

    struct pollfd fds[1 + INPUT_MAX];
    int n = 0;
    fds[n++] = (struct pollfd){wake_pipe[0], POLLIN, 0}; // -1 when there is no pipe: ignored
    for (int i = 0; i < in_n; i++) fds[n++] = (struct pollfd){in_fd[i], POLLIN, 0};
    int ms = (int)ceil((until - now) * 1000); // round up: waking early would spin
    if (poll(fds, (nfds_t)n, ms) <= 0) return;
    if (fds[0].revents & POLLIN) {
        char drain[64];
        while (read(wake_pipe[0], drain, sizeof drain) > 0) {}
    }
    // Input readiness is left to platform_poll, which reads and decodes it.
}

/* ---------- probe ---------- */

void platform_probe(FILE *out)
{
    probe_out = out;
    if (!out) return;
    if (fb < 0) {
        fprintf(out, "fb0: not open\n");
    } else {
        fprintf(out, "fb0: %ux%u, virtual %ux%u, %u bpp, line %d bytes, smem %zu, canvas at %d,%d, "
                     "%d page(s), vsync paced %d\n",
                fb_var.xres, fb_var.yres, fb_var.xres_virtual, fb_var.yres_virtual, fb_var.bits_per_pixel,
                fb_pitch, fb_smem, fb_ox, fb_oy, fb_pages, vsync_paced);
        fprintf(out, "fb0 timing: pixclock %u, margins %u %u %u %u, sync %u %u\n", fb_var.pixclock,
                fb_var.left_margin, fb_var.right_margin, fb_var.upper_margin, fb_var.lower_margin,
                fb_var.hsync_len, fb_var.vsync_len);
    }
    for (int i = 0; i < in_n; i++) {
        char name[128] = "?";
        ioctl(in_fd[i], EVIOCGNAME(sizeof name), name);
        fprintf(out, "input %d: %s\n", i, name);
    }
    if (!in_n) fprintf(out, "input: none with the panel's keys\n");
}

/* ---------- lifetime ---------- */

int platform_init(void)
{
    if (fb_init()) {
        fprintf(stderr, "shelf: /dev/fb0 unusable\n");
        return -1;
    }
    console_graphics();
    if (pipe(wake_pipe)) wake_pipe[0] = wake_pipe[1] = -1;
    for (int i = 0; i < 2; i++)
        if (wake_pipe[i] >= 0) fcntl(wake_pipe[i], F_SETFL, fcntl(wake_pipe[i], F_GETFL) | O_NONBLOCK);
    input_open();
    return 0;
}

void platform_quit(void)
{
    input_close();
    for (int i = 0; i < 2; i++)
        if (wake_pipe[i] >= 0) {
            close(wake_pipe[i]);
            wake_pipe[i] = -1;
        }
    fb_quit();
    console_quit();
}
