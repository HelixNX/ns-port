/* Kernel objects, handles, threads and synchronisation.
 *
 * Every guest thread is a host thread running lifted code on its own host stack (the guest stack is guest memory,
 * in the Ctx's SP). One kernel lock (klock) protects all kernel state, like the console kernel's scheduler lock;
 * a thread that blocks sleeps on its own condition variable with klock released, and whoever ends the wait sets
 * `woken` and `wait_result` and wakes it.
 *
 * Waiter lists for mutexes, condition variables and the address arbiter are not kept: the few dozen threads are
 * scanned for the waiting ones (highest priority first, then first come).
 *
 * Cores: the console gives the game three cores and runs one thread at a time on each, the highest-priority ready
 * one, which keeps its core until it waits or a higher-priority thread for that core becomes ready (equal
 * priorities take turns only when they yield). Games rely on it, e.g. with per-core scratch data indexed by
 * GetCurrentProcessorNumber. So a guest thread runs only while it owns a core from its affinity mask: it gives the
 * core up whenever it waits in the kernel (and during IPC) and takes one back afterwards.
 * Preemption: a thread that becomes ready while every core it may use is owned by a lower-priority thread takes the
 * core of the lowest-priority owner. If that owner is in lifted code it is suspended there on the spot (lifted code
 * never holds host locks); otherwise it is asked to give the core up at the end of its current SVC. */
#include "hos.h"
#include <stdlib.h>
#include <windows.h>
#include <process.h>

static SRWLOCK klk = SRWLOCK_INIT;

/* Threads to wake are woken once klk is released: woken under it, a thread only wakes to wait for klk. A spurious
 * or late wake is harmless (every sleeper re-checks what it waits for under klk). */
#define MAX_WAKES 64
static __declspec(thread) CONDITION_VARIABLE *wakes[MAX_WAKES];
static __declspec(thread) int nwakes;

static void wake_later(KThread *t) {
    if (nwakes < MAX_WAKES) wakes[nwakes++] = (CONDITION_VARIABLE *)t->cv;
    else WakeConditionVariable((CONDITION_VARIABLE *)t->cv);
}

static void wake_flush(void) {
    for (int i = 0; i < nwakes; i++) WakeConditionVariable(wakes[i]);
    nwakes = 0;
}

void klock(void) { AcquireSRWLockExclusive(&klk); }
void kunlock(void) {
    ReleaseSRWLockExclusive(&klk);
    wake_flush();
}

/* sleep on t's condition variable, klk released meanwhile; the wakes queued so far go first (still under klk,
 * but it is released right after) */
static void ksleep(KThread *t, DWORD ms) {
    wake_flush();
    SleepConditionVariableSRW((CONDITION_VARIABLE *)t->cv, &klk, ms, 0);
}

__declspec(thread) KThread *cur_thread;
static KThread *threads;            /* all threads, under klock */
static u64 next_thread_id = 0x50;   /* thread ids as the guest sees them */
static u64 wait_seq;
static const KThread *waitprof_thread;    /* NX_PROFILE's thread (see waitprof_add) */

/* ---------------------------------------------------------------------------------------------- objects */
void kobj_init(KObj *o, KType type, void (*destroy)(KObj *), bool (*signaled)(KObj *)) {
    memset(o, 0, sizeof *o);
    o->type = type;
    o->refs = 1;
    o->destroy = destroy;
    o->signaled = signaled;
}

void kobj_ref(KObj *o) { InterlockedIncrement(&o->refs); }

void kobj_unref(KObj *o) {
    if (InterlockedDecrement(&o->refs) == 0) {
        free(o->waiters);
        if (o->destroy) o->destroy(o); else free(o);
    }
}

static void waiter_add(KObj *o, KThread *t) {
    if (o->nwaiters == o->capwaiters) {
        o->capwaiters = o->capwaiters ? o->capwaiters * 2 : 4;
        o->waiters = realloc(o->waiters, o->capwaiters * sizeof *o->waiters);
    }
    o->waiters[o->nwaiters++] = t;
}

static void waiter_remove(KObj *o, KThread *t) {
    for (int i = 0; i < o->nwaiters; i++)
        if (o->waiters[i] == t) {
            o->waiters[i] = o->waiters[--o->nwaiters];
            return;
        }
}

static void end_wait(KThread *t, u32 result) {
    t->woken = true;
    t->wait_result = result;
    t->wait = W_NONE;
    if (t == waitprof_thread) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        t->wake_qpc = q.QuadPart;
    }
    wake_later(t);
}

void kobj_notify(KObj *o) {
    for (int i = 0; i < o->nwaiters; i++) {
        KThread *t = o->waiters[i];
        if (t->wait == W_SYNC && !t->woken) {
            t->wake_obj = o;
            end_wait(t, R_OK);
        }
    }
}

/* ---------------------------------------------------------------------------------------------- handles */
#define MAX_HANDLES 4096
static struct { KObj *obj; u16 gen; } htab[MAX_HANDLES];
static int hfree_hint = 1;
static SRWLOCK hlock = SRWLOCK_INIT;

