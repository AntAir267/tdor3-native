/* The Vulkan renderer (vk.h): Vulkan 1.3 with dynamic rendering. One target (colour + depth) at
   the window's resolution, drawn into during the frame and copied into the window at present;
   the game's own coordinates (its 640x480) are mapped onto it (vk.h: the game's area, widened
   scenes); textures in one bindless array; a ring of host memory per frame for vertices,
   indices and uploads; pipelines made per blend state, the rest dynamic. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <zlib.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include "runtime.h"
#include "vk.h"
#include "options.h"
#include "shaders/draw.vert.spv.h"
#include "shaders/draw.frag.spv.h"

#define FRAMES 2
#define RING_SIZE (64u << 20)
#define MAX_TEX 8192
#define NSAMPLERS 32
#define VK_CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) fatal("%s failed: %d", #x, (int)_r); } while (0)

static int ok, tried;
static SDL_Window *win;
static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkSurfaceKHR surface;
static VkPhysicalDeviceMemoryProperties memprops;

/* swapchain */
static VkSwapchainKHR swapchain;
static VkFormat sc_format;
static VkExtent2D sc_extent;
static uint32_t sc_count;
static VkImage sc_images[8];
static VkSemaphore sc_done[8];
static int sc_stale;

/* the game's target */
typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; VkImageLayout layout; } Img;
static Img color, depth;
static int tw = 640, th = 480;           /* its size in pixels */
static Img small;                        /* a game-sized image: the game's reads and writes of the target */

/* the game's coordinates on the target: its area (the middle, 4:3) at ax.. ax + aw, scale
   sx (x) and sy (y); a widened scene spreads by wk across the whole width */
static int lw = 640, lh = 480;           /* the game's size */
static int ax, aw = 640;
static float sx = 1, sy = 1, wk = 1;
static int wide;                         /* drawing a widened scene */
static int scene_on;                     /* drawing a 3D scene (srScene::render), widened or not */
static VkFormat depth_format;

/* frames */
typedef struct {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore acquired;
    VkBuffer ring;
    VkDeviceMemory ring_mem;
    uint8_t *ring_ptr;
    uint32_t ring_used;
    int trash[256], ntrash;              /* textures freed during the frame: destroyed when it is done */
} Frame;
static Frame frames[FRAMES];
static int fi;                           /* the frame being recorded */
static int recording, rendering;

/* textures */
typedef struct { Img im; int w, h, levels, used; } Tex;
static Tex *texs;
static int ntex;
static int *free_tex, nfree_tex;

/* descriptors and pipelines */
static VkDescriptorSetLayout dsl;
static VkDescriptorPool dpool;
static VkDescriptorSet dset;
static VkSampler samplers[NSAMPLERS];
static VkPipelineLayout playout;
static VkShaderModule vs, fs;
typedef struct { uint32_t key; VkPipeline p; } Pipe;
static Pipe pipes[512];
static int npipes;
static VkPipeline bound_pipe;

/* state */
static VkViewport cur_vp;
static VkRect2D cur_sc;                  /* the scissor in force: viewport and the engine's scissor */
static float lvp[6] = {0, 0, 640, 480, 0, 1};   /* the game's viewport: x, y, w, h, zmin, zmax */
static int lsc[4] = {0, 0, 640, 480};    /* the game's scissor: x0, y0, x1, y1 */
static int full_clip;                    /* viewport and scissor take in the game's whole screen */
static void apply_view(void);
static void layout(void);

/* push constants (shaders/push.glsl) */
typedef struct {
    float m[16];                         /* row-major: clip = M * pos */
    uint32_t shader;                     /* srShader */
    int32_t tex0, smp0, tex1, smp1;
    uint32_t fog_color;
    uint32_t flags;                      /* 1: stage 1 has a texture; 2: 2D */
    float d3d_shift[2];                  /* Direct3D's sample point, up-left, target pixels */
} Push;

/* ------------------------------------------------------------------ helpers */
static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    fatal("no Vulkan memory type %x", want);
}

