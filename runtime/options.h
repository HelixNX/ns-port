/* The options panel (options.c): Esc over the game; settings.ini in the cache folder */
#pragma once
#include "a64rt.h"
#include <SDL3/SDL.h>

#define OPT_W 440                   /* the panel's size in pixels (vk.c draws it centred) */
#define OPT_H 200

void options_init(void);            /* settings.ini: before the first frame */
bool options_event(const SDL_Event *e);     /* true: the panel used the event (the game does not see it) */
bool options_is_open(void);         /* the game gets no input while it is */
float options_mouse_sens(void);     /* input.c: the mouse's right-stick scale */
void options_draw(u32 *px);         /* OPT_W x OPT_H pixels (8-bit BGRA or RGBA: grey, white and green only) */
