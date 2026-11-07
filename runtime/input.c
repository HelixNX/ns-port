/* Input: the keyboard and the first gamepad (SDL3) as player 1's Pro Controller (services/hid.c).
 *
 * Bindings (input.h; the menu's Controls page rebinds them, settings.ini keeps them). Default keyboard: WASD = left
 * stick, IJKL = right stick, arrows = D-pad (games give the D-pad its own actions, e.g. Violet's emotes, so it is
 * not also a stick), Z = A, X = B, C = X, V = Y, Q = L, R = R, 1 = ZL, 3 = ZR, Enter = +, Backspace = -.
 * Default gamepad: by label (an Xbox pad's bottom button is A); "by position" puts A on the right as on a Switch.
 * Mouse: the right stick while captured (a click in the game captures it, Esc or losing focus releases it): the
 * stick follows the mouse's smoothed speed, full at 1000 pixels a second times the sensitivity (settings.ini,
 * NX_MOUSE_SENS; default 1); below the resting speed (150 pixels a second: a resting hand on a high-resolution
 * mouse) it stays centred. The game gets no keyboard while the menu (ui.cpp) has keyboard focus.
 * NX_INPUT="first-last:A+B;..." holds buttons over ranges of presented frames (unattended runs);
 * "first-last/n:A" presses A for 3 frames every n frames (mashing through screens). Names: A B X Y
 * L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN (D-pad), LSLEFT LSUP LSRIGHT LSDOWN and RSLEFT ... (sticks)
 * NX_INPUT_LOG logs every change of the input; NX_INPUT_REPLAY=<log file> plays such a log back by frame (a run
 * recorded by hand, replayed unattended), on top of live input. */
#include <math.h>
#include "hos.h"
#include "input.h"
#include "options.h"
#include "mods.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hid_set_input(int npad, u64 buttons, s32 lx, s32 ly, s32 rx, s32 ry);
u64 gpu_frames(void);
bool ui_wants_keyboard(void);

enum {
    B_A = 1 << 0, B_B = 1 << 1, B_X = 1 << 2, B_Y = 1 << 3, B_LSTICK = 1 << 4, B_RSTICK = 1 << 5, B_L = 1 << 6,
    B_R = 1 << 7, B_ZL = 1 << 8, B_ZR = 1 << 9, B_PLUS = 1 << 10, B_MINUS = 1 << 11, B_LEFT = 1 << 12, B_UP = 1 << 13,
    B_RIGHT = 1 << 14, B_DOWN = 1 << 15,
    B_LS_LEFT = 1 << 16, B_LS_UP = 1 << 17, B_LS_RIGHT = 1 << 18, B_LS_DOWN = 1 << 19,
    B_RS_LEFT = 1 << 20, B_RS_UP = 1 << 21, B_RS_RIGHT = 1 << 22, B_RS_DOWN = 1 << 23,
};

/* ---------------------------------------------------------------------------------------------- bindings */
static const u64 act_bit[ACT_COUNT] = {
    B_A, B_B, B_X, B_Y, B_L, B_R, B_ZL, B_ZR, B_PLUS, B_MINUS, B_LSTICK, B_RSTICK, B_LEFT, B_UP, B_RIGHT, B_DOWN,
    B_LS_LEFT, B_LS_UP, B_LS_RIGHT, B_LS_DOWN, B_RS_LEFT, B_RS_UP, B_RS_RIGHT, B_RS_DOWN};
static const char *const act_id[ACT_COUNT] = {
    "a", "b", "x", "y", "l", "r", "zl", "zr", "plus", "minus", "lstick", "rstick", "dleft", "dup", "dright", "ddown",
    "ls_left", "ls_up", "ls_right", "ls_down", "rs_left", "rs_up", "rs_right", "rs_down"};
static const char *const act_label[ACT_COUNT] = {
    "A", "B", "X", "Y", "L", "R", "ZL", "ZR", "+", "-", "L stick click", "R stick click",
    "D-pad left", "D-pad up", "D-pad right", "D-pad down", "L stick left", "L stick up", "L stick right",
    "L stick down", "R stick left", "R stick up", "R stick right", "R stick down"};
static const SDL_Scancode key_def[ACT_COUNT] = {
    SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_V, SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_1,
    SDL_SCANCODE_3, SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE, SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN,
    SDL_SCANCODE_LEFT, SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN,
    SDL_SCANCODE_A, SDL_SCANCODE_W, SDL_SCANCODE_D, SDL_SCANCODE_S, SDL_SCANCODE_J, SDL_SCANCODE_I, SDL_SCANCODE_L,
    SDL_SCANCODE_K};
static const int pad_label[ACT_PAD_COUNT] = {
    SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_NORTH, SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, PAD_LEFT_TRIGGER, PAD_RIGHT_TRIGGER,
    SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK, SDL_GAMEPAD_BUTTON_LEFT_STICK, SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SDL_GAMEPAD_BUTTON_DPAD_DOWN};
