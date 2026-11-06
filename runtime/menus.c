/* Pages added to the game's own menus (DESIGN.md decision 1: extras as options).

   The menus are text scripts (UIData scripts.arc: [desktop], [button], [menu], [animation]
   blocks in a 320x256 layout) that the menu shell reads by name through the archive reader
   0x467b4a(archive, name, &buffer, &size); its buffer comes from operator new. The reader is
   wrapped here: two of the game's scripts are changed as they are read and one is new.
   - o_op00.afw, Options: a seventh button, "More Options", the six above moved up to make room.
   - o_nat.afw (new), More Options: the runtime's options (options.c), one row each, Left /
     Right or A to change; each change is saved to options.txt and takes effect at once
     (texture filtering on the next start). Its buttons answer "Native_Next" / "Native_Prev"
     with the row's number, which the shell's action dispatcher (0x480bd8) is asked first.
   - o_op05.afw, Cheat Menu: rows for the four unlock cheats (all trucks, tracks, upgrades,
     divisions) above the six toggles it has.

   Cheats (the object at 0x4d4170, the player's progress: +0x104 the cheats unlocked, bit n
   cheat n; +0x108 the ones on; +0x100 the menu's current row). Codes typed as a name unlock and
   turn on one cheat (0x47284f); winning unlocks some (0x4709d1); the Cheat Menu lists the
   unlocked ones among 5-10 (0x472b7f) and toggles them; Apply_Cheats (0x47307e) carries out
   1-4, which change the tour's unlocks. Bit 0 (the debug keys) has no menu row. With option
   cheats = all (the default) bits 1-10 are kept unlocked, so the menu has every cheat from the
   start; cheats = earned is the game as shipped. The menu shows 1-4 here, and its Next / Prev
   (0x472f6b) and SetCurCheat (in 0x472c45) take them in. */
#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "options.h"
#include "funcs.h"

#define CHEATS 0x4d4170u
#define CHEATS_UNLOCKED (CHEATS + 0x104)
#define CHEATS_ON (CHEATS + 0x108)
#define CHEAT_ROW (CHEATS + 0x100)
#define CHEAT_SHOW_STATE 0x472fe6u        /* thiscall (cheats, n): its row's on / off */
#define MENU_SHOW 0x48dbd0u               /* thiscall (screen, menu id, visible) */
#define SET_TEXT 0x48dc1cu                /* thiscall (screen, button id, text id, text, is a string key) */
#define OPERATOR_NEW 0x4a21f8u            /* TDOR3.exe's import slots */
#define OPERATOR_DELETE 0x4a21ccu

void win_options(void);
void frame_limit_options(void);
void vk_options_changed(void);
void smooth_options(void);
void pad_options(void);

/* ------------------------------------------------------------------ text editing */
typedef struct { char *s; size_t n, cap; } Text;

static void text_add(Text *t, const char *s, size_t n)
{
    if (t->n + n + 1 > t->cap) {
        t->cap = (t->n + n + 1) * 2;
        t->s = realloc(t->s, t->cap);
    }
    memcpy(t->s + t->n, s, n);
    t->n += n;
    t->s[t->n] = 0;
}

