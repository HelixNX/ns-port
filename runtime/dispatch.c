/* What the lifted code calls into: indirect calls and branches (one table slot per text word, so a lookup is an
 * index), host functions at guest addresses ("host calls"), traps, the counter and FPCR, and the crash report. */
#include "hos.h"
#ifdef NX_PACKAGE
#include "game_config.h"                  /* NX_TITLE */
#endif
#include <stdlib.h>
#include <windows.h>
#include <dbghelp.h>
#include <psapi.h>
#include <intrin.h>
#include <process.h>

extern const ModInfo lifted_modules[];

typedef struct { u64 base, span; GuestFn *tab; const char *name; } FnTable;
static FnTable tables[4];
static int ntables;

static struct { const char *name; GuestFn fn; } hostcalls[HOSTCALL_MAX];
static int nhostcalls;

/* every module's slots in one table from the lowest module to the end of the highest text (a64rt.h guest_fn_fast):
 * the gaps between texts are never written, so their pages stay untouched */
GuestFn *fn_flat;
u64 fn_flat_base, fn_flat_span;

void dispatch_init(void) {
    u64 lo = ~0ull, hi = 0;
    for (const ModInfo *m = lifted_modules; m->name; m++) {
        if (m->base < lo) lo = m->base;
        if (m->text_end > hi) hi = m->text_end;
    }
    fn_flat_base = lo;
    fn_flat_span = hi - lo;
    fn_flat = VirtualAlloc(NULL, fn_flat_span / 4 * sizeof(GuestFn), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!fn_flat) fatal("no memory for the function table (%llx bytes of text)", (unsigned long long)fn_flat_span);
    for (const ModInfo *m = lifted_modules; m->name; m++) {
        FnTable *t = &tables[ntables++];
        t->base = m->base;
        t->span = m->text_end - m->base;
        t->name = m->name;
        t->tab = fn_flat + (m->base - fn_flat_base) / 4;
        for (const FnEntry *e = m->entries; e->fn; e++) {
            t->tab[(e->addr - m->base) / 4] = e->fn;
            u64 h = (u64)(uintptr_t)e->fn;
            if (!lifted_lo || h < lifted_lo) lifted_lo = h;
            if (h > lifted_hi) lifted_hi = h;
        }
    }
    /* Where lifted code is, for preemption (kernel.c): the linker puts lifted.lib's code in one block, right after
     * the runtime's objects and followed by a64rt.c's (lock-free helpers, so the range may include them). If that
     * ever changes, preemption waits for SVCs instead of suspending threads. */
    if ((u64)(uintptr_t)stxp64 > lifted_hi) lifted_hi = (u64)(uintptr_t)stxp64;
    const u64 host_fns[] = {(u64)(uintptr_t)svc_dispatch, (u64)(uintptr_t)hlog, (u64)(uintptr_t)call_ind,
                            (u64)(uintptr_t)dispatch_init, (u64)(uintptr_t)nvn_init};
    for (size_t i = 0; i < sizeof host_fns / sizeof *host_fns; i++)
        if (host_fns[i] >= lifted_lo && host_fns[i] < lifted_hi) {
            hlog(0, "runtime code lies among lifted code: threads are preempted only at SVCs");
            lifted_lo = lifted_hi = 0;
            break;
        }
}

u64 hostcall_register(const char *name, GuestFn fn) {
    if (nhostcalls == HOSTCALL_MAX) fatal("too many host calls");
    hostcalls[nhostcalls].name = name;
    hostcalls[nhostcalls].fn = fn;
    return HOSTCALL_BASE + 4 * (u64)nhostcalls++;
}

GuestFn guest_fn(u64 addr) {
    for (int i = 0; i < ntables; i++) {
        u64 off = addr - tables[i].base;
        if (off < tables[i].span) return off & 3 ? NULL : tables[i].tab[off / 4];
    }
    u64 off = addr - HOSTCALL_BASE;
    if (off < 4 * (u64)nhostcalls && !(off & 3)) return hostcalls[off / 4].fn;
    return NULL;
}

void dispatch_override(u64 addr, GuestFn fn) {
    for (int i = 0; i < ntables; i++) {
        u64 off = addr - tables[i].base;
        if (off < tables[i].span) tables[i].tab[off / 4] = fn;
    }
}

/* NX_TRACE_CALLS=addr,...: calls through pointers to those guest addresses are logged (x0-x3, the caller; then the
 * result), the first 40 of each. Direct calls (BL) are not seen. */
