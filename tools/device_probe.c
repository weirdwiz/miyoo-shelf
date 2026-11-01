// Device-only overlay test helpers: resolve real recents indices, capture fb0, and
// perform a short sleep/wake. The wake driver must survive keymon's suspend list.
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdio.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>
#include "../src/library.h"

static int standby_check(int pid)
{
    char path[64], line[1024], governor[32] = "";
    snprintf(path, sizeof path, "/proc/%d/stat", pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return 1;
    int ok = fgets(line, sizeof line, fp) != NULL;
    fclose(fp);
    char *end = ok ? strrchr(line, ')') : NULL;
    char state = end && end[1] == ' ' ? end[2] : '?';
    fp = fopen("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", "r");
    if (fp) {
        fscanf(fp, "%31s", governor);
        fclose(fp);
    }
    printf("standby: pid=%d state=%c governor=%s\n", pid, state, governor);
    fflush(stdout);
    return state != 'T' || strcmp(governor, "powersave") != 0;
}

static int powercycle(int check_pid)
{
    if (access("/mnt/SDCARD/.tmp_update/config/.disableStandby", F_OK) == 0) {
        fputs("Refusing sleep/wake test: short POWER is configured to shut down\n", stderr);
        return 1;
    }
    // Onion deliberately excludes processes named sh from its SIGSTOP sweep. This
    // keeps only the timed test driver alive; keymon still performs real sleep/wake.
    if (prctl(PR_SET_NAME, "sh", 0, 0, 0)) return 1;
    // Losing the SSH session must not kill the only scheduled wakeup source.
    if (signal(SIGHUP, SIG_IGN) == SIG_ERR) return 1;
    int fd = open("/dev/input/event0", O_WRONLY);
    if (fd < 0) return 1;
    struct input_event e = {.type = EV_KEY, .code = KEY_POWER};
    int failed = 0;
    for (int cycle = 0; cycle < 2; cycle++) {
        e.value = 1;
        if (write(fd, &e, sizeof e) != sizeof e) return 1;
        usleep(100000);
        e.value = 0;
        if (write(fd, &e, sizeof e) != sizeof e) return 1;
        if (cycle == 0 && check_pid > 0) {
            sleep(1);
            failed = standby_check(check_pid);
            sleep(2);
        } else {
            sleep(cycle == 0 ? 3 : 2);
        }
    }
    close(fd);
    puts("short power-button sleep/wake completed");
    if (failed) fputs("FAIL: foreground process or CPU governor did not enter standby\n", stderr);
    return failed;
}
static int capture(const char *path)
{
    int fd = open("/dev/fb0", O_RDONLY);
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
    if (fd < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &fix) || ioctl(fd, FBIOGET_VSCREENINFO, &var)) return 1;
    if (var.bits_per_pixel != 32 || fix.line_length < var.xres * 4 ||
        (size_t)fix.line_length * (var.yoffset + var.yres) > fix.smem_len) return 1;
    unsigned char *mem = mmap(NULL, fix.smem_len, PROT_READ, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) return 1;
    FILE *out = fopen(path, "wb");
    if (!out) return 1;
    fprintf(out, "P6\n%u %u\n255\n", var.xres, var.yres);
    for (int y = var.yres - 1; y >= 0; y--)
        for (int x = var.xres - 1; x >= 0; x--) {
            unsigned char *p = mem + (size_t)(var.yoffset + y) * fix.line_length + x * 4;
            unsigned char rgb[3] = {p[2], p[1], p[0]};
            if (fwrite(rgb, 1, 3, out) != 3) return 1;
        }
    int rc = fclose(out) != 0;
    munmap(mem, fix.smem_len);
    close(fd);
    return rc;
}
// Taps each named Miyoo button, as the hardware reports it (a press, then a release).
static int keys(int argc, char **argv)
{
    static const struct { const char *name; int code; } KEYS[] = {
        {"up", KEY_UP}, {"down", KEY_DOWN}, {"left", KEY_LEFT}, {"right", KEY_RIGHT},
        {"a", KEY_SPACE}, {"b", KEY_LEFTCTRL}, {"x", KEY_LEFTSHIFT}, {"y", KEY_LEFTALT},
        {"l1", KEY_E}, {"r1", KEY_T}, {"start", KEY_ENTER}, {"select", KEY_RIGHTCTRL},
        {"menu", KEY_ESC},
    };
    int fd = open("/dev/input/event0", O_WRONLY);
    if (fd < 0) return 1;
    for (int i = 0; i < argc; i++) {
        int code = -1;
        for (size_t k = 0; k < sizeof KEYS / sizeof *KEYS; k++)
            if (!strcmp(argv[i], KEYS[k].name)) code = KEYS[k].code;
        if (code < 0) { fprintf(stderr, "unknown button %s\n", argv[i]); close(fd); return 2; }
        struct input_event e[2] = {{.type = EV_KEY, .code = code}, {.type = EV_SYN, .code = SYN_REPORT}};
        for (int value = 1; value >= 0; value--) {
            e[0].value = value;
            if (write(fd, e, sizeof e) != sizeof e) { close(fd); return 1; }
            usleep(value ? 80000 : 250000);
        }
    }
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "keys")) return keys(argc - 2, argv + 2);
    if (argc == 2 && !strcmp(argv[1], "powercycle")) return powercycle(0);
    if (argc == 3 && !strcmp(argv[1], "standby")) {
        int pid = atoi(argv[2]);
        if (pid <= 2) return 2;
        return powercycle(pid);
    }
    if (argc == 3 && !strcmp(argv[1], "capture")) return capture(argv[2]);
    if (argc == 3 && !strcmp(argv[1], "index")) {
        static Recent rec[MAX_RECENT];
        Library lib;
        if (lib_load_recents(&lib, rec, MAX_RECENT)) return 1;
        int index = -1;
        for (int i = 0; i < lib.ngames; i++)
            if (strstr(lib.games[i].title, argv[2])) { index = i; break; }
        lib_free(&lib);
        if (index < 0) return 1;
        printf("%d\n", index);
        return 0;
    }
    fprintf(stderr, "usage: %s powercycle | standby <foreground-pid> | capture <ppm> | index <title substring> | keys <button>...\n", argv[0]);
    return 2;
}
