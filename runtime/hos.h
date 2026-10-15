/* The runtime under the lifted code: a Horizon-like kernel for one process (memory, threads, synchronisation,
 * SVCs) and its services, all in-process. Guest addresses are host addresses (see a64rt.h). */
#ifndef HOS_H
#define HOS_H

#include "a64rt.h"
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------------------------- address space
 * A 39-bit address space as svcGetInfo reports it. Everything the guest can map lives in RES_BASE..RES_END,
 * which is reserved from Windows at start-up, so that pages can be committed at 4 KB granularity. */
#define AS_BASE         0x1000000000ull     /* "ASLR" region: everything the guest may see */
#define AS_END          0x8000000000ull
#define ALIAS_BASE      0x1000000000ull
#define ALIAS_SIZE      0x1000000000ull
#define HEAP_BASE       0x2000000000ull
#define HEAP_MAX        0x0180000000ull
#define STACK_BASE      0x2200000000ull     /* svcMapMemory destinations (thread stacks) */
#define STACK_SIZE      0x0080000000ull
#define TLS_BASE        0x2280000000ull     /* thread-local pages, one 0x200 slot per thread */
#define TLS_SIZE        0x0001000000ull
#define MAINSTACK_BASE  0x2290000000ull     /* the main thread's stack */
#define MAINSTACK_MAX   0x0010000000ull
#define RES_BASE        ALIAS_BASE
#define RES_END         (MAINSTACK_BASE + MAINSTACK_MAX)
#define MODULES_BASE    0x7100000000ull     /* modules at a64lift.BASES */
#define MODULES_END     0x7103000000ull
#define MISC_BASE       0x7103000000ull     /* one page of loader data (rtld's lists) */
#define HOSTCALL_BASE   0x7104000000ull     /* guest addresses of host functions (never mapped) */
#define HOSTCALL_MAX    256

#define TOTAL_MEMORY    0xcd500000ull       /* the application pool reported to the game */

/* memory states (the low byte, as svcQueryMemory reports it) and permissions */
enum { MS_FREE = 0, MS_IO = 1, MS_NORMAL = 2, MS_CODE = 3, MS_CODEDATA = 4, MS_HEAP = 5, MS_SHARED = 6,
       MS_ALIAS = 7, MS_STACK = 0xb, MS_TLS = 0xc, MS_TRANSFERRED = 0xd, MS_INACCESSIBLE = 0x10 };
enum { MP_NONE = 0, MP_R = 1, MP_W = 2, MP_X = 4, MP_RW = 3, MP_RX = 5 };

typedef struct MemInfo { u64 addr, size; u32 state, attr, perm, ipc_refs, dev_refs, pad; } MemInfo;

void mem_init(void);
void mem_map(u64 addr, u64 size, u32 state, u32 perm);         /* commit pages (zeroed) and record them */
void mem_unmap(u64 addr, u64 size);                             /* decommit and mark free */
void mem_set_block(u64 addr, u64 size, u32 state, u32 perm);  /* bookkeeping only */
void mem_protect(u64 addr, u64 size, u32 perm);               /* host protection only */
bool mem_query(u64 addr, MemInfo *out);
bool mem_is_free(u64 addr, u64 size);
u64 mem_heap_size(void);
u32 mem_set_heap_size(u64 size, u64 *addr);
u32 mem_map_memory(u64 dst, u64 src, u64 size);
u32 mem_unmap_memory(u64 dst, u64 src, u64 size);
u32 mem_set_perm(u64 addr, u64 size, u32 perm);
u32 mem_set_attr(u64 addr, u64 size, u32 mask, u32 value);
u64 mem_used(void);

/* ---------------------------------------------------------------------------------------------- results */
#define R_OK                0u
#define KRES(d)             (0x01u | (u32)(d) << 9)
#define R_INVALID_SIZE      KRES(101)
#define R_INVALID_ADDRESS   KRES(102)
#define R_OUT_OF_RESOURCE   KRES(103)
#define R_OUT_OF_MEMORY     KRES(104)
#define R_OUT_OF_HANDLES    KRES(105)
#define R_INVALID_MEMSTATE  KRES(106)
#define R_INVALID_PERM      KRES(108)
#define R_INVALID_RANGE     KRES(110)
#define R_INVALID_PRIORITY  KRES(112)
#define R_INVALID_CORE      KRES(113)
#define R_INVALID_HANDLE    KRES(114)
#define R_INVALID_POINTER   KRES(115)
#define R_INVALID_COMBO     KRES(116)
#define R_TIMED_OUT         KRES(117)
#define R_CANCELLED         KRES(118)
#define R_OUT_OF_RANGE      KRES(119)
#define R_INVALID_ENUM      KRES(120)
#define R_NOT_FOUND         KRES(121)
#define R_INVALID_STATE     KRES(125)
#define R_SESSION_CLOSED    KRES(123)
#define R_LIMIT_REACHED     KRES(132)

/* ---------------------------------------------------------------------------------------------- objects */
typedef enum { KO_NONE, KO_THREAD, KO_EVENT, KO_SESSION, KO_SHMEM, KO_TMEM, KO_PROCESS, KO_RESLIMIT, KO_PORT } KType;

