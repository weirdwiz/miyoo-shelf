// Test the exact action compiled into the legacy keymon backport.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/evdev_keys.h"
static int marker, launches, updates;
static int exists(const char *path)
{
    assert(!strcmp(path, "/mnt/SDCARD/.tmp_update/.runGameSwitcher"));
    return marker;
}
static void set_gameSwitcher(void) { marker = 1; }
static int fake_system(const char *cmd)
{
    assert(marker);
    assert(!strcmp(cmd, "(gameSwitcher --overlay; touch /tmp/state_changed) &"));
    launches++;
    return 0;
}
static void system_state_update(void) { updates++; }
#define system fake_system
#include "../assets/compat/keymon-overlay.h"
int main(void)
{
    assert(evdev_button(57) == BTN_A && evdev_button(29) == BTN_B);
    assert(evdev_button(1) == BTN_MENU && evdev_button(106) == BTN_RIGHT);
    assert(evdev_button(103) == BTN_UP && evdev_button(105) == BTN_LEFT);
    assert(evdev_button(108) == BTN_DOWN && evdev_button(116) == BTN_NONE);
    assert(evdev_button(0) == BTN_NONE);
    action_RA_gameSwitcher();
    assert(marker && launches == 1 && updates == 1);
    action_RA_gameSwitcher();
    assert(launches == 1 && updates == 1);
    puts("keymon: MENU launches overlay without screenshot/termination; duplicate launch blocked");
    return 0;
}
