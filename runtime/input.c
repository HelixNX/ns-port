/* Input: the keyboard and the first gamepad (SDL3) as player 1's Pro Controller (services/hid.c).
 *
 * Keyboard: WASD = left stick, IJKL = right stick, arrows = D-pad (games give the D-pad its own actions, e.g.
 * Violet's emotes, so it is not also a stick), Z = A, X = B, C = X, V = Y, Q = L, R = R, 1 = ZL, 3 = ZR,
 * Enter = +, Backspace = -. Gamepads map by label (an Xbox pad's bottom button is A).
 * Mouse: the right stick while captured (a click in the window captures it, Esc or losing focus releases it): the
 * stick follows the mouse's smoothed speed, full at 2000 pixels a second times NX_MOUSE_SENS (default 1); below
 * 150 pixels a second (a resting hand on a high-resolution mouse) it stays centred.
 * NX_INPUT="first-last:A+B;..." holds buttons over ranges of presented frames (unattended runs);
 * "first-last/n:A" presses A for 3 frames every n frames (mashing through screens). Names: A B X Y
 * L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN (D-pad), LSLEFT LSUP LSRIGHT LSDOWN and RSLEFT ... (sticks)
 * NX_INPUT_LOG logs every change of the input; NX_INPUT_REPLAY=<log file> plays such a log back by frame (a run
 * recorded by hand, replayed unattended), on top of live input. */
#include "hos.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <string.h>

void hid_set_input(int npad, u64 buttons, s32 lx, s32 ly, s32 rx, s32 ry);
u64 gpu_frames(void);

enum {
    B_A = 1 << 0, B_B = 1 << 1, B_X = 1 << 2, B_Y = 1 << 3, B_LSTICK = 1 << 4, B_RSTICK = 1 << 5, B_L = 1 << 6,
    B_R = 1 << 7, B_ZL = 1 << 8, B_ZR = 1 << 9, B_PLUS = 1 << 10, B_MINUS = 1 << 11, B_LEFT = 1 << 12, B_UP = 1 << 13,
    B_RIGHT = 1 << 14, B_DOWN = 1 << 15,
    B_LS_LEFT = 1 << 16, B_LS_UP = 1 << 17, B_LS_RIGHT = 1 << 18, B_LS_DOWN = 1 << 19,
    B_RS_LEFT = 1 << 20, B_RS_UP = 1 << 21, B_RS_RIGHT = 1 << 22, B_RS_DOWN = 1 << 23,
};

static SDL_Gamepad *pad;
static bool mouse_captured;
static float mouse_dx, mouse_dy;        /* motion since the last poll (window thread: events and polls alike) */

static void mouse_capture(SDL_WindowID id, bool on) {
    SDL_Window *w = SDL_GetWindowFromID(id);
    if (w && SDL_SetWindowRelativeMouseMode(w, on)) mouse_captured = on;
    mouse_dx = mouse_dy = 0;
}

void input_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && !mouse_captured) mouse_capture(e->button.windowID, true);
    else if (e->type == SDL_EVENT_KEY_DOWN && e->key.scancode == SDL_SCANCODE_ESCAPE && mouse_captured)
        mouse_capture(e->key.windowID, false);
    else if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST && mouse_captured) mouse_capture(e->window.windowID, false);
    else if (e->type == SDL_EVENT_MOUSE_MOTION && mouse_captured) {
        mouse_dx += e->motion.xrel;
        mouse_dy += e->motion.yrel;
    }
    if (e->type == SDL_EVENT_JOYSTICK_ADDED)            /* every device SDL finds, a gamepad or not */
        hlog(1, "input: joystick %u %s (%s)", (unsigned)e->jdevice.which, SDL_GetJoystickNameForID(e->jdevice.which),
             SDL_IsGamepad(e->jdevice.which) ? "gamepad" : "not a gamepad");
    if (e->type == SDL_EVENT_GAMEPAD_ADDED && !pad) {
        pad = SDL_OpenGamepad(e->gdevice.which);
        if (pad) hlog(1, "input: gamepad %s", SDL_GetGamepadName(pad));
    } else if (e->type == SDL_EVENT_GAMEPAD_REMOVED && pad && SDL_GetGamepadID(pad) == e->gdevice.which) {
        SDL_CloseGamepad(pad);
        pad = NULL;
    }
}

static s32 axis(s32 v) { return v < -32767 ? -32767 : v; }

