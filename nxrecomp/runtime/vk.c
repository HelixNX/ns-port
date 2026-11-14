/* Vulkan: instance and device (the discrete GPU), the SDL window and its swapchain, device memory. See vk.h. */
#include "vk.h"
#include "framerate.h"
#include "game_config.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <process.h>
#include <stdlib.h>
#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>
#include <cfgmgr32.h>
#include <initguid.h>
#include <devpkey.h>
#include <pciprop.h>
#include <wincodec.h>
#include "nca.h"

#pragma comment(lib, "windowscodecs.lib")

#define VK_DEFINE(f) PFN_##f f;
VK_GLOBAL_FNS(VK_DEFINE)
VK_INSTANCE_FNS(VK_DEFINE)
VK_DEVICE_FNS(VK_DEFINE)
VK_DEVICE_OPT_FNS(VK_DEFINE)
VK_LABEL_FNS(VK_DEFINE)
#undef VK_DEFINE

Vk vk;
VkPipelineShaderStageRequiredSubgroupSizeCreateInfo vk_warp = {
    VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, .requiredSubgroupSize = 32};
static SDL_Window *window;
void input_event(const SDL_Event *e);
void input_poll(void);
#include "options.h"
#include "input.h"
#include "mods.h"
#include "ui.h"

SDL_Window *vk_window(void) { return window; }
void gpu_capture_next(void);
void gpu_reload_textures(void);
void gpu_save_pipelines(void);
void overlay_toggle(void);
static volatile LONG resized;

/* ---------------------------------------------------------------------------------------------- window */
/* the game's own icon (its control data, from the user's NSPs: a JPEG) as the window's, decoded by WIC */
static void window_icon(void) {
    if (!nx_from_nsp()) return;
    u64 size;
    void *jpg = nx_game_control_file("icon_AmericanEnglish.dat", &size);
    if (!jpg) return;
    IWICImagingFactory *wic = NULL;
    IWICStream *st = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *fr = NULL;
    IWICFormatConverter *cv = NULL;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    UINT w = 0, h = 0;
    if (SUCCEEDED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&wic)) &&
        SUCCEEDED(wic->lpVtbl->CreateStream(wic, &st)) &&
        SUCCEEDED(st->lpVtbl->InitializeFromMemory(st, jpg, (DWORD)size)) &&
        SUCCEEDED(wic->lpVtbl->CreateDecoderFromStream(wic, (IStream *)st, NULL, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->lpVtbl->GetFrame(dec, 0, &fr)) &&
        SUCCEEDED(wic->lpVtbl->CreateFormatConverter(wic, &cv)) &&
        SUCCEEDED(cv->lpVtbl->Initialize(cv, (IWICBitmapSource *)fr, &GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                         NULL, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(cv->lpVtbl->GetSize(cv, &w, &h)) && w && h) {
        SDL_Surface *s = SDL_CreateSurface((int)w, (int)h, SDL_PIXELFORMAT_ABGR8888);     /* bytes R, G, B, A */
        if (s && SUCCEEDED(cv->lpVtbl->CopyPixels(cv, NULL, (UINT)s->pitch, (UINT)(s->pitch * h), s->pixels)))
            SDL_SetWindowIcon(window, s);
        if (s) SDL_DestroySurface(s);
    }
    if (cv) cv->lpVtbl->Release(cv);
    if (fr) fr->lpVtbl->Release(fr);
    if (dec) dec->lpVtbl->Release(dec);
    if (st) st->lpVtbl->Release(st);
    if (wic) wic->lpVtbl->Release(wic);
    free(jpg);
}

void window_init(void) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) fatal("SDL_Init: %s", SDL_GetError());
    if (!SDL_Vulkan_LoadLibrary(NULL)) fatal("Vulkan is not available (%s).\n\nInstall or update your graphics driver: the game needs a GPU with Vulkan 1.3.", SDL_GetError());
    window = SDL_CreateWindow(NX_TITLE, 1280, 720, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!window) fatal("SDL_CreateWindow: %s", SDL_GetError());
    window_icon();
}

/* the host's main thread: window events until the guest's main thread ends or the window is closed */
void window_loop(void *guest_main_thread) {
    options_init();                                     /* settings.ini, then the controls and the menu (ui.c) */
    input_init();
    ui_init(window);
    mods_load(window);
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 8)) {
            do {
                bool used = e.type != SDL_EVENT_QUIT && ui_event(&e);   /* Esc, the menu, rebinding, hotkeys */
                if (!used && e.type != SDL_EVENT_QUIT) used = mods_event(&e);   /* then the mods' (mods.c) */
#ifdef NX_PACKAGE                                       /* no input while the start-up's shaders are translated */
                if (!gpu_shaders_starting())
#endif
                if (!used) input_event(&e);
                if (e.type == SDL_EVENT_QUIT) {
                    hlog(1, "window closed");
                    gpu_save_pipelines();
                    fflush(stderr);
                    ExitProcess(0);
                }
                if (e.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) InterlockedExchange(&resized, 1);
                /* the menu, FPS counter, frame capture (F12) and frame rate keys are hotkeys (options.c, ui.c) */
#ifndef NX_PACKAGE                                      /* developer key: not in a package for players */
                if (e.type == SDL_EVENT_KEY_DOWN && e.key.scancode == SDL_SCANCODE_F11 && !e.key.repeat) gpu_reload_textures();
#endif
            } while (SDL_PollEvent(&e));
        }
        options_tick();                                 /* the menu's changes, applied here; settings.ini saved */
        ui_tick();
#ifdef NX_PACKAGE
        if (!gpu_shaders_starting())
#endif
        input_poll();
        if (WaitForSingleObject((HANDLE)guest_main_thread, 0) == WAIT_OBJECT_0) return;
    }
}