static struct { u64 addr; GuestFn orig; volatile long n; } traced[16];
static int ntraced;
static void trace_call(Ctx *c) {
    u64 a = c->pc;
    int i = 0;
    while (i < ntraced && traced[i].addr != a) i++;
    if (i == ntraced) guest_trap(c, "traced call to an unknown address", a);
    long n = InterlockedIncrement(&traced[i].n);
    char b[160], l[160];
    if (n <= 40)
        hlog(1, "call %s (x0 %llx x1 %llx x2 %llx x3 %llx) from %s", describe_addr(a, b, sizeof b), c->x[0], c->x[1],
             c->x[2], c->x[3], describe_addr(c->x[30], l, sizeof l));
    u64 x1 = c->x[1];
    traced[i].orig(c);
    if (n <= 40)                                        /* and the word at x1, often an output */
        hlog(1, "  %s -> %llx; [x1] %08x", describe_addr(a, b, sizeof b), c->x[0],
             mem_is_free(x1 & ~3ull, 4) ? 0 : *(const u32 *)(uintptr_t)(x1 & ~3ull));
}
void dispatch_trace_init(void) {
    for (const char *e = getenv("NX_TRACE_CALLS"); e && *e && ntraced < 16;) {
        char *end;
        u64 a = strtoull(e, &end, 16);
        GuestFn f = guest_fn(a);
        if (f) {
            traced[ntraced].addr = a;
            traced[ntraced++].orig = f;
            dispatch_override(a, trace_call);
        } else {
            hlog(0, "NX_TRACE_CALLS: no lifted function at %llx", a);
        }
        e = *end == ',' ? end + 1 : end + strlen(end);
    }
}

void call_ind(Ctx *c, u64 addr) {
    GuestFn f = guest_fn(addr);
    c->pc = addr;                      /* host calls shared by several addresses tell them apart by it */
    if (!f) guest_trap(c, "call to an address with no lifted function", addr);
    f(c);
}

void jump_ind(Ctx *c) {
    GuestFn f = guest_fn(c->pc);
    if (!f) guest_trap(c, "branch to an address with no lifted function", c->pc);
    MUSTTAIL return f(c);
}

/* setjmp/longjmp (a64rt.h): the host jump buffers of each thread's setjmp calls, by guest jmp_buf. A buffer on the
 * guest stack below the stack pointer belongs to a frame that has returned, so its slot is free again: Lua runs
 * many short protected calls at varying depths, and evicting the oldest slot lost the outer pcall's buffer that a
 * later error unwound to. A jmp_buf elsewhere (in a decoder's heap state: each new one has a new address) is never
 * known to be dead: those went round-robin with the rest, and once 64 had piled up over a long session a live Lua
 * buffer could be the one evicted (2026-11-05, Violet: the same trap after hours of play). So the table is bigger,
 * and when it is full an entry that is not a live frame on this thread's stack goes first. */
#define HOST_JMPS 256
#define STACK_SPAN 0x1000000ull         /* how far from SP a jmp_buf counts as on the stack */
typedef struct { u64 guest; jmp_buf buf; } HostJmp;
static __declspec(thread) HostJmp host_jmps[HOST_JMPS];
static __declspec(thread) unsigned host_jmp_next;

static HostJmp *host_jmp_find(u64 guest_buf) {
    for (int i = 0; i < HOST_JMPS; i++)
        if (host_jmps[i].guest == guest_buf) return &host_jmps[i];
    return NULL;
}

jmp_buf *guest_jmp_slot(u64 guest_buf) {
    HostJmp *j = host_jmp_find(guest_buf);
    if (!j) {
        u64 sp = cur_thread ? cur_thread->ctx->x[31] : 0;     /* (saved before the call into setjmp) */
        for (int i = 0; i < HOST_JMPS && !j; i++) {             /* free, or in a frame that has returned */
            u64 g = host_jmps[i].guest;
            if (!g || (g < sp && sp - g < STACK_SPAN)) j = &host_jmps[i];
        }
        for (int n = 0; n < HOST_JMPS && !j; n++) {             /* full: one that is not a live stack frame */
            HostJmp *k = &host_jmps[host_jmp_next++ % HOST_JMPS];
            if (!(k->guest >= sp && k->guest - sp < STACK_SPAN)) j = k;
        }
        if (!j) {
            static volatile LONG warned;
            if (!InterlockedExchange(&warned, 1))
                hlog(0, "setjmp: %d live jmp_bufs on one thread's stack: evicting one", HOST_JMPS);
            j = &host_jmps[host_jmp_next++ % HOST_JMPS];
        }
        j->guest = guest_buf;
    }
    return &j->buf;
}

void h_longjmp(Ctx *c) {
    u64 b = c->x[0], val = c->x[1];
    HostJmp *j = host_jmp_find(b);
    if (!j) guest_trap(c, "longjmp to a jmp_buf no lifted setjmp call set", c->x[30]);
    const u64 *g = (const u64 *)(uintptr_t)b;      /* the sdk's layout: x19-x30, sp at 0x68, d8-d15 at 0x70 */
    for (int i = 0; i < 12; i++) c->x[19 + i] = g[i];
    c->x[31] = g[0x68 / 8];
    for (int i = 0; i < 8; i++) c->v[8 + i] = (v128){g[0x70 / 8 + i], 0};
    c->x[0] = val ? val : 1;
    longjmp(j->buf, 1);
}
void h__longjmp(Ctx *c) { h_longjmp(c); }

