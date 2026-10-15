/* HIPC framing, CMIF requests, sessions and domains, and the service manager (sm:). */
#include "ipc.h"
#include <stdlib.h>

/* ---------------------------------------------------------------------------------------------- service objects */
SvcObj *svcobj_new(const Iface *iface, void *data, const char *name) {
    SvcObj *o = calloc(1, sizeof *o);
    o->iface = iface;
    o->refs = 1;
    o->data = data;
    snprintf(o->name, sizeof o->name, "%s", name ? name : iface->name);
    return o;
}

void svcobj_ref(SvcObj *o) { __atomic_add_fetch(&o->refs, 1, __ATOMIC_SEQ_CST); }

void svcobj_unref(SvcObj *o) {
    if (__atomic_sub_fetch(&o->refs, 1, __ATOMIC_SEQ_CST) == 0) {
        if (o->iface->destroy) o->iface->destroy(o);
        free(o);
    }
}

#define MAX_SERVICES 128
static struct { char name[16]; SvcObj *(*create)(const char *); } services[MAX_SERVICES];
static int nservices;

void service_register(const char *name, SvcObj *(*create)(const char *)) {
    if (nservices == MAX_SERVICES) fatal("too many services");
    snprintf(services[nservices].name, sizeof services[nservices].name, "%s", name);
    services[nservices++].create = create;
}

SvcObj *service_open(const char *name) {
    for (int i = 0; i < nservices; i++)
        if (!strcmp(services[i].name, name)) return services[i].create(name);
    hlog(0, "service %s is not implemented: using a stub", name);
    return svcobj_new(&iface_stub, NULL, name);
}

/* ---------------------------------------------------------------------------------------------- sessions */
typedef struct KSession {
    KObj obj;
    SvcObj *root;
    bool domain;
    SvcObj *objs[64];               /* domain objects by id - 1 */
} KSession;

static bool session_signaled(KObj *o) { (void)o; return false; }

static void session_destroy(KObj *o) {
    KSession *s = (KSession *)o;
    if (s->root) svcobj_unref(s->root);
    for (int i = 0; i < 64; i++)
        if (s->objs[i]) svcobj_unref(s->objs[i]);
    free(s);
}

/* a new client session handle for o (takes the caller's reference) */
static u32 session_new(SvcObj *o, u32 *h) {
    KSession *s = calloc(1, sizeof *s);
    kobj_init(&s->obj, KO_SESSION, session_destroy, session_signaled);
    s->root = o;
    u32 r = handle_new(&s->obj, h);
    kobj_unref(&s->obj);
    return r;
}

static u32 domain_add(KSession *s, SvcObj *o) {
    for (int i = 0; i < 64; i++)
        if (!s->objs[i]) {
            s->objs[i] = o;
            return (u32)i + 1;
        }
    fatal("domain of %s is full", s->root ? s->root->name : "?");
}

/* ---------------------------------------------------------------------------------------------- replies */
void out_data(IpcReq *r, const void *p, u32 size) {
    u32 align = size >= 8 ? 8 : size >= 4 ? 4 : size >= 2 ? 2 : 1;
    r->out_size = (r->out_size + align - 1) & ~(align - 1);
    if (r->out_size + size > sizeof r->out) fatal("reply of %s too large", r->session->root->name);
    memcpy(r->out + r->out_size, p, size);
    r->out_size += size;
}

void out_object(IpcReq *r, SvcObj *o) {
    if (r->nout_objs == 8) fatal("too many objects in one reply");
    r->out_objs[r->nout_objs++] = o;
}

void out_copy_handle(IpcReq *r, u32 h) { r->out_copy[r->nout_copy++] = h; }
void out_move_handle(IpcReq *r, u32 h) { r->out_move[r->nout_move++] = h; }

void out_event(IpcReq *r, KEvent *e) {
    u32 h;
    if (handle_new(&e->obj, &h)) fatal("out of handles");
    out_copy_handle(r, h);
}

/* ---------------------------------------------------------------------------------------------- stub */
void ipc_stub(SvcObj *self, IpcReq *r) {
    hlog(0, "stub: %s command %u (%u bytes in) -> success", self->name, r->cmd, r->in_size);
}
const Iface iface_stub = {"stub", (const IpcCmd[]){{0, NULL, NULL}}, NULL};
void ipc_noop(SvcObj *self, IpcReq *r) { (void)self; (void)r; }

void services_init(const char *game_dir, const char *save_dir) {
    am_register();
    lm_register();
    apm_register();
    pctl_register();
    fs_register(game_dir, save_dir);
}

static void dispatch(SvcObj *o, IpcReq *r) {
    for (const IpcCmd *c = o->iface->cmds; c->fn; c++)
        if (c->id == r->cmd) {
            hlog(2, "ipc %s.%s (%u)", o->name, c->name, r->cmd);
            c->fn(o, r);
            return;
        }
    if (o->iface != &iface_stub) hlog(0, "%s: command %u is not implemented", o->name, r->cmd);
    ipc_stub(o, r);
}