/* NX_INPUT */
static u64 scripted(u64 frame) {
    static char *script;
    static bool init;
    if (!init) {
        init = true;
        const char *e = getenv("NX_INPUT");
        if (e) script = _strdup(e);
    }
    if (!script) return 0;
    static const struct { const char *n; u64 bit; } names[] = {
        {"A", B_A}, {"B", B_B}, {"X", B_X}, {"Y", B_Y}, {"L", B_L}, {"R", B_R}, {"ZL", B_ZL}, {"ZR", B_ZR},
        {"PLUS", B_PLUS}, {"MINUS", B_MINUS}, {"LEFT", B_LEFT}, {"UP", B_UP}, {"RIGHT", B_RIGHT}, {"DOWN", B_DOWN},
        {"LSLEFT", B_LS_LEFT}, {"LSUP", B_LS_UP}, {"LSRIGHT", B_LS_RIGHT}, {"LSDOWN", B_LS_DOWN},
        {"RSLEFT", B_RS_LEFT}, {"RSUP", B_RS_UP}, {"RSRIGHT", B_RS_RIGHT}, {"RSDOWN", B_RS_DOWN}};
    u64 b = 0;
    const char *p = script;
    while (*p) {
        unsigned long long lo = strtoull(p, (char **)&p, 10), hi = lo, period = 0;
        if (*p == '-') hi = strtoull(p + 1, (char **)&p, 10);
        if (*p == '/') period = strtoull(p + 1, (char **)&p, 10);     /* "lo-hi/n:A": A for 3 frames every n */
        if (*p == ':') p++;
        bool on = frame >= lo && frame <= hi && (!period || (frame - lo) % period < 3);
        while (*p && *p != ';') {
            const char *q = p;
            while (*q && *q != '+' && *q != ';') q++;
            for (size_t i = 0; i < sizeof names / sizeof *names; i++)
                if (strlen(names[i].n) == (size_t)(q - p) && !strncmp(names[i].n, p, q - p) && on)
                    b |= names[i].bit;
            p = *q == '+' ? q + 1 : q;
        }
        if (*p == ';') p++;
    }
    return b;
}

/* NX_INPUT_REPLAY: the input of the last logged change at or before this frame */
static bool replayed(u64 frame, u64 *b, s32 *lx, s32 *ly, s32 *rx, s32 *ry) {
    static struct Rec { u64 frame, b; s32 lx, ly, rx, ry; } *rec;
    static size_t n, at;
    static bool init;
    if (!init) {
        init = true;
        const char *path = getenv("NX_INPUT_REPLAY");
        FILE *f = path ? fopen(path, "r") : NULL;
        if (path && !f) hlog(0, "input: cannot open %s", path);
        char line[512];
        size_t cap = 0;
        while (f && fgets(line, sizeof line, f)) {
            const char *p = strstr(line, "input: frame ");
            struct Rec r;
            if (!p || sscanf(p, "input: frame %llu buttons %llx left %d,%d right %d,%d", &r.frame, &r.b, &r.lx, &r.ly,
                             &r.rx, &r.ry) != 6)
                continue;
            if (n == cap) rec = realloc(rec, (cap = cap * 2 + 256) * sizeof *rec);
            rec[n++] = r;
        }
        if (f) { fclose(f); hlog(1, "input: replaying %zu input changes from %s", n, path); }
    }
    while (at < n && rec[at].frame <= frame) at++;
    if (!at) return false;
    const struct Rec *r = &rec[at - 1];
    *b = r->b; *lx = r->lx; *ly = r->ly; *rx = r->rx; *ry = r->ry;
    return true;
}