void guest_call(Ctx *c, u64 addr) {
    c->x[30] = HOSTCALL_BASE;          /* a return address that is not guest code */
    call_ind(c, addr);
}

void guest_svc(Ctx *c, u32 imm) { svc_dispatch(c, imm); }

void host_yield(void) { YieldProcessor(); }

/* CNTPCT_EL0: 19.2 MHz since start-up */
static LARGE_INTEGER qpc_freq, qpc_start;
u64 host_cntpct(void) {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    u64 d = (u64)(now.QuadPart - qpc_start.QuadPart);
    u64 f = (u64)qpc_freq.QuadPart;
    return d / f * 19200000u + d % f * 19200000u / f;
}

/* FPCR.FZ (bit 24) -> MXCSR flush-to-zero and denormals-are-zero; rounding modes other than nearest are not
 * modelled (see CLAUDE.md) */
void host_set_fpcr(Ctx *c) {
    unsigned m = _mm_getcsr() & ~0x8040u;
    if (c->fpcr & (1u << 24)) m |= 0x8040u;
    _mm_setcsr(m);
}

void host_thread_init(Ctx *c) { host_set_fpcr(c); }

/* ---------------------------------------------------------------------------------------------- diagnostics */
int log_level = 1;
static SRWLOCK loglock = SRWLOCK_INIT;

static void vlog(const char *prefix, const char *fmt, va_list ap) {
    AcquireSRWLockExclusive(&loglock);
    fprintf(stderr, "[%s] %s", cur_thread ? cur_thread->name : "host", prefix);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    ReleaseSRWLockExclusive(&loglock);
}

void hlog(int level, const char *fmt, ...) {
    if (level > log_level) return;
    va_list ap;
    va_start(ap, fmt);
    vlog("", fmt, ap);
    va_end(ap);
}

void fatal(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog("fatal: ", fmt, ap);
    va_end(ap);
#ifdef NX_PACKAGE
    {   /* no console for players: say what went wrong and where the log is */
        char what[1024], msg[1400];
        va_start(ap, fmt);
        vsnprintf(what, sizeof what, fmt, ap);
        va_end(ap);
        snprintf(msg, sizeof msg, "%s\n\nThe log (log.txt) is in %%LOCALAPPDATA%%\\" NX_TITLE "\\.", what);
        MessageBoxA(NULL, msg, NX_TITLE, MB_ICONERROR);
    }
#endif
    if (IsDebuggerPresent()) __debugbreak();
    TerminateProcess(GetCurrentProcess(), 3);
    for (;;) {}
}

const char *describe_addr(u64 addr, char *buf, size_t n) {
    u64 off;
    const char *s = sym_name(addr, &off);
    for (int i = 0; i < num_loaded_mods; i++) {
        LoadedMod *m = &loaded_mods[i];
        if (addr >= m->base && addr < m->bss_end) {
            if (s && off < 0x100000) snprintf(buf, n, "%s+%llx (%s+%llx)", m->name, addr - m->base, s, off);
            else snprintf(buf, n, "%s+%llx", m->name, addr - m->base);
            return buf;
        }
    }
    snprintf(buf, n, "%llx", addr);
    return buf;
}

void dump_ctx(Ctx *c) {
    char b[256];
    AcquireSRWLockExclusive(&loglock);
    for (int i = 0; i < 32; i += 4)
        fprintf(stderr, "  %s%-2d %016llx  x%-2d %016llx  x%-2d %016llx  %s%-2d %016llx\n", "x", i, c->x[i], i + 1,
                c->x[i + 1], i + 2, c->x[i + 2], i == 28 ? "sp" : "x", i == 28 ? 0 : i + 3, c->x[i + 3]);
    fprintf(stderr, "  lr = %s\n", describe_addr(c->x[30], b, sizeof b));
    fprintf(stderr, "  tpidrro %llx tpidr %llx\n", c->tpidrro, c->tpidr);
    /* the guest's frame records: x29 -> {previous x29, return address} */
    u64 fp = c->x[29];
    for (int depth = 0; depth < 24 && fp && !(fp & 7); depth++) {
        MemInfo mi;
        mem_query(fp, &mi);
        if (mi.state == MS_FREE || mi.state == MS_INACCESSIBLE || !(mi.perm & MP_R)) break;
        u64 next = LD64(fp), ret = LD64(fp + 8);
        fprintf(stderr, "  #%-2d %s\n", depth, describe_addr(ret, b, sizeof b));
        if (next <= fp) break;
        fp = next;
    }
    fflush(stderr);
    ReleaseSRWLockExclusive(&loglock);
}

