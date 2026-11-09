/* Mods' host->run_script (nxmod.h) for Violet: source chunks run in the game's own Lua 5.4 VM.
 *
 * The game logic is Haxe compiled to Lua 5.4 (romfs script/lua/bin/release/main/main.blua, names obfuscated:
 * classes C<hash> are globals, statics S<hash>, methods F<hash>). Native code enters the VM through lua_pcallk
 * (~300 callers); patches.txt hooks its entry ("after" its first instruction, sub sp), where the caller's lua_State
 * is in x0 and the VM is between calls: a safe point to load and run a chunk of our own on top of the caller's
 * stack, with everything put back after (the stack top, every register).
 *
 * Main's Lua C API (4.0.0), found from luaL_loadfilex ("=stdin"):
 *   lua_load 232e0 (L, reader, data, chunkname, mode), getS 24b90 (lauxlib's string
 *   reader: data = {const char *s; size_t size}), lua_gettop b5d980, lua_settop b5db00, lua_tolstring b646d0,
 *   lua_pcallk b5ddd0 (L, nargs, nresults, errfunc, ctx, k).
 *
 * A chunk that returns NXMOD_SCRIPT_AGAIN ("\1again", e.g. a global it needs is not there: another VM, or the save
 * not loaded yet) is run again at a later safe point, up to AGAIN_MAX times, then reported as failed. */
#include "hos.h"
#include "mods.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define MAIN 0x7100000000ull
#define LUA_LOAD (MAIN + 0x232e0)
#define LUA_GETS (MAIN + 0x24b90)
#define LUA_GETTOP (MAIN + 0xb5d980)
#define LUA_SETTOP (MAIN + 0xb5db00)
#define LUA_TOLSTRING (MAIN + 0xb646d0)
#define LUA_PCALLK (MAIN + 0xb5ddd0)

#define AGAIN NXMOD_SCRIPT_AGAIN
#define AGAIN_MAX 2000
#define MAX_JOBS 64

typedef struct Job { char *text; NxScriptDone done; void *ud; int tries; } Job;

static SRWLOCK lock = SRWLOCK_INIT;
static Job jobs[MAX_JOBS];
static int njobs;
static volatile LONG pending;                   /* njobs, read without the lock by the hook */
static __declspec(thread) int busy;             /* our own lua_pcallk enters the hook too */

static bool queue(const char *text, NxScriptDone done, void *ud) {
    char *t = _strdup(text);
    if (!t) return false;
    AcquireSRWLockExclusive(&lock);
    bool ok = njobs < MAX_JOBS;
    if (ok) { jobs[njobs++] = (Job){t, done, ud, 0}; pending = njobs; }
    ReleaseSRWLockExclusive(&lock);
    if (!ok) { free(t); hlog(0, "lua: %d chunks already queued, one dropped", MAX_JOBS); }
    return ok;
}

__attribute__((constructor)) static void lua_bridge_init(void) { mods_set_script_runner(queue); }

static u64 gcall(Ctx *c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5) {
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->x[4] = a4; c->x[5] = a5;
    guest_call(c, fn);
    return c->x[0];
}

/* one chunk on L: true when it ran to the end (ok: without an error); out gets its first result or the error */
static bool run_one(Ctx *c, u64 L, const char *text, char *out, size_t outsz) {
    struct { const char *s; size_t size; } ls = {text, strlen(text)};
    static const char name[] = "=mod";
    s32 top = (s32)gcall(c, LUA_GETTOP, L, 0, 0, 0, 0, 0);
    s32 st = (s32)gcall(c, LUA_LOAD, L, LUA_GETS, (u64)(uintptr_t)&ls, (u64)(uintptr_t)name, 0, 0);
    if (st == 0) st = (s32)gcall(c, LUA_PCALLK, L, 0, 1, 0, 0, 0);
    u64 len = 0;
    u64 s = gcall(c, LUA_TOLSTRING, L, (u64)-1, (u64)(uintptr_t)&len, 0, 0, 0);
    if (s) {
        size_t n = len < outsz - 1 ? (size_t)len : outsz - 1;
        memcpy(out, (const void *)(uintptr_t)s, n);
        out[n] = 0;
    } else snprintf(out, outsz, st ? "(error without a message)" : "(no text result)");
    gcall(c, LUA_SETTOP, L, (u64)(s64)top, 0, 0, 0, 0);
    return st == 0;
}

/* main+b5ddd0, after sub sp, #0x40 */
void h_lua_pcall_entry(Ctx *c) {
    if (!pending || busy) return;
    Job mine[MAX_JOBS];
    AcquireSRWLockExclusive(&lock);
    int n = njobs;
    memcpy(mine, jobs, n * sizeof *jobs);
    njobs = 0;
    pending = 0;
    ReleaseSRWLockExclusive(&lock);

    busy = 1;
    Ctx saved = *c;
    u64 L = c->x[0];
    static bool logged;
    if (!logged) {
        logged = true;
        hlog(1, "lua: first chunk on thread %s, lua_State %llx (global state %llx)",
             cur_thread ? cur_thread->name : "?", L, *(u64 *)(uintptr_t)(L + 0x18));
    }
    char out[4096];
    for (int i = 0; i < n; i++) {
        Job *j = &mine[i];
        bool ok = run_one(c, L, j->text, out, sizeof out);
        *c = saved;
        if (ok && !strcmp(out, AGAIN)) {
            if (++j->tries < AGAIN_MAX) {
                AcquireSRWLockExclusive(&lock);
                if (njobs < MAX_JOBS) { jobs[njobs++] = *j; pending = njobs; j = NULL; }
                ReleaseSRWLockExclusive(&lock);
                if (!j) continue;
            }
            ok = false;
            snprintf(out, sizeof out, "gave up: the chunk asked to be run again %d times", j->tries);
        }
        hlog(ok ? 1 : 0, "lua: %s: %s", ok ? "ok" : "error", out);
        if (j->done) j->done(j->ud, ok, out);
        free(j->text);
    }
    busy = 0;
}
