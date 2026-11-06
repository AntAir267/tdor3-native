/* The native SurRender display driver (DESIGN.md decisions 2 and 5): the runtime's stand-in for
   srDD_DirectX6.dll. sr.dll loads it by name through LoadLibrary/GetProcAddress (loader.c's host
   modules), checks its API version and asks srDDInitDevice(n) for an srDD object: a C++ object
   whose 43 virtual methods (thiscall: `this` in ECX, the callee pops its arguments) are the whole
   interface. work/analysis/SRDD_SPEC.md describes every method and structure, from the DirectX6
   and Software drivers and sr.dll; the slot comments below cite its sections. Drawing goes to
   the Vulkan renderer (vk.c). */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "options.h"
#include "vk.h"
#include "win.h"

#define LOG_DD LOG_D3D
#define SRDD_API_VERSION 0x127u

static uint32_t dd_obj;                 /* the one device object (program memory) */

/* ------------------------------------------------------------------ state */
static uint32_t dd_hwnd;
static int win_w = 640, win_h = 480;    /* the open back buffer */
static uint32_t shader;                 /* srShader */
static int cull, polymode = 2;
static float depth_bias;
static float matrix[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static uint32_t fog_color;
static float clear_rgba[4];
static float clear_depth = 1.0f;
static uint8_t vai[0x6c];               /* VertexArrayInfo, copied */
static int vai_set;
static int stage_tex[2];                /* bound texture ids */
static int stage_smp[2] = {VS_LINEAR, VS_LINEAR};
static uint32_t modes, nmodes;          /* WindowInfo array (program memory) */
static uint32_t formats;                /* PixelFormat array (program memory) */
static unsigned long draws, frames_flipped;

/* ------------------------------------------------------------------ device and driver info */
/* getInfo(Info&) (spec 1): the 0x27c-byte srDD::Info; sr.dll fills defaults first and the
   driver writes the fields DX6's getInfo writes. */
METHOD(dd_getInfo, 1)
{
    uint32_t i = ARG(0);
    uint32_t mw = 0, mh = 0;
    for (uint32_t k = 0; k < nmodes; k++) {
        if (R32(modes + 12 * k) > mw) mw = R32(modes + 12 * k);
        if (R32(modes + 12 * k + 4) > mh) mh = R32(modes + 12 * k + 4);
    }
    W32(i + 0x00, mw);                   /* largest render size */
    W32(i + 0x04, mh);
    W32(i + 0x0c, 24);                   /* depth bits */
    W32(i + 0x18, 0x28);                 /* flags: depth buffer, the driver keeps its own texture copy */
    W32(i + 0x1c, 200);
    W32(i + 0x24, 0x10000000);           /* texture memory (shown only) */
    W32(i + 0x28, 2);                    /* texture stages */
    W32(i + 0x2c, 1);                    /* smallest mip */
    W32(i + 0x30, 2048);                 /* largest texture */
    W32(i + 0x34, 8);                    /* aspect ratio */
    W32(i + 0x38, 0);
    gstrncpy(i + 0x3c, "DirectX6", 64);
    gstrncpy(i + 0x7c, "Hybrid Holding, Ltd.", 64);
    gstrncpy(i + 0xbc, "Win32", 64);
    gstrncpy(i + 0x13c, "TDOR3 native", 64);
    gstrncpy(i + 0x17c, "1.0", 64);
    gstrncpy(i + 0x1bc, "Vulkan", 64);
    gstrncpy(i + 0x1fc, "Vulkan", 64);
    gstrncpy(i + 0x23c, "TDOR3 native", 64);
    LOG(LOG_DD, "srDD::getInfo: largest mode %ux%u", mw, mh);
    return 0;
}

/* getDriverInfo(DriverInfo&): id 2, "Direct3D" (TDOR3 takes the hardware path by the name's 'D') */
METHOD(dd_getDriverInfo, 1)
{
    uint32_t d = ARG(0);
    W32(d + 0x0c, SRDD_API_VERSION);
    W32(d + 0x10, 2);
    gstrncpy(d + 0x14, "Direct3D", 64);
    gstrncpy(d + 0x54, "1.39", 64);
    return 0;
}

/* createContext(HWND): 0 = done. The renderer starts here (on the game's window, or off-screen
   when headless). */
METHOD(dd_createContext, 1)
{
    dd_hwnd = ARG(0);
    int ok = vk_init(host_sdl_window());
    LOG(LOG_ALWAYS, "srDD::createContext(window %x): Vulkan %s", dd_hwnd, ok ? "running" : "not available");
    return ok ? 0 : 7;
}

METHOD(dd_deleteContext, 0) { return 0; }

/* getWindowList(WindowInfoList&) = {items, count}; WindowInfo = {w, h, bpp} */
METHOD(dd_getWindowList, 1)
{
    W32(ARG(0), modes);
    W32(ARG(0) + 4, nmodes);
    return 0;
}

static void put_argb8888(uint32_t pf)
{
    static const uint8_t bits[8] = {8, 16, 8, 8, 8, 0, 8, 24};
    for (int k = 0; k < 8; k++)
        W8(pf + (uint32_t)k, bits[k]);
    W32(pf + 8, 0);                      /* RGB(A) */
    W32(pf + 12, 3);                     /* 4 bytes a pixel */
}

/* getTextureFormats(PixelFormatList&) = {items, count}: ARGB8888 only (the engine converts every
   texture to it; no palette format, so no palettes) */
METHOD(dd_getTextureFormats, 1)
{
    W32(ARG(0), formats);
    W32(ARG(0) + 4, 1);
    return 0;
}

METHOD(dd_getBufferPixelFormat, 1)
{
    put_argb8888(ARG(0));
    return 0;
}

METHOD(dd_getStatistics, 1) { return 0; }    /* the engine zeroed it: nothing counted */
METHOD(dd_resetStatistics, 0) { return 0; }
METHOD(dd_extCommand, 3) { return 0; }
METHOD(dd_isBusy, 0) { return 0; }

/* ------------------------------------------------------------------ the window */
/* openWindow(const OpenInfo&, OpenResult&) (spec 1): {w, h, mode index or -1 windowed} */
METHOD(dd_openWindow, 2)
{
    uint32_t oi = ARG(0), orr = ARG(1);
    int w = (int)R32(oi), h = (int)R32(oi + 4), mode = (int)R32(oi + 8);
    if (mode >= 0) {
        if ((uint32_t)mode >= nmodes)
            return 6;
        w = (int)R32(modes + 12 * (uint32_t)mode);
        h = (int)R32(modes + 12 * (uint32_t)mode + 4);
    }
    if (w <= 0 || h <= 0)
        return 4;
    win_w = w;
    win_h = h;
    host_display_mode(w, h);
    vk_target_size(w, h);
    vk_viewport(0, 0, w, h, 0, 1);
    vk_scissor(0, 0, w, h);
    if (orr)
        W32(orr, 2);                     /* double buffered */
    LOG(LOG_ALWAYS, "srDD::openWindow %dx%d (%s)", w, h, mode >= 0 ? "full screen mode" : "windowed");
    return 0;
}

METHOD(dd_closeWindow, 0) { return 0; }
METHOD(dd_beginFrame, 0) { return 0; }
METHOD(dd_endFrame, 0) { return 0; }
METHOD(dd_flushFrame, 0) { return 0; }
METHOD(dd_fence, 0) { return 0; }

/* flipFrame(dst rects, src rects, n): the frame shown (the rects are always the whole frame) */
METHOD(dd_flipFrame, 3)
{
    extern void frame_presented(void);
    frames_flipped++;
    LOG(LOG_DRAW, "srDD::flipFrame #%lu (%lu draws)", frames_flipped, draws);
    draws = 0;
    vk_present();
    frame_presented();
    return 0;
}

/* update(const Update&): gamma and the swap interval (spec 2); gamma not yet applied */
METHOD(dd_update, 1)
{
    uint32_t u = ARG(0), fl = R32(u);
    if (fl & 4) {
        float g[3];
        for (int k = 0; k < 3; k++) { uint32_t v = R32(u + 4 + 4 * (uint32_t)k); memcpy(&g[k], &v, 4); }
        LOG(LOG_DD, "srDD::update: gamma %.2f %.2f %.2f", g[0], g[1], g[2]);
    }
    if (fl & 2)
        LOG(LOG_DD, "srDD::update: swap interval %u", R32(u + 0x14));
    return 0;
}

/* ------------------------------------------------------------------ viewport, scissor, clears */
static double rd_double(uint32_t a) { uint64_t v = R64(a); double d; memcpy(&d, &v, 8); return d; }
static float rd_float(uint32_t a) { uint32_t v = R32(a); float f; memcpy(&f, &v, 4); return f; }

METHOD(dd_setViewPort, 1)
{
    uint32_t v = ARG(0);
    LOG(LOG_DD, "srDD::setViewPort %d %d %d %d", (int)R32(v), (int)R32(v + 4), (int)R32(v + 8), (int)R32(v + 12));
    vk_viewport((int)R32(v), (int)R32(v + 4), (int)R32(v + 8), (int)R32(v + 12),
                (float)rd_double(v + 0x10), (float)rd_double(v + 0x18));
    return 0;
}

METHOD(dd_setScissor, 1)
{
    uint32_t s = ARG(0);
    LOG(LOG_DD, "srDD::setScissor %d %d %d %d", (int)R32(s), (int)R32(s + 4), (int)R32(s + 8), (int)R32(s + 12));
    vk_scissor((int)R32(s), (int)R32(s + 4), (int)R32(s + 8), (int)R32(s + 12));
    return 0;
}

METHOD(dd_setClearValues, 1)
{
    uint32_t cv = ARG(0);
    for (int k = 0; k < 3; k++)
        clear_rgba[k] = rd_float(cv + 4 * (uint32_t)k);
    clear_rgba[3] = 0;                   /* DX6 packs 0x00RRGGBB */
    clear_depth = (float)rd_double(cv + 0x20);
    return 0;
}

/* clearBuffers(flags*): 1 colour, 2 depth (4 stencil: none), over scissor and viewport */
METHOD(dd_clearBuffers, 1)
{
    uint32_t b = R32(ARG(0));
    vk_clear(b & 1, (b & 2) != 0, clear_rgba, clear_depth);
    return 0;
}

/* ------------------------------------------------------------------ the back buffer on the CPU
   bufferOp(const BufferCommand&) (spec 3): {buffer, cmd, data, x, y, count}; lock (0) and unlock
   (1) bracket spans read (2, 6), written (3, 5, 7, 9) or filled (4, 8), horizontal or vertical,
   in ARGB8888 (getBufferPixelFormat). */
static uint8_t *cpu_img;
static int cpu_w, cpu_h, lock_depth, cpu_dirty, cpu_valid;
static int dirty[4];                     /* the pixels written: x0, y0, x1, y1 */
static unsigned long op_count[10];

static void cpu_lock(void)
{
    if (lock_depth++ > 0)
        return;
    if (cpu_w != win_w || cpu_h != win_h) {
        free(cpu_img);
        cpu_img = malloc((size_t)win_w * win_h * 4);
        cpu_w = win_w;
        cpu_h = win_h;
    }
    vk_read_game(cpu_img);
    cpu_valid = 1;
    cpu_dirty = 0;
    dirty[0] = dirty[1] = 1 << 30;
    dirty[2] = dirty[3] = -1;
}

static void cpu_unlock(void)
{
    if (lock_depth <= 0 || --lock_depth > 0)
        return;
    if (cpu_dirty && dirty[2] >= dirty[0])     /* only what was written: the rest stays sharp */
        vk_write_game(cpu_img + ((size_t)dirty[1] * cpu_w + dirty[0]) * 4, dirty[0], dirty[1],
                      dirty[2] - dirty[0] + 1, dirty[3] - dirty[1] + 1, cpu_w * 4);
    cpu_dirty = 0;
    cpu_valid = 0;
}

METHOD(dd_bufferOp, 1)
{
    uint32_t bc = ARG(0);
    uint32_t buf = R32(bc), cmd = R32(bc + 4), data = R32(bc + 8);
    int x = (int)R32(bc + 12), y = (int)R32(bc + 16), n = (int)R32(bc + 20);
    if (buf > 1)
        return 2;
    if (cmd == 0) { cpu_lock(); return 0; }
    if (cmd == 1) { cpu_unlock(); return 0; }
    if (cmd > 9)
        return 2;
    if (!op_count[cmd]++)
        LOG(LOG_ALWAYS, "srDD::bufferOp: first command %u (%d pixels at %d,%d)", cmd, n, x, y);
    int implicit = lock_depth == 0;
    if (implicit)
        cpu_lock();
    int vertical = cmd >= 6;
    uint32_t *img = (uint32_t *)cpu_img;
    for (int k = 0; k < n; k++) {
        int px = vertical ? x : x + k, py = vertical ? y + k : y;
        if (px < 0 || py < 0 || px >= cpu_w || py >= cpu_h)
            continue;
        uint32_t *p = &img[(size_t)py * cpu_w + px];
        switch (cmd) {
        case 2: case 6: W32(data + 4 * (uint32_t)k, *p); continue;
        case 3: case 5: case 7: case 9: *p = R32(data + 4 * (uint32_t)k); break;
        case 4: case 8: *p = R32(data); break;
        default: continue;
        }
        cpu_dirty = 1;
        if (px < dirty[0]) dirty[0] = px;
        if (py < dirty[1]) dirty[1] = py;
        if (px > dirty[2]) dirty[2] = px;
        if (py > dirty[3]) dirty[3] = py;
    }
    if (implicit)
        cpu_unlock();
    return 0;
}

/* ------------------------------------------------------------------ state */
METHOD(dd_setShader, 1) { shader = R32(ARG(0)); return 0; }

METHOD(dd_setFogColor, 1)
{
    uint32_t v = ARG(0);
    uint32_t col = 0;
    for (int k = 0; k < 3; k++) {
        float f = rd_float(v + 4 * (uint32_t)k);
        int b = (int)(f * 255.0f + 0.5f);
        if (b < 0) b = 0;
        if (b > 255) b = 255;
        col |= (uint32_t)b << (16 - 8 * k);
    }
    fog_color = col;
    return 0;
}

METHOD(dd_setCullMode, 1) { cull = (int)ARG(0); return 0; }
METHOD(dd_setPolygonMode, 1) { polymode = (int)ARG(0); return 0; }
METHOD(dd_setPolygonOffset, 1) { depth_bias = -(float)(int32_t)ARG(0); return 0; }

METHOD(dd_setProjectionMatrix, 2)
{
    for (int k = 0; k < 16; k++)
        matrix[k] = rd_float(ARG(0) + 4 * (uint32_t)k);
    return 0;
}

METHOD(dd_setVertexArrayInfo, 1)
{
    if (ARG(0)) {
        memcpy(vai, gptr(ARG(0)), sizeof vai);
        vai_set = 1;
    }
    return 0;
}

/* setTextureParameters(stage, const TexParms&) = {bits, lodBias}: bits 4-5 mag, 6-7 min filter
   (0 point, else linear), 8-9 mips (0 none, 1 nearest, 2 linear), 10-11 / 12-13 wrap u / v
   (1 clamp) */
METHOD(dd_setTextureParameters, 2)
{
    uint32_t st = ARG(0), b = R32(ARG(1));
    if (st > 1)
        return 0;
    int s = 0;
    if (((b >> 4) & 3) || ((b >> 6) & 3)) s |= VS_LINEAR;
    if (((b >> 8) & 3) == 1) s |= VS_MIP_NEAREST;
    if (((b >> 8) & 3) == 2) s |= VS_MIP_LINEAR;
    if (((b >> 10) & 3) == 1) s |= VS_CLAMP_U;
    if (((b >> 12) & 3) == 1) s |= VS_CLAMP_V;
    stage_smp[st] = s;
    return 0;
}

/* ------------------------------------------------------------------ textures (spec 6)
   srDD::Texture: +0x20/+0x24 level-0 size, +0x28 base level, +0x2c max level, +0x30 format
   index (always our ARGB8888), +0x38 level pointers (tightly packed, may be NULL once the driver
   holds its copy), +0x68 the driver's handle (our texture id), +0x6c resident bytes. */
static void tex_upload_level(uint32_t t, int id, int lvl, int x0, int y0, int x1, int y1)
{
    uint32_t src = R32(t + 0x38 + 4 * (uint32_t)lvl);
    if (!src)
        return;
    int lw = (int)(R32(t + 0x20) >> lvl), lh = (int)(R32(t + 0x24) >> lvl);
    if (lw < 1) lw = 1;
    if (lh < 1) lh = 1;
    if (x1 > lw) x1 = lw;
    if (y1 > lh) y1 = lh;
    if (x1 <= x0 || y1 <= y0)
        return;
    int base = (int)R32(t + 0x28);
    vk_tex_upload(id, lvl - base, x0, y0, x1 - x0, y1 - y0, gptr(src + (uint32_t)((y0 * lw + x0) * 4)), lw * 4);
}

static int tex_resident(uint32_t t)
{
    int id = (int)R32(t + 0x68);
    if (id)
        return id;
    int base = (int)R32(t + 0x28), maxl = (int)R32(t + 0x2c);
    int w = (int)(R32(t + 0x20) >> base), h = (int)(R32(t + 0x24) >> base);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    int levels = maxl - base + 1;
    if (levels < 1) levels = 1;
    if (levels > 12) levels = 12;
    id = vk_tex_new(w, h, levels);
    if (!id)
        return 0;
    uint32_t bytes = 0;
    for (int l = base; l < base + levels; l++) {
        tex_upload_level(t, id, l, 0, 0, 1 << 20, 1 << 20);
        int lw = w >> (l - base), lh = h >> (l - base);
        bytes += (uint32_t)((lw ? lw : 1) * (lh ? lh : 1) * 4);
    }
    W32(t + 0x68, (uint32_t)id);
    W32(t + 0x6c, bytes);
    {   /* TDOR3_DUMP_TEX=dir: every texture's first level as dir/tex<id>.pam (debugging) */
        const char *dir = getenv("TDOR3_DUMP_TEX");
        uint32_t src = R32(t + 0x38 + 4 * (uint32_t)base);
        if (dir && src) {
            char path[1024];
            snprintf(path, sizeof path, "%s/tex%d.pam", dir, id);
            FILE *f = fopen(path, "wb");
            if (f) {
                fprintf(f, "P7\nWIDTH %d\nHEIGHT %d\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", w, h);
                for (int i = 0; i < w * h; i++) {
                    const uint8_t *q = (const uint8_t *)gptr(src + 4 * (uint32_t)i);
                    uint8_t px[4] = {q[2], q[1], q[0], q[3]};
                    fwrite(px, 1, 4, f);
                }
                fclose(f);
            }
        }
    }
    LOG(LOG_DD, "srDD: texture %d %dx%d, %d levels (texture record %08x)", id, w, h, levels, t);
    return id;
}

METHOD(dd_bindTexture, 2)
{
    uint32_t st = ARG(0), t = ARG(1);
    if (st > 1)
        return 0;
    stage_tex[st] = t ? tex_resident(t) : 0;
    return 0;
}

METHOD(dd_deleteTexture, 1)
{
    uint32_t t = ARG(0);
    int id = (int)R32(t + 0x68);
    if (id) {
        for (int k = 0; k < 2; k++)
            if (stage_tex[k] == id)
                stage_tex[k] = 0;
        vk_tex_free(id);
    }
    W32(t + 0x68, 0);
    W32(t + 0x6c, 0);
    return 0;
}

METHOD(dd_texImage, 2)
{
    uint32_t t = ARG(0);
    int id = (int)R32(t + 0x68);
    if (id)
        tex_upload_level(t, id, (int)ARG(1), 0, 0, 1 << 20, 1 << 20);
    return 0;
}

METHOD(dd_texSubImage, 6)
{
    uint32_t t = ARG(0);
    int id = (int)R32(t + 0x68);
    if (id)
        tex_upload_level(t, id, (int)ARG(1), (int)ARG(2), (int)ARG(3), (int)ARG(4), (int)ARG(5));
    return 0;
}

METHOD(dd_preBindTexture, 2) { return 0; }
METHOD(dd_setGlobalPalette, 2) { return 0; }
METHOD(dd_bindPalette, 1) { return 0; }
METHOD(dd_deletePalette, 1) { W32(ARG(0) + 8, 0); return 0; }

/* ------------------------------------------------------------------ drawing (spec 5)
   VertexArrayInfo: +0 enable mask (2 diffuse, 4 specular, 8 fog, 0x10 tex0, 0x20 tex1; the
   position always), +4 vertex count, +8 clip state; per array i (0 position, 1 diffuse,
   2 specular, 3 fog, 4 tex0, 5 tex1): size +0xc, type +0x24 (0 u8 /255, 1 float), stride +0x3c
   (0: one value for every vertex), pointer +0x54. */
static uint32_t va32(int off) { uint32_t v; memcpy(&v, vai + off, 4); return v; }

static int fetch(int arr, uint32_t vtx, float *out)
{
    uint32_t size = va32(0x0c + 4 * arr), type = va32(0x24 + 4 * arr);
    uint32_t stride = va32(0x3c + 4 * arr), ptr = va32(0x54 + 4 * arr);
    if (!ptr || !size)
        return 0;
    if (size > 4) size = 4;
    uint32_t a = ptr + stride * vtx;
    for (uint32_t k = 0; k < size; k++)
        out[k] = type == 0 ? R8(a + k) / 255.0f : rd_float(a + 4 * k);
    return (int)size;
}

static VkVert *vbuf;
static int vcap;

static void gather(VkVert *o, uint32_t vtx)
{
    uint32_t en = va32(0);
    float t[4];
    int n;
    o->pos[0] = o->pos[1] = o->pos[2] = 0;
    o->pos[3] = 1;
    n = fetch(0, vtx, t);
    for (int k = 0; k < n; k++) o->pos[k] = t[k];
    o->col[0] = o->col[1] = o->col[2] = o->col[3] = 1;
    if ((en & 2) && (n = fetch(1, vtx, t)) > 0)
        for (int k = 0; k < n; k++) o->col[k] = t[k];
    o->spec[0] = o->spec[1] = o->spec[2] = o->spec[3] = 0;
    if ((en & 4) && (n = fetch(2, vtx, t)) > 0)
        for (int k = 0; k < n; k++) o->spec[k] = t[k];
    if ((en & 8) && fetch(3, vtx, t) > 0)
        o->spec[3] = t[0];
    o->uv0[0] = o->uv0[1] = o->uv1[0] = o->uv1[1] = 0;
    if ((en & 0x10) && (n = fetch(4, vtx, t)) > 0) {
        o->uv0[0] = t[0];
        o->uv0[1] = n > 1 ? t[1] : 0;
        if (n == 4 && t[3] != 0) { o->uv0[0] /= t[3]; o->uv0[1] /= t[3]; }
    }
    if ((en & 0x20) && (n = fetch(5, vtx, t)) > 0) {
        o->uv1[0] = t[0];
        o->uv1[1] = n > 1 ? t[1] : 0;
        if (n == 4 && t[3] != 0) { o->uv1[0] /= t[3]; o->uv1[1] /= t[3]; }
    }
}

/* ------------------------------------------------------------------ HUD overlays (hud.c)
   While one overlay draws, its draws are kept; at its end they are drawn anchored to the
   screen's edge its place is nearest (by the middle of everything it drew, in the game's
   screen: left or right third, else the middle). */
typedef struct { VkDrawState st; int prim, nv, ni; VkVert *v; uint32_t *idx; } Kept;
static Kept *kept;
static int nkept, capkept, capturing;

void srdd_overlay_begin(void)
{
    capturing = 1;
    nkept = 0;
}

void srdd_overlay_end(void)
{
    capturing = 0;
    float lo = 1e9f, hi = -1e9f;
    for (int k = 0; k < nkept; k++) {
        const Kept *d = &kept[k];
        for (int j = 0; j < d->ni; j++) {
            const VkVert *q = &d->v[d->idx[j]];
            float cx = 0, cw = 0;
            for (int m = 0; m < 4; m++) {
                cx += d->st.matrix[m] * q->pos[m];
                cw += d->st.matrix[12 + m] * q->pos[m];
            }
            if (cw <= 0) continue;
            float x = cx / cw;
            lo = x < lo ? x : lo;
            hi = x > hi ? x : hi;
        }
    }
    float mid = (lo + hi) * 0.5f;
    int anchor = lo > hi ? 0 : mid < -1.0f / 3 ? -1 : mid > 1.0f / 3 ? 1 : 0;
    if (getenv("TDOR3_HUD_LOG"))
        LOG(LOG_ALWAYS, "hud: overlay of %d draws, x %.2f..%.2f: anchor %d", nkept, lo, hi, anchor);
    vk_hud(anchor);
    for (int k = 0; k < nkept; k++) {
        Kept *d = &kept[k];
        vk_draw(&d->st, d->prim, d->v, d->nv, d->idx, d->ni);
        free(d->v);
        free(d->idx);
    }
    vk_hud(-2);
    nkept = 0;
}

static void draw(int prim, uint32_t first, uint32_t nverts, const uint32_t *idx, int ni)
{
    if (!vai_set || !nverts || ni <= 0)
        return;
    if ((int)nverts > vcap) {
        vcap = (int)nverts + 1024;
        vbuf = realloc(vbuf, sizeof *vbuf * (size_t)vcap);
    }
    for (uint32_t k = 0; k < nverts; k++)
        gather(&vbuf[k], first + k);
    VkDrawState st;
    st.shader = shader;
    st.cull = cull;
    st.polymode = polymode;
    st.depth_bias = depth_bias;
    for (int k = 0; k < 2; k++) {
        st.tex[k] = stage_tex[k];
        st.smp[k] = stage_smp[k];
    }
    if (!(va32(0) & 0x20))
        st.tex[1] = 0;
    memcpy(st.matrix, matrix, sizeof matrix);
    st.fog_color = fog_color;
    if (LOGON(LOG_DRAW)) {
        LOG(LOG_DRAW, "draw prim %d: %u verts, %d indices, shader %08x, tex %d/%d smp %d, en %x, cull %d",
            prim, nverts, ni, shader, st.tex[0], st.tex[1], st.smp[0], va32(0), cull);
        LOG(LOG_DRAW, "  matrix %g %g %g %g / %g %g %g %g / %g %g %g %g / %g %g %g %g", matrix[0], matrix[1], matrix[2],
            matrix[3], matrix[4], matrix[5], matrix[6], matrix[7], matrix[8], matrix[9], matrix[10], matrix[11],
            matrix[12], matrix[13], matrix[14], matrix[15]);
        static int log_verts = -1;
        if (log_verts < 0) log_verts = getenv("TDOR3_LOG_VERTS") ? atoi(getenv("TDOR3_LOG_VERTS")) : 4;
        if (getenv("TDOR3_LOG_QUADS"))       /* each indexed triangle's corners, in game pixels and texels */
            for (int k = 0; k + 2 < ni; k += 3) {
                char b[600];
                int o = 0;
                for (int j = 0; j < 3; j++) {
                    const VkVert *q = &vbuf[idx[k + j]];
                    o += snprintf(b + o, sizeof b - o, " (%.3f,%.3f uv %.4f,%.4f w %g z %g c %.3f/%.3f uv1 %g,%g)",
                                  (q->pos[0] / q->pos[3] + 1) * 320, (q->pos[1] / q->pos[3] + 1) * 240, q->uv0[0],
                                  q->uv0[1], q->pos[3], q->pos[2], q->col[0], q->col[3], q->uv1[0], q->uv1[1]);
                }
                LOG(LOG_DRAW, "  tri%s", b);
            }
        for (uint32_t k = 0; k < nverts && k < (uint32_t)log_verts; k++)
            LOG(LOG_DRAW, "  v%u pos %g %g %g %g col %g %g %g %g uv %g %g", k, vbuf[k].pos[0], vbuf[k].pos[1],
                vbuf[k].pos[2], vbuf[k].pos[3], vbuf[k].col[0], vbuf[k].col[1], vbuf[k].col[2], vbuf[k].col[3],
                vbuf[k].uv0[0], vbuf[k].uv0[1]);
    }
    {   /* TDOR3_SKIP_TEX=a,b,...: draws with those textures left out (debugging) */
        static int skip[16], nskip = -1;
        if (nskip < 0) {
            nskip = 0;
            const char *e = getenv("TDOR3_SKIP_TEX");
            for (char *p = (char *)e; p && *p && nskip < 16; ) {
                skip[nskip++] = (int)strtol(p, &p, 10);
                if (*p == ',') p++; else break;
            }
        }
        for (int k = 0; k < nskip; k++)
            if (st.tex[0] == skip[k]) return;
    }
    if (capturing) {
        if (nkept == capkept) {
            capkept = capkept ? capkept * 2 : 16;
            kept = realloc(kept, sizeof *kept * (size_t)capkept);
        }
        Kept *d = &kept[nkept++];
        d->st = st;
        d->prim = prim;
        d->nv = (int)nverts;
        d->ni = ni;
        d->v = malloc(sizeof *vbuf * nverts);
        memcpy(d->v, vbuf, sizeof *vbuf * nverts);
        d->idx = malloc(sizeof *idx * (size_t)ni);
        memcpy(d->idx, idx, sizeof *idx * (size_t)ni);
        draws++;
        return;
    }
    vk_draw(&st, prim, vbuf, (int)nverts, idx, ni);
    draws++;
    {   /* TDOR3_PROBE=x,y (target pixels): with the draw log on, which draws change that pixel */
        static int px = -2, py;
        static uint32_t last;
        if (px == -2) {
            px = -1;
            const char *e = getenv("TDOR3_PROBE");
            if (e) sscanf(e, "%d,%d", &px, &py);
        }
        if (px >= 0 && LOGON(LOG_DRAW)) {
            int tw, th;
            extern void vk_target_pixels(int *w, int *h);
            vk_target_pixels(&tw, &th);
            static uint8_t *img;
            static size_t cap;
            if ((size_t)tw * th * 4 > cap) { cap = (size_t)tw * th * 4; img = realloc(img, cap); }
            vk_read_target(img);
            uint32_t v;
            memcpy(&v, img + ((size_t)py * tw + px) * 4, 4);
            int hit = 0;                 /* a used triangle's box takes in the pixel (game pixels, 640x480 run) */
            for (int k = 0; k + 2 < ni && !hit; k += 3) {
                float x0 = 1e9f, x1 = -1e9f, y0 = 1e9f, y1 = -1e9f;
                for (int j = 0; j < 3; j++) {
                    const VkVert *q = &vbuf[idx[k + j]];
                    float cx = 0, cy = 0, cw = 0;
                    for (int m = 0; m < 4; m++) {
                        cx += matrix[0 * 4 + m] * q->pos[m];
                        cy += matrix[1 * 4 + m] * q->pos[m];
                        cw += matrix[3 * 4 + m] * q->pos[m];
                    }
                    if (cw <= 0) { x0 = -1e9f; break; }
                    float sx = (cx / cw + 1) * 0.5f * win_w, sy = (1 - cy / cw) * 0.5f * win_h;
                    x0 = sx < x0 ? sx : x0; x1 = sx > x1 ? sx : x1;
                    y0 = sy < y0 ? sy : y0; y1 = sy > y1 ? sy : y1;
                }
                hit = px >= x0 && px <= x1 && py >= y0 && py <= y1;
            }
            if (v != last || (hit && prim == 5))
                LOG(LOG_ALWAYS, "probe: draw %lu (tex %d, shader %08x)%s: pixel %08x", draws, st.tex[0], shader,
                    hit ? " covers it" : "", v);
            last = v;
        }
    }
}

/* drawElements(prim, count, index type (0 u8, 1 u16, 2 u32), indices) */
METHOD(dd_drawElements, 4)
{
    int prim = (int)ARG(0), n = (int)ARG(1), it = (int)ARG(2);
    uint32_t ip = ARG(3);
    if (n <= 0 || !ip || prim > 5)
        return 0;
    static uint32_t *ix;
    static int ixcap;
    if (n > ixcap) { ixcap = n + 1024; ix = realloc(ix, sizeof *ix * (size_t)ixcap); }
    uint32_t mx = 0;
    for (int k = 0; k < n; k++) {
        uint32_t v = it == 0 ? R8(ip + (uint32_t)k) : it == 1 ? R16(ip + 2 * (uint32_t)k) : R32(ip + 4 * (uint32_t)k);
        ix[k] = v;
        if (v > mx) mx = v;
    }
    uint32_t nv = va32(4);
    if (!nv || nv <= mx)
        nv = mx + 1;
    draw(prim, 0, nv, ix, n);
    return 0;
}

/* drawArrays(prim, first, count) */
METHOD(dd_drawArrays, 3)
{
    int prim = (int)ARG(0), first = (int)ARG(1), n = (int)ARG(2);
    if (n <= 0 || prim > 5)
        return 0;
    static uint32_t *ix;
    static int ixcap;
    if (n > ixcap) { ixcap = n + 1024; ix = realloc(ix, sizeof *ix * (size_t)ixcap); }
    for (int k = 0; k < n; k++)
        ix[k] = (uint32_t)k;
    draw(prim, (uint32_t)first, (uint32_t)n, ix, n);
    return 0;
}

/* the scalar deleting destructor: the one device object stays */
METHOD(dd_destroy, 1) { return c->ecx; }

/* ------------------------------------------------------------------ the vtable */
static const struct { const char *name; void (*fn)(CPU *c); } methods[] = {
    {"~srDD", dd_destroy}, {"getInfo", dd_getInfo}, {"getWindowList", dd_getWindowList},
    {"getTextureFormats", dd_getTextureFormats}, {"getStatistics", dd_getStatistics},
    {"resetStatistics", dd_resetStatistics}, {"extCommand", dd_extCommand}, {"isBusy", dd_isBusy},
    {"openWindow", dd_openWindow}, {"closeWindow", dd_closeWindow}, {"beginFrame", dd_beginFrame},
    {"endFrame", dd_endFrame}, {"flushFrame", dd_flushFrame}, {"flipFrame", dd_flipFrame},
    {"clearBuffers", dd_clearBuffers}, {"bufferOp", dd_bufferOp}, {"update", dd_update},
    {"setScissor", dd_setScissor}, {"setViewPort", dd_setViewPort}, {"setClearValues", dd_setClearValues},
    {"setFogColor", dd_setFogColor}, {"setShader", dd_setShader},
    {"setTextureParameters", dd_setTextureParameters}, {"bindTexture", dd_bindTexture},
    {"deleteTexture", dd_deleteTexture}, {"texImage", dd_texImage}, {"texSubImage", dd_texSubImage},
    {"setGlobalPalette", dd_setGlobalPalette}, {"bindPalette", dd_bindPalette},
    {"deletePalette", dd_deletePalette}, {"createContext", dd_createContext},
    {"deleteContext", dd_deleteContext}, {"getDriverInfo", dd_getDriverInfo}, {"fence", dd_fence},
    {"getBufferPixelFormat", dd_getBufferPixelFormat}, {"preBindTexture", dd_preBindTexture},
    {"setPolygonMode", dd_setPolygonMode}, {"setCullMode", dd_setCullMode},
    {"setProjectionMatrix", dd_setProjectionMatrix}, {"setVertexArrayInfo", dd_setVertexArrayInfo},
    {"drawElements", dd_drawElements}, {"drawArrays", dd_drawArrays}, {"setPolygonOffset", dd_setPolygonOffset},
};
#define NMETHODS ((int)(sizeof methods / sizeof methods[0]))
_Static_assert(sizeof methods / sizeof methods[0] == 43, "srDD has 43 methods");

/* ------------------------------------------------------------------ the DLL's exports (cdecl) */
static uint32_t driver_name;

API_CDECL(SRDD, srDDGetDriverApiVersion) { return SRDD_API_VERSION; }
API_CDECL(SRDD, srDDGetDriverName) { return driver_name; }
API_CDECL(SRDD, srDDConfigureDriver)
{
    LOG(LOG_DD, "srDDConfigureDriver(\"%s\")", gstr(ARG(0)) ? gstr(ARG(0)) : "");
    return 0;
}
API_CDECL(SRDD, srDDGetDeviceCount) { return 1; }
API_CDECL(SRDD, srDDGetDeviceName) { return driver_name; }
API_CDECL(SRDD, srDDInitDevice) { return ARG(0) == 0 ? dd_obj : 0; }

static const struct { const char *name; void (*fn)(CPU *c); } exports[] = {
    {"srDDGetDriverApiVersion", w_SRDD_srDDGetDriverApiVersion},
    {"srDDGetDriverName", w_SRDD_srDDGetDriverName},
    {"srDDConfigureDriver", w_SRDD_srDDConfigureDriver},
    {"srDDGetDeviceCount", w_SRDD_srDDGetDeviceCount},
    {"srDDGetDeviceName", w_SRDD_srDDGetDeviceName},
    {"srDDInitDevice", w_SRDD_srDDInitDevice},
};

/* The display modes offered: the game asks for 640x480 (16, 24 or 32 bits); larger ones too. */
static void make_modes(void)
{
    static const int sizes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 960}, {1600, 1200}};
    static const int depths[] = {16, 24, 32};
    int n = (int)(sizeof sizes / sizeof sizes[0]) * 3;
    modes = galloc(12 * (uint32_t)n);
    nmodes = 0;
    for (int s = 0; s < (int)(sizeof sizes / sizeof sizes[0]); s++)
        for (int d = 0; d < 3; d++) {
            W32(modes + 12 * nmodes, (uint32_t)sizes[s][0]);
            W32(modes + 12 * nmodes + 4, (uint32_t)sizes[s][1]);
            W32(modes + 12 * nmodes + 8, (uint32_t)depths[d]);
            nmodes++;
        }
}

void srdd_init(void)
{
    driver_name = gstrdup("DirectX6");      /* the name tdor3.cfg's driver list asks for */
    uint32_t vt = galloc(4 * NMETHODS);
    for (int i = 0; i < NMETHODS; i++) {
        char *nm = malloc(strlen(methods[i].name) + 8);
        sprintf(nm, "srDD::%s", methods[i].name);
        W32(vt + 4 * (uint32_t)i, thunk_addr(methods[i].fn, nm));
    }
    dd_obj = galloc(0x40);
    memset(gptr(dd_obj), 0, 0x40);
    W32(dd_obj, vt);
    formats = galloc(16);
    put_argb8888(formats);
    make_modes();
    loader_add_host_module("srDD_DirectX6.dll", exports, (int)(sizeof exports / sizeof exports[0]));
}

void srdd_stop(void)
{
    vk_stop();
}
