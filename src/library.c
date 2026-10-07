#include "library.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "../third_party/cJSON.h"
#include "gfx.h"

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
    char *s = malloc(n + 1);
    if (s && fread(s, 1, n, f) != (size_t)n) { free(s); s = NULL; }
    if (s) s[n] = 0;
    fclose(f);
    return s;
}

// Friendly names and colours for Onion's Emu folder names.
static const struct { const char *dir, *label, *name; uint32_t color; } KNOWN[] = {
    {"GBA", "GBA", "Game Boy Advance", 0x5b4fd0},
    {"SFC", "SNES", "Super Nintendo", 0x8f5fc8},
    {"GBC", "GBC", "Game Boy Color", 0x13a39a},
    {"GB", "Game Boy", "Game Boy", 0x6f8f2f},
    {"FC", "NES", "Nintendo (NES)", 0xd6413a},
    {"PS", "PS1", "PlayStation", 0x4b5568},
    {"MD", "Mega Drive", "Mega Drive / Genesis", 0x1f5fbf},
    {"GG", "Game Gear", "Game Gear", 0x2b2f3a},
    {"MS", "Master Sys", "Master System", 0xb3262b},
    {"PCE", "PC Engine", "PC Engine", 0xe0782a},
    {"NGP", "NeoGeo Pkt", "Neo Geo Pocket", 0x3d6ea8},
    {"WS", "WonderSwan", "WonderSwan", 0x7a7f8c},
    {"ARCADE", "Arcade", "Arcade", 0xc0392b},
    {"FBNEO", "Arcade", "Arcade (FBNeo)", 0xc0392b},
    {"N64", "N64", "Nintendo 64", 0x2a9d48},
    {"PICO", "PICO-8", "PICO-8", 0xff004d},
};

static void sys_identity(System *s, const char *cfg_label)
{
    for (size_t i = 0; i < sizeof KNOWN / sizeof *KNOWN; i++)
        if (!strcasecmp(s->dir, KNOWN[i].dir)) {
            snprintf(s->label, sizeof s->label, "%s", KNOWN[i].label);
            snprintf(s->name, sizeof s->name, "%s", KNOWN[i].name);
            s->color = KNOWN[i].color;
            return;
        }
    snprintf(s->label, sizeof s->label, "%s", cfg_label && *cfg_label ? cfg_label : s->dir);
    snprintf(s->name, sizeof s->name, "%s", s->label);
    uint32_t h = 2166136261u;
    for (const char *c = s->dir; *c; c++) h = (h ^ (unsigned char)*c) * 16777619u;
    static const uint32_t PALETTE[] = {0x5b4fd0, 0x13a39a, 0xd6413a, 0x1f5fbf, 0xe0782a, 0x8f5fc8, 0x2a9d48};
    s->color = PALETTE[h % 7];
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
        char head[256], rest[256];
        snprintf(head, sizeof head, "%.*s", (int)(a - t), t);
        snprintf(rest, sizeof rest, "%s", after);
        snprintf(out, n, "%s %s%s", ARTICLES[i] + 2, head, rest);
        return;
    }
    snprintf(out, n, "%s", t);
}

static int ext_ok(const char *name, const char *extlist)
{
    const char *dot = strrchr(name, '.');
    if (!dot || !extlist || !*extlist) return dot != NULL;
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
        lib->games = realloc(lib->games, sizeof(Game) * *cap);
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
}

static void scan_directory(Library *lib, int *cap, int sys, const char *dir_dev,
                           const char *extlist)
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
        // Skip symlinks to prevent loops and traversal outside the ROM tree.
        int len = snprintf(full, sizeof full, "%s/%s", host, e->d_name);
        if (len < 0 || len >= sizeof full || lstat(full, &st)) continue;
        if (S_ISDIR(st.st_mode)) {
            char dev[PATH_LEN];
            len = snprintf(dev, sizeof dev, "%s/%s", dir_dev, e->d_name);
            if (len >= 0 && len < sizeof dev)
                scan_directory(lib, cap, sys, dev, extlist);
            continue;
        }
        if (!S_ISREG(st.st_mode) || !ext_ok(e->d_name, extlist)) continue;
        if (strstr(e->d_name, ".xml") || strstr(e->d_name, ".db")) continue;
        char dev[PATH_LEN];
        len = snprintf(dev, sizeof dev, "%s/%s", dir_dev, e->d_name);
        if (len < 0 || len >= sizeof dev) continue;
        add_game(lib, cap, sys, dir_dev, e->d_name);
        lib->sys[sys].ngames++;
    }
    closedir(d);
}

