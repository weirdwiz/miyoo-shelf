#include "settings.h"

#include <stdio.h>
#include <string.h>

#include "../third_party/cJSON.h"
#include "library.h"

#define SETTINGS_PATH DATA_DIR "/settings.json"

static const char *const SCALE_NAMES[SCALE_MODES] = {"sharp", "smooth", "integer"};

void settings_defaults(Settings *s)
{
    s->dark = 0;
    s->brightness = 7;
    s->volume = 12;
    s->scale = SCALE_SHARP;
    s->colours = 1;
    s->palette = PALETTE_DEFAULT;
}

// Reads key into *out when it holds a number inside [lo, hi]; anything else keeps what
// the defaults put there.
static void read_level(cJSON *obj, const char *key, int *out, int lo, int hi)
{
    cJSON *j = cJSON_GetObjectItem(obj, key);
    if (!cJSON_IsNumber(j)) return;
    int v = (int)j->valuedouble;
    if (v >= lo && v <= hi) *out = v;
}

int settings_load(Settings *s)
{
    if (!s) return -1;
    settings_defaults(s);
    char path[PATH_LEN], buf[2048];
    host_path(SETTINGS_PATH, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t len = fread(buf, 1, sizeof buf - 1, f);
    int bad = ferror(f);
    fclose(f);
    if (bad) return -1;
    buf[len] = 0;

    cJSON *j = cJSON_Parse(buf);
    if (!cJSON_IsObject(j)) { cJSON_Delete(j); return -1; }
    s->dark = cJSON_IsTrue(cJSON_GetObjectItem(j, "dark")) ? 1 : 0;
    read_level(j, "brightness", &s->brightness, 0, BRIGHTNESS_MAX);
    read_level(j, "volume", &s->volume, 0, VOLUME_MAX);
    cJSON *scale = cJSON_GetObjectItem(j, "scale");
    if (cJSON_IsString(scale))
        for (int i = 0; i < SCALE_MODES; i++)
            if (!strcmp(scale->valuestring, SCALE_NAMES[i])) s->scale = (ScaleMode)i;
    cJSON *colours = cJSON_GetObjectItem(j, "colours");
    if (cJSON_IsBool(colours)) s->colours = cJSON_IsTrue(colours);
    cJSON *palette = cJSON_GetObjectItem(j, "palette");
    if (cJSON_IsString(palette))
        for (int i = 0; i < PALETTE_COUNT; i++)
            if (!strcmp(palette->valuestring, core_palette(i)->id)) s->palette = i;
    cJSON_Delete(j);
    return 0;
}

int settings_save(const Settings *s)
{
    if (!s) return -1;
    int scale = s->scale >= 0 && s->scale < SCALE_MODES ? (int)s->scale : SCALE_SHARP;
    const CorePalette *palette = core_palette(s->palette);
    if (!palette) palette = core_palette(PALETTE_DEFAULT);
    char json[256];
    snprintf(json, sizeof json,
             "{\"dark\": %s, \"brightness\": %d, \"volume\": %d, \"scale\": \"%s\", "
             "\"colours\": %s, \"palette\": \"%s\"}\n",
             s->dark ? "true" : "false", s->brightness, s->volume, SCALE_NAMES[scale],
             s->colours ? "true" : "false", palette->id);
    if (lib_write_json(SETTINGS_PATH, json)) {
        fprintf(stderr, "shelf: couldn't save the settings\n");
        return -1;
    }
    return 0;
}