static void img_make(Img *im, uint32_t w, uint32_t h, uint32_t levels, VkFormat fmt, VkImageUsageFlags usage,
                     VkImageAspectFlags aspect)
{
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = fmt;
    ci.extent = (VkExtent3D){w, h, 1};
    ci.mipLevels = levels;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(dev, &ci, NULL, &im->img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, im->img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = mr.size;
    ai.memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(dev, &ai, NULL, &im->mem));
    VK_CHECK(vkBindImageMemory(dev, im->img, im->mem, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im->img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = (VkImageSubresourceRange){aspect, 0, levels, 0, 1};
    VK_CHECK(vkCreateImageView(dev, &vi, NULL, &im->view));
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
}

static void img_free(Img *im)
{
    if (im->view) vkDestroyImageView(dev, im->view, NULL);
    if (im->img) vkDestroyImage(dev, im->img, NULL);
    if (im->mem) vkFreeMemory(dev, im->mem, NULL);
    memset(im, 0, sizeof *im);
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = (VkImageSubresourceRange){aspect, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &di);
}

static void img_layout(VkCommandBuffer cb, Img *im, VkImageAspectFlags aspect, VkImageLayout to)
{
    if (im->layout == to)
        return;
    barrier(cb, im->img, aspect, im->layout, to);
    im->layout = to;
}

/* ------------------------------------------------------------------ the ring */
static void frame_begin(void);
static void flush_wait(void);

static uint32_t ring_alloc(uint32_t size, uint32_t align, VkBuffer *buf, uint8_t **ptr)
{
    Frame *f = &frames[fi];
    uint32_t off = (f->ring_used + align - 1) & ~(align - 1);
    if (off + size > RING_SIZE) {
        if (size > RING_SIZE / 2)
            fatal("Vulkan: %u bytes at once is more than the ring holds", size);
        flush_wait();                    /* the frame so far run, the ring empty again */
        f = &frames[fi];
        off = 0;
    }
    f->ring_used = off + size;
    *buf = f->ring;
    *ptr = f->ring_ptr + off;
    return off;
}

/* ------------------------------------------------------------------ the target */
static void target_make(void)
{
    if (color.img) {
        vkDeviceWaitIdle(dev);
        img_free(&color);
        img_free(&depth);
    }
    img_make(&color, tw, th, 1, VK_FORMAT_B8G8R8A8_UNORM,
             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
             VK_IMAGE_ASPECT_COLOR_BIT);
    img_make(&depth, tw, th, 1, depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (small.img)
        img_free(&small);
    img_make(&small, lw, lh, 1, VK_FORMAT_B8G8R8A8_UNORM,
             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    LOG(LOG_ALWAYS, "Vulkan: drawing at %dx%d (the game's %dx%d at %d..%d%s)", tw, th, lw, lh, ax, ax + aw,
        wk > 1.001f ? ", scenes widened" : "");
}

static void end_rendering(void)
{
    if (rendering) {
        vkCmdEndRendering(frames[fi].cmd);
        rendering = 0;
        bound_pipe = VK_NULL_HANDLE;
    }
}

static void begin_rendering(void)
{
    if (rendering)
        return;
    VkCommandBuffer cb = frames[fi].cmd;
    int fresh_color = color.layout == VK_IMAGE_LAYOUT_UNDEFINED;
    img_layout(cb, &color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    int fresh_depth = depth.layout == VK_IMAGE_LAYOUT_UNDEFINED;
    img_layout(cb, &depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = color.view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = fresh_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.clearValue.color = (VkClearColorValue){{0, 0, 0, 1}};
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView = depth.view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = fresh_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 1.0f;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = (VkRect2D){{0, 0}, {(uint32_t)tw, (uint32_t)th}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = &da;
    vkCmdBeginRendering(cb, &ri);
    rendering = 1;
    vkCmdSetViewport(cb, 0, 1, &cur_vp);
    vkCmdSetScissor(cb, 0, 1, &cur_sc);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, playout, 0, 1, &dset, 0, NULL);
}

/* ------------------------------------------------------------------ pipelines */
/* srShader blend factors: src bits 13-14, dst bits 5-7 */
static const VkBlendFactor src_factors[4] = {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_ALPHA,
                                             VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA};
static const VkBlendFactor dst_factors[8] = {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_SRC_COLOR,
                                             VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA,
                                             VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, VK_BLEND_FACTOR_SRC_COLOR,
                                             VK_BLEND_FACTOR_ZERO};

/* The pipelines' cache, kept in <user>/cache/pipelines.bin from run to run (as td6-native): a
   pipeline is made the first time its draw state is needed, and from the cache the driver need
   not compile its shaders again (the cache's header names the device and driver: Vulkan ignores
   one from another). At exit the log says how many were made and how long that took. */
static VkPipelineCache pcache;
static double pipe_ms, pipe_max_ms;
static unsigned pipe_n;
static size_t pcache_loaded;

static void pcache_path(char *p, size_t n)
{
    snprintf(p, n, "%s/cache/pipelines.bin", user_dir);
}

static void pcache_save(void)
{
    if (!pcache)
        return;
    LOG(LOG_ALWAYS, "Vulkan: %u pipelines made, %.1f ms in all (slowest %.1f ms); the cache had %lu bytes at start",
        pipe_n, pipe_ms, pipe_max_ms, (unsigned long)pcache_loaded);
    size_t n = 0;
    if (vkGetPipelineCacheData(dev, pcache, &n, NULL) != VK_SUCCESS || !n)
        return;
    void *d = malloc(n);
    char p[1200], tmp[1210];
    pcache_path(p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.new", p);
    FILE *f = d && vkGetPipelineCacheData(dev, pcache, &n, d) == VK_SUCCESS ? fopen(tmp, "wb") : NULL;
    if (f) {
        int good = fwrite(d, 1, n, f) == n;
        if (fclose(f) == 0 && good)
            rename(tmp, p);
    }
    free(d);
}

static void pcache_open(void)
{
    char p[1200];
    snprintf(p, sizeof p, "%s/cache", user_dir);
    mkdir(p, 0755);
    pcache_path(p, sizeof p);
    void *d = NULL;
    FILE *f = fopen(p, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0 && (d = malloc((size_t)n)) && fread(d, 1, (size_t)n, f) == (size_t)n)
            pcache_loaded = (size_t)n;
        fclose(f);
    }
    VkPipelineCacheCreateInfo ci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = pcache_loaded;
    ci.pInitialData = pcache_loaded ? d : NULL;
    if (vkCreatePipelineCache(dev, &ci, NULL, &pcache) != VK_SUCCESS) {
        pcache_loaded = 0;
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        if (vkCreatePipelineCache(dev, &ci, NULL, &pcache) != VK_SUCCESS)
            pcache = VK_NULL_HANDLE;
    }
    free(d);
    if (pcache)
        atexit(pcache_save);
}

static VkPipeline pipeline_get(const VkDrawState *st, int topo_class)
{
    uint32_t sh = st->shader;
    uint32_t srcb = (sh >> 13) & 3, dstb = (sh >> 5) & 7;
    int blend = !(srcb == 1 && (dstb == 0 || dstb == 7));
    int cw = (sh >> 4) & 1;
    uint32_t key = (uint32_t)blend | srcb << 1 | dstb << 3 | (uint32_t)cw << 6 | (uint32_t)topo_class << 7 |
                   (uint32_t)(st->polymode & 3) << 9;
    for (int i = 0; i < npipes; i++)
        if (pipes[i].key == key)
            return pipes[i].p;
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";
    VkVertexInputBindingDescription bind = {0, sizeof(VkVert), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[5] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 16},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
        {3, 0, VK_FORMAT_R32G32_SFLOAT, 48},
        {4, 0, VK_FORMAT_R32G32_SFLOAT, 56},
    };
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 5;
    vi.pVertexAttributeDescriptions = attrs;
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = topo_class == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : topo_class == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                                     : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    (void)st;
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = st->polymode == 0 ? VK_POLYGON_MODE_POINT : st->polymode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    rs.depthBiasEnable = VK_TRUE;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState ba = {0};
    ba.blendEnable = blend ? VK_TRUE : VK_FALSE;
    ba.srcColorBlendFactor = ba.srcAlphaBlendFactor = src_factors[srcb];
    ba.dstColorBlendFactor = ba.dstAlphaBlendFactor = dst_factors[dstb];
    ba.colorBlendOp = ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.colorWriteMask = cw ? 0xf : 0;
    VkPipelineColorBlendStateCreateInfo cbs = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &ba;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
                            VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                            VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                            VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = sizeof dyn / sizeof dyn[0];
    dy.pDynamicStates = dyn;
    VkFormat cf = VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo prc = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    prc.colorAttachmentCount = 1;
    prc.pColorAttachmentFormats = &cf;
    prc.depthAttachmentFormat = depth_format;
    VkGraphicsPipelineCreateInfo pc = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pc.pNext = &prc;
    pc.stageCount = 2;
    pc.pStages = stages;
    pc.pVertexInputState = &vi;
    pc.pInputAssemblyState = &ia;
    pc.pViewportState = &vps;
    pc.pRasterizationState = &rs;
    pc.pMultisampleState = &ms;
    pc.pDepthStencilState = &ds;
    pc.pColorBlendState = &cbs;
    pc.pDynamicState = &dy;
    pc.layout = playout;
    VkPipeline p;
    double t0 = SDL_GetTicksNS() / 1e6;
    VK_CHECK(vkCreateGraphicsPipelines(dev, pcache, 1, &pc, NULL, &p));
    double took = SDL_GetTicksNS() / 1e6 - t0;
    pipe_ms += took;
    pipe_n++;
    if (took > pipe_max_ms) pipe_max_ms = took;
    if (npipes < (int)(sizeof pipes / sizeof pipes[0]))
        pipes[npipes++] = (Pipe){key, p};
    LOG(LOG_D3D, "Vulkan: pipeline %x made (%d)", key, npipes);
    return p;
}

/* ------------------------------------------------------------------ device */
static int create_swapchain(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps));
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xffffffffu)
        ext = (VkExtent2D){(uint32_t)pw, (uint32_t)ph};
    if (!ext.width || !ext.height)
        return 0;                        /* minimised */
    uint32_t nf = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, NULL);
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &nf, fmts);
    VkSurfaceFormatKHR sf = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if ((fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) &&
            fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            sf = fmts[i];
            break;
        }
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    extern const char *option_str(const char *, const char *);
    if (!strcmp(option_str("vsync", "on"), "off")) {
        uint32_t nm = 0;
        VkPresentModeKHR modes[16];
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &nm, NULL);
        if (nm > 16) nm = 16;
        vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &nm, modes);
        for (uint32_t i = 0; i < nm; i++)
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR || modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR)
                mode = modes[i];
    }
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = surface;
    ci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount)
        ci.minImageCount = caps.maxImageCount;
    ci.imageFormat = sf.format;
    ci.imageColorSpace = sf.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain;
    VkSwapchainKHR sc;
    VK_CHECK(vkCreateSwapchainKHR(dev, &ci, NULL, &sc));
    if (swapchain) {
        vkDeviceWaitIdle(dev);
        vkDestroySwapchainKHR(dev, swapchain, NULL);
        for (uint32_t i = 0; i < sc_count; i++)
            vkDestroySemaphore(dev, sc_done[i], NULL);
    }
    swapchain = sc;
    sc_format = sf.format;
    sc_extent = ext;
    sc_count = 8;
    VK_CHECK(vkGetSwapchainImagesKHR(dev, swapchain, &sc_count, sc_images));
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < sc_count; i++)
        VK_CHECK(vkCreateSemaphore(dev, &si, NULL, &sc_done[i]));
    sc_stale = 0;
    LOG(LOG_ALWAYS, "Vulkan: window %ux%u, %u images, %s", ext.width, ext.height, sc_count,
        mode == VK_PRESENT_MODE_FIFO_KHR ? "vsync" : "no vsync");
    return 1;
}