// Keep launchable playlists / CUE sheets, not their individual discs / tracks.
// Resolve references after scanning so directory order does not affect discovery.
static void collapse_disc_files(Library *lib)
{
    int *owner = malloc(lib->ngames * sizeof *owner);
    if (!owner) return;
    for (int i = 0; i < lib->ngames; i++) owner[i] = -1;
    for (int i = 0; i < lib->ngames; i++) {
        const Game *g = &lib->games[i];
        const char *ext = strrchr(g->path, '.');
        int cue = ext && !strcasecmp(ext, ".cue");
        if (!cue && (!ext || strcasecmp(ext, ".m3u"))) continue;
        char host[PATH_LEN];
        host_path(g->path, host, sizeof host);
        FILE *f = fopen(host, "r");
        if (!f) continue;
        char line[PATH_LEN + 16];
        while (fgets(line, sizeof line, f)) {
            char *ref = line;
            if (!strncmp(ref, "\xef\xbb\xbf", 3)) ref += 3;
            while (isspace((unsigned char)*ref)) ref++;
            if (cue) {
                if (strncasecmp(ref, "FILE", 4) || !isspace((unsigned char)ref[4])) continue;
                ref += 4;
                while (isspace((unsigned char)*ref)) ref++;
                char *end;
                if (*ref == '"') {
                    ref++;
                    end = strchr(ref, '"');
                    if (!end) continue;
                } else {
                    end = ref;
                    while (*end && !isspace((unsigned char)*end)) end++;
                }
                *end = 0;
            } else {
                if (!*ref || *ref == '#') continue;
                char *end = ref + strlen(ref);
                while (end > ref && isspace((unsigned char)end[-1])) *--end = 0;
            }
            if (!*ref) continue;
            char path[PATH_LEN];
            int len;
            if (*ref == '/') len = snprintf(path, sizeof path, "%s", ref);
            else {
                const char *slash = strrchr(g->path, '/');
                len = snprintf(path, sizeof path, "%.*s/%s", (int)(slash - g->path), g->path, ref);
            }
            if (len < 0 || len >= sizeof path) continue;
            path_normalise(path);
            for (int j = 0; j < lib->ngames; j++)
                if (j != i && lib->games[j].sys == g->sys && !strcmp(lib->games[j].path, path))
                    owner[j] = i;
        }
        fclose(f);
    }
    // A recent/favorite disc belongs to its playlist, including M3U -> CUE -> BIN.
    for (int i = 0; i < lib->ngames; i++) {
        int target = i, steps = 0;
        while (owner[target] >= 0 && steps++ < lib->ngames) target = owner[target];
        if (steps >= lib->ngames) { owner[i] = -1; continue; }
        Game *g = &lib->games[target];
        g->fav |= lib->games[i].fav;
        int recent = lib->games[i].recent;
        if (recent >= 0 && (g->recent < 0 || recent < g->recent)) g->recent = recent;
    }
    int kept = 0;
    for (int i = 0; i < lib->ngames; i++) {
        if (owner[i] >= 0) lib->sys[lib->games[i].sys].ngames--;
        else lib->games[kept++] = lib->games[i];
    }
    lib->ngames = kept;
    free(owner);
}

static int find_game(const Library *lib, const char *rompath)
{
    char p[PATH_LEN];
    snprintf(p, sizeof p, "%s", rompath);
    path_normalise(p);
    for (int i = 0; i < lib->ngames; i++)
        if (!strcmp(lib->games[i].path, p)) return i;
    return -1;
}

