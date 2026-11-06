/* The Vulkan renderer under the native SurRender driver (srdd.c; DESIGN.md decision 5;
   work/analysis/SRDD_SPEC.md).

   The game draws into a back buffer of the size it opened (640x480), in its own pixels. The
   renderer's target is the window's size in pixels (options "resolution", "widescreen"): the
   game's screen is its middle 4:3 area, scaled; a widened scene (vk_wide: srScene::render with
   its camera widened, scene.c) spreads across the whole width. Vertices are homogeneous
   positions multiplied by the one projection matrix the engine sends (the mesh path sends clip
   space and an identity matrix), clipped by the GPU as OpenGL clips (NDC y up, z -1..1 mapped
   to the viewport's depth range). The state of a draw is SurRender's own srShader word, read by
   the shaders (draw.frag lists its bits). Everything is called from the game thread. */
#pragma once
#include <stdint.h>

typedef struct {
    float pos[4];                        /* x, y, z, w (before the matrix) */
    float col[4];                        /* diffuse RGBA 0..1 */
    float spec[4];                       /* specular RGB, w: fog (0 none .. 1 all fog colour) */
    float uv0[2], uv1[2];
} VkVert;

/* topologies (SurRender's e_primitive) */
enum { VP_POINTS, VP_LINESTRIP, VP_LINES, VP_TRISTRIP, VP_TRIFAN, VP_TRIANGLES };

typedef struct {
    uint32_t shader;                     /* srShader */
    int cull;                            /* 0 none, 1 cull counter-clockwise on screen, 2 clockwise */
    int polymode;                        /* 0 points, 1 lines, 2 fill */
    float depth_bias;
    int tex[2];                          /* texture ids, 0 none */
    int smp[2];                          /* sampler variants (VS_*) */
    float matrix[16];                    /* m[r*4+c], clip = M * pos */
    uint32_t fog_color;                  /* 0x00RRGGBB */
} VkDrawState;

/* sampler variants: filter (0 nearest, 1 linear) | mip (0 none, 2 nearest, 4 linear) | wrap u (0 wrap, 8 clamp) | wrap v (16) */
enum { VS_LINEAR = 1, VS_MIP_NEAREST = 2, VS_MIP_LINEAR = 4, VS_CLAMP_U = 8, VS_CLAMP_V = 16 };

int vk_init(void *sdl_window);           /* 0: Vulkan cannot run here */
int vk_ok(void);
void vk_target_size(int w, int h);       /* the back buffer's size (the game's pixels) */
void vk_get_target_size(int *w, int *h);
void vk_window_to_game(double px, double py, int *gx, int *gy);   /* window pixels -> the game's */
float vk_widening(void);                 /* how much wider than 4:3 a widened scene is (1: none) */
void vk_wide(int on);                    /* draws from here belong to a 3D scene (widened if wider than 4:3), or not */
void vk_hud(int anchor);                 /* HUD across the width: -1 left, 0 middle, 1 right edge; -2 off */
void vk_viewport(int x, int y, int w, int h, float zmin, float zmax);   /* the game's pixels */
void vk_scissor(int x0, int y0, int x1, int y1);   /* exclusive ends, the game's pixels */
void vk_clear(int color, int depth, const float rgba[4], float z);   /* inside viewport and scissor */
void vk_draw(const VkDrawState *st, int prim, const VkVert *v, int nv, const uint32_t *idx, int ni);
void vk_present(void);                   /* the frame to the window (or kept); starts the next */
void vk_stop(void);

/* textures: 32-bit pixels as bytes B, G, R, A (SurRender's ARGB8888) */
int vk_tex_new(int w, int h, int levels);
void vk_tex_upload(int tex, int level, int x, int y, int w, int h, const void *bgra, int pitch);
void vk_tex_free(int tex);

/* the back buffer on the CPU (bytes B, G, R, A), finishing the GPU work so far: the game's
   screen at its size (the engine's buffer spans), or the whole target (screenshots) */
void vk_read_game(uint8_t *bgra);        /* game w*h*4 */
void vk_write_game(const uint8_t *bgra, int x, int y, int w, int h, int pitch);
void vk_read_target(uint8_t *bgra);      /* target w*h*4 */
int vk_write_png(const char *path);      /* the last kept frame */
