/* Test Drive Off-Road 3 on the AntiStatic runtime: what this port adds to the core (port.h's
   hooks). Its own pieces (the SurRender display driver srdd.c on vk.c, Bink on FFmpeg, CD audio,
   the widescreen scenes, smoothing, HUD, menus, game.c) start here, the machine the game sees is
   set here, and its joystick is laid out for the game's bindings. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "input.h"
#include "options.h"
#include "pad.h"

void srdd_init(void);
void bink_init(void);
void crt_init(void);
void scene_init(void);
void smooth_init(void);
void game_hooks_init(void);
void hud_init(void);
void menus_init(void);
void music_poll(void);
void padgame_frame(unsigned long frame);
void vk_window_to_game(double px, double py, int *gx, int *gy);
int vk_write_png(const char *path);

/* ------------------------------------------------------------------ the machine
   A Pentium II class processor without MMX, so SurRender keeps to its plain x87 paths and picks
   the generic vector processor (the only one this build translates); RDTSC at PORT_TSC_HZ. */
void cpu_cpuid(CPU *c)
{
    uint32_t leaf = c->eax;
    switch (leaf) {
    case 0:
        c->eax = 1;
        c->ebx = 0x756e6547u;           /* "GenuineIntel" */
        c->edx = 0x49656e69u;
        c->ecx = 0x6c65746eu;
        break;
    case 1:
        c->eax = PORT_CPU_ID_VERSION;    /* family 6, model 5: Pentium II */
        c->ebx = 0;
        c->ecx = 0;
        c->edx = PORT_CPU_ID_FEATURES;   /* FPU, TSC, CX8, CMOV; no MMX */
        break;
    default:
        c->eax = c->ebx = c->ecx = c->edx = 0;
        break;
    }
}

/* SurRender's timer calibration (srTimer, sr.dll 0x100693b0: RDTSC ticks per second measured
   over Sleep(5000), kept under HKCU\SOFTWARE\Hybrid\hrtimer with the CPUID it was measured on)
   is the known rate of the runtime's RDTSC: no five-second wait, and right in test runs too */
static void seed_timer(void)
{
    static const char k[] = "HKCU\\SOFTWARE\\Hybrid\\hrtimer";
    uint64_t hz = (uint64_t)PORT_TSC_HZ;
    uint32_t maxid = 1, vers = PORT_CPU_ID_VERSION, feat = PORT_CPU_ID_FEATURES;
    registry_set(k, "hrticks", 3, &hz, 8);
    registry_set(k, "hrsignature", 3, "GenuineIntel", 13);
    registry_set(k, "hrmid", 3, &maxid, 4);
    registry_set(k, "hrvers", 3, &vers, 4);
    registry_set(k, "hrfeat", 3, &feat, 4);
}

/* ------------------------------------------------------------------ start-up */
void port_graphics_init(void)
{
    srdd_init();                         /* srDD_DirectX6.dll, the display driver the game loads */
    bink_init();
}

void game_init(void)
{
    seed_timer();
    crt_init();
    /* TDOR3_PLAIN=1: none of the runtime's hooks into the game's own functions (the
       enhancements), as the reference build runs it (tools/lockstep.py) */
    if (!port_getenv("PLAIN")) {
        scene_init();
        smooth_init();
        game_hooks_init();
        hud_init();
        menus_init();
    }
}

void game_frame(unsigned long frame)
{
    (void)frame;
    music_poll();
}

int d3d_write_png(const char *path) { return vk_write_png(path); }

/* the mouse: a window pixel through the scaling and bars to the game's 640x480 */
int port_window_to_game(float x, float y, float *gx, float *gy)
{
    int ix, iy;
    vk_window_to_game(x, y, &ix, &iy);
    *gx = (float)ix;
    *gy = (float)iy;
    return 1;
}

