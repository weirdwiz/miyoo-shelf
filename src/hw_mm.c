// Miyoo Mini / Mini Plus hardware settings: backlight PWM, MI_AO volume, battery,
// standby (panel, backlight, mute, powersave), CPU governor and power off. See
// docs/hardware.md.
//
// Every path goes through $SHELF_HW_ROOT (default: the real root), and a missing file or
// device is a silent no-op: the VM and the host test have none of this hardware.
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "platform.h"

#ifdef __linux__
#include <linux/i2c-dev.h>
#include <linux/i2c.h>
#include <linux/reboot.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#endif

#define HW_PATH_MAX 256

#define PWM_CHIP "/sys/class/pwm/pwmchip0"
#define PWM_EXPORT PWM_CHIP "/export"
#define PWM_PERIOD PWM_CHIP "/pwm0/period"
#define PWM_DUTY PWM_CHIP "/pwm0/duty_cycle"
#define PWM_ENABLE PWM_CHIP "/pwm0/enable"

#define GPIO_EXPORT "/sys/class/gpio/export"
#define PANEL_GPIO 4
#define CHARGE_GPIO 59
#define PANEL_DIR "/sys/class/gpio/gpio4/direction"
#define PANEL_VALUE "/sys/class/gpio/gpio4/value"
#define CHARGE_DIR "/sys/class/gpio/gpio59/direction"
#define CHARGE_VALUE "/sys/class/gpio/gpio59/value"

#define GOVERNOR "/sys/devices/system/cpu/cpufreq/policy0/scaling_governor"

#define MI_AO_DEVICE "/dev/mi_ao"
#define MI_AO_SETVOLUME 0x4008690bUL
#define MI_AO_SETMUTE 0x4008690dUL

#define AXP_TEST "/customer/app/axp_test"
#define AXP_ADDR 0x34
#define AXP_STATUS_REG 0x00
#define AXP_CAPACITY_REG 0xB9

#ifdef __linux__
#define SAR_DEVICE "/dev/sar"
#define SAR_INIT _IO('a', 0)
#define SAR_READ _IO('a', 1)
#endif

#define BATTERY_CACHE_MS 10000.0

// ---------------------------------------------------------------- path plumbing

// The hardware root, read once. Trailing slashes are dropped so paths stay single-slash.
static const char *hw_base(void)
{
    static char root[HW_PATH_MAX];
    static int ready;
    if (!ready) {
        const char *env = getenv("SHELF_HW_ROOT");
        if (env) snprintf(root, sizeof root, "%s", env);
        size_t n = strlen(root);
        while (n > 0 && root[n - 1] == '/') root[--n] = 0;
        ready = 1;
    }
    return root;
}

static int hw_path(char *out, size_t n, const char *rel)
{
    int len = snprintf(out, n, "%s%s", hw_base(), rel);
    return len > 0 && (size_t)len < n;
}

// Writes a sysfs-style value; O_TRUNC matches what `echo x > file` does.
static void hw_write(const char *rel, const char *value)
{
    char path[HW_PATH_MAX];
    if (!hw_path(path, sizeof path, rel)) return;
    int fd = open(path, O_WRONLY | O_TRUNC | O_CLOEXEC);
    if (fd < 0) return;
    write(fd, value, strlen(value));
    close(fd);
}

// Reads up to n-1 bytes and NUL-terminates; returns the byte count, -1 on failure.
static int hw_read(const char *rel, char *buf, size_t n)
{
    char path[HW_PATH_MAX];
    if (n == 0 || !hw_path(path, sizeof path, rel)) return -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t got = read(fd, buf, n - 1);
    close(fd);
    if (got < 0) return -1;
    buf[got] = 0;
    return (int)got;
}

// ---------------------------------------------------------------- pure mappings

// Not static: tools/test_hw_mm.c links this file separately and checks the mappings.
int hw_duty(int level)
{
    if (level < 0) level = 0;
    if (level > 10) level = 10;
    return (int)lround(3.0 * exp(0.350656 * level));
}

int hw_volume_db(int level)
{
    if (level <= 0) return -60;
    if (level > 20) level = 20;
    return (int)lround(48.0 * log10(1.0 + level)) - 60;
}

