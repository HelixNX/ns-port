/* NVN, intercepted (CLAUDE.md, decision 3): nvnBootstrapLoader and nv:: never reach the sdk's driver. Every NVN
 * function the game asks for (nvnBootstrapLoader, nvnDeviceGetProcAddress) gets a host-call address; the ones
 * implemented here run, the others stop with their name. */
#include "hos.h"
#include <stdlib.h>
#include <windows.h>

typedef struct NvnProc { const char *name; GuestFn fn; } NvnProc;

static void nvn_get_proc_address(Ctx *c);
static void bootstrap_loader(Ctx *c);

static const NvnProc procs[] = {
    {"nvnDeviceGetProcAddress", nvn_get_proc_address},
    {"nvnBootstrapLoader", bootstrap_loader},
};

/* names handed out so far, each with its host-call address */
static struct { char name[96]; u64 addr; } given[1024];
static int ngiven;
static SRWLOCK lock = SRWLOCK_INIT;

static void unimplemented(Ctx *c) {
    const char *name = "?";
    for (int i = 0; i < ngiven; i++)
        if (given[i].addr == c->pc) name = given[i].name;
    char b[200];
    hlog(0, "NVN function %s is not implemented (called from %s)", name, describe_addr(c->x[30], b, sizeof b));
    dump_ctx(c);
    fatal("unimplemented NVN function");
}

static u64 proc_address(const char *name) {
    if (!name || !*name) return 0;
    AcquireSRWLockExclusive(&lock);
    for (int i = 0; i < ngiven; i++)
        if (!strcmp(given[i].name, name)) {
            ReleaseSRWLockExclusive(&lock);
            return given[i].addr;
        }
    GuestFn fn = unimplemented;
    for (size_t i = 0; i < sizeof procs / sizeof *procs; i++)
        if (!strcmp(procs[i].name, name)) fn = procs[i].fn;
    if (ngiven == 1024) fatal("too many NVN names");
    snprintf(given[ngiven].name, sizeof given[ngiven].name, "%s", name);
    u64 a = hostcall_register(given[ngiven].name, fn);
    given[ngiven++].addr = a;
    ReleaseSRWLockExclusive(&lock);
    hlog(2, "nvn: %s -> %llx", name, a);
    return a;
}

static void bootstrap_loader(Ctx *c) { c->x[0] = proc_address((const char *)(uintptr_t)c->x[0]); }
static void nvn_get_proc_address(Ctx *c) { c->x[0] = proc_address((const char *)(uintptr_t)c->x[1]); }

/* the sdk entry points main calls (a64lift overrides, see tools/lifter/overrides.txt) */
void h_nvnBootstrapLoader(Ctx *c) { bootstrap_loader(c); }
void h__ZN2nv18InitializeGraphicsEPvm(Ctx *c) {
    hlog(1, "nv::InitializeGraphics(%llx, %llx): ignored", c->x[0], c->x[1]);
}
void h__ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_(Ctx *c) {}
void h__ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_(Ctx *c) {}

/* the same functions reached through pointers */
void nvn_init(void) {
    static const struct { const char *sym; GuestFn fn; } ov[] = {
        {"nvnBootstrapLoader", h_nvnBootstrapLoader},
        {"_ZN2nv18InitializeGraphicsEPvm", h__ZN2nv18InitializeGraphicsEPvm},
        {"_ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_",
         h__ZN2nv20SetGraphicsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_},
        {"_ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_",
         h__ZN2nv28SetGraphicsDevtoolsAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_},
    };
    for (size_t i = 0; i < sizeof ov / sizeof *ov; i++) {
        u64 a = sym_lookup(ov[i].sym);
        if (a) dispatch_override(a, ov[i].fn);
    }
}
