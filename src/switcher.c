#include "switcher.h"

#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb_image_write.h"

// On the simulator (SHELF_ROOT set) there is no Onion to drive: log instead of running
// its tools or killing processes.
static int simulated(void) { return getenv("SHELF_ROOT") != NULL; }

static void run(const char *cmd)
{
    if (simulated()) { fprintf(stderr, "shelf: (sim) %s\n", cmd); return; }
    if (system(cmd) == -1) perror(cmd);
}

/* ---------- Onion's screenshot names ---------- */

// FNV1A_Pippip_Yurii from Onion's src/common/utils/hash.h, with its unaligned 8-byte
// loads done through memcpy. Little-endian, like the device.
uint32_t sw_hash(const char *s)
{
    const uint32_t PRIME = 591798841;
    uint64_t h = 14695981039346656037ULL, w;
    size_t len = strlen(s);
    if (len > 8) {
        size_t cycles = ((len - 1) >> 4) + 1, head = len - (cycles << 3);
        for (; cycles--; s += 8) {
            memcpy(&w, s, 8);
            h = (h ^ w) * PRIME;
            memcpy(&w, s + head, 8);
            h = (h ^ w) * PRIME;
        }
    } else {
        w = 0; // Onion reads 8 bytes and masks off the ones past the end
        memcpy(&w, s, len);
        h = (h ^ w) * PRIME;
    }
    uint32_t h32 = (uint32_t)(h ^ (h >> 32));
    return h32 ^ (h32 >> 16);
}

void sw_screen_path(const char *rompath, char *out, int n)
{
    char dev[PATH_LEN];
    snprintf(dev, sizeof dev, SD_PREFIX "/Saves/CurrentProfile/romScreens/%u.png", (unsigned)sw_hash(rompath));
    host_path(dev, out, n);
}

/* ---------- RetroArch's network commands (UDP 55355) ---------- */

// Sends cmd; with a reply buffer, waits up to timeout_ms for the answer. Returns 0 on success.
static int ra_command(const char *cmd, char *reply, int n, int timeout_ms)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in to = {0};
    to.sin_family = AF_INET;
    to.sin_port = htons(55355);
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int rc = sendto(fd, cmd, strlen(cmd), 0, (struct sockaddr *)&to, sizeof to) < 0 ? -1 : 0;
    if (!rc && reply) {
        ssize_t got = recvfrom(fd, reply, n - 1, 0, NULL, NULL);
        if (got < 0) rc = -1;
        else reply[got] = 0;
    }
    close(fd);
    return rc;
}

/* ---------- overlay: save, resume, quit ---------- */

static void (*notify)(void);

void sw_set_notify(void (*fn)(void)) { notify = fn; }

static pthread_t saver;
static pthread_mutex_t save_lock = PTHREAD_MUTEX_INITIALIZER;
static int save_state, saver_started;
static Image *save_frame;
static char save_path[PATH_LEN];

static int write_png(const Image *img, const char *path)
{
    unsigned char *rgb = malloc((size_t)img->w * img->h * 3);
    if (!rgb) return -1;
    for (int i = 0; i < img->w * img->h; i++) {
        uint32_t p = img->px[i];
        rgb[i * 3] = p >> 16; rgb[i * 3 + 1] = p >> 8; rgb[i * 3 + 2] = p;
    }
    char tmp[PATH_LEN + 8];
    snprintf(tmp, sizeof tmp, "%s.new", path);
    stbi_write_png_compression_level = 4; // speed over size on the device's CPU
    int ok = stbi_write_png(tmp, img->w, img->h, 3, rgb, img->w * 3);
    free(rgb);
    if (!ok || rename(tmp, path)) { remove(tmp); return -1; }
    return 0;
}

static void *save_main(void *ud)
{
    (void)ud;
    if (save_frame && write_png(save_frame, save_path))
        fprintf(stderr, "shelf: couldn't save %s\n", save_path);
    // Onion's RetroArch answers once the state is written; a full save can take seconds.
    char reply[128];
    int ok = !ra_command("SAVE_STATE_SLOT -1", reply, sizeof reply, 60000);
    fprintf(stderr, "shelf: autosave %s\n", ok ? reply : "got no answer");
    pthread_mutex_lock(&save_lock);
    save_state = ok ? SW_SAVE_DONE : SW_SAVE_FAILED;
    pthread_mutex_unlock(&save_lock);
    if (notify) notify();
    return NULL;
}