/* ---------------------------------------------------------------------------------------------- device */
static bool has_ext(const VkExtensionProperties *e, u32 n, const char *name) {
    for (u32 i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return true;
    return false;
}

/* validation layer messages (NX_VK_VALIDATION): each message id is logged its first few times */
static VKAPI_ATTR VkBool32 VKAPI_CALL vk_debug(VkDebugUtilsMessageSeverityFlagBitsEXT sev,
                                               VkDebugUtilsMessageTypeFlagsEXT type,
                                               const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    enum { SLOTS = 1024, TIMES = 3 };
    static volatile LONG ids[SLOTS], counts[SLOTS];
    u32 id = (u32)d->messageIdNumber, k = (id * 2654435761u) % SLOTS;
    for (u32 i = 0; i < SLOTS; i++, k = (k + 1) % SLOTS) {
        LONG cur = InterlockedCompareExchange(&ids[k], (LONG)id, 0);
        if (cur == 0 || cur == (LONG)id) {
            LONG n = InterlockedIncrement(&counts[k]);
            if (n > TIMES) return VK_FALSE;
            break;
        }
    }
    hlog(1, "vk %s: %s", sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning", d->pMessage);
    return VK_FALSE;
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
    bool validate = getenv("NX_VK_VALIDATION") != NULL;
    if (validate) layers[nlayers++] = "VK_LAYER_KHRONOS_validation";
    if (getenv("NX_GPU_LABELS")) {
        u32 ne = 0;
        vkEnumerateInstanceExtensionProperties(NULL, &ne, NULL);
        VkExtensionProperties *ep = calloc(ne ? ne : 1, sizeof *ep);
        vkEnumerateInstanceExtensionProperties(NULL, &ne, ep);
        for (u32 i = 0; i < ne; i++) vk.labels |= !strcmp(ep[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        free(ep);
        if (!vk.labels) hlog(0, "vk: NX_GPU_LABELS: no VK_EXT_debug_utils");
    }
    if (validate || vk.labels) ext[next++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = NX_NAME,
                             .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                               .enabledExtensionCount = next, .ppEnabledExtensionNames = ext,
                               .enabledLayerCount = nlayers, .ppEnabledLayerNames = layers};
    VKCHECK(vkCreateInstance(&ci, NULL, &vk.instance));
#define VK_LOAD_I(f) if (!(f = (PFN_##f)gipa(vk.instance, #f))) fatal("Vulkan: no %s", #f);
    VK_INSTANCE_FNS(VK_LOAD_I)
    if (vk.labels) {
#define VK_LOAD_L(f) f = (PFN_##f)gipa(vk.instance, #f);
        VK_LABEL_FNS(VK_LOAD_L)
        vk.labels = vkCmdBeginDebugUtilsLabelEXT && vkCmdEndDebugUtilsLabelEXT;
    }
    if (validate) {
        PFN_vkCreateDebugUtilsMessengerEXT create =
            (PFN_vkCreateDebugUtilsMessengerEXT)gipa(vk.instance, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mi = {
            VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            .pfnUserCallback = vk_debug};
        VkDebugUtilsMessengerEXT m;
        if (!create || create(vk.instance, &mi, NULL, &m) != VK_SUCCESS) hlog(1, "vk: no debug messenger");
    }
}

static const char *const device_exts[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
    VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME,
    VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME,
    VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME,
    VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME,
    VK_EXT_TRANSFORM_FEEDBACK_EXTENSION_NAME,
};
/* optional (vk_init): VK_KHR_index_type_uint8 and VK_EXT_vertex_input_dynamic_state have fallbacks (vk.u8_indices,
 * vk.vi_dynamic); AMD's GCN driver (RX 400 / 500, Vega) lacks both */

/* The GPU's PCIe link, for gpu.c's pool mirrors: the display kernel gives the PCI address of the adapter with
 * Vulkan's LUID (VK_EXT_pci_bus_info would too, but AMD's Windows driver lacks it), and Windows' device tree has
 * each PCIe device's link. Radeon cards put a switch of their own in front of the GPU
 * whose links report full speed, so the slowest link from the GPU up to the root port counts. Each link's best
 * speed and width (both ends' maxima) are used, not the current ones: GPUs drop to slower speeds while idle */
static bool devnode_u32(DEVINST d, const DEVPROPKEY *k, u32 *v) {
    DEVPROPTYPE t;
    ULONG size = sizeof *v;
    return CM_Get_DevNode_PropertyW(d, k, &t, (PBYTE)v, &size, 0) == CR_SUCCESS && t == DEVPROP_TYPE_UINT32;
}

static void pcie_link(void) {
    VkPhysicalDeviceIDProperties idp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
    VkPhysicalDeviceProperties2 p2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &idp};
    vkGetPhysicalDeviceProperties2(vk.phys, &p2);
    if (!idp.deviceLUIDValid) { hlog(1, "vk: PCIe link not known (no LUID)"); return; }
    D3DKMT_OPENADAPTERFROMLUID oa = {0};
    memcpy(&oa.AdapterLuid, idp.deviceLUID, sizeof oa.AdapterLuid);
    if (D3DKMTOpenAdapterFromLuid(&oa)) { hlog(1, "vk: PCIe link not known (no adapter for the LUID)"); return; }
    D3DKMT_ADAPTERADDRESS bus = {0};
    D3DKMT_QUERYADAPTERINFO qi = {oa.hAdapter, KMTQAITYPE_ADAPTERADDRESS, &bus, sizeof bus};
    NTSTATUS st = D3DKMTQueryAdapterInfo(&qi);
    D3DKMT_CLOSEADAPTER ca = {oa.hAdapter};
    D3DKMTCloseAdapter(&ca);
    if (st) { hlog(1, "vk: PCIe link not known (no adapter address)"); return; }
    ULONG len = 0;
    if (CM_Get_Device_ID_List_SizeW(&len, L"PCI", CM_GETIDLIST_FILTER_ENUMERATOR) != CR_SUCCESS) return;
    WCHAR *ids = malloc(len * sizeof *ids);
    DEVINST gpu = 0;
    if (CM_Get_Device_ID_ListW(L"PCI", ids, len, CM_GETIDLIST_FILTER_ENUMERATOR) == CR_SUCCESS)
        for (WCHAR *id = ids; *id && !gpu; id += wcslen(id) + 1) {
            DEVINST d;
            u32 busno, addr;
            if (CM_Locate_DevNodeW(&d, id, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS &&
                devnode_u32(d, &DEVPKEY_Device_BusNumber, &busno) && devnode_u32(d, &DEVPKEY_Device_Address, &addr) &&
                busno == bus.BusNumber && addr == (bus.DeviceNumber << 16 | bus.FunctionNumber))
                gpu = d;
        }
    free(ids);
    if (!gpu) { hlog(1, "vk: PCIe link not known (no PCI device at bus %u, device %u)", bus.BusNumber, bus.DeviceNumber); return; }
    /* GB/s per lane by speed code (2.5, 5, 8, 16, 32, 64 GT/s; 8b/10b coding up to 5 GT/s, 128b/130b above) */
    static const double lane[] = {0, 0.25, 0.5, 0.985, 1.969, 3.938, 7.563};
    u32 speed = ~0u, width = ~0u;
    for (DEVINST d = gpu, parent; ; d = parent) {
        u32 s, w;
        if (!devnode_u32(d, &DEVPKEY_PciDevice_MaxLinkSpeed, &s) || !devnode_u32(d, &DEVPKEY_PciDevice_MaxLinkWidth, &w))
            break;
        if (s && s < speed) speed = s;
        if (w && w < width) width = w;
        if (CM_Get_Parent(&parent, d, 0) != CR_SUCCESS) break;
    }
    if (speed == ~0u || width == ~0u) { hlog(1, "vk: PCIe link not known"); return; }
    vk.pcie_gbps = lane[speed < 7 ? speed : 6] * width;
    hlog(1, "vk: PCIe link gen %u x%u (%.1f GB/s)", speed, width, vk.pcie_gbps);
}

static VkPhysicalDevice pick_device(void) {
    u32 n = 0;
    vkEnumeratePhysicalDevices(vk.instance, &n, NULL);
    VkPhysicalDevice devs[8];
    if (n > 8) n = 8;
    vkEnumeratePhysicalDevices(vk.instance, &n, devs);
    VkPhysicalDevice best = NULL;
    char why[1024] = "";                                  /* each unusable GPU and what it lacks, for the message */
    size_t wl = 0;
    for (u32 i = 0; i < n; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        u32 ne = 0;
        vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ne, NULL);
        VkExtensionProperties *e = malloc((ne ? ne : 1) * sizeof *e);
        vkEnumerateDeviceExtensionProperties(devs[i], NULL, &ne, e);
        char miss[768] = "";
        size_t ml = 0;
        if (p.apiVersion < VK_API_VERSION_1_3)
            ml += snprintf(miss + ml, sizeof miss - ml, " Vulkan 1.3 (has %u.%u)", VK_API_VERSION_MAJOR(p.apiVersion),
                           VK_API_VERSION_MINOR(p.apiVersion));
        for (size_t k = 0; k < sizeof device_exts / sizeof *device_exts; k++)
            if (!has_ext(e, ne, device_exts[k]) && ml < sizeof miss)
                ml += snprintf(miss + ml, sizeof miss - ml, " %s", device_exts[k] + 3);    /* without "VK_" */
        free(e);
        bool ok = !miss[0];
        hlog(1, "vk: %s (Vulkan %u.%u, driver %x)%s%s", p.deviceName, VK_API_VERSION_MAJOR(p.apiVersion),
             VK_API_VERSION_MINOR(p.apiVersion), p.driverVersion, ok ? "" : ": missing", miss);
        if (!ok && wl < sizeof why) wl += snprintf(why + wl, sizeof why - wl, "\n\n%s lacks:%s", p.deviceName, miss);
        if (ok && (!best || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)) best = devs[i];
    }
    if (!best) fatal("No graphics card with Vulkan 1.3 and the extensions the game needs was found.%s\n\nUpdate your "
                     "graphics driver; older GPUs may not be supported.", n ? why : "\n\n(Vulkan lists no GPUs.)");
    return best;
}

void vk_init(void) {
    load_instance();
    if (!SDL_Vulkan_CreateSurface(window, vk.instance, NULL, &vk.surface)) fatal("surface: %s", SDL_GetError());
    vk.phys = pick_device();
    vkGetPhysicalDeviceProperties(vk.phys, &vk.props);
    vkGetPhysicalDeviceMemoryProperties(vk.phys, &vk.mem);
    if (vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) pcie_link();
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
    u32 family_queues = 1;
    for (u32 i = 0; i < nq && vk.family == ~0u; i++) {
        VkBool32 present = 0;
        vkGetPhysicalDeviceSurfaceSupportKHR(vk.phys, i, vk.surface, &present);
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) && present)
            vk.family = i, family_queues = qf[i].queueCount;
    }
    if (vk.family == ~0u) fatal("no graphics queue that can present");

    /* optional features: a dynamic logic op (else gpu.c puts it in the pipeline key), patch control points (unused:
       draws with tessellation shaders are skipped) */
    VkPhysicalDeviceRobustness2FeaturesEXT hasrob = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT has2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, &hasrob};
    VkPhysicalDeviceFeatures2 hasf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &has2};
    vkGetPhysicalDeviceFeatures2(vk.phys, &hasf);
    vk.dyn_logic_op = has2.extendedDynamicState2LogicOp;
    if (!vk.dyn_logic_op) hlog(1, "vk: no dynamic logic op: logic ops are part of the pipeline");

    /* 8-bit indices and per-draw vertex input, where the device has them (else the fallbacks in gpu_draw.c / gpu_pipe.c);
     * NX_NO_U8_INDICES=1 / NX_NO_VI_DYNAMIC=1 take the fallbacks on a GPU that has them (testing). NX_NO_VI_DYNAMIC
     * keeps the extension enabled: with it off, AMD's RDNA3 driver (RX 7800 XT, 2026-11-12) lost the device on the
     * first game draw whatever the pipelines held (docs/GPUS.md), which says nothing about our fallback */
    bool vi_keep = false;
    u32 ne = 0;
    vkEnumerateDeviceExtensionProperties(vk.phys, NULL, &ne, NULL);
    VkExtensionProperties *dext = malloc((ne ? ne : 1) * sizeof *dext);
    vkEnumerateDeviceExtensionProperties(vk.phys, NULL, &ne, dext);
    {
        VkPhysicalDeviceIndexTypeUint8FeaturesKHR hasu8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_KHR};
        VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT hasvi = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT};
        bool u8x = has_ext(dext, ne, VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME);
        bool vix = has_ext(dext, ne, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
        void *chain = NULL;
        if (u8x) hasu8.pNext = chain, chain = &hasu8;
        if (vix) hasvi.pNext = chain, chain = &hasvi;
        VkPhysicalDeviceFeatures2 hasf8 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, chain};
        vkGetPhysicalDeviceFeatures2(vk.phys, &hasf8);
        const char *nu8 = getenv("NX_NO_U8_INDICES"), *nvi = getenv("NX_NO_VI_DYNAMIC");
        vk.u8_indices = u8x && hasu8.indexTypeUint8 && !(nu8 && atoi(nu8));
        vk.vi_dynamic = vix && hasvi.vertexInputDynamicState && !(nvi && atoi(nvi));
        vi_keep = vix && hasvi.vertexInputDynamicState;   /* enabled even when NX_NO_VI_DYNAMIC takes the fallback */
        if (!vk.u8_indices) hlog(1, "vk: no 8-bit indices%s: widened to 16 bits", u8x ? " (NX_NO_U8_INDICES)" : "");
        if (!vk.vi_dynamic) hlog(1, "vk: no dynamic vertex input%s: vertex layouts are part of the pipeline",
                                 vix ? " (NX_NO_VI_DYNAMIC)" : "");
    }

    /* subgroups of 32 like Maxwell's warps where the GPU can (cheapest for the translated ballots, lane masks and
     * shuffles). Before translate.py worked on 32-lane halves, AMD's default 64-wide waves lost lanes 32-63 and left
     * a compute loop spinning until the driver reset; GCN (64 only) now relies on the halves. NX_WARP64=1 asks for 64
     * instead (RDNA can: tests GCN's case) */
    VkPhysicalDeviceVulkan13Features has13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 hasf13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &has13};
    vkGetPhysicalDeviceFeatures2(vk.phys, &hasf13);
    VkPhysicalDeviceVulkan12Properties p12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES};
    VkPhysicalDeviceVulkan13Properties p13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES, &p12};
    VkPhysicalDeviceVulkan11Properties p11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, &p13};
    VkPhysicalDeviceProperties2 pp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p11};
    vkGetPhysicalDeviceProperties2(vk.phys, &pp);
    const char *w64 = getenv("NX_WARP64");
    u32 want = w64 && atoi(w64) ? 64 : 32;
    if (has13.subgroupSizeControl && p13.minSubgroupSize <= want && p13.maxSubgroupSize >= want &&
        (p11.subgroupSize != want || p13.minSubgroupSize != p13.maxSubgroupSize)) {
        vk.warp_stages = p13.requiredSubgroupSizeStages;
        vk_warp.requiredSubgroupSize = want;
    }
    hlog(1, "vk: subgroup size %u (%u-%u), %u required for stages %x%s", p11.subgroupSize, p13.minSubgroupSize,
         p13.maxSubgroupSize, vk_warp.requiredSubgroupSize, vk.warp_stages, want == 64 ? " (NX_WARP64)" : "");
    /* warp code runs on 32-lane halves of the subgroup: 32, 64 or 128 wide is right, narrower is not (Intel's 8 / 16) */
    u32 width = vk.warp_stages ? vk_warp.requiredSubgroupSize : p11.subgroupSize;
    if (width < 32 || width > 128 || (!vk.warp_stages && p13.minSubgroupSize < 32))
        hlog(0, "vk: subgroups of %u-%u lanes, not 32 or 64: warp-level shader code may be wrong", p13.minSubgroupSize,
             p13.maxSubgroupSize);
    /* translated shaders flush 32-bit denormals (Maxwell's .FTZ) and keep NaN and infinities (the console's NaN
     * results: .SAT, FMNMX, F2I), as execution modes in every SPIR-V (tools/shader/translate.py) */
    if (!p12.shaderDenormFlushToZeroFloat32 || !p12.shaderSignedZeroInfNanPreserveFloat32)
        hlog(0, "vk: no 32-bit float controls (flush to zero %u, NaN preserve %u): shaders may differ from the console",
             p12.shaderDenormFlushToZeroFloat32, p12.shaderSignedZeroInfNanPreserveFloat32);

    /* features: everything the NVN state maps to is dynamic state, so pipelines depend on shaders and formats */
    VkPhysicalDeviceTransformFeedbackFeaturesEXT xfb = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TRANSFORM_FEEDBACK_FEATURES_EXT, .transformFeedback = 1};
    VkPhysicalDeviceIndexTypeUint8FeaturesKHR u8i = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_KHR,
                                                     &xfb, .indexTypeUint8 = 1};
    VkPhysicalDeviceDepthClipControlFeaturesEXT clipc = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_CONTROL_FEATURES_EXT, vk.u8_indices ? (void *)&u8i : &xfb,
        .depthClipControl = 1};
    VkPhysicalDeviceDepthClipEnableFeaturesEXT clip = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT,
                                                       &clipc, .depthClipEnable = 1};
    /* bounds-checked buffer and image access (out of range: reads zero, writes dropped) where the device has it, so
     * a shader reading past what it was given does not take the device down; global memory (raw addresses) is not
     * covered */
    VkPhysicalDeviceRobustness2FeaturesEXT rob = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, &clip,
        .robustBufferAccess2 = hasrob.robustBufferAccess2, .robustImageAccess2 = hasrob.robustImageAccess2,
        .nullDescriptor = 1};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, &rob,
        .customBorderColors = 1, .customBorderColorWithoutFormat = 1};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vin = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT, &border,
        .vertexInputDynamicState = 1};
    VkPhysicalDeviceExtendedDynamicState2FeaturesEXT eds2 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT, vk.vi_dynamic || vi_keep ? (void *)&vin : &border,
        .extendedDynamicState2 = 1, .extendedDynamicState2LogicOp = vk.dyn_logic_op,
        .extendedDynamicState2PatchControlPoints = has2.extendedDynamicState2PatchControlPoints};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT eds3 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, &eds2,
        .extendedDynamicState3PolygonMode = 1, .extendedDynamicState3RasterizationSamples = 1,
        .extendedDynamicState3SampleMask = 1, .extendedDynamicState3AlphaToCoverageEnable = 1,
        .extendedDynamicState3LogicOpEnable = 1, .extendedDynamicState3ColorBlendEnable = 1,
        .extendedDynamicState3ColorBlendEquation = 1, .extendedDynamicState3ColorWriteMask = 1,
        .extendedDynamicState3DepthClampEnable = 1, .extendedDynamicState3DepthClipEnable = 1};
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &eds3,
        .dynamicRendering = 1, .synchronization2 = 1, .maintenance4 = 1, .shaderDemoteToHelperInvocation = 1,
        .subgroupSizeControl = vk.warp_stages != 0};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13,
        .timelineSemaphore = 1, .bufferDeviceAddress = 1, .descriptorIndexing = 1, .runtimeDescriptorArray = 1,
        .descriptorBindingPartiallyBound = 1, .descriptorBindingSampledImageUpdateAfterBind = 1,
        .descriptorBindingUpdateUnusedWhilePending = 1, .shaderSampledImageArrayNonUniformIndexing = 1,
        .descriptorBindingStorageImageUpdateAfterBind = 1, .shaderStorageImageArrayNonUniformIndexing = 1,
        .scalarBlockLayout = 1, .hostQueryReset = 1, .shaderFloat16 = 0, .samplerFilterMinmax = 1,
        .samplerMirrorClampToEdge = 1, .descriptorBindingVariableDescriptorCount = 1,
        .shaderOutputLayer = 1, .shaderOutputViewportIndex = 1,
        .descriptorBindingUniformBufferUpdateAfterBind = 0, .descriptorBindingStorageBufferUpdateAfterBind = 0};
    VkPhysicalDeviceVulkan11Features f11 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12,
        .shaderDrawParameters = 1};
    /* pipeline binaries (with maintenance5, which they need): compiled pipelines kept between sessions by gpu.c,
     * where the driver's own caches do not keep them (AMD); NX_PIPELINE_BINARY=0 turns them off */
    const char *exts[sizeof device_exts / sizeof *device_exts + 4];
    u32 next = 0;
    for (size_t i = 0; i < sizeof device_exts / sizeof *device_exts; i++) exts[next++] = device_exts[i];
    if (vk.u8_indices) exts[next++] = VK_KHR_INDEX_TYPE_UINT8_EXTENSION_NAME;
    if (vk.vi_dynamic || vi_keep) exts[next++] = VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME;
    VkPhysicalDevicePipelineBinaryFeaturesKHR hasbin = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR};
    VkPhysicalDeviceMaintenance5FeaturesKHR hasm5 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, &hasbin};
    VkPhysicalDeviceFeatures2 hasf5 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &hasm5};
    const char *pb = getenv("NX_PIPELINE_BINARY");
    if ((!pb || atoi(pb)) && has_ext(dext, ne, VK_KHR_PIPELINE_BINARY_EXTENSION_NAME) &&
        has_ext(dext, ne, VK_KHR_MAINTENANCE_5_EXTENSION_NAME)) {
        vkGetPhysicalDeviceFeatures2(vk.phys, &hasf5);
        vk.pipeline_binary = hasbin.pipelineBinaries && hasm5.maintenance5;
    }
    free(dext);
    VkPhysicalDevicePipelineBinaryFeaturesKHR fbin = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_FEATURES_KHR, &f11,
                                                      .pipelineBinaries = 1};
    VkPhysicalDeviceMaintenance5FeaturesKHR fm5 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, &fbin,
                                                   .maintenance5 = 1};
    if (vk.pipeline_binary) {
        exts[next++] = VK_KHR_MAINTENANCE_5_EXTENSION_NAME;
        exts[next++] = VK_KHR_PIPELINE_BINARY_EXTENSION_NAME;
        VkPhysicalDevicePipelineBinaryPropertiesKHR pbp = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_BINARY_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 pp2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &pbp};
        vkGetPhysicalDeviceProperties2(vk.phys, &pp2);
        hlog(1, "vk: pipeline binaries (driver: internal cache %u, prefers it %u, precompiled %u)",
             pbp.pipelineBinaryInternalCache, pbp.pipelineBinaryPrefersInternalCache,
             pbp.pipelineBinaryPrecompiledInternalCache);
    } else hlog(1, "vk: no pipeline binaries%s", pb && !atoi(pb) ? " (NX_PIPELINE_BINARY=0)" : "");
    VkPhysicalDeviceFeatures2 f = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, vk.pipeline_binary ? (void *)&fm5 : &f11};
    f.features = (VkPhysicalDeviceFeatures){
        .robustBufferAccess = 1, .imageCubeArray = 1, .independentBlend = 1, .geometryShader = 1, .tessellationShader = 1,
        .sampleRateShading = 1, .dualSrcBlend = 1, .logicOp = 1, .multiDrawIndirect = 1, .drawIndirectFirstInstance = 1,
        .depthClamp = 1, .depthBiasClamp = 1, .fillModeNonSolid = 1, .depthBounds = 1, .wideLines = 1,
        .largePoints = 1, .multiViewport = 1, .samplerAnisotropy = 1, .textureCompressionBC = 1,
        .occlusionQueryPrecise = 1, .vertexPipelineStoresAndAtomics = 1, .fragmentStoresAndAtomics = 1,
        .shaderImageGatherExtended = 1, .shaderStorageImageExtendedFormats = 1,
        .shaderStorageImageReadWithoutFormat = 1, .shaderStorageImageWriteWithoutFormat = 1,
        .shaderClipDistance = 1, .shaderCullDistance = 1,
        /* descriptor arrays indexed by a handle that is the same for the whole draw (translate.py: from c[2], no
         * nonuniformEXT) */
        .shaderSampledImageArrayDynamicIndexing = 1, .shaderStorageImageArrayDynamicIndexing = 1, .shaderFloat64 = 1, .shaderInt64 = 1, .shaderInt16 = 1};
    {
        /* each only where the device has it: asked for regardless, one it lacks failed vkCreateDevice with a bare
         * error code (Intel's GPUs have no shaderFloat64). What needs a missing one may draw wrong; the log names it */
        static const char *const names[] = {
            "robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray", "independentBlend", "geometryShader",
            "tessellationShader", "sampleRateShading", "dualSrcBlend", "logicOp", "multiDrawIndirect",
            "drawIndirectFirstInstance", "depthClamp", "depthBiasClamp", "fillModeNonSolid", "depthBounds", "wideLines",
            "largePoints", "alphaToOne", "multiViewport", "samplerAnisotropy", "textureCompressionETC2",
            "textureCompressionASTC_LDR", "textureCompressionBC", "occlusionQueryPrecise", "pipelineStatisticsQuery",
            "vertexPipelineStoresAndAtomics", "fragmentStoresAndAtomics", "shaderTessellationAndGeometryPointSize",
            "shaderImageGatherExtended", "shaderStorageImageExtendedFormats", "shaderStorageImageMultisample",
            "shaderStorageImageReadWithoutFormat", "shaderStorageImageWriteWithoutFormat",
            "shaderUniformBufferArrayDynamicIndexing", "shaderSampledImageArrayDynamicIndexing",
            "shaderStorageBufferArrayDynamicIndexing", "shaderStorageImageArrayDynamicIndexing", "shaderClipDistance",
            "shaderCullDistance", "shaderFloat64", "shaderInt64", "shaderInt16", "shaderResourceResidency",
            "shaderResourceMinLod", "sparseBinding", "sparseResidencyBuffer", "sparseResidencyImage2D",
            "sparseResidencyImage3D", "sparseResidency2Samples", "sparseResidency4Samples", "sparseResidency8Samples",
            "sparseResidency16Samples", "sparseResidencyAliased", "variableMultisampleRate", "inheritedQueries"};
        _Static_assert(sizeof names / sizeof *names * sizeof(VkBool32) == sizeof(VkPhysicalDeviceFeatures),
                       "one name per VkPhysicalDeviceFeatures member, in order");
        VkBool32 want[sizeof names / sizeof *names], has[sizeof names / sizeof *names];
        memcpy(want, &f.features, sizeof want);
        memcpy(has, &hasf.features, sizeof has);
        char miss[1024] = "";
        size_t ml = 0;
        for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
            if (!want[i] || has[i]) continue;
            want[i] = 0;
            if (ml < sizeof miss) ml += snprintf(miss + ml, sizeof miss - ml, " %s", names[i]);
        }
        memcpy(&f.features, want, sizeof want);
        if (miss[0]) hlog(0, "vk: this GPU lacks:%s (left off: what needs them may draw wrong)", miss);
        vk.features = f.features;
    }
    /* may a draw's topology be of another class (points, lines, triangles, patches) than its pipeline's? Where not,
     * the class goes into the pipeline key (gpu_draw.c; NX_TOPO_CLASS=1: as if not, for testing) */
    VkPhysicalDeviceExtendedDynamicState3PropertiesEXT eds3p = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 ppe = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &eds3p};
    vkGetPhysicalDeviceProperties2(vk.phys, &ppe);
    const char *tc = getenv("NX_TOPO_CLASS");
    vk.topo_class = !eds3p.dynamicPrimitiveTopologyUnrestricted || (tc && atoi(tc));
    if (vk.topo_class)
        hlog(1, "vk: a draw's topology class is its pipeline's%s: points and lines get pipelines of their own",
             eds3p.dynamicPrimitiveTopologyUnrestricted ? " (NX_TOPO_CLASS)" : "");
    float prio[2] = {1, 1};
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = vk.family,
                                   .queueCount = family_queues >= 2 ? 2 : 1, .pQueuePriorities = prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci,
                              .enabledExtensionCount = next, .ppEnabledExtensionNames = exts};
    VKCHECK(vkCreateDevice(vk.phys, &dci, NULL, &vk.dev));
