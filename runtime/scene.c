/* Widescreen (DESIGN.md goals): Hor+. SurRender draws every 3D view through
   srScene::render(srGERD&, srCamera*) (0x1005ea20), the camera's view plane the rectangle
   x0, y0, x1, y1 (doubles at +0x140, +0x148, +0x150, +0x158) that srCamera::processPush turns
   into the frustum. The game sets its cameras 4:3 (setFOV / setFocalLength with aspect 4/3).
   For the time of the call the view plane is made wider by the target's widening (vertical
   view kept), so SurRender culls and projects the wider view, and the renderer spreads that
   scene's viewport across the whole width (vk_wide). The camera is put back after, so
   everything else the game asks of it (srCamera::project for markers) stays 4:3, which matches:
   Hor+ keeps the middle 4:3 of the view as it was. SurRender batches geometry, so its
   renderers are flushed on both sides of the call: 2D queued before (the car select's labels
   and pointer, under its turning truck) stays 2D. */
#include "runtime.h"
#include "vk.h"
#include "funcs.h"

#define GERD_FLUSH_RENDERERS 0x10023bd0u     /* srGERD::flushRenderers (thiscall) */

/* [esp] return, [esp+4] srGERD&, [esp+8] srCamera*; ECX the scene */
static int scene_render_hook(CPU *c)
{
    uint32_t cam = R32(c->esp + 8);
    double k = cam ? vk_widening() : 1.0;
    uint32_t gerd = R32(c->esp + 4), keep = c->ecx;
    /* SurRender batches geometry: what was queued before (menu text, the pointer) is drawn
       now, as 2D, and the scene's own before the widening ends */
    c->ecx = gerd;
    guest_call(c, GERD_FLUSH_RENDERERS, 0);
    c->ecx = keep;
    double x0 = cam ? RF64(cam + 0x140) : 0, x1 = cam ? RF64(cam + 0x150) : 0;
    if (k > 1.001) {
        double mid = (x0 + x1) * 0.5, half = (x1 - x0) * 0.5;
        WF64(cam + 0x140, mid - half * k);
        WF64(cam + 0x150, mid + half * k);
    }
    vk_wide(1);
    F_1005ea20_orig(c);
    keep = c->ecx;
    c->ecx = gerd;
    guest_call(c, GERD_FLUSH_RENDERERS, 0);
    c->ecx = keep;
    vk_wide(0);
    if (k > 1.001) {
        WF64(cam + 0x140, x0);
        WF64(cam + 0x150, x1);
    }
    return 1;
}

void scene_init(void)
{
    hook_scene_render = scene_render_hook;
}