int sw_overlay_begin(const Image *frame, const Recent *top)
{
    run("playActivity stop_all &");
    char reply[1100];
    if (ra_command("GET_STATUS", reply, sizeof reply, 500)) {
        fprintf(stderr, "shelf: RetroArch didn't answer GET_STATUS\n");
        return 0;
    }
    // "GET_STATUS PAUSED game_boy_advance,Advance Wars (U) (V1.1) [!],crc32=26fd0fc9"
    char state[32] = "", info[1024] = "";
    sscanf(reply, "GET_STATUS %31s %1023[^\n]", state, info);
    if (strcmp(state, "PLAYING") && strcmp(state, "PAUSED")) return 0;
    if (!strcmp(state, "PLAYING")) ra_command("PAUSE", NULL, 0, 0);

    // Running content is the first recent when ",<rom name>," appears in the info, as
    // Onion checks.
    const char *base = strrchr(top->rompath, '/');
    char name[PATH_LEN + 2];
    snprintf(name, sizeof name, ",%s", base ? base + 1 : top->rompath);
    char *dot = strrchr(name, '.');
    if (dot && dot > name + 1) strcpy(dot, ",");
    else strncat(name, ",", sizeof name - strlen(name) - 1);
    char padded[1100];
    snprintf(padded, sizeof padded, "%s,", info);
    if (!strstr(padded, name)) {
        fprintf(stderr, "shelf: RetroArch is running something else: %s\n", info);
        return 0;
    }

    if (frame) {
        save_frame = img_new(frame->w, frame->h);
        if (save_frame) memcpy(save_frame->px, frame->px, (size_t)frame->w * frame->h * 4);
    }
    sw_screen_path(top->rompath, save_path, sizeof save_path);
    save_state = SW_SAVE_BUSY;
    if (pthread_create(&saver, NULL, save_main, NULL)) save_main(NULL);
    else saver_started = 1;
    return 1;
}

int sw_save_state(void)
{
    pthread_mutex_lock(&save_lock);
    int s = save_state;
    pthread_mutex_unlock(&save_lock);
    return s;
}

static void save_join(void)
{
    if (saver_started) pthread_join(saver, NULL);
    saver_started = 0;
    img_free(save_frame);
    save_frame = NULL;
}

void sw_overlay_resume(void)
{
    save_join();
    ra_command("UNPAUSE", NULL, 0, 0);
    run("playActivity resume &");
}

void sw_quit_game(void)
{
    save_join();
    run("killall -TERM retroarch");
    if (simulated()) return;
    for (int i = 0; i < 50 && system("pidof retroarch > /dev/null") == 0; i++) usleep(100000);
    if (system("pidof retroarch > /dev/null") == 0) {
        fprintf(stderr, "shelf: RetroArch didn't quit; killing it\n");
        fclose(fopen("/tmp/.forceKillRetroarch", "w")); // tells the runtime it wasn't a crash
        run("killall -9 retroarch");
    }
}

/* ---------- screenshot cards ---------- */

#define FULL_KEEP 3 // full-size screenshots kept besides the live frame

static struct {
    const Library *lib;
    const Recent *rec;
    int n, w, h, sel;
    Image **card, **full;
    char *tried;
    unsigned revision, used_clock, *used;
    int quit, running;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t wake;
} C = {.lock = PTHREAD_MUTEX_INITIALIZER, .wake = PTHREAD_COND_INITIALIZER};

static Image *card_from(const Image *full)
{
    // Screens are 4:3 like the cards; crop anything else to fill.
    float k = (float)C.w / full->w > (float)C.h / full->h ? (float)C.w / full->w : (float)C.h / full->h;
    int cw = (int)(C.w / k), ch = (int)(C.h / k);
    if (cw == full->w && ch == full->h) return img_resize(full, C.w, C.h);
    Image crop = {cw, ch, malloc((size_t)cw * ch * 4)};
    if (!crop.px) return NULL;
    int ox = (full->w - cw) / 2, oy = (full->h - ch) / 2;
    for (int y = 0; y < ch; y++) memcpy(crop.px + y * cw, full->px + (oy + y) * full->w + ox, (size_t)cw * 4);
    Image *out = img_resize(&crop, C.w, C.h);
    free(crop.px);
    return out;
}

// Cards this far either side of the selection are loaded: the row shows one each side,
// so the next two are ready before they scroll in. The rest load as the selection nears.
#define CARD_REACH 3