u32 handle_new(KObj *o, u32 *out) {
    AcquireSRWLockExclusive(&hlock);
    for (int n = 0; n < MAX_HANDLES; n++) {
        int i = (hfree_hint + n) % MAX_HANDLES;
        if (i == 0 || htab[i].obj) continue;
        htab[i].obj = o;
        htab[i].gen = (u16)((htab[i].gen + 1) & 0x7fff) ? (u16)((htab[i].gen + 1) & 0x7fff) : 1;
        kobj_ref(o);
        hfree_hint = i + 1;
        *out = (u32)htab[i].gen << 15 | (u32)i;
        ReleaseSRWLockExclusive(&hlock);
        return R_OK;
    }
    ReleaseSRWLockExclusive(&hlock);
    return R_OUT_OF_HANDLES;
}

KObj *handle_get(u32 h, KType type) {
    KObj *o = NULL;
    if (h == H_CUR_THREAD) {
        o = &cur_thread->obj;
    } else {
        u32 i = h & 0x7fff, gen = h >> 15;
        AcquireSRWLockShared(&hlock);
        if (i < MAX_HANDLES && htab[i].obj && htab[i].gen == gen) o = htab[i].obj;
        ReleaseSRWLockShared(&hlock);
    }
    if (!o || (type != KO_NONE && o->type != type)) return NULL;
    kobj_ref(o);
    return o;
}

u32 handle_close(u32 h) {
    if (h == H_CUR_THREAD || h == H_CUR_PROCESS) return R_OK;
    u32 i = h & 0x7fff, gen = h >> 15;
    AcquireSRWLockExclusive(&hlock);
    KObj *o = NULL;
    if (i < MAX_HANDLES && htab[i].obj && htab[i].gen == gen) {
        o = htab[i].obj;
        htab[i].obj = NULL;
    }
    ReleaseSRWLockExclusive(&hlock);
    if (!o) return R_INVALID_HANDLE;
    kobj_unref(o);
    return R_OK;
}

/* ---------------------------------------------------------------------------------------------- cores */
#define NCORES 3
static KThread *core_owner[NCORES];        /* under klock */
static u64 ready_seq;
u64 lifted_lo, lifted_hi;

static u64 qpc_now(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return (u64)q.QuadPart; }
static void hold_end(KThread *t) { if (t->hold_since) t->hold_qpc += qpc_now() - t->hold_since; t->hold_since = 0; }

static void core_assign(KThread *t, int c) {
    core_owner[c] = t;
    t->cur_core = c;
    t->hold_since = qpc_now();
    t->ready = false;
    if (t->preempted) {
        t->preempted = false;
        ResumeThread((HANDLE)t->host);
    } else {
        wake_later(t);
    }
}

/* who gets a free core first: higher priority, then a preempted thread (it was running), then the longest ready */
static bool better(KThread *a, KThread *b) {
    if (!b) return true;
    if (a->prio != b->prio) return a->prio < b->prio;
    if (a->preempted != b->preempted) return a->preempted;
    return a->ready_seq < b->ready_seq;
}

static KThread *next_for_core(int c) {
    KThread *best = NULL;
    for (KThread *t = threads; t; t = t->next) {
        if (t->preempted ? t->cur_core != c : !(t->ready && (t->affinity >> c & 1))) continue;
        if (better(t, best)) best = t;
    }
    return best;
}

static void core_release(KThread *t) {
    int c = t->cur_core;
    if (c < 0) return;
    t->cur_core = -1;
    t->preempt_req = false;
    hold_end(t);
    core_owner[c] = NULL;
    KThread *n = next_for_core(c);
    if (n) core_assign(n, c);
}

/* stop o where it is if it is in lifted code */
static bool try_suspend(KThread *o) {
    if (!lifted_hi || !o->host || SuspendThread((HANDLE)o->host) == (DWORD)-1) return false;
    CONTEXT ctx;
    memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    if (GetThreadContext((HANDLE)o->host, &ctx) && ctx.Rip >= lifted_lo && ctx.Rip < lifted_hi) return true;
    ResumeThread((HANDLE)o->host);
    return false;
}

/* give t a core now if it can have one: 1 done, 0 wait for a release, -1 retry soon (an owner was asked to yield) */
static int core_try_place(KThread *t) {
    int ideal = t->core;
    if (ideal >= 0 && ideal < NCORES && (t->affinity >> ideal & 1) && !core_owner[ideal]) {
        core_assign(t, ideal);
        return 1;
    }
    for (int c = 0; c < NCORES; c++)
        if ((t->affinity >> c & 1) && !core_owner[c]) {
            core_assign(t, c);
            return 1;
        }
    int v = -1;
    for (int c = 0; c < NCORES; c++)
        if ((t->affinity >> c & 1) && core_owner[c]->prio > t->prio && (v < 0 || core_owner[c]->prio > core_owner[v]->prio))
            v = c;
    if (v < 0) return 0;
    KThread *o = core_owner[v];
    if (!try_suspend(o)) {
        o->preempt_req = true;
        return -1;
    }
    o->preempted = true;
    hold_end(o);
    core_owner[v] = NULL;
    core_assign(t, v);
    return 1;
}

static void core_acquire(KThread *t) {
    if (t->cur_core >= 0) return;
    t->ready = true;
    t->ready_seq = ++ready_seq;
    LARGE_INTEGER q0, q1;
    QueryPerformanceCounter(&q0);
    int how = 0;
    for (;;) {
        bool free_core = false;
        for (int c = 0; c < NCORES && !free_core; c++) free_core = (t->affinity >> c & 1) && !core_owner[c];
        int r = core_try_place(t);
        if (r == 1 && !free_core && !how) how = 1;
        if (r == 1 || t->cur_core >= 0) break;
        if (r < 0) how = 2;
        else if (how != 2) how = 3;
        ksleep(t, r < 0 ? 1 : INFINITE);
        if (t->cur_core >= 0) break;
    }
    t->acq[how]++;
    QueryPerformanceCounter(&q1);
    t->ready_qpc += q1.QuadPart - q0.QuadPart;
    t->ready = false;
}