void input_poll(void) {
    const bool *k = SDL_GetKeyboardState(NULL);
    u64 b = 0;
    s32 lx = 0, ly = 0, rx = 0, ry = 0;
    static const struct { SDL_Scancode sc; u64 bit; } keys[] = {
        {SDL_SCANCODE_Z, B_A}, {SDL_SCANCODE_X, B_B}, {SDL_SCANCODE_C, B_X}, {SDL_SCANCODE_V, B_Y},
        {SDL_SCANCODE_Q, B_L}, {SDL_SCANCODE_R, B_R}, {SDL_SCANCODE_1, B_ZL}, {SDL_SCANCODE_3, B_ZR},
        {SDL_SCANCODE_RETURN, B_PLUS}, {SDL_SCANCODE_BACKSPACE, B_MINUS},
        {SDL_SCANCODE_LEFT, B_LEFT}, {SDL_SCANCODE_UP, B_UP}, {SDL_SCANCODE_RIGHT, B_RIGHT}, {SDL_SCANCODE_DOWN, B_DOWN},
    };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        if (k[keys[i].sc]) b |= keys[i].bit;
    if (k[SDL_SCANCODE_A]) lx -= 32767;                /* the left stick on WASD, apart from the D-pad */
    if (k[SDL_SCANCODE_D]) lx += 32767;
    if (k[SDL_SCANCODE_W]) ly += 32767;
    if (k[SDL_SCANCODE_S]) ly -= 32767;
    if (k[SDL_SCANCODE_J]) rx -= 32767;
    if (k[SDL_SCANCODE_L]) rx += 32767;
    if (k[SDL_SCANCODE_I]) ry += 32767;
    if (k[SDL_SCANCODE_K]) ry -= 32767;
    if (pad) {
        static const struct { SDL_GamepadButton gb; u64 bit; } map[] = {
            {SDL_GAMEPAD_BUTTON_SOUTH, B_A}, {SDL_GAMEPAD_BUTTON_EAST, B_B}, {SDL_GAMEPAD_BUTTON_NORTH, B_X},
            {SDL_GAMEPAD_BUTTON_WEST, B_Y}, {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, B_L},
            {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, B_R}, {SDL_GAMEPAD_BUTTON_START, B_PLUS},
            {SDL_GAMEPAD_BUTTON_BACK, B_MINUS}, {SDL_GAMEPAD_BUTTON_LEFT_STICK, B_LSTICK},
            {SDL_GAMEPAD_BUTTON_RIGHT_STICK, B_RSTICK}, {SDL_GAMEPAD_BUTTON_DPAD_LEFT, B_LEFT},
            {SDL_GAMEPAD_BUTTON_DPAD_UP, B_UP}, {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, B_RIGHT},
            {SDL_GAMEPAD_BUTTON_DPAD_DOWN, B_DOWN},
        };
        for (size_t i = 0; i < sizeof map / sizeof *map; i++)
            if (SDL_GetGamepadButton(pad, map[i].gb)) b |= map[i].bit;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000) b |= B_ZL;
        if (SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000) b |= B_ZR;
        s32 ax = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), ay = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        s32 bx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), by = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
        if (ax * ax + ay * ay > 6000 * 6000) { lx = axis(ax); ly = axis(-ay); }      /* SDL's y points down */
        if (bx * bx + by * by > 6000 * 6000) { rx = axis(bx); ry = axis(-by); }
    }
    {                                                   /* the mouse's speed (pixels a second), smoothed over ~50 ms */
        static float vx, vy, sens;
        static u64 last;
        if (!sens) sens = getenv("NX_MOUSE_SENS") ? (float)atof(getenv("NX_MOUSE_SENS")) : 1.0f;
        if (sens <= 0) sens = 1.0f;
        u64 now = SDL_GetTicksNS();
        float dt = last ? (float)(now - last) * 1e-9f : 0.008f;
        last = now;
        if (dt > 0.1f) dt = 0.1f;
        if (dt > 0) {
            float a = 1.0f - SDL_expf(-dt / 0.05f);
            vx += (mouse_dx / dt - vx) * a;
            vy += (mouse_dy / dt - vy) * a;
        }
        mouse_dx = mouse_dy = 0;
        if (!mouse_captured) vx = vy = 0;
        float mx = vx * sens * (32767.0f / 2000.0f), my = -vy * sens * (32767.0f / 2000.0f);   /* y points down */
        if (vx * vx + vy * vy > 150.0f * 150.0f) {
            rx = mx > 32767 ? 32767 : mx < -32767 ? -32767 : (s32)mx;
            ry = my > 32767 ? 32767 : my < -32767 ? -32767 : (s32)my;
        }
    }
    u64 pb;
    s32 plx, ply, prx, pry;
    if (replayed(gpu_frames(), &pb, &plx, &ply, &prx, &pry)) {
        b |= pb;
        if (plx || ply) { lx = plx; ly = ply; }
        if (prx || pry) { rx = prx; ry = pry; }
    }
    u64 sb = scripted(gpu_frames());                    /* scripted stick directions move the sticks fully */
    if (sb & B_LS_LEFT) lx = -32767;
    if (sb & B_LS_RIGHT) lx = 32767;
    if (sb & B_LS_UP) ly = 32767;
    if (sb & B_LS_DOWN) ly = -32767;
    if (sb & B_RS_LEFT) rx = -32767;
    if (sb & B_RS_RIGHT) rx = 32767;
    if (sb & B_RS_UP) ry = 32767;
    if (sb & B_RS_DOWN) ry = -32767;
    b |= sb;
    /* stick directions as buttons, as the console reports them */
    if (lx < -16000) b |= B_LS_LEFT;
    if (lx > 16000) b |= B_LS_RIGHT;
    if (ly > 16000) b |= B_LS_UP;
    if (ly < -16000) b |= B_LS_DOWN;
    if (rx < -16000) b |= B_RS_LEFT;
    if (rx > 16000) b |= B_RS_RIGHT;
    if (ry > 16000) b |= B_RS_UP;
    if (ry < -16000) b |= B_RS_DOWN;
    /* NX_INPUT_LOG: every change of what the game is given (input that seems to come from nowhere) */
    static int log_input = -1;
    static u64 lb;
    static s32 llx, lly, lrx, lry;
    if (log_input < 0) log_input = getenv("NX_INPUT_LOG") != NULL;
    if (log_input && (b != lb || lx != llx || ly != lly || rx != lrx || ry != lry)) {
        hlog(0, "input: frame %llu buttons %llx left %d,%d right %d,%d", gpu_frames(), b, lx, ly, rx, ry);
        lb = b; llx = lx; lly = ly; lrx = rx; lry = ry;
    }
    hid_set_input(0, b, lx, ly, rx, ry);
}
