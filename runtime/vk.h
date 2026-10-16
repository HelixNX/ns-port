/* Vulkan under our NVN (CLAUDE.md, decision 4): one device, one queue, the window and its swapchain.
 * Functions are loaded at run time (no import library); the list below is every function the runtime calls. */
#ifndef VK_H
#define VK_H

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "hos.h"

#define VK_GLOBAL_FNS(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define VK_INSTANCE_FNS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) \
    X(vkEnumerateDeviceExtensionProperties) X(vkGetDeviceProcAddr) X(vkDestroySurfaceKHR) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkGetPhysicalDeviceImageFormatProperties)
#define VK_DEVICE_FNS(X) \
    X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkQueueSubmit2) X(vkQueueWaitIdle) \
    X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) X(vkAcquireNextImageKHR) \
    X(vkQueuePresentKHR) X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkGetMemoryHostPointerPropertiesEXT) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkGetBufferDeviceAddress) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) \
    X(vkSignalSemaphore) X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCreatePipelineCache) \
    X(vkGetPipelineCacheData) X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateQueryPool) X(vkResetQueryPool) \
    X(vkCmdPipelineBarrier2) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdClearAttachments) \
    X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkCmdBlitImage) X(vkCmdCopyImage) \
    X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdFillBuffer) \
    X(vkCmdUpdateBuffer) X(vkCmdResolveImage) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) \
    X(vkCmdPushConstants) X(vkCmdBindVertexBuffers2) X(vkCmdBindIndexBuffer) X(vkCmdDraw) X(vkCmdDrawIndexed) \
    X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDispatch) X(vkCmdDispatchIndirect) \
    X(vkCmdSetViewportWithCount) X(vkCmdSetScissorWithCount) X(vkCmdSetLineWidth) X(vkCmdSetDepthBias) \
    X(vkCmdSetBlendConstants) X(vkCmdSetDepthBounds) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) \
    X(vkCmdSetStencilReference) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetPrimitiveTopology) \
    X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) \
    X(vkCmdSetDepthBoundsTestEnable) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) \
    X(vkCmdSetRasterizerDiscardEnable) X(vkCmdSetDepthBiasEnable) X(vkCmdSetPrimitiveRestartEnable) \
    X(vkCmdSetVertexInputEXT) X(vkCmdSetPolygonModeEXT) X(vkCmdSetRasterizationSamplesEXT) \
    X(vkCmdSetSampleMaskEXT) X(vkCmdSetAlphaToCoverageEnableEXT) X(vkCmdSetLogicOpEnableEXT) \
    X(vkCmdSetLogicOpEXT) X(vkCmdSetColorBlendEnableEXT) X(vkCmdSetColorBlendEquationEXT) \
    X(vkCmdSetColorWriteMaskEXT) X(vkCmdSetDepthClampEnableEXT) X(vkCmdSetDepthClipEnableEXT) \
    X(vkCmdSetPatchControlPointsEXT) X(vkCmdWriteTimestamp2) X(vkCmdBeginQuery) X(vkCmdEndQuery) \
    X(vkCmdCopyQueryPoolResults) X(vkCmdResetQueryPool)

#define VK_DECLARE(f) extern PFN_##f f;
VK_GLOBAL_FNS(VK_DECLARE)
VK_INSTANCE_FNS(VK_DECLARE)
VK_DEVICE_FNS(VK_DECLARE)
#undef VK_DECLARE

#define VKCHECK(call) do { VkResult r_ = (call); if (r_ < 0) fatal("%s failed: %d", #call, (int)r_); } while (0)

typedef struct Vk {
    VkInstance instance;
    VkPhysicalDevice phys;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem;
    VkDevice dev;
    VkQueue queue;
    u32 family;
    VkSurfaceKHR surface;
    u64 host_import_align;              /* minImportedHostPointerAlignment */
} Vk;
extern Vk vk;

/* device memory: sub-allocated from 64 MB blocks (dedicated above 16 MB) */
typedef struct VkAlloc { VkDeviceMemory mem; VkDeviceSize offset, size; void *block; u8 *map; } VkAlloc;
bool vk_alloc(const VkMemoryRequirements *req, VkMemoryPropertyFlags want, VkAlloc *out);
void vk_free(VkAlloc *a);
u32 vk_memory_type(u32 bits, VkMemoryPropertyFlags want);

/* a buffer over guest memory, imported (the GPU reads and writes the guest's memory itself, as on the console) */
typedef struct HostBuffer { VkDeviceMemory mem; VkBuffer buf; VkDeviceAddress addr; } HostBuffer;
bool vk_import_host(u64 ptr, u64 size, HostBuffer *out);
void vk_release_host(HostBuffer *b);

/* a plain buffer (host-visible or device-local) */
typedef struct VkBuf { VkBuffer buf; VkAlloc a; VkDeviceAddress addr; u64 size; } VkBuf;
void vk_buffer(u64 size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, VkBuf *out);
void vk_buffer_free(VkBuf *b);

/* window: created on the host's main thread, which then runs window_loop until the guest exits */
void window_init(void);
void window_loop(void *guest_main_thread);
void vk_init(void);

/* swapchain: present `src` (in GENERAL layout, the region given) scaled into the window. Called by the
 * thread that owns the queue (gpu.c), after the work that rendered src has been submitted. */
void vk_present(VkImage src, s32 x, s32 y, s32 w, s32 h, VkSemaphore wait, u64 wait_value);

#endif
