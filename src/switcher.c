#include "switcher.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
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
static int save_state, saver_started, legacy_ra, renderer_held;
static volatile sig_atomic_t renderer_pid;

static pid_t find_ra_pid(void)
{
    if (simulated()) return 0;
    DIR *dir = opendir("/proc");
    if (!dir) return 0;
    pid_t pid = 0;
    for (struct dirent *e; !pid && (e = readdir(dir));) {
        int candidate = atoi(e->d_name);
        if (candidate <= 0) continue;
        char path[64], name[32];
        snprintf(path, sizeof path, "/proc/%d/comm", candidate);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(name, sizeof name, f) && !strcmp(name, "retroarch\n")) pid = candidate;
        fclose(f);
    }
    closedir(dir);
    return pid;
}

static pid_t (*process_pid)(void) = find_ra_pid;
static int (*process_signal)(pid_t, int) = kill;

void sw_set_process_hooks(pid_t (*pid_fn)(void), int (*signal_fn)(pid_t, int))
{
    process_pid = pid_fn ? pid_fn : find_ra_pid;
    process_signal = signal_fn ? signal_fn : kill;
}

pid_t sw_overlay_pid(void) { return renderer_pid; }

static void hold_renderer(void)
{
    // PAUSED stops emulation, not SDL rendering. Freeze only AFTER required saves:
    // stopping before the UDP save reply would deadlock the save worker.
    int held = renderer_pid > 0 ? !process_signal(renderer_pid, SIGSTOP) : simulated();
    pthread_mutex_lock(&save_lock);
    renderer_held = held;
    pthread_mutex_unlock(&save_lock);
    if (!held) fprintf(stderr, "shelf: could not stop RetroArch renderer\n");
}

static void release_renderer(void)
{
    if (renderer_pid > 0) process_signal(renderer_pid, SIGCONT);
    renderer_pid = 0;
    pthread_mutex_lock(&save_lock);
    renderer_held = 0;
    pthread_mutex_unlock(&save_lock);
}

int sw_overlay_ready(void)
{
    pthread_mutex_lock(&save_lock);
    int ready = renderer_held;
    pthread_mutex_unlock(&save_lock);
    return ready;
}

const char *sw_resume_command(void) { return legacy_ra ? "PAUSE_TOGGLE" : "UNPAUSE"; }
static Image *save_frame;
static int save_timeout_ms = 60000; // large PS states on a slow card take seconds
void sw_set_save_timeout(int ms) { save_timeout_ms = ms > 0 ? ms : 60000; }
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
    // RetroArch 1.15 has no acknowledged autosave command. Do not send SAVE_STATE:
    // it changes user slots and cannot confirm completion. Its normal TERM exit writes
    // .state.auto; resuming a live game requires no state load.
    if (legacy_ra) {
        hold_renderer();
        pthread_mutex_lock(&save_lock);
        save_state = SW_SAVE_ON_EXIT;
        pthread_mutex_unlock(&save_lock);
        if (notify) notify();
        return NULL;
    }
    // Onion's newer RetroArch answers once the state is written (any reply, as Onion
    // reads it). Without an answer the state is unknown: say so, but still hold the
    // renderer once we stop waiting, or the switcher never shows and keeps the input.
    // A late save just continues after SIGCONT, which resume and quit both send first.
    char reply[128];
    int ok = !ra_command("SAVE_STATE_SLOT -1", reply, sizeof reply, save_timeout_ms);
    fprintf(stderr, "shelf: autosave %s\n", ok ? reply : "got no answer");
    hold_renderer();
    pthread_mutex_lock(&save_lock);
    save_state = ok ? SW_SAVE_DONE : SW_SAVE_FAILED;
    pthread_mutex_unlock(&save_lock);
    if (notify) notify();
    return NULL;
}

#define START_PATIENCE_MS 5000

// Whether RetroArch process `pid` lacks the newer commands. Probing costs a 200 ms
// timeout on legacy RA, so the answer is kept per process in /tmp (device only).
#define PROTOCOL_CACHE "/tmp/shelf-ra-protocol"
static int probe_legacy(pid_t pid)
{
    int cached_pid, cached;
    FILE *f = pid > 0 && !simulated() ? fopen(PROTOCOL_CACHE, "r") : NULL;
    if (f) {
        int ok = fscanf(f, "%d %d", &cached_pid, &cached) == 2 && cached_pid == pid;
        fclose(f);
        if (ok) return cached;
    }
    // Probe a read-only command, not a save or toggle. Legacy RA silently ignores it.
    char reply[128];
    int slot;
    int legacy = ra_command("GET_STATE_SLOT", reply, sizeof reply, 200) ||
                 sscanf(reply, "GET_STATE_SLOT %d", &slot) != 1;
    if (pid > 0 && !simulated() && (f = fopen(PROTOCOL_CACHE, "w"))) {
        fprintf(f, "%d %d\n", (int)pid, legacy);
        fclose(f);
    }
    return legacy;
}

