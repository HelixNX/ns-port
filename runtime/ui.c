/* The menu's runtime side: loads nxui.dll from next to the executable (ui_host.h says why it is a DLL) and hands
 * it the settings, controls and mods. Without the DLL the game still runs: Esc frees the mouse and the hotkeys
 * work, there is just no menu.
 *
 * Window thread: ui_event() first for every event (Esc, the menu's keys and clicks, rebinding, hotkeys), then mods'
 * event(), then input.c. ui_tick() once a loop. Render thread: ui_draw() in the present (vk.c). */
#include "ui.h"
#include "framerate.h"
#include "game_config.h"
#include "hos.h"
#include "input.h"
#include "mods.h"
#include "options.h"
#include "ui_host.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

void input_release_mouse(SDL_WindowID id);
void vk_queue_lock(void);
void vk_queue_unlock(void);

static UiHost h;
static NxUiEvent dll_event;
static NxUiVoid dll_tick, dll_toggle, dll_show;
static NxUiBool dll_wants_kb, dll_active;
static NxUiDraw dll_draw;
static NxUiImGui dll_imgui;

static int32_t get_rate(void) { return options_framerate(); }
static void set_rate(int32_t r) { options_set_framerate(r); }
static uint32_t get_scale(void) { return options_render_scale(); }
static void set_scale(uint32_t s) { options_set_render_scale(s); }
static int hk_key(int i) { return (int)options_hotkey_key(i); }
static void set_hk_key(int i, int k) { options_set_hotkey_key(i, (SDL_Scancode)k); }
static int get_key(int a) { return (int)input_key(a); }
static void set_key(int a, int k) { input_set_key(a, (SDL_Scancode)k); }

bool ui_init(SDL_Window *window) {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *s = strrchr(path, '\\');
    if (s) snprintf(s + 1, sizeof path - (size_t)(s + 1 - path), "nxui.dll");
    HMODULE m = LoadLibraryExA(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!m) { hlog(0, "ui: no nxui.dll next to the exe (error %lu): no menu", GetLastError()); return false; }
    NxUiInit init = (NxUiInit)(void *)GetProcAddress(m, "nxui_init");
    dll_imgui = (NxUiImGui)(void *)GetProcAddress(m, "nxui_imgui");
    dll_event = (NxUiEvent)(void *)GetProcAddress(m, "nxui_event");
    dll_tick = (NxUiVoid)(void *)GetProcAddress(m, "nxui_tick");
    dll_toggle = (NxUiVoid)(void *)GetProcAddress(m, "nxui_toggle_menu");
    dll_show = (NxUiVoid)(void *)GetProcAddress(m, "nxui_show");
    dll_wants_kb = (NxUiBool)(void *)GetProcAddress(m, "nxui_wants_keyboard");
    dll_active = (NxUiBool)(void *)GetProcAddress(m, "nxui_active");
    dll_draw = (NxUiDraw)(void *)GetProcAddress(m, "nxui_draw");
    h = (UiHost){
        .version = NXUI_VERSION, .log = hlog, .cache_dir = nx_cache_dir(), .title = NX_TITLE, .window = window,
#ifdef NX_PACKAGE
        .development = false,
#else
        .development = true,
#endif
        .framerate = get_rate, .set_framerate = set_rate, .render_scale = get_scale, .set_render_scale = set_scale,
        .fps_counter = options_fps_counter, .set_fps_counter = options_set_fps_counter,
        .fullscreen = options_fullscreen, .set_fullscreen = options_set_fullscreen,
        .mouse_sens = options_mouse_sens, .set_mouse_sens = options_set_mouse_sens,
        .mouse_invert_y = options_mouse_invert_y, .set_mouse_invert_y = options_set_mouse_invert_y,
        .mouse_rest = options_mouse_rest, .set_mouse_rest = options_set_mouse_rest,
        .mouse_captured = input_mouse_captured, .capture_mouse = input_capture_mouse,
        .hotkey_count = options_hotkey_count, .hotkey_label = options_hotkey_label, .hotkey_key = hk_key,
        .set_hotkey_key = set_hk_key, .reset_hotkeys = options_reset_hotkeys, .hotkey = options_hotkey,
        .action_count = ACT_COUNT, .pad_action_count = ACT_PAD_COUNT, .action_label = input_action_label,
        .key = get_key, .set_key = set_key, .pad = input_pad, .set_pad = input_set_pad, .pad_name = input_pad_name,
        .reset_keys = input_reset_keys, .reset_pad = input_reset_pad, .pad_deadzone = input_pad_deadzone,
        .set_pad_deadzone = input_set_pad_deadzone, .pad_connected = input_pad_connected,
        .mods_files = mods_files, .mods_windows = mods_windows, .mods_window_toggle = mods_window_toggle,
        .mods_set_enabled = mods_set_enabled, .mods_any_window_open = mods_any_window_open, .mods_ui = mods_ui,
        .theme = NX_UI_THEME, .setting = options_get, .set_setting = options_set,
    };
    if (!init || !dll_imgui || !dll_event || !dll_tick || !dll_toggle || !dll_show || !dll_wants_kb || !dll_active ||
        !dll_draw || !init(&h)) {
        hlog(0, "ui: nxui.dll does not fit this build: no menu");
        dll_event = NULL;
        FreeLibrary(m);
        return false;
    }
    hlog(1, "ui: the menu (nxui.dll)");
    return true;
}

void ui_imgui_host(int *version, void **ctx, void *(**alloc)(size_t, void *), void (**free_)(void *, void *), void **ud) {
    if (dll_event) dll_imgui(version, ctx, alloc, free_, ud);
    else { *version = 0; *ctx = NULL; *alloc = NULL; *free_ = NULL; *ud = NULL; }
}

bool ui_event(const SDL_Event *e) {
    if (dll_event) return dll_event(e);
    /* no menu: Esc frees the mouse, the hotkeys still work */
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_ESCAPE) {
        if (!e->key.repeat) input_capture_mouse(false);
        return true;
    }
    return options_hotkey(e);
}

void ui_tick(void) { if (dll_event) dll_tick(); }
bool ui_wants_keyboard(void) { return dll_event && dll_wants_kb(); }
bool ui_active(void) { return dll_event && dll_active(); }
void ui_toggle_menu(void) { if (dll_event) dll_toggle(); else input_capture_mouse(false); }
void ui_show(void) { if (dll_event) dll_show(); }

void ui_draw(VkCommandBuffer cmd, VkImage image, VkImageView view, VkFormat format, u32 w, u32 hgt, u32 count) {
    if (!dll_event) return;
    NxModFrame f = {cmd, image, view, format, w, hgt, count, NULL, vk.instance, vk.phys, vk.dev, vk.family, vk.queue,
                    vk_queue_lock, vk_queue_unlock};
    dll_draw(&f);
}
