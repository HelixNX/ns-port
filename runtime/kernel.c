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
void klock(void) { AcquireSRWLockExclusive(&klk); }
void kunlock(void) { ReleaseSRWLockExclusive(&klk); }

__declspec(thread) KThread *cur_thread;
static KThread *threads;            /* all threads, under klock */
static u64 next_thread_id = 0x50;   /* thread ids as the guest sees them */
static u64 wait_seq;

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
    WakeConditionVariable((CONDITION_VARIABLE *)t->cv);
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

static void core_assign(KThread *t, int c) {
    core_owner[c] = t;
    t->cur_core = c;
    t->ready = false;
    if (t->preempted) {
        t->preempted = false;
        ResumeThread((HANDLE)t->host);
    } else {
        WakeConditionVariable((CONDITION_VARIABLE *)t->cv);
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
    core_owner[v] = NULL;
    core_assign(t, v);
    return 1;
}

static void core_acquire(KThread *t) {
    if (t->cur_core >= 0) return;
    t->ready = true;
    t->ready_seq = ++ready_seq;
    for (;;) {
        int r = core_try_place(t);
        if (r == 1 || t->cur_core >= 0) break;
        SleepConditionVariableSRW((CONDITION_VARIABLE *)t->cv, &klk, r < 0 ? 1 : INFINITE, 0);
        if (t->cur_core >= 0) break;
    }
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

void thread_set_core(KThread *t, s32 core, u64 mask) {
    klock();
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
/* Sleep until end_wait (klock held on entry and exit), without the core. Returns the wait result, or R_TIMED_OUT
 * if the deadline passed first; the caller then takes the thread off whatever it waits on. */
static u32 block(KThread *t, s64 timeout_ns) {
    ULONGLONG deadline = 0;
    if (timeout_ns > 0) deadline = GetTickCount64() + (ULONGLONG)((timeout_ns + 999999) / 1000000);
    core_release(t);
    u32 r;
    for (;;) {
        if (t->woken) { r = t->wait_result; break; }
        DWORD ms = INFINITE;
        if (timeout_ns >= 0) {
            ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                /* nobody may end this wait any more while the thread waits for its core */
                t->woken = true;
                r = R_TIMED_OUT;
                break;
            }
            ms = (DWORD)(deadline - now);
        }
        SleepConditionVariableSRW((CONDITION_VARIABLE *)t->cv, &klk, ms, 0);
    }
    core_acquire(t);
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
    mutex_release(mutex);
    st_user(key, 1);
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

KThread *thread_create(u64 entry, u64 arg, u64 stack_top, s32 prio, s32 core) {
    KThread *t = calloc(1, sizeof *t);
    kobj_init(&t->obj, KO_THREAD, thread_destroy, thread_signaled);
    t->ctx = _aligned_malloc(sizeof(Ctx), 64);
    memset(t->ctx, 0, sizeof(Ctx));
    t->ctx->host = t;
    t->cv = calloc(1, sizeof(CONDITION_VARIABLE));
    InitializeConditionVariable((CONDITION_VARIABLE *)t->cv);
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
    static __declspec(thread) HANDLE timer;
    if (!timer) timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -(ns + 99) / 100;
    if (timer && SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) WaitForSingleObject(timer, INFINITE);
    else Sleep((DWORD)((ns + 999999) / 1000000));
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
void kernel_dump_threads(void) {
    static const char *const kinds[] = {"running", "WaitSynchronization", "mutex", "condition variable",
                                        "address arbiter", "sleep"};
    AcquireSRWLockShared(&klk);
    for (KThread *t = threads; t; t = t->next) {
        if (t->exited) continue;
        char b[200];
        hlog(0, "%s (id %llx, prio %d, core %d/%llx): %s%s addr %llx key %llx; lr %s", t->name, t->id, t->prio,
             t->cur_core, t->affinity, t->started ? "" : "not started, ",
             t->ready ? "waiting for a core" : t->preempted ? "preempted" : kinds[t->wait < 6 ? t->wait : 0],
             t->wait_addr, t->cv_key, describe_addr(t->ctx->x[30], b, sizeof b));
        if (t->host && t->wait == W_NONE && !t->ready && !t->preempted) {                  /* running: where it is now, five samples */
            char where[5][128];
            for (int k = 0; k < 5; k++) {
                host_where(t->host, where[k], sizeof where[k]);
                Sleep(20);
            }
            hlog(0, "  now in %s | %s | %s | %s | %s", where[0], where[1], where[2], where[3], where[4]);
        }
        dump_ctx(t->ctx);
    }
    ReleaseSRWLockShared(&klk);
}

void kernel_init(void) {}
