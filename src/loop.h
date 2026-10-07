// The frame loop shared by the home screen and the game switcher. While something moves it
// updates, draws and presents every frame; once the screen is settled it sleeps until
// input, a background wakeup, or the next time the screen or its owner needs it.
#ifndef SHELF_LOOP_H
#define SHELF_LOOP_H

#include "gfx.h"
#include "platform.h"

// The platform side (platform.h after platform_init), so tests can run the loop on a
// fake clock with a counting presenter.
typedef struct {
    double (*now)(void);                 // seconds, monotonic
    Button (*poll)(void);                // next press, BTN_NONE once drained
    void (*wait)(double until);          // until input, platform_wake() or `until`
    void (*present)(const Image *frame);
    int paced;                           // present waits for vblank: no frame sleep needed
} LoopIO;

// The screen side. Every callback gets `ud`.
typedef struct {
    void *ud;
    void (*button)(void *ud, Button b);
    // Outside state: status polling, save progress. Returns the absolute time it next
    // wants to run; the loop sleeps no longer than that. NULL: never.
    double (*tick)(void *ud, double now);
    void (*update)(void *ud, float dt);
    // Seconds until the screen changes by itself; 0 when a new frame is due now.
    double (*idle)(void *ud);
    void (*draw)(void *ud, Image *canvas);
    // After each pass: actions such as launching. Nonzero ends the loop and is returned.
    int (*after)(void *ud);
} LoopScreen;

typedef struct {
    unsigned frames;  // draws (each followed by a present)
    unsigned waits;   // sleeps with nothing to draw
} LoopStats;

// Runs until `after` returns nonzero. `stats` may be NULL.
int loop_run(const LoopIO *io, const LoopScreen *screen, Image *canvas, LoopStats *stats);

#endif
