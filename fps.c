/* Frame rates other than the game's 30 (patches.txt has the hooked instructions; the frame rate itself is the
 * runtime's, framerate.h). Off at the game's own rate, where every hook does what the original did. */
#include "framerate.h"
#include "hos.h"
#include <windows.h>

#define MAIN 0x7100000000ull
#define FRAME_ENTRY (MAIN + 0x3c2fbb0)          /* the 30 fps mode's frame duration, ns (rodata) */
#define NS_30 33333334ull

volatile u8 nx_flag_fps;                        /* patches.txt "if fps": the patched instruction */
static u32 scene_kind;                          /* fps_scene_kind */
static u64 entry_now = NS_30;

/* main+d9643c, once a frame: the speed is (frame duration / (this - 1)), so this stays the 30 fps duration while
 * the frame duration follows the frame rate. Also where the frame duration is set for the frame. */
void h_fps_speed_base(Ctx *c) {
    s32 r = nx_framerate();
    nx_flag_fps = r != FRAMERATE_GAME && r != 30;
    u64 ns = nx_flag_fps ? (u64)nx_frame_ns() : NS_30;
    if (ns != entry_now) {                      /* rodata: made writable once and left so (uncapped writes it every
                                                 * frame; restoring read-only raced another thread's write) */
        static volatile LONG writable;
        DWORD old;
        void *p = (void *)(uintptr_t)FRAME_ENTRY;
        if (writable || VirtualProtect(p, 8, PAGE_READWRITE, &old)) {
            writable = 1;
            *(volatile u64 *)p = ns;
            entry_now = ns;
        } else {
            static bool logged;
            if (!logged) { logged = true; hlog(0, "fps: the frame duration at %llx cannot be written (%lu)", FRAME_ENTRY, GetLastError()); }
        }
    }
    c->x[8] = nx_flag_fps ? NS_30 : *(u64 *)(uintptr_t)(c->x[8] + (c->x[20] << 3));
}

/* main+b92784: a timer's step, the frame duration, kept at 30 fps in scene kind 2 */
void h_fps_timer_step(Ctx *c) {
    c->x[1] = nx_flag_fps && scene_kind == 2 ? NS_30 : *(u64 *)(uintptr_t)(c->x[8] + 8);
}

/* main+2936584 */
void h_fps_scene_kind(Ctx *c) { scene_kind = (u32)c->x[8]; }
