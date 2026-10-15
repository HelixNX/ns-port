/* Services in-process: a session handle names a KSession, whose (domain) objects are service objects; a request in
 * the thread's TLS (HIPC framing, CMIF payload) is decoded into an IpcReq, handled synchronously by the object's
 * interface and the reply written back to the TLS. The guest's buffers are used in place (identity mapping). */
#ifndef IPC_H
#define IPC_H

#include "hos.h"

typedef struct IpcBuf { u64 addr, size; u32 flags; } IpcBuf;

typedef struct SvcObj SvcObj;

typedef struct IpcReq {
    u32 type, cmd, token;
    bool has_pid;
    u64 pid;
    u32 copy[8], move[8];
    int ncopy, nmove;
    IpcBuf x[16], a[16], b[16], w[16], c[16];
    int nx, na, nb, nw, nc;
    const u8 *in;                   /* arguments after the CMIF header */
    u32 in_size;
    u32 in_objs[8];                 /* domain object ids sent with the request */
    int nin_objs;
    /* reply */
    u32 result;
    u8 out[0x100];
    u32 out_size;
    u32 out_copy[8], out_move[8];
    int nout_copy, nout_move;
    SvcObj *out_objs[8];
    int nout_objs;
    struct KSession *session;
} IpcReq;

typedef void (*IpcHandler)(SvcObj *self, IpcReq *r);
typedef struct IpcCmd { u32 id; IpcHandler fn; const char *name; } IpcCmd;
typedef struct Iface {
    const char *name;
    const IpcCmd *cmds;             /* ends with {0, NULL} */
    void (*destroy)(SvcObj *self);  /* optional */
} Iface;

struct SvcObj {
    const Iface *iface;
    volatile long refs;
    void *data;
    char name[48];                  /* for logs: the service or interface name */
};

SvcObj *svcobj_new(const Iface *iface, void *data, const char *name);
void svcobj_ref(SvcObj *o);
void svcobj_unref(SvcObj *o);

/* services that sm:GetService can open: create returns a new root object */
void service_register(const char *name, SvcObj *(*create)(const char *name));
SvcObj *service_open(const char *name);

/* request arguments (offset into the raw arguments) and reply */
static inline u64 in_u64(IpcReq *r, u32 off) { u64 v = 0; if (off + 8 <= r->in_size) memcpy(&v, r->in + off, 8); return v; }
static inline u32 in_u32(IpcReq *r, u32 off) { u32 v = 0; if (off + 4 <= r->in_size) memcpy(&v, r->in + off, 4); return v; }
static inline u8 in_u8(IpcReq *r, u32 off) { return off < r->in_size ? r->in[off] : 0; }
void out_data(IpcReq *r, const void *p, u32 size);          /* appends, aligned to min(size, 8) */
static inline void out_u64(IpcReq *r, u64 v) { out_data(r, &v, 8); }
static inline void out_u32(IpcReq *r, u32 v) { out_data(r, &v, 4); }
static inline void out_u8(IpcReq *r, u8 v) { out_data(r, &v, 1); }
void out_object(IpcReq *r, SvcObj *o);                      /* a sub-interface: a session, or a domain object */
void out_copy_handle(IpcReq *r, u32 h);
void out_move_handle(IpcReq *r, u32 h);
void out_event(IpcReq *r, KEvent *e);                       /* copies a new handle to e */

/* a stub: logs the command and replies with success and no data */
void ipc_stub(SvcObj *self, IpcReq *r);
extern const Iface iface_stub;
/* success, no data, no log */
void ipc_noop(SvcObj *self, IpcReq *r);

/* a command that opens a sub-interface: IPC_OPEN(open_foo, &iface_foo) */
#define IPC_OPEN(fn, iface) static void fn(SvcObj *self, IpcReq *r) { (void)self; out_object(r, svcobj_new(iface, NULL, NULL)); }
#define IPC_END {0, NULL, NULL}

/* the services (runtime/services/<name>.c) */
void services_init(const char *game_dir, const char *save_dir);
void am_register(void);
void lm_register(void);
void apm_register(void);
void pctl_register(void);
void vi_register(void);
void fs_register(const char *game_dir, const char *save_dir);

#endif