#define VK_LOAD_D(f) if (!(f = (PFN_##f)vkGetDeviceProcAddr(vk.dev, #f))) fatal("Vulkan: no %s", #f);
    VK_DEVICE_FNS(VK_LOAD_D)
#define VK_LOAD_OPT(f) f = (PFN_##f)vkGetDeviceProcAddr(vk.dev, #f);
    VK_DEVICE_OPT_FNS(VK_LOAD_OPT)
    if (vk.vi_dynamic && !vkCmdSetVertexInputEXT) fatal("Vulkan: no vkCmdSetVertexInputEXT");
    if (vk.pipeline_binary && !(vkCreatePipelineBinariesKHR && vkDestroyPipelineBinaryKHR && vkGetPipelineBinaryDataKHR &&
                                vkReleaseCapturedPipelineDataKHR))
        vk.pipeline_binary = false;
    vkGetDeviceQueue(vk.dev, vk.family, 0, &vk.queue);
    vk.present_queue = vk.queue;
    if (qci.queueCount == 2) vkGetDeviceQueue(vk.dev, vk.family, 1, &vk.present_queue);
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
    VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
    VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;

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
    VkImageView views[MAX_IMAGES];          /* for mods that draw (mods.c), made with the swapchain */
    VkSemaphore done[MAX_IMAGES];           /* rendering into image i finished (waited on by the present) */
    VkSemaphore acquired[PRESENT_FRAMES];
    VkFence fence[PRESENT_FRAMES];
    VkCommandPool pool[PRESENT_FRAMES];
    VkCommandBuffer cmd[PRESENT_FRAMES];
    u32 frame;
} sw;

