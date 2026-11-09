/* Settings: settings.ini in the cache folder (nx_cache_dir: build\ in a development build, %LOCALAPPDATA%\<title>\ in
 * a package), "key=value" lines, written by the window thread soon after any change (options_tick).
 *
 * Video: frame rate (the game's own 30 or 60; a development build also 120 and uncapped), render scale (1-4x the
 * game's 1080p, gpu.c), the FPS counter, fullscreen (borderless). The menu (ui.cpp, render thread) only records
 * what it wants; options_tick applies it on the window thread, which owns the window and the swapchain's pacing.
 * Mouse: sensitivity (0.25-4x of input.c's scale), invert Y, the resting speed below which the stick stays centred.
 * Hotkeys: the menu (F1), the FPS counter (F3), a frame capture (F12), fullscreen (Alt+Enter, fixed), the frame
 * rate cycle (F5, development builds) and mods' windows; each rebindable, stored as hotkey.<id>=<SDL key name>.
 * NX_MOUSE_SENS, NX_FRAMERATE / NX_NOPACE, NX_FPS and NX_RES_SCALE still decide at start when set; a change in the
 * menu then wins and is saved. */
#include "options.h"
#include "framerate.h"
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

void overlay_set(bool on);
bool overlay_shown(void);
void gpu_render_scale_set(u32 s);
u32 gpu_render_scale(void);
void gpu_capture_next(void);
SDL_Window *vk_window(void);
void ui_toggle_menu(void);

/* ---------------------------------------------------------------------------------------------- the store */
#define MAX_KEYS 256
static struct { char key[64], value[128]; } kv[MAX_KEYS];
static int nkv;
static SRWLOCK kv_lock = SRWLOCK_INIT;
static volatile LONG dirty;

static void settings_path(char *out, size_t n) { snprintf(out, n, "%s\\settings.ini", nx_cache_dir()); }

const char *options_get(const char *key, const char *def) {
    const char *r = def;
    AcquireSRWLockShared(&kv_lock);
    for (int i = 0; i < nkv; i++)
        if (!strcmp(kv[i].key, key)) { r = kv[i].value; break; }
    ReleaseSRWLockShared(&kv_lock);
    return r;           /* the slot is never freed: a later set changes the text in place */
}
int options_get_int(const char *key, int def) { const char *v = options_get(key, NULL); return v ? atoi(v) : def; }
float options_get_float(const char *key, float def) { const char *v = options_get(key, NULL); return v ? (float)atof(v) : def; }

static void put(const char *key, const char *value) {
    AcquireSRWLockExclusive(&kv_lock);
    int i = 0;
    while (i < nkv && strcmp(kv[i].key, key)) i++;
    if (i == nkv && nkv < MAX_KEYS) snprintf(kv[nkv++].key, sizeof kv[0].key, "%s", key);
    if (i < MAX_KEYS) snprintf(kv[i].value, sizeof kv[i].value, "%s", value);
    ReleaseSRWLockExclusive(&kv_lock);
}
void options_set(const char *key, const char *value) { put(key, value); InterlockedExchange(&dirty, 1); }
void options_set_int(const char *key, int v) { char s[32]; snprintf(s, sizeof s, "%d", v); options_set(key, s); }
void options_set_float(const char *key, float v) { char s[32]; snprintf(s, sizeof s, "%.3f", v); options_set(key, s); }

static void save(void) {
    char path[MAX_PATH + 16];
    settings_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) { hlog(0, "options: cannot write %s", path); return; }
    AcquireSRWLockShared(&kv_lock);
    for (int i = 0; i < nkv; i++) fprintf(f, "%s=%s\n", kv[i].key, kv[i].value);
    ReleaseSRWLockShared(&kv_lock);
    fclose(f);
}

/* ---------------------------------------------------------------------------------------------- video */
static volatile LONG want_rate = 0x7fffffff, want_scale, want_fps = -1, want_full = -1;
static bool fullscreen;