int sw_overlay_begin(const Image *frame, const Recent *top)
{
    char reply[1100];
    int status_ok = 0;
    // MENU can arrive while a newly launched game is still loading (Lazarus' big state
    // took over 1.4 s on the device): keep asking for a while rather than drop the press.
    // Bounded; failure never falls through to a competing SDL console.
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int tries = 0;
    long ms = 0;
    while (!status_ok && ms < START_PATIENCE_MS) {
        status_ok = !ra_command("GET_STATUS", reply, sizeof reply, 300);
        tries++;
        if (!status_ok) usleep(50000);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    }
    if (!status_ok) {
        fprintf(stderr, "shelf: RetroArch didn't answer GET_STATUS (%d tries, %ld ms)\n", tries, ms);
        return 0;
    }
    fprintf(stderr, "shelf: GET_STATUS answered after %d %s, %ld ms\n", tries, tries == 1 ? "try" : "tries", ms);
    // "GET_STATUS PAUSED game_boy_advance,Advance Wars (U) (V1.1) [!],crc32=26fd0fc9"
    char state[32] = "", info[1024] = "";
    sscanf(reply, "GET_STATUS %31s %1023[^\n]", state, info);
    if (strcmp(state, "PLAYING") && strcmp(state, "PAUSED")) return 0;

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

    pid_t pid = process_pid();
    legacy_ra = probe_legacy(pid);
    if (!strcmp(state, "PLAYING") &&
        ra_command(legacy_ra ? "PAUSE_TOGGLE" : "PAUSE", NULL, 0, 0)) return 0;
    // Do not freeze a process until it has actually consumed the pause command.
    int paused = !strcmp(state, "PAUSED");
    for (int i = 0; !paused && i < 10; i++) {
        if (!ra_command("GET_STATUS", reply, sizeof reply, 100) &&
            sscanf(reply, "GET_STATUS %31s", state) == 1 && !strcmp(state, "PAUSED")) paused = 1;
        if (!paused) usleep(10000);
    }
    if (!paused) return 0;
    renderer_pid = pid;
    if (!renderer_pid && !simulated()) return 0;
    run("playActivity stop_all &");

    if (frame) {
        save_frame = img_new(frame->w, frame->h);
        if (save_frame) memcpy(save_frame->px, frame->px, (size_t)frame->w * frame->h * 4);
    }
    sw_screen_path(top->rompath, save_path, sizeof save_path);
    pthread_mutex_lock(&save_lock);
    save_state = SW_SAVE_BUSY;
    pthread_mutex_unlock(&save_lock);
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
    release_renderer(); // UDP commands cannot be answered while SIGSTOPped.
    // A toggle must only be sent when still paused (e.g. after a sleep/wake).
    char reply[1100], state[32] = "";
    if (!legacy_ra || (!ra_command("GET_STATUS", reply, sizeof reply, 500) &&
        sscanf(reply, "GET_STATUS %31s", state) == 1 && !strcmp(state, "PAUSED")))
        ra_command(sw_resume_command(), NULL, 0, 0);
    run("playActivity resume &");
}

void sw_quit_game(void)
{
    save_join();
    pid_t pid = renderer_pid;
    // Queue the TERM while the renderer is still stopped: it is handled as soon as the
    // CONT wakes it, so RA quits (autosaving) without first drawing its paused frame
    // over whatever the switcher left on screen.
    int gone = pid > 0 && process_signal(pid, SIGTERM) && errno == ESRCH;
    release_renderer(); // allow the exit autosave and normal termination to run
    if (pid <= 0 || gone) return; // never kill an unverified game or a newly launched one
    int waiting = 1;
    for (int i = 0; waiting && (legacy_ra || i < 50); i++) {
        waiting = !process_signal(pid, 0) || errno != ESRCH;
        if (waiting) usleep(100000);
    }
    // On legacy RA, termination IS the autosave. Never force-kill mid-save.
    if (waiting) {
        fprintf(stderr, "shelf: RetroArch didn't quit; killing it\n");
        FILE *marker = fopen("/tmp/.forceKillRetroarch", "w");
        if (marker) fclose(marker); // tells the runtime it wasn't a crash
        process_signal(pid, SIGKILL);
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
    C.lib = lib; C.rec = rec; C.n = lib->ngames; C.w = w; C.h = h; C.sel = 0; C.quit = 0;
    C.card = calloc(C.n + 1, sizeof *C.card);
    C.full = calloc(C.n + 1, sizeof *C.full);
    C.tried = calloc(C.n + 1, 1);
    C.used = calloc(C.n + 1, sizeof *C.used);
    if (!C.card || !C.full || !C.tried || !C.used) { // no cards: the row shows placeholders
        fprintf(stderr, "shelf: no memory for screenshot cards\n");
        sw_cards_stop();
        img_free(live);
        return;
    }
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
    for (int i = 0; i < C.n; i++) {
        if (C.card) img_free(C.card[i]);
        if (C.full) img_free(C.full[i]);
    }
    free(C.card); free(C.full); free(C.tried); free(C.used);
    C.card = C.full = NULL;
    C.tried = NULL;
    C.used = NULL;
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
