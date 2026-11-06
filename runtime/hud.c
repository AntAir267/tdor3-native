/* The race HUD in widescreen (option hud = edges | middle; edges by default): each overlay
   (timer, lap or checkpoint count, place, lap times, gauges, map...: Overlay.cpp objects, drawn
   in order by 0x414cb6 from the list at [0x4c4950], [0x4c4954] of them, sorted by 0x414d51 when
   [0x4c4958] says so; +8 visible; vtable slot 0 draws it) keeps its size and goes to the edge of
   the screen its place is nearest, as far from it as it was from the 4:3 screen's edge
   (srdd.c keeps an overlay's draws and places them when it is done; SurRender batches
   geometry, so its renderers are flushed before the overlays and after each). In menus, and with the
   option off, the game draws them itself in the middle 4:3. */
#include <strings.h>
#include "runtime.h"
#include "options.h"
#include "vk.h"
#include "funcs.h"

#define OVERLAYS 0x4c4950u
#define N_OVERLAYS 0x4c4954u
#define OVERLAYS_UNSORTED 0x4c4958u
#define OVERLAY_ORDER 0x414d51u
#define RACE_ON 0x4e67fdu
#define MSVCRT_QSORT 0x7801f230u
#define GERD_FLUSH_RENDERERS 0x10023bd0u     /* srGERD::flushRenderers: batched geometry out now */

void srdd_overlay_begin(void);
void srdd_overlay_end(void);

/* 0x414cb6(srGERD *, char hide_after) */
static int overlays_draw_hook(CPU *c)
{
    static int told;
    if (!told++)
        LOG(LOG_ALWAYS, "hud: overlays drawn (race %d, widening %.2f, %u overlays)", R8(RACE_ON), vk_widening(),
            R32(N_OVERLAYS));
    if (!R8(RACE_ON) || vk_widening() <= 1.001f || !strcasecmp(option_str("hud", "edges"), "middle"))
        return 0;
    uint32_t gerd = R32(c->esp + 4), hide = R8(c->esp + 8);
    if (R32(OVERLAYS_UNSORTED)) {
        guest_call(c, MSVCRT_QSORT, 4, R32(OVERLAYS), R32(N_OVERLAYS), 4u, OVERLAY_ORDER);
        W32(OVERLAYS_UNSORTED, 0);
    }
    uint32_t keep_ecx = c->ecx;
    c->ecx = gerd;
    guest_call(c, GERD_FLUSH_RENDERERS, 0);
    for (uint32_t k = 0; k < R32(N_OVERLAYS); k++) {
        uint32_t o = R32(R32(OVERLAYS) + 4 * k);
        if (!o || !R32(o + 8))
            continue;
        uint32_t keep = c->ecx;
        c->ecx = o;
        srdd_overlay_begin();
        guest_call(c, R32(R32(o)), 1, gerd);
        c->ecx = gerd;
        guest_call(c, GERD_FLUSH_RENDERERS, 0);
        srdd_overlay_end();
        c->ecx = keep;
        if (hide)
            W32(o + 8, 0);
    }
    c->ecx = keep_ecx;
    c->ret = R32(c->esp);                /* its ret (cdecl: the caller drops the arguments) */
    c->esp += 4;
    return 1;
}

void hud_init(void)
{
    hook_overlays_draw = overlays_draw_hook;
}
