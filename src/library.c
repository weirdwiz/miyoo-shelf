#include "library.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../third_party/cJSON.h"

static char g_root[PATH_LEN] = SD_PREFIX;

void lib_set_root(const char *root)
{
    snprintf(g_root, sizeof g_root, "%s", root);
    size_t n = strlen(g_root);
    while (n > 1 && g_root[n - 1] == '/') g_root[--n] = 0;
}

void host_path(const char *dev, char *out, int n)
{
    size_t k = strlen(SD_PREFIX);
    if (!strncmp(dev, SD_PREFIX, k) && (dev[k] == '/' || dev[k] == 0))
        snprintf(out, n, "%s%s", g_root, dev + k);
    else
        snprintf(out, n, "%s", dev);
}

void path_normalise(char *p)
{
    char *parts[64];
    int np = 0;
    char buf[PATH_LEN];
    snprintf(buf, sizeof buf, "%s", p);
    int abs = buf[0] == '/';
    char *save;
    for (char *tok = strtok_r(buf, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        if (!strcmp(tok, ".")) continue;
        if (!strcmp(tok, "..")) { if (np) np--; continue; }
        if (np < 64) parts[np++] = tok;
    }
    char out[PATH_LEN] = "";
    for (int i = 0; i < np; i++) {
        if (i || abs) strncat(out, "/", sizeof out - strlen(out) - 1);
        strncat(out, parts[i], sizeof out - strlen(out) - 1);
    }
    strcpy(p, out[0] ? out : (abs ? "/" : "."));
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = n < 0 ? NULL : malloc(n + 1);
    if (s && fread(s, 1, n, f) != (size_t)n) { free(s); s = NULL; }
    if (s) s[n] = 0;
    fclose(f);
    return s;
}

/* ---------- systems ---------- */

// The three systems Shelf runs, in home-row order. A folder matches by its own name or by a
// MinUI-style name such as "Game Boy Advance (GBA)"; GB folders also hold the original
// Game Boy formats (.dmg, .sgb), both of which gambatte runs.
static const struct {
    const char *dir, *label, *name, *core, *exts;
    uint32_t color;
} SYSTEMS[] = {
    {"GB", "GB", "Game Boy", "gambatte", "gb|gbc|dmg|sgb", 0x5a7040},
    {"GBC", "GBC", "Game Boy Color", "gambatte", "gb|gbc|dmg|sgb", 0x0e6e68},
    {"GBA", "GBA", "Game Boy Advance", "mgba", "gba", 0x4a4396},
};
#define NSYS ((int)(sizeof SYSTEMS / sizeof *SYSTEMS))

// "GB", "gb" and "Game Boy (GB)" all name the GB folder; -1 for anything else.
static int system_for(const char *name)
{
    for (int i = 0; i < NSYS; i++) {
        if (!strcasecmp(name, SYSTEMS[i].dir)) return i;
        char tag[8];
        snprintf(tag, sizeof tag, "(%s)", SYSTEMS[i].dir);
        size_t n = strlen(name), t = strlen(tag);
        if (n > t && !strcasecmp(name + n - t, tag)) return i;
    }
    return -1;
}

// "Legend of Zelda, The - The Minish Cap (USA) [!]" -> "The Legend of Zelda - The Minish Cap"
static void make_title(const char *stem, char *out, int n)
{
    char t[256];
    snprintf(t, sizeof t, "%s", stem);
    char *cut = t + strlen(t);
    for (char *c = t; *c; c++)
        if ((*c == '(' || *c == '[') && c > t) { cut = c; break; }
    *cut = 0;
    while (cut > t && cut[-1] == ' ') *--cut = 0;

    static const char *ARTICLES[] = {", The", ", A", ", An"};
    for (int i = 0; i < 3; i++) {
        char *a = strstr(t, ARTICLES[i]);
        if (!a) continue;
        char *after = a + strlen(ARTICLES[i]);
        if (*after && *after != ' ') continue;
        snprintf(out, n, "%s %.*s%s", ARTICLES[i] + 2, (int)(a - t), t, after); // may cut a long title
        return;
    }
    snprintf(out, n, "%s", t);
}

// Whether `name` has one of the extensions in `extlist` ("gba", "gb|gbc"), ignoring case.
static int ext_ok(const char *name, const char *extlist)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    const char *e = extlist;
    size_t n = strlen(dot + 1);
    while (*e) {
        const char *bar = strchr(e, '|');
        size_t len = bar ? (size_t)(bar - e) : strlen(e);
        if (len == n && !strncasecmp(e, dot + 1, n)) return 1;
        if (!bar) break;
        e = bar + 1;
    }
    return 0;
}