void guest_trap(Ctx *c, const char *what, u64 pc) {
    char b[256];
    hlog(0, "guest trap: %s at %s", what, describe_addr(pc, b, sizeof b));
    dump_ctx(c);
    fatal("stopped");
}

void guest_unimpl(Ctx *c, u64 pc, u32 insn, const char *what) {
    char b[256];
    hlog(0, "untranslated instruction %08x (%s) at %s", insn, what, describe_addr(pc, b, sizeof b));
    dump_ctx(c);
    fatal("stopped");
}

/* the guest instruction behind a host address in lifted code (its line number is the guest offset), or 0 */
static u64 guest_pc_of(DWORD64 rip) {
    IMAGEHLP_LINE64 line;
    memset(&line, 0, sizeof line);
    line.SizeOfStruct = sizeof line;
    DWORD disp = 0;
    if (!SymGetLineFromAddr64(GetCurrentProcess(), rip, &disp, &line) || !line.FileName) return 0;
    const char *f = strrchr(line.FileName, '\\');
    f = f ? f + 1 : line.FileName;
    for (int i = 0; i < num_loaded_mods; i++)
        if (!strcmp(f, loaded_mods[i].name)) return loaded_mods[i].base + line.LineNumber;
    return 0;
}

/* NX_WATCH_MEM=addr:len@ms: from ms milliseconds after start, every write to [addr, addr+len) is logged with the
 * writer's guest instruction, the old value and the host stack (NX_WATCH_DEPTH frames; the first 200 writes). The
 * pages around it are read-only (from when they are first mapped) until a write, which single-steps with them
 * writable and protects them again; other writes to those pages are logged once per writing instruction, with
 * NX_WATCH_PAGE set. Host reads into them through the kernel (ReadFile) fail instead (romfs reads then copy).
 * GPU writes (imported guest memory) are not seen */
static u64 watch_lo, watch_hi, watch_a, watch_n;
static volatile long watch_hits;

static unsigned __stdcall watch_arm(void *p) {
    Sleep((DWORD)(uintptr_t)p);
    DWORD old;
    while (!VirtualProtect((void *)(uintptr_t)watch_lo, watch_hi - watch_lo, PAGE_READONLY, &old)) Sleep(1);
    hlog(0, "watch: %llx+%llx armed", watch_a, watch_n);
    return 0;
}

static void watch_stack(void) {
    char sb[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)sb;
    memset(sb, 0, sizeof sb);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    void *frames[24];
    USHORT nf = RtlCaptureStackBackTrace(3, getenv("NX_WATCH_DEPTH") ? (USHORT)atoi(getenv("NX_WATCH_DEPTH")) : 24, frames, NULL);
    for (USHORT i = 0; i < nf; i++) {
        DWORD64 d = 0;
        if (SymFromAddr(GetCurrentProcess(), (DWORD64)frames[i], &d, si))
            hlog(0, "watch:   %s+%llx", si->Name, (unsigned long long)d);
    }
}