int hw_volume_muted(int level) { return level <= 0; }

// Onion's measured Mini Li-ion curve and ADC conversion: https://github.com/OnionUI/Onion/blob/main/src/batmon/batmon.c
static const struct { double v, pct; } battery_curve[] = {
    {4.20,100},{4.13,95},{4.06,90},{4.02,85},{3.99,80},{3.95,75},
    {3.92,70},{3.88,65},{3.85,60},{3.81,55},{3.78,50},{3.74,45},
    {3.71,40},{3.67,35},{3.64,30},{3.56,25},{3.49,20},{3.42,15},
    {3.30,10},{3.14,5},{3.00,0}
};

int hw_adc_percent(int adc)
{
    double v = (adc - 148) * 0.01;
    if (v >= battery_curve[0].v) return 100;
    for (size_t i = 0; i + 1 < sizeof battery_curve / sizeof battery_curve[0]; i++) {
        if (v <= battery_curve[i].v && v > battery_curve[i + 1].v) {
            double pct = battery_curve[i + 1].pct +
                (v - battery_curve[i + 1].v) * (battery_curve[i].pct - battery_curve[i + 1].pct) /
                (battery_curve[i].v - battery_curve[i + 1].v);
            return (int)pct;
        }
    }
    return 0;
}

// The percentage shown on a Mini. The ADC wobbles by about 80 mV under load (Onion PR
// #1712), so readings are averaged, but only over the last BATTERY_WINDOW seconds: callers
// poll every 30 s or so, and a low-battery shutdown can't wait minutes for an average to
// catch up. While discharging the figure never goes up on a brief rebound; it does once the
// average has stayed above it for BATTERY_RISE readings in a row (a sag under load that
// has passed). Plugging in or out starts afresh. `now` is in seconds.
#define BATTERY_SAMPLES 10
#define BATTERY_WINDOW 90.0
#define BATTERY_RISE 3
static struct {
    int adc[BATTERY_SAMPLES];
    double at[BATTERY_SAMPLES];
    int n, next, shown, above, charging;
} bat = { .shown = -1 };

void hw_battery_reset(void)
{
    memset(&bat, 0, sizeof bat);
    bat.shown = -1;
}

int hw_mini_percent(int adc, int charging, double now)
{
    charging = charging != 0;
    if (charging != bat.charging) {
        hw_battery_reset();
        bat.charging = charging;
    }
    bat.adc[bat.next] = adc;
    bat.at[bat.next] = now;
    bat.next = (bat.next + 1) % BATTERY_SAMPLES;
    if (bat.n < BATTERY_SAMPLES) bat.n++;
    int sum = 0, used = 0;
    for (int i = 0; i < bat.n; i++)
        if (now - bat.at[i] <= BATTERY_WINDOW) {
            sum += bat.adc[i];
            used++;
        }
    int pct = hw_adc_percent((sum + used / 2) / used); // used >= 1: this sample is in it
    if (charging || bat.shown < 0 || pct < bat.shown) {
        bat.shown = pct;
        bat.above = 0;
    } else if (pct > bat.shown && ++bat.above >= BATTERY_RISE) {
        bat.shown = pct;
        bat.above = 0;
    } else if (pct == bat.shown) {
        bat.above = 0;
    }
    return bat.shown;
}

// One "key":<int> field of axp_test's single-line JSON.
static int json_field(const char *json, const char *key, int *out)
{
    char pat[16];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return 0;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return 0;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    char *end;
    long v = strtol(p, &end, 10);
    if (end == p) return 0;
    *out = (int)v;
    return 1;
}

