// Runs one libretro core and game inside Shelf (no RetroArch). libretro callbacks are
// process-global, so there is at most one open player at a time.
//
// Threads: everything except player_audio_read runs on the main (game) thread.
// player_audio_read may run on an audio thread concurrently with player_run.
#ifndef SHELF_PLAYER_H
#define SHELF_PLAYER_H

#include <stdint.h>

typedef struct {
    const char *core_path;    // e.g. /mnt/SDCARD/.shelf/cores/mgba_libretro.so
    const char *rom_path;
    const char *system_dir;   // BIOS files (RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY)
    const char *save_dir;     // <stem>.srm / <stem>.rtc live here; created if missing
    const char *stem;         // ROM file name without extension
    const char *options_path; // RetroArch-style `key = "value"` lines; NULL or missing: core defaults
    int sample_rate;          // output rate player_audio_read produces, e.g. 48000
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
double player_fps(void);  // the core's frames per second (av_info timing)
void player_reset(void);

// Save states: the core's raw serialization. Writes are atomic (temp file, fsync,
// rename). Return 0 on success, -1 on failure (the old file is left intact).
int player_save_state(const char *path);
int player_load_state(const char *path);
// Writes SRAM (and RTC when the core has one) when it changed since the last write.
// Atomic like save states. Returns 0 when saved or unchanged, -1 on a write error.
int player_save_sram(void);

// Resampled interleaved stereo at cfg->sample_rate. Fills `frames` frames, padding with
// silence on underrun; returns how many were real audio. Safe on another thread.
int player_audio_read(int16_t *out, int frames);
// Discards buffered audio (after a pause, so resuming doesn't play stale sound).
void player_audio_flush(void);
// Buffered output frames not yet read (for diagnostics and pacing tests).
int player_audio_queued(void);

#endif