struct KThread;
typedef struct KObj {
    KType type;
    volatile long refs;
    void (*destroy)(struct KObj *);
    /* synchronisation (waitable objects): `signaled` says whether a wait on it completes now */
    bool (*signaled)(struct KObj *);
    struct KThread **waiters;
    int nwaiters, capwaiters;
} KObj;

void kobj_init(KObj *o, KType type, void (*destroy)(KObj *), bool (*signaled)(KObj *));
void kobj_ref(KObj *o);
void kobj_unref(KObj *o);
void kobj_notify(KObj *o);                          /* wake the threads waiting on o (klock held) */

/* handles: (generation << 15) | index; the pseudo-handles name the current process and thread */
#define H_CUR_PROCESS 0xffff8001u
#define H_CUR_THREAD  0xffff8000u
u32 handle_new(KObj *o, u32 *out);                  /* takes a reference */
KObj *handle_get(u32 h, KType type);                /* adds a reference, or NULL (type KO_NONE: any) */
u32 handle_close(u32 h);

/* ---------------------------------------------------------------------------------------------- threads */
enum { W_NONE, W_SYNC, W_MUTEX, W_CONDVAR, W_ARBITER, W_SLEEP };

typedef struct KThread {
    KObj obj;                       /* signalled once the thread has exited */
    Ctx *ctx;
    u64 id, entry, arg, stack_top, tls;
    s32 prio, core;
    u64 affinity;
    u32 handle;                     /* the handle the creator got (used as the mutex tag by the SDK) */
    void *host;                     /* Windows thread handle */
    u32 host_id;
    bool started, exited, cancel;
    /* wait state, under klock */
    int wait;
    bool woken;
    u32 wait_result;
    u64 wait_seq;
    u64 wait_addr, cv_key;
    u32 tag;
    KObj *wake_obj;
    void *cv;                       /* CONDITION_VARIABLE */
    struct KThread *next;           /* all threads */
    char name[32];
} KThread;

extern __declspec(thread) KThread *cur_thread;
void kernel_init(void);
void klock(void);
void kunlock(void);
KThread *thread_create(u64 entry, u64 arg, u64 stack_top, s32 prio, s32 core);
u32 thread_start(KThread *t);
__declspec(noreturn) void thread_exit(void);
u32 thread_wait_sync(KObj **objs, int n, s64 timeout_ns, s32 *index);
u32 thread_cancel_sync(KThread *t);
void thread_sleep(s64 ns);
u32 arbitrate_lock(u32 owner, u64 addr, u32 tag);
u32 arbitrate_unlock(u64 addr);
u32 condvar_wait(u64 mutex, u64 key, u32 tag, s64 timeout_ns);
void condvar_signal(u64 key, s32 count);
u32 address_wait(u64 addr, u32 type, s32 value, s64 timeout_ns);
u32 address_signal(u64 addr, u32 type, s32 value, s32 count);
void run_guest_main(KThread *t);

/* events: one object, two handles (the writable and the readable end) */
typedef struct KEvent { KObj obj; bool signaled; bool autoclear; } KEvent;
KEvent *event_create(void);
void event_signal(KEvent *e);
void event_clear(KEvent *e);

/* ---------------------------------------------------------------------------------------------- SVCs, IPC */
void svc_dispatch(Ctx *c, u32 imm);
u32 ipc_connect_named_port(const char *name, u32 *out);
u32 ipc_send_sync_request(u32 handle, u64 msg, u64 size);

/* ---------------------------------------------------------------------------------------------- loader */
typedef struct LoadedMod {
    const char *name;
    u64 base, text_end, ro_start, ro_end, data_start, data_end, bss_end, dt_init, dt_fini;
    u32 nsym;
    const struct ImgSym *syms;
    const char *strtab;
} LoadedMod;
typedef struct ImgSym { u64 addr; u32 name, size; } ImgSym;

extern LoadedMod loaded_mods[3];
extern int num_loaded_mods;
void loader_load(const char *image_dir);
u64 sym_lookup(const char *name);                   /* first definition in load order, or 0 */
const char *sym_name(u64 addr, u64 *offset);        /* nearest symbol at or below addr, or NULL */

/* ---------------------------------------------------------------------------------------------- dispatch */
void dispatch_init(void);
void diag_init(void);
u64 hostcall_register(const char *name, GuestFn fn);  /* returns the guest address that calls fn */
GuestFn guest_fn(u64 addr);                            /* lifted function or host call at addr, or NULL */
void guest_call(Ctx *c, u64 addr);                      /* call_ind without the guest's LR */
void host_thread_init(Ctx *c);                         /* per host thread: FP state */

/* ---------------------------------------------------------------------------------------------- logging */
extern int log_level;                                  /* 0 errors, 1 info, 2 trace (SVCs, IPC) */
void hlog(int level, const char *fmt, ...);
__declspec(noreturn) void fatal(const char *fmt, ...);
void dump_ctx(Ctx *c);
const char *describe_addr(u64 addr, char *buf, size_t n);

#endif