static LONG watch_fault(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    DWORD old;
    if (code == EXCEPTION_SINGLE_STEP && watch_lo) {
        VirtualProtect((void *)(uintptr_t)watch_lo, watch_hi - watch_lo, PAGE_READONLY, &old);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION || ep->ExceptionRecord->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;
    u64 a = ep->ExceptionRecord->ExceptionInformation[1];
    if (a < watch_lo || a >= watch_hi) return EXCEPTION_CONTINUE_SEARCH;
    u64 gpc = guest_pc_of(ep->ContextRecord->Rip);
    char wb[256];
    const char *who = gpc ? describe_addr(gpc, wb, sizeof wb) : "host code";
    if (a + 8 > watch_a && a < watch_a + watch_n) {      /* (writes are up to 8 bytes wide: those starting just before too) */
        if (InterlockedIncrement(&watch_hits) <= 200) {
            u64 v = *(const u64 *)(uintptr_t)(a & ~7ull);
            hlog(0, "watch: write to %llx (was %llx at %llx) at %s", a, v, a & ~7ull, who);
            watch_stack();
        }
    } else if (getenv("NX_WATCH_PAGE")) {
        static u64 seen[512];
        static int nseen;
        bool known = false;
        for (int i = 0; i < nseen && !known; i++) known = seen[i] == (gpc ? gpc : ep->ContextRecord->Rip);
        if (!known && nseen < 512) {
            seen[nseen++] = gpc ? gpc : ep->ContextRecord->Rip;
            hlog(0, "watch: write to %llx at %s", a, who);
        }
    }
    VirtualProtect((void *)(uintptr_t)watch_lo, watch_hi - watch_lo, PAGE_READWRITE, &old);
    ep->ContextRecord->EFlags |= 0x100;     /* trap after this instruction: protect again */
    return EXCEPTION_CONTINUE_EXECUTION;
}

static LONG CALLBACK crash_handler(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (watch_lo && watch_fault(ep) == EXCEPTION_CONTINUE_EXECUTION) return EXCEPTION_CONTINUE_EXECUTION;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_PRIV_INSTRUCTION && code != 0xc0000374u)
        return EXCEPTION_CONTINUE_SEARCH;
    char b[256], sb[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)sb;
    memset(sb, 0, sizeof sb);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 disp = 0, rip = ep->ContextRecord->Rip;
    const char *fn = SymFromAddr(GetCurrentProcess(), rip, &disp, si) ? si->Name : "?";
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        u64 a = ep->ExceptionRecord->ExceptionInformation[1];
        hlog(0, "access violation (%s %s) in %s+%llx", ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "writing" :
             "reading", describe_addr(a, b, sizeof b), fn, (unsigned long long)disp);
    } else {
        hlog(0, "exception %08lx in %s+%llx", code, fn, (unsigned long long)disp);
    }
    u64 gpc = guest_pc_of(rip);
    if (gpc) hlog(0, "  at guest instruction %s", describe_addr(gpc, b, sizeof b));
    /* a lifted function f_<addr> names the guest function */
    if (!strncmp(fn, "f_", 2) || !strncmp(fn, "b_", 2)) {
        u64 g = strtoull(fn + 2, NULL, 16);
        hlog(0, "  guest function %s", describe_addr(g, b, sizeof b));
    }
    /* the host frames (the handler runs on the faulting thread's stack, above them) */
    void *frames[48];
    USHORT nf = RtlCaptureStackBackTrace(1, 48, frames, NULL);
    for (USHORT i = 0; i < nf; i++) {
        DWORD64 d = 0;
        if (SymFromAddr(GetCurrentProcess(), (DWORD64)frames[i], &d, si))
            hlog(0, "  host %2u: %s+%llx", i, si->Name, (unsigned long long)d);
        else
            hlog(0, "  host %2u: %p", i, frames[i]);
    }
    if (cur_thread) {
        hlog(0, "  registers as of the last call:");
        dump_ctx(cur_thread->ctx);
    }
    dump_mem_env();
    fatal("crashed");
}

/* NX_PROFILE=<thread name>[,<thread name>...] (e.g. "main,thread 5a,thread 5b"; up to 4): samples those threads
 * about every millisecond and reports, with the CPU statistics every 600 frames, where their time went: lifted guest
 * code, our runtime or other modules (the driver), the hottest functions, and for time outside our code the runtime
 * function that called out. With NX_HITCH_MS, the samples taken during slow frames (profile_frame_end) are also
 * kept apart and reported per thread: what the game ran in the frames that dipped. */
#define PROF_MAX 262144
#define PROF_THREADS 4
static HANDLE prof_threads[PROF_THREADS];
static char prof_names[PROF_THREADS][32];
static volatile long nprof;
static HANDLE prof_thread;              /* the first one; set once prof_loop runs */
static u64 prof_top[PROF_MAX], prof_rt[PROF_MAX];
static u8 prof_who[PROF_MAX];
static volatile long prof_n;
static u64 exe_lo, exe_hi;
/* samples of slow frames, by thread (profile_frame_end) */
#define PROF_SLOW_MAX (PROF_MAX / 4)
static u64 slow_top[PROF_SLOW_MAX], slow_rt[PROF_SLOW_MAX];
static u8 slow_who[PROF_SLOW_MAX];
static long nslow, slow_frames, frame_start;

static unsigned __stdcall prof_loop(void *p) {
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -10000;
    SetWaitableTimer(timer, &due, 1, NULL, NULL, FALSE);
    for (;;) {
        WaitForSingleObject(timer, INFINITE);
        for (long k = 0; k < nprof; k++) {
            CONTEXT ctx = {0};
            ctx.ContextFlags = CONTEXT_FULL;
            HANDLE th = prof_threads[k];
            if (SuspendThread(th) == (DWORD)-1) continue;
            if (GetThreadContext(th, &ctx) && prof_n < PROF_MAX) {
                u64 top = ctx.Rip, rt = 0;
                for (int i = 0; i < 24 && ctx.Rip; i++) {
                    if (ctx.Rip >= exe_lo && ctx.Rip < exe_hi && !(ctx.Rip >= lifted_lo && ctx.Rip < lifted_hi)) { rt = ctx.Rip; break; }
                    if (ctx.Rip >= lifted_lo && ctx.Rip < lifted_hi) break;     /* in lifted code: no runtime caller */
                    DWORD64 base = 0;
                    RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
                    if (!rf) { ctx.Rip = *(DWORD64 *)ctx.Rsp; ctx.Rsp += 8; continue; }
                    void *hd = NULL;
                    DWORD64 frame = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &hd, &frame, NULL);
                }
                long i = prof_n;
                prof_top[i] = top;
                prof_rt[i] = rt;
                prof_who[i] = (u8)k;
                prof_n = i + 1;
            }
            ResumeThread(th);
        }
    }
    return 0;
}

