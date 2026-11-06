/* TDOR3-specific glue between the runtime's devices and the game: the controller in menus and
   at pause (keys, as the keyboard would give them), rumble, skipping videos.

   The game's menus answer keys only; a race reads the joystick itself (pad.c lays it out for
   the game's own bindings). So outside a race - and in one while it is paused - the controller
   types: D-pad or left stick the arrows (repeating while held), A Enter, B and Back Escape,
   Start Enter. Some menu buttons answer the mouse only (the controls page's Controller tab and
   its arrows): the right stick moves the game's pointer, and A clicks where it is until the
   D-pad or left stick is used again. In a race the keyboard-only commands (the exe's accelerator table) get buttons:
   Start Escape (pause), View the next camera (F1-F5), Y ignition (I), X lights (L). Paused
   ("press Q to quit, Enter to race"), View is Q.
   Prompts: the game's own lines naming keys are rewritten in place while the controller is
   the device used last (and put back for the keyboard). The controls pages (Options >
   Controls) take their labels from the menu string table (UIData scripts.arc strings.dat,
   looked up by key in 0x4938ed): the keyboard page's key column names the controller's buttons
   while it is the device used last. The joystick page builds each row's name from the
   joystick's DirectInput object name and a direction (0x48ad88, by binding code); those become
   the controller's names for pad.c's layout. */
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "input.h"
#include "pad.h"
#include "funcs.h"
#include "win.h"

#define RACE_ON 0x4e67fdu                /* a race is running (the race loop at 0x44fcd5) */
#define RACE_PAUSED 0x4e67ffu            /* its time stands still */
#define CAMERA_MODE 0x4b27ccu            /* 0-4 the F1-F5 views (0x42fc64) */

/* the ignition, quit and race-on lines (the game's own text, kept from memory the first time
   to put back for the keyboard) */
static const struct { uint32_t at; const char *pad; } prompts[] = {
    {0x4b4564, "Press Y to start the engine."},
    {0x4b48f4, "Press View to quit."},
    {0x4b4908, "Press A to race."},
};
#define NPROMPTS (sizeof prompts / sizeof prompts[0])

static void prompts_for(int controller)
{
    static int shown = -1;
    static char keys[NPROMPTS][64];
    if (controller == shown)
        return;
    if (shown < 0)
        for (size_t k = 0; k < NPROMPTS; k++)
            snprintf(keys[k], sizeof keys[k], "%s", (const char *)gptr(prompts[k].at));
    shown = controller;
    for (size_t k = 0; k < NPROMPTS; k++) {
        const char *t = controller ? prompts[k].pad : keys[k];
        size_t room = strlen(keys[k]);
        memset(gptr(prompts[k].at), 0, room);
        memcpy(gptr(prompts[k].at), t, strlen(t) < room ? strlen(t) : room);
    }
}

#include <math.h>
#include "options.h"

/* Rumble (option rumble, on). The game has no force feedback of its own (it notes whether a
   joystick has it and never makes an effect), so knocks are felt from the player's car: its
   velocity (car 0 at 0x4e4380, +0x4c, 3 floats) changing more in one physics step than
   driving can - a collision or a hard landing - shakes the controller by how much. Called
   once per step (smooth.c's hook on 0x430e72). */
#define PLAYER_CAR 0x4e4380u

int ifc_rumble_wanted(void) { return option_bool("rumble", 1); }
void menus_frame(void);
void ifc_frame(unsigned long frame) { (void)frame; menus_frame(); }

/* TDOR3_AUTOPILOT=1 (tests): the game's own AI drives the player's car (control mode at
   car +0x5c0: 1 the player, 2 the AI; 0x446cef(car, mode, skill) sets it, skill as the first
   opponent's, 0x4d79e8 + 0x18) so a test can finish races */
static void autopilot(void)
{
    static int on = -1;
    if (on < 0) on = getenv("TDOR3_AUTOPILOT") != NULL;
    if (!on || R32(PLAYER_CAR + 0x5c0) != 1)
        return;
    CPU *c = current_cpu();
    uint32_t keep = c->ecx;
    c->ecx = PLAYER_CAR;
    guest_call(c, 0x446cef, 2, 2u, R32(0x4d79e8 + 0x18));
    c->ecx = keep;
    LOG(LOG_ALWAYS, "autopilot: the AI drives the player's car");
}

