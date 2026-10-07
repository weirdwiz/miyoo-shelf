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

static int is_dir(const char *p)
{
    struct stat st;
    return !stat(p, &st) && S_ISDIR(st.st_mode);
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

static void scan_system(Library *lib, int *cap, int sys, const char *extlist)
{
    System *s = &lib->sys[sys];
    char host[PATH_LEN];
    host_path(s->rom_dir, host, sizeof host);
    DIR *d = opendir(host);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcasecmp(e->d_name, "Imgs")) continue;
        char full[PATH_LEN];
        snprintf(full, sizeof full, "%s/%s", host, e->d_name);
        if (is_dir(full) || !ext_ok(e->d_name, extlist)) continue;
        if (strstr(e->d_name, ".xml") || strstr(e->d_name, ".db")) continue;
        add_game(lib, cap, sys, s->rom_dir, e->d_name);
        s->ngames++;
    }
    closedir(d);
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
            scan_system(lib, &cap, lib->nsys, cJSON_GetStringValue(cJSON_GetObjectItem(cfg, "extlist")));
            if (s->ngames) lib->nsys++;
        }
        cJSON_Delete(cfg);
    }
    closedir(d);
    if (lib->ngames) qsort(lib->games, lib->ngames, sizeof(Game), cmp_game);
    read_entries(lib, SD_PREFIX "/Roms/recentlist.json", 1);
    read_entries(lib, SD_PREFIX "/Roms/favourite.json", 0);
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
    struct stat st;
    for (int i = 0; i < 4; i++) {
        char dev[PATH_LEN];
        snprintf(dev, sizeof dev, "%s/%s.%s", lib->sys[g->sys].img_dir, g->stem, EXTS[i]);
        host_path(dev, out, n);
        if (!stat(out, &st)) return 0;
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

int lib_launch(Library *lib, int gi, const char *cmd_path)
{
    Game *g = &lib->games[gi];
    System *s = &lib->sys[g->sys];

    // MainUI records rompath relative to the Emu folder ("<emu>/../../Roms/..."); keep that form.
    char rel[PATH_LEN], rompath[PATH_LEN];
    snprintf(rel, sizeof rel, "%s", g->path + strlen(SD_PREFIX) + 1);
    snprintf(rompath, sizeof rompath, "%s/../../%s", s->emu_dir, rel);

    FILE *f = fopen(cmd_path, "w");
    if (!f) { perror("shelf: cmd_to_run"); return -1; }
    fprintf(f, "LD_PRELOAD=/mnt/SDCARD/miyoo/app/../lib/libpadsp.so \"%s\" \"%s\"\n", s->launch, rompath);
    fclose(f);

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
