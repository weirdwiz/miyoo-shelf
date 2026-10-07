// Mac/Linux simulator: a 640x480 canvas in an SDL2 window, keyboard as the Miyoo buttons.
#include <SDL.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;

// D-pad auto-repeat, same feel as on the device.
static Button held = BTN_NONE;
static double held_next;
#define REPEAT_DELAY 0.28
#define REPEAT_RATE 0.075

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
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);
    return tex ? 0 : -1;
}

void platform_click(void) {} // the simulator stays silent
int platform_vsync_paced(void) { return 0; } // desktop panels may refresh faster than 60 Hz

void platform_quit(void)
{
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
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

Button platform_poll(void)
{
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        if (e.type == SDL_QUIT) return BTN_QUIT;
        if (e.type == SDL_KEYDOWN && !e.key.repeat) {
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

void platform_present(const Image *frame)
{
    SDL_UpdateTexture(tex, NULL, frame->px, frame->w * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

int platform_grab(Image *out)
{
    const char *path = getenv("SHELF_FRAME");
    Image *img = path ? img_load(path) : NULL;
    Image *fit = img ? img_resize(img, SCREEN_W, SCREEN_H) : NULL;
    img_free(img);
    if (!fit) return -1;
    memcpy(out->px, fit->px, (size_t)SCREEN_W * SCREEN_H * 4);
    img_free(fit);
    return 0;
}

double platform_now(void) { return SDL_GetPerformanceCounter() / (double)SDL_GetPerformanceFrequency(); }

void platform_sleep_until(double t)
{
    double d = t - platform_now();
    if (d > 0.001) SDL_Delay((Uint32)(d * 1000));
}
