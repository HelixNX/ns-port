/* The options panel, drawn by us over the game (the game's own options menu is the game's: not changed).
 *
 * Esc opens it (releasing the mouse) and closes it; up / down choose a line, left / right (or Enter) change it; the
 * mouse drags the sensitivity slider and clicks lines; a click outside the panel closes it and captures the mouse
 * again. The game gets no input while it is open. Lines: mouse sensitivity (0.25-4x of input.c's scale), the frame
 * rate (30, the game's own pace, or 60; F5 in a development build also has 120 and uncapped), the FPS counter
 * (F3) and the render scale (1-4x the game's 1080p, gpu.c; applied at the next frame).
 *
 * The values are kept in settings.ini in the cache folder (nx_cache_dir: build\ in a development build,
 * %LOCALAPPDATA%\<title>\ in a package), written on every change. NX_MOUSE_SENS, NX_FRAMERATE / NX_NOPACE,
 * NX_FPS and NX_RES_SCALE still decide at start when set; a change in the panel then wins and is saved. */
#include "options.h"
#include "framerate.h"
#include "hos.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

void ov_text(u32 *px, int w, int h, int x, int y, const char *s, u32 color, int scale);
void overlay_set(bool on);
bool overlay_shown(void);
void vk_swap_extent(u32 *w, u32 *h);
void input_release_mouse(SDL_WindowID id);
void gpu_render_scale_set(u32 s);
u32 gpu_render_scale(void);

enum { L_SENS, L_RATE, L_FPS, L_SCALE, N_LINES };
#define SCALE_MAX 4
#define SENS_MIN 0.25f
#define SENS_MAX 4.0f
/* layout (panel pixels) */
#define ROW_Y(i) (44 + (i) * 44)
#define BAR_X0 16
#define BAR_X1 (OPT_W - 16)
#define BAR_Y (ROW_Y(L_SENS) + 22)

static volatile LONG open_;
static int sel;
static float sens = 1.0f;
static bool dragging;
static const s32 rates[] = {FRAMERATE_GAME, 60};       /* 30 (the game's own pace) and 60 */

static void settings_path(char *out, size_t n) { snprintf(out, n, "%s\\settings.ini", nx_cache_dir()); }

static void save(void) {
    char path[MAX_PATH + 16];
    settings_path(path, sizeof path);
    FILE *f = fopen(path, "w");
    if (!f) { hlog(0, "options: cannot write %s", path); return; }
    fprintf(f, "mouse_sensitivity=%.2f\nframerate=%d\nfps_counter=%d\nrender_scale=%u\n", sens, nx_framerate(),
            overlay_shown(), gpu_render_scale());
    fclose(f);
}

void options_init(void) {
    char path[MAX_PATH + 16], line[128];
    settings_path(path, sizeof path);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        const char *v = eq + 1;
        if (!strcmp(line, "mouse_sensitivity") && !getenv("NX_MOUSE_SENS")) sens = (float)atof(v);
        else if (!strcmp(line, "framerate") && !getenv("NX_FRAMERATE") && !getenv("NX_NOPACE")) nx_framerate_set(atoi(v));
        else if (!strcmp(line, "fps_counter") && !getenv("NX_FPS")) overlay_set(atoi(v) != 0);
        else if (!strcmp(line, "render_scale")) gpu_render_scale_set((u32)atoi(v));
    }
    const char *rs = getenv("NX_RES_SCALE");
    if (rs) gpu_render_scale_set((u32)atoi(rs));
    if (gpu_render_scale() > 1) hlog(0, "options: render scale %ux", gpu_render_scale());
    if (f) { fclose(f); hlog(1, "options: %s", path); }
    if (getenv("NX_MOUSE_SENS")) sens = (float)atof(getenv("NX_MOUSE_SENS"));
    if (!(sens >= SENS_MIN)) sens = sens > 0 ? SENS_MIN : 1.0f;
    if (sens > SENS_MAX) sens = SENS_MAX;
}

bool options_is_open(void) { return open_ != 0; }
float options_mouse_sens(void) { return sens; }

static void set_sens(float v) {
    v = roundf(v * 4) / 4;
    sens = v < SENS_MIN ? SENS_MIN : v > SENS_MAX ? SENS_MAX : v;
}

static int rate_index(void) {
    s32 r = nx_framerate();
    for (int i = 0; i < (int)(sizeof rates / sizeof *rates); i++)
        if (rates[i] == r) return i;
    return r > 0 && r < 45 ? 0 : 1;     /* another rate (NX_FRAMERATE, F5 in a development build): the nearest */
}

static void change(int line, int d) {
    if (line == L_SENS) set_sens(sens + 0.25f * d);
    else if (line == L_RATE) {
        int n = (int)(sizeof rates / sizeof *rates), i = (rate_index() + d + n) % n;
        nx_framerate_set(rates[i]);
    } else if (line == L_FPS) overlay_set(!overlay_shown());
    else if (line == L_SCALE) gpu_render_scale_set((gpu_render_scale() - 1 + SCALE_MAX + d) % SCALE_MAX + 1);
    save();
}

