/* Controls (input.c): what each key and gamepad button does, kept in settings.ini (options.c's store) as
 * key.<action>=<SDL key name> and pad.<action>=<SDL gamepad button name, "lefttrigger", "righttrigger" or "">.
 * The menu (ui.cpp) lists and rebinds them. */
#pragma once
#include "a64rt.h"
#include <SDL3/SDL.h>

enum {
    ACT_A, ACT_B, ACT_X, ACT_Y, ACT_L, ACT_R, ACT_ZL, ACT_ZR, ACT_PLUS, ACT_MINUS, ACT_LSTICK, ACT_RSTICK,
    ACT_DLEFT, ACT_DUP, ACT_DRIGHT, ACT_DDOWN,
    ACT_LS_LEFT, ACT_LS_UP, ACT_LS_RIGHT, ACT_LS_DOWN, ACT_RS_LEFT, ACT_RS_UP, ACT_RS_RIGHT, ACT_RS_DOWN,
    ACT_COUNT,
    ACT_PAD_COUNT = ACT_LS_LEFT,    /* the gamepad's sticks move the sticks: only buttons are rebound */
};
#define PAD_LEFT_TRIGGER  (SDL_GAMEPAD_BUTTON_COUNT + 0)    /* the triggers as buttons (pressed past half) */
#define PAD_RIGHT_TRIGGER (SDL_GAMEPAD_BUTTON_COUNT + 1)
#define PAD_NONE (-1)

const char *input_action_label(int a);          /* "A", "Left stick up", ... */
SDL_Scancode input_key(int a);
void input_set_key(int a, SDL_Scancode k);      /* SDL_SCANCODE_UNKNOWN: none */
int input_pad(int a);                           /* SDL_GamepadButton, PAD_*_TRIGGER or PAD_NONE */
void input_set_pad(int a, int b);
const char *input_pad_name(int b);              /* for the menu: "A (bottom)", "Left trigger", ... */
void input_reset_keys(void);
void input_reset_pad(bool nintendo_positions);  /* false: by label (an Xbox pad's A is A); true: by position */
float input_pad_deadzone(void);                 /* 0-0.5 of full tilt */
void input_set_pad_deadzone(float d);
const char *input_pad_connected(void);          /* the gamepad's name, or NULL */

void input_init(void);                          /* window thread, after options_init */
bool input_mouse_captured(void);
void input_capture_mouse(bool on);              /* window thread */
