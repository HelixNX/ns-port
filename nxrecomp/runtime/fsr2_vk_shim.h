/* Forced into AMD FSR 2's sources (nxrecomp.cmake nx_fsr2): the runtime's Vulkan functions are pointers with
 * Vulkan's own names (vk.h, VK_NO_PROTOTYPES), and FSR 2's Vulkan backend calls six instance functions directly
 * (the rest it loads through the vkGetDeviceProcAddr it is given). Without prototypes those calls go through our
 * pointers; with them they would link to the pointer variables as if they were code. */
#pragma once
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
extern PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties;
extern PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties;
extern PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties;
extern PFN_vkGetPhysicalDeviceProperties2 vkGetPhysicalDeviceProperties2;
extern PFN_vkGetPhysicalDeviceFeatures2 vkGetPhysicalDeviceFeatures2;
extern PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
#ifdef __cplusplus
}
#endif
