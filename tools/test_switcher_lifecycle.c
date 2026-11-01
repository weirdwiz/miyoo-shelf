// Real main.c switcher orchestration + UI, fake platform/process adapters and UDP RA.
// Catches display handoff races that protocol-only tests miss.
#define main shelf_entry
#include "../src/main.c"
#undef main
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>

static pthread_mutex_t test_lock = PTHREAD_MUTEX_INITIALIZER;
static int fd, unknown, mismatch, delayed, requests, paused, frozen, server_stop;
static int display_open, init_calls, init_overlay, stops, continues, failures, key, kept_frame, term_pending;
static int play, wake, wake_sent, display_blank, exited;
static double clock_now;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", #c); failures++; } } while (0)

static pid_t fake_pid(void) { return 12345; }
static int fake_signal(pid_t pid, int sig)
{
    assert(pid == 12345);
    pthread_mutex_lock(&test_lock);
    if (sig == 0) {
        CHECK(exited);
        pthread_mutex_unlock(&test_lock);
        errno = ESRCH;
        return -1;
    }
    if (sig == SIGTERM) { // queued while stopped: handled once continued
        CHECK(!display_open);
        if (frozen) term_pending = 1; else exited = 1;
    }
    else if (sig == SIGSTOP) { CHECK(paused); frozen = 1; stops++; }
    else {
        assert(sig == SIGCONT);
        CHECK(!display_open); // no copying switcher pixels after emulation resumes
        frozen = 0; continues++;
        if (term_pending) exited = 1, term_pending = 0;
    }
    pthread_mutex_unlock(&test_lock);
    return 0;
}
static void *fake_ra(void *unused)
{
    for (;;) {
        struct sockaddr_in from;
        socklen_t len = sizeof from;
        char cmd[128], reply[256] = "";
        ssize_t n = recvfrom(fd, cmd, sizeof cmd - 1, 0, (void *)&from, &len);
        if (n < 0) continue;
        cmd[n] = 0;
        pthread_mutex_lock(&test_lock);
        if (server_stop) { pthread_mutex_unlock(&test_lock); break; }
        if (frozen) { pthread_mutex_unlock(&test_lock); continue; }
        if (!strcmp(cmd, "GET_STATUS")) {
            requests++;
            if (!(delayed && requests <= 2)) {
                if (unknown) strcpy(reply, "GET_STATUS CONTENTLESS\n");
                else snprintf(reply, sizeof reply, "GET_STATUS %s game_boy_advance,%s,crc32=123\n",
                              paused ? "PAUSED" : "PLAYING", mismatch ? "Other game" : "Pokemon - Emerald Version (USA, Europe)");
            }
        } else if (!strcmp(cmd, "PAUSE_TOGGLE")) paused = !paused;
        if (*reply) sendto(fd, reply, strlen(reply), 0, (void *)&from, len);
        pthread_mutex_unlock(&test_lock);
    }
    return NULL;
}
int platform_init(int overlay)
{
    init_calls++; init_overlay = overlay;
    pthread_mutex_lock(&test_lock); display_open = 1; pthread_mutex_unlock(&test_lock);
    return 0;
}
void platform_quit(void)
{
    char marker[PATH_LEN];
    host_path(SYSDIR "/.runGameSwitcher", marker, sizeof marker);
    CHECK(access(marker, F_OK) == 0); // keymon must still know the overlay owns input
    pthread_mutex_lock(&test_lock); display_open = 0; pthread_mutex_unlock(&test_lock);
}
Button platform_poll(void)
{
    if (wake && !wake_sent && clock_now >= 9) {
        wake_sent = 1; display_blank = 1;
        return BTN_REDRAW;
    }
    if (key == 0 && clock_now >= (wake ? 10 : 2)) { key++; return play ? BTN_RIGHT : BTN_A; }
    if (play && key == 1 && clock_now >= 2.4) { key++; return BTN_A; }
    if (clock_now > 12) { CHECK(0 && "loop did not finish"); return BTN_QUIT; }
    return BTN_NONE;
}
void platform_present(const Image *frame) { display_blank = 0; clock_now += 1.0 / 60; }
void platform_keep_last_frame(void) { kept_frame = 1; }
int platform_grab(Image *out) { memset(out->px, 0, (size_t)out->w * out->h * 4); return 0; }
int platform_vsync_paced(void) { return 1; }
void platform_click(void) {}
double platform_now(void) { return clock_now; }
void platform_sleep_until(double until) { clock_now = until; usleep(1000); }
void platform_wait(double until)
{
    double next = wake && !wake_sent ? 9 : key == 0 ? (wake ? 10 : 2) : play && key == 1 ? 2.4 : until;
    if (next < until) until = next;
    if (until > clock_now) clock_now = until;
    usleep(1000); // let real workers finish, independently of virtual animation time
}
void platform_wake(void) {}