static volatile LONG unthrottled;

void vk_set_unthrottled(bool on) {
    if (InterlockedExchange(&unthrottled, on) != (LONG)on) InterlockedExchange(&resized, 1);   /* a new swapchain */
}

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
    /* uncapped or above 60 fps (vk_set_unthrottled): not held to the display's refresh; IMMEDIATE tears, so only
     * then and only without MAILBOX */
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    bool mailbox = false;
    for (u32 i = 0; i < nm; i++) if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) mode = modes[i], mailbox = true;
    for (u32 i = 0; unthrottled && !mailbox && i < nm; i++) if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = modes[i];
    static const char *const mode_names[] = {"immediate", "mailbox", "FIFO", "relaxed FIFO"};
    hlog(1, "vk: present mode %s", mode < 4 ? mode_names[mode] : "other");
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
    for (u32 i = 0; i < MAX_IMAGES; i++)                /* the device is idle here (present_locked) */
        if (sw.views[i]) { vkDestroyImageView(vk.dev, sw.views[i], NULL); sw.views[i] = NULL; }
    sw.count = MAX_IMAGES;
    VKCHECK(vkGetSwapchainImagesKHR(vk.dev, sw.sc, &sw.count, sw.images));
    VkSemaphoreCreateInfo si = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (u32 i = 0; i < sw.count; i++) if (!sw.done[i]) VKCHECK(vkCreateSemaphore(vk.dev, &si, NULL, &sw.done[i]));
    hlog(1, "vk: swapchain %ux%u, %u images, present mode %d", ext.width, ext.height, sw.count, mode);
}