static void text_printf(Text *t, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void text_printf(Text *t, const char *fmt, ...)
{
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    text_add(t, buf, (size_t)(n < (int)sizeof buf ? n : (int)sizeof buf - 1));
}

/* every `from` in t replaced by `to`; how many */
static int replace(Text *t, const char *from, const char *to)
{
    Text o = {0};
    size_t fl = strlen(from);
    int k = 0;
    const char *p = t->s, *q;
    while ((q = strstr(p, from))) {
        text_add(&o, p, (size_t)(q - p));
        text_add(&o, to, strlen(to));
        p = q + fl;
        k++;
    }
    text_add(&o, p, strlen(p));
    free(t->s);
    *t = o;
    return k;
}

/* the game's scripts end lines with CRLF or LF; edits are made on LF text */
static Text text_from(const char *s, size_t n)
{
    Text t = {0};
    text_add(&t, "", 0);
    for (size_t i = 0; i < n; i++)
        if (s[i] != '\r')
            text_add(&t, s + i, 1);
    return t;
}

/* ------------------------------------------------------------------ Options: one more button */
static const int old_y[6] = {60, 88, 115, 142, 169, 196}, new_y[7] = {56, 80, 104, 128, 152, 176, 200};

static void patch_options(Text *t)
{
    char a[64], b[64];
    for (int i = 0; i < 6; i++) {
        snprintf(a, sizeof a, "key = 19 %d 230 25", old_y[i]);
        snprintf(b, sizeof b, "key = 19 %d 230 25", new_y[i]);
        replace(t, a, b);
        snprintf(a, sizeof a, "\ny = %d\n", old_y[i]);
        snprintf(b, sizeof b, "\ny = %d\n", new_y[i]);
        replace(t, a, b);
    }
    replace(t, "ondownp = b_controller\n[button]\nid = b_controller",      /* (wrapping round) */
            "ondownp = b_controller\nonup = setFocus\nonupp = b_more\n[button]\nid = b_controller");
    replace(t, "key = (15,60,200,160);", "key = (15,52,200,178);");
    replace(t, "coords(33,0,175,160)", "coords(33,0,175,178)");
    replace(t, "onupp = b_cheatmenu\n[button]\nid = OK5",
            "onupp = b_cheatmenu\n"
            "ondown = setFocus\n"
            "ondownp = b_more\n"
            "[animation]\n"
            "id = a_7thChoice\n"
            "type = oneshot\n"
            "predelay = 1400\n"
            "runtime = 500\n"
            "key = 19 256 230 25\n"
            "key = 19 200 230 25\n"
            "[button]\n"
            "id = b_more\n"
            "x = 19\n"
            "y = 200\n"
            "width = 300\n"
            "height = 25\n"
            "animation = a_7thChoice\n"
            "image = \"button_off\" at 0,0; plane 10\n"
            "focustype = flicker\n"
            "focustint = color 50 200 50\n"
            "image = \"button_highlight\" at 8,7; plane 9;\n"
            "focusType = strobe\n"
            "text = string(natMore); at 33,11; color 94,94,20; font font1;plane 10;\n"
            "hasFocus = no\n"
            "action = loadscript\n"
            "aparam = o_nat.afw\n"
            "onup = setFocus\n"
            "onupp = b_credits\n"
            "ondown = setFocus\n"
            "ondownp = b_memcard\n"
            "[button]\n"
            "id = OK5");
}

/* ------------------------------------------------------------------ More Options */
typedef struct {
    const char *key, *label, *def;
    const char *values[8], *names[8];
} Row;

static const Row rows[] = {
    {"display", "Display", "fullscreen", {"fullscreen", "window"}, {"Full Screen", "Window"}},
    {"resolution", "Resolution", "native", {"native", "2160", "1440", "1080", "720", "original"},
     {"Native", "2160p", "1440p", "1080p", "720p", "640x480"}},
    {"widescreen", "Widescreen", "on", {"on", "off"}, {"On", "Off (4:3)"}},
    {"hud", "Race HUD", "edges", {"edges", "middle"}, {"Screen Edges", "Middle"}},
    {"antialiasing", "Anti-Aliasing", "4x", {"4x", "2x", "off"}, {"4x", "2x", "Off"}},
    {"anisotropic", "Texture Filter", "16", {"16", "8", "4", "2", "off"}, {"16x", "8x", "4x", "2x", "Off"}},
    {"smooth", "Smooth Motion", "on", {"on", "off"}, {"On", "Off (30 fps)"}},
    {"vsync", "V-Sync", "on", {"on", "off"}, {"On", "Off"}},
    {"frame_limit", "Frame Limit", "off", {"off", "screen", "60", "120", "144", "165", "240"},
     {"Off", "Below Screen", "60", "120", "144", "165", "240"}},
    {"rumble", "Rumble", "on", {"on", "off"}, {"On", "Off"}},
    {"steering", "Stick Steering", "linear", {"linear", "soft", "softer"}, {"Linear", "Soft Centre", "Softer Centre"}},
    {"controller_deadzone", "Stick Dead Zone", "6", {"3", "6", "10", "15", "20"},
     {"3 Percent", "6 Percent", "10 Percent", "15 Percent", "20 Percent"}},
    {"movies", "Movies", "all", {"all", "nointro", "none"}, {"All", "Skip Intro", "None"}},
    {"cheats", "Cheats", "all", {"all", "earned"}, {"All Unlocked", "Earned"}},
};
#define NROWS ((int)(sizeof rows / sizeof rows[0]))

static int nvalues(const Row *r)
{
    int n = 0;
    while (n < 8 && r->values[n]) n++;
    return n;
}

/* the row's current value, by its index in values */
static int row_value(int i)
{
    const Row *r = &rows[i];
    const char *v;
    if (!strcmp(r->key, "movies"))
        v = !option_bool("videos", 1) ? "none" : !option_bool("intro_videos", 1) ? "nointro" : "all";
    else
        v = option_str(r->key, r->def);
    for (int k = 0; k < nvalues(r); k++)
        if (!strcasecmp(v, r->values[k]))
            return k;
    if (!strcmp(r->key, "anisotropic") && atoi(v) <= 1) return nvalues(r) - 1;
    if (!strcmp(r->key, "display") && !strcasecmp(v, "windowed")) return 1;
    for (int k = 0; k < nvalues(r); k++)          /* (a value from options.txt the row lacks) */
        if (!strcasecmp(r->def, r->values[k]))
            return k;
    return 0;
}

static void row_set(int i, int k)
{
    const Row *r = &rows[i];
    const char *v = r->values[k];
    if (!strcmp(r->key, "movies")) {
        option_set("videos", strcmp(v, "none") ? "on" : "off");
        option_set("intro_videos", strcmp(v, "all") ? "off" : "on");
    } else
        option_set(r->key, v);
    options_save();
    win_options();
    frame_limit_options();
    vk_options_changed();
    smooth_options();
    pad_options();
    LOG(LOG_ALWAYS, "More Options: %s = %s", r->key, v);
}

static void make_page(Text *t)
{
    text_printf(t,
        "[desktop]\nwidth = 320\nheight = 256\nonBack = loadScript\nonBackP = o_op00.afw\n"
        "image = \"OPTbackground\" at(0,0); plane 4000;\nmouse = \"mouse_Main\"\n"
        "[button]\nid = done\nx = 290\ny = 220\nwidth = 25\nheight = 25\nfocusType = highlight\n"
        "image = \"done\" at (0,0);plane(10);\naction = loadscript\naparam = o_op00.afw\n"
        "[button]\nid = back\nx = 10\ny = 220\nwidth = 25\nheight = 25\nfocusType = highlight\n"
        "image = \"back\" at (0,0);plane(10);\naction = loadscript\naparam = o_op00.afw\n"
        "[button]\nid = b_label\nx = 50\ny = 23\nwidth = 200\nheight = 30\n"
        "text = string(natMore); centered(100,13); color(128,128,128); font(font1); plane(9);\n"
        "image = \"text_lefthook\" at(0,0); plane 10;\nimage = \"text_centertile\" at(50,0); plane 10;\n"
        "image = \"text_centertile\" at(70,0); plane 10;\nimage = \"text_centertile\" at(90,0); plane 10;\n"
        "image = \"text_centertile\" at(110,0); plane 10;\nimage = \"text_centertile\" at(120,0); plane 10;\n"
        "image = \"text_centertile\" at(140,0); plane 10;\nimage = \"text_righthook\" at(160,0); plane 10;\n");
    for (int i = 0; i < NROWS; i++) {
        int up = (i + NROWS - 1) % NROWS, down = (i + 1) % NROWS;
        text_printf(t,
            "[button]\nid = r%d\nx = 0\ny = 0\nwidth = 270\nheight = 10\nfocusType = Highlight\n"
            "image = \"very_small_button\" at 0, 1; plane 10;\n"
            "text = string(natL%d); at (10,4);color (94,94,20);font (font2);plane 10;\n"
            "text = string(natBlank) centered (212,4);id(v%d);color 128,128,128;plane 10;font(font2)\n"
            "image = \"small_left\" at (166,0); plane 5;\nimage = \"small_right\" at (250,0); plane 5;\n"
            "onUp = setFocus\nonUpP = r%d\nonDown = setFocus\nonDownP = r%d\n"
            "onRight = Native_Next\nonRightP = %d\nonLeft = Native_Prev\nonLeftP = %d\n"
            "action = Native_Next\naparam = %d\nhasFocus = %s\n"
            "[menu]\nid = m%d\nx = 22\ny = %d\nitem = r%d\n",
            i, i, i, up, down, i, i, i, i ? "no" : "yes", i, 51 + 11 * i, i);
    }
    text_printf(t, "[button]\nid = note\nx = 32\ny = 207\nwidth = 250\nheight = 10\n"
                   "text = string(natNote); at (0,4);color (80,80,80);font (font2);plane 10;\n");
}

/* ------------------------------------------------------------------ Cheat Menu: four more rows */
static const char *const unlock_ids[4] = {"AllTrucks", "AllTracks", "AllUpgrades", "AllDivisions"};

static void patch_cheats(Text *t)
{
    /* the six toggles and Apply moved down */
    static const char *const ids[6] = {"UnlimitedCash", "StuntMode", "BigAir", "Lowrider", "Sumo", "Mars"};
    char a[96], b[96];
    for (int i = 0; i < 6; i++) {
        snprintf(a, sizeof a, "y = %d\nitem = %s\n", 72 + 16 * i, ids[i]);
        snprintf(b, sizeof b, "y = %d\nitem = %s\n", 110 + 14 * i, ids[i]);
        replace(t, a, b);
    }
    replace(t, "id = Apply_Cheats\nx = 25\ny = 182\n", "id = Apply_Cheats\nx = 25\ny = 198\n");
    Text rowsx = {0};
    text_add(&rowsx, "", 0);
    for (int i = 0; i < 4; i++)
        text_printf(&rowsx,
            "[button]\nid = %s\nx = 0\ny = 0\nwidth = 240\nheight = 10\nfocusType = Highlight\n"
            "image = \"very_small_button\" at 0, 1; plane 10;\n"
            "text = string(%s); at (10,4);color (94,94,20);font (font2);plane 10;\n"
            "text = string(on) centered (200,4);id(natChk%d);color 128,128,128;plane 10;font(font2)\n"
            "image = \"small_left\" at (180,0); plane 5;\nimage = \"small_right\" at (210,0); plane 5;\n"
            "onDown = Next_Cheat\nonUp = Prev_Cheat\nonRight = Toggle_Cheat\nonLeft = Toggle_Cheat\n"
            "action = Toggle_Cheat\nonRollover = SetCurCheat\nonRolloverP = %s\n"
            "[menu]\nid = %sMenu\nx = 25\ny = %d\nitem = %s\nisVisible = no\n",
            unlock_ids[i], unlock_ids[i], i, unlock_ids[i], unlock_ids[i], 54 + 14 * i, unlock_ids[i]);
    text_add(&rowsx, "[button]\nid = UnlimitedCash\n", strlen("[button]\nid = UnlimitedCash\n"));
    replace(t, "[button]\nid = UnlimitedCash\n", rowsx.s);
    free(rowsx.s);
}

/* ------------------------------------------------------------------ the archive reader */
/* thiscall (archive, name, uint32_t *buffer, uint32_t *size): ret 12 */
static int arc_read_hook(CPU *c)
{
    const char *name = gstr(R32(c->esp + 4));
    uint32_t pbuf = R32(c->esp + 8), psize = R32(c->esp + 12);
    int which = !strcasecmp(name, "o_op00.afw") ? 1 : !strcasecmp(name, "o_nat.afw") ? 2
              : !strcasecmp(name, "o_op05.afw") ? 3 : 0;
    if (!which)
        return 0;
    Text t = {0};
    uint32_t old = 0;
    if (which == 2) {
        text_add(&t, "", 0);
        make_page(&t);
        c->ret = R32(c->esp);
        c->esp += 16;
    } else {
        F_00467b4a_orig(c);
        if (!c->eax)
            return 1;
        old = R32(pbuf);
        t = text_from((const char *)gptr(old), R32(psize));
        if (which == 1) patch_options(&t);
        else patch_cheats(&t);
    }
    uint32_t keep_eax = c->eax;
    uint32_t nb = guest_call(c, R32(OPERATOR_NEW), 1, (uint32_t)t.n + 8);
    memcpy(gptr(nb), t.s, t.n);
    W32(pbuf, nb);
    W32(psize, (uint32_t)t.n);
    if (old)
        guest_call(c, R32(OPERATOR_DELETE), 1, old);
    LOG(LOG_ALWAYS, "menus: %s %s (%zu bytes)", name, which == 2 ? "made" : "changed", t.n);
    free(t.s);
    c->eax = which == 2 ? 1 : keep_eax;
    return 1;
}

/* ------------------------------------------------------------------ texts on a page */
static uint32_t gtext(int slot, const char *s)
{
    static uint32_t buf[4];
    if (!buf[slot]) buf[slot] = galloc(128);
    gstrncpy(buf[slot], s, 128);
    return buf[slot];
}

static void set_text(CPU *c, uint32_t screen, const char *button, const char *id, const char *text)
{
    if (!screen) return;
    uint32_t keep = c->ecx;
    c->ecx = screen;
    guest_call(c, SET_TEXT, 4, gtext(0, button), gtext(1, id), gtext(2, text), 0u);
    c->ecx = keep;
}

static void show_row(CPU *c, uint32_t screen, int i)
{
    char b[16], v[16];
    snprintf(b, sizeof b, "r%d", i);
    snprintf(v, sizeof v, "v%d", i);
    set_text(c, screen, b, v, rows[i].names[row_value(i)]);
}

static int cheats_all(void) { return strcasecmp(option_str("cheats", "all"), "earned") != 0; }

/* thiscall (shell, name, flag): a script loaded and its page set up; ret 8 */
static int script_load_hook(CPU *c)
{
    const char *name = gstr(R32(c->esp + 4));
    int which = !strcasecmp(name, "o_nat.afw") ? 2 : !strcasecmp(name, "o_op05.afw") ? 3 : 0;
    if (!which)
        return 0;
    uint32_t shell = c->ecx;
    F_00480f1c_orig(c);
    uint32_t screen = R32(shell + 0x1d0), keep_eax = c->eax;
    if (which == 2)
        for (int i = 0; i < NROWS; i++)
            show_row(c, screen, i);
    else {
        uint32_t keep = c->ecx;
        for (int b = 1; b <= 4; b++) {
            char menu[32];
            snprintf(menu, sizeof menu, "%sMenu", unlock_ids[b - 1]);
            uint32_t on = R32(CHEATS_UNLOCKED) >> b & 1;
            c->ecx = screen;
            guest_call(c, MENU_SHOW, 2, gtext(0, menu), on);
            if (on) {
                c->ecx = CHEATS;
                guest_call(c, CHEAT_SHOW_STATE, 1, (uint32_t)b);
            }
        }
        c->ecx = keep;
    }
    c->eax = keep_eax;
    return 1;
}

/* thiscall (shell, action, parameter): a menu action; ret 8 */
static int menu_action_hook(CPU *c)
{
    const char *act = gstr(R32(c->esp + 4)), *par = R32(c->esp + 8) ? gstr(R32(c->esp + 8)) : "";
    int next = !strcasecmp(act, "Native_Next"), prev = !strcasecmp(act, "Native_Prev");
    if (!next && !prev)
        return 0;
    int i = atoi(par);
    if (i >= 0 && i < NROWS) {
        int n = nvalues(&rows[i]);
        row_set(i, (row_value(i) + (next ? 1 : n - 1)) % n);
        show_row(c, R32(c->ecx + 0x1d0), i);
    }
    c->eax = 1;
    c->ret = R32(c->esp);
    c->esp += 12;
    return 1;
}

/* thiscall (cheats, row, forward): the next unlocked row (1-10, 11 Apply); ret 8 */
static int cheat_step_hook(CPU *c)
{
    int row = (int)R32(c->esp + 4), fwd = (int)R32(c->esp + 8) != 0;
    uint32_t have = R32(CHEATS_UNLOCKED);
    for (int k = 0; k < 11; k++) {
        row = fwd ? (row >= 11 ? 1 : row + 1) : (row <= 1 ? 11 : row - 1);
        if (row == 11 || (have >> row & 1))
            break;
    }
    c->eax = (uint32_t)row;
    c->ret = R32(c->esp);
    c->esp += 12;
    return 1;
}

/* thiscall (cheats, n): row n's on / off shown; the game's table has no text id for 1-4
   (their rows were not in its menu); ret 4 */
static int cheat_show_hook(CPU *c)
{
    uint32_t b = R32(c->esp + 4);
    if (b < 1 || b > 4)
        return 0;
    char id[16];
    snprintf(id, sizeof id, "natChk%u", b - 1);
    uint32_t screen = R32(R32(CHEATS + 8) + 0x1d0), keep = c->ecx;
    if (screen) {
        c->ecx = screen;
        guest_call(c, SET_TEXT, 4, gtext(0, unlock_ids[b - 1]), gtext(1, id),
                   gtext(2, R32(CHEATS_ON) >> b & 1 ? "on" : "off"), 1u);
        c->ecx = keep;
    }
    c->ret = R32(c->esp);
    c->esp += 8;
    return 1;
}

/* thiscall (cheats, action, parameter): the Cheat Menu's actions; SetCurCheat for the rows
   added; ret 8 */
static int cheat_action_hook(CPU *c)
{
    const char *act = gstr(R32(c->esp + 4)), *par = R32(c->esp + 8) ? gstr(R32(c->esp + 8)) : "";
    if (strcasecmp(act, "SetCurCheat"))
        return 0;
    for (int b = 1; b <= 4; b++)
        if (!strcasecmp(par, unlock_ids[b - 1])) {
            W32(CHEAT_ROW, (uint32_t)b);
            c->eax = 0;
            c->ret = R32(c->esp);
            c->esp += 12;
            return 1;
        }
    return 0;
}

/* the texts of the added rows (string table keys, game.c's lookup) */
const char *menus_string(const char *key)
{
    if (!strcasecmp(key, "natMore")) return "More Options";
    if (!strcasecmp(key, "natBlank")) return " ";
    if (!strcasecmp(key, "natNote")) return "Texture filter: from the next start";
    if (!strncasecmp(key, "natL", 4) && isdigit((unsigned char)key[4])) {
        int i = atoi(key + 4);
        return i >= 0 && i < NROWS ? rows[i].label : NULL;
    }
    return NULL;
}

/* every frame: all cheats unlocked (option cheats = all) */
void menus_frame(void)
{
    if (cheats_all() && (R32(CHEATS_UNLOCKED) & 0x7fe) != 0x7fe)
        W32(CHEATS_UNLOCKED, R32(CHEATS_UNLOCKED) | 0x7fe);
}

void menus_init(void)
{
    hook_arc_read = arc_read_hook;
    hook_script_load = script_load_hook;
    hook_menu_action = menu_action_hook;
    hook_cheat_step = cheat_step_hook;
    hook_cheat_action = cheat_action_hook;
    hook_cheat_show = cheat_show_hook;
}