void profile_start(void *host, const char *name) {
    if (!host || nprof >= PROF_THREADS) return;
    for (long k = 0; k < nprof; k++)
        if (!strcmp(prof_names[k], name)) return;
    if (!prof_thread) {
        MODULEINFO mi;
        GetModuleInformation(GetCurrentProcess(), GetModuleHandleA(NULL), &mi, sizeof mi);
        exe_lo = (u64)(uintptr_t)mi.lpBaseOfDll;
        exe_hi = exe_lo + mi.SizeOfImage;
    }
    long k = nprof;
    snprintf(prof_names[k], sizeof prof_names[k], "%s", name);
    DuplicateHandle(GetCurrentProcess(), (HANDLE)host, GetCurrentProcess(), &prof_threads[k], 0, FALSE, DUPLICATE_SAME_ACCESS);
    nprof = k + 1;
    if (!prof_thread) {
        prof_thread = prof_threads[0];
        _beginthreadex(NULL, 0, prof_loop, NULL, 0, NULL);
    }
}

/* the end of a presented frame (gpu.c, on the render thread, as profile_report): a slow frame's samples are kept */
void profile_frame_end(bool slow) {
    long end = prof_n;
    if (!prof_thread) return;
    if (slow) {
        slow_frames++;
        for (long i = frame_start; i < end && nslow < PROF_SLOW_MAX; i++) {
            slow_top[nslow] = prof_top[i];
            slow_rt[nslow] = prof_rt[i];
            slow_who[nslow++] = prof_who[i];
        }
    }
    frame_start = end;
}

typedef struct { char name[96]; long n; } ProfEnt;
static int prof_cmp(const void *a, const void *b) { return ((const ProfEnt *)b)->n - ((const ProfEnt *)a)->n; }

static void prof_name(u64 rip, char *out, size_t n) {
    char sb[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)sb;
    memset(sb, 0, sizeof sb);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    DWORD64 d = 0;
    if (rip >= lifted_lo && rip < lifted_hi) {           /* lifted: the guest function */
        u64 g = guest_pc_of(rip);
        char b[160];
        if (SymFromAddr(GetCurrentProcess(), rip, &d, si)) snprintf(out, n, "%s", si->Name);
        else snprintf(out, n, "%s", g ? describe_addr(g, b, sizeof b) : "lifted ?");
        return;
    }
    if (SymFromAddr(GetCurrentProcess(), rip, &d, si)) { snprintf(out, n, "%s", si->Name); return; }
    HMODULE m = NULL;
    char mod[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)(uintptr_t)rip, &m)) {
        GetModuleFileNameA(m, mod, sizeof mod);
    }
    const char *slash = strrchr(mod, '\\');
    snprintf(out, n, "[%s]", slash ? slash + 1 : mod);
}

/* lines: name addresses by function and source line; 1: runtime return addresses (the call that went outside), only
 * samples whose innermost frame is outside the exe; 2: innermost addresses in the runtime only */
static void prof_table(const char *title, const u64 *rips, long n, int show, int lines) {
    ProfEnt *e = calloc(4096, sizeof *e);
    int ne = 0;
    for (long i = 0; i < n; i++) {
        if (!rips[i]) continue;
        char nm[96];
        if (lines) {
            bool in_rt = prof_top[i] >= exe_lo && prof_top[i] < exe_hi && !(prof_top[i] >= lifted_lo && prof_top[i] < lifted_hi);
            if (lines == 1 ? prof_top[i] >= exe_lo && prof_top[i] < exe_hi : !in_rt) continue;
            /* the innermost inlined function at the call (a static function inlined into its caller) */
            HANDLE pr = GetCurrentProcess();
            u64 a = rips[i] - (lines == 1);
            IMAGEHLP_LINE64 line = {sizeof line};
            DWORD disp = 0, ictx = 0, iidx = 0;
            bool ok = false;
            prof_name(rips[i], nm, sizeof nm);
            if (SymAddrIncludeInlineTrace(pr, a) && SymQueryInlineTrace(pr, a, 0, a, a, &ictx, &iidx)) {
                char sb[sizeof(SYMBOL_INFO) + 96];
                SYMBOL_INFO *si = (SYMBOL_INFO *)sb;
                memset(sb, 0, sizeof sb);
                si->SizeOfStruct = sizeof(SYMBOL_INFO);
                si->MaxNameLen = 95;
                DWORD64 d64 = 0;
                if (SymFromInlineContext(pr, a, ictx, &d64, si)) snprintf(nm, sizeof nm, "%s", si->Name);
                ok = SymGetLineFromInlineContext(pr, a, ictx, 0, &disp, &line);
            }
            if (!ok) ok = SymGetLineFromAddr64(pr, a, &disp, &line);
            if (ok) snprintf(nm + strlen(nm), sizeof nm - strlen(nm), ":%lu", line.LineNumber);
        } else prof_name(rips[i], nm, sizeof nm);
        int k = 0;
        while (k < ne && strcmp(e[k].name, nm)) k++;
        if (k == ne && ne < 4096) { snprintf(e[ne].name, sizeof e[ne].name, "%s", nm); ne++; }
        if (k < ne) e[k].n++;
    }
    qsort(e, ne, sizeof *e, prof_cmp);
    hlog(0, "profile: %s", title);
    for (int k = 0; k < ne && k < show; k++) hlog(0, "profile:   %5.1f%%  %s", 100.0 * e[k].n / n, e[k].name);
    free(e);
}