void game_race_step(void)
{
    if (R8(RACE_ON) && !R8(RACE_PAUSED))
        autopilot();
    static int carlog = -1;
    if (carlog < 0) carlog = getenv("TDOR3_CAR_LOG") != NULL;
    if (carlog && R8(RACE_ON) && !R8(RACE_PAUSED)) {   /* (tests: the player's car each physics step) */
        static unsigned long step;
        float f[12];
        memcpy(f, gptr(PLAYER_CAR + 0x30), sizeof f);
        LOG(LOG_ALWAYS, "car %lu: %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g", step++, f[0], f[1],
            f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
    }
    static float last[3];
    static int have;
    if (!R8(RACE_ON) || R8(RACE_PAUSED) || !pad_count()) {
        have = 0;
        return;
    }
    float v[3];
    for (int k = 0; k < 3; k++) {
        uint32_t u = R32(PLAYER_CAR + 0x4c + 4 * (uint32_t)k);
        memcpy(&v[k], &u, 4);
    }
    if (!isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2])) { have = 0; return; }
    if (have) {
        double dx = v[0] - last[0], dy = v[1] - last[1], dz = v[2] - last[2];
        double d = sqrt(dx * dx + dy * dy + dz * dz);
        if (d > 1.5 && ifc_rumble_wanted()) {
            double k = (d - 1.5) / 6.0;
            if (k > 1) k = 1;
            uint16_t low = (uint16_t)(0x3000 + k * 0xcfff), high = (uint16_t)(0x2000 + k * 0x8000);
            LOG(LOG_INPUT, "rumble: the car's velocity changed by %.1f in a step: %04x/%04x", d, low, high);
            for (int s = 0; s < INPUT_MAX_JOYS; s++)
                input_joy_rumble(s, low, high, (uint32_t)(120 + 180 * k));
        }
    }
    memcpy(last, v, sizeof last);
    have = 1;
}

/* a key typed for the controller (which stays the device used last, for the prompts) */
static void key(int dik, int down)
{
    input_key(dik, down);
    if (input_script_key_hook)
        input_script_key_hook(dik, down);
    input_used(1);
}

typedef struct { int dik, held; double next; } Rep;

/* a key held while `on`, pressed again every so often for menu lists */
static void repeat(Rep *r, int on, double now)
{
    if (on && !r->held) {
        key(r->dik, 1);
        r->held = 1;
        r->next = now + 400;
    } else if (on && now >= r->next) {
        key(r->dik, 0);
        key(r->dik, 1);
        r->next = now + 110;
    } else if (!on && r->held) {
        key(r->dik, 0);
        r->held = 0;
    }
}

static void press(int *held, int on, int dik)
{
    if (on != *held) {
        key(dik, on);
        *held = on;
    }
}

void padgame_frame(unsigned long frame)
{
    (void)frame;
    static Rep up = {DIK_UP}, down = {DIK_DOWN}, left = {DIK_LEFT}, right = {DIK_RIGHT};
    static int a, b, start, quit, ign, lights, cam, cam_key;
    prompts_for(input_controller_last());
    if (!pad_count() && !up.held && !down.held && !left.held && !right.held && !a && !b && !start && !quit && !cam)
        return;
    double now = now_ms();
    int racing = R8(RACE_ON) && !R8(RACE_PAUSED);
    int menu = !racing;
    int lx = pad_axis_value(PAD_LX), ly = pad_axis_value(PAD_LY);
    repeat(&up, menu && (pad_held(PAD_DUP) || ly < -20000), now);
    repeat(&down, menu && (pad_held(PAD_DDOWN) || ly > 20000), now);
    repeat(&left, menu && (pad_held(PAD_DLEFT) || lx < -20000), now);
    repeat(&right, menu && (pad_held(PAD_DRIGHT) || lx > 20000), now);
    int paused = R8(RACE_ON) && R8(RACE_PAUSED);
    /* the pointer: right stick, A clicks once it has moved */
    static double last_t, px = 320, py = 240;
    static int pointing, clicking;
    double dt = last_t ? now - last_t : 0;
    last_t = now;
    if (dt > 100) dt = 100;
    int rx = pad_axis_value(PAD_RX), ry = pad_axis_value(PAD_RY);
    double r = sqrt((double)rx * rx + (double)ry * ry);
    if (menu && r > 8000) {
        pointing = 1;
        double k = (r - 8000) / (32767 - 8000);
        double v = 0.7 * k * k / r;          /* up to 700 game pixels a second */
        px += rx * v * dt;
        py += ry * v * dt;
        px = px < 0 ? 0 : px > 639 ? 639 : px;
        py = py < 0 ? 0 : py > 479 ? 479 : py;
        host_mouse_at((int)px, (int)py);
    } else if (!menu || up.held || down.held || left.held || right.held)
        pointing = 0;
    if (!pointing) {
        int mx, my;
        host_mouse_where(&mx, &my);          /* (the mouse may have moved it) */
        px = mx;
        py = my;
    }
    int click = menu && pointing && pad_held(PAD_A);
    if (click != clicking) {
        host_mouse_button(0, click);
        clicking = click;
    }
    press(&a, menu && !pointing && !clicking && pad_held(PAD_A), DIK_RETURN);
    press(&b, menu && (pad_held(PAD_B) || (!paused && pad_held(PAD_BACK))), DIK_ESCAPE);
    press(&quit, paused && pad_held(PAD_BACK), DIK_Q);
    press(&ign, racing && pad_held(PAD_Y), DIK_I);
    press(&lights, racing && pad_held(PAD_X), DIK_L);
    static int view_used;                /* View pressed outside a race: not a camera press until let go */
    if (!pad_held(PAD_BACK)) view_used = 0;
    else if (!racing) view_used = 1;
    int view = racing && pad_held(PAD_BACK) && !view_used;
    if (view && !cam) {
        int next = ((int)R32(CAMERA_MODE) + 1) % 5;
        cam_key = DIK_F1 + (next < 0 ? 0 : next);
        key(cam_key, 1);
    } else if (!view && cam)
        key(cam_key, 0);
    cam = view;
    int st = pad_held(PAD_START);
    if (st && !start)
        key(start = racing ? DIK_ESCAPE : DIK_RETURN, 1);
    else if (!st && start) {
        key(start, 0);
        start = 0;
    }
}

