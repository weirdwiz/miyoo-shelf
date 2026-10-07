// Shelf as Onion's game switcher. Onion runs .tmp_update/bin/gameSwitcher, which boot.sh
// bind-mounts to this binary: with --overlay from keymon when MENU is pressed in RetroArch
// (the game is paused, its frame still on screen), and without it from the runtime (no
// game running). This file is the part that talks to Onion and RetroArch; the screen is
// ui.c's switcher view.
#ifndef SHELF_SWITCHER_H
#define SHELF_SWITCHER_H

#include <stdint.h>

#include "gfx.h"
#include "library.h"

// Onion's FNV1A_Pippip_Yurii, which names romScreens/<hash>.png after a recent's rompath.
uint32_t sw_hash(const char *s);
// Host path of the screenshot Onion keeps for a rompath (may not exist).
void sw_screen_path(const char *rompath, char *out, int n);

// Called from background threads when a save finishes or a screenshot loads (wakes an
// idle frame loop).
void sw_set_notify(void (*fn)(void));

// Overlay start: stop play-time tracking, pause RetroArch and check that it's running the
// first recent. If so, save `frame` as that game's screenshot and autosave its state on a
// background thread. Returns 1 when the game is running (and so can be resumed).
int sw_overlay_begin(const Image *frame, const Recent *top);
// Whether the autosave thread is still working; 2 once it reported success, 3 on failure.
enum { SW_SAVE_NONE, SW_SAVE_BUSY, SW_SAVE_DONE, SW_SAVE_FAILED };
int sw_save_state(void);
// Waits for the autosave, unpauses RetroArch and resumes play-time tracking.
void sw_overlay_resume(void);
// Waits for the autosave, then quits RetroArch: TERM, up to 5 s, then KILL.
void sw_quit_game(void);

// Screenshot cards: each recent's romScreen decoded and scaled to w x h on a worker
// thread, plus the selected one at full size for the zoom back into the game. `live`, if
// given, is recents[0]'s screen as it is now (the paused game); the cards take it over.
void sw_cards_start(const Library *lib, const Recent *rec, int w, int h, Image *live);
void sw_cards_stop(void);
// The card image, NULL while loading or when the game has no screenshot.
const Image *sw_card(int i);
// The selected recent's screenshot at full size, NULL until loaded.
const Image *sw_full(int i);
void sw_card_select(int i);
unsigned sw_cards_revision(void);

#endif
