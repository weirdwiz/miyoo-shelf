// Shelf as Onion's game switcher. Onion runs .tmp_update/bin/gameSwitcher, which boot.sh
// bind-mounts to this binary: with --overlay from keymon when MENU is pressed in RetroArch
// (the game is paused, its frame still on screen), and without it from the runtime (no
// game running). This file is the part that talks to Onion and RetroArch; the screen is
// ui.c's switcher view.
#ifndef SHELF_SWITCHER_H
#define SHELF_SWITCHER_H

#include <stdint.h>
#include <sys/types.h>

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
// background thread. Legacy RetroArch saves only on clean exit, never to a user slot.
// Returns 1 when the game is running (and so can be resumed).
int sw_overlay_begin(const Image *frame, const Recent *top);
// Save worker status; legacy RA reports ON_EXIT after its screenshot worker finishes.
enum { SW_SAVE_NONE, SW_SAVE_BUSY, SW_SAVE_DONE, SW_SAVE_FAILED, SW_SAVE_ON_EXIT };
// Cached protocol choice, also usable by the crash handler without locks or I/O.
const char *sw_resume_command(void);
// Renderer held after saving; crash recovery must SIGCONT this PID before unpausing.
pid_t sw_overlay_pid(void);
// No overlay frame may be presented until its renderer is exclusively held.
int sw_overlay_ready(void);
// Process-control seam for overlay tests; NULL restores real /proc discovery + kill.
void sw_set_process_hooks(pid_t (*pid_fn)(void), int (*signal_fn)(pid_t, int));
// SW_SAVE_FAILED: no answer within the timeout (0 restores 60 s); the renderer is held
// anyway so the switcher can show it, and the game's own exit autosave still runs on quit.
void sw_set_save_timeout(int ms);
int sw_save_state(void);
// Waits for the autosave, unpauses RetroArch and resumes play-time tracking.
void sw_overlay_resume(void);
// Waits for the autosave, then quits RetroArch. Legacy RA is never killed mid-exit save.
void sw_quit_game(void);

// Screenshot cards: each recent's romScreen decoded and scaled to w x h on a worker
// thread, plus the selected one at full size for the zoom back into the game. `live`, if
// given, is recents[0]'s screen as it is now (the paused game); the cards take it over.
void sw_cards_start(const Library *lib, const Recent *rec, int w, int h, Image *live);
void sw_cards_stop(void);
// Images are borrowed and only used on the main thread, which is also the only caller
// of sw_card_select. The worker never replaces or frees a card once published, so a
// card stays valid (and its address identifies it) until sw_cards_stop. A full-size
// image may be evicted once it is no longer the selection: use it only until the next
// sw_card_select or sw_cards_stop.
// The card image, NULL while loading or when the game has no screenshot.
const Image *sw_card(int i);
// The selected recent's screenshot at full size, NULL until loaded.
const Image *sw_full(int i);
void sw_card_select(int i);
unsigned sw_cards_revision(void);

#endif