s32 options_framerate(void) { return want_rate != 0x7fffffff ? (s32)want_rate : nx_framerate(); }
void options_set_framerate(s32 r) { InterlockedExchange(&want_rate, r); }
u32 options_render_scale(void) { return want_scale ? (u32)want_scale : gpu_render_scale(); }
void options_set_render_scale(u32 s) { InterlockedExchange(&want_scale, (LONG)(s < 1 ? 1 : s > 4 ? 4 : s)); }
bool options_fps_counter(void) { return want_fps >= 0 ? want_fps != 0 : overlay_shown(); }
void options_set_fps_counter(bool on) { InterlockedExchange(&want_fps, on); }
bool options_fullscreen(void) { return want_full >= 0 ? want_full != 0 : fullscreen; }
void options_set_fullscreen(bool on) { InterlockedExchange(&want_full, on); }
void options_toggle_fullscreen(void) { options_set_fullscreen(!options_fullscreen()); }

/* ---------------------------------------------------------------------------------------------- mouse */
#define SENS_MIN 0.25f
#define SENS_MAX 4.0f
static float sens = 1.0f, rest = 150.0f;
static bool invert_y;

float options_mouse_sens(void) { return sens; }
void options_set_mouse_sens(float v) {
    sens = v < SENS_MIN ? SENS_MIN : v > SENS_MAX ? SENS_MAX : v;
    options_set_float("mouse_sensitivity", sens);
}
bool options_mouse_invert_y(void) { return invert_y; }
void options_set_mouse_invert_y(bool on) { invert_y = on; options_set_int("mouse_invert_y", on); }
float options_mouse_rest(void) { return rest; }
void options_set_mouse_rest(float v) { rest = v < 0 ? 0 : v > 1000 ? 1000 : v; options_set_float("mouse_rest", rest); }

/* ---------------------------------------------------------------------------------------------- hotkeys */
#define MAX_HOTKEYS 48
static struct Hotkey { char id[64], label[96]; SDL_Scancode def, key; NxHotkeyFn fn; void *ud; } hk[MAX_HOTKEYS];
static volatile LONG nhk;

int options_add_hotkey(const char *id, const char *label, SDL_Scancode def, NxHotkeyFn fn, void *ud) {
    if (nhk >= MAX_HOTKEYS) return -1;
    struct Hotkey *h = &hk[nhk];
    snprintf(h->id, sizeof h->id, "%s", id);
    snprintf(h->label, sizeof h->label, "%s", label);
    h->def = def; h->fn = fn; h->ud = ud;
    char k[96];
    snprintf(k, sizeof k, "hotkey.%s", id);
    const char *v = options_get(k, NULL);
    h->key = !v ? def : !*v ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(v);
    return (int)InterlockedIncrement(&nhk) - 1;
}
int options_hotkey_count(void) { return (int)nhk; }
const char *options_hotkey_label(int i) { return i >= 0 && i < nhk ? hk[i].label : ""; }
SDL_Scancode options_hotkey_key(int i) { return i >= 0 && i < nhk ? hk[i].key : SDL_SCANCODE_UNKNOWN; }
void options_set_hotkey_key(int i, SDL_Scancode k) {
    if (i < 0 || i >= nhk) return;
    hk[i].key = k;
    char key[96];
    snprintf(key, sizeof key, "hotkey.%s", hk[i].id);
    options_set(key, k == SDL_SCANCODE_UNKNOWN ? "" : SDL_GetScancodeName(k));
}
void options_reset_hotkeys(void) { for (int i = 0; i < nhk; i++) options_set_hotkey_key(i, hk[i].def); }

bool options_hotkey(const SDL_Event *e) {
    if (e->type != SDL_EVENT_KEY_DOWN || e->key.repeat) return false;
    if (e->key.scancode == SDL_SCANCODE_RETURN && (e->key.mod & SDL_KMOD_ALT)) { options_toggle_fullscreen(); return true; }
    if (e->key.mod & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI)) return false;
    for (int i = 0; i < nhk; i++)
        if (hk[i].key != SDL_SCANCODE_UNKNOWN && hk[i].key == e->key.scancode) { hk[i].fn(hk[i].ud); return true; }
    return false;
}