int vk_ok(void) { return ok; }

int vk_init(void *sdl_window)
{
    if (tried)
        return ok;
    tried = 1;
    win = sdl_window;
    if (win && !SDL_Vulkan_LoadLibrary(NULL) && 0)
        return 0;
    const char *exts[16];
    uint32_t next = 0;
    if (win) {
        Uint32 n = 0;
        const char *const *se = SDL_Vulkan_GetInstanceExtensions(&n);
        for (Uint32 i = 0; se && i < n && next < 14; i++)
            exts[next++] = se[i];
    }
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "TDOR3 native";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ic = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ic.pApplicationInfo = &app;
    ic.enabledExtensionCount = next;
    ic.ppEnabledExtensionNames = exts;
    const char *layer = "VK_LAYER_KHRONOS_validation";
    if (getenv("TDOR3_VK_VALIDATE")) {
        ic.enabledLayerCount = 1;
        ic.ppEnabledLayerNames = &layer;
    }
    if (vkCreateInstance(&ic, NULL, &inst) != VK_SUCCESS) {
        LOG(LOG_ALWAYS, "Vulkan: no instance");
        return 0;
    }
    if (win && !SDL_Vulkan_CreateSurface(win, inst, NULL, &surface)) {
        LOG(LOG_ALWAYS, "Vulkan: no surface: %s", SDL_GetError());
        return 0;
    }
    uint32_t np = 8;
    VkPhysicalDevice pds[8];
    vkEnumeratePhysicalDevices(inst, &np, pds);
    for (uint32_t i = 0; i < np && !phys; i++) {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(pds[i], &pp);
        if (pp.apiVersion < VK_API_VERSION_1_3)
            continue;
        uint32_t nq = 16;
        VkQueueFamilyProperties qp[16];
        vkGetPhysicalDeviceQueueFamilyProperties(pds[i], &nq, qp);
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 pres = VK_TRUE;
            if (surface)
                vkGetPhysicalDeviceSurfaceSupportKHR(pds[i], q, surface, &pres);
            if ((qp[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && pres) {
                phys = pds[i];
                qfam = q;
                LOG(LOG_ALWAYS, "Vulkan: %s", pp.deviceName);
                break;
            }
        }
    }
    if (!phys) {
        LOG(LOG_ALWAYS, "Vulkan: no device with Vulkan 1.3");
        return 0;
    }
    vkGetPhysicalDeviceMemoryProperties(phys, &memprops);
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qc = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qc.queueFamilyIndex = qfam;
    qc.queueCount = 1;
    qc.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    f12.descriptorIndexing = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.pNext = &f12;
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    f2.features.fillModeNonSolid = VK_TRUE;
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(phys, &have);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    float aniso = (float)atof(option_str("anisotropic", "16"));
    if (aniso > props.limits.maxSamplerAnisotropy) aniso = props.limits.maxSamplerAnisotropy;
    if (!have.samplerAnisotropy) aniso = 0;
    f2.features.samplerAnisotropy = aniso > 1 ? VK_TRUE : VK_FALSE;
    const char *dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dc = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dc.pNext = &f2;
    dc.queueCreateInfoCount = 1;
    dc.pQueueCreateInfos = &qc;
    dc.enabledExtensionCount = surface ? 1 : 0;
    dc.ppEnabledExtensionNames = dexts;
    if (vkCreateDevice(phys, &dc, NULL, &dev) != VK_SUCCESS) {
        LOG(LOG_ALWAYS, "Vulkan: no device");
        return 0;
    }
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    pcache_open();
    /* depth: 32-bit float where it can be drawn into */
    depth_format = VK_FORMAT_D32_SFLOAT;
    /* frames */
    for (int i = 0; i < FRAMES; i++) {
        Frame *f = &frames[i];
        VkCommandPoolCreateInfo pc = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pc.queueFamilyIndex = qfam;
        VK_CHECK(vkCreateCommandPool(dev, &pc, NULL, &f->pool));
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f->pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(dev, &ai, &f->cmd));
        VkFenceCreateInfo fc = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VK_CHECK(vkCreateFence(dev, &fc, NULL, &f->fence));
        VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(dev, &si, NULL, &f->acquired));
        VkBufferCreateInfo bc = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bc.size = RING_SIZE;
        bc.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VK_CHECK(vkCreateBuffer(dev, &bc, NULL, &f->ring));
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(dev, f->ring, &mr);
        VkMemoryAllocateInfo ma = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ma.allocationSize = mr.size;
        ma.memoryTypeIndex = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                         VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        VK_CHECK(vkAllocateMemory(dev, &ma, NULL, &f->ring_mem));
        VK_CHECK(vkBindBufferMemory(dev, f->ring, f->ring_mem, 0));
        VK_CHECK(vkMapMemory(dev, f->ring_mem, 0, RING_SIZE, 0, (void **)&f->ring_ptr));
    }
    /* samplers */
    for (int s = 0; s < NSAMPLERS; s++) {
        VkSamplerCreateInfo sc = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sc.magFilter = sc.minFilter = (s & VS_LINEAR) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        sc.mipmapMode = (s & VS_MIP_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sc.addressModeU = (s & VS_CLAMP_U) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sc.addressModeV = (s & VS_CLAMP_V) ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sc.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sc.maxLod = (s & (VS_MIP_NEAREST | VS_MIP_LINEAR)) ? 16.0f : 0.0f;
        if (aniso > 1 && (s & VS_LINEAR) && (s & (VS_MIP_NEAREST | VS_MIP_LINEAR))) {
            sc.anisotropyEnable = VK_TRUE;      /* option anisotropic (16; 0 or 1 off) */
            sc.maxAnisotropy = aniso;
        }
        VK_CHECK(vkCreateSampler(dev, &sc, NULL, &samplers[s]));
    }
    /* descriptors: the textures (bindless) and the samplers */
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, MAX_TEX, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_SAMPLER, NSAMPLERS, VK_SHADER_STAGE_FRAGMENT_BIT, samplers},
    };
    VkDescriptorBindingFlags bf[2] = {VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT, 0};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount = 2;
    bfi.pBindingFlags = bf;
    VkDescriptorSetLayoutCreateInfo dl = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.pNext = &bfi;
    dl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    dl.bindingCount = 2;
    dl.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(dev, &dl, NULL, &dsl));
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, MAX_TEX}, {VK_DESCRIPTOR_TYPE_SAMPLER, NSAMPLERS}};
    VkDescriptorPoolCreateInfo dp = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    dp.maxSets = 1;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = ps;
    VK_CHECK(vkCreateDescriptorPool(dev, &dp, NULL, &dpool));
    VkDescriptorSetAllocateInfo da = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = dpool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &dsl;
    VK_CHECK(vkAllocateDescriptorSets(dev, &da, &dset));
    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pl = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    VK_CHECK(vkCreatePipelineLayout(dev, &pl, NULL, &playout));
    VkShaderModuleCreateInfo sm = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    sm.codeSize = sizeof spv_draw_vert;
    sm.pCode = spv_draw_vert;
    VK_CHECK(vkCreateShaderModule(dev, &sm, NULL, &vs));
    sm.codeSize = sizeof spv_draw_frag;
    sm.pCode = spv_draw_frag;
    VK_CHECK(vkCreateShaderModule(dev, &sm, NULL, &fs));
    texs = calloc(MAX_TEX, sizeof *texs);
    free_tex = calloc(MAX_TEX, sizeof *free_tex);
    ntex = 1;                            /* 0: none */
    if (surface && !create_swapchain())
        sc_stale = 1;
    ok = 1;
    frame_begin();
    layout();
    if (!color.img)
        target_make();
    apply_view();
    return 1;
}

