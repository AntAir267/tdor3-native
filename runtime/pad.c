/* Controllers as the game's joystick (pad.h): the mapping SDL's gamepad events go through
   (win.c), and TDOR3_PAD, a scripted controller for headless tests that goes through the
   same calls.

   TDOR3 looks for joysticks once, at start-up, and uses the one tdor3.cfg names
   (Joystick_Index 0). Its bindings (JOY_CONTROLS; decoded at 0x457a1a) know two axes, eight
   buttons and the hat: 0x200/0x201 X left/right, 0x202/0x203 Y up/down, 0x204 + n button n,
   0x20c + 4 * hat + direction (up, right, down, left). The defaults steer with X, throttle
   and brake with Y up and down (analogue), shift up / down with buttons 0 / 1, e-brake 2,
   horn 3, rear view and looking left / right with the hat. So the runtime keeps one joystick
   connected from the start whatever is plugged in (controllers come and go behind it), laid
   out for those defaults:
     X  left stick (round dead zone)        Y  left trigger - right trigger (brake / throttle)
     Rx, Ry  right stick
     buttons 0 RB (shift up), 1 LB (shift down), 2 A (e-brake), 3 left stick click (horn),
             4 B, 5 X, 6 Y, 7 right stick click
     hat  the D-pad, else the right stick pushed (rear view down, look left / right)
   Every connected controller drives it (the one furthest from rest per axis). Menus and pause
   get keys from the controller (game.c). */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "input.h"
#include "options.h"
#include "pad.h"

#define JOY 0                            /* the joystick slot the game sees */

typedef struct {
    int connected;
    uint8_t button[PAD_BUTTONS];
    int axis[PAD_AXES];
} Pad;

static pthread_mutex_t pad_lock = PTHREAD_MUTEX_INITIALIZER;
static Pad pads[INPUT_MAX_JOYS];
static double stick_dead = 0.06 * 32767;
static int joy_ready;

static int clamp16(double v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int)lround(v); }

static int last_controller;             /* the device used last was a controller */
void input_used(int controller) { last_controller = controller != 0; }
int input_controller_last(void) { return last_controller; }

static void joy_ensure(void)
{
    if (joy_ready) return;
    joy_ready = 1;
    input_joy_connect(JOY, "Controller", 4, 8, 1);
}

static double steer_power = 1.0;          /* option steering: the left stick's x response */
static void joy_update(void);

/* a stick's pair through the round dead zone, the rest of the travel over the full range */
static void stick(int x, int y, int *ox, int *oy)
{
    double r = hypot(x, y), k = r > stick_dead ? (r - stick_dead) / (32767 - stick_dead) * 32767 / r : 0;
    *ox = clamp16(x * k);
    *oy = clamp16(y * k);
}

/* options controller_deadzone (percent, 6) and steering (linear as shipped: the game steers by
   the stick's x in proportion; soft / softer: less near the middle, full lock the same) */
void pad_options(void)
{
    const char *dz = option_str("controller_deadzone", NULL);
    double v = dz ? atof(dz) : 6;
    stick_dead = (v < 0 ? 0 : v > 90 ? 90 : v) / 100 * 32767;
    const char *s = option_str("steering", "linear");
    steer_power = !strcasecmp(s, "soft") ? 1.6 : !strcasecmp(s, "softer") ? 2.3 : 1.0;
    pthread_mutex_lock(&pad_lock);
    joy_update();
    pthread_mutex_unlock(&pad_lock);
}

/* the joystick from every connected controller (pad_lock held) */
static void joy_update(void)
{
    int ax[PAD_AXES] = {0};
    uint8_t bt[PAD_BUTTONS] = {0};
    for (int s = 0; s < INPUT_MAX_JOYS; s++) {
        if (!pads[s].connected) continue;
        for (int a = 0; a < PAD_AXES; a++)
            if (abs(pads[s].axis[a]) > abs(ax[a])) ax[a] = pads[s].axis[a];
        for (int b = 0; b < PAD_BUTTONS; b++)
            bt[b] |= pads[s].button[b];
    }
    int lx, ly, rx, ry;
    stick(ax[PAD_LX], ax[PAD_LY], &lx, &ly);
    stick(ax[PAD_RX], ax[PAD_RY], &rx, &ry);
    if (steer_power != 1.0)
        lx = clamp16((lx < 0 ? -1 : 1) * pow(abs(lx) / 32767.0, steer_power) * 32767);
    input_joy_axis(JOY, INPUT_AXIS_X, lx);
    input_joy_axis(JOY, INPUT_AXIS_Y, clamp16((double)ax[PAD_LT] - ax[PAD_RT]));
    input_joy_axis(JOY, INPUT_AXIS_Z, rx);
    input_joy_axis(JOY, INPUT_AXIS_RX, ry);
    static const int order[8] = {PAD_RB, PAD_LB, PAD_A, PAD_LSTICK, PAD_B, PAD_X, PAD_Y, PAD_RSTICK};
    for (int k = 0; k < 8; k++)
        input_joy_button(JOY, k, bt[order[k]]);
    int up = bt[PAD_DUP], dn = bt[PAD_DDOWN], l = bt[PAD_DLEFT], r = bt[PAD_DRIGHT];
    if (!(up | dn | l | r) && hypot(rx, ry) > 0.6 * 32767) {
        if (abs(rx) > abs(ry)) { r = rx > 0; l = rx < 0; }
        else { dn = ry > 0; up = ry < 0; }
    }
    int v = INPUT_POV_CENTERED;
    if (up && r) v = 4500; else if (r && dn) v = 13500;
    else if (dn && l) v = 22500; else if (l && up) v = 31500;
    else if (up) v = 0; else if (r) v = 9000; else if (dn) v = 18000; else if (l) v = 27000;
    input_joy_pov(JOY, 0, v);
}