/* The present itself runs on a thread of its own, on the family's second queue where there is one (NVIDIA has 16;
 * AMD's graphics family has one, which the present thread then shares under queue_lock): on a laptop whose display
 * hangs off the integrated GPU, NVIDIA's vkQueuePresentKHR held the render thread ~4 ms a frame (it waits for the
 * frame and copies it across), time it now spends on the next frame. sw_lock covers the swapchain (acquire,
 * present, recreation) and the present queue, which Vulkan wants used by one thread at a time. Lock order: sw_lock,
 * then queue_lock */
static SRWLOCK sw_lock = SRWLOCK_INIT;
static SRWLOCK queue_lock = SRWLOCK_INIT;

/* mods' own submissions to vk.queue (nxmod.h NxModFrame) */
void vk_queue_lock(void) { AcquireSRWLockExclusive(&queue_lock); }
void vk_queue_unlock(void) { ReleaseSRWLockExclusive(&queue_lock); }

VkResult vk_submit(const VkSubmitInfo2 *si, VkFence fence) {
    AcquireSRWLockExclusive(&queue_lock);
    VkResult r = vkQueueSubmit2(vk.queue, 1, si, fence);
    ReleaseSRWLockExclusive(&queue_lock);
    return r;
}
static struct { HANDLE go; u32 idx[MAX_IMAGES]; u32 head, tail; } pq;   /* images to present, under sw_lock */
static u64 present_dropped;             /* frames not shown (uncapped: the display was still busy) */
u64 vk_presents_dropped(void) { return present_dropped; }