/* ------------------------------------------------------------------ frames */
static void frame_begin(void)
{
    Frame *f = &frames[fi];
    VK_CHECK(vkWaitForFences(dev, 1, &f->fence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(dev, 1, &f->fence));
    for (int i = 0; i < f->ntrash; i++) {
        Tex *t = &texs[f->trash[i]];
        img_free(&t->im);
        t->used = 0;
        free_tex[nfree_tex++] = f->trash[i];
    }
    f->ntrash = 0;
    f->ring_used = 0;
    VK_CHECK(vkResetCommandBuffer(f->cmd, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f->cmd, &bi));
    recording = 1;
}

/* Submit what is recorded and wait for it (CPU reads of the target, a full ring); recording
   then goes on in the same frame slot. */
static void flush_wait(void)
{
    Frame *f = &frames[fi];
    end_rendering();
    VK_CHECK(vkEndCommandBuffer(f->cmd));
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = f->cmd;
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    VK_CHECK(vkQueueSubmit2(queue, 1, &si, f->fence));
    recording = 0;
    frame_begin();
}

/* The target's size: the window's in pixels (or "resolution" = original, WxH, or a height),
   as wide as the window with widescreen on (else 4:3, the window showing bars). Without a window
   (tests): "resolution" WxH or the game's size. */
static void layout(void)
{
    const char *res = option_str("resolution", "native");
    int ws = option_bool("widescreen", 1);
    int pw = lw, ph = lh, rw = 0, rh = 0;
    if (win)
        SDL_GetWindowSizeInPixels(win, &pw, &ph);
    if (sscanf(res, "%dx%d", &rw, &rh) == 2 && rw > 0 && rh > 0) {
        if (!win) { pw = rw; ph = rh; }
        else { pw = (int)((double)pw * rh / ph + 0.5); ph = rh; }   /* the window's shape at that height */
    } else if (sscanf(res, "%d", &rh) == 1 && rh > 0 && win) {
        pw = (int)((double)pw * rh / ph + 0.5);
        ph = rh;
    } else if (!strcasecmp(res, "original") || pw < 16 || ph < 16) {
        pw = lw;
        ph = lh;
    }
    int w, h;
    if ((long)pw * lh <= (long)ph * lw) {     /* narrower than the game: its shape, as wide as the window */
        w = pw;
        h = (int)((long)pw * lh / lw);
    } else {                                  /* wider: the window's height; the window's width with widescreen */
        h = ph;
        w = ws ? pw : (int)((long)ph * lw / lh);
    }
    /* option antialiasing: off, 2x (1.41 per side) or 4x (2 x 2): drawn larger, filtered
       down when shown (alpha-tested foliage too, which multisampling would leave jagged) */
    const char *aa = option_str("antialiasing", win ? "4x" : "off");   /* (tests: off unless asked) */
    double ss = !strcasecmp(aa, "4x") ? 2.0 : !strcasecmp(aa, "2x") ? 1.41421356 : 1.0;
    while (ss > 1.0 && (double)w * h * ss * ss > 5120.0 * 3200.0)
        ss = ss > 1.5 ? 1.41421356 : 1.0;            /* (bounded: 4x at 2560x1600 at most) */
    w = (int)(w * ss + 0.5);
    h = (int)(h * ss + 0.5);
    if (w > 16384) w = 16384;
    if (h > 16384) h = 16384;
    int naw = (int)((long)h * lw / lh);
    if (naw > w) naw = w;
    if (w == tw && h == th && naw == aw && color.img)
        return;
    tw = w;
    th = h;
    aw = naw;
    ax = (tw - aw) / 2;
    sx = (float)aw / lw;
    sy = (float)th / lh;
    wk = (float)tw / aw;
    if (!ok)
        return;
    end_rendering();
    flush_wait();
    target_make();
    apply_view();
}

void vk_target_size(int w, int h)
{
    if (w <= 0 || h <= 0 || (w == lw && h == lh))
        return;
    lw = w;
    lh = h;
    lvp[0] = lvp[1] = 0; lvp[2] = (float)w; lvp[3] = (float)h; lvp[4] = 0; lvp[5] = 1;
    lsc[0] = lsc[1] = 0; lsc[2] = w; lsc[3] = h;
    tw = 0;                                   /* made again */
    if (ok)
        layout();
}

/* options changed in the game's menu (menus.c): v-sync needs a new swapchain */
void vk_options_changed(void) { sc_stale = 1; }

void vk_get_target_size(int *w, int *h) { *w = lw; *h = lh; }
void vk_target_pixels(int *w, int *h) { *w = tw; *h = th; }

/* where the target was last shown in the window (vk_present): scale and offset, pixels */
static double shown_s = 1;
static int shown_dx, shown_dy;

/* a point of the window (its pixels) as the game's pixel (mouse input) */
void vk_window_to_game(double px, double py, int *gx, int *gy)
{
    double tx = (px - shown_dx) / shown_s, ty = (py - shown_dy) / shown_s;
    double x = (tx - ax) / sx, y = ty / sy;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x > lw - 1) x = lw - 1;
    if (y > lh - 1) y = lh - 1;
    *gx = (int)x;
    *gy = (int)y;
}
float vk_widening(void) { return option_bool("widescreen", 1) ? wk : 1.0f; }

