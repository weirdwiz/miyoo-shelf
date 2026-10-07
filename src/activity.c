#include "activity.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Onion ships libsqlite3 in .tmp_update/lib, which isn't on the runtime's library path,
// and macOS has its own; load whichever exists rather than linking against either.
static const char *const SQLITE_LIBS[] = {
    "libsqlite3.so.0",
    "/mnt/SDCARD/.tmp_update/lib/libsqlite3.so.0",
    "libsqlite3.dylib",
};
#define SQLITE_OPEN_READONLY 1
#define SQLITE_ROW 100

typedef struct {
    int (*open_v2)(const char *, void **, int, const char *);
    int (*prepare_v2)(void *, const char *, int, void **, const char **);
    int (*step)(void *);
    const unsigned char *(*column_text)(void *, int);
    long long (*column_int64)(void *, int);
    int (*finalize)(void *);
    int (*close)(void *);
} Sqlite;

static const Library *sort_lib;
static int cmp_path(const void *a, const void *b)
{
    return strcmp(sort_lib->games[*(const int *)a].path, sort_lib->games[*(const int *)b].path);
}

static int find_game(const Library *lib, const int *by_path, const char *path)
{
    int lo = 0, hi = lib->ngames - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(path, lib->games[by_path[mid]].path);
        if (!c) return by_path[mid];
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

static int query(Library *lib, const Sqlite *q, void *db)
{
    // Onion stores ROM paths relative to /mnt/SDCARD/Roms, one rom row per game.
    static const char SQL[] =
        "SELECT rom.file_path, COUNT(play_activity.rowid), SUM(play_activity.play_time), "
        "MAX(play_activity.created_at) FROM rom JOIN play_activity ON rom.id = play_activity.rom_id "
        "GROUP BY rom.file_path";
    void *stmt = NULL;
    if (q->prepare_v2(db, SQL, -1, &stmt, NULL) || !stmt) return -1;
    int *by_path = malloc(sizeof(int) * (lib->ngames ? lib->ngames : 1));
    for (int i = 0; i < lib->ngames; i++) by_path[i] = i;
    sort_lib = lib;
    qsort(by_path, lib->ngames, sizeof(int), cmp_path);
    int matched = 0;
    while (q->step(stmt) == SQLITE_ROW) {
        const char *rel = (const char *)q->column_text(stmt, 0);
        if (!rel) continue;
        char path[PATH_LEN];
        snprintf(path, sizeof path, rel[0] == '/' ? "%s" : SD_PREFIX "/Roms/%s", rel);
        path_normalise(path);
        int gi = find_game(lib, by_path, path);
        if (gi < 0) continue;
        Game *g = &lib->games[gi];
        g->play_count += (int)q->column_int64(stmt, 1);
        g->play_time += (int)q->column_int64(stmt, 2);
        long long last = q->column_int64(stmt, 3);
        if (last > g->last_played) g->last_played = last;
        matched++;
    }
    q->finalize(stmt);
    free(by_path);
    return matched;
}

int activity_load(Library *lib)
{
    void *so = NULL;
    for (int i = 0; !so && i < (int)(sizeof SQLITE_LIBS / sizeof *SQLITE_LIBS); i++)
        so = dlopen(SQLITE_LIBS[i], RTLD_NOW | RTLD_LOCAL);
    if (!so) return -1;
    Sqlite q = {
        (int (*)(const char *, void **, int, const char *))dlsym(so, "sqlite3_open_v2"),
        (int (*)(void *, const char *, int, void **, const char **))dlsym(so, "sqlite3_prepare_v2"),
        (int (*)(void *))dlsym(so, "sqlite3_step"),
        (const unsigned char *(*)(void *, int))dlsym(so, "sqlite3_column_text"),
        (long long (*)(void *, int))dlsym(so, "sqlite3_column_int64"),
        (int (*)(void *))dlsym(so, "sqlite3_finalize"),
        (int (*)(void *))dlsym(so, "sqlite3_close"),
    };
    int n = -1;
    char path[PATH_LEN];
    host_path(SD_PREFIX "/Saves/CurrentProfile/play_activity/play_activity_db.sqlite", path, sizeof path);
    void *db = NULL;
    if (q.open_v2 && q.prepare_v2 && q.step && q.column_text && q.column_int64 &&
        q.finalize && q.close) {
        if (q.open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) == 0) n = query(lib, &q, db);
        if (db) q.close(db); // read-only open never creates the file
    }
    dlclose(so);
    return n;
}
