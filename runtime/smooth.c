/* Smooth motion above the physics rate (DESIGN.md goals; option "smooth", on by default).

   A race runs its physics in fixed 33 ms steps (the loop at 0x44fcd5: elapsed ms into the
   accumulator [0x4e6850], a step ([0x4b4730] ms) while it holds more than one, at most five),
   then draws once (0x4221e0). Between steps every frame drew the same state: 30 different
   pictures a second however fast the screen. Here the frame shows the state between the last
   two steps instead, by how far time has gone into the next (accumulator / step): one step of
   latency for motion at the screen's rate.

   It is done on SurRender's scene nodes. A node's transform is its rotation (3x3 doubles at
   +0x18) and location (3 doubles at +0x60); every change goes through srNode::setWSDirty
   (0x10059d90). Nodes changed during the race loop are tracked (dropped in ~srNode,
   0x100590a0). When the frame's scenes are drawn (0x41e526, the track's sectors with the cars,
   then the rest in 0x4221e0) each tracked node is read: after a step (0x430e72 runs once per
   step) its previous and current transforms move on; the node is drawn at the blend and put
   back after the frame. Nodes that change between steps (animated by the frame) and jumps
   (resets, camera cuts) are drawn as they are. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "options.h"
#include "funcs.h"

#define NODE_SET_WS_DIRTY 0x10059d90u
#define ACCUMULATOR 0x4e6850u
#define STEP_MS 0x4b4730u
#define RACE_ON 0x4e67fdu

typedef struct {
    uint32_t node;
    int has, frame_anim, written;
    double prev[12], cur[12];
} Node;

static Node *nodes;
static int nnodes, cap;
static int *index_of;                    /* open addressing: node -> index + 1 */
static int index_cap;
static int tracking, applied, on = -1;
static unsigned long steps, steps_seen;

static unsigned hash(uint32_t a) { return (a >> 3) * 2654435761u; }

static void rebuild_index(void)
{
    free(index_of);
    index_cap = 1024;
    while (index_cap < nnodes * 4) index_cap *= 2;
    index_of = calloc((size_t)index_cap, sizeof *index_of);
    for (int i = 0; i < nnodes; i++) {
        unsigned h = hash(nodes[i].node) & (unsigned)(index_cap - 1);
        while (index_of[h]) h = (h + 1) & (unsigned)(index_cap - 1);
        index_of[h] = i + 1;
    }
}

static int find(uint32_t node)
{
    if (!index_cap) return -1;
    unsigned h = hash(node) & (unsigned)(index_cap - 1);
    while (index_of[h]) {
        if (nodes[index_of[h] - 1].node == node) return index_of[h] - 1;
        h = (h + 1) & (unsigned)(index_cap - 1);
    }
    return -1;
}

static void track(uint32_t node)
{
    if (find(node) >= 0) return;
    if (nnodes == cap) {
        cap = cap ? cap * 2 : 256;
        nodes = realloc(nodes, sizeof *nodes * (size_t)cap);
    }
    memset(&nodes[nnodes], 0, sizeof nodes[0]);
    nodes[nnodes++].node = node;
    if (nnodes * 2 > index_cap) rebuild_index();
    else {
        unsigned h = hash(node) & (unsigned)(index_cap - 1);
        while (index_of[h]) h = (h + 1) & (unsigned)(index_cap - 1);
        index_of[h] = nnodes;
    }
}

static void untrack(uint32_t node)
{
    int i = find(node);
    if (i < 0) return;
    nodes[i] = nodes[--nnodes];
    rebuild_index();
}

static void read_tf(uint32_t node, double *v)
{
    for (int k = 0; k < 12; k++) v[k] = RF64(node + 0x18 + 8 * (uint32_t)k);
}

static void write_tf(CPU *c, uint32_t node, const double *v)
{
    for (int k = 0; k < 12; k++) WF64(node + 0x18 + 8 * (uint32_t)k, v[k]);
    uint32_t keep = c->ecx;
    c->ecx = node;
    guest_call(c, NODE_SET_WS_DIRTY, 0);
    c->ecx = keep;
}

