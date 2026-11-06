/* Enhanced options (design point 1): <user>/options.txt, "key = value" lines, # comments.
   Missing keys keep the defaults. --option key=value on the command line overrides the file
   for one run. The game's Options > More Options page (menus.c) edits the same file. */
#include <ctype.h>
#include <stdlib.h>
#include <strings.h>
#include "runtime.h"
#include "options.h"

typedef struct { char key[48], value[208]; int dirty; } Opt;
static Opt opts[64];
static int nopts;

static void set(const char *k, const char *v)
{
    for (int i = 0; i < nopts; i++)
        if (!strcasecmp(opts[i].key, k)) {
            snprintf(opts[i].value, sizeof opts[i].value, "%s", v);
            return;
        }
    if (nopts < 64) {
        snprintf(opts[nopts].key, sizeof opts[nopts].key, "%s", k);
        snprintf(opts[nopts].value, sizeof opts[nopts].value, "%s", v);
        nopts++;
    }
}

/* "key = value" -> set; returns 0 if the line has no '=' */
int option_parse(const char *line)
{
    const char *eq = strchr(line, '=');
    if (!eq) return 0;
    char k[64], v[256];
    const char *a = line, *b = eq;
    while (a < b && isspace((unsigned char)*a)) a++;
    while (b > a && isspace((unsigned char)b[-1])) b--;
    snprintf(k, sizeof k, "%.*s", (int)(b - a), a);
    a = eq + 1;
    while (*a && isspace((unsigned char)*a)) a++;
    b = a + strlen(a);
    while (b > a && isspace((unsigned char)b[-1])) b--;
    snprintf(v, sizeof v, "%.*s", (int)(b - a), a);
    set(k, v);
    return 1;
}

void options_load(void)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/options.txt", user_dir);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n#")] = 0;
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        if (*s && !option_parse(s))
            LOG(LOG_ALWAYS, "options.txt: \"%s\" is not key = value", s);
    }
    fclose(f);
}

/* Where a key was found, by the address of the caller's key string (some are asked many times a
   frame): a hint, checked; each thread its own. A key not there stays not there until one is added. */
static __thread struct { const char *key; int idx, n; } hint[32];

const char *option_str(const char *key, const char *def)
{
    unsigned h = (unsigned)((uintptr_t)key >> 3) % 32;
    if (hint[h].key == key) {
        int i = hint[h].idx;
        if (i < 0 ? hint[h].n == nopts : !strcasecmp(opts[i].key, key))
            return i < 0 ? def : opts[i].value;
    }
    int found = -1;
    for (int i = 0; i < nopts && found < 0; i++)
        if (!strcasecmp(opts[i].key, key))
            found = i;
    hint[h].key = key;
    hint[h].idx = found;
    hint[h].n = nopts;
    return found < 0 ? def : opts[found].value;
}

int option_bool(const char *key, int def)
{
    const char *v = option_str(key, NULL);
    if (!v) return def;
    return !strcasecmp(v, "on") || !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcmp(v, "1");
}

void option_set(const char *key, const char *value)
{
    set(key, value);
    for (int i = 0; i < nopts; i++)
        if (!strcasecmp(opts[i].key, key))
            opts[i].dirty = 1;
}

/* comments for keys written to the file the first time */
static const char *const help[][2] = {
    {"resolution", "resolution: native (the window's own pixels), original (640x480, scaled up), a height (e.g. 1080) or WxH"},
    {"widescreen", "widescreen: on (3D as wide as the screen, Hor+) or off (4:3 as shipped)"},
    {"hud", "hud: edges (the race HUD at the screen's edges in widescreen) or middle (kept in the 4:3 middle)"},
    {"antialiasing", "antialiasing: 4x, 2x or off (supersampling)"},
    {"anisotropic", "anisotropic: 16, 8, 4, 2 or off (texture filtering at an angle)"},
    {"smooth", "smooth: on (motion drawn between the game's 30 physics steps a second) or off"},
    {"display", "display: fullscreen or window (Alt+Enter switches too)"},
    {"vsync", "vsync: on (frames wait for the screen) or off"},
    {"rumble", "rumble: on or off (controller vibration on knocks)"},
    {"videos", "videos: on or off (every movie: intro, cup wins and losses, credits)"},
    {"intro_videos", "intro_videos: on or off (the legal screen and intro at start-up)"},
    {"steering", "steering: linear (as shipped), soft or softer (less steering near the stick's middle)"},
    {"controller_deadzone", "controller_deadzone: the sticks' dead zone in percent (6)"},
    {"cheats", "cheats: all (every cheat in Options > Cheat Menu from the start) or earned (as shipped)"},
    {"frame_limit", "frame_limit: off, screen (3 under the screen's refresh, for G-Sync / FreeSync) or a rate"},
    {NULL, NULL}};

/* Rewrite options.txt keeping its comments and order: a changed key's line is replaced, keys
   the file does not have yet are appended. */
int options_save(void)
{
    char path[1200], tmp[1300];
    snprintf(path, sizeof path, "%s/options.txt", user_dir);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *in = fopen(path, "r"), *out = fopen(tmp, "w");
    if (!out) {
        if (in) fclose(in);
        LOG(LOG_ALWAYS, "options: cannot write %s", tmp);
        return 0;
    }
    int written[64] = {0};
    char line[512];
    if (!in)
        fputs("# TDOR3 native: options (key = value), also set from the game's Options menu.\n"
              "# Delete a line to get the original behaviour.\n", out);
    while (in && fgets(line, sizeof line, in)) {
        char k[64] = "";
        const char *eq = strchr(line, '=');
        const char *hash = strchr(line, '#');
        if (eq && (!hash || hash > eq)) {
            const char *a = line, *b = eq;
            while (a < b && isspace((unsigned char)*a)) a++;
            while (b > a && isspace((unsigned char)b[-1])) b--;
            snprintf(k, sizeof k, "%.*s", (int)(b - a), a);
        }
        int done = 0;
        for (int i = 0; i < nopts && k[0]; i++)
            if (!strcasecmp(opts[i].key, k) && opts[i].dirty) {
                if (!written[i]) fprintf(out, "%s = %s\n", opts[i].key, opts[i].value);
                written[i] = done = 1;
            }
        if (!done)
            fputs(line, out);
    }
    for (int i = 0; i < nopts; i++)
        if (opts[i].dirty && !written[i]) {
            for (int k = 0; help[k][0]; k++)
                if (!strcasecmp(help[k][0], opts[i].key))
                    fprintf(out, "\n# %s\n", help[k][1]);
            fprintf(out, "%s = %s\n", opts[i].key, opts[i].value);
        }
    if (in) fclose(in);
    int ok = fclose(out) == 0 && rename(tmp, path) == 0;
    if (!ok)
        LOG(LOG_ALWAYS, "options: cannot save %s", path);
    return ok;
}