static void add_game(Library *lib, int *cap, int sys, const char *dir_dev, const char *file)
{
    if (lib->ngames == *cap) {
        *cap = *cap ? *cap * 2 : 256;
        Game *grown = realloc(lib->games, sizeof(Game) * *cap);
        if (!grown) return;
        lib->games = grown;
    }
    Game *g = &lib->games[lib->ngames++];
    memset(g, 0, sizeof *g);
    g->sys = sys;
    g->recent = -1;
    snprintf(g->stem, sizeof g->stem, "%s", file);
    char *dot = strrchr(g->stem, '.');
    if (dot) *dot = 0;
    make_title(g->stem, g->title, sizeof g->title);
    snprintf(g->path, sizeof g->path, "%s/%s", dir_dev, file);
    lib->sys[sys].ngames++;
}

// Adds every ROM under `dir_dev`, including subfolders but not hidden ones, Imgs or
// anything reached through a symlink (which could loop or leave the card).
static void scan_directory(Library *lib, int *cap, int sys, const char *dir_dev)
{
    char host[PATH_LEN];
    host_path(dir_dev, host, sizeof host);
    DIR *d = opendir(host);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcasecmp(e->d_name, "Imgs")) continue;
        char full[PATH_LEN];
        struct stat st;
        int len = snprintf(full, sizeof full, "%s/%s", host, e->d_name);
        if (len < 0 || len >= (int)sizeof full || lstat(full, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            char dev[PATH_LEN];
            len = snprintf(dev, sizeof dev, "%s/%s", dir_dev, e->d_name);
            if (len >= 0 && len < (int)sizeof dev) scan_directory(lib, cap, sys, dev);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !ext_ok(e->d_name, SYSTEMS[sys].exts)) continue;
        add_game(lib, cap, sys, dir_dev, e->d_name);
    }
    closedir(d);
}

/* ---------- Shelf's own files ---------- */

static int find_game(const Library *lib, const char *path)
{
    char p[PATH_LEN];
    snprintf(p, sizeof p, "%s", path);
    path_normalise(p);
    for (int i = 0; i < lib->ngames; i++)
        if (!strcmp(lib->games[i].path, p)) return i;
    return -1;
}

// The JSON in a device file, or NULL when it is missing or unreadable.
static cJSON *read_json(const char *dev_path)
{
    char host[PATH_LEN];
    host_path(dev_path, host, sizeof host);
    char *txt = read_file(host);
    cJSON *j = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    return j;
}

// An array of ROM paths: recents (most recent first) or favorites. *n is 0 when the file
// is missing, corrupt or not an array of strings.
static char **read_paths(const char *dev_path, int *n)
{
    *n = 0;
    cJSON *arr = read_json(dev_path);
    if (!cJSON_IsArray(arr)) { cJSON_Delete(arr); return NULL; }
    int count = cJSON_GetArraySize(arr);
    char **paths = malloc(sizeof *paths * (count ? count : 1));
    for (int i = 0; paths && i < count; i++) {
        const char *p = cJSON_GetStringValue(cJSON_GetArrayItem(arr, i));
        if (p && (paths[*n] = strdup(p))) (*n)++; // the strings die with the cJSON tree
    }
    cJSON_Delete(arr);
    return paths;
}

static void free_paths(char **paths, int n)
{
    for (int i = 0; i < n; i++) free(paths[i]);
    free(paths);
}