static volatile SDL_Scancode keys[ACT_COUNT];
static volatile int pads[ACT_PAD_COUNT];
static volatile float deadzone = 6000.0f / 32767.0f;

const char *input_action_label(int a) { return a >= 0 && a < ACT_COUNT ? act_label[a] : ""; }
SDL_Scancode input_key(int a) { return a >= 0 && a < ACT_COUNT ? keys[a] : SDL_SCANCODE_UNKNOWN; }
void input_set_key(int a, SDL_Scancode k) {
    if (a < 0 || a >= ACT_COUNT) return;
    keys[a] = k;
    char id[32];
    snprintf(id, sizeof id, "key.%s", act_id[a]);
    options_set(id, k == SDL_SCANCODE_UNKNOWN ? "" : SDL_GetScancodeName(k));
}
int input_pad(int a) { return a >= 0 && a < ACT_PAD_COUNT ? pads[a] : PAD_NONE; }
static const char *pad_id(int b) {
    return b == PAD_LEFT_TRIGGER ? "lefttrigger" : b == PAD_RIGHT_TRIGGER ? "righttrigger" :
           b >= 0 ? SDL_GetGamepadStringForButton((SDL_GamepadButton)b) : "";
}
void input_set_pad(int a, int b) {
    if (a < 0 || a >= ACT_PAD_COUNT) return;
    pads[a] = b;
    char id[32];
    snprintf(id, sizeof id, "pad.%s", act_id[a]);
    options_set(id, pad_id(b));
}
const char *input_pad_name(int b) {
    switch (b) {
    case PAD_NONE: return "(none)";
    case PAD_LEFT_TRIGGER: return "L trigger";
    case PAD_RIGHT_TRIGGER: return "R trigger";
    case SDL_GAMEPAD_BUTTON_SOUTH: return "Bottom";
    case SDL_GAMEPAD_BUTTON_EAST: return "Right";
    case SDL_GAMEPAD_BUTTON_WEST: return "Left";
    case SDL_GAMEPAD_BUTTON_NORTH: return "Top";
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return "L shoulder";
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return "R shoulder";
    case SDL_GAMEPAD_BUTTON_START: return "Start";
    case SDL_GAMEPAD_BUTTON_BACK: return "Back";
    case SDL_GAMEPAD_BUTTON_GUIDE: return "Guide";
    case SDL_GAMEPAD_BUTTON_LEFT_STICK: return "L stick click";
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK: return "R stick click";
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return "D-pad left";
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return "D-pad up";
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return "D-pad right";
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return "D-pad down";
    default: { const char *s = SDL_GetGamepadStringForButton((SDL_GamepadButton)b); return s ? s : "?"; }
    }
}
void input_reset_keys(void) { for (int a = 0; a < ACT_COUNT; a++) input_set_key(a, key_def[a]); }
void input_reset_pad(bool nintendo) {
    for (int a = 0; a < ACT_PAD_COUNT; a++) input_set_pad(a, pad_label[a]);
    if (nintendo) {                     /* a Switch's A is on the right, B at the bottom, X on top, Y on the left */
        input_set_pad(ACT_A, SDL_GAMEPAD_BUTTON_EAST);
        input_set_pad(ACT_B, SDL_GAMEPAD_BUTTON_SOUTH);
        input_set_pad(ACT_X, SDL_GAMEPAD_BUTTON_NORTH);
        input_set_pad(ACT_Y, SDL_GAMEPAD_BUTTON_WEST);
    }
}
float input_pad_deadzone(void) { return deadzone; }
void input_set_pad_deadzone(float d) {
    deadzone = d < 0 ? 0 : d > 0.5f ? 0.5f : d;
    options_set_float("pad_deadzone", deadzone);
}

void input_init(void) {
    char id[32];
    for (int a = 0; a < ACT_COUNT; a++) {
        snprintf(id, sizeof id, "key.%s", act_id[a]);
        const char *v = options_get(id, NULL);
        keys[a] = !v ? key_def[a] : !*v ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(v);
    }
    for (int a = 0; a < ACT_PAD_COUNT; a++) {
        snprintf(id, sizeof id, "pad.%s", act_id[a]);
        const char *v = options_get(id, NULL);
        if (!v) pads[a] = pad_label[a];
        else if (!*v) pads[a] = PAD_NONE;
        else if (!strcmp(v, "lefttrigger")) pads[a] = PAD_LEFT_TRIGGER;
        else if (!strcmp(v, "righttrigger")) pads[a] = PAD_RIGHT_TRIGGER;
        else pads[a] = SDL_GetGamepadButtonFromString(v);
    }
    deadzone = options_get_float("pad_deadzone", 6000.0f / 32767.0f);
}

