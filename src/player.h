// Runs one libretro core and game inside Shelf (no RetroArch). libretro callbacks are
// process-global, so there is at most one open player at a time.
//
// Threads: everything except player_audio_read runs on the main (game) thread.
// player_audio_read may run on an audio thread concurrently with player_run.
#ifndef SHELF_PLAYER_H
#define SHELF_PLAYER_H

#include <stdint.h>

// A core option, by the key and value the core declares (`gambatte_mix_frames`, `disabled`).
typedef struct {
    const char *key;
    const char *value;
} PlayerOption;

typedef struct {
    const char *core_path;    // e.g. /mnt/SDCARD/.shelf/cores/mgba_libretro.so
    const char *rom_path;
    const char *system_dir;   // BIOS files (RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY)
    const char *save_dir;     // <stem>.srm / <stem>.rtc live here; created if missing
    const char *stem;         // ROM file name without extension
    const char *options_path; // RetroArch-style `key = "value"` lines; NULL or missing: core defaults
    // Shelf's own defaults, over the core's. The options file overrides any of them. A value
    // the core doesn't allow is ignored and the core's default stays.
    const PlayerOption *defaults;
    int ndefaults;
    // The player's choices in Shelf (colour correction, palette): over the options file
    // too, for the keys they set. The same strength as player_set_option.
    const PlayerOption *choices;
    int nchoices;
    int sample_rate;          // output rate player_audio_read produces, e.g. 48000
    int buffer_ms;            // audio kept buffered ahead of the reader; 0: 32 ms
} PlayerConfig;

// The latest video frame, converted to XRGB8888. Valid until the next player_run or
// player_close. A core that dupes a frame leaves the previous one in place.
typedef struct {
    const uint32_t *px;
    int w, h;          // current size (may change with RETRO_ENVIRONMENT_SET_GEOMETRY)
    int pitch;         // in pixels
    float aspect;      // display aspect ratio (w/h when the core reports none)
    unsigned serial;   // increments whenever a new (non-duped) frame arrives
} PlayerFrame;

// Joypad bits for player_set_input: 1u << RETRO_DEVICE_ID_JOYPAD_*.
enum {
    PLAYER_B = 1u << 0, PLAYER_Y = 1u << 1, PLAYER_SELECT = 1u << 2, PLAYER_START = 1u << 3,
    PLAYER_UP = 1u << 4, PLAYER_DOWN = 1u << 5, PLAYER_LEFT = 1u << 6, PLAYER_RIGHT = 1u << 7,
    PLAYER_A = 1u << 8, PLAYER_X = 1u << 9, PLAYER_L = 1u << 10, PLAYER_R = 1u << 11,
    PLAYER_L2 = 1u << 12, PLAYER_R2 = 1u << 13,
};

// Loads the core, the game and its SRAM/RTC (not a save state). Returns 0, or -1 with a
// one-line reason in `err` (core missing/invalid, ROM unreadable, core refused the game,
// another player already open). Failure leaves nothing loaded.
int player_open(const PlayerConfig *cfg, char *err, int errlen);
int player_is_open(void);
// Saves SRAM/RTC when changed, unloads the game and the core (dlclose).
void player_close(void);

void player_set_input(unsigned mask);
// Runs exactly one emulated frame (retro_run).
void player_run(void);
const PlayerFrame *player_frame(void);
// Changes a core option while the game runs, over Shelf's defaults and the options file.
// The core is told before its next frame (RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE) and
// re-reads the options it can change live. Returns 1 when the value changed, 0 when it
// already had it, -1 when the core has no such option or doesn't allow the value.
int player_set_option(const char *key, const char *value);
// Runs `frames` frames (1..8) with the buttons last set, keeps the last frame they drew in
// player_frame(), and puts the game back exactly where it was: the core's state, SRAM and
// RTC. The frames make no sound; what was buffered before is left as it was. For showing a
// paused game with an option just changed. Returns 0; -1 (nothing run) when the game can't
// be snapshotted; PLAYER_PEEK_LOST when the core couldn't take its state back. Then the game
// is ahead of where it was paused and may not match its saves, so from then on the player
// refuses to save anything of it (player_save_state, player_save_sram, and player_close
// writes nothing) or to peek again: end the session.
int player_peek(int frames);
#define PLAYER_PEEK_LOST (-2)
double player_fps(void);  // the core's frames per second (av_info timing)
void player_reset(void);

// Save states: the core's raw serialization. Writes are atomic (temp file, fsync,
// rename). Return 0 on success, -1 on failure (the old file is left intact).
int player_save_state(const char *path);
int player_load_state(const char *path);
// Writes SRAM (and RTC when the core has one) when it changed since the last write.
// Atomic like save states. Returns 0 when saved or unchanged, -1 on a write error (or after
// PLAYER_PEEK_LOST).
int player_save_sram(void);

// Resampled interleaved stereo at cfg->sample_rate. Fills `frames` frames, padding with
// silence on underrun; returns how many were real audio. After opening or a flush it
// reads nothing until cfg->buffer_ms of sound is buffered, so playback starts with the
// cushion it keeps instead of stuttering up to it. Safe on another thread.
int player_audio_read(int16_t *out, int frames);
// Discards buffered audio (after a pause, so resuming doesn't play stale sound).
void player_audio_flush(void);
// Buffered output frames not yet read (for diagnostics and pacing tests).
int player_audio_queued(void);

#endif