static void present_now(u32 idx) {
    VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &sw.done[idx],
                           .swapchainCount = 1, .pSwapchains = &sw.sc, .pImageIndices = &idx};
    bool shared = vk.present_queue == vk.queue;
    if (shared) AcquireSRWLockExclusive(&queue_lock);
    VkResult r = vkQueuePresentKHR(vk.present_queue, &pi);
    if (shared) ReleaseSRWLockExclusive(&queue_lock);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) InterlockedExchange(&resized, 1);
    else if (r < 0) fatal("vkQueuePresentKHR: %d", r);
}

static void present_pending(void) {
    for (; pq.tail != pq.head; pq.tail++) present_now(pq.idx[pq.tail % MAX_IMAGES]);
}

/* every queue idle, the queued presents done (gpu.c: before images still in flight are remade) */
void vk_wait_idle(void) {
    AcquireSRWLockExclusive(&sw_lock);
    present_pending();
    AcquireSRWLockExclusive(&queue_lock);
    vkDeviceWaitIdle(vk.dev);
    ReleaseSRWLockExclusive(&queue_lock);
    ReleaseSRWLockExclusive(&sw_lock);
}

static unsigned __stdcall present_thread(void *arg) {
    (void)arg;
    for (;;) {
        WaitForSingleObject(pq.go, INFINITE);
        AcquireSRWLockExclusive(&sw_lock);
        present_pending();
        ReleaseSRWLockExclusive(&sw_lock);
    }
}

