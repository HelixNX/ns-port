/* The SVCs the sdk uses (57 numbers; see CLAUDE.md), with the console's register conventions: arguments in
 * x0..x7, the result in w0, outputs from x1 on. */
#include "hos.h"
#include <stdlib.h>
#include <windows.h>
#include <bcrypt.h>

#define PROGRAM_ID 0x0100152000022000ull

static const char *svc_name(u32 n) {
    static const char *const names[0x80] = {
        [0x01] = "SetHeapSize", [0x02] = "SetMemoryPermission", [0x03] = "SetMemoryAttribute", [0x04] = "MapMemory",
        [0x05] = "UnmapMemory", [0x06] = "QueryMemory", [0x07] = "ExitProcess", [0x08] = "CreateThread",
        [0x09] = "StartThread", [0x0a] = "ExitThread", [0x0b] = "SleepThread", [0x0c] = "GetThreadPriority",
        [0x0d] = "SetThreadPriority", [0x0e] = "GetThreadCoreMask", [0x0f] = "SetThreadCoreMask",
        [0x10] = "GetCurrentProcessorNumber", [0x11] = "SignalEvent", [0x12] = "ClearEvent",
        [0x13] = "MapSharedMemory", [0x14] = "UnmapSharedMemory", [0x15] = "CreateTransferMemory",
        [0x16] = "CloseHandle", [0x17] = "ResetSignal", [0x18] = "WaitSynchronization",
        [0x19] = "CancelSynchronization", [0x1a] = "ArbitrateLock", [0x1b] = "ArbitrateUnlock",
        [0x1c] = "WaitProcessWideKeyAtomic", [0x1d] = "SignalProcessWideKey", [0x1f] = "ConnectToNamedPort",
        [0x21] = "SendSyncRequest", [0x22] = "SendSyncRequestWithUserBuffer",
        [0x23] = "SendAsyncRequestWithUserBuffer", [0x25] = "GetThreadId", [0x26] = "Break",
        [0x27] = "OutputDebugString", [0x28] = "ReturnFromException", [0x29] = "GetInfo",
        [0x2a] = "FlushEntireDataCache", [0x2c] = "MapPhysicalMemory", [0x2d] = "UnmapPhysicalMemory",
        [0x2f] = "GetLastThreadInfo", [0x32] = "SetThreadActivity", [0x33] = "GetThreadContext3",
        [0x34] = "WaitForAddress", [0x35] = "SignalToAddress", [0x40] = "CreateSession", [0x41] = "AcceptSession",
        [0x43] = "ReplyAndReceive", [0x44] = "ReplyAndReceiveWithUserBuffer", [0x45] = "CreateEvent",
        [0x50] = "CreateSharedMemory", [0x51] = "MapTransferMemory", [0x52] = "UnmapTransferMemory",
        [0x53] = "CreateInterruptEvent", [0x70] = "CreatePort", [0x72] = "ConnectToPort"};
    return n < 0x80 && names[n] ? names[n] : "?";
}

static KThread *get_thread(u32 h) { return (KThread *)handle_get(h, KO_THREAD); }

static u32 get_info(u32 id0, u32 handle, u64 id1, u64 *out) {
    switch (id0) {
    case 0: *out = 0x7; return R_OK;                                   /* core mask: cores 0-2 */
    case 1: *out = 0x0ffffffff0000000ull; return R_OK;                 /* priorities 28-59 */
    case 2: *out = ALIAS_BASE; return R_OK;
    case 3: *out = ALIAS_SIZE; return R_OK;
    case 4: *out = HEAP_BASE; return R_OK;
    case 5: *out = HEAP_MAX; return R_OK;
    case 6: *out = TOTAL_MEMORY; return R_OK;
    case 7: *out = mem_used(); return R_OK;
    case 8: *out = 0; return R_OK;                                     /* no debugger */
    case 9: *out = 0; return R_OK;                                     /* no resource limit handle */
    case 10: *out = 0; return R_OK;                                    /* idle ticks */
    case 11: BCryptGenRandom(NULL, (u8 *)out, 8, BCRYPT_USE_SYSTEM_PREFERRED_RNG); return R_OK;
    case 12: *out = AS_BASE; return R_OK;
    case 13: *out = AS_END - AS_BASE; return R_OK;
    case 14: *out = STACK_BASE; return R_OK;
    case 15: *out = STACK_SIZE; return R_OK;
    case 16: case 17: *out = 0; return R_OK;                           /* system resource */
    case 18: *out = PROGRAM_ID; return R_OK;
    case 20: *out = 0; return R_OK;                                    /* user exception context */
    case 21: *out = TOTAL_MEMORY; return R_OK;
    case 22: *out = mem_used(); return R_OK;
    case 0xf0000002: *out = 0; return R_OK;                            /* thread tick count */
    }
    hlog(0, "GetInfo(%u, %x, %llu) not implemented", id0, handle, id1);
    *out = 0;
    return R_INVALID_ENUM;
}