static void read_data(Library *lib)
{
    int n = 0;
    char **paths = read_paths(DATA_DIR "/recents.json", &n);
    for (int i = 0; i < n && lib->nrecent < MAX_RECENT; i++) {
        int gi = find_game(lib, paths[i]);
        if (gi < 0 || lib->games[gi].recent >= 0) continue;
        lib->games[gi].recent = lib->nrecent;
        lib->recent[lib->nrecent++] = gi;
    }
    free_paths(paths, n);

    paths = read_paths(DATA_DIR "/favorites.json", &n);
    for (int i = 0; i < n; i++) {
        int gi = find_game(lib, paths[i]);
        if (gi >= 0) lib->games[gi].fav = 1;
    }
    free_paths(paths, n);

    // playtime.json: path -> [seconds, sessions, last unix time].
    cJSON *obj = read_json(DATA_DIR "/playtime.json");
    if (!cJSON_IsObject(obj)) { cJSON_Delete(obj); return; }
    for (cJSON *row = obj->child; row; row = row->next) {
        int gi = row->string ? find_game(lib, row->string) : -1;
        if (gi < 0 || !cJSON_IsArray(row)) continue;
        Game *g = &lib->games[gi];
        cJSON *secs = cJSON_GetArrayItem(row, 0), *count = cJSON_GetArrayItem(row, 1);
        cJSON *last = cJSON_GetArrayItem(row, 2);
        if (cJSON_IsNumber(secs)) g->play_time = (int)secs->valuedouble;
        if (cJSON_IsNumber(count)) g->play_count = (int)count->valuedouble;
        if (cJSON_IsNumber(last)) g->last_played = (long long)last->valuedouble;
    }
    cJSON_Delete(obj);
}

/* ---------- writing ---------- */

// mkdir -p for a host path. 0 when the folder exists afterwards.
static int mkdirs(const char *path)
{
    char buf[PATH_LEN];
    if (snprintf(buf, sizeof buf, "%s", path) >= (int)sizeof buf) return -1;
    for (char *c = buf + 1;; c++) {
        if (*c && *c != '/') continue;
        char keep = *c;
        *c = 0;
        if (mkdir(buf, 0755) && errno != EEXIST) return -1;
        *c = keep;
        if (!keep) return 0;
    }
}

int lib_write_json(const char *dev_path, const char *json)
{
    char host[PATH_LEN], dir[PATH_LEN], tmp[PATH_LEN + 8];
    host_path(dev_path, host, sizeof host);
    snprintf(tmp, sizeof tmp, "%s.tmp", host);
    snprintf(dir, sizeof dir, "%s", host);
    char *slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        if (mkdirs(dir)) {
            fprintf(stderr, "shelf: can't create %s: %s\n", dir, strerror(errno));
            return -1;
        }
    }
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "shelf: can't write %s: %s\n", tmp, strerror(errno));
        return -1;
    }
    size_t len = strlen(json);
    int bad = fwrite(json, 1, len, f) != len;
    if (!bad) bad = fflush(f) != 0;
    if (!bad) bad = fsync(fileno(f)) != 0;
    if (fclose(f)) bad = 1;
    if (!bad && rename(tmp, host)) bad = 1;
    if (bad) {
        fprintf(stderr, "shelf: can't save %s: %s\n", dev_path, strerror(errno));
        remove(tmp);
        return -1;
    }
    return 0;
}

#define RESUME DATA_DIR "/resume"

int lib_set_resume(const Library *lib, int game)
{
    if (!lib || game < 0 || game >= lib->ngames) return -1;
    char line[PATH_LEN + 2];
    snprintf(line, sizeof line, "%s\n", lib->games[game].path);
    return lib_write_json(RESUME, line); // temp file, fsync, rename
}