// Onion stores recents/favourites as one JSON object per line.
static void read_entries(Library *lib, const char *dev_path, int recent)
{
    char host[PATH_LEN];
    host_path(dev_path, host, sizeof host);
    char *s = read_file(host);
    if (!s) return;
    char *save;
    for (char *line = strtok_r(s, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        cJSON *j = cJSON_Parse(line);
        if (!j) continue;
        const char *rp = cJSON_GetStringValue(cJSON_GetObjectItem(j, "rompath"));
        int gi = rp ? find_game(lib, rp) : -1;
        if (gi >= 0) {
            if (recent && lib->games[gi].recent < 0 && lib->nrecent < MAX_RECENT) {
                lib->games[gi].recent = lib->nrecent;
                lib->recent[lib->nrecent++] = gi;
            }
            if (!recent) lib->games[gi].fav = 1;
        }
        cJSON_Delete(j);
    }
    free(s);
}

static int cmp_game(const void *a, const void *b)
{
    return strcasecmp(((const Game *)a)->title, ((const Game *)b)->title);
}

int lib_load(Library *lib)
{
    memset(lib, 0, sizeof *lib);
    int cap = 0;
    char emu_host[PATH_LEN];
    host_path(SD_PREFIX "/Emu", emu_host, sizeof emu_host);
    DIR *d = opendir(emu_host);
    if (!d) {
        fprintf(stderr, "shelf: no Emu folder at %s\n", emu_host);
        return -1;
    }
    struct dirent *e;
    while ((e = readdir(d)) && lib->nsys < MAX_SYSTEMS) {
        if (e->d_name[0] == '.') continue;
        char cfg_path[PATH_LEN];
        snprintf(cfg_path, sizeof cfg_path, "%s/%s/config.json", emu_host, e->d_name);
        char *txt = read_file(cfg_path);
        if (!txt) continue;
        cJSON *cfg = cJSON_Parse(txt);
        free(txt);
        if (!cfg) continue;
        const char *rompath = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "rompath"));
        const char *launch = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "launch"));
        const char *imgpath = cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "imgpath"));
        if (rompath && launch) {
            System *s = &lib->sys[lib->nsys];
            memset(s, 0, sizeof *s);
            snprintf(s->dir, sizeof s->dir, "%s", e->d_name);
            snprintf(s->emu_dir, sizeof s->emu_dir, SD_PREFIX "/Emu/%s", e->d_name);
            snprintf(s->launch, sizeof s->launch, "%s/%s", s->emu_dir, launch);
            snprintf(s->rom_dir, sizeof s->rom_dir, "%s/%s", s->emu_dir, rompath);
            path_normalise(s->rom_dir);
            if (imgpath) snprintf(s->img_dir, sizeof s->img_dir, "%s/%s", s->emu_dir, imgpath);
            else snprintf(s->img_dir, sizeof s->img_dir, "%s/Imgs", s->rom_dir);
            path_normalise(s->img_dir);
            sys_identity(s, cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "label")));
            scan_directory(lib, &cap, lib->nsys, s->rom_dir,
                           cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "extlist")));
            if (s->ngames) lib->nsys++;
        }
        cJSON_Delete(cfg);
    }
    closedir(d);
    // With Onion's "hide recents" setting, MainUI's runtime moves each new recentlist.json
    // into recentlist-hidden.json (newest first). Shelf's home row shows both.
    read_entries(lib, SD_PREFIX "/Roms/recentlist.json", 1);
    read_entries(lib, SD_PREFIX "/Roms/recentlist-hidden.json", 1);
    read_entries(lib, SD_PREFIX "/Roms/favourite.json", 0);
    collapse_disc_files(lib);
    if (lib->ngames) qsort(lib->games, lib->ngames, sizeof(Game), cmp_game);
    // Sorting and collapsing change indices; compact recents in their original order.
    int nrecent = 0;
    for (int rank = 0; rank < lib->nrecent; rank++)
        for (int i = 0; i < lib->ngames; i++)
            if (lib->games[i].recent == rank) lib->recent[nrecent++] = i;
    lib->nrecent = nrecent;
    for (int i = 0; i < nrecent; i++) lib->games[lib->recent[i]].recent = i;
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
    const char *relative = g->path + strlen(s->rom_dir) + 1;
    int subdir_len = (int)(slash - relative);
    struct stat st;
    // Prefer Onion's configured artwork folder, then mirrored category folders,
    // then an Imgs folder beside the ROM.
    for (int location = 0; location < 3; location++) {
        if (location == 1 && subdir_len <= 0) continue;
        for (int i = 0; i < 4; i++) {
            char dev[PATH_LEN];
            int len;
            if (location == 0)
                len = snprintf(dev, sizeof dev, "%s/%s.%s", s->img_dir, g->stem, EXTS[i]);
            else if (location == 1)
                len = snprintf(dev, sizeof dev, "%s/%.*s/%s.%s", s->img_dir,
                               subdir_len, relative, g->stem, EXTS[i]);
            else
                len = snprintf(dev, sizeof dev, "%.*s/Imgs/%s.%s",
                               (int)(slash - g->path), g->path, g->stem, EXTS[i]);
            if (len < 0 || len >= sizeof dev) continue;
            host_path(dev, out, n);
            if (!stat(out, &st) && S_ISREG(st.st_mode)) return 0;
        }
    }
    return -1;
}

