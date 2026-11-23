/* AMD FSR 2 around one frame (fsr2.h). One context, made again when the sizes or the depth direction change; the
 * caller's images stay in GENERAL and are moved to SHADER_READ_ONLY_OPTIMAL only for the dispatch (FSR 2's Vulkan
 * backend binds its sampled inputs in that layout). NX_FSR2_DEBUG=1: FSR 2's own checks of the values it is given,
 * its messages in the log. */
#include "fsr2.h"
#include <ffx_fsr2.h>
#include <ffx_fsr2_vk.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

extern "C" void hlog(int level, const char *fmt, ...);

static VkPhysicalDevice g_phys;
static VkDevice g_dev;
static PFN_vkGetDeviceProcAddr g_gdpa;
static PFN_vkCmdPipelineBarrier2 g_barrier2;
static PFN_vkDeviceWaitIdle g_wait_idle;

static FfxFsr2Context g_ctx;
static void *g_scratch;
static bool g_have_ctx, g_broken;
static uint32_t g_w, g_h, g_ow, g_oh;
static bool g_inv, g_inf;

extern "C" void fsr2_init(VkPhysicalDevice phys, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa, PFN_vkCmdPipelineBarrier2 barrier2) {
    g_phys = phys, g_dev = dev, g_gdpa = gdpa, g_barrier2 = barrier2;
    g_wait_idle = (PFN_vkDeviceWaitIdle)gdpa(dev, "vkDeviceWaitIdle");
}

static void message(FfxFsr2MsgType type, const wchar_t *m) {
    static int n;
    if (n++ < 50) hlog(0, "fsr2: %s: %ls", type == FFX_FSR2_MESSAGE_TYPE_ERROR ? "error" : "warning", m);
}

static void destroy_ctx(void) {
    if (!g_have_ctx) return;
    if (g_wait_idle) g_wait_idle(g_dev);        /* its images may still be in use by frames in flight */
    ffxFsr2ContextDestroy(&g_ctx);
    g_have_ctx = false;
}

extern "C" void fsr2_destroy(void) {
    destroy_ctx();
    free(g_scratch);
    g_scratch = NULL;
}

static bool make_ctx(const Fsr2Frame *f) {
    if (g_have_ctx && g_w == f->w && g_h == f->h && g_ow == f->ow && g_oh == f->oh && g_inv == f->depth_inverted &&
        g_inf == f->depth_infinite)
        return true;
    destroy_ctx();
    size_t n = ffxFsr2GetScratchMemorySizeVK(g_phys);
    free(g_scratch);
    g_scratch = calloc(1, n);
    FfxFsr2ContextDescription d;
    memset(&d, 0, sizeof d);
    FfxErrorCode e = ffxFsr2GetInterfaceVK(&d.callbacks, g_scratch, n, g_phys, g_gdpa);
    if (e != FFX_OK) {
        hlog(0, "fsr2: no Vulkan interface (%d): off", (int)e);
        return false;
    }
    static int debug = -1;
    if (debug < 0) debug = getenv("NX_FSR2_DEBUG") && atoi(getenv("NX_FSR2_DEBUG"));
    /* auto exposure: the input is the tone-mapped picture, but FSR 2 still weighs its history by luminance */
    d.flags = FFX_FSR2_ENABLE_AUTO_EXPOSURE | (f->depth_inverted ? FFX_FSR2_ENABLE_DEPTH_INVERTED : 0) |
              (f->depth_infinite ? FFX_FSR2_ENABLE_DEPTH_INFINITE : 0) |
              (debug ? FFX_FSR2_ENABLE_DEBUG_CHECKING : 0);
    d.maxRenderSize = {f->w, f->h};
    d.displaySize = {f->ow, f->oh};
    d.device = ffxGetDeviceVK(g_dev);
    d.fpMessage = message;
    if ((e = ffxFsr2ContextCreate(&g_ctx, &d)) != FFX_OK) {
        hlog(0, "fsr2: context %ux%u -> %ux%u not made (%d): off", f->w, f->h, f->ow, f->oh, (int)e);
        return false;
    }
    g_have_ctx = true;
    g_w = f->w, g_h = f->h, g_ow = f->ow, g_oh = f->oh, g_inv = f->depth_inverted, g_inf = f->depth_infinite;
    hlog(0, "fsr2: context %ux%u -> %ux%u, depth %s", f->w, f->h, f->ow, f->oh, f->depth_inverted ? "inverted" : "0..1");
    return true;
}

