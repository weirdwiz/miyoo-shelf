// Play time from Onion's play activity DB (Saves/CurrentProfile/play_activity).
#ifndef SHELF_ACTIVITY_H
#define SHELF_ACTIVITY_H

#include "library.h"

// Fills each game's play_time, play_count and last_played. Returns the number of games
// matched, or -1 when the DB or libsqlite3 is unavailable (fields stay 0).
int activity_load(Library *lib);

#endif
