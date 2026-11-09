/* Wild spawns made to order (the raids mod's F8, "Wild spawns"): every Pokemon param the game creates goes through
 * PokemonParam::Create (main+debe4c: x0 out, x1 heap, x2 the InitialSpec), the wild spawner's too. While the mod has
 * a spec set (violet_wild_set, found by the mod with GetProcAddress on the exe), a create whose spec is the chosen
 * species and form gets the mod's shiny / IVs / EVs / nature / ability / gender / level written into the spec before
 * the constructor reads it, then moves and Tera type set on the new param (Create's end).
 *
 * InitialSpec (4.0.0; from SetupInitialSpec main+160ffd0, the PokeData path, and its caller 160df74's defaults):
 *   0x00 u64 rand seed, 0x08 u8 seed set, 0x10 u64 personal rand (-1 random), 0x18 u64 shiny: high dword 1 never,
 *   2 always, 3 random (low dword -1), 0x20 u64 trainer id (-1), 0x28 u16 DevID, 0x2a u16 form, 0x2c u16 item,
 *   0x2e u16 level, 0x30 u16 sex (0 male, 1 female, 0xff random), 0x32 u16 nature (0-24, 0xffff random), 0x34 u16
 *   stat nature, 0x36 u8 ability (0 / 1 / 2 = slot 1 / 2 / hidden, 0xff random 1-2, 0xfe random 1-2-H), 0x37 u8
 *   shiny rolls, 0x38 u16 IVs hp atk def spa spd spe (0xffff random), 0x44 u16 EVs (same order), 0x50 u32
 *   friendship, 0x54 u8 guaranteed perfect IVs, 0x56 / 0x58 / 0x5a u16 weight / height / scale (0x100 random).
 * Param + 0x70 is the core param; setters (from the PokeData path's move setup 1d22f60 and gemType): move
 *   dee5f0(core, slot, move), PP ups dee290(core, slot, n), PP dedf30(core, slot, pp) with max PP bf5fe0(move, 0),
 *   Tera type e8921c(core, type 0-17).
 * Hooked in the constructor (debf7c(obj, spec), which hands the spec to dec698 first), not in Create: Create
 * (debe4c) is one of its callers; 1245474 is another, an inlined Create on a global heap. main+10cd434 (spawn-object-
 * like fields +0x120 DevID .. +0x130 level) is NOT the wild spawner: it makes the same ~26 mons at every save load.
 * NX_WILD_LOG=1: log every create (DevID, form, level, shiny mode, five return addresses up the frame chain). */
#include "hos.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define MAIN 0x7100000000ull
#define SET_MOVE (MAIN + 0xdee5f0)
#define SET_PP_UPS (MAIN + 0xdee290)
#define SET_PP (MAIN + 0xdedf30)
#define MAX_PP (MAIN + 0xbf5fe0)
#define SET_TERA (MAIN + 0xe8921c)

/* the mod's side (mods/raids/mons.cpp has the same layout) */
typedef struct VioletWild {
    u32 size;                   /* sizeof(VioletWild) */
    u16 dev, form;              /* the species to change (internal number, form) */
    u16 level;                  /* 0: as spawned */
    u8 shiny;                   /* 0 as rolled, 1 never, 2 always */
    u8 sex;                     /* 0xff as rolled, 0 male, 1 female */
    u16 nature;                 /* 0xffff as rolled, else 0-24 */
    u8 ability;                 /* 0xff as rolled, else 0 / 1 / 2 (hidden) */
    s8 tera;                    /* -1 as rolled, else 0-17 */
    u8 iv[6];                   /* hp atk def spa spd spe; 0xff as rolled */
    u8 ev[6];
    u16 moves[4];               /* all 0: the species' level-up moves */
    u32 swap_pct;               /* wild spawns turned into this species: 0 none, 100 all */
    u16 item;                   /* held item, 0 as rolled */
    u16 pad;
} VioletWild;

static SRWLOCK lock = SRWLOCK_INIT;
static VioletWild cur;
static volatile LONG active;
static int log_all = -1;
static __declspec(thread) int post_pending;     /* this thread's create was changed: moves / Tera at its end */
static __declspec(thread) VioletWild post;
static __declspec(thread) int busy;

/* NULL: off */
__declspec(dllexport) void violet_wild_set(const VioletWild *w) {
    AcquireSRWLockExclusive(&lock);
    if (w && w->size == sizeof *w) { cur = *w; active = 1; }
    else active = 0;
    ReleaseSRWLockExclusive(&lock);
    if (w && w->size != sizeof *w) hlog(0, "wild: the mod's spec is %u bytes, ours %zu: off", w->size, sizeof *w);
    else hlog(0, w ? "wild: DevID %u form %u, level %u, shiny %u, nature %d, ability %d" : "wild: off",
              w ? w->dev : 0, w ? w->form : 0, w ? w->level : 0, w ? w->shiny : 0,
              w ? (int)(s16)w->nature : 0, w ? (int)(s8)w->ability : 0);
}

