// The power policy (src/power.c) on its own: dimming, standby, power off and the battery.
#include <stdio.h>

#include "../src/power.h"

static int failures;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static const PowerBattery FULL = {76, 0, 1};

static PowerAction step(PowerState *st, double now, double input, PowerContext ctx, PowerBattery b)
{
    return power_step(st, now, input, ctx, b).action;
}

static void idle_menus(PowerContext ctx)
{
    PowerState st;
    power_init(&st);
    PowerDecision d = power_step(&st, 0, 0, ctx, FULL);
    CHECK(d.action == POWER_NONE && d.next == 30); // the battery comes round first
    d = power_step(&st, 30, 0, ctx, FULL);
    CHECK(d.action == POWER_NONE && d.next == POWER_DIM_SECONDS);
    CHECK(step(&st, 44.9, 0, ctx, FULL) == POWER_NONE);
    CHECK(step(&st, 45, 0, ctx, FULL) == POWER_DIM && st.dimmed);
    d = power_step(&st, 45, 0, ctx, FULL);
    CHECK(d.action == POWER_NONE && d.next == 60);
    CHECK(step(&st, 50, 50, ctx, FULL) == POWER_UNDIM && !st.dimmed); // a press wakes it
    CHECK(step(&st, 50, 50, ctx, FULL) == POWER_NONE);
    CHECK(step(&st, 95, 50, ctx, FULL) == POWER_DIM);
    CHECK(step(&st, 229.9, 50, ctx, FULL) == POWER_NONE);
    CHECK(step(&st, 230, 50, ctx, FULL) == POWER_STANDBY && !st.dimmed);
}

static void idle_gameplay(void)
{
    PowerState st;
    power_init(&st);
    // Never dims; sleeps after ten minutes without a change in the buttons held.
    for (double t = 0; t < POWER_PLAY_STANDBY_SECONDS; t += 15)
        CHECK(step(&st, t, 0, POWER_GAMEPLAY, FULL) == POWER_NONE);
    CHECK(!st.dimmed);
    CHECK(step(&st, 600, 0, POWER_GAMEPLAY, FULL) == POWER_STANDBY);
    CHECK(step(&st, 600, 599, POWER_GAMEPLAY, FULL) == POWER_NONE);

    // The menu dimmed and the game resumed: back to full brightness.
    power_init(&st);
    CHECK(step(&st, 50, 0, POWER_MENU, FULL) == POWER_DIM);
    CHECK(step(&st, 51, 0, POWER_GAMEPLAY, FULL) == POWER_UNDIM);
    CHECK(step(&st, 51, 0, POWER_GAMEPLAY, FULL) == POWER_NONE);
}

static void asleep(void)
{
    PowerState st;
    power_init(&st);
    PowerBattery dead = {1, 0, 1};
    PowerDecision d = power_step(&st, 100, 100, POWER_ASLEEP, dead);
    CHECK(d.action == POWER_NONE && d.next == 100 + POWER_OFF_SECONDS);
    CHECK(power_battery_due(&st, 100)); // no reading counted asleep
    CHECK(step(&st, 699.9, 100, POWER_ASLEEP, dead) == POWER_NONE);
    CHECK(step(&st, 700, 100, POWER_ASLEEP, dead) == POWER_OFF_RESUMABLE);
}

static void battery_polling(void)
{
    PowerState st;
    power_init(&st);
    CHECK(power_battery_due(&st, 0));
    step(&st, 0, 0, POWER_GAMEPLAY, FULL);
    CHECK(!power_battery_due(&st, 29.9) && power_battery_due(&st, 30));
    // A reading handed over between polls is not counted: three low ones in ten seconds
    // are still one.
    power_init(&st);
    PowerBattery low = {3, 0, 1};
    for (double t = 0; t < 10; t += 1) CHECK(step(&st, t, t, POWER_GAMEPLAY, low) == POWER_NONE);
    CHECK(st.low == 1);
}

static void low_battery(void)
{
    PowerState st;
    power_init(&st);
    PowerBattery four = {4, 0, 1}, five = {5, 0, 1}, plugged = {2, 1, 1}, unknown = {-1, 0, 0};
    CHECK(step(&st, 0, 0, POWER_GAMEPLAY, four) == POWER_NONE);
    CHECK(step(&st, 30, 30, POWER_GAMEPLAY, four) == POWER_NONE);
    CHECK(step(&st, 60, 60, POWER_GAMEPLAY, four) == POWER_OFF_RESUMABLE);

    // Not three in a row: a charging, a failed or a higher reading starts the count again.
    PowerBattery breaks[] = {plugged, unknown, five};
    for (int i = 0; i < 3; i++) {
        power_init(&st);
        CHECK(step(&st, 0, 0, POWER_GAMEPLAY, four) == POWER_NONE);
        CHECK(step(&st, 30, 30, POWER_GAMEPLAY, four) == POWER_NONE);
        CHECK(step(&st, 60, 60, POWER_GAMEPLAY, breaks[i]) == POWER_NONE);
        CHECK(step(&st, 90, 90, POWER_GAMEPLAY, four) == POWER_NONE);
        CHECK(step(&st, 120, 120, POWER_GAMEPLAY, four) == POWER_NONE);
        CHECK(step(&st, 150, 150, POWER_GAMEPLAY, four) == POWER_OFF_RESUMABLE);
    }

    // On the home screen too, and before an idle standby that is due at the same time.
    power_init(&st);
    step(&st, 0, 0, POWER_HOME, four);
    step(&st, 30, 0, POWER_HOME, four);
    CHECK(step(&st, 180, 0, POWER_HOME, four) == POWER_OFF_RESUMABLE);
}

static void warning(void)
{
    PowerState st;
    power_init(&st);
    PowerBattery ten = {10, 0, 1}, eleven = {11, 0, 1}, plugged = {10, 1, 1};
    CHECK(step(&st, 0, 0, POWER_HOME, eleven) == POWER_NONE);
    CHECK(step(&st, 30, 30, POWER_HOME, ten) == POWER_LOW_BATTERY_WARNING);
    CHECK(step(&st, 30, 30, POWER_HOME, ten) == POWER_NONE);
    CHECK(step(&st, 60, 60, POWER_HOME, ten) == POWER_NONE); // once
    CHECK(step(&st, 90, 90, POWER_HOME, plugged) == POWER_NONE);
    CHECK(step(&st, 120, 120, POWER_HOME, ten) == POWER_LOW_BATTERY_WARNING); // unplugged again

    // A game can't show it: it waits for the menu.
    power_init(&st);
    CHECK(step(&st, 0, 0, POWER_GAMEPLAY, ten) == POWER_NONE);
    CHECK(step(&st, 10, 0, POWER_GAMEPLAY, ten) == POWER_NONE);
    CHECK(step(&st, 12, 12, POWER_MENU, ten) == POWER_LOW_BATTERY_WARNING);
}

int main(void)
{
    idle_menus(POWER_HOME);
    idle_menus(POWER_MENU);
    idle_gameplay();
    asleep();
    battery_polling();
    low_battery();
    warning();
    if (failures) return 1;
    puts("power: dim, standby, power off, battery polling, warning and low-battery power off behave");
    return 0;
}
