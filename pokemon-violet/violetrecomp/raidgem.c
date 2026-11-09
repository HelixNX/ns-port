/* Tera raid crystals rebuilt on demand (the raids mod's F6, through the export violet_raid_refresh): seeds written
 * into the dens otherwise showed only after a restart.
 *
 * The crystals belong to a manager object M (4.0.0; found 2026-11-07, Ghidra on main):
 *   M + 0x2d       300 x 6 bytes {i32 -1, u8 0, u8}: per-den state, reset only by the new-day handler (01ea82c0,
 *                  which then posts "RaidGemForceLottery", the reroll); meaning not known yet (logged below)
 *   M + 0x738..750 the area key of crystal group 0..3 (29337b4 registers an area's spawn points into a group;
 *                  29341d8(M, key) drops a group)
 *   M + 0x7e8      23 vectors {begin, end, ...} (0x38 bytes each) of 0xa0-byte crystal entries: +0x10 group,
 *                  +0x18 the spawn point's object, +0x70 the crystal object (+0xd1: rebuild it), +0x98 placed
 *   M + 0x1662     "reflect" flag: the per-frame update (2933374(M, dt)) posts "RaidGemForceReflection" (FNV-1a
 *                  event on M+8, 01ea821c) at its end and clears it
 * Every game path that refreshes one crystal (29341d8, 29343a0, 01ea82c0) does entry+0x98 = 0, crystal+0xd1 = 1,
 * M+0x1662 = 1. A refresh here does that for every registered entry, keeping the groups (no reroll: the dens'
 * seeds stay as written). Hooked after `mov x19, x0` in the update (main+2933398), on the game's thread. */
#include "hos.h"
#include <stdio.h>
#include <string.h>
#include <windows.h>

#define GEM_CACHE 0x2d
#define GEM_CACHE_N 300
#define GEM_GROUPS 0x738
#define GEM_VECS 0x7e8
#define GEM_VECS_END 0xcf0
#define GEM_REFLECT 0x1662
#define ENTRY_SIZE 0xa0

static volatile LONG refresh_req;       /* 1 rebuild the crystals, 2 also reset the per-den state at M+0x2d */
static volatile LONG64 manager;
static volatile LONG logged;

/* the mod's side: what = 1 or 2 as above; returns the manager's address (0: no update seen yet: not in the field) */
__declspec(dllexport) unsigned long long violet_raid_refresh(int what) {
    if (what) InterlockedExchange(&refresh_req, what);
    return (unsigned long long)manager;
}

static void log_state(u8 *m, const char *why) {
    char line[512];
    int n = 0;
    for (int g = 0; g < 4; g++) n += snprintf(line + n, sizeof line - n, " %llx", *(unsigned long long *)(m + GEM_GROUPS + 8 * g));
    hlog(0, "raidgem: %s manager %llx, groups%s", why, (unsigned long long)(uintptr_t)m, line);
    n = 0;
    for (int i = 0; i < 16 && n < (int)sizeof line - 32; i++) {
        u8 *e = m + GEM_CACHE + 6 * i;
        n += snprintf(line + n, sizeof line - n, " %d:%02x%02x", *(int *)e, e[4], e[5]);
    }
    hlog(0, "raidgem: den state%s", line);
    int shown = 0, total = 0;
    for (u64 *v = (u64 *)(m + GEM_VECS); (u8 *)v < m + GEM_VECS_END; v += 7)
        for (u64 a = v[0]; a < v[1]; a += ENTRY_SIZE) {
            u8 *e = (u8 *)(uintptr_t)a;
            total++;
            if (shown++ < 6)
                hlog(0, "raidgem:  vec %d entry group %d, 44 %d 48 %d 4c %d 64 %d, placed %d, crystal %llx",
                     (int)((u8 *)v - (m + GEM_VECS)) / 0x38, *(int *)(e + 0x10), *(int *)(e + 0x44), *(int *)(e + 0x48),
                     *(int *)(e + 0x4c), *(int *)(e + 0x64), e[0x98], *(unsigned long long *)(e + 0x70));
        }
    hlog(0, "raidgem: %d crystal entries", total);
}

void h_raidgem_update(Ctx *c) {
    u8 *m = (u8 *)(uintptr_t)c->x[19];
    if ((u64)manager != (u64)(uintptr_t)m) {
        InterlockedExchange64(&manager, (LONG64)(uintptr_t)m);
        if (!InterlockedExchange(&logged, 1)) log_state(m, "first update,");
    }
    LONG what = InterlockedExchange(&refresh_req, 0);
    if (!what) return;
    log_state(m, "refresh,");
    int n = 0;
    for (u64 *v = (u64 *)(m + GEM_VECS); (u8 *)v < m + GEM_VECS_END; v += 7)
        for (u64 a = v[0]; a < v[1]; a += ENTRY_SIZE) {
            u8 *e = (u8 *)(uintptr_t)a;
            u64 gem = *(u64 *)(e + 0x70);
            e[0x98] = 0;
            if (gem) *(u8 *)(uintptr_t)(gem + 0xd1) = 1;
            n++;
        }
    if (what == 2)
        for (int i = 0; i < GEM_CACHE_N; i++) {
            u8 *e = m + GEM_CACHE + 6 * i;
            *(int *)e = -1;
            e[4] = 0;
        }
    m[GEM_REFLECT] = 1;
    hlog(0, "raidgem: %d crystals marked for rebuild%s", n, what == 2 ? ", den state reset" : "");
}