/* ---------------------------------------------------------------------------------------------- HIPC */
#define SFCI 0x49434653u
#define SFCO 0x4f434653u

static u64 desc_addr3(const u32 *d) { return (u64)d[1] | (u64)(d[2] >> 28) << 32 | (u64)(d[2] >> 2 & 7) << 36; }
static u64 desc_size3(const u32 *d) { return (u64)d[0] | (u64)(d[2] >> 24 & 15) << 32; }

static void control(KSession *s, IpcReq *r) {
    switch (r->cmd) {
    case 0:                                              /* ConvertCurrentObjectToDomain */
        if (!s->domain) {
            s->domain = true;
            s->objs[0] = s->root;
            s->root = NULL;
        }
        out_u32(r, 1);
        break;
    case 1: {                                            /* CopyFromCurrentDomain */
        u32 id = in_u32(r, 0);
        if (!s->domain || id == 0 || id > 64 || !s->objs[id - 1]) { r->result = 0x3d80b; break; }  /* sf: invalid object */
        svcobj_ref(s->objs[id - 1]);
        u32 h;
        session_new(s->objs[id - 1], &h);
        out_move_handle(r, h);
        break;
    }
    case 2: case 4: {                                    /* CloneCurrentObject(Ex) */
        u32 h;
        KSession *n = calloc(1, sizeof *n);
        kobj_init(&n->obj, KO_SESSION, session_destroy, session_signaled);
        n->domain = s->domain;
        if (s->root) { svcobj_ref(s->root); n->root = s->root; }
        for (int i = 0; i < 64; i++)
            if (s->objs[i]) { svcobj_ref(s->objs[i]); n->objs[i] = s->objs[i]; }
        handle_new(&n->obj, &h);
        kobj_unref(&n->obj);
        out_move_handle(r, h);
        break;
    }
    case 3:                                              /* QueryPointerBufferSize */
        out_data(r, &(u16){0x8000}, 2);
        break;
    default:
        hlog(0, "control request %u is not implemented", r->cmd);
    }
}