static void present_init(void) {
    pq.go = CreateEventW(NULL, FALSE, FALSE, NULL);
    _beginthreadex(NULL, 0, present_thread, NULL, 0, NULL);
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

/* ---------------------------------------------------------------------------------------------- fps overlay */
/* Frame times between presents; average fps, 5% and 1% lows (the mean of the slowest 5% / 1% of frame times)
 * over the last OV_FRAMES frames, drawn with a 5x7 font into a host buffer and copied onto the swapchain
 * image. On unless NX_FPS=0; F3 toggles it. */
#define OV_FRAMES 1024
#define OV_W 200
#define OV_H 134
#define OV_LINES 7
#define OV_SCALE 2
static volatile LONG overlay_on = -1;
static struct {
    double times[OV_FRAMES];                /* ms */
    double steps[OV_FRAMES];                /* the game time each frame stood for, ms */
    u32 n, next;
    LARGE_INTEGER last, freq, shown;
    char lines[OV_LINES][24];
    VkBuf buf[PRESENT_FRAMES];
} ov;

static const struct { char c; u8 rows[7]; } glyphs[] = {
    {'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}}, {'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
    {'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}}, {'3', {0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e}},
    {'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}}, {'5', {0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e}},
    {'6', {0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e}}, {'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}}, {'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c}},
    {'.', {0, 0, 0, 0, 0, 0x0c, 0x0c}}, {'%', {0x18, 0x19, 0x02, 0x04, 0x08, 0x13, 0x03}},
    {'A', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}}, {'F', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x10}},
    {'G', {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}}, {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}},
    {'M', {0x11, 0x1b, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'O', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {'P', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}}, {'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}}, {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}},
    {'X', {0x11, 0x11, 0x0a, 0x04, 0x0a, 0x11, 0x11}}, {'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'I', {0x0e, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0e}}, {'C', {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'H', {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
    {'Z', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f}}, {'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}},
    {'D', {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}}, {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}}, {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
    {':', {0, 0x0c, 0x0c, 0, 0x0c, 0x0c, 0}}, {'/', {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}},
    {'B', {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}}, {'J', {0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0c}},
    {'Q', {0x0e, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0d}}, {'Y', {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}},
    {'-', {0, 0, 0, 0x1f, 0, 0, 0}}, {'<', {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02}},
    {'>', {0x08, 0x04, 0x02, 0x01, 0x02, 0x04, 0x08}}, {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}},
    {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}}, {'=', {0, 0, 0x1f, 0, 0x1f, 0, 0}},
};

/* text in the 5x7 font, `scale` pixels a dot, into a w x h pixel buffer (options.c's panel too) */
void ov_text(u32 *px, int w, int h, int x, int y, const char *s, u32 color, int scale) {
    for (; *s; s++, x += 6 * scale) {
        const u8 *rows = NULL;
        for (size_t g = 0; g < sizeof glyphs / sizeof *glyphs; g++)
            if (glyphs[g].c == *s) rows = glyphs[g].rows;
        if (!rows || x < 0 || y < 0 || x + 5 * scale > w || y + 7 * scale > h) continue;
        for (int r = 0; r < 7 * scale; r++)
            for (int c = 0; c < 5 * scale; c++)
                if (rows[r / scale] >> (4 - c / scale) & 1) px[(y + r) * w + x + c] = color;
    }
}

void overlay_toggle(void) {
    InterlockedExchange(&overlay_on, overlay_on ? 0 : 1);
}

void overlay_set(bool on) { InterlockedExchange(&overlay_on, on); }
void vk_swap_extent(u32 *w, u32 *h) { *w = sw.extent.width; *h = sw.extent.height; }
bool overlay_shown(void) { return overlay_on > 0; }

static int cmp_desc(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

static void overlay_stats(void) {
    static double sorted[OV_FRAMES];
    u32 n = ov.n;
    double sum = 0;
    for (u32 i = 0; i < n; i++) sum += sorted[i] = ov.times[i];
    qsort(sorted, n, sizeof *sorted, cmp_desc);
    u32 n5 = n / 20 ? n / 20 : 1, n1 = n / 100 ? n / 100 : 1;
    double s5 = 0, s1 = 0;
    for (u32 i = 0; i < n5; i++) s5 += sorted[i];
    for (u32 i = 0; i < n1; i++) s1 += sorted[i];
    double last = ov.times[(ov.next + OV_FRAMES - 1) % OV_FRAMES];
    snprintf(ov.lines[0], sizeof ov.lines[0], "FPS %.1f", 1000.0 * n / sum);
    snprintf(ov.lines[1], sizeof ov.lines[1], "5%% LOW %.1f", 1000.0 * n5 / s5);
    snprintf(ov.lines[2], sizeof ov.lines[2], "1%% LOW %.1f", 1000.0 * n1 / s1);
    snprintf(ov.lines[3], sizeof ov.lines[3], "MS %.1f MAX %.0f", last, sorted[0]);
    /* the frame rate set, the game's tick rate (game time a second in 30 Hz ticks, the game's own rate: each frame
     * steps the game by the time it stands for, so 30 at normal speed whatever the frame rate) and its speed */
    s32 r = nx_framerate();
    if (r == FRAMERATE_GAME) snprintf(ov.lines[4], sizeof ov.lines[4], "CAP GAME");
    else if (r == FRAMERATE_UNCAPPED) snprintf(ov.lines[4], sizeof ov.lines[4], "CAP NONE");
    else snprintf(ov.lines[4], sizeof ov.lines[4], "CAP %d", r);
    double steps = 0;
    for (u32 i = 0; i < n; i++) steps += ov.steps[i];
    snprintf(ov.lines[5], sizeof ov.lines[5], "TICK %.1f HZ", 30.0 * steps / sum);
    snprintf(ov.lines[6], sizeof ov.lines[6], "SPEED %.0f%%", 100.0 * steps / sum);
}

/* draw the overlay for present frame f; false when it is off or the swapchain format is not 8-bit BGRA/RGBA */
static bool overlay_draw(u32 f) {
    if (overlay_on < 0) {
        const char *e = getenv("NX_FPS");
#ifdef NX_PACKAGE
        InterlockedExchange(&overlay_on, e && *e && *e != '0');          /* off unless NX_FPS=1; F3 shows it */
#else
        InterlockedExchange(&overlay_on, !(e && *e == '0'));
#endif
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!ov.freq.QuadPart) {
        QueryPerformanceFrequency(&ov.freq);
        ov.last = ov.shown = now;
        return false;
    }
    ov.times[ov.next] = 1000.0 * (double)(now.QuadPart - ov.last.QuadPart) / (double)ov.freq.QuadPart;
    ov.steps[ov.next] = nx_frame_step_ns() / 1e6;
    ov.next = (ov.next + 1) % OV_FRAMES;
    if (ov.n < OV_FRAMES) ov.n++;
    ov.last = now;
    if (!overlay_on) return false;
    bool bgra = sw.format == VK_FORMAT_B8G8R8A8_UNORM || sw.format == VK_FORMAT_B8G8R8A8_SRGB;
    if (!bgra && sw.format != VK_FORMAT_R8G8B8A8_UNORM && sw.format != VK_FORMAT_R8G8B8A8_SRGB) return false;
    if (sw.extent.width < OV_W + 8 || sw.extent.height < OV_H + 8) return false;
    if (!ov.lines[0][0] || now.QuadPart - ov.shown.QuadPart > ov.freq.QuadPart / 2) {
        overlay_stats();
        ov.shown = now;
    }
    if (!ov.buf[f].buf)
        vk_buffer(OV_W * OV_H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &ov.buf[f]);
    u32 *px = (u32 *)ov.buf[f].a.map;
    if (!px) return false;
    u32 bg = 0xff181818, fg = 0xff40ff40;      /* green on dark grey, the same in BGRA and RGBA */
    for (u32 i = 0; i < OV_W * OV_H; i++) px[i] = bg;
    for (int l = 0; l < OV_LINES; l++) {
        int x0 = 8, y0 = 6 + l * 18;
        for (const char *s = ov.lines[l]; *s; s++, x0 += 6 * OV_SCALE) {
            const u8 *rows = NULL;
            for (size_t g = 0; g < sizeof glyphs / sizeof *glyphs; g++)
                if (glyphs[g].c == *s) rows = glyphs[g].rows;
            if (!rows || x0 + 5 * OV_SCALE > OV_W) continue;
            for (int r = 0; r < 7 * OV_SCALE; r++)
                for (int c = 0; c < 5 * OV_SCALE; c++)
                    if (rows[r / OV_SCALE] >> (4 - c / OV_SCALE) & 1) px[(y0 + r) * OV_W + x0 + c] = fg;
        }
    }
    return true;
}

static void present_locked(VkImage src, VkFormat src_fmt, u32 src_w, u32 src_h, s32 x, s32 y, s32 w, s32 h,
                           bool flip_y, VkSemaphore wait, u64 wait_value);

/* "COMPILING SHADERS <n>" in the bottom-left corner while the translator has shaders to do (whatever F3 says): the
 * first start draws little until thousands are done. F12's "CAPTURING FRAME..." / "FRAME CAPTURED" go there too
 * (the glyphs have no B, J, Q or Y). */
#define MSG_W 360
#define MSG_H 22
static VkBuf msg_buf[PRESENT_FRAMES];
bool gpu_shader_progress(u32 *done, u32 *total);
const char *gpu_capture_status(void);

static bool msg_draw(u32 f) {
    u32 done, total;
    char text[40];
    const char *cap = gpu_capture_status();     /* F12: shown over the shader count while it lasts */
    if (cap) snprintf(text, sizeof text, "%s", cap);
    else if (gpu_shader_progress(&done, &total)) snprintf(text, sizeof text, "COMPILING SHADERS: %u/%u", done, total);
    else return false;
    if (sw.extent.width < MSG_W + 8 || sw.extent.height < MSG_H + 8) return false;
    bool bgra = sw.format == VK_FORMAT_B8G8R8A8_UNORM || sw.format == VK_FORMAT_B8G8R8A8_SRGB;
    if (!bgra && sw.format != VK_FORMAT_R8G8B8A8_UNORM && sw.format != VK_FORMAT_R8G8B8A8_SRGB) return false;
    if (!msg_buf[f].buf)
        vk_buffer(MSG_W * MSG_H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &msg_buf[f]);
    u32 *px = (u32 *)msg_buf[f].a.map;
    if (!px) return false;
    for (u32 i = 0; i < MSG_W * MSG_H; i++) px[i] = 0xff181818;
    int x0 = 6;
    for (const char *s = text; *s; s++, x0 += 6 * OV_SCALE) {
        const u8 *rows = NULL;
        for (size_t g = 0; g < sizeof glyphs / sizeof *glyphs; g++)
            if (glyphs[g].c == *s) rows = glyphs[g].rows;
        if (!rows || x0 + 5 * OV_SCALE > MSG_W) continue;
        for (int r = 0; r < 7 * OV_SCALE; r++)
            for (int c = 0; c < 5 * OV_SCALE; c++)
                if (rows[r / OV_SCALE] >> (4 - c / OV_SCALE) & 1) px[(4 + r) * MSG_W + x0 + c] = 0xffffffff;
    }
    return true;
}

VkPresentTimes vk_present_times;

static double pt_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return 1000.0 * (double)c.QuadPart / (double)f.QuadPart;
}

void vk_present(VkImage src, VkFormat src_fmt, u32 src_w, u32 src_h, s32 x, s32 y, s32 w, s32 h, bool flip_y,
                VkSemaphore wait, u64 wait_value) {
    if (!sw.fence[0]) present_init();
    /* uncapped, with a present thread: a frame that comes while the last one is still being presented is not shown
     * (as MAILBOX would replace it; under Optimus even MAILBOX waits for the display) */
    double t0 = pt_ms();
    if (unthrottled) {
        if (!TryAcquireSRWLockExclusive(&sw_lock)) { present_dropped++; return; }
        if (pq.tail != pq.head) { ReleaseSRWLockExclusive(&sw_lock); present_dropped++; return; }
    } else AcquireSRWLockExclusive(&sw_lock);
    double t1 = pt_ms();
    vk_present_times.lock += t1 - t0;
    present_pending();                  /* the last frame's, if its thread has not got to it */
    vk_present_times.present += pt_ms() - t1;
    present_locked(src, src_fmt, src_w, src_h, x, y, w, h, flip_y, wait, wait_value);
    bool queued = pq.tail != pq.head;
    ReleaseSRWLockExclusive(&sw_lock);
    if (queued) SetEvent(pq.go);
}

/* Shrinking the frame into the window (a render scale above the window's size): drawn by a built-in pass
 * (shaders/downscale.*) that averages each window pixel's whole footprint, where a linear blit reads only 2 x 2
 * source texels per pixel and skips the rest (aliasing back what the larger render took away) */
#include "shaders/downscale_spv.h"
static struct {
    VkFormat fmt;                       /* the swapchain format the pipeline was made for */
    VkPipeline pipe;
    VkPipelineLayout layout;
    VkDescriptorSetLayout set;
    VkSampler smp;
    VkImageView src[PRESENT_FRAMES];    /* each frame slot's view of its source (remade every frame) */
} ds;

static bool downscale_init(void) {
    if (ds.pipe && ds.fmt == sw.format) return true;
    if (!ds.layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo sci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 1, .pBindings = &b};
        VKCHECK(vkCreateDescriptorSetLayout(vk.dev, &sci, NULL, &ds.set));
        VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 28};
        VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                          .pSetLayouts = &ds.set, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
        VKCHECK(vkCreatePipelineLayout(vk.dev, &lci, NULL, &ds.layout));
        VkSamplerCreateInfo smci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
            .minFilter = VK_FILTER_LINEAR, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
        VKCHECK(vkCreateSampler(vk.dev, &smci, NULL, &ds.smp));
    }
    if (ds.pipe) { vkDestroyPipeline(vk.dev, ds.pipe, NULL); ds.pipe = NULL; }    /* (the device is idle: a new swapchain) */
    VkShaderModule m[2];
    VkShaderModuleCreateInfo mci[2] = {
        {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof downscale_vs, .pCode = downscale_vs},
        {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof downscale_fs, .pCode = downscale_fs}};
    VKCHECK(vkCreateShaderModule(vk.dev, &mci[0], NULL, &m[0]));
    VKCHECK(vkCreateShaderModule(vk.dev, &mci[1], NULL, &m[1]));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = m[0], .pName = "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = m[1], .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState att = {.colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = 1, .pAttachments = &att};
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
                                           .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = 1,
                                        .pColorAttachmentFormats = &sw.format};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, .stageCount = 2,
        .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
        .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &dy,
        .layout = ds.layout};
    VkResult r = vkCreateGraphicsPipelines(vk.dev, NULL, 1, &ci, NULL, &ds.pipe);
    vkDestroyShaderModule(vk.dev, m[0], NULL);
    vkDestroyShaderModule(vk.dev, m[1], NULL);
    if (r != VK_SUCCESS) { hlog(0, "vk: downscale pipeline failed (%d): blitting instead", r); ds.pipe = NULL; return false; }
    ds.fmt = sw.format;
    hlog(1, "vk: frames larger than the window are shrunk by the downscale pass");
    return true;
}