/* Plugging a controller in counts as using it (the prompts switch to it); unplugging the last
   one hands them back to the keyboard. Returns the controller's number (its rumble). */
int pad_attach(const char *id, const char *name)
{
    (void)id; (void)name;
    joy_ensure();
    pthread_mutex_lock(&pad_lock);
    int slot = -1;
    for (int s = 0; s < INPUT_MAX_JOYS && slot < 0; s++)
        if (!pads[s].connected) slot = s;
    if (slot >= 0) {
        memset(&pads[slot], 0, sizeof pads[slot]);
        pads[slot].connected = 1;
        joy_update();
    }
    pthread_mutex_unlock(&pad_lock);
    if (slot >= 0) {
        input_used(1);
        LOG(LOG_ALWAYS, "controller %d: %s", slot, name ? name : "?");
    }
    return slot;
}

void pad_detach(int slot)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS)
        return;
    pthread_mutex_lock(&pad_lock);
    memset(&pads[slot], 0, sizeof pads[slot]);
    joy_update();
    pthread_mutex_unlock(&pad_lock);
    if (!pad_count())
        input_used(0);
}

void pad_axis(int slot, int axis, int value)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS || axis < 0 || axis >= PAD_AXES)
        return;
    pthread_mutex_lock(&pad_lock);
    pads[slot].axis[axis] = value;
    joy_update();
    pthread_mutex_unlock(&pad_lock);
    if (abs(value) > 12000)
        input_used(1);
}

void pad_button(int slot, int button, int down)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS || button < 0 || button >= PAD_BUTTONS)
        return;
    if (down) {
        input_used(1);
        video_skip_press();             /* (any button skips a movie: game.c) */
    }
    pthread_mutex_lock(&pad_lock);
    pads[slot].button[button] = down != 0;
    joy_update();
    pthread_mutex_unlock(&pad_lock);
}

int pad_held(int button)
{
    int r = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        r |= pads[s].connected && pads[s].button[button];
    pthread_mutex_unlock(&pad_lock);
    return r;
}

int pad_axis_value(int axis)
{
    int r = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        if (pads[s].connected && abs(pads[s].axis[axis]) > abs(r))
            r = pads[s].axis[axis];
    pthread_mutex_unlock(&pad_lock);
    return r;
}

int pad_count(void)
{
    int n = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        n += pads[s].connected;
    pthread_mutex_unlock(&pad_lock);
    return n;
}

/* ------------------------------------------------------------------ TDOR3_PAD */
/* axis: 0 a button, 1 an axis, 2 plugging (index 0 unplug, 1 the controller, 2 another one) */
typedef struct { unsigned long at, release; int axis, index, value, hold, state; } PadPress;
static PadPress *script;
static int nscript, script_slot = -1;
static const char *const plug_names[3] = {"UNPLUG", "PLUG", "PLUG2"};

static const struct { const char *name; int button; } button_names[] = {
    {"A", PAD_A}, {"B", PAD_B}, {"X", PAD_X}, {"Y", PAD_Y}, {"BACK", PAD_BACK}, {"VIEW", PAD_BACK},
    {"SELECT", PAD_BACK}, {"GUIDE", PAD_GUIDE}, {"START", PAD_START}, {"MENU", PAD_START},
    {"LS", PAD_LSTICK}, {"L3", PAD_LSTICK}, {"RS", PAD_RSTICK}, {"R3", PAD_RSTICK},
    {"LB", PAD_LB}, {"RB", PAD_RB}, {"UP", PAD_DUP}, {"DOWN", PAD_DDOWN}, {"LEFT", PAD_DLEFT},
    {"RIGHT", PAD_DRIGHT}, {"MISC", PAD_MISC1},
};
static const char *const axis_names[PAD_AXES] = {"LX", "LY", "RX", "RY", "LT", "RT"};