// Percent from axp_test's output, -1 when it has no usable "battery" field.
int hw_parse_axp(const char *json, int *charging)
{
    int pct, ch = 0;
    if (!json || !json_field(json, "battery", &pct)) return -1;
    json_field(json, "charging", &ch);
    if (charging) *charging = ch == 3;
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

// ---------------------------------------------------------------- backlight

static int brightness = 5;

static void pwm_duty(int duty)
{
    static int exported;
    char value[16];
    if (!exported) {
        // Channel 0 only. Export fails with EBUSY when it already is, which is fine.
        hw_write(PWM_EXPORT, "0");
        hw_write(PWM_PERIOD, "800");
        exported = 1;
    }
    snprintf(value, sizeof value, "%d", duty);
    hw_write(PWM_DUTY, value);
}

static void pwm_enable(int on) { hw_write(PWM_ENABLE, on ? "1" : "0"); }

void platform_set_brightness(int level)
{
    if (level < 0) level = 0;
    if (level > 10) level = 10;
    brightness = level;
    pwm_duty(hw_duty(level));
    pwm_enable(1);
}

// ---------------------------------------------------------------- panel power

static void panel_power(int on)
{
    static int ready;
    if (!ready) {
        char pin[8];
        snprintf(pin, sizeof pin, "%d", PANEL_GPIO);
        hw_write(GPIO_EXPORT, pin);
        hw_write(PANEL_DIR, "out");
        ready = 1;
    }
    hw_write(PANEL_VALUE, on ? "1" : "0");
}

// ---------------------------------------------------------------- audio volume

// One MI_AO ioctl, whose argument is {sizeof(int[2]), pointer to int[2]{0, value}}.
static void mi_ao_call(unsigned long cmd, int *args)
{
#ifdef __linux__
    char path[HW_PATH_MAX];
    uint64_t arg[2] = { sizeof(int[2]), (uintptr_t)args };
    if (!hw_path(path, sizeof path, MI_AO_DEVICE)) return;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    ioctl(fd, cmd, arg);
    close(fd);
#else
    (void)cmd;
    (void)args;
#endif
}

static void volume_mute(int on)
{
    int mute[2] = { 0, on };
    mi_ao_call(MI_AO_SETMUTE, mute);
}

static void volume_apply(int level)
{
    int vol[2] = { 0, hw_volume_db(level) };
    int mute[2] = { 0, hw_volume_muted(level) };
    mi_ao_call(MI_AO_SETVOLUME, vol);
    mi_ao_call(MI_AO_SETMUTE, mute);
}

static int volume = 12;

void platform_set_volume(int level)
{
    if (level < 0) level = 0;
    if (level > 20) level = 20;
    volume = level;
    volume_apply(level);
}

int platform_volume(void) { return volume; }

// ---------------------------------------------------------------- CPU governor

static char boot_governor[32];
static int boot_governor_read;

static int governor_read(char *out, size_t n)
{
    char buf[64];
    if (hw_read(GOVERNOR, buf, sizeof buf) <= 0) return 0;
    buf[strcspn(buf, "\r\n")] = 0;
    if (!buf[0]) return 0;
    snprintf(out, n, "%s", buf);
    return 1;
}

static void governor_write(const char *name) { hw_write(GOVERNOR, name); }

// The boot governor, so menus can be restored to it even after a standby happened first.
static void governor_remember_boot(void)
{
    if (!boot_governor_read) boot_governor_read = governor_read(boot_governor, sizeof boot_governor);
}

void platform_performance(int game)
{
    governor_remember_boot();
    if (!boot_governor_read) return; // no cpufreq here
    governor_write(game ? "performance" : "ondemand");
}

// ---------------------------------------------------------------- battery

// Mini Plus: AXP223 register read over I2C. Returns the byte, -1 on failure.
static int axp_read(int reg)
{
#ifdef __linux__
    char path[HW_PATH_MAX];
    if (!hw_path(path, sizeof path, "/dev/i2c-1")) return -1;
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return -1;
    uint8_t addr = (uint8_t)reg, value = 0;
    struct i2c_msg msgs[2] = {
        { .addr = AXP_ADDR, .flags = 0, .len = 1, .buf = &addr },
        { .addr = AXP_ADDR, .flags = I2C_M_RD, .len = 1, .buf = &value },
    };
    struct i2c_rdwr_ioctl_data data = { .msgs = msgs, .nmsgs = 2 };
    int ok = ioctl(fd, I2C_RDWR, &data) == 2;
    close(fd);
    return ok ? value : -1;
#else
    (void)reg;
    return -1;
#endif
}

static int axp_test(int *charging)
{
    char path[HW_PATH_MAX], cmd[HW_PATH_MAX + 32];
    if (!hw_path(path, sizeof path, AXP_TEST) || access(path, X_OK) != 0) return -1;
    snprintf(cmd, sizeof cmd, "'%s' 2>/dev/null", path);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    char buf[256];
    size_t got = fread(buf, 1, sizeof buf - 1, f);
    buf[got] = 0;
    pclose(f);
    return hw_parse_axp(buf, charging);
}

static int battery_plus(int *charging)
{
    int status = axp_read(AXP_STATUS_REG);
    int capacity = status >= 0 ? axp_read(AXP_CAPACITY_REG) : -1;
    if (capacity >= 0) {
        *charging = (status >> 2) & 1;
        return capacity & 0x7f;
    }
    return axp_test(charging);
}

// Nonzero on a Mini Plus; detected once by a successful AXP223 register 0 read.
static int model_plus(void)
{
    static int plus = -1;
    if (plus < 0) plus = axp_read(AXP_STATUS_REG) >= 0;
    return plus;
}

#ifdef __linux__
static int charge_gpio(void)
{
    static int ready;
    char value[8];
    if (!ready) {
        char pin[8];
        snprintf(pin, sizeof pin, "%d", CHARGE_GPIO);
        hw_write(GPIO_EXPORT, pin);
        hw_write(CHARGE_DIR, "in");
        ready = 1;
    }
    if (hw_read(CHARGE_VALUE, value, sizeof value) < 0) return 0;
    return atoi(value) != 0;
}
#endif

static int battery_mini(int *charging)
{
#ifdef __linux__
    static int fd = -2; // -2 until opened, -1 when the device is absent
    if (fd == -2) {
        char path[HW_PATH_MAX];
        fd = hw_path(path, sizeof path, SAR_DEVICE) ? open(path, O_RDWR | O_CLOEXEC) : -1;
        if (fd >= 0 && ioctl(fd, SAR_INIT) < 0) {
            close(fd);
            fd = -1;
        }
    }
    if (fd >= 0) {
        struct {
            int channel, adc;
        } sample = { 0, 0 };
        if (ioctl(fd, SAR_READ, &sample) == 0) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            *charging = charge_gpio();
            return hw_mini_percent(sample.adc, *charging, (double)ts.tv_sec + ts.tv_nsec / 1e9);
        }
    }
#else
    (void)charging;
#endif
    return -1;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int platform_battery(int *charging)
{
    static int percent = -1, was_charging, cached;
    static double cached_at;
    double now = now_ms();
    if (cached && now - cached_at < BATTERY_CACHE_MS) {
        if (charging) *charging = was_charging;
        return percent;
    }
    int ch = 0;
    int pct = model_plus() ? battery_plus(&ch) : battery_mini(&ch);
    if (pct < 0) pct = -1;
    else if (pct > 100) pct = 100;
    percent = pct;
    was_charging = ch;
    cached_at = now;
    cached = 1;
    if (charging) *charging = ch;
    return percent;
}

// ---------------------------------------------------------------- standby

// The governor a standby replaced, so leaving it restores what was running.
static char standby_governor[32];
static int standby_governor_valid;
static int standby_active;

void platform_standby(int on)
{
    if (on) {
        if (standby_active) return;
        standby_active = 1;
        pwm_duty(0);
        panel_power(0);
        volume_mute(1);
        standby_governor_valid = governor_read(standby_governor, sizeof standby_governor);
        if (standby_governor_valid) {
            governor_remember_boot();
            governor_write("powersave");
        }
    } else {
        if (!standby_active) return;
        standby_active = 0;
        panel_power(1);
        pwm_enable(0); // the panel needs the PWM restarted, not just its duty restored
        pwm_enable(1);
        pwm_duty(hw_duty(brightness));
        platform_set_volume(volume);
        if (standby_governor_valid) governor_write(standby_governor);
    }
}

// ---------------------------------------------------------------- power off

void platform_power_off(void)
{
    sync();
    const char *skip = getenv("SHELF_NO_POWEROFF");
    if (skip && strcmp(skip, "1") == 0) exit(0);
#ifdef __linux__
    reboot(RB_POWER_OFF);
#endif
    execl("/sbin/poweroff", "poweroff", (char *)NULL);
}