/* ---------------------------------------------------------------------------------------------- devices */
static SDL_Gamepad *pad;
static volatile bool mouse_captured;
static float mouse_dx, mouse_dy;        /* motion since the last poll (window thread: events and polls alike) */
SDL_Window *vk_window(void);

static void mouse_capture(SDL_Window *w, bool on) {
    if (w && SDL_SetWindowRelativeMouseMode(w, on)) mouse_captured = on;
    mouse_dx = mouse_dy = 0;
}

bool input_mouse_captured(void) { return mouse_captured; }
void input_capture_mouse(bool on) { if (on != mouse_captured) mouse_capture(vk_window(), on); }
const char *input_pad_connected(void) { return pad ? SDL_GetGamepadName(pad) : NULL; }

/* mods.c (a mod's host->release_mouse) */
void input_release_mouse(SDL_WindowID id) {
    if (mouse_captured) mouse_capture(SDL_GetWindowFromID(id), false);
}

void input_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_MOUSE_BUTTON_DOWN && !mouse_captured) mouse_capture(SDL_GetWindowFromID(e->button.windowID), true);
    else if (e->type == SDL_EVENT_WINDOW_FOCUS_LOST && mouse_captured) mouse_capture(SDL_GetWindowFromID(e->window.windowID), false);
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
/* a stick's travel is round: keys pressed together (W and A) or a pad reporting a square would go past full tilt
 * on diagonals (by up to 1.41: the player moved that much faster) */
static void round_stick(s32 *x, s32 *y) {
    double m = sqrt((double)*x * *x + (double)*y * *y);
    if (m > 32767) { *x = (s32)(*x * 32767 / m); *y = (s32)(*y * 32767 / m); }
}

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
    bool no_keys = ui_wants_keyboard();                 /* the menu has keyboard focus (or a text box) */
    if (mods_want_input()) {                            /* an older mod's window has the keyboard and mouse */
        mouse_dx = mouse_dy = 0;
        hid_set_input(0, 0, 0, 0, 0, 0);
        return;
    }
    const bool *k = SDL_GetKeyboardState(NULL);
    u64 b = 0;
    s32 lx = 0, ly = 0, rx = 0, ry = 0;
    if (!no_keys) {
        for (int a = 0; a < ACT_LS_LEFT; a++)
            if (keys[a] != SDL_SCANCODE_UNKNOWN && k[keys[a]]) b |= act_bit[a];
        const struct { int a; s32 *v; s32 d; } dirs[] = {
            {ACT_LS_LEFT, &lx, -32767}, {ACT_LS_RIGHT, &lx, 32767}, {ACT_LS_UP, &ly, 32767}, {ACT_LS_DOWN, &ly, -32767},
            {ACT_RS_LEFT, &rx, -32767}, {ACT_RS_RIGHT, &rx, 32767}, {ACT_RS_UP, &ry, 32767}, {ACT_RS_DOWN, &ry, -32767}};
        for (size_t i = 0; i < sizeof dirs / sizeof *dirs; i++)
            if (keys[dirs[i].a] != SDL_SCANCODE_UNKNOWN && k[keys[dirs[i].a]]) *dirs[i].v += dirs[i].d;
    }
    if (pad) {
        for (int a = 0; a < ACT_PAD_COUNT; a++) {
            int pb = pads[a];
            bool on = pb == PAD_LEFT_TRIGGER ? SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 16000 :
                      pb == PAD_RIGHT_TRIGGER ? SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 16000 :
                      pb >= 0 && SDL_GetGamepadButton(pad, (SDL_GamepadButton)pb);
            if (on) b |= act_bit[a];
        }
        s32 ax = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX), ay = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY);
        s32 bx = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTX), by = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY);
        double dz = deadzone * 32767.0, dz2 = dz * dz;
        if ((double)ax * ax + (double)ay * ay > dz2) { lx = axis(ax); ly = axis(-ay); }      /* SDL's y points down */
        if ((double)bx * bx + (double)by * by > dz2) { rx = axis(bx); ry = axis(-by); }
    }
    {                                                   /* the mouse's speed (pixels a second), smoothed over ~50 ms */
        static float vx, vy;
        static u64 last;
        float sens = options_mouse_sens(), rest = options_mouse_rest();
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
        float mx = vx * sens * (32767.0f / 1000.0f), my = -vy * sens * (32767.0f / 1000.0f);   /* y points down */
        if (options_mouse_invert_y()) my = -my;
        if (vx * vx + vy * vy > rest * rest) {
            rx = mx > 32767 ? 32767 : mx < -32767 ? -32767 : (s32)mx;
            ry = my > 32767 ? 32767 : my < -32767 ? -32767 : (s32)my;
        }
    }
    u64 pb;
    round_stick(&lx, &ly);                              /* before replays: logs made before keep their values */
    round_stick(&rx, &ry);
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