void pad_init(void)
{
    joy_ensure();
    pad_options();
    const char *env = getenv("TDOR3_PAD");
    if (!env || !*env)
        return;
    char *copy = strdup(env), *save = NULL;
    for (char *tok = strtok_r(copy, ", \t\n", &save); tok; tok = strtok_r(NULL, ", \t\n", &save)) {
        char *colon = strchr(tok, ':'), *end;
        unsigned long at = colon ? strtoul(tok, &end, 10) : 0;
        if (!colon || end != colon) {
            LOG(LOG_ALWAYS, "TDOR3_PAD: \"%s\" is not frame:BUTTON or frame:AXIS=value", tok);
            continue;
        }
        char *name = colon + 1, *star = strchr(name, '*'), *eq = strchr(name, '=');
        int hold = 3;
        if (star) {
            *star = 0;
            hold = atoi(star + 1) > 0 ? atoi(star + 1) : 1;
        }
        PadPress p = {at, 0, 0, -1, 1, hold, 0};
        for (int k = 0; k < 3 && !eq; k++)
            if (!strcasecmp(name, plug_names[k])) {
                p.axis = 2;
                p.index = k;
            }
        if (p.axis == 2) {
        } else if (eq) {
            *eq = 0;
            p.axis = 1;
            p.value = atoi(eq + 1);
            for (int a = 0; a < PAD_AXES; a++)
                if (!strcasecmp(name, axis_names[a]))
                    p.index = a;
        } else {
            for (size_t k = 0; k < sizeof button_names / sizeof *button_names; k++)
                if (!strcasecmp(name, button_names[k].name))
                    p.index = button_names[k].button;
        }
        if (p.index < 0) {
            LOG(LOG_ALWAYS, "TDOR3_PAD: unknown %s \"%s\"", eq ? "axis" : "button", name);
            continue;
        }
        script = realloc(script, (nscript + 1) * sizeof *script);
        script[nscript++] = p;
    }
    free(copy);
    int first = -1;                 /* the first plugging event: if it plugs in, none at start-up */
    for (int k = 0; k < nscript; k++)
        if (script[k].axis == 2 && (first < 0 || script[k].at < script[first].at))
            first = k;
    if (first >= 0 && script[first].index) {
        LOG(LOG_ALWAYS, "TDOR3_PAD: %d scripted inputs, the controller not connected yet", nscript);
        return;
    }
    script_slot = pad_attach("tdor3-pad-script", "Scripted controller");
    LOG(LOG_ALWAYS, "TDOR3_PAD: controller connected as joystick %d, %d scripted inputs", script_slot, nscript);
}

static void script_plug(unsigned long frame, int which)
{
    if (script_slot >= 0) {
        LOG(LOG_ALWAYS, "TDOR3_PAD: frame %lu: joystick %d unplugged", frame, script_slot);
        pad_detach(script_slot);
        script_slot = -1;
    }
    if (which) {
        script_slot = which == 1 ? pad_attach("tdor3-pad-script", "Scripted controller")
                                 : pad_attach("tdor3-pad-script-2", "Another scripted controller");
        LOG(LOG_ALWAYS, "TDOR3_PAD: frame %lu: %s connected as joystick %d", frame,
            which == 1 ? "controller" : "another controller", script_slot);
    }
}

void pad_frame(unsigned long frame)
{
    for (int k = 0; k < nscript; k++) {
        PadPress *s = &script[k];
        int down = -1;
        if (s->state == 0 && frame >= s->at) {
            s->state = 1;
            s->release = frame + (unsigned long)s->hold;
            down = 1;
        } else if (s->state == 1 && frame >= s->release) {
            s->state = 2;
            down = 0;
        }
        if (down < 0)
            continue;
        if (s->axis == 2) {
            if (down)
                script_plug(frame, s->index);
        } else if (s->axis) {
            LOG(LOG_ALWAYS, "TDOR3_PAD: frame %lu: %s = %d", frame, axis_names[s->index], down ? s->value : 0);
            pad_axis(script_slot, s->index, down ? s->value : 0);
        } else {
            const char *n = "?";
            for (size_t b = 0; b < sizeof button_names / sizeof *button_names; b++)
                if (button_names[b].button == s->index) {
                    n = button_names[b].name;
                    break;
                }
            LOG(LOG_ALWAYS, "TDOR3_PAD: frame %lu: %s %s", frame, n, down ? "down" : "up");
            pad_button(script_slot, s->index, down);
        }
    }
    padgame_frame(frame);
}
