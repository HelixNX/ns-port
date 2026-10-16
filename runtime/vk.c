/* Vulkan: instance and device (the discrete GPU), the SDL window and its swapchain, device memory. See vk.h. */
#include "vk.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <stdlib.h>
#include <windows.h>

#define VK_DEFINE(f) PFN_##f f;
VK_GLOBAL_FNS(VK_DEFINE)
VK_INSTANCE_FNS(VK_DEFINE)
VK_DEVICE_FNS(VK_DEFINE)
#undef VK_DEFINE

Vk vk;
static SDL_Window *window;
void input_event(const SDL_Event *e);
void input_poll(void);
static volatile LONG resized;

/* ---------------------------------------------------------------------------------------------- window */
void window_init(void) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) fatal("SDL_Init: %s", SDL_GetError());
    if (!SDL_Vulkan_LoadLibrary(NULL)) fatal("no Vulkan: %s", SDL_GetError());
    window = SDL_CreateWindow("Mario Kart 8 Deluxe", 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window) fatal("SDL_CreateWindow: %s", SDL_GetError());
}

/* the host's main thread: window events until the guest's main thread ends or the window is closed */
void window_loop(void *guest_main_thread) {
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 8)) {
            do {
                input_event(&e);
                if (e.type == SDL_EVENT_QUIT) {
                    hlog(1, "window closed");
                    fflush(stderr);
                    ExitProcess(0);
                }
                if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) InterlockedExchange(&resized, 1);
            } while (SDL_PollEvent(&e));
        }
        input_poll();
        if (WaitForSingleObject((HANDLE)guest_main_thread, 0) == WAIT_OBJECT_0) return;
    }
}

/* ---------------------------------------------------------------------------------------------- device */
static bool has_ext(const VkExtensionProperties *e, u32 n, const char *name) {
    for (u32 i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

static void load_instance(void) {
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!gipa) fatal("no vkGetInstanceProcAddr");
#define VK_LOAD_G(f) f = (PFN_##f)gipa(NULL, #f);
    VK_GLOBAL_FNS(VK_LOAD_G)
    u32 n;
    const char *const *sdl_ext = SDL_Vulkan_GetInstanceExtensions(&n);
    const char *ext[16];
    u32 next = 0;
    for (u32 i = 0; i < n && next < 15; i++) ext[next++] = sdl_ext[i];
    const char *layers[1];
    u32 nlayers = 0;
    if (getenv("MK8_VK_VALIDATION")) layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "mk8recomp",
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = next, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = nlayers, .ppEnabledLayerNames = layers};
    VKCHECK(vkCreateInstance(&ci, NULL, &vk.instance));
#define VK_LOAD_I(f) if (!(f = (PFN_##f)gipa(vk.instance, #f))) fatal("Vulkan: no %s", #f);
    VK_INSTANCE_FNS(VK_LOAD_I)
}

static const char *const device_exts[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
    VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
    VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME,
    VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME,
};

static VkPhysicalDevice pick_device(void) {
    u32 n = 0;
    vkEnumeratePhysicalDevices(vk.instance, &n, NULL);
    VkPhysicalDevice devs[8];
    if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(vk.instance, &n, devs);
    VkPhysicalDevice best = NULL;
    for (u32 i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        u32 ne = 0;
        vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ne, NULL);
        VkExtensionProperties *e = malloc(ne * sizeof *e);
        vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ne, e);
        bool ok = p.apiVersion >= VK_API_VERSION_1_3;
        for (size_t k = 0; k < sizeof device_exts / sizeof *device_exts; k++) ok = ok && has_ext(e, ne, device_exts[k]);
        free(e);
        hlog(1, "vk: %s (Vulkan %u.%u)%s", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
             VK_API_VERSION_MINOR(p.apiVersion), ok ? "" : ": missing features");
        if (ok && (!best || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) best = devs[i];
    }
    if (!best) fatal("no Vulkan 1.3 device with the extensions we need");
    return best;
}

