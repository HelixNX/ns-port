/* Mods (nxmod.h): DLLs in the mods\ folder next to the executable, each optional, loaded on the window thread
 * before its first event (NX_MODS=0: none; settings.ini mods.off=<file>;<file> skips those). Each mod's windows are
 * registered here (Mods menu, a rebindable hotkey); ui.cpp draws the menu and calls each mod's ui() inside its
 * ImGui frame; draw() (raw Vulkan) comes after, over everything (vk.c). */
#include "mods.h"
#include "game_config.h"
#include "hos.h"
#include "options.h"
#include <SDL3/SDL_vulkan.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define MAX_MODS 16
#define MAX_FILES 32
#define MAX_WINDOWS 32
static const NxMod *mods[MAX_MODS];
static u32 nmods;
static NxModHost host;
static char title_id[17];
static ModFile files[MAX_FILES];
static u32 nfiles;
static ModWindow windows[MAX_WINDOWS];
static u32 nwindows;
static int loading = -1;                    /* the file being initialised (add_window ties windows to it) */

void input_release_mouse(SDL_WindowID id);
void vk_queue_lock(void);
void vk_queue_unlock(void);
void ui_imgui_host(int *version, void **ctx, void *(**alloc)(size_t, void *), void (**free_)(void *, void *), void **ud);
void ui_show(void);

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

void mods_window_toggle(uint32_t i) {
    if (i >= nwindows) return;
    windows[i].toggle();
    if (windows[i].is_open()) ui_show();        /* its window shows: the mouse is freed for it */
}
static void hotkey_toggle(void *ud) { mods_window_toggle((u32)(uintptr_t)ud); }

static bool add_window(const char *title, const char *default_key, bool (*is_open)(void), void (*toggle)(void)) {
    if (nwindows == MAX_WINDOWS || !title || !is_open || !toggle) return false;
    ModWindow *w = &windows[nwindows];
    snprintf(w->title, sizeof w->title, "%s", title);
    w->is_open = is_open;
    w->toggle = toggle;
    w->file = loading;
    char id[96], label[160];
    snprintf(id, sizeof id, "mod.%s", title);
    snprintf(label, sizeof label, "%s (mod)", title);
    SDL_Scancode k = default_key ? SDL_GetScancodeFromName(default_key) : SDL_SCANCODE_UNKNOWN;
    w->hotkey = options_add_hotkey(id, label, k, hotkey_toggle, (void *)(uintptr_t)nwindows);
    nwindows++;
    return true;
}

static bool disabled(const char *file) {
    const char *off = options_get("mods.off", "");
    size_t n = strlen(file);
    for (const char *p = off; (p = strstr(p, file)) != NULL; p += n)
        if ((p == off || p[-1] == ';') && (p[n] == ';' || !p[n])) return true;
    return false;
}

void mods_set_enabled(uint32_t i, bool on) {
    if (i >= nfiles) return;
    files[i].enabled = on;
    char off[1024] = "";
    for (u32 k = 0; k < nfiles; k++)
        if (!files[k].enabled) {
            if (*off) strncat(off, ";", sizeof off - strlen(off) - 1);
            strncat(off, files[k].file, sizeof off - strlen(off) - 1);
        }
    options_set("mods.off", off);
}

void mods_load(SDL_Window *window) {
    const char *e = getenv("NX_MODS");
    if (e && *e == '0') return;
    snprintf(title_id, sizeof title_id, "%016llX", (unsigned long long)NX_TITLE_ID);
    host = (NxModHost){NXMOD_VERSION, title_id, NX_TITLE, nx_cache_dir(), window, mod_log, release_mouse, diag_watch,
                       run_script};
    ui_imgui_host(&host.imgui_version, &host.imgui_context, &host.imgui_alloc, &host.imgui_free, &host.imgui_alloc_ud);
    host.add_window = add_window;
    char dir[MAX_PATH], pat[MAX_PATH + 16];
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    char *s = strrchr(dir, '\\');
    if (s) *s = 0;
    snprintf(pat, sizeof pat, "%s\\mods\\*.dll", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (nfiles == MAX_FILES) break;
        ModFile *mf = &files[nfiles];
        snprintf(mf->file, sizeof mf->file, "%s", fd.cFileName);
        mf->enabled = !disabled(fd.cFileName);
        nfiles++;
        if (!mf->enabled) { snprintf(mf->status, sizeof mf->status, "off (turned on at the next start)"); continue; }
        char path[MAX_PATH * 2];
        snprintf(path, sizeof path, "%s\\mods\\%s", dir, fd.cFileName);
        HMODULE m = LoadLibraryExA(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!m) {
            snprintf(mf->status, sizeof mf->status, "does not load (error %lu)", GetLastError());
            hlog(0, "mods: %s %s", fd.cFileName, mf->status);
            continue;
        }
        NxModInit init = (NxModInit)(void *)GetProcAddress(m, "nx_mod_init");
        loading = (int)nfiles - 1;
        u32 before = nwindows;
        const NxMod *mod = init && nmods < MAX_MODS ? init(&host) : NULL;
        loading = -1;
        if (!mod) {
            nwindows = before;                  /* (a declining mod should not have added any) */
            snprintf(mf->status, sizeof mf->status, "%s", init ? "declined (another game or an older mod version)" :
                                                                  "not a mod (no nx_mod_init)");
            hlog(0, "mods: %s %s", fd.cFileName, mf->status);
            FreeLibrary(m);
            continue;
        }
        mf->mod = mod;
        snprintf(mf->status, sizeof mf->status, "loaded");
        mods[nmods++] = mod;
        hlog(0, "mods: %s %s (%s)", mod->name ? mod->name : "?", mod->version ? mod->version : "", fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

uint32_t mods_files(const ModFile **out) { *out = files; return nfiles; }
uint32_t mods_windows(const ModWindow **out) { *out = windows; return nwindows; }

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

bool mods_any_window_open(void) {
    for (u32 i = 0; i < nwindows; i++)
        if (windows[i].is_open()) return true;
    return false;
}

void mods_ui(void) {
    for (u32 i = 0; i < nmods; i++)
        if (mods[i]->ui) mods[i]->ui();
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