int lib_take_resume(const Library *lib)
{
    char host[PATH_LEN], want[PATH_LEN] = "";
    host_path(RESUME, host, sizeof host);
    FILE *f = fopen(host, "rb");
    if (!f) return -1;
    if (!fgets(want, sizeof want, f)) want[0] = 0;
    fclose(f);
    // Gone before anything is started, and on the card: a game that crashes or hangs on
    // the way in must not come back on every boot.
    if (unlink(host) && errno != ENOENT) {
        fprintf(stderr, "shelf: can't remove %s: %s; not resuming\n", host, strerror(errno));
        return -1;
    }
    sync();
    want[strcspn(want, "\r\n")] = 0;
    for (int i = 0; lib && want[0] && i < lib->ngames; i++)
        if (!strcmp(lib->games[i].path, want)) return i;
    if (want[0]) fprintf(stderr, "shelf: %s is no longer on the card; not resuming\n", want);
    return -1;
}

// Prints and saves `j`, which it releases: 0 or -1.
static int save_json(cJSON *j, const char *dev_path)
{
    if (!j) return -1;
    char *json = cJSON_Print(j);
    cJSON_Delete(j);
    int rc = json ? lib_write_json(dev_path, json) : -1;
    cJSON_free(json);
    return rc;
}

// Appends one string to a JSON array; nonzero when out of memory.
static int add_path(cJSON *arr, const char *path)
{
    cJSON *item = cJSON_CreateString(path);
    if (!item) return 1;
    cJSON_AddItemToArray(arr, item);
    return 0;
}

int lib_mark_played(Library *lib, int game)
{
    if (game < 0 || game >= lib->ngames) return -1;
    int order[MAX_RECENT], n = 0;
    order[n++] = game;
    for (int i = 0; i < lib->nrecent && n < MAX_RECENT; i++)
        if (lib->recent[i] != game) order[n++] = lib->recent[i];

    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; arr && i < n; i++) add_path(arr, lib->games[order[i]].path);
    if (save_json(arr, DATA_DIR "/recents.json")) return -1;

    for (int i = 0; i < lib->ngames; i++) lib->games[i].recent = -1;
    for (int i = 0; i < n; i++) {
        lib->recent[i] = order[i];
        lib->games[order[i]].recent = i;
    }
    lib->nrecent = n;
    return 0;
}

int lib_set_fav(Library *lib, int game, int fav)
{
    if (game < 0 || game >= lib->ngames) return -1;
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; arr && i < lib->ngames; i++)
        if (i == game ? fav : lib->games[i].fav) add_path(arr, lib->games[i].path);
    if (save_json(arr, DATA_DIR "/favorites.json")) return -1;
    lib->games[game].fav = fav ? 1 : 0;
    return 0;
}

int lib_add_play(Library *lib, int game, int seconds, long long now)
{
    if (game < 0 || game >= lib->ngames) return -1;
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return -1;
    for (int i = 0; i < lib->ngames; i++) {
        const Game *g = &lib->games[i];
        int played = i == game;
        int count = g->play_count + played;
        if (!count) continue; // never started: no entry
        cJSON *row = cJSON_CreateArray();
        if (!row) { cJSON_Delete(obj); return -1; }
        cJSON_AddItemToArray(row, cJSON_CreateNumber(g->play_time + (played ? seconds : 0)));
        cJSON_AddItemToArray(row, cJSON_CreateNumber(count));
        cJSON_AddItemToArray(row, cJSON_CreateNumber((double)(played ? now : g->last_played)));
        if (!cJSON_AddItemToObject(obj, g->path, row)) { cJSON_Delete(obj); return -1; }
    }
    if (save_json(obj, DATA_DIR "/playtime.json")) return -1;
    lib->games[game].play_time += seconds;
    lib->games[game].play_count++;
    lib->games[game].last_played = now;
    return 0;
}

void lib_save_dir(const Library *lib, int game, char *out, int n)
{
    snprintf(out, n, SD_PREFIX "/Saves/%s", lib->sys[lib->games[game].sys].dir);
}

/* ---------- loading ---------- */

static int cmp_game(const void *a, const void *b)
{
    return strcasecmp(((const Game *)a)->title, ((const Game *)b)->title);
}