void vk_init(void) {
    load_instance();
    if (!SDL_Vulkan_CreateSurface(window, vk.instance, NULL, &vk.surface)) fatal("surface: %s", SDL_GetError());
    vk.phys = pick_device();
    vkGetPhysicalDeviceProperties(vk.phys, &vk.props);
    vkGetPhysicalDeviceMemoryProperties(vk.phys, &vk.mem);
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostp = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hostp};
    vkGetPhysicalDeviceProperties2(vk.phys, &p2);
    vk.host_import_align = hostp.minImportedHostPointerAlignment;

    u32 nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.phys, &nq, NULL);
    VkQueueFamilyProperties qf[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(vk.phys, &nq, qf);
    vk.family = ~0u;
    for (u32 i = 0; i < nq && vk.family == ~0u; i++) {
        VkBool32 present = 0;
        vkGetPhysicalDeviceSurfaceSupportKHR(vk.phys, i, vk.surface, &present);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present)
            vk.family = i;
    }
    if (vk.family == ~0u) fatal("no graphics queue that can present");

    /* features: everything the NVN state maps to is dynamic state, so pipelines depend on shaders and formats */
    VkPhysicalDeviceIndexTypeUint8FeaturesKHR u8i = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_KHR,
                                                     .indexTypeUint8 = 1};
    VkPhysicalDeviceDepthClipControlFeaturesEXT clipc = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, &u8i, .depthClipControl = 1};
    VkPhysicalDeviceDepthClipEnableFeaturesEXT clip = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT,
                                                       &clipc, .depthClipEnable = 1};
    VkPhysicalDeviceRobustness2FeaturesEXT rob = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, &clip,
                                                  .nullDescriptor = 1};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, &rob,
        .customBorderColors = 1, .customBorderColorWithoutFormat = 1};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vin = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT, &border,
        .vertexInputDynamicState = 1};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, &vin,
        .extendedDynamicState2 = 1, .extendedDynamicState2LogicOp = 1, .extendedDynamicState2PatchControlPoints = 1};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, &eds2,
        .extendedDynamicState3PolygonMode = 1, .extendedDynamicState3RasterizationSamples = 1,
        .extendedDynamicState3SampleMask = 1, .extendedDynamicState3AlphaToCoverageEnable = 1,
        .extendedDynamicState3LogicOpEnable = 1, .extendedDynamicState3ColorBlendEnable = 1,
        .extendedDynamicState3ColorBlendEquation = 1, .extendedDynamicState3ColorWriteMask = 1,
        .extendedDynamicState3DepthClampEnable = 1, .extendedDynamicState3DepthClipEnable = 1};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &eds3,
        .dynamicRendering = 1, .synchronization2 = 1, .maintenance4 = 1, .shaderDemoteToHelperInvocation = 1};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13,
        .timelineSemaphore = 1, .bufferDeviceAddress = 1, .descriptorIndexing = 1, .runtimeDescriptorArray = 1,
        .descriptorBindingPartiallyBound = 1, .descriptorBindingSampledImageUpdateAfterBind = 1,
        .descriptorBindingUpdateUnusedWhilePending = 1, .shaderSampledImageArrayNonUniformIndexing = 1,
        .scalarBlockLayout = 1, .hostQueryReset = 1, .shaderFloat16 = 0, .samplerFilterMinmax = 1,
        .samplerMirrorClampToEdge = 1, .descriptorBindingVariableDescriptorCount = 1,
        .descriptorBindingUniformBufferUpdateAfterBind = 0, .descriptorBindingStorageBufferUpdateAfterBind = 0};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12,
        .shaderDrawParameters = 1};
    VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
    f.features = (VkPhysicalDeviceFeatures){
        .robustBufferAccess = 1, .imageCubeArray = 1, .independentBlend = 1, .geometryShader = 1, .tessellationShader = 1,
        .sampleRateShading = 1, .dualSrcBlend = 1, .logicOp = 1, .multiDrawIndirect = 1, .drawIndirectFirstInstance = 1,
        .depthClamp = 1, .depthBiasClamp = 1, .fillModeNonSolid = 1, .depthBounds = 1, .wideLines = 1,
        .largePoints = 1, .multiViewport = 1, .samplerAnisotropy = 1, .textureCompressionBC = 1,
        .occlusionQueryPrecise = 1, .vertexPipelineStoresAndAtomics = 1, .fragmentStoresAndAtomics = 1,
        .shaderImageGatherExtended = 1, .shaderStorageImageExtendedFormats = 1,
        .shaderStorageImageReadWithoutFormat = 1, .shaderStorageImageWriteWithoutFormat = 1,
        .shaderClipDistance = 1, .shaderCullDistance = 1, .shaderFloat64 = 1, .shaderInt64 = 1, .shaderInt16 = 1};
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = vk.family,
                                   .queueCount = 1, .pQueuePriorities = &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci,
                              .enabledExtensionCount = sizeof device_exts / sizeof *device_exts,
                              .ppEnabledExtensionNames = device_exts};
    VKCHECK(vkCreateDevice(vk.phys, &dci, NULL, &vk.dev));