u32 ipc_send_sync_request(u32 handle, u64 msg, u64 size) {
    (void)size;
    KSession *s = (KSession *)handle_get(handle, KO_SESSION);
    if (!s) return R_INVALID_HANDLE;
    u32 *m = (u32 *)(uintptr_t)msg;
    IpcReq *r = calloc(1, sizeof *r);
    r->session = s;
    r->type = m[0] & 0xffff;
    int nx = m[0] >> 16 & 15, na = m[0] >> 20 & 15, nb = m[0] >> 24 & 15, nw = m[0] >> 28 & 15;
    u32 raw_words = m[1] & 0x3ff, cflags = m[1] >> 10 & 15;
    u32 *p = m + 2;
    if (m[1] >> 31) {
        u32 sh = *p++;
        r->has_pid = sh & 1;
        r->ncopy = sh >> 1 & 15;
        r->nmove = sh >> 5 & 15;
        if (r->has_pid) { r->pid = p[0] | (u64)p[1] << 32; p += 2; }
        for (int i = 0; i < r->ncopy; i++) r->copy[i] = *p++;
        for (int i = 0; i < r->nmove; i++) r->move[i] = *p++;
    }
    for (int i = 0; i < nx; i++, p += 2) {
        r->x[i].addr = (u64)p[1] | (u64)(p[0] >> 12 & 15) << 32 | (u64)(p[0] >> 6 & 7) << 36;
        r->x[i].size = p[0] >> 16;
        r->x[i].flags = (p[0] & 0x3f) | (p[0] >> 9 & 7) << 6;          /* counter */
    }
    r->nx = nx;
    for (int i = 0; i < na; i++, p += 3) { r->a[i].addr = desc_addr3(p); r->a[i].size = desc_size3(p); r->a[i].flags = p[2] & 3; }
    for (int i = 0; i < nb; i++, p += 3) { r->b[i].addr = desc_addr3(p); r->b[i].size = desc_size3(p); r->b[i].flags = p[2] & 3; }
    for (int i = 0; i < nw; i++, p += 3) { r->w[i].addr = desc_addr3(p); r->w[i].size = desc_size3(p); r->w[i].flags = p[2] & 3; }
    r->na = na; r->nb = nb; r->nw = nw;
    u32 *raw = p;
    if (cflags > 2) {
        u32 *cd = raw + raw_words;
        r->nc = (int)cflags - 2;
        for (int i = 0; i < r->nc && i < 16; i++, cd += 2) {
            r->c[i].addr = (u64)cd[0] | (u64)(cd[1] & 0xffff) << 32;
            r->c[i].size = cd[1] >> 16;
        }
    }
    /* the CMIF payload starts at the next 16-byte boundary */
    const u8 *data = (const u8 *)(((uintptr_t)raw + 15) & ~(uintptr_t)15);
    const u8 *raw_end = (const u8 *)(raw + raw_words);
    SvcObj *target = s->root;
    bool domain_msg = s->domain && (r->type == 4 || r->type == 6);
    u32 domain_cmd = 0, domain_obj = 0;
    if (domain_msg) {
        domain_cmd = data[0];
        r->nin_objs = data[1];
        u32 payload = data[2] | data[3] << 8;
        memcpy(&domain_obj, data + 4, 4);
        for (int i = 0; i < r->nin_objs && i < 8; i++) memcpy(&r->in_objs[i], data + 16 + payload + 4 * i, 4);
        data += 16;
        raw_end = data + payload;
        target = domain_obj && domain_obj <= 64 ? s->objs[domain_obj - 1] : NULL;
    }
    u32 magic;
    memcpy(&magic, data, 4);
    if (r->type != 2 && !(domain_msg && domain_cmd == 2)) {
        if (magic != SFCI) hlog(0, "ipc: request without SFCI header (type %u)", r->type);
        memcpy(&r->cmd, data + 8, 4);
        memcpy(&r->token, data + 12, 4);
        r->in = data + 16;
        r->in_size = raw_end > r->in ? (u32)(raw_end - r->in) : 0;
    }
    switch (r->type) {
    case 2:                                              /* Close: the handle itself is closed by svcCloseHandle */
        break;
    case 4: case 6:
        if (domain_msg && domain_cmd == 2) {             /* close a domain object */
            if (domain_obj && domain_obj <= 64 && s->objs[domain_obj - 1]) {
                svcobj_unref(s->objs[domain_obj - 1]);
                s->objs[domain_obj - 1] = NULL;
            }
        } else if (!target) {
            hlog(0, "ipc: request to missing domain object %u", domain_obj);
            r->result = 0x3d80b;
        } else {
            dispatch(target, r);
        }
        break;
    case 5: case 7:
        control(s, r);
        break;
    default:
        hlog(0, "ipc: unsupported request type %u", r->type);
        r->result = 0x1a60a;
        break;
    }
    /* reply: header, handles, then padding to 16 bytes, [domain header], SFCO header, data, [domain object ids] */
    u32 nobjs_handles = 0;
    if (!s->domain)
        for (int i = 0; i < r->nout_objs; i++) {
            u32 h;
            session_new(r->out_objs[i], &h);
            r->out_move[r->nout_move++] = h;
            nobjs_handles++;
        }
    memset(m, 0, 0x100);
    u32 *q = m + 2;
    bool special = r->nout_copy || r->nout_move;
    if (special) {
        *q++ = (u32)r->nout_copy << 1 | (u32)r->nout_move << 5;
        for (int i = 0; i < r->nout_copy; i++) *q++ = r->out_copy[i];
        for (int i = 0; i < r->nout_move; i++) *q++ = r->out_move[i];
    }
    u8 *rawout = (u8 *)q;
    u8 *d = (u8 *)(((uintptr_t)rawout + 15) & ~(uintptr_t)15);
    if (r->type == 2) {
        m[0] = 0;
        m[1] = 0;
    } else {
        if (domain_msg) {
            u32 n = (u32)r->nout_objs;
            memcpy(d, &n, 4);
            d += 16;
        }
        u32 hdr[4] = {SFCO, 0, r->result, 0};
        memcpy(d, hdr, 16);
        memcpy(d + 16, r->out, r->out_size);
        u8 *end = d + 16 + r->out_size;
        if (domain_msg)
            for (int i = 0; i < r->nout_objs; i++) {
                u32 id = domain_add(s, r->out_objs[i]);
                memcpy(end, &id, 4);
                end += 4;
            }
        u32 words = (u32)((end - rawout + 3) / 4);
        if ((u8 *)m + 0x100 < end) fatal("reply overflows the message buffer");
        m[0] = 0;
        m[1] = words | (special ? 1u << 31 : 0);
    }
    (void)nobjs_handles;
    kobj_unref(&s->obj);
    free(r);
    return R_OK;
}

/* ---------------------------------------------------------------------------------------------- sm: */
static void sm_initialize(SvcObj *self, IpcReq *r) { (void)self; (void)r; }

static void sm_get_service(SvcObj *self, IpcReq *r) {
    (void)self;
    char name[9] = {0};
    u64 n = in_u64(r, 0);
    memcpy(name, &n, 8);
    hlog(1, "sm: GetService(%s)", name);
    out_object(r, service_open(name));
}

static const Iface iface_sm = {"sm:", (const IpcCmd[]){
    {0, sm_initialize, "Initialize"},
    {1, sm_get_service, "GetService"},
    {0, NULL, NULL}}, NULL};

u32 ipc_connect_named_port(const char *name, u32 *out) {
    if (strcmp(name, "sm:")) {
        hlog(0, "ConnectToNamedPort(%s): no such port", name);
        return R_NOT_FOUND;
    }
    return session_new(svcobj_new(&iface_sm, NULL, "sm:"), out);
}