/* HUD overlays in widescreen (srdd.c): drawn across the whole width at the game's scale,
   moved out to the screen's left or right edge (anchor -1 / 1) or kept in the middle (0);
   -2 ends it */
static int hud_on, hud_anchor;

void vk_hud(int anchor)
{
    int on = anchor != -2 && wk > 1.001f;
    hud_anchor = anchor;
    if (on == hud_on)
        return;
    hud_on = on;
    apply_view();
}

void vk_wide(int on)
{
    scene_on = on;
    on = on && wk > 1.001f;
    if (on == wide)
        return;
    wide = on;
    apply_view();
}

void vk_viewport(int x, int y, int w, int h, float zmin, float zmax)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    lvp[0] = (float)x; lvp[1] = (float)y; lvp[2] = (float)w; lvp[3] = (float)h; lvp[4] = zmin; lvp[5] = zmax;
    apply_view();
}

void vk_scissor(int x0, int y0, int x1, int y1)
{
    lsc[0] = x0; lsc[1] = y0; lsc[2] = x1; lsc[3] = y1;
    apply_view();
}

/* The game's viewport and scissor on the target: in its area, or for a widened scene spread
   about the viewport's middle (a whole-screen one then spans the target). The scissor in force
   is the engine's within the viewport, within the target. */