#define VK_LOAD_D(f) if (!(f = (PFN_##f)vkGetDeviceProcAddr(vk.dev, #f))) fatal("Vulkan: no %s", #f);
    VK_DEVICE_FNS(VK_LOAD_D)
    vkGetDeviceQueue(vk.dev, vk.family, 0, &vk.queue);
    hlog(1, "vk: using %s, driver %x, host import alignment %llx", vk.props.deviceName, vk.props.driverVersion,
         vk.host_import_align);
}

/* ---------------------------------------------------------------------------------------------- memory */
u32 vk_memory_type(u32 bits, VkMemoryPropertyFlags want) {
    for (u32 i = 0; i < vk.mem.memoryTypeCount; i++)
        if (bits & 1u << i && (vk.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return ~0u;
}

#define BLOCK_SIZE (64ull << 20)
#define DEDICATED_MIN (16ull << 20)
typedef struct Range { VkDeviceSize off, size; } Range;
typedef struct Block {
    VkDeviceMemory mem;
    u32 type;
    u8 *map;
    Range *free;                        /* free ranges, sorted by offset */
    int nfree, capfree;
    struct Block *next;
} Block;
static Block *blocks;
static SRWLOCK alloc_lock = SRWLOCK_INIT;

static bool block_take(Block *b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize *off) {
    for (int i = 0; i < b->nfree; i++) {
        Range *r = &b->free[i];
        VkDeviceSize start = (r->off + align - 1) / align * align;
        if (start + size > r->off + r->size) continue;
        VkDeviceSize end = start + size, rend = r->off + r->size;
        if (start > r->off && end < rend) {             /* split in two */
            if (b->nfree == b->capfree) b->free = realloc(b->free, (b->capfree *= 2) * sizeof *b->free);
            memmove(&b->free[i + 2], &b->free[i + 1], (b->nfree - i - 1) * sizeof *b->free);
            b->nfree++;
            b->free[i].size = start - r->off;
            b->free[i + 1] = (Range){end, rend - end};
        } else if (start > r->off) {
            r->size = start - r->off;
        } else if (end < rend) {
            *r = (Range){end, rend - end};
        } else {
            memmove(r, r + 1, (b->nfree - i - 1) * sizeof *r);
            b->nfree--;
        }
        *off = start;
        return true;
    }
    return false;
}

static void block_give(Block *b, VkDeviceSize off, VkDeviceSize size) {
    int i = 0;
    while (i < b->nfree && b->free[i].off < off) i++;
    bool join_prev = i > 0 && b->free[i - 1].off + b->free[i - 1].size == off;
    bool join_next = i < b->nfree && off + size == b->free[i].off;
    if (join_prev && join_next) {
        b->free[i - 1].size += size + b->free[i].size;
        memmove(&b->free[i], &b->free[i + 1], (b->nfree - i - 1) * sizeof *b->free);
        b->nfree--;
    } else if (join_prev) {
        b->free[i - 1].size += size;
    } else if (join_next) {
        b->free[i].off = off;
        b->free[i].size += size;
    } else {
        if (b->nfree == b->capfree) b->free = realloc(b->free, (b->capfree *= 2) * sizeof *b->free);
        memmove(&b->free[i + 1], &b->free[i], (b->nfree - i) * sizeof *b->free);
        b->free[i] = (Range){off, size};
        b->nfree++;
    }
}

static bool alloc_memory(u32 type, VkDeviceSize size, VkDeviceMemory *mem, u8 **map) {
    VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                    .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, size, type};
    if (vkAllocateMemory(vk.dev, &ai, NULL, mem) != VK_SUCCESS) return false;
    *map = NULL;
    if (vk.mem.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        VKCHECK(vkMapMemory(vk.dev, *mem, 0, VK_WHOLE_SIZE, 0, (void **)map));
    return true;
}

bool vk_alloc(const VkMemoryRequirements *req, VkMemoryPropertyFlags want, VkAlloc *out) {
    u32 type = vk_memory_type(req->memoryTypeBits, want);
    if (type == ~0u) type = vk_memory_type(req->memoryTypeBits, want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (type == ~0u) return false;
    memset(out, 0, sizeof *out);
    out->size = req->size;
    if (req->size >= DEDICATED_MIN) return alloc_memory(type, req->size, &out->mem, &out->map);
    AcquireSRWLockExclusive(&alloc_lock);
    Block *b;
    VkDeviceSize off = 0;
    for (b = blocks; b; b = b->next)
        if (b->type == type && block_take(b, req->size, req->alignment, &off)) break;
    if (!b) {
        b = calloc(1, sizeof *b);
        if (!alloc_memory(type, BLOCK_SIZE, &b->mem, &b->map)) {
            free(b);
            ReleaseSRWLockExclusive(&alloc_lock);
            return false;
        }
        b->type = type;
        b->capfree = 16;
        b->free = malloc(b->capfree * sizeof *b->free);
        b->free[0] = (Range){0, BLOCK_SIZE};
        b->nfree = 1;
        b->next = blocks;
        blocks = b;
        block_take(b, req->size, req->alignment, &off);
    }
    ReleaseSRWLockExclusive(&alloc_lock);
    out->mem = b->mem;
    out->offset = off;
    out->block = b;
    out->map = b->map ? b->map + off : NULL;
    return true;
}

void vk_free(VkAlloc *a) {
    if (!a->mem) return;
    if (!a->block) vkFreeMemory(vk.dev, a->mem, NULL);
    else {
        AcquireSRWLockExclusive(&alloc_lock);
        block_give(a->block, a->offset, a->size);
        ReleaseSRWLockExclusive(&alloc_lock);
    }
    memset(a, 0, sizeof *a);
}

static const VkBufferUsageFlags ALL_BUFFER_USAGE =
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT |
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;

bool vk_import_host(u64 ptr, u64 size, HostBuffer *out) {
    memset(out, 0, sizeof *out);
    VkMemoryHostPointerPropertiesEXT hp = {VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    if (vkGetMemoryHostPointerPropertiesEXT(vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            (void *)(uintptr_t)ptr, &hp) != VK_SUCCESS) return false;
    VkExternalMemoryBufferCreateInfo ext = {VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ext, .size = size, .usage = ALL_BUFFER_USAGE};
    if (vkCreateBuffer(vk.dev, &bci, NULL, &out->buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.dev, out->buf, &req);
    u32 type = vk_memory_type(req.memoryTypeBits & hp.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == ~0u) type = vk_memory_type(req.memoryTypeBits & hp.memoryTypeBits, 0);
    VkImportMemoryHostPointerInfoEXT imp = {VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = (void *)(uintptr_t)ptr};
    VkMemoryAllocateFlagsInfo fl = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &imp,
                                    .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, size, type};
    if (type == ~0u || vkAllocateMemory(vk.dev, &ai, NULL, &out->mem) != VK_SUCCESS) {
        vkDestroyBuffer(vk.dev, out->buf, NULL);
        out->buf = NULL;
        return false;
    }
    VKCHECK(vkBindBufferMemory(vk.dev, out->buf, out->mem, 0));
    VkBufferDeviceAddressInfo bda = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = out->buf};
    out->addr = vkGetBufferDeviceAddress(vk.dev, &bda);
    return true;
}

void vk_release_host(HostBuffer *b) {
    if (b->buf) vkDestroyBuffer(vk.dev, b->buf, NULL);
    if (b->mem) vkFreeMemory(vk.dev, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

void vk_buffer(u64 size, VkBufferUsageFlags usage, VkMemoryPropertyFlags want, VkBuf *out) {
    memset(out, 0, sizeof *out);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                              .usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT};
    VKCHECK(vkCreateBuffer(vk.dev, &bci, NULL, &out->buf));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.dev, out->buf, &req);
    if (!vk_alloc(&req, want, &out->a)) fatal("vk: out of memory for a %llx-byte buffer", size);
    VKCHECK(vkBindBufferMemory(vk.dev, out->buf, out->a.mem, out->a.offset));
    VkBufferDeviceAddressInfo bda = {VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = out->buf};
    out->addr = vkGetBufferDeviceAddress(vk.dev, &bda);
    out->size = size;
}

void vk_buffer_free(VkBuf *b) {
    if (b->buf) vkDestroyBuffer(vk.dev, b->buf, NULL);
    vk_free(&b->a);
    memset(b, 0, sizeof *b);
}

/* ---------------------------------------------------------------------------------------------- swapchain */
#define MAX_IMAGES 8
#define PRESENT_FRAMES 3
static struct {
    VkSwapchainKHR sc;
    VkFormat format;
    VkExtent2D extent;
    u32 count;
    VkImage images[MAX_IMAGES];
    VkSemaphore done[MAX_IMAGES];           /* rendering into image i finished (waited on by the present) */
    VkSemaphore acquired[PRESENT_FRAMES];
    VkFence fence[PRESENT_FRAMES];
    VkCommandPool pool[PRESENT_FRAMES];
    VkCommandBuffer cmd[PRESENT_FRAMES];
    u32 frame;
} sw;

static void swapchain_create(void) {
    VkSurfaceCapabilitiesKHR caps;
    VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.phys, vk.surface, &caps));
    if (!sw.format) {
        u32 n = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(vk.phys, vk.surface, &n, NULL);
        VkSurfaceFormatKHR fm[64];
        if (n > 64) n = 64;
        vkGetPhysicalDeviceSurfaceFormatsKHR(vk.phys, vk.surface, &n, fm);
        sw.format = fm[0].format;
        for (u32 i = 0; i < n; i++)
            if (fm[i].format == VK_FORMAT_B8G8R8A8_UNORM && fm[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                sw.format = fm[i].format;
    }
    /* the game paces itself to 60 Hz (nvn.c): present without waiting for the display where possible */
    u32 nm = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk.phys, vk.surface, &nm, NULL);
    VkPresentModeKHR modes[16];
    if (nm > 16) nm = 16;
    vkGetPhysicalDeviceSurfacePresentModesKHR(vk.phys, vk.surface, &nm, modes);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (u32 i = 0; i < nm; i++) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i];
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == ~0u) {
        int w, h;
        SDL_GetWindowSizeInPixels(window, &w, &h);
        ext = (VkExtent2D){(u32)w, (u32)h};
    }
    sw.extent = ext;
    if (!ext.width || !ext.height) return;              /* minimised */
    u32 count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkSwapchainKHR old = sw.sc;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = vk.surface,
        .minImageCount = count, .imageFormat = sw.format, .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .preTransform = caps.currentTransform, .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = mode, .clipped = 1, .oldSwapchain = old};
    VKCHECK(vkCreateSwapchainKHR(vk.dev, &ci, NULL, &sw.sc));
    if (old) vkDestroySwapchainKHR(vk.dev, old, NULL);
    sw.count = MAX_IMAGES;
    VKCHECK(vkGetSwapchainImagesKHR(vk.dev, sw.sc, &sw.count, sw.images));
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (u32 i = 0; i < sw.count; i++) if (!sw.done[i]) VKCHECK(vkCreateSemaphore(vk.dev, &si, NULL, &sw.done[i]));
    hlog(1, "vk: swapchain %ux%u, %u images, present mode %d", ext.width, ext.height, sw.count, mode);
}