/* a jump, not motion: drawn as it is */
static int jump(const double *a, const double *b)
{
    double dx = a[9] - b[9], dy = a[10] - b[10], dz = a[11] - b[11];
    if (dx * dx + dy * dy + dz * dz > 30.0 * 30.0) return 1;
    for (int r = 0; r < 3; r++) {       /* each axis turned by more than ~45 degrees */
        double d = 0, la = 0, lb = 0;
        for (int k = 0; k < 3; k++) {
            d += a[r * 3 + k] * b[r * 3 + k];
            la += a[r * 3 + k] * a[r * 3 + k];
            lb += b[r * 3 + k] * b[r * 3 + k];
        }
        if (la > 0 && lb > 0 && d < 0.7 * sqrt(la * lb)) return 1;
    }
    return 0;
}

static void apply(CPU *c)
{
    if (applied) return;
    uint32_t step = R32(STEP_MS);
    double alpha = step ? (double)R32(ACCUMULATOR) / step : 1.0;
    if (alpha < 0) alpha = 0;
    if (alpha > 1) alpha = 1;
    int stepped = steps != steps_seen;
    steps_seen = steps;
    applied = 1;
    for (int i = 0; i < nnodes; i++) {
        Node *n = &nodes[i];
        double v[12];
        n->written = 0;
        read_tf(n->node, v);
        if (!n->has) {
            memcpy(n->prev, v, sizeof v);
            memcpy(n->cur, v, sizeof v);
            n->has = 1;
            continue;
        }
        if (stepped) {
            memcpy(n->prev, n->cur, sizeof v);
            memcpy(n->cur, v, sizeof v);
        } else if (memcmp(v, n->cur, sizeof v)) {
            n->frame_anim = 1;             /* moves between steps: the frame animates it */
            memcpy(n->prev, v, sizeof v);
            memcpy(n->cur, v, sizeof v);
        }
        if (n->frame_anim || alpha >= 1.0 || !memcmp(n->prev, n->cur, sizeof v) || jump(n->prev, n->cur))
            continue;
        double m[12];
        for (int k = 0; k < 12; k++)
            m[k] = n->prev[k] + (n->cur[k] - n->prev[k]) * alpha;
        write_tf(c, n->node, m);
        n->written = 1;
    }
    static int dbg = -1;
    if (dbg < 0) dbg = getenv("TDOR3_SMOOTH_LOG") != NULL;
    if (dbg) {                           /* the race camera's place as drawn */
        uint32_t cam = R32(0x4c4a64);
        int nw = 0, na = 0;
        for (int i = 0; i < nnodes; i++) { nw += nodes[i].written; na += nodes[i].frame_anim; }
        LOG(LOG_ALWAYS, "smooth: frame %lu, %d nodes (%d blended, %d animated by frame), alpha %.2f%s, camera %.3f %.3f %.3f",
            frame_count, nnodes, nw, na, alpha, stepped ? " (stepped)" : "", RF64(cam + 0x60), RF64(cam + 0x68),
            RF64(cam + 0x70));
    }
}

static void restore(CPU *c)
{
    if (!applied) return;
    applied = 0;
    for (int i = 0; i < nnodes; i++)
        if (nodes[i].written) {
            write_tf(c, nodes[i].node, nodes[i].cur);
            nodes[i].written = 0;
        }
}

/* the option changed in the game's menu (menus.c) */
void smooth_options(void) { on = -1; }

static int enabled(void)
{
    if (on < 0) on = option_bool("smooth", 1);
    return on;
}

/* the race loop: what moves in it is tracked */
static int race_loop_hook(CPU *c)
{
    if (!enabled() || !R8(RACE_ON)) {
        if (nnodes) { nnodes = 0; rebuild_index(); }
        return 0;
    }
    tracking = 1;
    F_0044fcd5_orig(c);
    restore(c);
    tracking = 0;
    return 1;
}

static int node_dirty_hook(CPU *c)
{
    if (tracking && !applied)
        track(c->ecx);
    return 0;
}

static int node_dtor_hook(CPU *c)
{
    if (nnodes) untrack(c->ecx);
    return 0;
}

static int race_step_hook(CPU *c)
{
    extern void game_race_step(void);
    (void)c;
    steps++;
    game_race_step();               /* rumble (game.c) */
    return 0;
}

static int race_scenes_hook(CPU *c)
{
    if (tracking) apply(c);
    return 0;
}

static int race_draw_hook(CPU *c)
{
    F_004221e0_orig(c);
    restore(c);
    return 1;
}

void smooth_init(void)
{
    hook_race_loop = race_loop_hook;
    hook_node_dirty = node_dirty_hook;
    hook_node_dtor = node_dtor_hook;
    hook_race_step = race_step_hook;
    hook_race_scenes = race_scenes_hook;
    hook_race_draw = race_draw_hook;
}