void core_leave(void) {
    klock();
    core_release(cur_thread);
    kunlock();
}

void core_enter(void) {
    klock();
    core_acquire(cur_thread);
    kunlock();
}

void kernel_svc_exit(void) {
    KThread *t = cur_thread;
    if (!t->preempt_req) return;
    klock();
    t->preempt_req = false;
    KThread *n = t->cur_core >= 0 ? next_for_core(t->cur_core) : NULL;
    if (n && n->prio < t->prio) {
        core_release(t);
        core_acquire(t);
    }
    kunlock();
}

/* NX_ONE_CORE: every thread on core 0, so that guest threads never run at the same time (to tell races from
 * other bugs) */
static int one_core = -1;
static bool only_core0(void) {
    if (one_core < 0) one_core = getenv("NX_ONE_CORE") != NULL;
    return one_core;
}

void thread_set_core(KThread *t, s32 core, u64 mask) {
    klock();
    if (only_core0()) { mask = 1; core = 0; }
    if (mask) t->affinity = mask & ((1u << NCORES) - 1);
    if (core >= 0) t->core = core;
    if (!(t->affinity >> t->core & 1)) t->core = t->affinity ? (s32)__builtin_ctzll(t->affinity) : 0;
    if (!t->affinity) t->affinity = 1ull << t->core;
    hlog(1, "%s: ideal core %d, mask %llx", t->name, t->core, t->affinity);
    if (t == cur_thread && t->cur_core >= 0 && !(t->affinity >> t->cur_core & 1)) {
        core_release(t);
        core_acquire(t);
    } else if (t->cur_core >= 0 && !(t->affinity >> t->cur_core & 1)) {
        t->preempt_req = true;                         /* moves at its next SVC */
    }
    kunlock();
}

/* ---------------------------------------------------------------------------------------------- waiting */
/* NX_PROFILE's thread: time it spends blocked, by wait kind and guest call chain (its frame records: x30, then the
 * return addresses through x29), reported with the CPU statistics (who it waits for, and from where); and for the
 * waits something ended, how long from that to the thread holding klk again (the host's wake-up) and to it owning
 * a core */
#define WAITPROF_CHAIN 5
typedef struct { int kind; u64 chain[WAITPROF_CHAIN]; double ms, wake_us, core_us; u64 n, nwoken; } WaitProf;
static WaitProf waitprof[512];
static int nwaitprof;

/* the profiled thread's waits that ended in the current frame (gpu.c's slow-frame lines, waitprof_frame) */
static double wf_blocked_ms, wf_long_ms;
static u64 wf_long_chain[WAITPROF_CHAIN];
static int wf_long_kind, wf_n;

static void waitprof_add(const KThread *t, int kind, double ms, double wake_us, double core_us) {
    WaitProf w = {kind};
    bool woken = wake_us >= 0;
    const Ctx *c = t->ctx;
    w.chain[0] = c->x[30];
    u64 fp = c->x[29];
    for (int i = 1; i < WAITPROF_CHAIN; i++) {
        if (!fp || fp & 15 || fp < c->x[31] || fp + 16 > t->stack_top) break;
        w.chain[i] = *(const u64 *)(uintptr_t)(fp + 8);
        fp = *(const u64 *)(uintptr_t)fp;
    }
    wf_blocked_ms += ms;
    wf_n++;
    if (ms > wf_long_ms) { wf_long_ms = ms; wf_long_kind = kind; memcpy(wf_long_chain, w.chain, sizeof w.chain); }
    for (int i = 0; i < nwaitprof; i++)
        if (waitprof[i].kind == w.kind && !memcmp(waitprof[i].chain, w.chain, sizeof w.chain)) {
            waitprof[i].ms += ms;
            waitprof[i].n++;
            if (woken) { waitprof[i].wake_us += wake_us; waitprof[i].core_us += core_us; waitprof[i].nwoken++; }
            return;
        }
    if (nwaitprof < 512) {
        w.ms = ms;
        w.n = 1;
        if (woken) { w.wake_us = wake_us; w.core_us = core_us; w.nwoken = 1; }
        waitprof[nwaitprof++] = w;
    }
}

/* NX_PROFILE's thread this frame: blocked time, waits, the longest one (where: its guest call chain, written as
 * module+offset); then starts the next frame. false without NX_PROFILE */
bool waitprof_frame(double *blocked_ms, int *waits, double *longest_ms, char *where, size_t wn) {
    if (!waitprof_thread) return false;
    klock();
    *blocked_ms = wf_blocked_ms;
    *waits = wf_n;
    *longest_ms = wf_long_ms;
    int n = 0;
    where[0] = 0;
    static const char *kinds[] = {"none", "sync", "mutex", "condvar", "arbiter", "sleep"};
    if (wf_n) n += snprintf(where, wn, "%s", kinds[wf_long_kind >= 0 && wf_long_kind < 6 ? wf_long_kind : 0]);
    for (int i = 0; i < WAITPROF_CHAIN && wf_long_chain[i] && n < (int)wn - 1; i++) {
        char b[160];
        n += snprintf(where + n, wn - n, " < %s", describe_addr(wf_long_chain[i], b, sizeof b));
    }
    wf_blocked_ms = wf_long_ms = 0;
    wf_n = 0;
    memset(wf_long_chain, 0, sizeof wf_long_chain);
    kunlock();
    return true;
}