int lib_load(Library *lib)
{
    memset(lib, 0, sizeof *lib);
    for (int i = 0; i < NSYS; i++) {
        System *s = &lib->sys[lib->nsys++];
        snprintf(s->dir, sizeof s->dir, "%s", SYSTEMS[i].dir);
        snprintf(s->label, sizeof s->label, "%s", SYSTEMS[i].label);
        snprintf(s->name, sizeof s->name, "%s", SYSTEMS[i].name);
        snprintf(s->core, sizeof s->core, "%s", SYSTEMS[i].core);
        s->color = SYSTEMS[i].color;
        snprintf(s->rom_dir, sizeof s->rom_dir, SD_PREFIX "/Roms/%s", SYSTEMS[i].dir);
        snprintf(s->img_dir, sizeof s->img_dir, SD_PREFIX "/Roms/%s/Imgs", SYSTEMS[i].dir);
    }

    char roms[PATH_LEN];
    host_path(SD_PREFIX "/Roms", roms, sizeof roms);
    DIR *d = opendir(roms);
    if (!d) {
        fprintf(stderr, "shelf: no ROM folder at %s\n", roms);
        return -1;
    }
    int cap = 0, have_dir[MAX_SYSTEMS] = {0};
    struct dirent *e;
    while ((e = readdir(d))) {
        int sys = system_for(e->d_name);
        if (sys < 0) continue;
        char host[PATH_LEN], dev[PATH_LEN];
        struct stat st;
        int len = snprintf(host, sizeof host, "%s/%s", roms, e->d_name);
        if (len < 0 || len >= (int)sizeof host || lstat(host, &st) || !S_ISDIR(st.st_mode)) continue;
        snprintf(dev, sizeof dev, SD_PREFIX "/Roms/%s", e->d_name);
        if (!have_dir[sys]) { // the first matching folder names the system's own art folder
            have_dir[sys] = 1;
            snprintf(lib->sys[sys].rom_dir, PATH_LEN, "%s", dev);
            if (snprintf(lib->sys[sys].img_dir, PATH_LEN, "%s/Imgs", dev) >= PATH_LEN)
                lib->sys[sys].img_dir[0] = 0; // too long to hold: no art folder rather than a wrong one
        }
        scan_directory(lib, &cap, sys, dev);
    }
    closedir(d);

    read_data(lib);
    if (lib->ngames) qsort(lib->games, lib->ngames, sizeof(Game), cmp_game);
    // Sorting reindexes the games; compact the recents in the order they were read.
    int n = 0;
    for (int rank = 0; rank < lib->nrecent; rank++)
        for (int i = 0; i < lib->ngames; i++)
            if (lib->games[i].recent == rank) lib->recent[n++] = i;
    lib->nrecent = n;
    for (int i = 0; i < n; i++) lib->games[lib->recent[i]].recent = i;
    return 0;
}

void lib_free(Library *lib)
{
    free(lib->games);
    memset(lib, 0, sizeof *lib);
}

int lib_img_path(const Library *lib, const Game *g, char *out, int n)
{
    static const char *EXTS[] = {"png", "jpg", "jpeg", "gif"};
    const System *s = &lib->sys[g->sys];
    const char *slash = strrchr(g->path, '/');
    struct stat st;
    // The system's own Imgs folder first, then an Imgs folder beside the ROM (which is the
    // same folder for a ROM at the top of Roms/<SYS>).
    for (int beside = 0; beside < 2; beside++) {
        for (int i = 0; i < 4; i++) {
            char dev[PATH_LEN];
            int len;
            if (beside)
                len = snprintf(dev, sizeof dev, "%.*s/Imgs/%s.%s", (int)(slash - g->path),
                               g->path, g->stem, EXTS[i]);
            else if (!s->img_dir[0])
                continue; // its folder's path was too long to hold
            else
                len = snprintf(dev, sizeof dev, "%s/%s.%s", s->img_dir, g->stem, EXTS[i]);
            if (len < 0 || len >= (int)sizeof dev) continue;
            host_path(dev, out, n);
            if (!stat(out, &st) && S_ISREG(st.st_mode)) return 0;
        }
    }
    return -1;
}
