/* The menu (nxui.dll, nxrecomp/ui/nxui.cpp) and the runtime (ui.c) talk through this table and these exports.
 *
 * Why a DLL: the runtime defines its own Vulkan function pointers under Vulkan's own names (vk.h), and vcpkg's
 * Dear ImGui backend calls Vulkan by those names; linked into the exe it would call the pointers' addresses. In a
 * DLL of its own it links the Vulkan loader, as mods do. Without nxui.dll the game runs, without a menu. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "mods_list.h"                      /* nxmod.h (SDL, Vulkan types, NxModFrame) and the mod lists */

#ifdef __cplusplus
extern "C" {
#endif

#define NXUI_VERSION 2

typedef struct UiHost {
    uint32_t version;                       /* NXUI_VERSION */
    void (*log)(int level, const char *fmt, ...);
    const char *cache_dir;
    const char *title;
    SDL_Window *window;
    bool development;                       /* not a package: development frame rates and hotkeys */
    /* video (options.h: recorded here, applied on the window thread) */
    int32_t (*framerate)(void);             /* -1 the game's own, 0 uncapped, else fps */
    void (*set_framerate)(int32_t r);
    uint32_t (*render_scale)(void);
    void (*set_render_scale)(uint32_t s);
    bool (*fps_counter)(void);
    void (*set_fps_counter)(bool on);
    bool (*fullscreen)(void);
    void (*set_fullscreen)(bool on);
    /* mouse */
    float (*mouse_sens)(void);
    void (*set_mouse_sens)(float v);
    bool (*mouse_invert_y)(void);
    void (*set_mouse_invert_y)(bool on);
    float (*mouse_rest)(void);
    void (*set_mouse_rest)(float v);
    bool (*mouse_captured)(void);
    void (*capture_mouse)(bool on);         /* window thread */
    /* hotkeys */
    int (*hotkey_count)(void);
    const char *(*hotkey_label)(int i);
    int (*hotkey_key)(int i);               /* SDL_Scancode */
    void (*set_hotkey_key)(int i, int key);
    void (*reset_hotkeys)(void);
    bool (*hotkey)(const SDL_Event *e);     /* window thread: runs a hotkey's action */
    /* controls (input.h) */
    int action_count, pad_action_count;
    const char *(*action_label)(int a);
    int (*key)(int a);
    void (*set_key)(int a, int key);
    int (*pad)(int a);
    void (*set_pad)(int a, int button);     /* SDL_GamepadButton, button_count (+0 left / +1 right trigger), -1 */
    const char *(*pad_name)(int button);
    void (*reset_keys)(void);
    void (*reset_pad)(bool nintendo_positions);
    float (*pad_deadzone)(void);
    void (*set_pad_deadzone)(float d);
    const char *(*pad_connected)(void);
    /* mods */
    uint32_t (*mods_files)(const ModFile **out);
    uint32_t (*mods_windows)(const ModWindow **out);
    void (*mods_window_toggle)(uint32_t i);
    void (*mods_set_enabled)(uint32_t i, bool on);
    bool (*mods_any_window_open)(void);
    void (*mods_ui)(void);
    /* version 2: the game's theme (game.json "theme": "key=value;...", empty: none) and settings.ini itself */
    const char *theme;
    const char *(*setting)(const char *key, const char *def);
    void (*set_setting)(const char *key, const char *value);
} UiHost;

/* nxui.dll's exports */
typedef bool (*NxUiInit)(const UiHost *host);                          /* nxui_init: window thread, before mods */
typedef void (*NxUiImGui)(int *version, void **ctx, void *(**alloc)(size_t, void *), void (**free_)(void *, void *),
                          void **ud);                                   /* nxui_imgui: for NxModHost */
typedef bool (*NxUiEvent)(const SDL_Event *e);                         /* nxui_event: window thread; true: used */
typedef void (*NxUiVoid)(void);                                        /* nxui_tick (window thread), nxui_toggle_menu, nxui_show */
typedef bool (*NxUiBool)(void);                                        /* nxui_wants_keyboard, nxui_active */
typedef void (*NxUiDraw)(const NxModFrame *f);                         /* nxui_draw: render thread, in the present */

#ifdef __cplusplus
}
#endif
