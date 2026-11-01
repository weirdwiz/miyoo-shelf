// Exercise the real overlay against old and new RetroArch UDP protocols.
#include "../src/switcher.h"
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int fd, legacy, silent, paused, saves, stop, frozen, stops, continues, exited, term_pending;
static pid_t fake_pid(void) { return 12345; }
static int fake_signal(pid_t pid, int sig)
{
    assert(pid == 12345);
    pthread_mutex_lock(&lock);
    if (sig == 0) {
        assert(exited);
        pthread_mutex_unlock(&lock);
        errno = ESRCH;
        return -1;
    }
    if (sig == SIGTERM) {
        if (frozen) term_pending = 1; // a stopped process handles it once continued
        else exited = 1;
    } else if (sig == SIGSTOP) {
        assert(paused && !frozen);
        assert(legacy || silent || saves == stops + 1); // acknowledgement must precede STOP
        frozen = 1; stops++;
    } else {
        assert(sig == SIGCONT && frozen);
        frozen = 0; continues++;
        if (term_pending) exited = 1, term_pending = 0;
    }
    pthread_mutex_unlock(&lock);
    return 0;
}
static void *server(void *unused)
{
    char cmd[128];
    while (1) {
        struct sockaddr_in from;
        socklen_t len = sizeof from;
        ssize_t n = recvfrom(fd, cmd, sizeof cmd - 1, 0, (void *)&from, &len);
        if (n < 0) continue;
        cmd[n] = 0;
        pthread_mutex_lock(&lock);
        if (stop) { pthread_mutex_unlock(&lock); break; }
        if (frozen) { pthread_mutex_unlock(&lock); continue; } // stopped RA cannot answer UDP
        char reply[256] = "";
        if (!strcmp(cmd, "GET_STATUS"))
            snprintf(reply, sizeof reply, "GET_STATUS %s game_boy_advance,Apotris,crc32=123\n", paused ? "PAUSED" : "PLAYING");
        else if (!strcmp(cmd, "GET_STATE_SLOT") && !legacy) strcpy(reply, "GET_STATE_SLOT 0\n");
        else if (!strcmp(cmd, "PAUSE") && !legacy) paused = 1;
        else if (!strcmp(cmd, "UNPAUSE") && !legacy) paused = 0;
        else if (!strcmp(cmd, "PAUSE_TOGGLE")) paused = !paused;
        else if (!strcmp(cmd, "SAVE_STATE_SLOT -1") && !legacy) {
            saves++;
            if (!silent) strcpy(reply, "SAVE_STATE_SLOT -1\n");
        }
        if (*reply) sendto(fd, reply, strlen(reply), 0, (void *)&from, len);
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}
static void expect_paused(int want)
{
    for (int i = 0; i < 100; i++) {
        pthread_mutex_lock(&lock);
        int got = paused;
        pthread_mutex_unlock(&lock);
        if (got == want) return;
        usleep(1000);
    }
    fprintf(stderr, "FAIL: expected RetroArch %s (%s protocol)\n", want ? "paused" : "playing", legacy ? "legacy" : "modern");
    exit(1);
}
int main(int argc, char **argv)
{
    legacy = argc > 1 && !strcmp(argv[1], "legacy");
    sw_set_process_hooks(fake_pid, fake_signal);
    setenv("SHELF_ROOT", "/tmp", 1); // Never run Onion tools or kill a real emulator.
    fd = socket(AF_INET, SOCK_DGRAM, 0);
    assert(fd >= 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(55355), .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    assert(bind(fd, (void *)&addr, sizeof addr) == 0);
    pthread_t thread;
    assert(!pthread_create(&thread, NULL, server, NULL));
    Recent top = {0};
    strcpy(top.rompath, "/mnt/SDCARD/Roms/GBA/Apotris.gba");
    for (int already_paused = 0; already_paused <= 1; already_paused++) {
        pthread_mutex_lock(&lock); paused = already_paused; pthread_mutex_unlock(&lock);
        assert(sw_overlay_begin(NULL, &top));
        expect_paused(1);
        for (int i = 0; i < 1000 && sw_save_state() == SW_SAVE_BUSY; i++) usleep(1000);
        assert(sw_save_state() == (legacy ? SW_SAVE_ON_EXIT : SW_SAVE_DONE));
        assert(!strcmp(sw_resume_command(), legacy ? "PAUSE_TOGGLE" : "UNPAUSE"));
        pthread_mutex_lock(&lock); assert(frozen); pthread_mutex_unlock(&lock);
        sw_overlay_resume();
        expect_paused(0);
        assert(sw_overlay_pid() == 0);
    }
    // Switching/home must thaw the renderer before TERM can write its exit autosave.
    assert(sw_overlay_begin(NULL, &top));
    sw_quit_game();
    pthread_mutex_lock(&lock);
    assert(!frozen && stops == 3 && continues == 3);
    paused = 0;
    pthread_mutex_unlock(&lock);
    // A save that never answers: reported as failed, but the renderer is still held so the
    // switcher shows; resuming thaws it before unpausing.
    if (!legacy) {
        pthread_mutex_lock(&lock); silent = 1; pthread_mutex_unlock(&lock);
        sw_set_save_timeout(50);
        assert(sw_overlay_begin(NULL, &top));
        for (int i = 0; i < 1000 && sw_save_state() == SW_SAVE_BUSY; i++) usleep(1000);
        assert(sw_save_state() == SW_SAVE_FAILED && sw_overlay_ready());
        pthread_mutex_lock(&lock); assert(frozen); pthread_mutex_unlock(&lock);
        sw_overlay_resume();
        expect_paused(0);
        pthread_mutex_lock(&lock); assert(!frozen && stops == 4 && continues == 4); silent = 0; pthread_mutex_unlock(&lock);
        sw_set_save_timeout(0);
    }
    // A mismatched recent must never pause unrelated running content.
    strcpy(top.rompath, "/mnt/SDCARD/Roms/GBA/Other.gba");
    assert(!sw_overlay_begin(NULL, &top));
    expect_paused(0);
    pthread_mutex_lock(&lock);
    assert(saves == (legacy ? 0 : 4));
    stop = 1;
    pthread_mutex_unlock(&lock);
    sendto(fd, "stop", 4, 0, (void *)&addr, sizeof addr);
    pthread_join(thread, NULL);
    close(fd);
    printf("overlay %s: pause, save-before-freeze, thaw-before-resume/quit and mismatch passed\n", legacy ? "legacy" : "modern");
    return 0;
}