static void hk_menu(void *ud) { (void)ud; ui_toggle_menu(); }
static void hk_fps(void *ud) { (void)ud; options_set_fps_counter(!options_fps_counter()); }
static void hk_capture(void *ud) { (void)ud; gpu_capture_next(); }
#ifndef NX_PACKAGE
static void hk_rate(void *ud) { (void)ud; nx_framerate_cycle(); options_set_int("framerate", nx_framerate()); }
#endif

/* ---------------------------------------------------------------------------------------------- start, tick */
void options_init(void) {
    char path[MAX_PATH + 16], line[256];
    settings_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1, *nl = strpbrk(v, "\r\n");
        if (nl) *nl = 0;
        put(line, v);
    }
    if (f) { fclose(f); hlog(1, "options: %s", path); }
    const char *v;
    if ((v = options_get("framerate", NULL)) && !getenv("NX_FRAMERATE") && !getenv("NX_NOPACE")) {
        s32 r = atoi(v);
#ifdef NX_PACKAGE                       /* players get 30 or 60 only (the menu's choices); anything else is 30 */
        if (r != 60) r = FRAMERATE_GAME;
#endif
        nx_framerate_set(r);
    }
    if ((v = options_get("fps_counter", NULL)) && !getenv("NX_FPS")) overlay_set(atoi(v) != 0);
    if ((v = options_get("render_scale", NULL))) gpu_render_scale_set((u32)atoi(v));
    const char *rs = getenv("NX_RES_SCALE");
    if (rs) gpu_render_scale_set((u32)atoi(rs));
    if (gpu_render_scale() > 1) hlog(0, "options: render scale %ux", gpu_render_scale());
    sens = options_get_float("mouse_sensitivity", 1.0f);
    if (getenv("NX_MOUSE_SENS")) sens = (float)atof(getenv("NX_MOUSE_SENS"));
    if (!(sens >= SENS_MIN)) sens = sens > 0 ? SENS_MIN : 1.0f;
    if (sens > SENS_MAX) sens = SENS_MAX;
    invert_y = options_get_int("mouse_invert_y", 0) != 0;
    rest = options_get_float("mouse_rest", 150.0f);
    if (options_get_int("fullscreen", 0)) want_full = 1;
    options_add_hotkey("menu", "Menu", SDL_SCANCODE_F1, hk_menu, NULL);
    options_add_hotkey("fps", "FPS counter", SDL_SCANCODE_F3, hk_fps, NULL);
    options_add_hotkey("capture", "Capture frame", SDL_SCANCODE_F12, hk_capture, NULL);
#ifndef NX_PACKAGE
    options_add_hotkey("framerate", "Frame rate (dev)", SDL_SCANCODE_F5, hk_rate, NULL);
#endif
}

void options_tick(void) {
    LONG r = InterlockedExchange(&want_rate, 0x7fffffff);
    if (r != 0x7fffffff) { nx_framerate_set(r); options_set_int("framerate", r); }
    LONG s = InterlockedExchange(&want_scale, 0);
    if (s) { gpu_render_scale_set((u32)s); options_set_int("render_scale", s); }
    LONG fp = InterlockedExchange(&want_fps, -1);
    if (fp >= 0) { overlay_set(fp != 0); options_set_int("fps_counter", fp); }
    LONG fs = InterlockedExchange(&want_full, -1);
    if (fs >= 0 && (fs != 0) != fullscreen) {
        SDL_Window *w = vk_window();
        if (w && SDL_SetWindowFullscreen(w, fs != 0)) {
            fullscreen = fs != 0;
            options_set_int("fullscreen", fullscreen);
        }
    }
    if (InterlockedExchange(&dirty, 0)) save();
}
