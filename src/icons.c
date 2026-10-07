#include "icons.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CACHE_VERSION 2

typedef struct {
    Image *img[2];   // [0] small, [1] big
    char want[2];    // requested this session
    char missing;    // no source art: caller draws a placeholder
} Entry;

static Library *L;
static Entry *E;
static char cache_dir[PATH_LEN];
static unsigned revision;
// Guards Entry fields and revision once the worker runs.
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t worker;
static int worker_running, worker_quit;

unsigned icons_revision(void)
{
    pthread_mutex_lock(&lock);
    unsigned r = revision;
    pthread_mutex_unlock(&lock);
    return r;
}

static void mkdirs(const char *path)
{
    char p[PATH_LEN];
    snprintf(p, sizeof p, "%s", path);
    for (char *c = p + 1; *c; c++)
        if (*c == '/') { *c = 0; mkdir(p, 0755); *c = '/'; }
    mkdir(p, 0755);
}

void icons_init(Library *lib)
{
    L = lib;
    revision = 0;
    E = calloc(lib->ngames ? lib->ngames : 1, sizeof *E);
    host_path(SD_PREFIX "/App/Shelf/cache", cache_dir, sizeof cache_dir);
    mkdirs(cache_dir);
}

void icons_free(void)
{
    if (!E) return;
    if (worker_running) {
        pthread_mutex_lock(&lock);
        worker_quit = 1;
        pthread_cond_signal(&wake);
        pthread_mutex_unlock(&lock);
        pthread_join(worker, NULL);
        worker_running = worker_quit = 0;
    }
    for (int i = 0; i < L->ngames; i++) { img_free(E[i].img[0]); img_free(E[i].img[1]); }
    free(E);
    E = NULL;
}

const Image *icon_get(int game, int big)
{
    Entry *e = &E[game];
    pthread_mutex_lock(&lock);
    if (!e->img[big] && !e->missing && !e->want[big]) {
        e->want[big] = 1;
        pthread_cond_signal(&wake);
    }
    const Image *img = e->img[big];
    pthread_mutex_unlock(&lock);
    return img;
}

// A square tile is a frame, not a square crop of a portrait poster.
static Image *art_tile(const Image *src, int size)
{
    int inset = (int)(size * .07f + .5f);
    int available = size - 2 * inset;
    float k = (float)available / src->w < (float)available / src->h
            ? (float)available / src->w : (float)available / src->h;
    int fw = (int)(src->w * k + .5f), fh = (int)(src->h * k + .5f);
    Image *fg = img_resize(src, fw, fh);
    if (!fg) return NULL;
    Image *out = img_new(size, size);
    if (!out) { img_free(fg); return NULL; }
    gfx_clear(out, rgb(0xf8f9fc));
    gfx_blit(out, fg, (size - fw) / 2, (size - fh) / 2, 255);
    img_free(fg);
    img_round(out, size * .085f);
    return out;
}

static void cache_path(const Game *g, int size, char *out, int n)
{
    uint32_t h = 2166136261u;
    for (const char *c = g->path; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
    snprintf(out, n, "%s/%08x-%d-v%d.raw", cache_dir, h, size, CACHE_VERSION);
}

static int newer(const char *a, const char *b) // a modified after b, or b missing
{
    struct stat sa, sb;
    if (stat(b, &sb)) return 1;
    if (stat(a, &sa)) return 0;
    return sa.st_mtime > sb.st_mtime;
}

// Pure work on read-only library data; safe off the main thread. NULL = no usable art.
static Image *build(int gi, int big)
{
    Game *g = &L->games[gi];
    int size = big ? ICON_BIG : ICON_SMALL;
    char src_path[PATH_LEN], raw[PATH_LEN];
    if (lib_img_path(L, g, src_path, sizeof src_path)) return NULL;
    cache_path(g, size, raw, sizeof raw);
    Image *img;
    if (!newer(src_path, raw) && (img = img_load_raw(raw))) return img;
    Image *src = img_load(src_path);
    if (!src) return NULL;
    img = art_tile(src, size);
    img_free(src);
    if (img) img_save_raw(img, raw);
    return img;
}

// Next request, big icons first in library order. Caller holds the lock.
static int next_request(int *gi, int *big)
{
    for (int i = 0; i < L->ngames; i++)
        for (int b = 1; b >= 0; b--)
            if (E[i].want[b] && !E[i].img[b] && !E[i].missing) { *gi = i; *big = b; return 1; }
    return 0;
}

static void publish(int gi, int big, Image *img)
{
    E[gi].img[big] = img;
    if (!img) E[gi].missing = 1;
    E[gi].want[big] = 0;
    revision++;
}

int icons_pump(int budget)
{
    if (worker_running) return 0;
    int made = 0, gi, big;
    while (made < budget && next_request(&gi, &big)) {
        publish(gi, big, build(gi, big));
        made++;
    }
    return made;
}

static void *work(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&lock);
    for (;;) {
        int gi, big;
        while (!worker_quit && !next_request(&gi, &big)) pthread_cond_wait(&wake, &lock);
        if (worker_quit) break;
        pthread_mutex_unlock(&lock);
        Image *img = build(gi, big);
        pthread_mutex_lock(&lock);
        publish(gi, big, img);
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

int icons_start_worker(void)
{
    if (worker_running) return 0;
    worker_quit = 0;
    if (pthread_create(&worker, NULL, work, NULL)) return -1;
    worker_running = 1;
    return 0;
}
