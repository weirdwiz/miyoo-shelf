// Host/VM test for src/hw_mm.c: every sysfs/device path is pointed at a throwaway tree via
// SHELF_HW_ROOT, so no Miyoo hardware (and no Linux ioctl) is involved. The mappings that
// cannot be observed through files are pure functions and are called directly.
#include "../src/platform.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

// Internal to hw_mm.c, non-static so this separately compiled test can link them.
int hw_duty(int level);
int hw_volume_db(int level);
int hw_volume_muted(int level);
int hw_adc_percent(int adc);
int hw_parse_axp(const char *json, int *charging);

static char root[512];
static int failures;

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void host_path(const char *rel, char *out, size_t n) { snprintf(out, n, "%s/%s", root, rel); }

// mkdir -p for a host path.
static void mkdirs(const char *path)
{
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *c = buf + 1;; c++) {
        if (*c && *c != '/') continue;
        char keep = *c;
        *c = 0;
        mkdir(buf, 0700);
        *c = keep;
        if (!keep) return;
    }
}

// Creates an empty tree-relative file, with its folders.
static void touch(const char *rel)
{
    char path[1024], dir[1024];
    host_path(rel, path, sizeof path);
    snprintf(dir, sizeof dir, "%s", path);
    *strrchr(dir, '/') = 0;
    mkdirs(dir);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    CHECK(fd >= 0);
    if (fd >= 0) close(fd);
}

static void put(const char *rel, const char *text)
{
    touch(rel);
    char path[1024];
    host_path(rel, path, sizeof path);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (!f) return;
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static void check_file(const char *rel, const char *want)
{
    char path[1024], buf[128] = "";
    host_path(rel, path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "FAIL: %s is missing, want \"%s\"\n", rel, want);
        failures++;
        return;
    }
    size_t got = fread(buf, 1, sizeof buf - 1, f);
    buf[got] = 0;
    fclose(f);
    if (strcmp(buf, want)) {
        fprintf(stderr, "FAIL: %s = \"%s\", want \"%s\"\n", rel, buf, want);
        failures++;
    }
}

static void make_tree(void)
{
    put("sys/class/pwm/pwmchip0/export", "");
    put("sys/class/pwm/pwmchip0/pwm0/period", "");
    put("sys/class/pwm/pwmchip0/pwm0/duty_cycle", "");
    put("sys/class/pwm/pwmchip0/pwm0/enable", "");
    put("sys/class/gpio/export", "");
    put("sys/class/gpio/gpio4/direction", "");
    put("sys/class/gpio/gpio4/value", "1");
    put("sys/class/gpio/gpio59/direction", "");
    put("sys/class/gpio/gpio59/value", "0");
    put("sys/devices/system/cpu/cpufreq/policy0/scaling_governor", "ondemand");
    char dev[1024];
    host_path("dev", dev, sizeof dev);
    mkdirs(dev); // the empty dev/ every device path resolves under, so all are absent
}

static void test_volume_state(void)
{
    CHECK(platform_volume() == 12); // default
    platform_set_volume(20);
    CHECK(platform_volume() == 20);
    platform_set_volume(99);
    CHECK(platform_volume() == 20);
    platform_set_volume(-3);
    CHECK(platform_volume() == 0);
    platform_set_volume(12);
}

static void test_brightness(void)
{
    platform_set_brightness(0);
    check_file("sys/class/pwm/pwmchip0/export", "0");
    check_file("sys/class/pwm/pwmchip0/pwm0/period", "800");
    check_file("sys/class/pwm/pwmchip0/pwm0/duty_cycle", "3");
    check_file("sys/class/pwm/pwmchip0/pwm0/enable", "1");

    platform_set_brightness(5);
    check_file("sys/class/pwm/pwmchip0/pwm0/duty_cycle", "17");

    platform_set_brightness(10);
    check_file("sys/class/pwm/pwmchip0/pwm0/duty_cycle", "100");
    check_file("sys/class/pwm/pwmchip0/pwm0/enable", "1");
}