/* the return addresses of the frames above (frame pointer chain), as main offsets, into buf */
static void chain(Ctx *c, char *buf, size_t n) {
    u64 fp = c->x[29];
    size_t at = 0;
    buf[0] = 0;
    for (int i = 0; i < 5 && fp && at < n; i++) {
        u64 lr = *(u64 *)(uintptr_t)(fp + 8);
        at += (size_t)snprintf(buf + at, n - at, " %llx", lr >= MAIN && lr < MAIN + 0x4400000 ? lr - MAIN : lr);
        fp = *(u64 *)(uintptr_t)fp;
    }
}

/* main+debf88 (mov x19, x0): the param constructor's entry (x0 the object, x1 the spec), after its prologue */
void h_wild_create(Ctx *c) {
    if (busy) return;
    u8 *s = (u8 *)(uintptr_t)c->x[1];
    if (log_all < 0) { const char *e = getenv("NX_WILD_LOG"); log_all = e && *e == '1'; }
    if (log_all && s) {
        char ch[128];
        chain(c, ch, sizeof ch);
        hlog(0, "wild: create DevID %u form %u level %u shiny mode %u (callers%s)", *(u16 *)(s + 0x28),
             *(u16 *)(s + 0x2a), *(u16 *)(s + 0x2e), *(u32 *)(s + 0x1c), ch);
    }
    post_pending = 0;
    if (!active || !s) return;
    VioletWild w;
    AcquireSRWLockShared(&lock);
    w = cur;
    int on = active;
    ReleaseSRWLockShared(&lock);
    if (!on || *(u16 *)(s + 0x28) != w.dev || *(u16 *)(s + 0x2a) != w.form) return;
    if (w.level) *(u16 *)(s + 0x2e) = w.level;
    if (w.shiny) *(u64 *)(s + 0x18) = (u64)w.shiny << 32 | 0xffffffffu;
    if (w.sex != 0xff) *(u16 *)(s + 0x30) = w.sex;
    if (w.nature != 0xffff) *(u16 *)(s + 0x32) = *(u16 *)(s + 0x34) = w.nature;
    if (w.ability != 0xff) s[0x36] = w.ability;
    if (w.item) *(u16 *)(s + 0x2c) = w.item;
    int fixed = 0;
    for (int i = 0; i < 6; i++) {
        if (w.iv[i] != 0xff) { *(u16 *)(s + 0x38 + 2 * i) = w.iv[i]; fixed++; }
        *(u16 *)(s + 0x44 + 2 * i) = w.ev[i];
    }
    if (fixed) s[0x54] = 0;                     /* no guaranteed perfect IVs over the ones asked for */
    post = w;
    post_pending = 1;
    hlog(1, "wild: changed a create of DevID %u form %u", w.dev, w.form);
}

static u64 gcall(Ctx *c, u64 fn, u64 a0, u64 a1, u64 a2) {
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2;
    guest_call(c, fn);
    return c->x[0];
}

/* main+debff8 (mov x0, x19): the constructor about to finish, x19 the new param */
void h_wild_created(Ctx *c) {
    if (!post_pending || busy) return;
    post_pending = 0;
    u64 param = c->x[19];
    if (!param) return;
    u64 core = *(u64 *)(uintptr_t)(param + 0x70);
    if (!core) return;
    busy = 1;
    Ctx saved = *c;
    int any = 0;
    for (int i = 0; i < 4; i++) any |= post.moves[i] != 0;
    if (any)
        for (int i = 0; i < 4; i++) {
            u16 m = post.moves[i];
            u64 pp = m ? (u8)gcall(c, MAX_PP, m, 0, 0) : 0;
            gcall(c, SET_MOVE, core, (u64)i, m);
            gcall(c, SET_PP_UPS, core, (u64)i, 0);
            gcall(c, SET_PP, core, (u64)i, pp);
        }
    if (post.tera >= 0 && post.tera < 18) gcall(c, SET_TERA, core, (u64)(u8)post.tera, 0);
    *c = saved;
    busy = 0;
}

/* Dev toggle "wild Pokemon don't start battles" (the raids mod's F8, through the export violet_no_battles): the mons'
 * AI turns "touching the player starts a battle" on and off with SetEnableEncount / SetEnableEncountBlackboard
 * behaviour-tree nodes (type names at main+2995db0 / 29954e0, vtables at main+4474d48 / 4474a00), which call
 * main+1d0e5d4(component, enable): it stores the flag as a byte (1 / 0) in the component's encounter object, then
 * notifies it. Hooked after `mov w19, w1` (main+1d0e5e4), where w19 is the enable bit the rest of the function uses:
 * while the toggle is on every call disables. Mons whose encounter was enabled before the toggle keep it until
 * their AI sets it again (or they despawn). Not yet confirmed in game that this flag gates contact battles. */
static volatile LONG no_battles;

__declspec(dllexport) void violet_no_battles(int on) {
    InterlockedExchange(&no_battles, on ? 1 : 0);
    hlog(0, "wild: contact battles %s", on ? "off (dev toggle)" : "back on");
}

void h_encount_enable(Ctx *c) {
    if (no_battles) c->x[19] = 0;
}