/* lifted code's samples by the kind of guest instruction behind them (the line tables give the guest pc). A call's
 * line covers its SAVE, the host call and its LOAD; "entry" is a function's own loads of the Ctx before its first
 * guest line. Per report window */
enum { PK_ENTRY, PK_BL, PK_BLR, PK_RET, PK_BR, PK_BRANCH, PK_EXCL, PK_BARRIER, PK_SVC, PK_LDST, PK_LDST_SIMD,
       PK_SIMD, PK_INT, PK_NK };
static const char *const pk_names[PK_NK] = {"entry (Ctx loads)", "bl (SAVE, call, LOAD)", "blr (call_ind)", "ret (SAVE)",
    "br (jump_ind / switch)", "b, b.cond, cbz, tbz", "exclusive / acquire / release", "dmb / dsb", "svc",
    "load / store", "load / store (FP/SIMD)", "FP / SIMD", "integer"};
static long pk_count[PK_NK], pk_total;

static int pk_kind(u32 w) {
    if ((w & 0xfc000000u) == 0x94000000u) return PK_BL;
    if ((w & 0xfffffc1fu) == 0xd63f0000u) return PK_BLR;
    if ((w & 0xfffffc1fu) == 0xd65f0000u) return PK_RET;
    if ((w & 0xfffffc1fu) == 0xd61f0000u) return PK_BR;
    if ((w & 0xfc000000u) == 0x14000000u || (w & 0xff000010u) == 0x54000000u || (w & 0x7e000000u) == 0x34000000u ||
        (w & 0x7e000000u) == 0x36000000u) return PK_BRANCH;
    if ((w & 0x3f000000u) == 0x08000000u) return PK_EXCL;
    if ((w & 0xfffff09fu) == 0xd503309fu) return PK_BARRIER;
    if ((w & 0xffe0001fu) == 0xd4000001u) return PK_SVC;
    if ((w & 0x0a000000u) == 0x08000000u) return w & 0x04000000u ? PK_LDST_SIMD : PK_LDST;
    if ((w & 0x0e000000u) == 0x0e000000u) return PK_SIMD;
    return PK_INT;
}

void profile_report(void) {
    long n = prof_n;
    if (!prof_thread || n < 100) return;
    for (long i = 0; i < n; i++) {
        u64 r = prof_top[i];
        if (r < lifted_lo || r >= lifted_hi) continue;
        u64 g = guest_pc_of(r);
        pk_count[g ? pk_kind(*(const u32 *)(uintptr_t)g) : PK_ENTRY]++;
        pk_total++;
    }
    if (pk_total) {
        char b[512];
        int o = snprintf(b, sizeof b, "profile: lifted samples by guest instruction (%ld):", pk_total);
        for (int k = 0; k < PK_NK; k++)
            if (pk_count[k] && o < (int)sizeof b)
                o += snprintf(b + o, sizeof b - o, " %s %.1f%%;", pk_names[k], 100.0 * pk_count[k] / pk_total);
        hlog(0, "%s", b);
        memset(pk_count, 0, sizeof pk_count);
        pk_total = 0;
    }
    long lifted = 0, runtime = 0, other = 0;
    for (long i = 0; i < n; i++) {
        u64 r = prof_top[i];
        if (r >= lifted_lo && r < lifted_hi) lifted++;
        else if (r >= exe_lo && r < exe_hi) runtime++;
        else other++;
    }
    hlog(0, "profile: %ld samples: lifted guest code %.1f%%, runtime %.1f%%, other modules %.1f%%", n,
         100.0 * lifted / n, 100.0 * runtime / n, 100.0 * other / n);
    prof_table("innermost function", prof_top, n, 25, 0);
    prof_table("runtime function calling outside code (driver, CRT, OS)", prof_rt, n, 15, 0);
    prof_table("runtime call sites, outside code only (function:line)", prof_rt, n, 25, 1);
    prof_table("runtime lines, innermost (function:line)", prof_top, n, 40, 2);
    if (nslow) {
        static u64 one[PROF_SLOW_MAX], one_rt[PROF_SLOW_MAX];
        for (long k = 0; k < nprof; k++) {
            long m = 0, lifted_s = 0, other_s = 0;
            for (long i = 0; i < nslow; i++)
                if (slow_who[i] == k) {
                    u64 r = slow_top[i];
                    one_rt[m] = slow_rt[i];
                    one[m++] = r;
                    if (r >= lifted_lo && r < lifted_hi) lifted_s++;
                    else if (r < exe_lo || r >= exe_hi) other_s++;
                }
            if (m < 20) continue;
            char title[200];
            snprintf(title, sizeof title, "slow frames (%ld): %s, %ld samples (lifted %.0f%%, outside the exe %.0f%%), "
                     "innermost function", slow_frames, prof_names[k], m, 100.0 * lifted_s / m, 100.0 * other_s / m);
            prof_table(title, one, m, 25, 0);
            snprintf(title, sizeof title, "slow frames: %s, runtime function calling outside code", prof_names[k]);
            prof_table(title, one_rt, m, 12, 0);
        }
    }
    nslow = slow_frames = 0;
    prof_n = 0;
    frame_start = 0;
}

