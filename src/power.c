#include "power.h"

#include <string.h>

void power_init(PowerState *st)
{
    memset(st, 0, sizeof *st);
    st->read_at = -1;
}

int power_battery_due(const PowerState *st, double now)
{
    return st->read_at < 0 || now >= st->read_at + POWER_BATTERY_SECONDS;
}

static void count_reading(PowerState *st, double now, PowerBattery b)
{
    st->read_at = now;
    st->battery = b;
    if (!b.valid || b.charging) {
        st->low = 0; // an unknown reading never powers off
        if (b.valid) st->warned = 0;
        return;
    }
    st->low = b.percent <= POWER_OFF_PERCENT ? st->low + 1 : 0;
}

static PowerDecision act(PowerAction a, double now) { return (PowerDecision){a, now}; }

PowerDecision power_step(PowerState *st, double now, double last_input, PowerContext ctx,
                         PowerBattery battery)
{
    if (ctx == POWER_ASLEEP) {
        st->dimmed = 0;
        double off = last_input + POWER_OFF_SECONDS;
        return now >= off ? act(POWER_OFF_RESUMABLE, now) : (PowerDecision){POWER_NONE, off};
    }
    if (power_battery_due(st, now)) count_reading(st, now, battery);
    if (st->low >= POWER_OFF_READINGS) return act(POWER_OFF_RESUMABLE, now);

    int menu = ctx != POWER_GAMEPLAY; // the home screen or the in-game menu
    double standby = last_input + (menu ? POWER_MENU_STANDBY_SECONDS : POWER_PLAY_STANDBY_SECONDS);
    if (now >= standby) {
        st->dimmed = 0;
        return act(POWER_STANDBY, now);
    }
    double dim = last_input + POWER_DIM_SECONDS;
    if (st->dimmed && (!menu || now < dim)) {
        st->dimmed = 0;
        return act(POWER_UNDIM, now);
    }
    if (menu && !st->dimmed && now >= dim) {
        st->dimmed = 1;
        return act(POWER_DIM, now);
    }
    // Only where a message can be seen; a game keeps it until its menu opens.
    const PowerBattery *b = &st->battery;
    if (menu && !st->warned && b->valid && !b->charging && b->percent <= POWER_WARN_PERCENT) {
        st->warned = 1;
        return act(POWER_LOW_BATTERY_WARNING, now);
    }

    double next = st->read_at + POWER_BATTERY_SECONDS;
    if (standby < next) next = standby;
    if (menu && !st->dimmed && dim < next) next = dim;
    return (PowerDecision){POWER_NONE, next};
}