static int waitprof_cmp(const void *a, const void *b) {
    double d = ((const WaitProf *)b)->ms - ((const WaitProf *)a)->ms;
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

static void waitprof_report(double wall) {
    if (!waitprof_thread || !nwaitprof || wall <= 0) return;
    static const char *kinds[] = {"none", "sync", "mutex", "condvar", "arbiter", "sleep"};
    qsort(waitprof, nwaitprof, sizeof *waitprof, waitprof_cmp);
    hlog(0, "profile: %s blocked, by guest call chain (%% of wall time; waits/s; after a wake: us to klk, us to a core):",
         waitprof_thread->name);
    for (int i = 0; i < nwaitprof && i < 12; i++) {
        const WaitProf *w = &waitprof[i];
        char line[1024], b[160];
        double nw = w->nwoken ? (double)w->nwoken : 1;
        int n = snprintf(line, sizeof line, "  %5.1f%% %6.1f/s %6.1f %6.1f %-7s", w->ms / 10.0 / wall, w->n / wall,
                         w->wake_us / nw, w->core_us / nw, kinds[w->kind >= 0 && w->kind < 6 ? w->kind : 0]);
        for (int k = 0; k < WAITPROF_CHAIN && w->chain[k] && n < (int)sizeof line - 170; k++)
            n += snprintf(line + n, sizeof line - n, " < %s", describe_addr(w->chain[k], b, sizeof b));
        hlog(0, "profile: %s", line);
    }
    nwaitprof = 0;
}

/* Sleep until end_wait (klock held on entry and exit), without the core. Returns the wait result, or R_TIMED_OUT
 * if the deadline passed first; the caller then takes the thread off whatever it waits on. */
/* microseconds, for wait deadlines. GetTickCount64 moves in 15.6 ms steps whatever the timer resolution: a guest
 * wait of 1-2 ms (a job poll, a streaming thread's back-off) lasted until the next step, up to ~16 ms */
static ULONGLONG kwait_now_us(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (ULONGLONG)(c.QuadPart / f.QuadPart * 1000000 + c.QuadPart % f.QuadPart * 1000000 / f.QuadPart);
}

static u32 block(KThread *t, s64 timeout_ns) {
    ULONGLONG deadline = 0;
    if (timeout_ns > 0) deadline = kwait_now_us() + (ULONGLONG)((timeout_ns + 999) / 1000);
    core_release(t);
    LARGE_INTEGER q0, q1, qf;
    int kind = t->wait;                 /* end_wait clears it */
    if (t == waitprof_thread) QueryPerformanceCounter(&q0);
    u32 r;
    for (;;) {
        if (t->woken) { r = t->wait_result; break; }
        DWORD ms = INFINITE;
        if (timeout_ns >= 0) {
            ULONGLONG now = kwait_now_us();
            if (now >= deadline) {
                /* nobody may end this wait any more while the thread waits for its core */
                t->woken = true;
                r = R_TIMED_OUT;
                break;
            }
            ULONGLONG left = (deadline - now + 999) / 1000;
            ms = left > 0x7fffffff ? 0x7fffffff : (DWORD)left;     /* (a huge timeout: not truncated into a short one) */
        }
        ksleep(t, ms);
    }
    if (t == waitprof_thread) {
        QueryPerformanceCounter(&q1);
        QueryPerformanceFrequency(&qf);
    }
    core_acquire(t);
    if (t == waitprof_thread) {
        LARGE_INTEGER q2;
        QueryPerformanceCounter(&q2);
        double us = 1e6 / (double)qf.QuadPart;
        bool ended = r != R_TIMED_OUT && t->wake_qpc;
        waitprof_add(t, kind, (double)(q1.QuadPart - q0.QuadPart) * 1000.0 / (double)qf.QuadPart,
                     ended ? (double)(q1.QuadPart - t->wake_qpc) * us : -1, (double)(q2.QuadPart - q1.QuadPart) * us);
        t->wake_qpc = 0;
    }
    return r;
}

static void begin_wait(KThread *t, int kind) {
    t->wait = kind;
    t->woken = false;
    t->wait_result = R_OK;
    t->wait_seq = ++wait_seq;
}

/* the waiting thread that goes first: highest priority (lowest number), then the longest waiting */
static KThread *best_waiter(int kind, u64 addr, int *count) {
    KThread *best = NULL;
    int n = 0;
    for (KThread *t = threads; t; t = t->next) {
        if (t->wait != kind || t->woken) continue;
        if ((kind == W_CONDVAR ? t->cv_key : t->wait_addr) != addr) continue;
        n++;
        if (!best || t->prio < best->prio || (t->prio == best->prio && t->wait_seq < best->wait_seq)) best = t;
    }
    if (count) *count = n;
    return best;
}

u32 thread_wait_sync(KObj **objs, int n, s64 timeout_ns, s32 *index) {
    KThread *t = cur_thread;
    klock();
    for (int i = 0; i < n; i++)
        if (objs[i]->signaled(objs[i])) {
            *index = i;
            kunlock();
            return R_OK;
        }
    if (t->cancel) {
        t->cancel = false;
        kunlock();
        return R_CANCELLED;
    }
    if (timeout_ns == 0) {
        kunlock();
        return R_TIMED_OUT;
    }
    begin_wait(t, W_SYNC);
    t->wake_obj = NULL;
    for (int i = 0; i < n; i++) waiter_add(objs[i], t);
    u32 r = block(t, timeout_ns);
    for (int i = 0; i < n; i++) waiter_remove(objs[i], t);
    t->wait = W_NONE;
    *index = -1;
    if (r == R_OK)
        for (int i = 0; i < n; i++)
            if (objs[i] == t->wake_obj) { *index = i; break; }
    kunlock();
    return r;
}

u32 thread_cancel_sync(KThread *t) {
    klock();
    if (t->wait == W_SYNC && !t->woken) end_wait(t, R_CANCELLED);
    else t->cancel = true;
    kunlock();
    return R_OK;
}

/* ---------------------------------------------------------------------------------------------- mutexes */
#define WAIT_MASK 0x40000000u

static u32 ld_user(u64 a) { return __atomic_load_n((u32 *)(uintptr_t)a, __ATOMIC_SEQ_CST); }
static void st_user(u64 a, u32 v) { __atomic_store_n((u32 *)(uintptr_t)a, v, __ATOMIC_SEQ_CST); }

u32 arbitrate_lock(u32 owner, u64 addr, u32 tag) {
    if (addr & 3) return R_INVALID_ADDRESS;
    KThread *t = cur_thread;
    klock();
    if (ld_user(addr) != (owner | WAIT_MASK)) {
        kunlock();
        return R_OK;
    }
    KObj *o = handle_get(owner, KO_THREAD);
    if (!o) {
        kunlock();
        return R_INVALID_HANDLE;
    }
    kobj_unref(o);
    begin_wait(t, W_MUTEX);
    t->wait_addr = addr;
    t->tag = tag;
    u32 r = block(t, -1);
    kunlock();
    return r;
}

/* hand the mutex at addr to its next waiter, or free it (klock held) */
static void mutex_release(u64 addr) {
    int n;
    KThread *w = best_waiter(W_MUTEX, addr, &n);
    u32 next = 0;
    if (w) {
        next = w->tag | (n > 1 ? WAIT_MASK : 0);
        end_wait(w, R_OK);
    }
    FENCE();
    st_user(addr, next);
}

u32 arbitrate_unlock(u64 addr) {
    if (addr & 3) return R_INVALID_ADDRESS;
    klock();
    mutex_release(addr);
    kunlock();
    return R_OK;
}

/* ---------------------------------------------------------------------------------------------- condition variables */
u32 condvar_wait(u64 mutex, u64 key, u32 tag, s64 timeout_ns) {
    if (mutex & 3) return R_INVALID_ADDRESS;
    KThread *t = cur_thread;
    klock();
    /* the key's "has waiters" flag first: the SDK's Signal skips the SVC while the key is 0, and the mutex, once
     * released, can be taken in user mode (no kernel lock) by a signaller that would then miss this waiter */
    st_user(key, 1);
    mutex_release(mutex);
    if (timeout_ns == 0) {
        kunlock();
        return R_TIMED_OUT;
    }
    begin_wait(t, W_CONDVAR);
    t->cv_key = key;
    t->wait_addr = mutex;
    t->tag = tag;
    u32 r = block(t, timeout_ns);
    if (r == R_TIMED_OUT) {
        /* still waiting on the condition variable or, after a signal, on the mutex: stop either */
        t->wait = W_NONE;
        if (!best_waiter(W_CONDVAR, key, NULL)) st_user(key, 0);
    }
    kunlock();
    return r;
}

void condvar_signal(u64 key, s32 count) {
    klock();
    int n = 0;
    while (count <= 0 || n < count) {
        KThread *w = best_waiter(W_CONDVAR, key, NULL);
        if (!w) break;
        n++;
        /* take the mutex for w, or queue w on it */
        u64 m = w->wait_addr;
        for (;;) {
            u32 v = ld_user(m);
            if (v == 0) {
                if (__sync_bool_compare_and_swap((u32 *)(uintptr_t)m, 0u, w->tag)) {
                    end_wait(w, R_OK);
                    break;
                }
            } else if (__sync_bool_compare_and_swap((u32 *)(uintptr_t)m, v, v | WAIT_MASK)) {
                w->wait = W_MUTEX;
                w->wait_seq = ++wait_seq;
                break;
            }
        }
    }
    if (!best_waiter(W_CONDVAR, key, NULL)) st_user(key, 0);
    kunlock();
}

/* ---------------------------------------------------------------------------------------------- address arbiter */
u32 address_wait(u64 addr, u32 type, s32 value, s64 timeout_ns) {
    if (addr & 3) return R_INVALID_ADDRESS;
    KThread *t = cur_thread;
    klock();
    s32 v = (s32)ld_user(addr);
    bool wait;
    switch (type) {
    case 0: wait = v < value; break;                                 /* WaitIfLessThan */
    case 1:                                                          /* DecrementAndWaitIfLessThan */
        wait = v < value;
        if (wait) st_user(addr, (u32)(v - 1));
        break;
    case 2: wait = v == value; break;                                /* WaitIfEqual */
    default: kunlock(); return R_INVALID_ENUM;
    }
    if (!wait) {
        kunlock();
        return R_INVALID_STATE;
    }
    if (timeout_ns == 0) {
        kunlock();
        return R_TIMED_OUT;
    }
    begin_wait(t, W_ARBITER);
    t->wait_addr = addr;
    u32 r = block(t, timeout_ns);
    t->wait = W_NONE;
    kunlock();
    return r;
}

u32 address_signal(u64 addr, u32 type, s32 value, s32 count) {
    if (addr & 3) return R_INVALID_ADDRESS;
    klock();
    int waiting;
    best_waiter(W_ARBITER, addr, &waiting);
    if (type == 1 || type == 2) {
        s32 nv = value + 1;
        if (type == 2) {
            if (count <= 0) nv = waiting ? value - 2 : value + 1;
            else if (!waiting) nv = value + 1;
            else nv = waiting - 1 < count ? value - 1 : value;   /* waiters after the first, vs count */
        }
        s32 v = (s32)ld_user(addr);
        if (v != value) {
            kunlock();
            return R_INVALID_STATE;
        }
        if (nv != value) st_user(addr, (u32)nv);
    } else if (type != 0) {
        kunlock();
        return R_INVALID_ENUM;
    }
    for (int n = 0; count <= 0 || n < count; n++) {
        KThread *w = best_waiter(W_ARBITER, addr, NULL);
        if (!w) break;
        end_wait(w, R_OK);
    }
    kunlock();
    return R_OK;
}

/* ---------------------------------------------------------------------------------------------- threads */
static bool thread_signaled(KObj *o) { return ((KThread *)o)->exited; }

static void thread_destroy(KObj *o) {
    KThread *t = (KThread *)o;
    /* threads stay listed (and their TLS slots taken) until the process ends: there are few */
    (void)t;
}

static u64 tls_next = TLS_BASE;

/* svcGetInfo(UserExceptionContextAddress): a page of the process's thread-local region (the last page of ours,
 * which thread slots never reach), where newer SDKs keep e.g. the dying-message region */
u64 user_exception_context(void) {
    static u64 page;
    klock();
    if (!page) {
        page = TLS_BASE + TLS_SIZE - 0x1000;
        mem_map(page, 0x1000, MS_TLS, MP_RW);
    }
    kunlock();
    return page;
}

KThread *thread_create(u64 entry, u64 arg, u64 stack_top, s32 prio, s32 core) {
    KThread *t = calloc(1, sizeof *t);
    kobj_init(&t->obj, KO_THREAD, thread_destroy, thread_signaled);
    t->ctx = _aligned_malloc(sizeof(Ctx), 64);
    memset(t->ctx, 0, sizeof(Ctx));
    t->ctx->host = t;
    t->cv = calloc(1, sizeof(CONDITION_VARIABLE));
    InitializeConditionVariable((CONDITION_VARIABLE *)t->cv);
    if (only_core0()) core = 0;
    t->entry = entry; t->arg = arg; t->stack_top = stack_top; t->prio = prio; t->core = core;
    t->affinity = 1ull << (core < 0 ? 0 : core);
    t->cur_core = -1;
    klock();
    t->id = next_thread_id++;
    if ((tls_next & 0xfff) == 0) mem_map(tls_next, 0x1000, MS_TLS, MP_RW);
    t->tls = tls_next;
    tls_next += 0x200;
    t->next = threads;
    threads = t;
    kunlock();
    Ctx *c = t->ctx;
    c->x[0] = arg;
    c->x[31] = stack_top;
    c->tpidrro = t->tls;
    snprintf(t->name, sizeof t->name, "thread %llx", (unsigned long long)t->id);
    return t;
}

void run_guest_main(KThread *t) {
    cur_thread = t;
    host_thread_init(t->ctx);
    core_enter();
    guest_call(t->ctx, t->entry);
    hlog(1, "%s: entry returned", t->name);
    thread_exit();
}

static unsigned __stdcall host_thread_main(void *p) {
    run_guest_main(p);
    return 0;
}

u32 thread_start(KThread *t) {
    if (t->started) return R_INVALID_STATE;
    t->started = true;
    kobj_ref(&t->obj);                                  /* the running thread keeps itself alive */
    unsigned id;
    /* host stack: lifted code recurses as deeply as the guest does, with larger frames */
    t->host = (void *)_beginthreadex(NULL, 32 << 20, host_thread_main, t, STACK_SIZE_PARAM_IS_A_RESERVATION, &id);
    if (!t->host) fatal("cannot start a host thread");
    t->host_id = id;
    return R_OK;
}

void thread_exit(void) {
    KThread *t = cur_thread;
    klock();
    t->exited = true;
    kobj_notify(&t->obj);
    core_release(t);
    kunlock();
    hlog(2, "%s exits", t->name);
    kobj_unref(&t->obj);
    _endthreadex(0);
    for (;;) {}
}

#define SLEEP_SPIN_NS 250000                    /* thread_sleep spins the last 0.25 ms */

void thread_sleep(s64 ns) {
    KThread *t = cur_thread;
    if (ns <= 0) {                                     /* 0, -1, -2: the yield variants, to equal priorities */
        klock();
        KThread *n = t->cur_core >= 0 ? next_for_core(t->cur_core) : NULL;
        if (n && n->prio <= t->prio) {
            core_release(t);
            core_acquire(t);
        }
        kunlock();
        if (!n) SwitchToThread();
        return;
    }
    klock();
    t->wait = W_SLEEP;
    core_release(t);
    kunlock();
    /* the high-resolution timer to SLEEP_SPIN_NS before the deadline, then spin to it: the timer alone wakes late
     * by a fraction of a millisecond, varying (frame pacing sleeps here once a frame). The core is released, so
     * the spin costs host time only */
    static __declspec(thread) HANDLE timer;
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER q0, q1, f;
    QueryPerformanceCounter(&q0);
    QueryPerformanceFrequency(&f);
    u64 deadline = (u64)q0.QuadPart + (u64)((double)ns * (double)f.QuadPart / 1e9);
    if (ns > SLEEP_SPIN_NS) {
        LARGE_INTEGER due;
        due.QuadPart = -(ns - SLEEP_SPIN_NS) / 100;
        if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
        else Sleep((DWORD)((ns - SLEEP_SPIN_NS) / 1000000));
    }
    do { _mm_pause(); QueryPerformanceCounter(&q1); } while ((u64)q1.QuadPart < deadline);
    t->sleep_qpc += q1.QuadPart - q0.QuadPart;
    t->sleeps++;
    klock();
    t->wait = W_NONE;
    core_acquire(t);
    kunlock();
}

/* ---------------------------------------------------------------------------------------------- events */
static bool event_signaled(KObj *o) { return ((KEvent *)o)->signaled; }

KEvent *event_create(void) {
    KEvent *e = calloc(1, sizeof *e);
    kobj_init(&e->obj, KO_EVENT, NULL, event_signaled);
    return e;
}

void event_signal(KEvent *e) {
    klock();
    e->signaled = true;
    kobj_notify(&e->obj);
    kunlock();
}

void event_clear(KEvent *e) {
    klock();
    e->signaled = false;
    kunlock();
}

/* every guest thread: what it waits for, and its backtrace as of its last call or SVC */
/* every 600 frames: frames per second, and the share of one host core each busy guest thread used since the last
 * report (a thread near 100% is what holds the frame rate) */
/* every presented frame (gpu.c, with NX_HITCH_MS): how long each guest thread held its emulated core and waited for
 * one since the last call (exact, from the scheduler; Windows' CPU times come in 15.6 ms steps); for a slow frame
 * (report), the threads that held or waited most, as "name held core 0 12.3 ms (core wait 1.0), ..." (core: the
 * thread's ideal core) */
void kernel_frame_threads(bool report, char *out, size_t n) {
    static struct { KThread *t; u64 cpu, ready, hold; } last[128];
    static int nlast;
    struct { const char *name; double cpu, ready, hold; int core; } top[128];
    int ntop = 0;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    klock();
    u64 now = qpc_now();
    for (KThread *t = threads; t; t = t->next)          /* holders so far: count up to now */
        if (t->hold_since) { t->hold_qpc += now - t->hold_since; t->hold_since = now; }
    for (KThread *t = threads; t; t = t->next) {
        FILETIME c0, e0, k, u;
        if (!t->host || !GetThreadTimes(t->host, &c0, &e0, &k, &u)) continue;
        u64 cpu = ((u64)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((u64)u.dwHighDateTime << 32 | u.dwLowDateTime);
        int i = 0;
        while (i < nlast && last[i].t != t) i++;
        if (i == nlast) {
            if (nlast == 128) continue;
            last[nlast++] = (typeof(last[0])){t, cpu, t->ready_qpc, t->hold_qpc};
            continue;
        }
        double ms = (double)(cpu - last[i].cpu) / 1e4, ready = (double)(t->ready_qpc - last[i].ready) * 1000.0 / (double)f.QuadPart;
        double hold = (double)(t->hold_qpc - last[i].hold) * 1000.0 / (double)f.QuadPart;
        last[i].cpu = cpu;
        last[i].ready = t->ready_qpc;
        last[i].hold = t->hold_qpc;
        if (report && (hold >= 1 || ready >= 1)) top[ntop++] = (typeof(top[0])){t->name, ms, ready, hold, t->core};
    }
    kunlock();
    if (!report) return;
    int o = 0;
    out[0] = 0;
    for (int shown = 0; shown < 8 && ntop; shown++) {
        int b = 0;
        for (int i = 1; i < ntop; i++)
            if (top[i].hold + top[i].ready > top[b].hold + top[b].ready) b = i;
        if (o < (int)n)
            o += snprintf(out + o, n - o, "%s%s held core %d %.1f ms", shown ? ", " : "", top[b].name, top[b].core, top[b].hold);
        if (top[b].ready >= 0.5 && o < (int)n) o += snprintf(out + o, n - o, " (core wait %.1f)", top[b].ready);
        top[b] = top[--ntop];
    }
}

void kernel_report_cpu(u64 frames) {
    static LARGE_INTEGER last_t;
    static u64 last_frames;
    static struct { KThread *t; u64 cpu, ready, sleep, sleeps; u64 acq[4]; } prev[128];
    static int nprev;
    LARGE_INTEGER f, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&now);
    double wall = last_t.QuadPart ? (double)(now.QuadPart - last_t.QuadPart) / (double)f.QuadPart : 0;
    char line[1024];
    int n = 0;
    struct { const char *name; double pct, ready, sleep, sleeps; u64 acq[4]; } busy[128];
    int nb = 0;
    AcquireSRWLockShared(&klk);
    for (KThread *t = threads; t; t = t->next) {
        FILETIME c0, e0, k, u;
        if (!t->host || !GetThreadTimes(t->host, &c0, &e0, &k, &u)) continue;
        u64 cpu = ((u64)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((u64)u.dwHighDateTime << 32 | u.dwLowDateTime);
        int i = 0;
        while (i < nprev && prev[i].t != t) i++;
        if (i == nprev && nprev < 128) {
            prev[nprev] = (typeof(prev[0])){t, cpu, t->ready_qpc, t->sleep_qpc, t->sleeps};
            memcpy(prev[nprev++].acq, t->acq, sizeof t->acq);
        }
        else if (i < nprev) {
            double pct = wall > 0 ? (double)(cpu - prev[i].cpu) / 1e7 / wall * 100 : 0;
            double ready = wall > 0 ? (double)(t->ready_qpc - prev[i].ready) / (double)f.QuadPart / wall * 100 : 0;
            double sleep = wall > 0 ? (double)(t->sleep_qpc - prev[i].sleep) / (double)f.QuadPart / wall * 100 : 0;
            double sleeps = (double)(t->sleeps - prev[i].sleeps);
            u64 acq[4];
            for (int k = 0; k < 4; k++) acq[k] = t->acq[k] - prev[i].acq[k];
            prev[i] = (typeof(prev[0])){t, cpu, t->ready_qpc, t->sleep_qpc, t->sleeps};
            memcpy(prev[i].acq, t->acq, sizeof t->acq);
            if ((pct >= 5 || ready >= 5 || sleep >= 5) && nb < 128) {
                busy[nb] = (typeof(busy[0])){t->name, pct, ready, sleep, sleeps};
                memcpy(busy[nb++].acq, acq, sizeof acq);
            }
        }
    }
    /* NX_PROFILE (dispatch.c): a comma-separated list of thread names; the first one's waits are also profiled */
    static char pn[256];
    static bool pn_read;
    if (!pn_read) { const char *e = getenv("NX_PROFILE"); if (e) snprintf(pn, sizeof pn, ",%s,", e); pn_read = true; }
    for (KThread *t = threads; pn[0] && t; t = t->next) {
        char key[48];
        snprintf(key, sizeof key, ",%s,", t->name);
        const char *at = t->host ? strstr(pn, key) : NULL;
        if (!at) continue;
        profile_start(t->host, t->name);
        if (at == pn) waitprof_thread = t;
    }
    waitprof_report(wall);                              /* its table is written under klock */
    ReleaseSRWLockShared(&klk);
    profile_report();
    if (wall > 0) {
        n += snprintf(line + n, sizeof line - n, "cpu: %.1f fps; threads", (double)(frames - last_frames) / wall);
        for (int i = 0; i < nb && n < (int)sizeof line - 40; i++)
            n += snprintf(line + n, sizeof line - n, " %s %.0f%%", busy[i].name, busy[i].pct);
        hlog(0, "%s", line);
        for (int i = 0; i < nb; i++)
            if (busy[i].ready >= 1 || busy[i].sleep >= 1)
            {
                double fr = frames > last_frames ? (double)(frames - last_frames) : 1;
                hlog(1, "cpu:   %s: running %.0f%%, waiting for a core %.0f%%, asleep %.0f%% (%.1f sleeps/frame); cores taken "
                        "per frame: %.1f free, %.1f by preempting, %.1f after 1 ms retries, %.1f after a release", busy[i].name,
                     busy[i].pct, busy[i].ready, busy[i].sleep, busy[i].sleeps / fr, busy[i].acq[0] / fr, busy[i].acq[1] / fr,
                     busy[i].acq[2] / fr, busy[i].acq[3] / fr);
            }
    }
    last_t = now;
    last_frames = frames;
}

void kernel_dump_threads(void) {
    static const char *const kinds[] = {"running", "WaitSynchronization", "mutex", "condition variable",
                                        "address arbiter", "sleep"};
    void *run[16];
    const char *run_names[16];
    int nrun = 0;
    AcquireSRWLockShared(&klk);
    for (KThread *t = threads; t; t = t->next) {
        if (t->exited) continue;
        char b[200];
        hlog(0, "%s (id %llx, prio %d, core %d/%llx): %s%s addr %llx key %llx; lr %s", t->name, t->id, t->prio,
             t->cur_core, t->affinity, t->started ? "" : "not started, ",
             t->ready ? "waiting for a core" : t->preempted ? "preempted" : kinds[t->wait < 6 ? t->wait : 0],
             t->wait_addr, t->cv_key, describe_addr(t->ctx->x[30], b, sizeof b));
        if (t->wait == W_MUTEX && t->wait_addr) {           /* the owner, from the handle in the mutex word */
            u32 owner = ld_user(t->wait_addr) & ~WAIT_MASK;
            KObj *o = handle_get(owner, KO_THREAD);
            KThread *ot = o ? (KThread *)((char *)o - offsetof(KThread, obj)) : NULL;
            hlog(0, "  mutex word %08x: owner %s", ld_user(t->wait_addr), ot ? ot->name : "(no such thread)");
            if (o) kobj_unref(o);
        }
        if (t->host && t->wait == W_NONE && !t->ready && !t->preempted && nrun < 16) {
            run_names[nrun] = t->name;
            run[nrun++] = t->host;
        }
        dump_ctx(t->ctx);
    }
    ReleaseSRWLockShared(&klk);
    /* the running threads, sampled without the kernel lock (holding it would stop each at its next SVC) */
    for (int i = 0; i < nrun; i++)
        for (int k = 0; k < 4; k++) {
            hlog(0, "%s running, sample %d:", run_names[i], k);
            host_stack(run[i]);
            Sleep(37);
        }
}

void kernel_init(void) {}