static void present_init(void) {
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < PRESENT_FRAMES; i++) {
        VKCHECK(vkCreateSemaphore(vk.dev, &si, NULL, &sw.acquired[i]));
        VKCHECK(vkCreateFence(vk.dev, &fi, NULL, &sw.fence[i]));
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = vk.family};
        VKCHECK(vkCreateCommandPool(vk.dev, &pci, NULL, &sw.pool[i]));
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = sw.pool[i],
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VKCHECK(vkAllocateCommandBuffers(vk.dev, &ai, &sw.cmd[i]));
    }
    swapchain_create();
}

static void image_barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = from, .newLayout = to, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cmd, &d);
}

void vk_present(VkImage src, s32 x, s32 y, s32 w, s32 h, bool flip_y, VkSemaphore wait, u64 wait_value) {
    if (!sw.fence[0]) present_init();
    if (InterlockedExchange(&resized, 0) || !sw.sc) {
        vkDeviceWaitIdle(vk.dev);
        swapchain_create();
    }
    if (!sw.sc || !sw.extent.width) return;
    u32 f = sw.frame++ % PRESENT_FRAMES;
    VKCHECK(vkWaitForFences(vk.dev, 1, &sw.fence[f], 1, UINT64_MAX));
    u32 idx;
    VkResult r = vkAcquireNextImageKHR(vk.dev, sw.sc, UINT64_MAX, sw.acquired[f], NULL, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { InterlockedExchange(&resized, 1); return; }
    if (r < 0) fatal("vkAcquireNextImageKHR: %d", r);
    VKCHECK(vkResetFences(vk.dev, 1, &sw.fence[f]));
    VKCHECK(vkResetCommandPool(vk.dev, sw.pool[f], 0));
    VkCommandBuffer cmd = sw.cmd[f];
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKCHECK(vkBeginCommandBuffer(cmd, &bi));
    VkImage dst = sw.images[idx];
    image_barrier(cmd, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    image_barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (src && w > 0 && h > 0) {
        /* keep the aspect ratio: letterbox or pillarbox */
        u32 W = sw.extent.width, H = sw.extent.height;
        u32 dw = W, dh = (u32)((u64)W * h / w);
        if (dh > H) { dh = H; dw = (u32)((u64)H * w / h); }
        s32 dx = (s32)(W - dw) / 2, dy = (s32)(H - dh) / 2;
        VkImageBlit blit = {
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffsets = {{x, flip_y ? y + h : y, 0}, {x + w, flip_y ? y : y + h, 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffsets = {{dx, dy, 0}, {dx + (s32)dw, dy + (s32)dh, 1}}};
        vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, VK_FILTER_LINEAR);
    }
    image_barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    VKCHECK(vkEndCommandBuffer(cmd));
    VkSemaphoreSubmitInfo waits[2] = {
        {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = sw.acquired[f],
         .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
        {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait, .value = wait_value,
         .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    VkSemaphoreSubmitInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = sw.done[idx],
                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cmd};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = wait ? 2 : 1,
                        .pWaitSemaphoreInfos = waits, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi,
                        .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &sig};
    VKCHECK(vkQueueSubmit2(vk.queue, 1, &si, sw.fence[f]));
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &sw.done[idx],
                           .swapchainCount = 1, .pSwapchains = &sw.sc, .pImageIndices = &idx};
    r = vkQueuePresentKHR(vk.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) InterlockedExchange(&resized, 1);
    else if (r < 0) fatal("vkQueuePresentKHR: %d", r);
}