void svc_dispatch(Ctx *c, u32 n) {
    u64 *x = c->x;
    KThread *t = cur_thread;
    if (log_level >= 2 && n != 0x18 && n != 0x1a && n != 0x1b && n != 0x1c && n != 0x1d)
        hlog(2, "svc %s(%llx, %llx, %llx, %llx)", svc_name(n), x[0], x[1], x[2], x[3]);
    u32 r = R_OK;
    switch (n) {
    case 0x01: { u64 a = 0; r = mem_set_heap_size(x[1], &a); x[1] = a; break; }
    case 0x02: r = mem_set_perm(x[0], x[1], (u32)x[2]); break;
    case 0x03: r = mem_set_attr(x[0], x[1], (u32)x[2], (u32)x[3]); break;
    case 0x04: r = mem_map_memory(x[0], x[1], x[2]); break;
    case 0x05: r = mem_unmap_memory(x[0], x[1], x[2]); break;
    case 0x06: {
        MemInfo mi;
        mem_query(x[2], &mi);
        memcpy((void *)(uintptr_t)x[0], &mi, sizeof mi);
        x[1] = 0;
        break;
    }
    case 0x07:
        hlog(1, "ExitProcess");
        ExitProcess(0);
    case 0x08: {
        s32 prio = (s32)x[4], core = (s32)x[5];
        if (prio < 0 || prio > 63) { r = R_INVALID_PRIORITY; break; }
        if (core == -2) core = 0;
        if (core < 0 || core > 2) { r = R_INVALID_CORE; break; }
        KThread *nt = thread_create(x[1], x[2], x[3], prio, core);
        u32 h = 0;
        r = handle_new(&nt->obj, &h);
        kobj_unref(&nt->obj);
        nt->handle = h;
        x[1] = h;
        char b[200];
        hlog(1, "CreateThread %s entry %s stack %llx prio %d core %d", nt->name, describe_addr(nt->entry, b, sizeof b),
             nt->stack_top, prio, core);
        break;
    }
    case 0x09: {
        KThread *nt = get_thread((u32)x[0]);
        if (!nt) { r = R_INVALID_HANDLE; break; }
        r = thread_start(nt);
        kobj_unref(&nt->obj);
        break;
    }
    case 0x0a: thread_exit();
    case 0x0b: thread_sleep((s64)x[0]); break;
    case 0x0c: {
        KThread *o = get_thread((u32)x[1]);
        if (!o) { r = R_INVALID_HANDLE; break; }
        x[1] = (u32)o->prio;
        kobj_unref(&o->obj);
        break;
    }
    case 0x0d: {
        KThread *o = get_thread((u32)x[0]);
        if (!o) { r = R_INVALID_HANDLE; break; }
        if ((u32)x[1] > 63) r = R_INVALID_PRIORITY; else o->prio = (s32)x[1];
        kobj_unref(&o->obj);
        break;
    }
    case 0x0e: {
        KThread *o = get_thread((u32)x[2]);
        if (!o) { r = R_INVALID_HANDLE; break; }
        x[1] = (u32)o->core;
        x[2] = o->affinity;
        kobj_unref(&o->obj);
        break;
    }
    case 0x0f: {
        KThread *o = get_thread((u32)x[0]);
        if (!o) { r = R_INVALID_HANDLE; break; }
        s32 core = (s32)x[1];
        if (core == -2) core = 0;                     /* the process's ideal core; -1 and -3 keep the current one */
        thread_set_core(o, core, x[2]);
        kobj_unref(&o->obj);
        break;
    }
    case 0x10: x[0] = (u64)(t->cur_core < 0 ? 0 : t->cur_core); kernel_svc_exit(); return;
    case 0x11: case 0x12: case 0x17: {
        KEvent *e = (KEvent *)handle_get((u32)x[0], KO_EVENT);
        if (!e) { r = R_INVALID_HANDLE; break; }
        if (n == 0x11) event_signal(e);
        else if (n == 0x17 && !e->signaled) r = R_INVALID_STATE;
        else event_clear(e);
        kobj_unref(&e->obj);
        break;
    }
    case 0x13: r = shmem_map((u32)x[0], x[1], x[2], (u32)x[3]); break;
    case 0x14: r = shmem_unmap((u32)x[0], x[1], x[2]); break;
    case 0x15: { u32 h = 0; r = tmem_create(x[1], x[2], (u32)x[3], &h); x[1] = h; break; }
    case 0x16: r = handle_close((u32)x[0]); break;
    case 0x18: {
        u32 cnt = (u32)x[2];
        if (cnt > 64) { r = R_OUT_OF_RANGE; break; }
        KObj *objs[64];
        const u32 *hs = (const u32 *)(uintptr_t)x[1];
        for (u32 i = 0; i < cnt; i++) {
            objs[i] = handle_get(hs[i], KO_NONE);
            if (!objs[i] || !objs[i]->signaled) {
                if (objs[i]) kobj_unref(objs[i]);
                for (u32 k = 0; k < i; k++) kobj_unref(objs[k]);
                hlog(0, "WaitSynchronization: bad handle %x", hs[i]);
                r = R_INVALID_HANDLE;
                goto done;
            }
        }
        s32 idx = -1;
        r = thread_wait_sync(objs, (int)cnt, (s64)x[3], &idx);
        for (u32 i = 0; i < cnt; i++) kobj_unref(objs[i]);
        x[1] = (u64)(s64)idx;
        break;
    }
    case 0x19: {
        KThread *o = get_thread((u32)x[0]);
        if (!o) { r = R_INVALID_HANDLE; break; }
        r = thread_cancel_sync(o);
        kobj_unref(&o->obj);
        break;
    }
    case 0x1a: r = arbitrate_lock((u32)x[0], x[1], (u32)x[2]); break;
    case 0x1b: r = arbitrate_unlock(x[0]); break;
    case 0x1c: r = condvar_wait(x[0], x[1], (u32)x[2], (s64)x[3]); break;
    case 0x1d: condvar_signal(x[0], (s32)x[1]); break;
    case 0x1f: { u32 h = 0; r = ipc_connect_named_port((const char *)(uintptr_t)x[1], &h); x[1] = h; break; }
    /* the client waits for the server on the console: other threads may have its core meanwhile */
    case 0x21: core_leave(); r = ipc_send_sync_request((u32)x[0], t->tls, 0x100); core_enter(); break;
    case 0x22: core_leave(); r = ipc_send_sync_request((u32)x[2], x[0], x[1]); core_enter(); break;
    case 0x25: {
        KThread *o = get_thread((u32)x[1]);
        if (!o) { r = R_INVALID_HANDLE; break; }
        x[1] = o->id;
        kobj_unref(&o->obj);
        break;
    }
    case 0x26: {
        char b[200];
        hlog(0, "svcBreak(%llx, %llx, %llx) from %s", x[0], x[1], x[2], describe_addr(x[30], b, sizeof b));
        dump_ctx(c);
        if (x[0] & 0x80000000u) break;                /* a notification only */
        fatal("the guest called svcBreak");
    }
    case 0x27: {
        u64 len = x[1];
        if (len > 4096) len = 4096;
        hlog(0, "guest: %.*s", (int)len, (const char *)(uintptr_t)x[0]);
        break;
    }
    case 0x29: { u64 v = 0; r = get_info((u32)x[1], (u32)x[2], x[3], &v); x[1] = v; break; }
    case 0x2a: break;
    case 0x2c: case 0x2d: {
        if ((x[0] | x[1]) & 0xfff || x[0] < ALIAS_BASE || x[0] + x[1] > ALIAS_BASE + ALIAS_SIZE) { r = R_INVALID_ADDRESS; break; }
        if (n == 0x2c) mem_map(x[0], x[1], MS_ALIAS, MP_RW); else mem_unmap(x[0], x[1]);
        break;
    }
    case 0x34: r = address_wait(x[0], (u32)x[1], (s32)x[2], (s64)x[3]); break;
    case 0x35: r = address_signal(x[0], (u32)x[1], (s32)x[2], (s32)x[3]); break;
    case 0x45: {
        KEvent *e = event_create();
        u32 hw = 0, hr = 0;
        r = handle_new(&e->obj, &hw);
        if (!r) r = handle_new(&e->obj, &hr);
        kobj_unref(&e->obj);
        x[1] = hw;
        x[2] = hr;
        break;
    }
    default: {
        char b[200];
        hlog(0, "svc 0x%x (%s) is not implemented; called from %s", n, svc_name(n), describe_addr(x[30], b, sizeof b));
        dump_ctx(c);
        fatal("unimplemented SVC");
    }
    }
done:
    /* routine failures: timeouts, GetInfo probes, resetting an event that isn't signalled */
    if (r && log_level >= 1 && n != 0x18 && n != 0x34 && n != 0x1c && !(n == 0x29) && !(n == 0x17 && r == 0xfa01))
        hlog(1, "svc %s -> %x", svc_name(n), r);
    x[0] = r;
    kernel_svc_exit();
}