int main(int argc, char **argv)
{
    if (argc != 4) return 2;
    unknown = !strcmp(argv[1], "unknown");
    mismatch = !strcmp(argv[1], "mismatch");
    delayed = !strcmp(argv[1], "delayed");
    play = !strcmp(argv[1], "play");
    wake = !strcmp(argv[1], "wake");
    char dir[] = "/tmp/shelf-lifecycle-XXXXXX", cmd[2048];
    assert(mkdtemp(dir));
    snprintf(cmd, sizeof cmd, "cp -R '%s/.' '%s/'", argv[2], dir);
    assert(system(cmd) == 0);
    setenv("SHELF_ROOT", dir, 1); // sw process operations never kill a host emulator
    setenv("SHELF_STATUS", "76,0,3", 1);
    lib_set_root(dir);
    char update[PATH_LEN];
    snprintf(update, sizeof update, "%s/.tmp_update", dir);
    assert(mkdir(update, 0755) == 0);
    touch(SYSDIR "/.runGameSwitcher");
    // Runtime handoff flags: both or neither, and a failure is reported, not ignored.
    if (!strcmp(argv[1], "resume")) {
        char flags[PATH_LEN + 16], flag[PATH_LEN + 48];
        snprintf(flags, sizeof flags, "%s/missing", dir);
        runtime_tmp = flags;
        CHECK(queue_resume_flags() == -1);
        snprintf(flags, sizeof flags, "%s/flags", dir);
        assert(mkdir(flags, 0755) == 0);
        snprintf(flag, sizeof flag, "%s/force_auto_load_state", flags);
        assert(mkdir(flag, 0755) == 0); // the second flag can't be created
        CHECK(queue_resume_flags() == -1);
        snprintf(flag, sizeof flag, "%s/quick_switch", flags);
        CHECK(access(flag, F_OK) != 0);
        snprintf(flag, sizeof flag, "%s/force_auto_load_state", flags);
        assert(rmdir(flag) == 0);
        CHECK(queue_resume_flags() == 0 && access(flag, F_OK) == 0);
    }
    // keymon's flag for Onion's "Saving ..." panel: Shelf's switcher clears it.
    char flags[PATH_LEN + 16], saving[PATH_LEN + 48];
    snprintf(flags, sizeof flags, "%s/flags", dir);
    mkdir(flags, 0755);
    runtime_tmp = flags;
    snprintf(saving, sizeof saving, "%s/.displaySavingMessage", flags);
    assert(touch(saving) == 0);
    sw_set_process_hooks(fake_pid, fake_signal);
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(55355), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(fd, (void *)&addr, sizeof addr) == 0);
    pthread_t server;
    assert(!pthread_create(&server, NULL, fake_ra, NULL));
    // Real orchestration with simulated filesystem handoff (no host /tmp flags).
    char *args[] = {"gameSwitcher", "--overlay", NULL};
    int rc = switcher_main(1, 1, argv[3], args);
    CHECK(rc == 0);
    if (unknown || mismatch) CHECK(init_calls == 0);
    else {
        CHECK(init_calls == 1 && init_overlay);
        CHECK(stops == 1 && continues == 1);
    }
    CHECK(!display_open && !frozen);
    if (wake) CHECK(wake_sent && !display_blank);
    if (play) CHECK(exited);
    // A game being left keeps the switcher's frame up; a resumed one gets its own back.
    CHECK(kept_frame == play);
    CHECK((access(saving, F_OK) == 0) == (unknown || mismatch));
    pthread_mutex_lock(&test_lock); server_stop = 1; pthread_mutex_unlock(&test_lock);
    sendto(fd, "stop", 4, 0, (void *)&addr, sizeof addr);
    pthread_join(server, NULL);
    close(fd);
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    assert(system(cmd) == 0);
    if (failures) return 1;
    printf("switcher lifecycle %s passed\n", argv[1]);
    return 0;
}