// Next job: the selection first (card and full size), then cards outward from it.
static int next_job(int *want_full)
{
    for (int d = 0; d < C.n && d <= CARD_REACH; d++)
        for (int s = -1; s <= 1; s += 2) {
            int i = C.sel + d * s;
            if (i < 0 || i >= C.n || (d == 0 && s > 0)) continue;
            if (C.tried[i]) continue;
            *want_full = i == C.sel;
            return i;
        }
    if (C.sel >= 0 && C.sel < C.n && !C.full[C.sel] && C.tried[C.sel] == 1) { *want_full = 1; return C.sel; }
    return -1;
}

static void *cards_main(void *ud)
{
    (void)ud;
    pthread_mutex_lock(&C.lock);
    while (!C.quit) {
        int want_full, i = next_job(&want_full);
        if (i < 0) { pthread_cond_wait(&C.wake, &C.lock); continue; }
        C.tried[i] = 2; // 1: card made, full size not kept; 2: nothing more to load
        int need_card = !C.card[i];
        char path[PATH_LEN];
        sw_screen_path(C.rec[i].rompath, path, sizeof path);
        pthread_mutex_unlock(&C.lock);
        Image *full = img_load(path), *card = full && need_card ? card_from(full) : NULL;
        pthread_mutex_lock(&C.lock);
        if (card) C.card[i] = card;
        if (full && want_full && i == C.sel && !C.full[i]) {
            C.full[i] = full;
            C.used[i] = ++C.used_clock;
            full = NULL;
            // Keep a few; never the selection, never the live frame (0 is set, not loaded).
            int kept = 0;
            for (int k = 1; k < C.n; k++) kept += C.full[k] != NULL;
            while (kept > FULL_KEEP) {
                int old = -1;
                for (int k = 1; k < C.n; k++)
                    if (C.full[k] && k != C.sel && (old < 0 || C.used[k] < C.used[old])) old = k;
                if (old < 0) break;
                img_free(C.full[old]);
                C.full[old] = NULL;
                C.tried[old] = 1;
                kept--;
            }
        } else if (full) C.tried[i] = 1;
        int changed = card || C.full[i];
        if (changed) C.revision++;
        pthread_mutex_unlock(&C.lock);
        img_free(full);
        if (changed && notify) notify();
        pthread_mutex_lock(&C.lock);
    }
    pthread_mutex_unlock(&C.lock);
    return NULL;
}

void sw_cards_start(const Library *lib, const Recent *rec, int w, int h, Image *live)
{
    C.lib = lib; C.rec = rec; C.n = lib->ngames; C.w = w; C.h = h; C.sel = 0;
    C.card = calloc(C.n + 1, sizeof *C.card);
    C.full = calloc(C.n + 1, sizeof *C.full);
    C.tried = calloc(C.n + 1, 1);
    C.used = calloc(C.n + 1, sizeof *C.used);
    if (live && C.n) {
        C.full[0] = live;
        C.card[0] = card_from(live);
        C.tried[0] = 2;
    } else img_free(live);
    C.running = !pthread_create(&C.thread, NULL, cards_main, NULL);
}

void sw_cards_stop(void)
{
    if (C.running) {
        pthread_mutex_lock(&C.lock);
        C.quit = 1;
        pthread_cond_signal(&C.wake);
        pthread_mutex_unlock(&C.lock);
        pthread_join(C.thread, NULL);
        C.running = 0;
    }
    for (int i = 0; i < C.n; i++) { img_free(C.card[i]); img_free(C.full[i]); }
    free(C.card); free(C.full); free(C.tried); free(C.used);
    C.card = C.full = NULL;
    C.n = 0;
}

const Image *sw_card(int i)
{
    pthread_mutex_lock(&C.lock);
    const Image *img = i >= 0 && i < C.n ? C.card[i] : NULL;
    pthread_mutex_unlock(&C.lock);
    return img;
}

const Image *sw_full(int i)
{
    pthread_mutex_lock(&C.lock);
    const Image *img = i >= 0 && i < C.n ? C.full[i] : NULL;
    if (img) C.used[i] = ++C.used_clock;
    pthread_mutex_unlock(&C.lock);
    return img;
}

void sw_card_select(int i)
{
    pthread_mutex_lock(&C.lock);
    C.sel = i;
    pthread_cond_signal(&C.wake);
    pthread_mutex_unlock(&C.lock);
}

unsigned sw_cards_revision(void)
{
    pthread_mutex_lock(&C.lock);
    unsigned r = C.revision;
    pthread_mutex_unlock(&C.lock);
    return r;
}