/* src (sw x sh texels, format fmt; the rectangle x, y, w, h of it) into the window rectangle dx, dy, dw, dh of
 * swapchain image idx (TRANSFER_DST before and after); false: not done (blit instead) */
static bool downscale(VkCommandBuffer cmd, u32 f, u32 idx, VkImage src, VkFormat fmt, u32 sw_, u32 sh_, s32 x, s32 y,
                      s32 w, s32 h, bool flip_y, s32 dx, s32 dy, u32 dw, u32 dh) {
    if (!fmt || !sw_ || !sh_ || !downscale_init()) return false;
    VkImage dst = sw.images[idx];
    if (!sw.views[idx]) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = dst, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = sw.format, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VKCHECK(vkCreateImageView(vk.dev, &vi, NULL, &sw.views[idx]));
    }
    if (ds.src[f]) { vkDestroyImageView(vk.dev, ds.src[f], NULL); ds.src[f] = NULL; }    /* (slot f's fence waited) */
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = src, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = fmt, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageViewUsageCreateInfo uci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    vi.pNext = &uci;
    if (vkCreateImageView(vk.dev, &vi, NULL, &ds.src[f]) != VK_SUCCESS) { ds.src[f] = NULL; return false; }
    float u0 = (float)x / sw_, u1 = (float)(x + w) / sw_;
    float v0 = (float)(flip_y ? y + h : y) / sh_, v1 = (float)(flip_y ? y : y + h) / sh_;
    float rx = (float)w / dw, ry = (float)h / dh, r = rx > ry ? rx : ry;
    int n = (int)r + (r > (float)(int)r);      /* taps per axis: at most a source texel apart */
    if (n < 1) n = 1;
    if (n > 8) n = 8;
    struct { float src[4], step[2]; s32 n; } pc = {{u0, v0, u1, v1}, {(u1 - u0) / dw, (v1 - v0) / dh}, n};
    image_barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = sw.views[idx],
        .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRect2D area = {{dx, dy}, {dw, dh}};
    VkRenderingInfo rinfo = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = area, .layerCount = 1,
                             .colorAttachmentCount = 1, .pColorAttachments = &ca};
    vkCmdBeginRendering(cmd, &rinfo);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ds.pipe);
    VkViewport vpt = {(float)dx, (float)dy, (float)dw, (float)dh, 0, 1};
    vkCmdSetViewportWithCount(cmd, 1, &vpt);
    vkCmdSetScissorWithCount(cmd, 1, &area);
    VkDescriptorImageInfo ii = {ds.smp, ds.src[f], VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wd = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, ds.layout, 0, 1, &wd);
    vkCmdPushConstants(cmd, ds.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    image_barrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    return true;
}

static void present_locked(VkImage src, VkFormat src_fmt, u32 src_w, u32 src_h, s32 x, s32 y, s32 w, s32 h,
                           bool flip_y, VkSemaphore wait, u64 wait_value) {
    if (InterlockedExchange(&resized, 0) || !sw.sc) {
        AcquireSRWLockExclusive(&queue_lock);     /* every queue: the render thread's too */
        vkDeviceWaitIdle(vk.dev);
        ReleaseSRWLockExclusive(&queue_lock);
        swapchain_create();
    }
    if (!sw.sc || !sw.extent.width) return;
    u32 f = sw.frame++ % PRESENT_FRAMES;
    double t0 = pt_ms();
    VKCHECK(vkWaitForFences(vk.dev, 1, &sw.fence[f], 1, UINT64_MAX));
    double t1 = pt_ms();
    vk_present_times.fence += t1 - t0;
    u32 idx;
    VkResult r = vkAcquireNextImageKHR(vk.dev, sw.sc, UINT64_MAX, sw.acquired[f], NULL, &idx);
    vk_present_times.acquire += pt_ms() - t1;
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { InterlockedExchange(&resized, 1); return; }
    if (r < 0) fatal("vkAcquireNextImageKHR: %d", r);
    double t_rec = pt_ms(), t_ui = 0;   /* recording: all of it, the menu and mods' windows, mods' raw draws */
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
        bool shrink = (u32)w > dw || (u32)h > dh;
        if (!(shrink && downscale(cmd, f, idx, src, src_fmt, src_w, src_h, x, y, w, h, flip_y, dx, dy, dw, dh))) {
        VkImageBlit blit = {
            .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .srcOffsets = {{x, flip_y ? y + h : y, 0}, {x + w, flip_y ? y : y + h, 1}},
            .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .dstOffsets = {{dx, dy, 0}, {dx + (s32)dw, dy + (s32)dh, 1}}};
        vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_GENERAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, VK_FILTER_LINEAR);
        }
    }
    if (overlay_draw(f)) {
        image_barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageOffset = {8, 8, 0}, .imageExtent = {OV_W, OV_H, 1}};
        vkCmdCopyBufferToImage(cmd, ov.buf[f].buf, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    }
    if (msg_draw(f)) {
        image_barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkBufferImageCopy bc = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageOffset = {8, (s32)sw.extent.height - MSG_H - 8, 0}, .imageExtent = {MSG_W, MSG_H, 1}};
        vkCmdCopyBufferToImage(cmd, msg_buf[f].buf, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bc);
    }
    bool draw_ui = ui_active(), draw_mods = mods_draw_any();
    if (!sw.views[idx] && (draw_ui || draw_mods)) {     /* made on first use */
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = dst,
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = sw.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VKCHECK(vkCreateImageView(vk.dev, &vi, NULL, &sw.views[idx]));
    }
    if (sw.views[idx] && (draw_ui || draw_mods)) {      /* the menu and mods' windows (ui.c), then mods' raw draws */
        image_barrier(cmd, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        double u0 = pt_ms();
        if (draw_ui) ui_draw(cmd, dst, sw.views[idx], sw.format, sw.extent.width, sw.extent.height, sw.count);
        double u1 = pt_ms();
        if (draw_mods) mods_draw(cmd, dst, sw.views[idx], sw.format, sw.extent.width, sw.extent.height, sw.count);
        vk_present_times.ui += u1 - u0;
        vk_present_times.mods += pt_ms() - u1;
        t_ui = pt_ms() - u0;
        image_barrier(cmd, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    } else
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
    double t2 = pt_ms();
    vk_present_times.record += t2 - t_rec - t_ui;
    VKCHECK(vk_submit(&si, sw.fence[f]));
    vk_present_times.submit += pt_ms() - t2;
    pq.idx[pq.head++ % MAX_IMAGES] = idx;
}
