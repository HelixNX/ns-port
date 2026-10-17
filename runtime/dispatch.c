/* What the lifted code calls into: indirect calls and branches (one table slot per text word, so a lookup is an
 * index), host functions at guest addresses ("host calls"), traps, the counter and FPCR, and the crash report. */
#include "hos.h"
#include <stdlib.h>
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>

extern const ModInfo lifted_modules[];

typedef struct { u64 base, span; GuestFn *tab; const char *name; } FnTable;
static FnTable tables[4];
static int ntables;

static struct { const char *name; GuestFn fn; } hostcalls[HOSTCALL_MAX];
static int nhostcalls;

void dispatch_init(void) {
    for (const ModInfo *m = lifted_modules; m->name; m++) {
        FnTable *t = &tables[ntables++];
        t->base = m->base;
        t->span = m->text_end - m->base;
        t->name = m->name;
        t->tab = calloc(t->span / 4, sizeof(GuestFn));
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

/* setjmp/longjmp (a64rt.h): the host jump buffers of each thread's latest setjmp calls, by guest jmp_buf */
#define HOST_JMPS 16
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
        j = &host_jmps[host_jmp_next++ % HOST_JMPS];
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

static LONG CALLBACK crash_handler(EXCEPTION_POINTERS *ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
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
    fatal("crashed");
}

/* the lifted function a host thread is executing (suspends it briefly) */
void host_where(void *thread, char *buf, size_t n) {
    HANDLE h = (HANDLE)thread;
    CONTEXT ctx = {0};
    ctx.ContextFlags = CONTEXT_CONTROL;
    snprintf(buf, n, "?");
    if (SuspendThread(h) == (DWORD)-1) return;
    if (GetThreadContext(h, &ctx)) {
        char sb[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *si = (SYMBOL_INFO *)sb;
        memset(sb, 0, sizeof sb);
        si->SizeOfStruct = sizeof(SYMBOL_INFO);
        si->MaxNameLen = 255;
        DWORD64 disp = 0;
        u64 gpc = guest_pc_of(ctx.Rip);
        char b[160];
        if (gpc) {
            snprintf(buf, n, "%s", describe_addr(gpc, b, sizeof b));
        } else if (SymFromAddr(GetCurrentProcess(), ctx.Rip, &disp, si)) {
            if (!strncmp(si->Name, "f_", 2) || !strncmp(si->Name, "b_", 2))
                snprintf(buf, n, "%s", describe_addr(strtoull(si->Name + 2, NULL, 16), b, sizeof b));
            else
                snprintf(buf, n, "%s+%llx", si->Name, (unsigned long long)disp);
        }
    }
    ResumeThread(h);
}

void diag_init(void) {
    QueryPerformanceFrequency(&qpc_freq);
    QueryPerformanceCounter(&qpc_start);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, crash_handler);
}
