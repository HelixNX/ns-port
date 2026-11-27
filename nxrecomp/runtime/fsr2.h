/* AMD FSR 2 (deps/FidelityFX-FSR2, nxrecomp.cmake nx_fsr2): temporal anti-aliasing / upscaling of one frame.
 * fsr2.cpp is the only file that sees AMD's API (C++); the gpu code calls these. Self-contained (no vk.h: that is
 * the C runtime's), so the device and the two functions it needs come in fsr2_init. Built only with NX_FSR2. */
#pragma once
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Fsr2Frame {
    /* inputs at the render size w x h, in VK_IMAGE_LAYOUT_GENERAL (fsr2_record moves them to SHADER_READ_ONLY for
     * the dispatch and back): the jittered colour, its depth (a depth-aspect view), motion vectors (RG16F, render
     * pixels, current -> previous position) */
    VkImage color, depth, mv;
    VkImageView color_view, depth_view, mv_view;
    VkFormat color_format, depth_format;
    uint32_t color_w, color_h;      /* the colour image's own size (the frame is its top-left w x h; 0: w x h) */
    /* the output at ow x oh, a storage image in GENERAL */
    VkImage out;
    VkImageView out_view;
    VkFormat out_format;
    uint32_t w, h, ow, oh;
    float jx, jy;                   /* the frame's jitter in render pixels (fsr2_jitter) */
    float dt_ms, near_z, far_z, fov_y, sharpness;
    bool depth_inverted, depth_infinite, reset;
} Fsr2Frame;

void fsr2_init(VkPhysicalDevice phys, VkDevice dev, PFN_vkGetDeviceProcAddr gdpa, PFN_vkCmdPipelineBarrier2 barrier2);
/* records FSR 2 into cmd (outside a render pass); false: not done (logged once per reason) */
bool fsr2_record(VkCommandBuffer cmd, const Fsr2Frame *f);
/* the jitter of frame `index` for this render / output width, in render pixels (FSR 2's Halton sequence) */
void fsr2_jitter(uint32_t index, uint32_t render_w, uint32_t out_w, float *jx, float *jy);
void fsr2_destroy(void);           /* (the device must be idle) */

#ifdef __cplusplus
}
#endif