/* ------------------------------------------------------------------ the joystick
   TDOR3 looks for joysticks once, at start-up. Its bindings (JOY_CONTROLS; decoded at 0x457a1a)
   know two axes, eight buttons and the hat: 0x200/0x201 X left/right, 0x202/0x203 Y up/down,
   0x204 + n button n, 0x20c + 4 * hat + direction. The defaults steer with X, throttle and brake
   with Y up and down (analogue), shift up / down with buttons 0 / 1, e-brake 2, horn 3, rear view
   and looking left / right with the hat. So:
     X  left stick (option steering: soft / softer bends it)   Y  left trigger - right trigger
     Rx, Ry  right stick
     buttons 0 RB (shift up), 1 LB (shift down), 2 A (e-brake), 3 left stick click (horn),
             4 B, 5 X, 6 Y, 7 right stick click
     hat  the D-pad, else the right stick pushed (rear view down, look left / right) */
static int clamp16(double v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int)lround(v); }

int port_pad_send(int joy, const PadState *st)
{
    const char *s = option_str("steering", "linear");
    double power = !strcasecmp(s, "soft") ? 1.6 : !strcasecmp(s, "softer") ? 2.3 : 1.0;
    int lx = st->axis[PAD_LX];
    if (power != 1.0)
        lx = clamp16((lx < 0 ? -1 : 1) * pow(abs(lx) / 32767.0, power) * 32767);
    int rx = st->axis[PAD_RX], ry = st->axis[PAD_RY];
    input_joy_axis(joy, INPUT_AXIS_X, lx);
    input_joy_axis(joy, INPUT_AXIS_Y, clamp16((double)st->axis[PAD_LT] - st->axis[PAD_RT]));
    input_joy_axis(joy, INPUT_AXIS_Z, rx);
    input_joy_axis(joy, INPUT_AXIS_RX, ry);
    static const int order[8] = {PAD_RB, PAD_LB, PAD_A, PAD_LSTICK, PAD_B, PAD_X, PAD_Y, PAD_RSTICK};
    for (int k = 0; k < 8; k++)
        input_joy_button(joy, k, st->button[order[k]]);
    int up = st->button[PAD_DUP], dn = st->button[PAD_DDOWN], l = st->button[PAD_DLEFT], r = st->button[PAD_DRIGHT];
    if (!(up | dn | l | r) && hypot(rx, ry) > 0.6 * 32767) {
        if (abs(rx) > abs(ry)) { r = rx > 0; l = rx < 0; }
        else { dn = ry > 0; up = ry < 0; }
    }
    int v = INPUT_POV_CENTERED;
    if (up && r) v = 4500; else if (r && dn) v = 13500;
    else if (dn && l) v = 22500; else if (l && up) v = 31500;
    else if (up) v = 0; else if (r) v = 9000; else if (dn) v = 18000; else if (l) v = 27000;
    input_joy_pov(joy, 0, v);
    return 1;
}

const char *port_pad_name(int joy) { (void)joy; return "Controller"; }

/* per frame: a button pressed skips a movie playing (game.c); the menus and pause by pad */
void port_pad_frame(unsigned long frame)
{
    static uint8_t was[PAD_BUTTONS];
    for (int b = 0; b < PAD_BUTTONS; b++) {
        int now = pad_held(b);
        if (now && !was[b])
            video_skip_press();
        was[b] = (uint8_t)now;
    }
    padgame_frame(frame);
}

/* ------------------------------------------------------------------ options.txt's comments */
const char *port_option_help(const char *key)
{
    static const char *const help[][2] = {
        {"hud", "hud: edges (the race HUD at the screen's edges in widescreen) or middle (kept in the 4:3 middle)"},
        {"smooth", "smooth: on (motion drawn between the game's 30 physics steps a second) or off"},
        {"videos", "videos: on or off (every movie: intro, cup wins and losses, credits)"},
        {"intro_videos", "intro_videos: on or off (the legal screen and intro at start-up)"},
        {"steering", "steering: linear (as shipped), soft or softer (less steering near the stick's middle)"},
        {"cheats", "cheats: all (every cheat in Options > Cheat Menu from the start) or earned (as shipped)"},
        {NULL, NULL}};
    for (int i = 0; help[i][0]; i++)
        if (!strcasecmp(help[i][0], key))
            return help[i][1];
    return NULL;
}
