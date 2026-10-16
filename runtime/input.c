/* Input: the keyboard and the first gamepad (SDL3) as player 1's Pro Controller (services/hid.c).
 *
 * Keyboard: arrows = left stick and D-pad, Z = A, X = B, C = X, V = Y, Q = L, E = R, 1 = ZL, 3 = ZR,
 * Enter = +, Backspace = -, IJKL = right stick. Gamepads map by label (an Xbox pad's bottom button is A).
 * MK8_INPUT="first-last:A+B;..." holds buttons over ranges of presented frames (unattended runs). Names: A B X Y
 * L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN. */
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

void input_event(const SDL_Event *e) {
    if (e->type == SDL_EVENT_GAMEPAD_ADDED && !pad) {
        pad = SDL_OpenGamepad(e->gdevice.which);
        if (pad) hlog(1, "input: gamepad %s", SDL_GetGamepadName(pad));
    } else if (e->type == SDL_EVENT_GAMEPAD_REMOVED && pad && SDL_GetGamepadID(pad) == e->gdevice.which) {
        SDL_CloseGamepad(pad);
        pad = NULL;
    }
}

static s32 axis(s32 v) { return v < -32767 ? -32767 : v; }

/* MK8_INPUT */
static u64 scripted(u64 frame) {
    static char *script;
    static bool init;
    if (!init) {
        init = true;
        const char *e = getenv("MK8_INPUT");
        if (e) script = _strdup(e);
    }
    if (!script) return 0;
    static const struct { const char *n; u64 bit; } names[] = {
        {"A", B_A}, {"B", B_B}, {"X", B_X}, {"Y", B_Y}, {"L", B_L}, {"R", B_R}, {"ZL", B_ZL}, {"ZR", B_ZR},
        {"PLUS", B_PLUS}, {"MINUS", B_MINUS}, {"LEFT", B_LEFT}, {"UP", B_UP}, {"RIGHT", B_RIGHT}, {"DOWN", B_DOWN}};
    u64 b = 0;
    const char *p = script;
    while (*p) {
        unsigned long long lo = strtoull(p, (char **)&p, 10), hi = lo;
        if (*p == '-') hi = strtoull(p + 1, (char **)&p, 10);
        if (*p == ':') p++;
        while (*p && *p != ';') {
            const char *q = p;
            while (*q && *q != '+' && *q != ';') q++;
            for (size_t i = 0; i < sizeof names / sizeof *names; i++)
                if (strlen(names[i].n) == (size_t)(q - p) && !strncmp(names[i].n, p, q - p) && frame >= lo && frame <= hi)
                    b |= names[i].bit;
            p = *q == '+' ? q + 1 : q;
        }
        if (*p == ';') p++;
    }
    return b;
}

void input_poll(void) {
    const bool *k = SDL_GetKeyboardState(NULL);
    u64 b = 0;
    s32 lx = 0, ly = 0, rx = 0, ry = 0;
    static const struct { SDL_Scancode sc; u64 bit; } keys[] = {
        {SDL_SCANCODE_Z, B_A}, {SDL_SCANCODE_X, B_B}, {SDL_SCANCODE_C, B_X}, {SDL_SCANCODE_V, B_Y},
        {SDL_SCANCODE_Q, B_L}, {SDL_SCANCODE_E, B_R}, {SDL_SCANCODE_1, B_ZL}, {SDL_SCANCODE_3, B_ZR},
        {SDL_SCANCODE_RETURN, B_PLUS}, {SDL_SCANCODE_BACKSPACE, B_MINUS},
        {SDL_SCANCODE_LEFT, B_LEFT}, {SDL_SCANCODE_UP, B_UP}, {SDL_SCANCODE_RIGHT, B_RIGHT}, {SDL_SCANCODE_DOWN, B_DOWN},
    };
    for (size_t i = 0; i < sizeof keys / sizeof *keys; i++)
        if (k[keys[i].sc]) b |= keys[i].bit;
    if (k[SDL_SCANCODE_LEFT]) lx -= 32767;
    if (k[SDL_SCANCODE_RIGHT]) lx += 32767;
    if (k[SDL_SCANCODE_UP]) ly += 32767;
    if (k[SDL_SCANCODE_DOWN]) ly -= 32767;
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
    b |= scripted(gpu_frames());
    /* stick directions as buttons, as the console reports them */
    if (lx < -16000) b |= B_LS_LEFT;
    if (lx > 16000) b |= B_LS_RIGHT;
    if (ly > 16000) b |= B_LS_UP;
    if (ly < -16000) b |= B_LS_DOWN;
    if (rx < -16000) b |= B_RS_LEFT;
    if (rx > 16000) b |= B_RS_RIGHT;
    if (ry > 16000) b |= B_RS_UP;
    if (ry < -16000) b |= B_RS_DOWN;
    hid_set_input(0, b, lx, ly, rx, ry);
}