/* panel coordinates of a mouse event (window coordinates; the panel is centred in the swapchain image) */
static bool panel_xy(SDL_WindowID id, float wx, float wy, int *x, int *y) {
    SDL_Window *w = SDL_GetWindowFromID(id);
    float d = w ? SDL_GetWindowPixelDensity(w) : 1.0f;
    u32 W, H;
    vk_swap_extent(&W, &H);
    *x = (int)(wx * d) - (int)(W - OPT_W) / 2;
    *y = (int)(wy * d) - (int)(H - OPT_H) / 2;
    return *x >= 0 && *y >= 0 && *x < OPT_W && *y < OPT_H;
}

static void drag_to(int x) {
    float t = (float)(x - BAR_X0) / (float)(BAR_X1 - BAR_X0);
    set_sens(SENS_MIN + (t < 0 ? 0 : t > 1 ? 1 : t) * (SENS_MAX - SENS_MIN));
}

bool options_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_ESCAPE) {
        if (!e->key.repeat) {
            if (open_) InterlockedExchange(&open_, 0);
            else {
                input_release_mouse(e->key.windowID);
                sel = 0;
                dragging = false;
                InterlockedExchange(&open_, 1);
            }
        }
        return true;
    }
    if (!open_) return false;
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        switch (e->key.scancode) {
        case SDL_SCANCODE_UP: sel = (sel + N_LINES - 1) % N_LINES; break;
        case SDL_SCANCODE_DOWN: sel = (sel + 1) % N_LINES; break;
        case SDL_SCANCODE_LEFT: change(sel, -1); break;
        case SDL_SCANCODE_RIGHT: case SDL_SCANCODE_RETURN: change(sel, 1); break;
        default: break;
        }
        return true;
    case SDL_EVENT_KEY_UP:
        return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        int x, y;
        if (!panel_xy(e->button.windowID, e->button.x, e->button.y, &x, &y)) {
            InterlockedExchange(&open_, 0);      /* outside: back to the game, the click captures the mouse */
            return false;
        }
        for (int i = 0; i < N_LINES; i++)
            if (y >= ROW_Y(i) - 4 && y < ROW_Y(i) + 36) {
                sel = i;
                if (i == L_SENS && y >= BAR_Y - 8) { dragging = true; drag_to(x); }
                else if (i != L_SENS) change(i, 1);
            }
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION:
        if (dragging) {
            int x, y;
            panel_xy(e->motion.windowID, e->motion.x, e->motion.y, &x, &y);
            drag_to(x);
        }
        return true;
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (dragging) { dragging = false; save(); }
        return true;
    case SDL_EVENT_MOUSE_WHEEL:
        if (sel == L_SENS) change(L_SENS, e->wheel.y > 0 ? 1 : -1);
        return true;
    default:
        return false;
    }
}

void options_draw(u32 *px) {
    const u32 bg = 0xff181818, edge = 0xff606060, white = 0xffffffff, green = 0xff40ff40, grey = 0xff909090;
    for (int i = 0; i < OPT_W * OPT_H; i++) px[i] = bg;
    for (int x = 0; x < OPT_W; x++) px[x] = px[(OPT_H - 1) * OPT_W + x] = edge;
    for (int y = 0; y < OPT_H; y++) px[y * OPT_W] = px[y * OPT_W + OPT_W - 1] = edge;
    ov_text(px, OPT_W, OPT_H, 16, 14, "OPTIONS", white, 2);
    char v[32];
    for (int i = 0; i < N_LINES; i++) {
        u32 c = i == sel ? green : white;
        const char *label = i == L_SENS ? "MOUSE SENSITIVITY" : i == L_RATE ? "FRAME RATE" :
                            i == L_FPS ? "FPS COUNTER" : "RENDER SCALE";
        if (i == L_SENS) snprintf(v, sizeof v, "%.2fX", sens);
        else if (i == L_RATE) {
            s32 r = nx_framerate();
            if (r == FRAMERATE_GAME) snprintf(v, sizeof v, "30");
            else if (r == FRAMERATE_UNCAPPED) snprintf(v, sizeof v, "UNCAPPED");
            else snprintf(v, sizeof v, "%d", r);
        } else if (i == L_FPS) snprintf(v, sizeof v, "%s", overlay_shown() ? "ON" : "OFF");
        else snprintf(v, sizeof v, "%uX  %uP", gpu_render_scale(), 1080 * gpu_render_scale());
        ov_text(px, OPT_W, OPT_H, 16, ROW_Y(i), label, c, 2);
        ov_text(px, OPT_W, OPT_H, OPT_W - 16 - 12 * (int)strlen(v), ROW_Y(i), v, c, 2);
    }
    /* the slider: SENS_MIN at the left, SENS_MAX at the right */
    int knob = BAR_X0 + (int)((sens - SENS_MIN) / (SENS_MAX - SENS_MIN) * (BAR_X1 - BAR_X0));
    for (int y = BAR_Y; y < BAR_Y + 6; y++)
        for (int x = BAR_X0; x < BAR_X1; x++) px[y * OPT_W + x] = x <= knob ? (sel == L_SENS ? green : white) : edge;
    for (int y = BAR_Y - 4; y < BAR_Y + 10; y++)
        for (int x = knob - 3; x <= knob + 3; x++)
            if (x >= 0 && x < OPT_W) px[y * OPT_W + x] = white;
    ov_text(px, OPT_W, OPT_H, 16, OPT_H - 26, "ESC CLOSE   ARROWS CHANGE", grey, 2);
}