/* ------------------------------------------------------------------ the controls pages */
static const struct { const char *key, *pad_only, *always; } labels[] = {
    {"contConf", NULL, "Controller"},     /* the keyboard page's button to the joystick page */
    /* the keyboard page (o_op02.afw), with the controller in use: its default bindings */
    {"Keyboard", "Controller", NULL},
    {"LeftArrow", "Left Stick:", NULL},           {"RightArrow", "Left Stick:", NULL},
    {"UpArrow", "Right Trigger:", NULL},          {"DownArrow", "Left Trigger:", NULL},
    {"PgUp", "Right Bumper:", NULL},              {"PgDn", "Left Bumper:", NULL},
    {"End", "A Button:", NULL},                          {"R", "D-Pad Down:", NULL},
    {"L", "X Button:", NULL},                            {"H", "Left Stick Click:", NULL},
    {"I", "Y Button:", NULL},                            {"Esc", "Start:", NULL},
    {"F1F11", "View:", NULL},
};
static uint32_t label_text[sizeof labels / sizeof labels[0]][2];

/* [esp+4] the key (a string object: its text at +4); returns the text or 0 */
static int string_lookup_hook(CPU *c)
{
    uint32_t keyobj = R32(c->esp + 4);
    const char *key = keyobj ? gstr(R32(keyobj + 4)) : NULL;
    F_004938ed_orig(c);
    if (key && !c->eax) {                    /* the texts of the pages menus.c adds */
        extern const char *menus_string(const char *key);
        const char *t = menus_string(key);
        static struct { char key[16]; uint32_t text; } made[32];
        static int nmade;
        if (t) {
            for (int k = 0; k < nmade && !c->eax; k++)
                if (!strcmp(made[k].key, key)) c->eax = made[k].text;
            if (!c->eax && nmade < 32) {
                snprintf(made[nmade].key, sizeof made[nmade].key, "%s", key);
                c->eax = made[nmade++].text = gstrdup(t);
            }
        }
        return 1;
    }
    if (!key || !c->eax)
        return 1;
    for (size_t k = 0; k < sizeof labels / sizeof labels[0]; k++) {
        if (strcasecmp(key, labels[k].key))
            continue;
        int pad = labels[k].always ? 1 : input_controller_last();
        const char *t = labels[k].always ? labels[k].always : pad ? labels[k].pad_only : NULL;
        if (t) {
            if (!label_text[k][0])
                label_text[k][0] = gstrdup(t);
            c->eax = label_text[k][0];
        }
        break;
    }
    return 1;
}

/* the joystick page's row name: thiscall (page), [esp+4] the row (its binding code at +0),
   [esp+8] the object name, [esp+12] the text out */
static int joy_label_hook(CPU *c)
{
    static const char *const names[12] = {
        "Left Stick Left", "Left Stick Right", "Right Trigger", "Left Trigger",
        "Right Bumper", "Left Bumper", "A Button", "Left Stick Click",
        "B Button", "X Button", "Y Button", "Right Stick Click",
    };
    uint32_t row = R32(c->esp + 4), out = R32(c->esp + 12);
    F_0048ad88_orig(c);
    uint32_t code = row ? R32(row) : 0;
    if (out && code >= 0x200 && code < 0x20c)
        strcpy(gptr(out), names[code - 0x200]);
    return 1;
}

static int movie_frame_hook(CPU *c);

void game_init(void)
{
    hook_string_lookup = string_lookup_hook;
    hook_joy_label = joy_label_hook;
    hook_movie_frame = movie_frame_hook;
}

/* ------------------------------------------------------------------ skipping movies
   A movie plays by 0x4130e3 (thiscall movie, srGERD *): a frame drawn, false at the end. The
   game ends one on WM_KEYDOWN / WM_COMMAND, but only those it plays from the main loop: the
   legal screen at start-up plays in a loop of its own that reads no messages (8 s; presses
   made there waited in the queue and then skipped the intro). Any key or controller button
   pressed while a movie plays now ends it at its next frame, the legal screen included; the
   queued key messages of that press are dropped so they do not end the next movie too. */
static volatile int skip_pending;
static uint32_t skip_movie;

void video_skip_press(void) { skip_pending = 1; }

static int movie_frame_hook(CPU *c)
{
    uint32_t movie = c->ecx;
    if (movie != skip_movie) {               /* a new movie: presses before it do not count */
        skip_movie = movie;
        skip_pending = 0;
    }
    if (!skip_pending)
        return 0;
    skip_pending = 0;
    LOG(LOG_ALWAYS, "movie skipped");
    drop_key_messages();
    c->eax &= ~0xffu;                        /* (false: it has ended) */
    c->ret = R32(c->esp);
    c->esp += 8;
    return 1;
}
