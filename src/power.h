// When Shelf dims, sleeps and powers off by itself. A pure policy: the callers own the
// clock, the input times, the battery reads and the hardware; this only decides. Every
// number lives here.
#ifndef SHELF_POWER_H
#define SHELF_POWER_H

#define POWER_DIM_SECONDS 45.0             // idle on the home screen or in-game menu: dim
#define POWER_MENU_STANDBY_SECONDS 180.0   // idle there this long: standby
#define POWER_PLAY_STANDBY_SECONDS 600.0   // playing with the same buttons held: standby
#define POWER_OFF_SECONDS 600.0            // asleep this long: power off
#define POWER_BATTERY_SECONDS 30.0         // how often the battery is read
#define POWER_WARN_PERCENT 10              // at or below, not charging: one quiet warning
#define POWER_OFF_PERCENT 4                // at or below, not charging: save and power off
#define POWER_OFF_READINGS 3               // ... once this many readings in a row say so

typedef enum {
    POWER_HOME,     // the home screen
    POWER_MENU,     // the paused in-game menu
    POWER_GAMEPLAY, // a game running
    POWER_ASLEEP,   // in standby; "last input" is when it went to sleep
} PowerContext;

typedef struct {
    int percent, charging;
    int valid; // 0: the read failed
} PowerBattery;

typedef enum {
    POWER_NONE,
    POWER_DIM,           // backlight to its lowest
    POWER_UNDIM,         // the player's brightness again
    POWER_STANDBY,       // sleep, as a POWER press would
    POWER_OFF_RESUMABLE, // save and power off; a game resumes on the next boot
    POWER_LOW_BATTERY_WARNING,
} PowerAction;

// What the policy remembers between steps. Zeroed by power_init.
typedef struct {
    int dimmed;           // DIM was answered and no UNDIM or STANDBY since
    int low;              // readings in a row at or below POWER_OFF_PERCENT, not charging
    int warned;           // the warning was given; charging re-arms it
    double read_at;       // when the last reading was counted (< 0: never)
    PowerBattery battery; // that reading
} PowerState;

typedef struct {
    PowerAction action;
    double next; // step again by then at the latest (now when an action was returned)
} PowerDecision;

void power_init(PowerState *st);

// Nonzero when the next step counts a battery reading: read it fresh first.
int power_battery_due(const PowerState *st, double now);

// One decision. `last_input` is the last press (gameplay: the last change in the buttons
// held). `battery` is the newest reading; it is counted once per POWER_BATTERY_SECONDS,
// never while asleep. One action per call, the most urgent first; the caller carries it
// out and steps again until POWER_NONE. STANDBY leaves the panel undimmed: waking up
// restores the brightness.
PowerDecision power_step(PowerState *st, double now, double last_input, PowerContext ctx,
                         PowerBattery battery);

#endif