static bool has_stencil(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
}

/* the three inputs GENERAL <-> SHADER_READ_ONLY_OPTIMAL around the dispatch */
static void inputs_layout(VkCommandBuffer cmd, const Fsr2Frame *f, bool to_read) {
    VkImageMemoryBarrier2 b[3];
    VkImage im[3] = {f->color, f->depth, f->mv};
    for (int i = 0; i < 3; i++) {
        VkImageAspectFlags asp = i != 1 ? VK_IMAGE_ASPECT_COLOR_BIT
                                        : VK_IMAGE_ASPECT_DEPTH_BIT | (has_stencil(f->depth_format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        b[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b[i].srcStageMask = b[i].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b[i].srcAccessMask = b[i].dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        b[i].oldLayout = to_read ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b[i].newLayout = to_read ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
        b[i].srcQueueFamilyIndex = b[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[i].image = im[i];
        b[i].subresourceRange = {asp, 0, 1, 0, 1};
    }
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 3;
    di.pImageMemoryBarriers = b;
    g_barrier2(cmd, &di);
}

extern "C" bool fsr2_record(VkCommandBuffer cmd, const Fsr2Frame *f) {
    if (g_broken || !g_dev) return false;
    if (!make_ctx(f)) {
        g_broken = true;
        return false;
    }
    inputs_layout(cmd, f, true);
    FfxFsr2DispatchDescription d;
    memset(&d, 0, sizeof d);
    d.commandList = ffxGetCommandListVK(cmd);
    d.color = ffxGetTextureResourceVK(&g_ctx, f->color, f->color_view, f->w, f->h, f->color_format, L"nx color");
    d.depth = ffxGetTextureResourceVK(&g_ctx, f->depth, f->depth_view, f->w, f->h, f->depth_format, L"nx depth");
    d.motionVectors = ffxGetTextureResourceVK(&g_ctx, f->mv, f->mv_view, f->w, f->h, VK_FORMAT_R16G16_SFLOAT, L"nx mv");
    d.output = ffxGetTextureResourceVK(&g_ctx, f->out, f->out_view, f->ow, f->oh, f->out_format, L"nx out",
                                       FFX_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {f->jx, f->jy};
    d.motionVectorScale = {1.0f, 1.0f};         /* (render pixels; FSR 2 divides by the render size) */
    d.renderSize = {f->w, f->h};
    d.enableSharpening = f->sharpness > 0;
    d.sharpness = f->sharpness;
    d.frameTimeDelta = f->dt_ms;
    d.preExposure = 1.0f;
    d.reset = f->reset;
    d.cameraNear = f->near_z;
    d.cameraFar = f->far_z;
    d.cameraFovAngleVertical = f->fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    FfxErrorCode e = ffxFsr2ContextDispatch(&g_ctx, &d);
    inputs_layout(cmd, f, false);
    if (e != FFX_OK) {
        hlog(0, "fsr2: dispatch failed (%d): off", (int)e);
        g_broken = true;
        return false;
    }
    return true;
}

extern "C" void fsr2_jitter(uint32_t index, uint32_t render_w, uint32_t out_w, float *jx, float *jy) {
    int32_t n = ffxFsr2GetJitterPhaseCount((int32_t)render_w, (int32_t)out_w);
    *jx = *jy = 0;
    if (n > 0) ffxFsr2GetJitterOffset(jx, jy, (int32_t)(index % (uint32_t)n), n);
}
