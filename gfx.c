/* Graphics quality patches (patches.txt "if gfx" / "if shadow" / "if nodof": Fl4sh's Violet 4.0.0 "Improved Graphic
 * Quality" and "2K Shadows" pchtxt mods, as a map), each with a switch read once before the game starts:
 *   NX_QUALITY=0    the two quality values as the game has them ("if gfx")
 *   NX_SHADOWS2K=1  2048x2048 shadow maps ("if shadow"): OFF by default, opening the map crashes with them
 *                   (null read at main+c281d0, 2026-11-05; not looked into)
 *   NX_DOF=1        keep the depth-of-field blur ("if nodof")
 *   NX_GFX=0        all of them off: the game's own settings */
#include "hos.h"
#include <stdlib.h>

volatile u8 nx_flag_gfx = 1;
volatile u8 nx_flag_shadow = 0;
volatile u8 nx_flag_nodof = 1;

static bool env_is(const char *name, char v) {
    const char *e = getenv(name);
    return e && *e == v;
}

__attribute__((constructor)) static void gfx_init(void) {
    if (env_is("NX_QUALITY", '0')) nx_flag_gfx = 0;
    if (env_is("NX_SHADOWS2K", '1')) nx_flag_shadow = 1;
    if (env_is("NX_DOF", '1')) nx_flag_nodof = 0;
    if (env_is("NX_GFX", '0')) nx_flag_gfx = nx_flag_shadow = nx_flag_nodof = 0;
}