static void apply_view(void)
{
    float kx = sx, ox = (float)ax;
    if (wide || hud_on) {
        float cl = lvp[0] + lvp[2] * 0.5f;          /* the viewport's middle, game x */
        kx = sx * wk;
        ox = (float)ax + cl * sx - cl * kx;
    }
    cur_vp = (VkViewport){ox + lvp[0] * kx, lvp[1] * sy, lvp[2] * kx, lvp[3] * sy, lvp[4], lvp[5]};
    int l0 = (int)lvp[0] > lsc[0] ? (int)lvp[0] : lsc[0];
    int t0 = (int)lvp[1] > lsc[1] ? (int)lvp[1] : lsc[1];
    int l1 = (int)(lvp[0] + lvp[2]) < lsc[2] ? (int)(lvp[0] + lvp[2]) : lsc[2];
    int t1 = (int)(lvp[1] + lvp[3]) < lsc[3] ? (int)(lvp[1] + lvp[3]) : lsc[3];
    full_clip = l0 <= 0 && t0 <= 0 && l1 >= lw && t1 >= lh;
    int x0 = (int)floorf(ox + l0 * kx + 0.5f), x1 = (int)floorf(ox + l1 * kx + 0.5f);
    int y0 = (int)floorf(t0 * sy + 0.5f), y1 = (int)floorf(t1 * sy + 0.5f);
    if ((wide || hud_on) && l0 <= 0 && l1 >= lw) { x0 = 0; x1 = tw; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > tw) x1 = tw;
    if (y1 > th) y1 = th;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    cur_sc = (VkRect2D){{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    if (rendering) {
        vkCmdSetViewport(frames[fi].cmd, 0, 1, &cur_vp);
        vkCmdSetScissor(frames[fi].cmd, 0, 1, &cur_sc);
    }
}

/* A clear of the game's whole screen clears the whole target (the sides of a widened view too). */
void vk_clear(int col, int dep, const float rgba[4], float z)
{
    if (!ok || (!col && !dep) || !cur_sc.extent.width || !cur_sc.extent.height)
        return;
    begin_rendering();
    VkClearAttachment ca[2];
    int n = 0;
    if (col) {
        ca[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca[n].colorAttachment = 0;
        for (int i = 0; i < 4; i++)
            ca[n].clearValue.color.float32[i] = rgba[i];
        n++;
    }
    if (dep) {
        ca[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        ca[n].clearValue.depthStencil.depth = z;
        n++;
    }
    VkClearRect r = {cur_sc, 0, 1};
    if (full_clip)
        r.rect = (VkRect2D){{0, 0}, {(uint32_t)tw, (uint32_t)th}};
    vkCmdClearAttachments(frames[fi].cmd, (uint32_t)n, ca, 1, &r);
}

/* Strips and fans as lists (one pipeline topology per class; no restarts to worry about). */
static uint32_t *expand(int prim, const uint32_t *idx, int n, int *nout)
{
    uint32_t *o;
    int m = 0;
    switch (prim) {
    case VP_TRISTRIP:
        o = malloc(sizeof *o * (size_t)(n > 2 ? 3 * (n - 2) : 1));
        for (int i = 0; i + 2 < n; i++) {
            if (i & 1) { o[m++] = idx[i + 1]; o[m++] = idx[i]; o[m++] = idx[i + 2]; }
            else { o[m++] = idx[i]; o[m++] = idx[i + 1]; o[m++] = idx[i + 2]; }
        }
        break;
    case VP_TRIFAN:
        o = malloc(sizeof *o * (size_t)(n > 2 ? 3 * (n - 2) : 1));
        for (int i = 1; i + 1 < n; i++) { o[m++] = idx[0]; o[m++] = idx[i]; o[m++] = idx[i + 1]; }
        break;
    case VP_LINESTRIP:
        o = malloc(sizeof *o * (size_t)(n > 1 ? 2 * (n - 1) : 1));
        for (int i = 0; i + 1 < n; i++) { o[m++] = idx[i]; o[m++] = idx[i + 1]; }
        break;
    case VP_TRIANGLES:
        n -= n % 3;
        /* fall through */
    default:
        o = malloc(sizeof *o * (size_t)(n ? n : 1));
        memcpy(o, idx, sizeof *o * (size_t)n);
        m = n;
        break;
    }
    *nout = m;
    return o;
}

static const VkCompareOp depth_ops[8] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL,
                                         VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER,
                                         VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL,
                                         VK_COMPARE_OP_ALWAYS};

void vk_draw(const VkDrawState *st, int prim, const VkVert *v, int nv, const uint32_t *idx, int ni)
{
    if (!ok || nv <= 0 || ni <= 0 || !cur_sc.extent.width || !cur_sc.extent.height)
        return;
    int n;
    uint32_t *ix = expand(prim, idx, ni, &n);
    if (n <= 0) { free(ix); return; }
    int cls = prim == VP_POINTS ? 0 : (prim == VP_LINES || prim == VP_LINESTRIP) ? 1 : 2;
    VkBuffer vb, ib;
    uint8_t *vp, *ip;
    uint32_t voff = ring_alloc((uint32_t)(nv * sizeof(VkVert)), 16, &vb, &vp);
    memcpy(vp, v, (size_t)nv * sizeof(VkVert));
    uint32_t ioff = ring_alloc((uint32_t)(n * 4), 4, &ib, &ip);
    memcpy(ip, ix, (size_t)n * 4);
    free(ix);
    begin_rendering();
    VkCommandBuffer cb = frames[fi].cmd;
    VkPipeline p = pipeline_get(st, cls);
    if (p != bound_pipe) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        bound_pipe = p;
    }
    vkCmdSetPrimitiveTopology(cb, cls == 0 ? VK_PRIMITIVE_TOPOLOGY_POINT_LIST : cls == 1 ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                                                          : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    /* cull 1: triangles counter-clockwise on the screen are dropped (Direct3D's CULL_CCW) */
    vkCmdSetFrontFace(cb, st->cull == 1 ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetCullMode(cb, st->cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
    vkCmdSetDepthTestEnable(cb, VK_TRUE);
    vkCmdSetDepthWriteEnable(cb, (st->shader >> 3) & 1 ? VK_TRUE : VK_FALSE);
    vkCmdSetDepthCompareOp(cb, depth_ops[st->shader & 7]);
    vkCmdSetDepthBias(cb, st->depth_bias, 0.0f, 0.0f);
    Push pc;
    memcpy(pc.m, st->matrix, sizeof pc.m);
    if (hud_on) {                        /* x/w scaled back to the game's size, moved to its edge */
        float shift = hud_anchor * (1.0f - 1.0f / wk);
        for (int k = 0; k < 4; k++)
            pc.m[k] = pc.m[k] / wk + shift * pc.m[12 + k];
    }
    pc.shader = st->shader;
    pc.tex0 = st->tex[0] > 0 && st->tex[0] < ntex && texs[st->tex[0]].used ? st->tex[0] : -1;
    pc.smp0 = st->smp[0] & (NSAMPLERS - 1);
    pc.tex1 = st->tex[1] > 0 && st->tex[1] < ntex && texs[st->tex[1]].used ? st->tex[1] : -1;
    pc.smp1 = st->smp[1] & (NSAMPLERS - 1);
    pc.fog_color = st->fog_color;
    pc.flags = pc.tex1 >= 0 ? 1u : 0u;
    int flat = !scene_on || hud_on;      /* 2D: menus, movies, HUD */
    pc.d3d_shift[0] = flat ? 0.5f * sx : 0.0f;
    pc.d3d_shift[1] = flat ? 0.5f * sy : 0.0f;
    if (flat) pc.flags |= 2u;
    if (LOGON(LOG_DRAW))
        LOG(LOG_DRAW, "  vk: scene %d wide %d hud %d shift %g,%g viewport %g %g %g %g", scene_on, wide, hud_on, pc.d3d_shift[0],
            pc.d3d_shift[1], lvp[0], lvp[1], lvp[2], lvp[3]);
    vkCmdPushConstants(cb, playout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    VkDeviceSize off = voff;
    vkCmdBindVertexBuffers(cb, 0, 1, &vb, &off);
    vkCmdBindIndexBuffer(cb, ib, ioff, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cb, (uint32_t)n, 1, 0, 0, 0);
}

/* ------------------------------------------------------------------ textures */
int vk_tex_new(int w, int h, int levels)
{
    if (!ok || w <= 0 || h <= 0)
        return 0;
    int id = nfree_tex ? free_tex[--nfree_tex] : ntex < MAX_TEX ? ntex++ : 0;
    if (!id)
        fatal("Vulkan: more than %d textures", MAX_TEX);
    Tex *t = &texs[id];
    img_make(&t->im, (uint32_t)w, (uint32_t)h, (uint32_t)levels, VK_FORMAT_B8G8R8A8_UNORM,
             VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    t->w = w;
    t->h = h;
    t->levels = levels;
    t->used = 1;
    /* cleared to transparent black, so a draw before its first upload is still valid */
    end_rendering();
    VkCommandBuffer cb = frames[fi].cmd;
    img_layout(cb, &t->im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue z = {{0, 0, 0, 0}};
    VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, (uint32_t)levels, 0, 1};
    vkCmdClearColorImage(cb, t->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &r);
    img_layout(cb, &t->im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    VkDescriptorImageInfo ii = {VK_NULL_HANDLE, t->im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wd = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wd.dstSet = dset;
    wd.dstBinding = 0;
    wd.dstArrayElement = (uint32_t)id;
    wd.descriptorCount = 1;
    wd.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    wd.pImageInfo = &ii;
    vkUpdateDescriptorSets(dev, 1, &wd, 0, NULL);
    return id;
}

void vk_tex_upload(int id, int level, int x, int y, int w, int h, const void *bgra, int pitch)
{
    if (!ok || id <= 0 || id >= ntex || !texs[id].used || w <= 0 || h <= 0)
        return;
    Tex *t = &texs[id];
    end_rendering();
    VkBuffer buf;
    uint8_t *p;
    uint32_t off = ring_alloc((uint32_t)(w * h * 4), 16, &buf, &p);
    for (int r = 0; r < h; r++)
        memcpy(p + (size_t)r * w * 4, (const uint8_t *)bgra + (size_t)r * pitch, (size_t)w * 4);
    end_rendering();
    VkCommandBuffer cb = frames[fi].cmd;
    img_layout(cb, &t->im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c = {0};
    c.bufferOffset = off;
    c.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)level, 0, 1};
    c.imageOffset = (VkOffset3D){x, y, 0};
    c.imageExtent = (VkExtent3D){(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(cb, buf, t->im.img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    img_layout(cb, &t->im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void vk_tex_free(int id)
{
    if (!ok || id <= 0 || id >= ntex || !texs[id].used)
        return;
    Frame *f = &frames[fi];
    if (f->ntrash == 256)
        flush_wait();
    f = &frames[fi];
    f->trash[f->ntrash++] = id;          /* destroyed once this frame's work is done */
}

/* ------------------------------------------------------------------ the target on the CPU */
static uint8_t *last_frame;              /* the last presented frame (PNG dumps) */
static int last_w, last_h;

/* Submits the frame so far and waits for it; recording goes on in the same slot (the ring kept:
   a copy just made into it is read after). */
static void submit_wait_keep(void)
{
    Frame *f = &frames[fi];
    VK_CHECK(vkEndCommandBuffer(f->cmd));
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = f->cmd;
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    VK_CHECK(vkQueueSubmit2(queue, 1, &si, f->fence));
    VK_CHECK(vkWaitForFences(dev, 1, &f->fence, VK_TRUE, UINT64_MAX));
    VK_CHECK(vkResetFences(dev, 1, &f->fence));
    VK_CHECK(vkResetCommandBuffer(f->cmd, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(f->cmd, &bi));
}

/* (in bands of rows the ring holds: a supersampled target is larger than it) */
static void copy_out(Img *im, int w, int h, uint8_t *bgra)
{
    int band = (int)(RING_SIZE / 4 / ((uint32_t)w * 4));
    for (int y = 0; y < h; y += band) {
        int n = h - y < band ? h - y : band;
        VkBuffer buf;
        uint8_t *p;
        uint32_t off = ring_alloc((uint32_t)(w * n * 4), 16, &buf, &p);
        VkCommandBuffer cb = frames[fi].cmd;
        img_layout(cb, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c = {0};
        c.bufferOffset = off;
        c.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        c.imageOffset = (VkOffset3D){0, y, 0};
        c.imageExtent = (VkExtent3D){(uint32_t)w, (uint32_t)n, 1};
        vkCmdCopyImageToBuffer(cb, im->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
        submit_wait_keep();
        memcpy(bgra + (size_t)y * w * 4, p, (size_t)w * n * 4);
    }
}

/* the whole target, in pixels (screenshots) */
void vk_read_target(uint8_t *bgra)
{
    if (!ok) { memset(bgra, 0, (size_t)tw * th * 4); return; }
    end_rendering();
    copy_out(&color, tw, th, bgra);
}

/* the game's area at the game's size (its reads of the back buffer) */
static void blit(Img *from, int fx0, int fy0, int fx1, int fy1, Img *to, int tx0, int ty0, int tx1, int ty1)
{
    VkCommandBuffer cb = frames[fi].cmd;
    img_layout(cb, from, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    img_layout(cb, to, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit b = {0};
    b.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.dstSubresource = b.srcSubresource;
    b.srcOffsets[0] = (VkOffset3D){fx0, fy0, 0};
    b.srcOffsets[1] = (VkOffset3D){fx1, fy1, 1};
    b.dstOffsets[0] = (VkOffset3D){tx0, ty0, 0};
    b.dstOffsets[1] = (VkOffset3D){tx1, ty1, 1};
    vkCmdBlitImage(cb, from->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &b, VK_FILTER_LINEAR);
}

void vk_read_game(uint8_t *bgra)
{
    if (!ok) { memset(bgra, 0, (size_t)lw * lh * 4); return; }
    end_rendering();
    if (tw == lw && th == lh) {
        copy_out(&color, tw, th, bgra);
        return;
    }
    blit(&color, ax, 0, ax + aw, th, &small, 0, 0, lw, lh);
    copy_out(&small, lw, lh, bgra);
}

/* a rectangle of the game's screen written (game pixels), scaled onto its area */
void vk_write_game(const uint8_t *bgra, int x, int y, int w, int h, int pitch)
{
    if (!ok || w <= 0 || h <= 0)
        return;
    end_rendering();
    VkBuffer buf;
    uint8_t *p;
    uint32_t off = ring_alloc((uint32_t)(w * h * 4), 16, &buf, &p);
    for (int r = 0; r < h; r++)
        memcpy(p + (size_t)r * w * 4, bgra + (size_t)r * pitch, (size_t)w * 4);
    int direct = tw == lw && th == lh;
    Img *im = direct ? &color : &small;
    VkCommandBuffer cb = frames[fi].cmd;
    img_layout(cb, im, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c = {0};
    c.bufferOffset = off;
    c.imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.imageOffset = (VkOffset3D){x, y, 0};
    c.imageExtent = (VkExtent3D){(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(cb, buf, im->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    if (!direct)
        blit(&small, x, y, x + w, y + h, &color, ax + (int)lroundf(x * sx), (int)lroundf(y * sy),
             ax + (int)lroundf((x + w) * sx), (int)lroundf((y + h) * sy));
}

/* ------------------------------------------------------------------ present */
void vk_present(void)
{
    if (!ok)
        return;
    static int keep = -1;
    if (keep < 0)
        keep = getenv("TDOR3_KEEP_FRAMES") != NULL;
    extern int shot_due(void);
    if (!surface && !keep && !shot_due()) {
        end_rendering();
        flush_wait();                    /* headless: the frame run, nothing kept */
        return;
    }
    if (!surface || keep || shot_due()) {
        /* headless (or frames kept for PNG dumps): the frame's pixels kept on the CPU */
        if (last_w != tw || last_h != th) {
            free(last_frame);
            last_frame = malloc((size_t)tw * th * 4);
            last_w = tw;
            last_h = th;
        }
        vk_read_target(last_frame);
        if (!surface)
            return;
    }
    Frame *f = &frames[fi];
    end_rendering();
    VkCommandBuffer cb = f->cmd;
    if (sc_stale && !create_swapchain()) {
        flush_wait();                    /* minimised: the frame is not shown */
        return;
    }
    uint32_t si_img;
    VkResult r = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, f->acquired, VK_NULL_HANDLE, &si_img);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        sc_stale = 1;
        flush_wait();
        return;
    }
    img_layout(cb, &color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    barrier(cb, sc_images[si_img], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cb, sc_images[si_img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
    /* the target scaled to fit the window, its shape kept (bars at the sides or top) */
    double sx = (double)sc_extent.width / tw, sy = (double)sc_extent.height / th;
    double s = sx < sy ? sx : sy;
    int dw = (int)(tw * s + 0.5), dh = (int)(th * s + 0.5);
    int dx = ((int)sc_extent.width - dw) / 2, dy = ((int)sc_extent.height - dh) / 2;
    shown_s = s;
    shown_dx = dx;
    shown_dy = dy;
    VkImageBlit b = {0};
    b.srcSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = (VkOffset3D){tw, th, 1};
    b.dstSubresource = b.srcSubresource;
    b.dstOffsets[0] = (VkOffset3D){dx, dy, 0};
    b.dstOffsets[1] = (VkOffset3D){dx + dw, dy + dh, 1};
    vkCmdBlitImage(cb, color.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, sc_images[si_img],
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    barrier(cb, sc_images[si_img], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    VK_CHECK(vkEndCommandBuffer(cb));
    VkSemaphoreSubmitInfo wait = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = f->acquired;
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sig.semaphore = sc_done[si_img];
    sig.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cb;
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &wait;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &sig;
    VK_CHECK(vkQueueSubmit2(queue, 1, &si, f->fence));
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &sc_done[si_img];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain;
    pi.pImageIndices = &si_img;
    r = vkQueuePresentKHR(queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        sc_stale = 1;
    recording = 0;
    fi = (fi + 1) % FRAMES;
    frame_begin();
    /* the window's size changed: a new swapchain next time, the target to match */
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(win, &pw, &ph);
    if ((uint32_t)pw != sc_extent.width || (uint32_t)ph != sc_extent.height)
        sc_stale = 1;
    layout();
}

void vk_stop(void)
{
    if (ok)
        vkDeviceWaitIdle(dev);
}

/* ------------------------------------------------------------------ PNG of the last frame */
static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t n)
{
    uint8_t be[4] = {(uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n};
    fwrite(be, 1, 4, f);
    fwrite(type, 1, 4, f);
    if (n) fwrite(data, 1, n, f);
    uLong crc = crc32(0, (const Bytef *)type, 4);
    if (n) crc = crc32(crc, data, n);
    uint8_t c[4] = {(uint8_t)(crc >> 24), (uint8_t)(crc >> 16), (uint8_t)(crc >> 8), (uint8_t)crc};
    fwrite(c, 1, 4, f);
}

int vk_write_png(const char *path)
{
    if (!last_frame)
        return 0;
    FILE *f = fopen(path, "wb");
    if (!f)
        return 0;
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ih[13] = {(uint8_t)(last_w >> 24), (uint8_t)(last_w >> 16), (uint8_t)(last_w >> 8), (uint8_t)last_w,
                      (uint8_t)(last_h >> 24), (uint8_t)(last_h >> 16), (uint8_t)(last_h >> 8), (uint8_t)last_h,
                      8, 2, 0, 0, 0};
    png_chunk(f, "IHDR", ih, 13);
    size_t raw_n = (size_t)last_h * (1 + (size_t)last_w * 3);
    uint8_t *raw = malloc(raw_n), *o = raw;
    for (int y = 0; y < last_h; y++) {
        *o++ = 0;
        const uint8_t *s = last_frame + (size_t)y * last_w * 4;
        for (int x = 0; x < last_w; x++, s += 4) {
            *o++ = s[2]; *o++ = s[1]; *o++ = s[0];
        }
    }
    uLongf zn = compressBound(raw_n);
    uint8_t *z = malloc(zn);
    compress2(z, &zn, raw, raw_n, 6);
    png_chunk(f, "IDAT", z, (uint32_t)zn);
    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
    free(raw);
    free(z);
    return 1;
}
