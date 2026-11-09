/* Settings (options.c): settings.ini in the cache folder, the video options, the hotkeys. The menu that changes
 * them is ui.cpp; the controls (key and button bindings) are input.c's, kept in the same file. */
#pragma once
#include "a64rt.h"
#include <SDL3/SDL.h>

void options_init(void);                    /* settings.ini: window thread, before the first frame and the mods */
void options_tick(void);                    /* window thread, every loop: applies changes made in the menu, saves */

/* the raw store (any thread): a value for a key, or def; set marks the file to be written by options_tick */
const char *options_get(const char *key, const char *def);
int options_get_int(const char *key, int def);
float options_get_float(const char *key, float def);
void options_set(const char *key, const char *value);
void options_set_int(const char *key, int v);
void options_set_float(const char *key, float v);

/* video (the menu calls these from the render thread; applied on the window thread by options_tick) */
s32 options_framerate(void);                /* FRAMERATE_GAME, FRAMERATE_UNCAPPED or fps (framerate.h) */
void options_set_framerate(s32 r);
u32 options_render_scale(void);             /* 1-4 */
void options_set_render_scale(u32 s);
bool options_fps_counter(void);
void options_set_fps_counter(bool on);
bool options_fullscreen(void);
void options_set_fullscreen(bool on);
void options_toggle_fullscreen(void);       /* window thread */

/* mouse (input.c reads these every poll) */
float options_mouse_sens(void);             /* 0.25-4: the right stick's scale */
void options_set_mouse_sens(float v);
bool options_mouse_invert_y(void);
void options_set_mouse_invert_y(bool on);
float options_mouse_rest(void);             /* pixels a second below which the stick stays centred */
void options_set_mouse_rest(float v);

/* hotkeys: built-in ones and mods' windows (mods.c registers those). Window thread: options_hotkey() runs the
 * action of a key-down event and returns true. The menu lists them (ui.cpp) and rebinds them. */
typedef void (*NxHotkeyFn)(void *ud);
int options_add_hotkey(const char *id, const char *label, SDL_Scancode def, NxHotkeyFn fn, void *ud);  /* index */
int options_hotkey_count(void);
const char *options_hotkey_label(int i);
SDL_Scancode options_hotkey_key(int i);
void options_set_hotkey_key(int i, SDL_Scancode k);     /* SDL_SCANCODE_UNKNOWN: none */
void options_reset_hotkeys(void);
bool options_hotkey(const SDL_Event *e);