/* a host thread's whole native stack (suspends it, unwinds with the x64 unwind tables): where a thread blocked in
 * host code (a service, a lock) is waiting */
void host_stack(void *thread) {
    HANDLE h = (HANDLE)thread;
    CONTEXT ctx = {0};
    ctx.ContextFlags = CONTEXT_FULL;
    if (SuspendThread(h) == (DWORD)-1) return;
    if (GetThreadContext(h, &ctx)) {
        char sb[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *)sb;
        for (int i = 0; i < 32 && ctx.Rip; i++) {
            memset(sb, 0, sizeof sb);
            si->SizeOfStruct = sizeof(SYMBOL_INFO);
            si->MaxNameLen = 255;
            DWORD64 disp = 0;
            u64 gpc = guest_pc_of(ctx.Rip);
            char b[160];
            if (gpc) hlog(0, "  host %2d: guest %s", i, describe_addr(gpc, b, sizeof b));
            else if (SymFromAddr(GetCurrentProcess(), ctx.Rip, &disp, si))
                hlog(0, "  host %2d: %s+%llx", i, si->Name, (unsigned long long)disp);
            else hlog(0, "  host %2d: %llx", i, (unsigned long long)ctx.Rip);
            DWORD64 base = 0;
            RUNTIME_FUNCTION *rf = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
            if (!rf) {                                  /* a leaf function: the return address is at rsp */
                ctx.Rip = *(DWORD64 *)ctx.Rsp;
                ctx.Rsp += 8;
                continue;
            }
            void *hd = NULL;
            DWORD64 frame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &hd, &frame, NULL);
        }
    }
    ResumeThread(h);
}

/* NX_WATCH_MEM armed while running (mods: host->watch_mem). A range already watched is let go first */
void diag_watch(u64 a, u64 n) {
    DWORD old;
    if (watch_lo) VirtualProtect((void *)(uintptr_t)watch_lo, watch_hi - watch_lo, PAGE_READWRITE, &old);
    watch_lo = 0;
    if (!n) return;
    watch_a = a;
    watch_n = n;
    watch_hits = 0;
    u64 lo = a & ~0xfffull, hi = (a + n + 0xfff) & ~0xfffull;
    watch_hi = hi;
    watch_lo = lo;
    if (!VirtualProtect((void *)(uintptr_t)lo, hi - lo, PAGE_READONLY, &old)) {
        hlog(0, "watch: %llx+%llx could not be armed (%lu)", a, n, GetLastError());
        watch_lo = 0;
        return;
    }
    hlog(0, "watch: %llx+%llx armed", a, n);
}

void diag_init(void) {
    QueryPerformanceFrequency(&qpc_freq);
    QueryPerformanceCounter(&qpc_start);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, crash_handler);
    const char *w = getenv("NX_WATCH_MEM");
    if (w) {
        char *end;
        u64 a = strtoull(w, &end, 16), n = *end == ':' ? strtoull(end + 1, &end, 16) : 8;
        u32 ms = *end == '@' ? (u32)strtoul(end + 1, NULL, 10) : 0;
        watch_a = a;
        watch_n = n;
        watch_lo = a & ~0xfffull;
        watch_hi = (a + n + 0xfff) & ~0xfffull;
        _beginthreadex(NULL, 0, watch_arm, (void *)(uintptr_t)ms, 0, NULL);
    }
}