static void json_escape(const char *s, char *out, int n)
{
    int k = 0;
    for (; *s && k < n - 2; s++) {
        if (*s == '"' || *s == '\\') out[k++] = '\\';
        out[k++] = *s;
    }
    out[k] = 0;
}

// Onion's runtime parses MainUI's exact layout (LD_PRELOAD=... "<launch>" "<rompath>") with
// awk and escapes '$' itself, so write it verbatim. Names it cannot quote are refused.
static int unquotable(const char *s)
{
    return strpbrk(s, "\"`\\\n") != NULL;
}

int lib_launch(Library *lib, int gi, const char *cmd_path)
{
    Game *g = &lib->games[gi];
    System *s = &lib->sys[g->sys];

    // MainUI records rompath relative to the Emu folder ("<emu>/../../Roms/..."); keep that form.
    char rel[PATH_LEN], rompath[PATH_LEN];
    snprintf(rel, sizeof rel, "%s", g->path + strlen(SD_PREFIX) + 1);
    snprintf(rompath, sizeof rompath, "%s/../../%s", s->emu_dir, rel);

    if (unquotable(s->launch) || unquotable(rompath)) {
        fprintf(stderr, "shelf: cannot launch %s: quote, backtick or backslash in path\n", g->path);
        return -1;
    }
    FILE *f = fopen(cmd_path, "w");
    if (!f) { perror("shelf: cmd_to_run"); return -1; }
    fprintf(f, "LD_PRELOAD=/mnt/SDCARD/miyoo/app/../lib/libpadsp.so \"%s\" \"%s\"\n", s->launch, rompath);
    int failed = fflush(f) != 0;
    if (!failed && fchmod(fileno(f), 0755)) failed = 1;
    if (fclose(f)) failed = 1;
    if (failed) {
        perror("shelf: write launch command");
        remove(cmd_path);
        return -1;
    }
    if (getenv("SHELF_TRY")) return 0; // trial runs never touch the real recents file

    // Rewrite recentlist.json with this game first, dropping its old line.
    char host[PATH_LEN], tmp[PATH_LEN];
    host_path(SD_PREFIX "/Roms/recentlist.json", host, sizeof host);
    snprintf(tmp, sizeof tmp, "%s.tmp", host);
    char *old = read_file(host);
    FILE *out = fopen(tmp, "w");
    if (!out) { free(old); return 0; }

    char label[256], launch[PATH_LEN * 2], rp[PATH_LEN * 2], img_dev[PATH_LEN], img[PATH_LEN * 2];
    json_escape(g->stem, label, sizeof label);
    json_escape(s->launch, launch, sizeof launch);
    json_escape(rompath, rp, sizeof rp);
    snprintf(img_dev, sizeof img_dev, "%s/%s.png", s->img_dir, g->stem);
    json_escape(img_dev, img, sizeof img);
    fprintf(out, "{\"label\":\"%s\",\"launch\":\"%s\",\"type\":5,\"imgpath\":\"%s\",\"rompath\":\"%s\"}\n", label, launch, img, rp);

    int kept = 1;
    if (old) {
        char *save;
        for (char *line = strtok_r(old, "\n", &save); line && kept < 50; line = strtok_r(NULL, "\n", &save)) {
            cJSON *j = cJSON_Parse(line);
            const char *p = j ? cJSON_GetStringValue(cJSON_GetObjectItem(j, "rompath")) : NULL;
            char norm[PATH_LEN] = "";
            if (p) { snprintf(norm, sizeof norm, "%s", p); path_normalise(norm); }
            if (j && strcmp(norm, g->path)) { fprintf(out, "%s\n", line); kept++; }
            cJSON_Delete(j);
        }
        free(old);
    }
    fclose(out);
    rename(tmp, host);

    // Refresh in-memory recents.
    for (int i = 0; i < lib->ngames; i++) lib->games[i].recent = -1;
    int n = 0, order[MAX_RECENT];
    order[n++] = gi;
    for (int i = 0; i < lib->nrecent && n < MAX_RECENT; i++)
        if (lib->recent[i] != gi) order[n++] = lib->recent[i];
    for (int i = 0; i < n; i++) { lib->recent[i] = order[i]; lib->games[order[i]].recent = i; }
    lib->nrecent = n;
    return 0;
}
