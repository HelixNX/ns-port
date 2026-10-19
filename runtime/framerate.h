/* Frame rate: the game's own or a target of ours (nvn.c paces presents to it; F5 cycles it during play). A rate
 * other than the game's own plays right only when the game's hooks (patches.txt and host code in its repository)
 * step the game by nx_frame_ns() per frame */
#pragma once
#include "a64rt.h"

#define FRAMERATE_GAME (-1)     /* the game's own pace: 60 Hz / its present interval */
#define FRAMERATE_UNCAPPED 0

s32 nx_framerate(void);         /* FRAMERATE_GAME, FRAMERATE_UNCAPPED or frames per second */
void nx_framerate_cycle(void);  /* the game's own, 60, 120, uncapped, the game's own, ... */
double nx_frame_ns(void);       /* the time one frame stands for at the rate set (smoothed when below it) */
double nx_frame_step_ns(void);  /* the game time the last frame stood for (the overlay's tick rate and speed) */
