/* nxmod.h: what the runtime gives mods, and what a mod gives back.
 *
 * A mod is a DLL in the mods\ folder next to the executable (NX_MODS=0: none loaded). The runtime loads each at
 * start-up and calls its export
 *     const NxMod *nx_mod_init(const NxModHost *host);
 * (NULL: the mod declines, e.g. another game). Mods see the guest's memory directly at its own addresses (the
 * modules at game.json's bases, the heap at NXMOD_HEAP_BASE), as the lifted code does.
 *
 * Mods are optional: each is its own DLL, so leaving one out of mods\ leaves it out (the menu's Mods > Manage
 * also turns one off for the next start, settings.ini mods.off).
 *
 * Version 4: the runtime has one Dear ImGui (its menu bar and Options) and mods draw into it. A mod registers its
 * windows with host->add_window (listed under the Mods menu, each with a hotkey the player can rebind) and draws
 * them in ui(), on the render thread inside the runtime's ImGui frame, with host->imgui_context made current
 * (modkit::init does that; the mod is built against the same ImGui, host->imgui_version). The runtime frees the
 * mouse when one of a mod's windows opens and keeps the game's keyboard while ImGui has keyboard focus.
 * Version 3 mods (their own ImGui, draw() into the swapchain image) are not loaded any more.
 *
 * Threads: event() runs on the window thread (SDL's main thread), ui() and draw() on the render thread inside the
 * present; a mod synchronises its own state between them. */
#ifndef NXMOD_H
#define NXMOD_H
#include <stdbool.h>
#include <stdint.h>
#include <vulkan/vulkan.h>                  /* with or without VK_NO_PROTOTYPES (the runtime's vk.h has it) */
#include <SDL3/SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NXMOD_VERSION 4
#define NXMOD_HEAP_BASE 0x2000000000ull     /* hos.h HEAP_BASE: svcSetHeapSize's heap */
#define NXMOD_HEAP_MAX  0x0180000000ull     /* the most it can span (hos.h HEAP_MAX) */
#define NXMOD_ALIAS_BASE 0x1000000000ull    /* hos.h ALIAS_BASE: svcMapPhysicalMemory's region, where games with */
#define NXMOD_ALIAS_SIZE 0x1000000000ull    /* their own allocator (Violet's 12 GB one) keep most of their data */

/* run_script's result: the chunk's first result as text (ok), or the error message; on the game's thread */
#define NXMOD_SCRIPT_AGAIN "again"           /* a chunk returning this is run again at a later safe point */
typedef void (*NxScriptDone)(void *ud, bool ok, const char *result);

typedef struct NxModHost {
    uint32_t version;                       /* NXMOD_VERSION */
    const char *title_id;                   /* "01008F6008C5E000" */
    const char *title;                      /* game.json's title */
    const char *cache_dir;                  /* the build folder, or %LOCALAPPDATA%\<title> in a package */
    SDL_Window *window;
    void (*log)(int level, const char *fmt, ...);   /* log.txt (level 0 always, 1 with --log 1) */
    void (*release_mouse)(void);            /* give the mouse back from the game's camera (window thread) */
    /* debugging (version 2): log every write to [addr, addr + len) from now on (NX_WATCH_MEM's report: the writer's
     * guest instruction and host stack, i.e. the lifted f_<addr> chain; the first 200 writes). One range at a time */
    void (*watch_mem)(uint64_t addr, uint64_t len);
    /* version 3: run a chunk of source in the game's own script VM (Violet: Lua 5.4), at the next safe point on a
     * thread that runs scripts; `done` (may be NULL) gets what it returned. The text is copied. false: the game's
     * host code has no script runner. Results and errors also go to the log (level 1 / 0) */
    bool (*run_script)(const char *text, NxScriptDone done, void *ud);
    /* version 4: the runtime's Dear ImGui. A mod makes the context current with its allocators (modkit::init):
     *   ImGui::SetAllocatorFunctions(host->imgui_alloc, host->imgui_free, host->imgui_alloc_ud);
     *   ImGui::SetCurrentContext((ImGuiContext *)host->imgui_context);
     * and declines when imgui_version is not its own IMGUI_VERSION_NUM */
    int imgui_version;
    void *imgui_context;
    void *(*imgui_alloc)(size_t size, void *ud);
    void (*imgui_free)(void *p, void *ud);
    void *imgui_alloc_ud;
    /* a window of the mod's, listed under Mods: is_open() for the check mark, toggle() from the menu or the hotkey
     * (default_key: an SDL key name such as "F6", or NULL for none; the player can rebind it). Call from
     * nx_mod_init. false: too many windows */
    bool (*add_window)(const char *title, const char *default_key, bool (*is_open)(void), void (*toggle)(void));
} NxModHost;

/* draw()'s frame: the swapchain image in COLOR_ATTACHMENT_OPTIMAL (left so), `cmd` recording. Submissions of the
 * mod's own (uploads) go to `queue` between queue_lock() and queue_unlock(): the runtime's threads share it. */
typedef struct NxModFrame {
    VkCommandBuffer cmd;
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t width, height;
    uint32_t image_count;                   /* swapchain images */
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t family;
    VkQueue queue;
    void (*queue_lock)(void);
    void (*queue_unlock)(void);
} NxModFrame;

typedef struct NxMod {
    const char *name;
    bool (*event)(const SDL_Event *e);      /* window thread; true: used (the game does not see it). May be NULL */
    bool (*wants_input)(void);              /* while true the game gets no keyboard, mouse or pad input. May be NULL */
    void (*draw)(const NxModFrame *f);      /* render thread, once a presented frame, after the menu. May be NULL */
    /* version 4 */
    const char *version;                    /* shown in Mods > Manage */
    const char *description;
    void (*ui)(void);                       /* render thread, inside the runtime's ImGui frame (while the menu shows) */
} NxMod;

typedef const NxMod *(*NxModInit)(const NxModHost *host);

#ifdef __cplusplus
}
#endif
#endif