static void test_standby(void)
{
    platform_standby(1);
    check_file("sys/class/pwm/pwmchip0/pwm0/duty_cycle", "0");
    check_file("sys/class/gpio/gpio4/value", "0");
    check_file("sys/devices/system/cpu/cpufreq/policy0/scaling_governor", "powersave");

    platform_standby(0);
    check_file("sys/class/gpio/gpio4/value", "1");
    check_file("sys/class/pwm/pwmchip0/pwm0/enable", "1");
    check_file("sys/class/pwm/pwmchip0/pwm0/duty_cycle", "100");
    check_file("sys/devices/system/cpu/cpufreq/policy0/scaling_governor", "ondemand");
}

static void test_performance(void)
{
    platform_performance(1);
    check_file("sys/devices/system/cpu/cpufreq/policy0/scaling_governor", "performance");
    platform_performance(0);
    check_file("sys/devices/system/cpu/cpufreq/policy0/scaling_governor", "ondemand");
}

static void test_battery_absent(void)
{
    int charging = 1, fresh = platform_battery(&charging);
    CHECK(fresh == -1);
    CHECK(charging == 0);
}

static void test_mappings(void)
{
    CHECK(hw_duty(0) == 3);
    CHECK(hw_duty(5) == 17);
    CHECK(hw_duty(10) == 100);

    // 0 mutes; 20 is 48*log10(21)-60 = +3 dB, the top of the scale.
    CHECK(hw_volume_muted(0) == 1);
    CHECK(hw_volume_muted(1) == 0);
    CHECK(hw_volume_db(0) == -60);
    CHECK(hw_volume_db(20) == 3);
    CHECK(hw_volume_db(19) == 2);
    CHECK(hw_volume_db(1) == -46);

    CHECK(hw_adc_percent(578) == 100);
    CHECK(hw_adc_percent(600) == 100);
    CHECK(hw_adc_percent(540) == 62);
    CHECK(hw_adc_percent(520) == 37);
    CHECK(hw_adc_percent(490) == 9);
    CHECK(hw_adc_percent(470) == 0);

    int charging = 7;
    CHECK(hw_parse_axp("{\"battery\":73, \"voltage\":3900, \"charging\":1}", &charging) == 73);
    CHECK(charging == 1);
    CHECK(hw_parse_axp("{\"battery\":4, \"voltage\":3300, \"charging\":0}", &charging) == 4);
    CHECK(charging == 0);
    CHECK(hw_parse_axp("{\"voltage\":3300}", &charging) == -1);
    CHECK(hw_parse_axp("not json at all", &charging) == -1);
    CHECK(hw_parse_axp("{\"battery\":", &charging) == -1);
    CHECK(hw_parse_axp("\"battery\":200", &charging) == 100); // clamped
}

static void test_power_off(void)
{
    fflush(NULL); // keep the child from repeating buffered output
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0) {
        setenv("SHELF_NO_POWEROFF", "1", 1);
        platform_power_off();
        _exit(99); // only reached when power off did not take the process down
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}

// No SHELF_HW_ROOT: this machine has none of the Miyoo's files or devices. Nothing may
// crash, print or hang; the battery is simply unknown.
static int real_root(void)
{
    int charging = 1;
    platform_set_brightness(5);
    platform_set_volume(12);
    CHECK(platform_volume() == 12);
    CHECK(platform_battery(&charging) == -1);
    CHECK(charging == 0);
    platform_standby(1);
    platform_standby(0);
    platform_performance(1);
    platform_performance(0);
    if (failures) return 1;
    printf("PASS real root: no hardware, no crash, battery -1\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--real") == 0) return real_root();

    snprintf(root, sizeof root, "/tmp/shelf-hw-test-XXXXXX");
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 1;
    }
    setenv("SHELF_HW_ROOT", root, 1); // read once, on the first hardware call
    make_tree();

    test_volume_state();
    test_brightness();
    test_standby();
    test_performance();
    test_battery_absent();
    test_mappings();
    test_power_off();

    if (failures) {
        printf("FAIL test-hw-mm: %d check(s) failed\n", failures);
        return 1;
    }
    printf("PASS test-hw-mm: brightness, standby, governor, battery, mappings, power off\n");
    return 0;
}
