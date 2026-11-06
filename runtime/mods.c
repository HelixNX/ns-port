/* Mods (nxmod.h): DLLs in the mods\ folder next to the executable, loaded on the window thread before its first
 * event (NX_MODS=0: none). Each gets the window's events after the options panel (options.c), can hold the game's
 * input while it shows something, and draws over the swapchain image at the end of each present (vk.c). */
#include "mods.h"
#include "game_config.h"
#include "hos.h"
#include <SDL3/SDL_vulkan.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define MAX_MODS 16
static const NxMod *mods[MAX_MODS];
static u32 nmods;
static NxModHost host;
static char title_id[17];

void input_release_mouse(SDL_WindowID id);
void vk_queue_lock(void);
void vk_queue_unlock(void);

static void mod_log(int level, const char *fmt, ...) {
    char s[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    hlog(level, "%s", s);
}

static void release_mouse(void) {
    if (host.window) input_release_mouse(SDL_GetWindowID(host.window));
}

static NxScriptRunner script_runner;
void mods_set_script_runner(NxScriptRunner r) { script_runner = r; }
static bool run_script(const char *text, NxScriptDone done, void *ud) {
    return script_runner && script_runner(text, done, ud);
}

void mods_load(SDL_Window *window) {
    const char *e = getenv("NX_MODS");
    if (e && *e == '0') return;
    snprintf(title_id, sizeof title_id, "%016llX", (unsigned long long)NX_TITLE_ID);
    host = (NxModHost){NXMOD_VERSION, title_id, NX_TITLE, nx_cache_dir(), window, mod_log, release_mouse, diag_watch, run_script};
    char dir[MAX_PATH], pat[MAX_PATH + 16];
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    char *s = strrchr(dir, '\\');
    if (s) *s = 0;
    snprintf(pat, sizeof pat, "%s\\mods\\*.dll", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char path[MAX_PATH * 2];
        snprintf(path, sizeof path, "%s\\mods\\%s", dir, fd.cFileName);
        HMODULE m = LoadLibraryExA(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m) { hlog(0, "mods: %s does not load (%lu)", fd.cFileName, GetLastError()); continue; }
        NxModInit init = (NxModInit)(void *)GetProcAddress(m, "nx_mod_init");
        const NxMod *mod = init ? init(&host) : NULL;
        if (!mod || nmods == MAX_MODS) {
            hlog(0, "mods: %s %s", fd.cFileName, init ? "declined" : "has no nx_mod_init");
            FreeLibrary(m);
            continue;
        }
        mods[nmods++] = mod;
        hlog(0, "mods: %s (%s)", mod->name ? mod->name : "?", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

bool mods_event(const SDL_Event *e) {
    for (u32 i = 0; i < nmods; i++)
        if (mods[i]->event && mods[i]->event(e)) return true;
    return false;
}

bool mods_want_input(void) {
    for (u32 i = 0; i < nmods; i++)
        if (mods[i]->wants_input && mods[i]->wants_input()) return true;
    return false;
}

bool mods_draw_any(void) {
    for (u32 i = 0; i < nmods; i++)
        if (mods[i]->draw) return true;
    return false;
}

void mods_draw(VkCommandBuffer cmd, VkImage image, VkImageView view, VkFormat format, u32 w, u32 h, u32 count) {
    NxModFrame f = {cmd, image, view, format, w, h, count, (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr(),
                    vk.instance, vk.phys, vk.dev, vk.family, vk.queue, vk_queue_lock, vk_queue_unlock};
    for (u32 i = 0; i < nmods; i++)
        if (mods[i]->draw) mods[i]->draw(&f);
}
