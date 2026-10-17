/* NVN, intercepted (CLAUDE.md, decision 3): nvnBootstrapLoader and nv:: never reach the sdk's driver. Every NVN
 * function the game asks for (nvnBootstrapLoader, nvnDeviceGetProcAddress) gets a host-call address.
 *
 * This is the front end: objects, builders, state blocks, queries, and command recording (nvn.h describes the
 * encoding); gpu.c replays the commands on Vulkan. Storage:
 * - builders, texture views and the state blocks are values the game copies, so their fields live inline in the
 *   game's memory (layouts in nvn.h, each within the API's size for that type);
 * - objects (device, queue, pools, buffers, textures, ...) keep a magic word and a pointer to host state;
 * - a memory pool's GPU address is the CPU address of its storage (identity, like the CPU side); virtual pools
 *   get a range above the guest's address space.
 * Functions not written yet log once and return 0.
 *
 * Enum values follow the NVN headers' numbering as we know it; the first use of each state setter is logged at
 * level 2 so that a wrong guess shows. */
#include "nvn.h"
#include <stdlib.h>
#include <windows.h>

#define A(i) (c->x[i])
#define F32(i) (((v4f32)c->v[i])[0])
#define RET(v) (c->x[0] = (u64)(v))
#define P(T, i) ((T *)(uintptr_t)c->x[i])
#define PTR(a) ((void *)(uintptr_t)(a))

typedef struct NvnProc { const char *name; GuestFn fn; } NvnProc;

/* ---------------------------------------------------------------------------------------------- objects */
/* live host states and the guest object that owns each: the game copies initialised objects around (and may
 * finalise both copies), so only the owner frees */
static struct { void *host; u64 owner; } *live;
static u32 live_cap, live_count;
static SRWLOCK live_lock = SRWLOCK_INIT;
#define LIVE_TOMB ((void *)1)

static u32 live_slot(void *h) { return (u32)(((uintptr_t)h >> 4) * 0x9e3779b1u) & (live_cap - 1); }
static void live_put(void *h, u64 owner) {
    if ((live_count + 1) * 2 > live_cap) {
        u32 oc = live_cap;
        void *old = live;
        live_cap = oc ? oc * 2 : 4096;
        live = calloc(live_cap, sizeof *live);
        live_count = 0;
        for (u32 i = 0; i < oc; i++) {
            void *hh = ((struct { void *host; u64 owner; } *)old)[i].host;
            if (hh && hh != LIVE_TOMB) live_put(hh, ((struct { void *host; u64 owner; } *)old)[i].owner);
        }
        free(old);
    }
    u32 k = live_slot(h);
    while (live[k].host && live[k].host != LIVE_TOMB) k = (k + 1) & (live_cap - 1);
    live[k].host = h;
    live[k].owner = owner;
    live_count++;
}
/* true if h is live and owned by owner (then it is forgotten) */
static bool live_take(void *h, u64 owner) {
    if (!live_cap) return false;
    u32 k = live_slot(h);
    while (live[k].host) {
        if (live[k].host == h) {
            if (live[k].owner != owner) return false;
            live[k].host = LIVE_TOMB;
            return true;
        }
        k = (k + 1) & (live_cap - 1);
    }
    return false;
}

static void *obj_new(u64 addr, u32 type, size_t size) {
    void *h = calloc(1, size);
    ObjHdr *o = (ObjHdr *)(uintptr_t)addr;
    o->magic = NVN_MAGIC;
    o->type = type;
    o->host = h;
    AcquireSRWLockExclusive(&live_lock);
    live_put(h, addr);
    ReleaseSRWLockExclusive(&live_lock);
    return h;
}

void *obj_get(u64 addr, u32 type) {
    ObjHdr *o = (ObjHdr *)(uintptr_t)addr;
    if (!addr || o->magic != NVN_MAGIC || o->type != type) return NULL;
    return o->host;
}

static void obj_free(u64 addr) {
    ObjHdr *o = (ObjHdr *)(uintptr_t)addr;
    if (!addr || o->magic != NVN_MAGIC) return;
    AcquireSRWLockExclusive(&live_lock);
    bool mine = live_take(o->host, addr);
    ReleaseSRWLockExclusive(&live_lock);
    if (mine) free(o->host);
    o->magic = 0;
    o->host = NULL;
}

static u64 debug_ids;
static void next_debug_id(Ctx *c) { RET(InterlockedIncrement64((LONG64 *)&debug_ids)); }
static void return_true(Ctx *c) { RET(1); }
static void return_zero(Ctx *c) { RET(0); }
static void nothing(Ctx *c) {}

/* ---------------------------------------------------------------------------------------------- device */
typedef struct DeviceBuilder { u32 flags; } DeviceBuilder;                      /* 0x40 */
typedef struct Device { u32 flags, window_origin, depth_mode; } Device;

static void device_builder_defaults(Ctx *c) { memset(P(void, 0), 0, 0x40); }
static void device_builder_flags(Ctx *c) { P(DeviceBuilder, 0)->flags = (u32)A(1); }
static void device_initialize(Ctx *c) {
    Device *d = obj_new(A(0), T_DEVICE, sizeof(Device));
    d->flags = P(DeviceBuilder, 1)->flags;
    hlog(1, "nvn: device initialised (flags %x)", d->flags);
    RET(1);
}
static void device_finalize(Ctx *c) { obj_free(A(0)); }

/* nvnDeviceGetInteger(device, NVNdeviceInfo, int *value) */
static void device_get_integer(Ctx *c) {
    static const struct { u32 id; s32 v; } info[] = {
        {0, 53}, {1, 311},                       /* API version */
        {2, 14}, {3, 0x10000}, {4, 0x100},       /* uniform buffers per stage, max size, alignment */
        {5, 8}, {6, 16}, {7, 4}, {8, 16}, {9, 32}, {10, 16}, {11, 4}, {12, 4}, {13, 4}, {14, 16},
        {15, 32}, {16, 32},                      /* texture / sampler descriptor sizes */
        {17, 256}, {18, 256},                    /* reserved descriptors */
        {19, 4}, {20, 8}, {21, 0x10000}, {22, 0x1000},   /* command buffer alignments and minimum sizes */
        {23, 256}, {24, 256}, {25, 0x20000}, {26, 0x20000},
        {27, 16}, {28, 1536}, {29, 1024}, {30, 64}, {31, 1536}, {32, 65535}, {33, 65535}, {34, 65535},
        {35, 8}, {36, 1048576}, {37, 4096}, {38, 16}, {39, 0x10000}, {40, 1}, {41, 1}, {42, 1},
        {43, 0x40000}, {44, 15}, {45, 2048}, {46, 1}, {47, 1}, {48, 15}, {49, 5}, {50, 1}, {51, 8}, {52, 8},
        {53, 4}, {54, 32}, {55, 256}, {56, 32}, {57, 32}, {58, 0x1000}, {59, 1}, {60, 4}, {61, 16384},
        {62, 134217728}, {63, 2048}, {64, 16384}, {65, 16384}, {66, 1}, {67, 1}, {68, 1}, {69, 1}, {70, 5},
        {71, 1}, {72, 1}, {73, 1}, {74, 1}, {75, 1}, {76, 1}, {77, 16}, {78, 1024}, {79, 32},
        {80, 0x4000}, {81, 0x10000}, {82, 0x100000}, {83, 0x4000}, {84, 0x10000}, {85, 0x40000},
        {86, 0x10000}, {87, 1}, {88, 4}, {89, 2}, {90, 0}, {91, 0x4000}, {92, 0x4000}, {93, 0x1000},
        {94, 128}, {95, 32}, {96, 0xffff}, {97, 1},
    };
    u32 id = (u32)A(1);
    s32 v = 0;
    bool found = false;
    for (size_t i = 0; i < sizeof info / sizeof *info; i++)
        if (info[i].id == id) { v = info[i].v; found = true; }
    if (!found) hlog(0, "nvn: device info %u unknown, answering 0", id);
    if (A(2)) ST32(A(2), (u32)v);
}

static u64 now_ns(void) {
    LARGE_INTEGER f, n;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&n);
    return (u64)((double)n.QuadPart * 1e9 / (double)f.QuadPart);
}
static void device_timestamp(Ctx *c) { RET(now_ns()); }
/* handles: texture id in the low 20 bits, sampler id above, as on the console */
static void device_texture_handle(Ctx *c) { RET((u64)(u32)A(1) | (u64)(u32)A(2) << 20); }
static void device_texel_fetch_handle(Ctx *c) { RET((u64)(u32)A(1)); }
static void device_image_handle(Ctx *c) { RET((u64)(u32)A(1)); }
static void device_separate_texture_handle(Ctx *c) { RET((u64)(u32)A(1)); }
static void device_separate_sampler_handle(Ctx *c) { RET((u64)(u32)A(1) << 20); }
static void device_set_window_origin(Ctx *c) {
    Device *d = obj_get(A(0), T_DEVICE);
    if (!d) return;
    d->window_origin = (u32)A(1);
    hlog(1, "nvn: window origin mode %u", d->window_origin);
    gpu_device_modes(d->window_origin, d->depth_mode);
}
static void device_set_depth_mode(Ctx *c) {
    Device *d = obj_get(A(0), T_DEVICE);
    if (!d) return;
    d->depth_mode = (u32)A(1);
    hlog(1, "nvn: depth mode %u", d->depth_mode);
    gpu_device_modes(d->window_origin, d->depth_mode);
}
static void device_get_window_origin(Ctx *c) { Device *d = obj_get(A(0), T_DEVICE); RET(d ? d->window_origin : 0); }
static void device_get_depth_mode(Ctx *c) { Device *d = obj_get(A(0), T_DEVICE); RET(d ? d->depth_mode : 0); }

/* ---------------------------------------------------------------------------------------------- memory pools */
typedef struct PoolBuilder { u64 device, storage, size; u32 flags; } PoolBuilder;   /* 0x40 */

static volatile LONG64 virtual_next = 0x8000000000ll;       /* virtual pools' GPU addresses: never CPU memory */

static void pool_builder_defaults(Ctx *c) { memset(P(void, 0), 0, 0x40); }
static void pool_builder_device(Ctx *c) { P(PoolBuilder, 0)->device = A(1); }
static void pool_builder_storage(Ctx *c) { P(PoolBuilder, 0)->storage = A(1); P(PoolBuilder, 0)->size = A(2); }
static void pool_builder_flags(Ctx *c) { P(PoolBuilder, 0)->flags = (u32)A(1); }
static void pool_builder_get_storage(Ctx *c) { RET(P(PoolBuilder, 0)->storage); }
static void pool_builder_get_size(Ctx *c) { RET(P(PoolBuilder, 0)->size); }
static void pool_builder_get_flags(Ctx *c) { RET(P(PoolBuilder, 0)->flags); }
static void pool_initialize(Ctx *c) {
    PoolBuilder *b = P(PoolBuilder, 1);
    Pool *p = obj_new(A(0), T_POOL, sizeof(Pool));
    p->storage = b->storage;
    p->size = b->size;
    p->flags = b->flags;
    if (p->flags & POOL_VIRTUAL) {
        u64 sz = (p->size + 0xffff) & ~0xffffull;
        p->gpu = (u64)InterlockedAdd64(&virtual_next, (LONG64)sz) - sz;
    } else {
        p->gpu = p->storage;
    }
    hlog(2, "nvn: pool %llx: storage %llx+%llx flags %x gpu %llx", A(0), p->storage, p->size, p->flags, p->gpu);
    gpu_pool_created(p);
    RET(1);
}
static void pool_finalize(Ctx *c) {
    Pool *p = obj_get(A(0), T_POOL);
    if (p) gpu_pool_destroyed(p);
    obj_free(A(0));
}
static void pool_map(Ctx *c) { Pool *p = obj_get(A(0), T_POOL); RET(p ? p->storage : 0); }
static void pool_address(Ctx *c) { Pool *p = obj_get(A(0), T_POOL); RET(p ? p->gpu : 0); }
static void pool_size(Ctx *c) { Pool *p = obj_get(A(0), T_POOL); RET(p ? p->size : 0); }
static void pool_flags(Ctx *c) { Pool *p = obj_get(A(0), T_POOL); RET(p ? p->flags : 0); }
/* nvnMemoryPoolMapVirtual(pool, int count, const NVNmappingRequest *): texture memory lives in our images, so
 * the mapping only matters for the addresses, which the virtual pool already has */
static void pool_map_virtual(Ctx *c) {
    static bool logged;
    if (!logged) { logged = true; hlog(1, "nvn: MapVirtual(%llx, %d requests): accepted", A(0), (int)A(1)); }
    RET(1);
}

u64 pool_storage(u64 pool) { Pool *p = obj_get(pool, T_POOL); return p ? p->storage : 0; }
u64 pool_gpu(u64 pool) { Pool *p = obj_get(pool, T_POOL); return p ? p->gpu : 0; }

/* ---------------------------------------------------------------------------------------------- buffers */
typedef struct BufferBuilder { u64 device, pool, offset, size; } BufferBuilder;   /* 0x40 */
typedef struct Buffer { u64 pool, offset, size, addr, cpu; } Buffer;

static void buffer_builder_defaults(Ctx *c) { memset(P(void, 0), 0, sizeof(BufferBuilder)); }
static void buffer_builder_device(Ctx *c) { P(BufferBuilder, 0)->device = A(1); }
static void buffer_builder_storage(Ctx *c) {
    BufferBuilder *b = P(BufferBuilder, 0);
    b->pool = A(1); b->offset = A(2); b->size = A(3);
}
static void buffer_builder_get_pool(Ctx *c) { RET(P(BufferBuilder, 0)->pool); }
static void buffer_builder_get_offset(Ctx *c) { RET(P(BufferBuilder, 0)->offset); }
static void buffer_builder_get_size(Ctx *c) { RET(P(BufferBuilder, 0)->size); }
static void buffer_initialize(Ctx *c) {
    BufferBuilder *b = P(BufferBuilder, 1);
    Buffer *x = obj_new(A(0), T_BUFFER, sizeof(Buffer));
    x->pool = b->pool; x->offset = b->offset; x->size = b->size;
    x->addr = pool_gpu(b->pool) + b->offset;
    x->cpu = pool_storage(b->pool) ? pool_storage(b->pool) + b->offset : 0;
    RET(1);
}
static void buffer_finalize(Ctx *c) { obj_free(A(0)); }
static void buffer_map(Ctx *c) { Buffer *b = obj_get(A(0), T_BUFFER); RET(b ? b->cpu : 0); }
static void buffer_address(Ctx *c) { Buffer *b = obj_get(A(0), T_BUFFER); RET(b ? b->addr : 0); }
static void buffer_size(Ctx *c) { Buffer *b = obj_get(A(0), T_BUFFER); RET(b ? b->size : 0); }
static void buffer_pool(Ctx *c) { Buffer *b = obj_get(A(0), T_BUFFER); RET(b ? b->pool : 0); }
static void buffer_offset(Ctx *c) { Buffer *b = obj_get(A(0), T_BUFFER); RET(b ? b->offset : 0); }

/* ---------------------------------------------------------------------------------------------- queues */
typedef struct QueueBuilder {                                                   /* 0x40 */
    u64 device;
    u32 flags, priority;
    u64 command_size, compute_size, control_size, memory, memory_size, flush_threshold;
} QueueBuilder;
typedef struct Queue { u32 flags; } Queue;

static void queue_builder_defaults(Ctx *c) {
    QueueBuilder *b = P(QueueBuilder, 0);
    memset(b, 0, 0x40);
    b->command_size = 0x100000; b->compute_size = 0x40000; b->control_size = 0x4000;
    b->flush_threshold = 0x10000;
}
static void queue_builder_device(Ctx *c) { P(QueueBuilder, 0)->device = A(1); }
static void queue_builder_flags(Ctx *c) { P(QueueBuilder, 0)->flags = (u32)A(1); }
static void queue_builder_command_size(Ctx *c) { P(QueueBuilder, 0)->command_size = A(1); }
static void queue_builder_compute_size(Ctx *c) { P(QueueBuilder, 0)->compute_size = A(1); }
static void queue_builder_control_size(Ctx *c) { P(QueueBuilder, 0)->control_size = A(1); }
static void queue_builder_flush_threshold(Ctx *c) { P(QueueBuilder, 0)->flush_threshold = A(1); }
static void queue_builder_priority(Ctx *c) { P(QueueBuilder, 0)->priority = (u32)A(1); }
static void queue_builder_memory(Ctx *c) { P(QueueBuilder, 0)->memory = A(1); P(QueueBuilder, 0)->memory_size = A(2); }
/* the memory a queue wants from the game for its own command buffers */
static void queue_builder_memory_size(Ctx *c) {
    QueueBuilder *b = P(QueueBuilder, 0);
    RET((b->command_size + b->compute_size + b->control_size + 0xfff) & ~0xfffull);
}
static void queue_initialize(Ctx *c) {
    Queue *q = obj_new(A(0), T_QUEUE, sizeof(Queue));
    q->flags = P(QueueBuilder, 1)->flags;
    RET(1);
}
static void queue_finalize(Ctx *c) { gpu_finish(A(0)); obj_free(A(0)); }
static void queue_get_error(Ctx *c) { if (A(1)) memset(P(void, 1), 0, 0x18); RET(0); }
/* nvnQueueSubmitCommands(queue, int count, const NVNcommandHandle *) */
static void queue_submit(Ctx *c) {
    u32 n = (u32)A(1);
    if (n) gpu_submit(A(0), n, P(u64, 2));
}
static void queue_flush(Ctx *c) { gpu_flush(A(0)); }
static void queue_finish(Ctx *c) { gpu_finish(A(0)); }

/* ---------------------------------------------------------------------------------------------- sync */
static void sync_initialize(Ctx *c) { obj_new(A(0), T_SYNC, sizeof(Sync)); RET(1); }
static void sync_finalize(Ctx *c) { obj_free(A(0)); }
/* nvnQueueFenceSync(queue, sync, condition, flags) */
static void queue_fence_sync(Ctx *c) {
    Sync *s = obj_get(A(1), T_SYNC);
    if (!s) return;
    s->value = gpu_fence(A(0));
    s->fenced = true;
}
/* nvnSyncWait(sync, u64 timeout_ns): ALREADY_SIGNALED 0, CONDITION_SATISFIED 1, TIMEOUT_EXPIRED 2, FAILED 3 */
static void sync_wait(Ctx *c) {
    Sync *s = obj_get(A(0), T_SYNC);
    if (!s || !s->fenced || gpu_completed() >= s->value) { RET(0); return; }
    core_leave();
    bool ok = gpu_wait(s->value, A(1));
    core_enter();
    RET(ok ? 1 : 2);
}

/* ---------------------------------------------------------------------------------------------- views */
void view_resolve(const Texture *tex, const TexView *v, ViewInfo *o) {
    const TexInfo *t = &tex->info;
    memset(o, 0, sizeof *o);
    o->format = v && v->flags & VIEW_FORMAT ? v->format : t->format;
    o->target = v && v->flags & VIEW_TARGET ? v->target : t->target;
    u32 levels = t->levels ? t->levels : 1;
    o->base_level = 0;
    o->num_levels = (u16)levels;
    if (v && v->flags & VIEW_LEVELS) {
        o->base_level = v->base_level;
        o->num_levels = v->num_levels ? v->num_levels : (u16)(levels - v->base_level);
    }
    u32 layers = 1;
    switch (t->target) {
    case TEX_1D_ARRAY: layers = t->height; break;
    case TEX_2D_ARRAY: case TEX_2D_MS_ARRAY: case TEX_CUBE: case TEX_CUBE_ARRAY: layers = t->depth; break;
    }
    o->min_layer = 0;
    o->num_layers = (u16)layers;
    if (v && v->flags & VIEW_LAYERS) {
        o->min_layer = (u16)v->min_layer;
        o->num_layers = (u16)(v->num_layers ? v->num_layers : layers - v->min_layer);
    }
    /* what a view leaves unset comes from the texture (nvnTextureBuilderSetSwizzle: e.g. BC5 as R R R G) */
    static const u8 identity[4] = {2, 3, 4, 5};
    const u8 *sw = v && v->flags & VIEW_SWIZZLE ? v->swizzle : tex->b.swizzle;
    memcpy(o->swizzle, sw[0] | sw[1] | sw[2] | sw[3] ? sw : identity, 4);     /* all ZERO: never set */
    o->ds_mode = v && v->flags & VIEW_DS_MODE ? v->ds_mode : tex->b.ds_mode;
}

/* ---------------------------------------------------------------------------------------------- textures */
static void texinfo_from_builder(const TexBuilder *b, TexInfo *t) {
    memset(t, 0, sizeof *t);
    t->addr = pool_gpu(b->pool) + b->offset;
    t->format = b->format;
    t->target = b->target;
    t->width = b->width ? b->width : 1;
    t->height = b->height ? b->height : 1;
    t->depth = b->depth ? b->depth : 1;
    t->levels = b->levels ? b->levels : 1;
    t->samples = b->samples;
    t->flags = b->flags;
    t->stride = (s32)b->stride;
    /* NVNpackagedTextureLayout: byte 0 is the block height (log2 GOBs) chosen by the packaging tool */
    t->block_height = b->has_layout ? b->layout[0] : 0xff;
    if (b->has_layout) {
        static u64 seen[64];
        static int nseen;
        u64 key = 0;
        memcpy(&key, b->layout, 8);
        bool found = false;
        for (int i = 0; i < nseen; i++) found |= seen[i] == key;
        if (!found && nseen < 64) {
            seen[nseen++] = key;
            hlog(1, "nvn: packaged layout %016llx for %ux%u fmt %u levels %u", key, t->width, t->height, t->format, t->levels);
        }
    }
    if (t->target == TEX_1D || t->target == TEX_BUFFER) t->height = 1;
    if (t->target != TEX_3D && t->target != TEX_2D_ARRAY && t->target != TEX_2D_MS_ARRAY && t->target != TEX_CUBE &&
        t->target != TEX_CUBE_ARRAY) t->depth = 1;
}

static void tb_defaults(Ctx *c) {
    TexBuilder *b = P(TexBuilder, 0);
    u64 dev = b->device;
    memset(b, 0, 0x80);
    b->device = dev;
    b->target = TEX_2D; b->width = b->height = b->depth = 1; b->levels = 1; b->samples = 0;
    b->swizzle[0] = 2; b->swizzle[1] = 3; b->swizzle[2] = 4; b->swizzle[3] = 5;   /* R, G, B, A */
}
static void tb_device(Ctx *c) { P(TexBuilder, 0)->device = A(1); }
static void tb_flags(Ctx *c) { P(TexBuilder, 0)->flags = (u32)A(1); }
static void tb_target(Ctx *c) { P(TexBuilder, 0)->target = (u32)A(1); }
static void tb_width(Ctx *c) { P(TexBuilder, 0)->width = (u32)A(1); }
static void tb_height(Ctx *c) { P(TexBuilder, 0)->height = (u32)A(1); }
static void tb_depth(Ctx *c) { P(TexBuilder, 0)->depth = (u32)A(1); }
static void tb_size1d(Ctx *c) { TexBuilder *b = P(TexBuilder, 0); b->width = (u32)A(1); b->height = b->depth = 1; }
static void tb_size2d(Ctx *c) { TexBuilder *b = P(TexBuilder, 0); b->width = (u32)A(1); b->height = (u32)A(2); b->depth = 1; }
static void tb_size3d(Ctx *c) { TexBuilder *b = P(TexBuilder, 0); b->width = (u32)A(1); b->height = (u32)A(2); b->depth = (u32)A(3); }
static void tb_levels(Ctx *c) { P(TexBuilder, 0)->levels = (u32)A(1); }
static void tb_format(Ctx *c) { P(TexBuilder, 0)->format = (u32)A(1); }
static void tb_samples(Ctx *c) { P(TexBuilder, 0)->samples = (u32)A(1); }
static void tb_swizzle(Ctx *c) { TexBuilder *b = P(TexBuilder, 0); for (int i = 0; i < 4; i++) b->swizzle[i] = (u8)A(1 + i); }
static void tb_ds_mode(Ctx *c) { P(TexBuilder, 0)->ds_mode = (u32)A(1); }
static void tb_storage(Ctx *c) { P(TexBuilder, 0)->pool = A(1); P(TexBuilder, 0)->offset = A(2); }
static void tb_stride(Ctx *c) { P(TexBuilder, 0)->stride = (s64)A(1); }
static void tb_packaged_data(Ctx *c) { P(TexBuilder, 0)->packaged = A(1); }
static void tb_packaged_layout(Ctx *c) {
    TexBuilder *b = P(TexBuilder, 0);
    if (A(1)) { memcpy(b->layout, P(void, 1), 8); b->has_layout = 1; }
    else b->has_layout = 0;
}
static void tb_gl_name(Ctx *c) { P(TexBuilder, 0)->gl_name = (u32)A(1); }
static void tb_storage_size(Ctx *c) { TexInfo t; texinfo_from_builder(P(TexBuilder, 0), &t); RET(texture_storage_size(&t)); }
static void tb_storage_alignment(Ctx *c) { RET(0x200); }
static void tb_get_device(Ctx *c) { RET(P(TexBuilder, 0)->device); }
static void tb_get_flags(Ctx *c) { RET(P(TexBuilder, 0)->flags); }
static void tb_get_target(Ctx *c) { RET(P(TexBuilder, 0)->target); }
static void tb_get_width(Ctx *c) { RET(P(TexBuilder, 0)->width); }
static void tb_get_height(Ctx *c) { RET(P(TexBuilder, 0)->height); }
static void tb_get_depth(Ctx *c) { RET(P(TexBuilder, 0)->depth); }
static void tb_get_levels(Ctx *c) { RET(P(TexBuilder, 0)->levels); }
static void tb_get_format(Ctx *c) { RET(P(TexBuilder, 0)->format); }
static void tb_get_samples(Ctx *c) { RET(P(TexBuilder, 0)->samples); }
static void tb_get_ds_mode(Ctx *c) { RET(P(TexBuilder, 0)->ds_mode); }
static void tb_get_stride(Ctx *c) { RET(P(TexBuilder, 0)->stride); }
static void tb_get_pool(Ctx *c) { RET(P(TexBuilder, 0)->pool); }
static void tb_get_pool_offset(Ctx *c) { RET(P(TexBuilder, 0)->offset); }
static void tb_get_packaged(Ctx *c) { RET(P(TexBuilder, 0)->packaged); }
static void tb_get_gl_name(Ctx *c) { RET(P(TexBuilder, 0)->gl_name); }
static void tb_get_swizzle(Ctx *c) {
    TexBuilder *b = P(TexBuilder, 0);
    for (int i = 0; i < 4; i++) if (A(1 + i)) ST32(A(1 + i), b->swizzle[i]);
}
/* NVNstorageClass for MapVirtual: the console's generic block-linear kind */
static void tb_storage_class(Ctx *c) { RET(P(TexBuilder, 0)->flags & TEX_FLAG_LINEAR ? 0 : 0xfe); }

static void tex_initialize(Ctx *c) {
    TexBuilder *b = P(TexBuilder, 1);
    Texture *t = obj_new(A(0), T_TEXTURE, sizeof(Texture));
    t->b = *b;
    texinfo_from_builder(b, &t->info);
    t->size = texture_storage_size(&t->info);
    hlog(2, "nvn: texture %llx: %ux%ux%u fmt %u target %u levels %u at %llx+%llx", A(0), b->width, b->height,
         b->depth, b->format, b->target, b->levels, t->info.addr, t->size);
    RET(1);
}
static void tex_finalize(Ctx *c) { obj_free(A(0)); }
#define TEX_GET(name, expr) static void name(Ctx *c) { Texture *t = obj_get(A(0), T_TEXTURE); RET(t ? (u64)(expr) : 0); }
TEX_GET(tex_get_width, t->b.width)
TEX_GET(tex_get_height, t->b.height)
TEX_GET(tex_get_depth, t->b.depth)
TEX_GET(tex_get_levels, t->b.levels)
TEX_GET(tex_get_format, t->b.format)
TEX_GET(tex_get_target, t->b.target)
TEX_GET(tex_get_flags, t->b.flags)
TEX_GET(tex_get_samples, t->b.samples)
TEX_GET(tex_get_stride, t->b.stride)
TEX_GET(tex_get_storage_size, t->size)
TEX_GET(tex_get_address, t->info.addr)
TEX_GET(tex_get_pool, t->b.pool)
TEX_GET(tex_get_pool_offset, t->b.offset)
TEX_GET(tex_get_ds_mode, t->b.ds_mode)
TEX_GET(tex_get_storage_class, t->b.flags & TEX_FLAG_LINEAR ? 0 : 0xfe)
static void tex_get_swizzle(Ctx *c) {
    Texture *t = obj_get(A(0), T_TEXTURE);
    for (int i = 0; i < 4; i++) if (A(1 + i)) ST32(A(1 + i), t ? t->b.swizzle[i] : 0);
}
static void tex_compare(Ctx *c) {
    Texture *a = obj_get(A(0), T_TEXTURE), *b = obj_get(A(1), T_TEXTURE);
    RET(a && b && !memcmp(&a->info, &b->info, sizeof a->info));
}
/* nvnTextureWriteTexels(tex, view, region, data) / Strided(..., rowStride, imageStride): the CPU writes texels.
 * The image (gpu.c) holds the texture's contents, so the data goes there. */
static void write_texels(Ctx *c, s64 row, s64 image) {
    Texture *t = obj_get(A(0), T_TEXTURE);
    if (!t || !A(2) || !A(3)) return;
    ViewInfo v;
    view_resolve(t, A(1) ? P(TexView, 1) : NULL, &v);
    gpu_texture_written(&t->info, &v, P(CopyRegion, 2), P(void, 3), row, image);
}
static void tex_write_texels(Ctx *c) { write_texels(c, 0, 0); }
static void tex_write_texels_strided(Ctx *c) { write_texels(c, (s64)A(4), (s64)A(5)); }
/* nvnTextureReadTexels / Strided: the CPU reads texels back (the ghost thumbnail of Time Trials) */
static void read_texels(Ctx *c, s64 row, s64 image) {
    Texture *t = obj_get(A(0), T_TEXTURE);
    if (!t || !A(2) || !A(3)) return;
    ViewInfo v;
    view_resolve(t, A(1) ? P(TexView, 1) : NULL, &v);
    gpu_texture_read(&t->info, &v, P(CopyRegion, 2), P(void, 3), row, image);
}
static void tex_read_texels(Ctx *c) { read_texels(c, 0, 0); }
static void tex_read_texels_strided(Ctx *c) { read_texels(c, (s64)A(4), (s64)A(5)); }

static void view_defaults(Ctx *c) { memset(P(void, 0), 0, sizeof(TexView)); }
static void view_levels(Ctx *c) { TexView *v = P(TexView, 0); v->base_level = (u16)A(1); v->num_levels = (u16)A(2); v->flags |= VIEW_LEVELS; }
static void view_layers(Ctx *c) { TexView *v = P(TexView, 0); v->min_layer = (u32)A(1); v->num_layers = (u32)A(2); v->flags |= VIEW_LAYERS; }
static void view_format(Ctx *c) { TexView *v = P(TexView, 0); v->format = (u32)A(1); v->flags |= VIEW_FORMAT; }
static void view_swizzle(Ctx *c) { TexView *v = P(TexView, 0); for (int i = 0; i < 4; i++) v->swizzle[i] = (u8)A(1 + i); v->flags |= VIEW_SWIZZLE; }
static void view_ds_mode(Ctx *c) { TexView *v = P(TexView, 0); v->ds_mode = (u32)A(1); v->flags |= VIEW_DS_MODE; }
static void view_target(Ctx *c) { TexView *v = P(TexView, 0); v->target = (u32)A(1); v->flags |= VIEW_TARGET; }
static void view_get_levels(Ctx *c) {
    TexView *v = P(TexView, 0);
    if (A(1)) ST32(A(1), v->base_level);
    if (A(2)) ST32(A(2), v->num_levels);
    RET(!!(v->flags & VIEW_LEVELS));
}
static void view_get_layers(Ctx *c) {
    TexView *v = P(TexView, 0);
    if (A(1)) ST32(A(1), v->min_layer);
    if (A(2)) ST32(A(2), v->num_layers);
    RET(!!(v->flags & VIEW_LAYERS));
}
static void view_get_format(Ctx *c) { TexView *v = P(TexView, 0); if (A(1)) ST32(A(1), v->format); RET(!!(v->flags & VIEW_FORMAT)); }
static void view_get_target(Ctx *c) { TexView *v = P(TexView, 0); if (A(1)) ST32(A(1), v->target); RET(!!(v->flags & VIEW_TARGET)); }
static void view_get_ds_mode(Ctx *c) { TexView *v = P(TexView, 0); if (A(1)) ST32(A(1), v->ds_mode); RET(!!(v->flags & VIEW_DS_MODE)); }
static void view_get_swizzle(Ctx *c) {
    TexView *v = P(TexView, 0);
    for (int i = 0; i < 4; i++) if (A(1 + i)) ST32(A(1 + i), v->swizzle[i]);
    RET(!!(v->flags & VIEW_SWIZZLE));
}
static void view_compare(Ctx *c) { RET(memcmp(P(void, 0), P(void, 1), sizeof(TexView)) == 0); }

/* ---------------------------------------------------------------------------------------------- interned descriptors */
typedef struct Intern { void *items; u32 size, count, cap; u32 *hash; u32 hcap; SRWLOCK lock; } Intern;
static Intern tdescs = {NULL, sizeof(TexDesc), 0, 0, NULL, 0, SRWLOCK_INIT};
static Intern sdescs = {NULL, sizeof(SamplerState), 0, 0, NULL, 0, SRWLOCK_INIT};

static u64 fnv64(const void *p, size_t n) {
    const u8 *b = p;
    u64 h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}

/* entries never move once handed out: items live in fixed blocks */
#define INTERN_BLOCK 4096
static void *intern_item(Intern *t, u32 i) { return ((u8 **)t->items)[i / INTERN_BLOCK] + (size_t)(i % INTERN_BLOCK) * t->size; }

static u32 intern(Intern *t, const void *v) {
    u64 h = fnv64(v, t->size);
    AcquireSRWLockExclusive(&t->lock);
    if (t->count * 2 >= t->hcap) {
        u32 ncap = t->hcap ? t->hcap * 2 : 1024;
        u32 *nh = malloc(ncap * sizeof *nh);
        memset(nh, 0xff, ncap * sizeof *nh);
        for (u32 i = 0; i < t->count; i++) {
            u64 hi = fnv64(intern_item(t, i), t->size);
            u32 k = (u32)hi & (ncap - 1);
            while (nh[k] != ~0u) k = (k + 1) & (ncap - 1);
            nh[k] = i;
        }
        free(t->hash);
        t->hash = nh;
        t->hcap = ncap;
    }
    u32 k = (u32)h & (t->hcap - 1);
    while (t->hash[k] != ~0u) {
        if (!memcmp(intern_item(t, t->hash[k]), v, t->size)) {
            u32 r = t->hash[k];
            ReleaseSRWLockExclusive(&t->lock);
            return r;
        }
        k = (k + 1) & (t->hcap - 1);
    }
    u32 i = t->count;
    if (i / INTERN_BLOCK >= t->cap) {
        t->items = realloc(t->items, (t->cap + 64) * sizeof(u8 *));
        t->cap += 64;
    }
    if (i % INTERN_BLOCK == 0) ((u8 **)t->items)[i / INTERN_BLOCK] = malloc((size_t)INTERN_BLOCK * t->size);
    memcpy(intern_item(t, i), v, t->size);
    t->hash[k] = i;
    t->count = i + 1;
    ReleaseSRWLockExclusive(&t->lock);
    return i;
}
u32 tdesc_intern(const TexDesc *d) { return intern(&tdescs, d); }
u32 sdesc_intern(const SamplerState *s) { return intern(&sdescs, s); }
const TexDesc *tdesc_get(u32 i) { return i < tdescs.count ? intern_item(&tdescs, i) : NULL; }
const SamplerState *sdesc_get(u32 i) { return i < sdescs.count ? intern_item(&sdescs, i) : NULL; }

/* ---------------------------------------------------------------------------------------------- samplers */
typedef struct SmpBuilder { u64 device; SamplerState s; } SmpBuilder;          /* 0x60 */
typedef struct Sampler { SamplerState s; } Sampler;
static void sb_defaults(Ctx *c) {
    SmpBuilder *b = P(SmpBuilder, 0);
    memset(&b->s, 0, sizeof b->s);
    b->s.min_filter = 4;                /* NEAREST_MIPMAP_LINEAR, as in GL */
    b->s.mag_filter = 1;                /* LINEAR */
    b->s.wrap[0] = b->s.wrap[1] = b->s.wrap[2] = 1;   /* REPEAT */
    b->s.lod_min = -1000; b->s.lod_max = 1000; b->s.max_aniso = 1;
    b->s.compare_func = 8;
}
static void sb_device(Ctx *c) { P(SmpBuilder, 0)->device = A(1); }
static void sb_min_mag(Ctx *c) { SmpBuilder *b = P(SmpBuilder, 0); b->s.min_filter = (u8)A(1); b->s.mag_filter = (u8)A(2); }
static void sb_wrap(Ctx *c) { SmpBuilder *b = P(SmpBuilder, 0); for (int i = 0; i < 3; i++) b->s.wrap[i] = (u8)A(1 + i); }
static void sb_lod_clamp(Ctx *c) { SmpBuilder *b = P(SmpBuilder, 0); b->s.lod_min = F32(0); b->s.lod_max = F32(1); }
static void sb_lod_bias(Ctx *c) { P(SmpBuilder, 0)->s.lod_bias = F32(0); }
static void sb_compare(Ctx *c) { SmpBuilder *b = P(SmpBuilder, 0); b->s.compare_mode = (u8)A(1); b->s.compare_func = (u8)A(2); }
static void sb_aniso(Ctx *c) { P(SmpBuilder, 0)->s.max_aniso = F32(0); }
static void sb_reduction(Ctx *c) { P(SmpBuilder, 0)->s.reduction = (u8)A(1); }
static void sb_lod_snap(Ctx *c) { P(SmpBuilder, 0)->s.lod_snap = F32(0) != 0; }
static void sb_border(Ctx *c, u8 type) {
    SmpBuilder *b = P(SmpBuilder, 0);
    memcpy(b->s.border, P(void, 1), 16);
    b->s.border_type = type;
}
static void sb_border_f(Ctx *c) { sb_border(c, 0); }
static void sb_border_i(Ctx *c) { sb_border(c, 1); }
static void sb_border_ui(Ctx *c) { sb_border(c, 2); }
static const SamplerState *smp_state(Ctx *c) {
    ObjHdr *o = P(ObjHdr, 0);
    if (o->magic == NVN_MAGIC && o->type == T_SAMPLER) return &((Sampler *)o->host)->s;
    return &P(SmpBuilder, 0)->s;                         /* builder getters share these */
}
static void ret_float(Ctx *c, float f) { memset(&c->v[0], 0, sizeof c->v[0]); memcpy(&c->v[0], &f, 4); }
static void s_get_min_mag(Ctx *c) { const SamplerState *s = smp_state(c); if (A(1)) ST32(A(1), s->min_filter); if (A(2)) ST32(A(2), s->mag_filter); }
static void s_get_wrap(Ctx *c) { const SamplerState *s = smp_state(c); for (int i = 0; i < 3; i++) if (A(1 + i)) ST32(A(1 + i), s->wrap[i]); }
static void s_get_lod_clamp(Ctx *c) {
    const SamplerState *s = smp_state(c);
    u32 v;
    if (A(1)) { memcpy(&v, &s->lod_min, 4); ST32(A(1), v); }
    if (A(2)) { memcpy(&v, &s->lod_max, 4); ST32(A(2), v); }
}
static void s_get_lod_bias(Ctx *c) { ret_float(c, smp_state(c)->lod_bias); }
static void s_get_aniso(Ctx *c) { ret_float(c, smp_state(c)->max_aniso); }
static void s_get_lod_snap(Ctx *c) { ret_float(c, smp_state(c)->lod_snap); }
static void s_get_compare(Ctx *c) { const SamplerState *s = smp_state(c); if (A(1)) ST32(A(1), s->compare_mode); if (A(2)) ST32(A(2), s->compare_func); }
static void s_get_reduction(Ctx *c) { RET(smp_state(c)->reduction); }
static void s_get_border(Ctx *c) { if (A(1)) memcpy(P(void, 1), smp_state(c)->border, 16); }
static void smp_initialize(Ctx *c) {
    Sampler *s = obj_new(A(0), T_SAMPLER, sizeof(Sampler));
    s->s = P(SmpBuilder, 1)->s;
    RET(1);
}
static void smp_finalize(Ctx *c) { obj_free(A(0)); }
static void smp_compare(Ctx *c) {
    Sampler *a = obj_get(A(0), T_SAMPLER), *b = obj_get(A(1), T_SAMPLER);
    RET(a && b && !memcmp(&a->s, &b->s, sizeof a->s));
}

/* ---------------------------------------------------------------------------------------------- descriptor pools */
typedef struct DescPool { u64 pool, offset, cpu; u32 count; } DescPool;
#define DESC_SIZE 32
static void descpool_init(Ctx *c, u32 type) {
    DescPool *p = obj_new(A(0), type, sizeof(DescPool));
    p->pool = A(1); p->offset = A(2); p->count = (u32)A(3);
    p->cpu = pool_storage(p->pool) + p->offset;
    RET(1);
}
static void texpool_initialize(Ctx *c) { descpool_init(c, T_TEXPOOL); }
static void smppool_initialize(Ctx *c) { descpool_init(c, T_SMPPOOL); }
static void descpool_finalize(Ctx *c) { obj_free(A(0)); }
static DescPool *descpool(u64 a) {
    ObjHdr *o = (ObjHdr *)(uintptr_t)a;
    return a && o->magic == NVN_MAGIC && (o->type == T_TEXPOOL || o->type == T_SMPPOOL) ? o->host : NULL;
}
static void descpool_pool(Ctx *c) { DescPool *p = descpool(A(0)); RET(p ? p->pool : 0); }
static void descpool_offset(Ctx *c) { DescPool *p = descpool(A(0)); RET(p ? p->offset : 0); }
static void descpool_size(Ctx *c) { DescPool *p = descpool(A(0)); RET(p ? p->count : 0); }
static void write_desc(DescPool *p, u32 id, u32 magic, u32 index) {
    if (!p || id >= p->count || !p->cpu) return;
    u64 a = p->cpu + (u64)id * DESC_SIZE;
    ST32(a, magic);
    ST32(a + 4, index);
}
/* nvnTexturePoolRegisterTexture(pool, int id, const NVNtexture *, const NVNtextureView *) (and RegisterImage) */
static void texpool_register(Ctx *c) {
    Texture *t = obj_get(A(2), T_TEXTURE);
    if (!t) return;
    TexDesc d;
    memset(&d, 0, sizeof d);
    d.t = t->info;
    view_resolve(t, A(3) ? P(TexView, 3) : NULL, &d.v);
    write_desc(descpool(A(0)), (u32)A(1), TDESC_MAGIC, tdesc_intern(&d));
}
/* nvnSamplerPoolRegisterSampler(pool, int id, const NVNsampler *) / ...SamplerBuilder(pool, id, builder) */
static void smppool_register(Ctx *c) {
    Sampler *s = obj_get(A(2), T_SAMPLER);
    if (s) write_desc(descpool(A(0)), (u32)A(1), SDESC_MAGIC, sdesc_intern(&s->s));
}
static void smppool_register_builder(Ctx *c) {
    SamplerState s = P(SmpBuilder, 2)->s;
    write_desc(descpool(A(0)), (u32)A(1), SDESC_MAGIC, sdesc_intern(&s));
}

/* ---------------------------------------------------------------------------------------------- programs */
typedef struct Program { Shaders *shaders; } Program;
static struct { ShaderCode **items; u32 count, cap; SRWLOCK lock; } codes = {NULL, 0, 0, SRWLOCK_INIT};
static struct { Shaders **items; u32 count, cap; } sets;
static char shader_dir[MAX_PATH];

static void shader_dump(const ShaderCode *s, const void *control) {
    if (!shader_dir[0]) {
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, sizeof exe);
        char *slash = strrchr(exe, '\\');
        if (slash) *slash = 0;
        snprintf(shader_dir, sizeof shader_dir, "%s\\..\\shaders", exe);
        CreateDirectoryA(shader_dir, NULL);
    }
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s\\%016llx.bin", shader_dir, s->hash);
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) return;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    /* "MKSH", control size, code size, stage; control section; code */
    u32 hdr[4] = {0x48534b4d, 0x7d0, (u32)s->size, s->stage};
    fwrite(hdr, 4, 4, f);
    fwrite(control, 1, 0x7d0, f);
    fwrite(PTR(s->code), 1, s->size, f);
    fclose(f);
}

/* the stage from the SPH (word 0: bits 0-4 type, 1 VTG / 2 pixel; bits 10-13 VTG kind); compute has none */
static u32 shader_stage(u64 code) {
    if (LD32(code) != 0x12345678u) return ST_COMPUTE;    /* compute code has no NVN header */
    u32 w0 = LD32(code + 0x30);
    if ((w0 & 0x1f) == 2) return ST_FRAGMENT;
    if ((w0 & 0x1f) == 1) {
        switch (w0 >> 10 & 0xf) {
        case 1: return ST_VERTEX;
        case 2: return ST_TESS_CONTROL;
        case 3: return ST_TESS_EVAL;
        case 4: return ST_GEOMETRY;
        }
    }
    return ST_COMPUTE;
}

static ShaderCode *shader_intern(u64 data, u64 control) {
    /* GLSLC control section: 0x6f8 code size (after the 0x30 header), 0x6fc embedded constants' size,
     * 0x700 their offset, 0x704 the end of everything */
    u64 code = data;                    /* GPU address: identity outside virtual pools, which hold no code */
    u32 end = LD32(control + 0x704), csize = LD32(control + 0x6fc), coff = LD32(control + 0x700);
    if (end < 0x80 || end > 0x100000) end = 0x30 + LD32(control + 0x6f8);
    u64 h = fnv64(PTR(code), end);
    AcquireSRWLockExclusive(&codes.lock);
    for (u32 i = 0; i < codes.count; i++)
        if (codes.items[i]->hash == h) { ReleaseSRWLockExclusive(&codes.lock); return codes.items[i]; }
    ShaderCode *s = calloc(1, sizeof *s);
    s->hash = h;
    s->code = code;
    s->size = end;
    s->stage = shader_stage(code);
    s->const_addr = csize ? data + coff : 0;
    s->const_size = csize;
    if (codes.count == codes.cap) codes.items = realloc(codes.items, (codes.cap = codes.cap * 2 + 64) * sizeof *codes.items);
    codes.items[codes.count++] = s;
    ReleaseSRWLockExclusive(&codes.lock);
    shader_dump(s, PTR(control));
    hlog(2, "nvn: shader %016llx: stage %u, %u bytes, constants %u", h, s->stage, end, csize);
    gpu_shaders_set(s);
    return s;
}

/* nvnProgramSetShaders(program, int count, const NVNshaderData *): {NVNbufferAddress code; const void *control} */
static void program_set_shaders(Ctx *c) {
    Program *p = obj_get(A(0), T_PROGRAM);
    if (!p) { RET(0); return; }
    Shaders set;
    memset(&set, 0, sizeof set);
    u32 n = (u32)A(1);
    for (u32 i = 0; i < n; i++) {
        u64 data = LD64(A(2) + 16 * i), ctl = LD64(A(2) + 16 * i + 8);
        if (!data || !ctl || LD32(ctl) != 0x98761234u) {
            hlog(0, "nvn: SetShaders: stage %u has no GLSLC control section", i);
            continue;
        }
        ShaderCode *s = shader_intern(data, ctl);
        set.stage[s->stage] = s;
        set.mask |= 1u << s->stage;
    }
    for (int i = 0; i < ST_COUNT; i++) set.key = set.key * 31 + (set.stage[i] ? set.stage[i]->hash : 0);
    AcquireSRWLockExclusive(&codes.lock);
    Shaders *found = NULL;
    for (u32 i = 0; i < sets.count && !found; i++)
        if (sets.items[i]->key == set.key && !memcmp(sets.items[i]->stage, set.stage, sizeof set.stage)) found = sets.items[i];
    if (!found) {
        found = malloc(sizeof *found);
        *found = set;
        if (sets.count == sets.cap) sets.items = realloc(sets.items, (sets.cap = sets.cap * 2 + 64) * sizeof *sets.items);
        sets.items[sets.count++] = found;
    }
    ReleaseSRWLockExclusive(&codes.lock);
    p->shaders = found;
    RET(1);
}
static void program_initialize(Ctx *c) { obj_new(A(0), T_PROGRAM, sizeof(Program)); RET(1); }
static void program_finalize(Ctx *c) { obj_free(A(0)); }

/* ---------------------------------------------------------------------------------------------- state blocks */
static bool first_use(const char *what, Ctx *c) {
    if (log_level < 2) return false;
    static const char *seen[256];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == what) return false;
    if (n < 256) seen[n++] = what;
    hlog(2, "nvn: first %s(%llx, %llx, %llx, %llx, %llx)", what, A(1), A(2), A(3), A(4), A(5));
    return true;
}
#define TRACE() first_use(__func__, c)

static void blend_defaults(Ctx *c) {
    BlendState *s = P(BlendState, 0);
    memset(s, 0, sizeof *s);
    s->src_rgb = s->src_a = 2;          /* ONE */
    s->dst_rgb = s->dst_a = 1;          /* ZERO */
    s->eq = 1 | 1 << 4;                 /* ADD */
}
static void blend_target(Ctx *c) { P(BlendState, 0)->target = (u8)A(1); }
static void blend_func(Ctx *c) {
    TRACE();
    BlendState *s = P(BlendState, 0);
    s->src_rgb = (u8)A(1); s->dst_rgb = (u8)A(2); s->src_a = (u8)A(3); s->dst_a = (u8)A(4);
}
static void blend_equation(Ctx *c) { TRACE(); P(BlendState, 0)->eq = (u8)((A(1) & 0xf) | (A(2) & 0xf) << 4); }
static void blend_advanced_mode(Ctx *c) { P(BlendState, 0)->advanced = (u8)A(1); }
static void blend_advanced_overlap(Ctx *c) { BlendState *s = P(BlendState, 0); s->adv_flags = (u8)((s->adv_flags & ~3) | (A(1) & 3)); }
static void blend_advanced_premul(Ctx *c) { BlendState *s = P(BlendState, 0); s->adv_flags = (u8)((s->adv_flags & ~4) | (A(1) ? 4 : 0)); }
static void blend_advanced_normdst(Ctx *c) { BlendState *s = P(BlendState, 0); s->adv_flags = (u8)((s->adv_flags & ~8) | (A(1) ? 8 : 0)); }
static void blend_get_target(Ctx *c) { RET(P(BlendState, 0)->target); }
static void blend_get_func(Ctx *c) {
    BlendState *s = P(BlendState, 0);
    u8 v[4] = {s->src_rgb, s->dst_rgb, s->src_a, s->dst_a};
    for (int i = 0; i < 4; i++) if (A(1 + i)) ST32(A(1 + i), v[i]);
}
static void blend_get_equation(Ctx *c) { BlendState *s = P(BlendState, 0); if (A(1)) ST32(A(1), s->eq & 0xf); if (A(2)) ST32(A(2), s->eq >> 4); }
static void blend_get_advanced_mode(Ctx *c) { RET(P(BlendState, 0)->advanced); }
static void blend_get_advanced_overlap(Ctx *c) { RET(P(BlendState, 0)->adv_flags & 3); }
static void blend_get_advanced_premul(Ctx *c) { RET(!!(P(BlendState, 0)->adv_flags & 4)); }
static void blend_get_advanced_normdst(Ctx *c) { RET(!!(P(BlendState, 0)->adv_flags & 8)); }

static void color_defaults(Ctx *c) { ColorState *s = P(ColorState, 0); memset(s, 0, sizeof *s); s->logic_op = 3; s->alpha_test = 8; }
static void color_blend_enable(Ctx *c) {
    ColorState *s = P(ColorState, 0);
    u32 i = (u32)A(1) & 7;
    s->blend_enable = (u8)((s->blend_enable & ~(1u << i)) | (A(2) & 1 ? 1u << i : 0));
}
static void color_logic_op(Ctx *c) { TRACE(); P(ColorState, 0)->logic_op = (u8)A(1); }
static void color_alpha_test(Ctx *c) { TRACE(); P(ColorState, 0)->alpha_test = (u8)A(1); }
static void color_get_blend_enable(Ctx *c) { RET(P(ColorState, 0)->blend_enable >> (A(1) & 7) & 1); }
static void color_get_logic_op(Ctx *c) { RET(P(ColorState, 0)->logic_op); }
static void color_get_alpha_test(Ctx *c) { RET(P(ColorState, 0)->alpha_test); }

static void mask_defaults(Ctx *c) { P(ChannelMaskState, 0)->mask = 0xffffffffu; }
static void mask_set(Ctx *c) {
    ChannelMaskState *s = P(ChannelMaskState, 0);
    u32 i = (u32)A(1) & 7;
    u32 m = (A(2) & 1) | (A(3) & 1) << 1 | (A(4) & 1) << 2 | (A(5) & 1) << 3;
    s->mask = (s->mask & ~(0xfu << 4 * i)) | m << 4 * i;
}
static void mask_get(Ctx *c) {
    u32 m = P(ChannelMaskState, 0)->mask >> 4 * (A(1) & 7);
    for (int k = 0; k < 4; k++) if (A(2 + k)) ST32(A(2 + k), m >> k & 1);
}

static void ds_defaults(Ctx *c) {
    DepthStencilState *s = P(DepthStencilState, 0);
    memset(s, 0, sizeof *s);
    s->flags = 2;                       /* depth writes on, tests off */
    s->depth_func = 2;                  /* LESS */
    s->stencil_func = 8 | 8 << 4;       /* ALWAYS */
    s->fail = s->zfail = s->zpass = 1 | 1 << 4;     /* KEEP */
}
static void ds_flag(Ctx *c, u8 bit) { DepthStencilState *s = P(DepthStencilState, 0); s->flags = (u8)(A(1) & 1 ? s->flags | bit : s->flags & ~bit); }
static void ds_depth_test(Ctx *c) { ds_flag(c, 1); }
static void ds_depth_write(Ctx *c) { ds_flag(c, 2); }
static void ds_stencil_test(Ctx *c) { ds_flag(c, 4); }
static void ds_depth_func(Ctx *c) { TRACE(); P(DepthStencilState, 0)->depth_func = (u8)A(1); }
static u8 face_set(u8 old, u32 face, u32 v) {
    if (face & 1) old = (u8)((old & 0xf0) | (v & 0xf));
    if (face & 2) old = (u8)((old & 0x0f) | (v & 0xf) << 4);
    return old;
}
static void ds_stencil_func(Ctx *c) { TRACE(); DepthStencilState *s = P(DepthStencilState, 0); s->stencil_func = face_set(s->stencil_func, (u32)A(1), (u32)A(2)); }
static void ds_stencil_op(Ctx *c) {
    TRACE();
    DepthStencilState *s = P(DepthStencilState, 0);
    u32 f = (u32)A(1);
    s->fail = face_set(s->fail, f, (u32)A(2));
    s->zfail = face_set(s->zfail, f, (u32)A(3));
    s->zpass = face_set(s->zpass, f, (u32)A(4));
}
static void ds_get_depth_test(Ctx *c) { RET(P(DepthStencilState, 0)->flags & 1); }
static void ds_get_depth_write(Ctx *c) { RET(P(DepthStencilState, 0)->flags >> 1 & 1); }
static void ds_get_stencil_test(Ctx *c) { RET(P(DepthStencilState, 0)->flags >> 2 & 1); }
static void ds_get_depth_func(Ctx *c) { RET(P(DepthStencilState, 0)->depth_func); }
static u32 face_get(u8 v, u32 face) { return face == 2 ? v >> 4 : v & 0xf; }
static void ds_get_stencil_func(Ctx *c) { RET(face_get(P(DepthStencilState, 0)->stencil_func, (u32)A(1))); }
static void ds_get_stencil_op(Ctx *c) {
    DepthStencilState *s = P(DepthStencilState, 0);
    u32 f = (u32)A(1);
    if (A(2)) ST32(A(2), face_get(s->fail, f));
    if (A(3)) ST32(A(3), face_get(s->zfail, f));
    if (A(4)) ST32(A(4), face_get(s->zpass, f));
}

static void poly_defaults(Ctx *c) { PolygonState *s = P(PolygonState, 0); s->cull = 0; s->front_face = 1; s->mode = 2; s->offset_enables = 0; }
static void poly_cull(Ctx *c) { TRACE(); P(PolygonState, 0)->cull = (u8)A(1); }
static void poly_front_face(Ctx *c) { TRACE(); P(PolygonState, 0)->front_face = (u8)A(1); }
static void poly_mode(Ctx *c) { TRACE(); P(PolygonState, 0)->mode = (u8)A(1); }
static void poly_offset_enables(Ctx *c) { P(PolygonState, 0)->offset_enables = (u8)A(1); }
static void poly_get_cull(Ctx *c) { RET(P(PolygonState, 0)->cull); }
static void poly_get_front_face(Ctx *c) { RET(P(PolygonState, 0)->front_face); }
static void poly_get_mode(Ctx *c) { RET(P(PolygonState, 0)->mode); }
static void poly_get_offset_enables(Ctx *c) { RET(P(PolygonState, 0)->offset_enables); }

static void ms_defaults(Ctx *c) { MultisampleState *s = P(MultisampleState, 0); memset(s, 0, sizeof *s); s->enable = 1; }
#define MS_SET(name, field) static void name(Ctx *c) { P(MultisampleState, 0)->field = (u8)A(1); } \
                            static void name##_get(Ctx *c) { RET(P(MultisampleState, 0)->field); }
MS_SET(ms_enable, enable)
MS_SET(ms_samples, samples)
MS_SET(ms_a2c, a2c)
MS_SET(ms_a2c_dither, a2c_dither)
MS_SET(ms_raster_samples, raster_samples)
MS_SET(ms_cov_mod, cov_mod)
MS_SET(ms_cov_to_color, cov_to_color)
MS_SET(ms_cov_to_color_out, cov_to_color_out)
MS_SET(ms_sample_loc, sample_loc)
MS_SET(ms_sample_loc_grid, sample_loc_grid)

static void va_defaults(Ctx *c) { P(VertexAttribState, 0)->v = 0; }
/* nvnVertexAttribStateSetFormat(state, NVNformat, ptrdiff_t relativeOffset) */
static void va_format(Ctx *c) { VertexAttribState *s = P(VertexAttribState, 0); s->v = (s->v & 0xff000000u) | ((u32)A(1) & 0x3ff) | ((u32)A(2) & 0x3fff) << 10; }
static void va_stream(Ctx *c) { VertexAttribState *s = P(VertexAttribState, 0); s->v = (s->v & 0x00ffffffu) | ((u32)A(1) & 0x3f) << 24; }
static void va_get_format(Ctx *c) { VertexAttribState *s = P(VertexAttribState, 0); if (A(1)) ST32(A(1), VA_FORMAT(*s)); if (A(2)) ST64(A(2), VA_OFFSET(*s)); }
static void va_get_stream(Ctx *c) { RET(VA_STREAM(*P(VertexAttribState, 0))); }
static void vs_defaults(Ctx *c) { VertexStreamState *s = P(VertexStreamState, 0); s->stride = 0; s->divisor = 0; }
static void vs_stride(Ctx *c) { P(VertexStreamState, 0)->stride = (u32)A(1); }
static void vs_divisor(Ctx *c) { P(VertexStreamState, 0)->divisor = (u32)A(1); }
static void vs_get_stride(Ctx *c) { RET(P(VertexStreamState, 0)->stride); }
static void vs_get_divisor(Ctx *c) { RET(P(VertexStreamState, 0)->divisor); }

/* ---------------------------------------------------------------------------------------------- command buffers */
typedef struct CmdBuf {
    u64 self;
    u64 cur, end, chunk;                /* write position, end and start of the current command memory */
    u64 start;                          /* first command of the recording */
    u64 cmd_total, ctrl_total, ctrl_used;
    u64 callback, callback_data;
    s64 row_stride, image_stride;       /* nvnCommandBufferSetCopyRowStride / ImageStride */
    bool recording;
} CmdBuf;
#define JUMP_SIZE 12                    /* kept free at the end of command memory: header + address */

static CmdBuf *cmdbuf(Ctx *c) { return obj_get(A(0), T_CMDBUF); }

static void cmdbuf_initialize(Ctx *c) { CmdBuf *b = obj_new(A(0), T_CMDBUF, sizeof(CmdBuf)); b->self = A(0); RET(1); }
static void cmdbuf_finalize(Ctx *c) { obj_free(A(0)); }

static void put_jump(u64 at, u64 to) {
    ST32(at, OP_JUMP | 8u << 8);
    ST64(at + 4, to);
}

/* new command memory: continue the recording there */
static void switch_memory(CmdBuf *b, u64 start, u64 size) {
    if (b->recording && b->cur && b->cur + JUMP_SIZE <= b->end) put_jump(b->cur, start);
    b->cur = b->chunk = start;
    b->end = start + size;
    if (b->recording && !b->start) b->start = start;
}

/* nvnCommandBufferAddCommandMemory(cmd, const NVNmemoryPool *, ptrdiff_t offset, size_t size) */
static void cmdbuf_add_command_memory(Ctx *c) {
    CmdBuf *b = cmdbuf(c);
    if (!b) return;
    u64 at = pool_storage(A(1)) + A(2);
    switch_memory(b, at, A(3));
    b->cmd_total += A(3);
}
static void cmdbuf_add_control_memory(Ctx *c) { CmdBuf *b = cmdbuf(c); if (b) { b->ctrl_total += A(2); b->ctrl_used = 0; } }
static void cmdbuf_command_size(Ctx *c) { CmdBuf *b = cmdbuf(c); RET(b ? b->end - b->chunk : 0); }
static void cmdbuf_control_size(Ctx *c) { CmdBuf *b = cmdbuf(c); RET(b ? b->ctrl_total : 0); }
static void cmdbuf_command_used(Ctx *c) { CmdBuf *b = cmdbuf(c); RET(b ? b->cur - b->chunk : 0); }
static void cmdbuf_command_free(Ctx *c) {
    CmdBuf *b = cmdbuf(c);
    RET(b && b->end > b->cur + JUMP_SIZE ? b->end - b->cur - JUMP_SIZE : 0);
}
static void cmdbuf_control_used(Ctx *c) { CmdBuf *b = cmdbuf(c); RET(b ? b->ctrl_used : 0); }
static void cmdbuf_control_free(Ctx *c) { CmdBuf *b = cmdbuf(c); RET(b ? b->ctrl_total - b->ctrl_used : 0); }
static void cmdbuf_set_callback(Ctx *c) { CmdBuf *b = cmdbuf(c); if (b) b->callback = A(1); }
static void cmdbuf_set_callback_data(Ctx *c) { CmdBuf *b = cmdbuf(c); if (b) b->callback_data = A(1); }
static void cmdbuf_get_callback(Ctx *c) { CmdBuf *b = cmdbuf(c); RET(b ? b->callback : 0); }
static void cmdbuf_get_callback_data(Ctx *c) { CmdBuf *b = cmdbuf(c); RET(b ? b->callback_data : 0); }
static void cmdbuf_begin(Ctx *c) {
    CmdBuf *b = cmdbuf(c);
    if (!b) return;
    b->recording = true;
    b->start = b->cur;
}
static void cmdbuf_is_recording(Ctx *c) { CmdBuf *b = cmdbuf(c); RET(b && b->recording); }
static void cmdbuf_set_row_stride(Ctx *c) { CmdBuf *b = cmdbuf(c); if (b) b->row_stride = (s64)A(1); }
static void cmdbuf_set_image_stride(Ctx *c) { CmdBuf *b = cmdbuf(c); if (b) b->image_stride = (s64)A(1); }
static void cmdbuf_get_row_stride(Ctx *c) { CmdBuf *b = cmdbuf(c); RET(b ? b->row_stride : 0); }
static void cmdbuf_get_image_stride(Ctx *c) { CmdBuf *b = cmdbuf(c); RET(b ? b->image_stride : 0); }

/* room for `need` bytes of command: from the game's memory callback (OUT_OF_COMMAND_MEMORY), or from the host */
static void cmd_room(Ctx *c, CmdBuf *b, u32 need) {
    if (b->cur && b->cur + need + JUMP_SIZE <= b->end) return;
    if (b->callback) {
        Ctx saved = *c;
        c->x[0] = b->self;
        c->x[1] = 0;
        c->x[2] = need + JUMP_SIZE;
        c->x[3] = b->callback_data;
        guest_call(c, b->callback);
        *c = saved;
        if (b->cur && b->cur + need + JUMP_SIZE <= b->end) return;
    }
    static bool logged;
    if (!logged) { logged = true; hlog(0, "nvn: command buffer %llx out of memory without a callback: using host memory", b->self); }
    u64 size = need + JUMP_SIZE > 0x100000 ? need + JUMP_SIZE : 0x100000;
    switch_memory(b, (u64)(uintptr_t)malloc(size), size);
}

static u8 *cmd_put(Ctx *c, CmdBuf *b, u32 op, u32 len) {
    len = (len + 3) & ~3u;
    if (!b->recording) { b->recording = true; b->start = b->cur; }
    cmd_room(c, b, 4 + len);
    u64 at = b->cur;
    if (!b->start) b->start = at;
    ST32(at, op | len << 8);
    b->cur = at + 4 + len;
    return PTR(at + 4);
}

static void cmdbuf_end(Ctx *c) {
    CmdBuf *b = cmdbuf(c);
    if (!b) { RET(0); return; }
    cmd_put(c, b, OP_END, 0);
    u64 h = b->start;
    b->recording = false;
    b->start = 0;
    b->ctrl_used += 16;
    if (b->ctrl_used > b->ctrl_total) b->ctrl_used = b->ctrl_total;
    RET(h);
}

/* emit a command with a fixed payload: `p` points at the payload */
#define EMIT(op, T) CmdBuf *b = cmdbuf(c); if (!b) return; T *p = (T *)cmd_put(c, b, op, sizeof(T))
#define EMIT_N(op, n) CmdBuf *b = cmdbuf(c); if (!b) return; u8 *p = cmd_put(c, b, op, (u32)(n))

static void cmd_bind_blend(Ctx *c) { EMIT(OP_BLEND, BlendState); *p = *P(BlendState, 1); }
static void cmd_bind_channel_mask(Ctx *c) { EMIT(OP_CHANNEL_MASK, ChannelMaskState); *p = *P(ChannelMaskState, 1); }
static void cmd_bind_color(Ctx *c) { EMIT(OP_COLOR, ColorState); *p = *P(ColorState, 1); }
static void cmd_bind_multisample(Ctx *c) { EMIT(OP_MULTISAMPLE, MultisampleState); *p = *P(MultisampleState, 1); }
static void cmd_bind_polygon(Ctx *c) { EMIT(OP_POLYGON, PolygonState); *p = *P(PolygonState, 1); }
static void cmd_bind_depth_stencil(Ctx *c) { EMIT(OP_DEPTH_STENCIL, DepthStencilState); *p = *P(DepthStencilState, 1); }
static void cmd_bind_vertex_attribs(Ctx *c) {
    u32 n = (u32)A(1) > 32 ? 32 : (u32)A(1);
    EMIT_N(OP_VERTEX_ATTRIBS, 4 + 4 * n);
    memcpy(p, &n, 4);
    memcpy(p + 4, P(void, 2), 4 * n);
}
static void cmd_bind_vertex_streams(Ctx *c) {
    u32 n = (u32)A(1) > 32 ? 32 : (u32)A(1);
    EMIT_N(OP_VERTEX_STREAMS, 4 + 8 * n);
    memcpy(p, &n, 4);
    memcpy(p + 4, P(void, 2), 8 * n);
}
/* nvnCommandBufferBindProgram(cmd, const NVNprogram *, int stageBits) */
static void cmd_bind_program(Ctx *c) {
    Program *pr = obj_get(A(1), T_PROGRAM);
    struct { u64 shaders; u32 stages; } v = {pr ? (u64)(uintptr_t)pr->shaders : 0, (u32)A(2)};
    EMIT_N(OP_PROGRAM, 12);
    memcpy(p, &v.shaders, 8);
    memcpy(p + 8, &v.stages, 4);
}

/* bindings: {stage, first, count} then the values */
static void emit_bind(Ctx *c, u32 op, u32 stage, u32 first, u32 count, const void *vals, u32 each) {
    if (count > 64) count = 64;
    EMIT_N(op, sizeof(CmdBind) + count * each);
    CmdBind h = {stage, first, count};
    memcpy(p, &h, sizeof h);
    memcpy(p + sizeof h, vals, (size_t)count * each);
}
static void cmd_bind_vertex_buffer(Ctx *c) { BufferRange r = {A(2), A(3)}; emit_bind(c, OP_VERTEX_BUFFERS, 0, (u32)A(1), 1, &r, 16); }
static void cmd_bind_vertex_buffers(Ctx *c) { emit_bind(c, OP_VERTEX_BUFFERS, 0, (u32)A(1), (u32)A(2), P(void, 3), 16); }
static void cmd_bind_uniform_buffer(Ctx *c) { BufferRange r = {A(3), A(4)}; emit_bind(c, OP_UNIFORM_BUFFERS, (u32)A(1), (u32)A(2), 1, &r, 16); }
static void cmd_bind_uniform_buffers(Ctx *c) { emit_bind(c, OP_UNIFORM_BUFFERS, (u32)A(1), (u32)A(2), (u32)A(3), P(void, 4), 16); }
static void cmd_bind_storage_buffer(Ctx *c) { BufferRange r = {A(3), A(4)}; emit_bind(c, OP_STORAGE_BUFFERS, (u32)A(1), (u32)A(2), 1, &r, 16); }
static void cmd_bind_storage_buffers(Ctx *c) { emit_bind(c, OP_STORAGE_BUFFERS, (u32)A(1), (u32)A(2), (u32)A(3), P(void, 4), 16); }
static void cmd_bind_xfb_buffer(Ctx *c) { BufferRange r = {A(2), A(3)}; emit_bind(c, OP_XFB_BUFFERS, 0, (u32)A(1), 1, &r, 16); }
static void cmd_bind_xfb_buffers(Ctx *c) { emit_bind(c, OP_XFB_BUFFERS, 0, (u32)A(1), (u32)A(2), P(void, 3), 16); }
static void cmd_bind_texture(Ctx *c) { u64 h = A(3); emit_bind(c, OP_TEXTURES, (u32)A(1), (u32)A(2), 1, &h, 8); }
static void cmd_bind_textures(Ctx *c) { emit_bind(c, OP_TEXTURES, (u32)A(1), (u32)A(2), (u32)A(3), P(void, 4), 8); }
static void cmd_bind_image(Ctx *c) { u64 h = A(3); emit_bind(c, OP_IMAGES, (u32)A(1), (u32)A(2), 1, &h, 8); }
static void cmd_bind_images(Ctx *c) { emit_bind(c, OP_IMAGES, (u32)A(1), (u32)A(2), (u32)A(3), P(void, 4), 8); }
static void cmd_bind_sep_texture(Ctx *c) { u64 h = A(3); emit_bind(c, OP_SEP_TEXTURES, (u32)A(1), (u32)A(2), 1, &h, 8); }
static void cmd_bind_sep_textures(Ctx *c) { emit_bind(c, OP_SEP_TEXTURES, (u32)A(1), (u32)A(2), (u32)A(3), P(void, 4), 8); }
static void cmd_bind_sep_sampler(Ctx *c) { u64 h = A(3); emit_bind(c, OP_SEP_SAMPLERS, (u32)A(1), (u32)A(2), 1, &h, 8); }
static void cmd_bind_sep_samplers(Ctx *c) { emit_bind(c, OP_SEP_SAMPLERS, (u32)A(1), (u32)A(2), (u32)A(3), P(void, 4), 8); }

/* nvnCommandBufferUpdateUniformBuffer(cmd, buffer, size_t bufferSize, ptrdiff_t offset, size_t size, data) */
static void cmd_update_ubo(Ctx *c) {
    u32 len = (u32)A(4) > 0x10000 ? 0x10000 : (u32)A(4);
    CmdUpdateUbo h = {A(1), A(2), A(3), len};
    EMIT_N(OP_UPDATE_UBO, sizeof h + len);
    memcpy(p, &h, sizeof h);
    memcpy(p + sizeof h, P(void, 5), len);
}

static void emit_draw(Ctx *c, const CmdDraw *d) { EMIT(d->index_type != ~0u ? OP_DRAW_ELEMENTS : OP_DRAW_ARRAYS, CmdDraw); *p = *d; }
static void cmd_draw_arrays(Ctx *c) { CmdDraw d = {(u32)A(1), ~0u, (u32)A(2), (u32)A(3), 0, 0, 1, 0}; emit_draw(c, &d); }
static void cmd_draw_arrays_instanced(Ctx *c) { CmdDraw d = {(u32)A(1), ~0u, (u32)A(2), (u32)A(3), 0, (u32)A(4), (u32)A(5), 0}; emit_draw(c, &d); }
static void cmd_draw_elements(Ctx *c) { CmdDraw d = {(u32)A(1), (u32)A(2), 0, (u32)A(3), 0, 0, 1, A(4)}; emit_draw(c, &d); }
static void cmd_draw_elements_base_vertex(Ctx *c) { CmdDraw d = {(u32)A(1), (u32)A(2), 0, (u32)A(3), (s32)A(5), 0, 1, A(4)}; emit_draw(c, &d); }
static void cmd_draw_elements_instanced(Ctx *c) {
    CmdDraw d = {(u32)A(1), (u32)A(2), 0, (u32)A(3), (s32)A(5), (u32)A(6), (u32)A(7), A(4)};
    emit_draw(c, &d);
}
static void cmd_draw_arrays_indirect(Ctx *c) { EMIT(OP_DRAW_ARRAYS_INDIRECT, CmdDrawIndirect); *p = (CmdDrawIndirect){(u32)A(1), ~0u, 0, A(2), 0, 1, 16}; }
static void cmd_draw_elements_indirect(Ctx *c) { EMIT(OP_DRAW_ELEMENTS_INDIRECT, CmdDrawIndirect); *p = (CmdDrawIndirect){(u32)A(1), (u32)A(2), A(3), A(4), 0, 1, 20}; }
/* MultiDrawArraysIndirectCount(cmd, mode, indirect, parameterBuffer, maxDrawCount, stride) */
static void cmd_multi_draw_arrays(Ctx *c) {
    EMIT(OP_MULTI_DRAW_INDIRECT, CmdDrawIndirect);
    *p = (CmdDrawIndirect){(u32)A(1), ~0u, 0, A(2), A(3), (u32)A(4), (u32)A(5)};
}
/* MultiDrawElementsIndirectCount(cmd, mode, type, indexBuffer, indirect, parameterBuffer, maxDrawCount, stride):
 * the ninth argument is on the stack */
static void cmd_multi_draw_elements(Ctx *c) {
    u32 stride = LD32(c->x[31]);
    EMIT(OP_MULTI_DRAW_INDIRECT, CmdDrawIndirect);
    *p = (CmdDrawIndirect){(u32)A(1), (u32)A(2), A(3), A(4), A(5), (u32)A(6), stride};
}
static void cmd_dispatch(Ctx *c) { u32 v[3] = {(u32)A(1), (u32)A(2), (u32)A(3)}; EMIT_N(OP_DISPATCH, 12); memcpy(p, v, 12); }
static void cmd_dispatch_indirect(Ctx *c) { u64 a = A(1); EMIT_N(OP_DISPATCH_INDIRECT, 8); memcpy(p, &a, 8); }
/* nvnCommandBufferDrawTexture(cmd, NVNtextureHandle, const NVNdrawTextureRegion *dst, const ... *src) */
static void cmd_draw_texture(Ctx *c) {
    EMIT(OP_DRAW_TEXTURE, CmdDrawTexture);
    p->handle = A(1);
    memcpy(p->dst, P(void, 2), 16);
    memcpy(p->src, P(void, 3), 16);
}
static void cmd_draw_xfb(Ctx *c) { u32 m = (u32)A(1); u64 a = A(2); EMIT_N(OP_DRAW_XFB, 12); memcpy(p, &m, 4); memcpy(p + 4, &a, 8); }

static void emit_range(Ctx *c, u32 op, u32 first, u32 count, const void *vals, u32 each) {
    if (count > 16) count = 16;
    EMIT_N(op, sizeof(CmdRange) + count * each);
    CmdRange h = {first, count};
    memcpy(p, &h, sizeof h);
    memcpy(p + sizeof h, vals, (size_t)count * each);
}
static void cmd_viewport(Ctx *c) { float v[4] = {(float)(s32)A(1), (float)(s32)A(2), (float)(s32)A(3), (float)(s32)A(4)}; emit_range(c, OP_VIEWPORTS, 0, 1, v, 16); }
static void cmd_viewports(Ctx *c) { emit_range(c, OP_VIEWPORTS, (u32)A(1), (u32)A(2), P(void, 3), 16); }
static void cmd_scissor(Ctx *c) { s32 v[4] = {(s32)A(1), (s32)A(2), (s32)A(3), (s32)A(4)}; emit_range(c, OP_SCISSORS, 0, 1, v, 16); }
static void cmd_scissors(Ctx *c) { emit_range(c, OP_SCISSORS, (u32)A(1), (u32)A(2), P(void, 3), 16); }
static void cmd_depth_range(Ctx *c) { float v[2] = {F32(0), F32(1)}; emit_range(c, OP_DEPTH_RANGES, 0, 1, v, 8); }
static void cmd_depth_ranges(Ctx *c) { emit_range(c, OP_DEPTH_RANGES, (u32)A(1), (u32)A(2), P(void, 3), 8); }
static void cmd_depth_bounds(Ctx *c) {
    u32 e = (u32)A(1) & 1;
    float v[2] = {F32(0), F32(1)};
    EMIT_N(OP_DEPTH_BOUNDS, 12);
    memcpy(p, &e, 4);
    memcpy(p + 4, v, 8);
}
static void emit_u32x2(Ctx *c, u32 op, u32 a, u32 b2) { EMIT_N(op, 8); memcpy(p, &a, 4); memcpy(p + 4, &b2, 4); }
static void emit_floats(Ctx *c, u32 op, const float *v, u32 n) { EMIT_N(op, 4 * n); memcpy(p, v, 4 * n); }
static void cmd_stencil_value_mask(Ctx *c) { emit_u32x2(c, OP_STENCIL_VALUE_MASK, (u32)A(1), (u32)A(2)); }
static void cmd_stencil_mask(Ctx *c) { emit_u32x2(c, OP_STENCIL_MASK, (u32)A(1), (u32)A(2)); }
static void cmd_stencil_ref(Ctx *c) { emit_u32x2(c, OP_STENCIL_REF, (u32)A(1), (u32)A(2)); }
static void cmd_blend_color(Ctx *c) { float v[4]; memcpy(v, P(void, 1), 16); emit_floats(c, OP_BLEND_COLOR, v, 4); }
static void cmd_point_size(Ctx *c) { float f = F32(0); emit_floats(c, OP_POINT_SIZE, &f, 1); }
static void cmd_line_width(Ctx *c) { float f = F32(0); emit_floats(c, OP_LINE_WIDTH, &f, 1); }
static void cmd_polygon_offset(Ctx *c) { float f[3] = {F32(0), F32(1), F32(2)}; emit_floats(c, OP_POLYGON_OFFSET, f, 3); }
static void cmd_alpha_ref(Ctx *c) { float f = F32(0); emit_floats(c, OP_ALPHA_REF, &f, 1); }
static void cmd_sample_mask(Ctx *c) { emit_u32x2(c, OP_SAMPLE_MASK, (u32)A(1), 0); }
static void cmd_rasterizer_discard(Ctx *c) { emit_u32x2(c, OP_RASTERIZER_DISCARD, (u32)A(1) & 1, 0); }
static void cmd_depth_clamp(Ctx *c) { emit_u32x2(c, OP_DEPTH_CLAMP, (u32)A(1) & 1, 0); }
static void cmd_primitive_restart(Ctx *c) { emit_u32x2(c, OP_PRIMITIVE_RESTART, (u32)A(1) & 1, (u32)A(2)); }
static void cmd_patch_size(Ctx *c) { emit_u32x2(c, OP_PATCH_SIZE, (u32)A(1), 0); }
static void cmd_render_enable(Ctx *c) { emit_u32x2(c, OP_RENDER_ENABLE, (u32)A(1) & 1, 0); }

/* a texture by value (TexRef): what it is and which part of it */
static bool texref(u64 tex, u64 view, TexRef *r) {
    Texture *t = obj_get(tex, T_TEXTURE);
    memset(r, 0, sizeof *r);
    if (!t) return false;
    r->t = t->info;
    view_resolve(t, view ? (const TexView *)PTR(view) : NULL, &r->v);
    return true;
}

/* nvnCommandBufferSetRenderTargets(cmd, int n, const NVNtexture *const *colors, const NVNtextureView *const *views,
 *                                  const NVNtexture *depth, const NVNtextureView *depthView) */
static void cmd_render_targets(Ctx *c) {
    u32 n = (u32)A(1) > 8 ? 8 : (u32)A(1);
    TexRef refs[9];
    for (u32 i = 0; i < n; i++)
        texref(A(2) ? LD64(A(2) + 8 * i) : 0, A(3) ? LD64(A(3) + 8 * i) : 0, &refs[i]);
    bool depth = texref(A(4), A(5), &refs[n]);
    EMIT_N(OP_RENDER_TARGETS, sizeof(CmdRenderTargets) + (n + 1) * sizeof(TexRef));
    CmdRenderTargets h = {n, depth};
    memcpy(p, &h, sizeof h);
    memcpy(p + sizeof h, refs, (n + 1) * sizeof(TexRef));
}
/* nvnCommandBufferClearColor(cmd, int index, const float *color, int mask) and the i / ui forms */
static void clear_color(Ctx *c, u32 type) {
    CmdClearColor v = {(u32)A(1), (u32)A(3), type, {0}};
    memcpy(v.v, P(void, 2), 16);
    EMIT(OP_CLEAR_COLOR, CmdClearColor);
    *p = v;
}
static void cmd_clear_color(Ctx *c) { clear_color(c, 0); }
static void cmd_clear_color_i(Ctx *c) { clear_color(c, 1); }
static void cmd_clear_color_ui(Ctx *c) { clear_color(c, 2); }
/* nvnCommandBufferClearDepthStencil(cmd, float depth, NVNboolean depthMask, int stencil, int stencilMask) */
static void cmd_clear_depth_stencil(Ctx *c) {
    CmdClearDS v = {F32(0), (u32)A(1) & 1, (u32)A(2), (u32)A(3)};
    EMIT(OP_CLEAR_DEPTH_STENCIL, CmdClearDS);
    *p = v;
}
/* nvnCommandBufferClearTexture(cmd, tex, view, const NVNcopyRegion *, const float *color, int mask) */
static void clear_texture(Ctx *c, u32 type) {
    CmdClearTexture v;
    if (!texref(A(1), A(2), &v.tex)) return;
    v.r = *P(CopyRegion, 3);
    memcpy(v.v, P(void, 4), 16);
    v.mask = (u32)A(5);
    v.type = type;
    EMIT(OP_CLEAR_TEXTURE, CmdClearTexture);
    *p = v;
}
static void cmd_clear_texture(Ctx *c) { clear_texture(c, 0); }
static void cmd_clear_texture_i(Ctx *c) { clear_texture(c, 1); }
static void cmd_clear_texture_ui(Ctx *c) { clear_texture(c, 2); }
static void cmd_discard_color(Ctx *c) { emit_u32x2(c, OP_DISCARD, (u32)A(1), 0); }
static void cmd_discard_depth_stencil(Ctx *c) { emit_u32x2(c, OP_DISCARD, ~0u, 0); }

/* nvnCommandBufferCopyBufferToTexture(cmd, buffer, dstTex, dstView, dstRegion, flags) */
static void cmd_copy_buffer_to_texture(Ctx *c) {
    CmdBuf *b0 = cmdbuf(c);
    CmdCopyBT v;
    if (!b0 || !texref(A(2), A(3), &v.tex)) return;
    v.buffer = A(1); v.r = *P(CopyRegion, 4); v.flags = (u32)A(5);
    v.row_stride = b0->row_stride; v.image_stride = b0->image_stride;
    EMIT(OP_COPY_BUFFER_TO_TEXTURE, CmdCopyBT);
    *p = v;
}
/* nvnCommandBufferCopyTextureToBuffer(cmd, srcTex, srcView, srcRegion, buffer, flags) */
static void cmd_copy_texture_to_buffer(Ctx *c) {
    CmdBuf *b0 = cmdbuf(c);
    CmdCopyBT v;
    if (!b0 || !texref(A(1), A(2), &v.tex)) return;
    v.buffer = A(4); v.r = *P(CopyRegion, 3); v.flags = (u32)A(5);
    v.row_stride = b0->row_stride; v.image_stride = b0->image_stride;
    EMIT(OP_COPY_TEXTURE_TO_BUFFER, CmdCopyBT);
    *p = v;
}
/* nvnCommandBufferCopyTextureToTexture(cmd, src, srcView, srcRegion, dst, dstView, dstRegion, flags) */
static void cmd_copy_texture_to_texture(Ctx *c) {
    CmdCopyTT v;
    if (!texref(A(1), A(2), &v.src) || !texref(A(4), A(5), &v.dst)) return;
    v.sr = *P(CopyRegion, 3); v.dr = *P(CopyRegion, 6); v.flags = (u32)A(7);
    EMIT(OP_COPY_TEXTURE_TO_TEXTURE, CmdCopyTT);
    *p = v;
}
static void cmd_copy_buffer(Ctx *c) { CmdCopyBuffer v = {A(1), A(2), A(3), (u32)A(4)}; EMIT(OP_COPY_BUFFER, CmdCopyBuffer); *p = v; }
static void cmd_clear_buffer(Ctx *c) { CmdClearBuffer v = {A(1), A(2), (u32)A(3)}; EMIT(OP_CLEAR_BUFFER, CmdClearBuffer); *p = v; }
/* Downsample(cmd, src, dst) and DownsampleTextureView(cmd, src, srcView, dst, dstView) */
static void cmd_downsample(Ctx *c) {
    CmdDownsample v;
    if (!texref(A(1), 0, &v.src) || !texref(A(2), 0, &v.dst)) return;
    EMIT(OP_DOWNSAMPLE, CmdDownsample);
    *p = v;
}
static void cmd_downsample_view(Ctx *c) {
    CmdDownsample v;
    if (!texref(A(1), A(2), &v.src) || !texref(A(3), A(4), &v.dst)) return;
    EMIT(OP_DOWNSAMPLE, CmdDownsample);
    *p = v;
}
static void cmd_barrier(Ctx *c) { emit_u32x2(c, OP_BARRIER, (u32)A(1), 0); }
/* nvnCommandBufferFenceSync(cmd, sync, condition, flags) / WaitSync(cmd, sync) */
static void cmd_fence_sync(Ctx *c) { CmdSync v = {A(1), (u32)A(2), (u32)A(3)}; EMIT(OP_FENCE_SYNC, CmdSync); *p = v; }
static void cmd_wait_sync(Ctx *c) { CmdSync v = {A(1), 0, 0}; EMIT(OP_WAIT_SYNC, CmdSync); *p = v; }

/* ---------------------------------------------------------------------------------------------- events */
typedef struct EventBuilder { u64 pool, offset; } EventBuilder;
typedef struct Event { u64 addr, gpu; } Event;
static void eb_defaults(Ctx *c) { memset(P(void, 0), 0, sizeof(EventBuilder)); }
static void eb_storage(Ctx *c) { P(EventBuilder, 0)->pool = A(1); P(EventBuilder, 0)->offset = A(2); }
static void event_initialize(Ctx *c) {
    EventBuilder *b = P(EventBuilder, 1);
    Event *e = obj_new(A(0), T_EVENT, sizeof(Event));
    e->addr = pool_storage(b->pool) + b->offset;
    e->gpu = pool_gpu(b->pool) + b->offset;
    RET(1);
}
static void event_finalize(Ctx *c) { obj_free(A(0)); }
static void event_get_value(Ctx *c) { Event *e = obj_get(A(0), T_EVENT); RET(e && e->addr ? LD32(e->addr) : 0); }
/* nvnEventSignal(event, mode (0 write, 1 add), value) */
static void nvn_event_signal(Ctx *c) {
    Event *e = obj_get(A(0), T_EVENT);
    if (!e || !e->addr) return;
    ST32(e->addr, (u32)A(1) == 1 ? LD32(e->addr) + (u32)A(2) : (u32)A(2));
}
/* nvnCommandBufferSignalEvent(cmd, event, mode, location, flags, value) / WaitEvent(cmd, event, mode, value) */
static void cmd_signal_event(Ctx *c) {
    Event *e = obj_get(A(1), T_EVENT);
    if (!e) return;
    CmdEvent v = {e->addr, (u32)A(2), (u32)A(5), (u32)A(3), (u32)A(4)};
    EMIT(OP_SIGNAL_EVENT, CmdEvent);
    *p = v;
}
static void cmd_wait_event(Ctx *c) {
    Event *e = obj_get(A(1), T_EVENT);
    if (!e) return;
    CmdEvent v = {e->addr, (u32)A(2), (u32)A(3), 0, 0};
    EMIT(OP_WAIT_EVENT, CmdEvent);
    *p = v;
}
/* nvnCommandBufferReportCounter(cmd, counter, address), ResetCounter(cmd, counter), ReportValue(cmd, value, addr) */
static void cmd_report_counter(Ctx *c) { CmdReport v = {(u32)A(1), 0, A(2)}; EMIT(OP_REPORT_COUNTER, CmdReport); *p = v; }
static void cmd_reset_counter(Ctx *c) { CmdReport v = {(u32)A(1), 0, 0}; EMIT(OP_RESET_COUNTER, CmdReport); *p = v; }
static void cmd_report_value(Ctx *c) { CmdReport v = {0, (u32)A(1), A(2)}; EMIT(OP_REPORT_VALUE, CmdReport); *p = v; }
/* nvnDeviceGetTimestampInNanoseconds(device, const NVNcounterData *): the GPU writes our nanoseconds */
static void device_counter_ns(Ctx *c) { RET(A(1) ? LD64(A(1) + 8) : 0); }

static void cmd_texture_pool(Ctx *c) {
    DescPool *d = descpool(A(1));
    CmdPool v = {d ? d->cpu : 0, d ? d->count : 0};
    EMIT(OP_TEXTURE_POOL, CmdPool);
    *p = v;
}
static void cmd_sampler_pool(Ctx *c) {
    DescPool *d = descpool(A(1));
    CmdPool v = {d ? d->cpu : 0, d ? d->count : 0};
    EMIT(OP_SAMPLER_POOL, CmdPool);
    *p = v;
}
/* nvnCommandBufferCallCommands(cmd, int n, const NVNcommandHandle *) */
static void cmd_call_commands(Ctx *c) {
    u32 n = (u32)A(1);
    u64 list = A(2);
    for (u32 i = 0; i < n; i++) {
        u64 h = LD64(list + 8 * i);
        EMIT_N(OP_CALL, 8);
        memcpy(p, &h, 8);
    }
}
/* CopyCommands: the game may reuse the source's memory at once, so the commands are copied */
static void copy_stream(Ctx *c, CmdBuf *b, u64 at, int depth) {
    for (;;) {
        u32 hdr = LD32(at), op = hdr & 0xff, len = hdr >> 8;
        if (op == OP_END) return;
        if (op == OP_JUMP) { at = LD64(at + 4); continue; }
        if (op == OP_CALL && depth < 8) { copy_stream(c, b, LD64(at + 4), depth + 1); at += 4 + len; continue; }
        u8 *p = cmd_put(c, b, op, len);
        memcpy(p, PTR(at + 4), len);
        at += 4 + len;
    }
}
static void cmd_copy_commands(Ctx *c) {
    CmdBuf *b = cmdbuf(c);
    if (!b) return;
    u32 n = (u32)A(1);
    u64 list = A(2);
    for (u32 i = 0; i < n; i++) copy_stream(c, b, LD64(list + 8 * i), 0);
}
/* transform feedback: Begin / End / Pause / Resume (cmd, NVNbufferAddress) */
static void cmd_xfb(Ctx *c, u32 kind) { u64 a = A(1); EMIT_N(OP_XFB, 12); memcpy(p, &kind, 4); memcpy(p + 4, &a, 8); }
static void cmd_xfb_begin(Ctx *c) { cmd_xfb(c, 0); }
static void cmd_xfb_end(Ctx *c) { cmd_xfb(c, 1); }
static void cmd_xfb_pause(Ctx *c) { cmd_xfb(c, 2); }
static void cmd_xfb_resume(Ctx *c) { cmd_xfb(c, 3); }

/* ---------------------------------------------------------------------------------------------- windows */
typedef struct WinBuilder { u64 device, native, textures; u32 count, interval; } WinBuilder;   /* 0x40 */

static void wb_defaults(Ctx *c) { WinBuilder *b = P(WinBuilder, 0); memset(b, 0, 0x40); b->interval = 1; }
static void wb_device(Ctx *c) { P(WinBuilder, 0)->device = A(1); }
static void wb_native(Ctx *c) { P(WinBuilder, 0)->native = A(1); }
static void wb_textures(Ctx *c) { P(WinBuilder, 0)->count = (u32)A(1); P(WinBuilder, 0)->textures = A(2); }
static void wb_interval(Ctx *c) { P(WinBuilder, 0)->interval = (u32)A(1); }
static void wb_get_native(Ctx *c) { RET(P(WinBuilder, 0)->native); }
static void wb_get_interval(Ctx *c) { RET(P(WinBuilder, 0)->interval); }
static void win_initialize(Ctx *c) {
    WinBuilder *b = P(WinBuilder, 1);
    Window *w = obj_new(A(0), T_WINDOW, sizeof(Window));
    w->native = b->native;
    w->count = b->count > 8 ? 8 : b->count;
    for (u32 i = 0; i < w->count; i++) {
        w->textures[i] = LD64(b->textures + 8 * i);
        Texture *t = obj_get(w->textures[i], T_TEXTURE);
        if (t) w->info[i] = t->info;
    }
    w->interval = b->interval;
    w->crop[2] = w->count ? (s32)w->info[0].width : 1920;
    w->crop[3] = w->count ? (s32)w->info[0].height : 1080;
    hlog(1, "nvn: window with %u textures (%ux%u, format %u), present interval %u", w->count, w->info[0].width,
         w->info[0].height, w->info[0].format, w->interval);
    gpu_window_created(w);
    RET(1);
}
static void win_finalize(Ctx *c) { obj_free(A(0)); }
/* acquire the next texture in turn; it is free once the presents before have read it */
static void win_acquire(Ctx *c) {
    Window *w = obj_get(A(0), T_WINDOW);
    Sync *s = obj_get(A(1), T_SYNC);
    if (s) { s->value = gpu_fence(0); s->fenced = true; }
    if (!w || !w->count) { RET(1); return; }
    if (A(2)) ST32(A(2), w->next);
    w->next = (w->next + 1) % w->count;
    RET(0);
}
static void queue_acquire(Ctx *c) {
    Window *w = obj_get(A(1), T_WINDOW);
    if (!w || !w->count) { RET(1); return; }
    if (A(2)) ST32(A(2), w->next);
    w->next = (w->next + 1) % w->count;
    RET(0);
}
/* present, then keep the pace of the display (60 Hz / interval) */
static void queue_present(Ctx *c) {
    Window *w = obj_get(A(1), T_WINDOW);
    if (!w) return;
    u32 index = (u32)A(2);
    if (index < w->count) gpu_present(A(0), w, index);
    LARGE_INTEGER f, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&now);
    s64 period = f.QuadPart / 60 * (w->interval ? w->interval : 1);
    static int nopace = -1;                     /* MK8_NOPACE=1: as fast as it goes (unattended runs) */
    if (nopace < 0) nopace = getenv("MK8_NOPACE") && atoi(getenv("MK8_NOPACE"));
    if (w->last && !nopace) {
        s64 due = w->last + period - now.QuadPart;
        if (due > 0) thread_sleep(due * 1000000000 / f.QuadPart);
        if (due > -period) now.QuadPart = w->last + period;
        else QueryPerformanceCounter(&now);
    }
    w->last = now.QuadPart;
    static u64 frames;
    if (++frames == 1) hlog(1, "nvn: first frame presented");
    if (frames % 600 == 0) hlog(1, "nvn: %llu frames presented", frames);
}
static void win_get_native(Ctx *c) { Window *w = obj_get(A(0), T_WINDOW); RET(w ? w->native : 0); }
static void win_get_interval(Ctx *c) { Window *w = obj_get(A(0), T_WINDOW); RET(w ? w->interval : 0); }
static void win_set_interval(Ctx *c) { Window *w = obj_get(A(0), T_WINDOW); if (w) w->interval = (u32)A(1); }
static void win_set_crop(Ctx *c) { Window *w = obj_get(A(0), T_WINDOW); if (w) for (int i = 0; i < 4; i++) w->crop[i] = (s32)A(1 + i); }
static void win_get_crop(Ctx *c) { Window *w = obj_get(A(0), T_WINDOW); if (w && A(1)) memcpy(P(void, 1), w->crop, 16); }

/* ---------------------------------------------------------------------------------------------- the table */
static const NvnProc procs[] = {
    {"nvnDeviceBuilderSetDefaults", device_builder_defaults},
    {"nvnDeviceBuilderSetFlags", device_builder_flags},
    {"nvnDeviceInitialize", device_initialize},
    {"nvnDeviceFinalize", device_finalize},
    {"nvnDeviceSetDebugLabel", nothing},
    {"nvnDeviceGetInteger", device_get_integer},
    {"nvnDeviceGetCurrentTimestampInNanoseconds", device_timestamp},
    {"nvnDeviceSetIntermediateShaderCache", nothing},
    {"nvnDeviceGetTextureHandle", device_texture_handle},
    {"nvnDeviceGetTexelFetchHandle", device_texel_fetch_handle},
    {"nvnDeviceGetImageHandle", device_image_handle},
    {"nvnDeviceGetSeparateTextureHandle", device_separate_texture_handle},
    {"nvnDeviceGetSeparateSamplerHandle", device_separate_sampler_handle},
    {"nvnDeviceInstallDebugCallback", nothing},
    {"nvnDeviceGenerateDebugDomainId", return_zero},
    {"nvnDeviceSetWindowOriginMode", device_set_window_origin},
    {"nvnDeviceSetDepthMode", device_set_depth_mode},
    {"nvnDeviceGetWindowOriginMode", device_get_window_origin},
    {"nvnDeviceGetDepthMode", device_get_depth_mode},
    {"nvnDeviceRegisterFastClearColor", return_true},
    {"nvnDeviceRegisterFastClearColori", return_true},
    {"nvnDeviceRegisterFastClearColorui", return_true},
    {"nvnDeviceRegisterFastClearDepth", return_true},
    {"nvnDeviceApplyDeferredFinalizes", nothing},
    {"nvnDeviceFinalizeCommandHandle", nothing},
    {"nvnDeviceIsExternalDebuggerAttached", return_zero},
    {"nvnDeviceWalkDebugDatabase", nothing},
    {"nvnDeviceGetTimestampInNanoseconds", device_counter_ns},

    {"nvnMemoryPoolBuilderSetDefaults", pool_builder_defaults},
    {"nvnMemoryPoolBuilderSetDevice", pool_builder_device},
    {"nvnMemoryPoolBuilderSetStorage", pool_builder_storage},
    {"nvnMemoryPoolBuilderSetFlags", pool_builder_flags},
    {"nvnMemoryPoolBuilderGetMemory", pool_builder_get_storage},
    {"nvnMemoryPoolBuilderGetSize", pool_builder_get_size},
    {"nvnMemoryPoolBuilderGetFlags", pool_builder_get_flags},
    {"nvnMemoryPoolInitialize", pool_initialize},
    {"nvnMemoryPoolFinalize", pool_finalize},
    {"nvnMemoryPoolSetDebugLabel", nothing},
    {"nvnMemoryPoolMap", pool_map},
    {"nvnMemoryPoolFlushMappedRange", nothing},
    {"nvnMemoryPoolInvalidateMappedRange", nothing},
    {"nvnMemoryPoolGetBufferAddress", pool_address},
    {"nvnMemoryPoolGetSize", pool_size},
    {"nvnMemoryPoolGetFlags", pool_flags},
    {"nvnMemoryPoolMapVirtual", pool_map_virtual},

    {"nvnBufferBuilderSetDefaults", buffer_builder_defaults},
    {"nvnBufferBuilderSetDevice", buffer_builder_device},
    {"nvnBufferBuilderSetStorage", buffer_builder_storage},
    {"nvnBufferBuilderGetMemoryPool", buffer_builder_get_pool},
    {"nvnBufferBuilderGetMemoryOffset", buffer_builder_get_offset},
    {"nvnBufferBuilderGetSize", buffer_builder_get_size},
    {"nvnBufferInitialize", buffer_initialize},
    {"nvnBufferFinalize", buffer_finalize},
    {"nvnBufferSetDebugLabel", nothing},
    {"nvnBufferMap", buffer_map},
    {"nvnBufferGetAddress", buffer_address},
    {"nvnBufferGetSize", buffer_size},
    {"nvnBufferGetMemoryPool", buffer_pool},
    {"nvnBufferGetMemoryOffset", buffer_offset},
    {"nvnBufferGetDebugID", next_debug_id},
    {"nvnBufferFlushMappedRange", nothing},
    {"nvnBufferInvalidateMappedRange", nothing},

    {"nvnQueueBuilderSetDefaults", queue_builder_defaults},
    {"nvnQueueBuilderSetDevice", queue_builder_device},
    {"nvnQueueBuilderSetFlags", queue_builder_flags},
    {"nvnQueueBuilderSetCommandMemorySize", queue_builder_command_size},
    {"nvnQueueBuilderSetComputeMemorySize", queue_builder_compute_size},
    {"nvnQueueBuilderSetControlMemorySize", queue_builder_control_size},
    {"nvnQueueBuilderSetCommandFlushThreshold", queue_builder_flush_threshold},
    {"nvnQueueBuilderSetQueuePriority", queue_builder_priority},
    {"nvnQueueBuilderGetQueueMemorySize", queue_builder_memory_size},
    {"nvnQueueBuilderSetQueueMemory", queue_builder_memory},
    {"nvnQueueInitialize", queue_initialize},
    {"nvnQueueFinalize", queue_finalize},
    {"nvnQueueSetDebugLabel", nothing},
    {"nvnQueueSubmitCommands", queue_submit},
    {"nvnQueueFlush", queue_flush},
    {"nvnQueueFinish", queue_finish},
    {"nvnQueueGetError", queue_get_error},
    {"nvnQueueFenceSync", queue_fence_sync},
    {"nvnQueueWaitSync", return_true},
    {"nvnQueueGetTotalCommandMemoryUsed", return_zero},
    {"nvnQueueGetTotalComputeMemoryUsed", return_zero},
    {"nvnQueueGetTotalControlMemoryUsed", return_zero},
    {"nvnQueueResetMemoryUsageCounts", nothing},

    {"nvnSyncInitialize", sync_initialize},
    {"nvnSyncFinalize", sync_finalize},
    {"nvnSyncSetDebugLabel", nothing},
    {"nvnSyncWait", sync_wait},

    {"nvnCommandBufferInitialize", cmdbuf_initialize},
    {"nvnCommandBufferFinalize", cmdbuf_finalize},
    {"nvnCommandBufferSetDebugLabel", nothing},
    {"nvnCommandBufferAddCommandMemory", cmdbuf_add_command_memory},
    {"nvnCommandBufferAddControlMemory", cmdbuf_add_control_memory},
    {"nvnCommandBufferGetCommandMemorySize", cmdbuf_command_size},
    {"nvnCommandBufferGetControlMemorySize", cmdbuf_control_size},
    {"nvnCommandBufferGetCommandMemoryFree", cmdbuf_command_free},
    {"nvnCommandBufferGetControlMemoryFree", cmdbuf_control_free},
    {"nvnCommandBufferGetCommandMemoryUsed", cmdbuf_command_used},
    {"nvnCommandBufferGetControlMemoryUsed", cmdbuf_control_used},
    {"nvnCommandBufferSetMemoryCallback", cmdbuf_set_callback},
    {"nvnCommandBufferSetMemoryCallbackData", cmdbuf_set_callback_data},
    {"nvnCommandBufferGetMemoryCallback", cmdbuf_get_callback},
    {"nvnCommandBufferGetMemoryCallbackData", cmdbuf_get_callback_data},
    {"nvnCommandBufferBeginRecording", cmdbuf_begin},
    {"nvnCommandBufferEndRecording", cmdbuf_end},
    {"nvnCommandBufferIsRecording", cmdbuf_is_recording},
    {"nvnCommandBufferSetCopyRowStride", cmdbuf_set_row_stride},
    {"nvnCommandBufferSetCopyImageStride", cmdbuf_set_image_stride},
    {"nvnCommandBufferGetCopyRowStride", cmdbuf_get_row_stride},
    {"nvnCommandBufferGetCopyImageStride", cmdbuf_get_image_stride},
    {"nvnCommandBufferBindBlendState", cmd_bind_blend},
    {"nvnCommandBufferBindChannelMaskState", cmd_bind_channel_mask},
    {"nvnCommandBufferBindColorState", cmd_bind_color},
    {"nvnCommandBufferBindMultisampleState", cmd_bind_multisample},
    {"nvnCommandBufferBindPolygonState", cmd_bind_polygon},
    {"nvnCommandBufferBindDepthStencilState", cmd_bind_depth_stencil},
    {"nvnCommandBufferBindVertexAttribState", cmd_bind_vertex_attribs},
    {"nvnCommandBufferBindVertexStreamState", cmd_bind_vertex_streams},
    {"nvnCommandBufferBindProgram", cmd_bind_program},
    {"nvnCommandBufferBindVertexBuffer", cmd_bind_vertex_buffer},
    {"nvnCommandBufferBindVertexBuffers", cmd_bind_vertex_buffers},
    {"nvnCommandBufferBindUniformBuffer", cmd_bind_uniform_buffer},
    {"nvnCommandBufferBindUniformBuffers", cmd_bind_uniform_buffers},
    {"nvnCommandBufferBindStorageBuffer", cmd_bind_storage_buffer},
    {"nvnCommandBufferBindStorageBuffers", cmd_bind_storage_buffers},
    {"nvnCommandBufferBindTransformFeedbackBuffer", cmd_bind_xfb_buffer},
    {"nvnCommandBufferBindTransformFeedbackBuffers", cmd_bind_xfb_buffers},
    {"nvnCommandBufferBindTexture", cmd_bind_texture},
    {"nvnCommandBufferBindTextures", cmd_bind_textures},
    {"nvnCommandBufferBindImage", cmd_bind_image},
    {"nvnCommandBufferBindImages", cmd_bind_images},
    {"nvnCommandBufferBindSeparateTexture", cmd_bind_sep_texture},
    {"nvnCommandBufferBindSeparateTextures", cmd_bind_sep_textures},
    {"nvnCommandBufferBindSeparateSampler", cmd_bind_sep_sampler},
    {"nvnCommandBufferBindSeparateSamplers", cmd_bind_sep_samplers},
    {"nvnCommandBufferUpdateUniformBuffer", cmd_update_ubo},
    {"nvnCommandBufferDrawArrays", cmd_draw_arrays},
    {"nvnCommandBufferDrawArraysInstanced", cmd_draw_arrays_instanced},
    {"nvnCommandBufferDrawElements", cmd_draw_elements},
    {"nvnCommandBufferDrawElementsBaseVertex", cmd_draw_elements_base_vertex},
    {"nvnCommandBufferDrawElementsInstanced", cmd_draw_elements_instanced},
    {"nvnCommandBufferDrawArraysIndirect", cmd_draw_arrays_indirect},
    {"nvnCommandBufferDrawElementsIndirect", cmd_draw_elements_indirect},
    {"nvnCommandBufferMultiDrawArraysIndirectCount", cmd_multi_draw_arrays},
    {"nvnCommandBufferMultiDrawElementsIndirectCount", cmd_multi_draw_elements},
    {"nvnCommandBufferDispatchCompute", cmd_dispatch},
    {"nvnCommandBufferDispatchComputeIndirect", cmd_dispatch_indirect},
    {"nvnCommandBufferDrawTexture", cmd_draw_texture},
    {"nvnCommandBufferDrawTransformFeedback", cmd_draw_xfb},
    {"nvnCommandBufferSetViewport", cmd_viewport},
    {"nvnCommandBufferSetViewports", cmd_viewports},
    {"nvnCommandBufferSetScissor", cmd_scissor},
    {"nvnCommandBufferSetScissors", cmd_scissors},
    {"nvnCommandBufferSetDepthRange", cmd_depth_range},
    {"nvnCommandBufferSetDepthRanges", cmd_depth_ranges},
    {"nvnCommandBufferSetDepthBounds", cmd_depth_bounds},
    {"nvnCommandBufferSetStencilValueMask", cmd_stencil_value_mask},
    {"nvnCommandBufferSetStencilMask", cmd_stencil_mask},
    {"nvnCommandBufferSetStencilRef", cmd_stencil_ref},
    {"nvnCommandBufferSetBlendColor", cmd_blend_color},
    {"nvnCommandBufferSetPointSize", cmd_point_size},
    {"nvnCommandBufferSetLineWidth", cmd_line_width},
    {"nvnCommandBufferSetPolygonOffsetClamp", cmd_polygon_offset},
    {"nvnCommandBufferSetAlphaRef", cmd_alpha_ref},
    {"nvnCommandBufferSetSampleMask", cmd_sample_mask},
    {"nvnCommandBufferSetRasterizerDiscard", cmd_rasterizer_discard},
    {"nvnCommandBufferSetDepthClamp", cmd_depth_clamp},
    {"nvnCommandBufferSetPrimitiveRestart", cmd_primitive_restart},
    {"nvnCommandBufferSetPatchSize", cmd_patch_size},
    {"nvnCommandBufferSetInnerTessellationLevels", nothing},
    {"nvnCommandBufferSetOuterTessellationLevels", nothing},
    {"nvnCommandBufferSetRenderEnable", cmd_render_enable},
    {"nvnCommandBufferSetRenderEnableConditional", nothing},
    {"nvnCommandBufferSetRenderTargets", cmd_render_targets},
    {"nvnCommandBufferClearColor", cmd_clear_color},
    {"nvnCommandBufferClearColori", cmd_clear_color_i},
    {"nvnCommandBufferClearColorui", cmd_clear_color_ui},
    {"nvnCommandBufferClearDepthStencil", cmd_clear_depth_stencil},
    {"nvnCommandBufferClearTexture", cmd_clear_texture},
    {"nvnCommandBufferClearTexturei", cmd_clear_texture_i},
    {"nvnCommandBufferClearTextureui", cmd_clear_texture_ui},
    {"nvnCommandBufferDiscardColor", cmd_discard_color},
    {"nvnCommandBufferDiscardDepthStencil", cmd_discard_depth_stencil},
    {"nvnCommandBufferCopyBufferToTexture", cmd_copy_buffer_to_texture},
    {"nvnCommandBufferCopyTextureToBuffer", cmd_copy_texture_to_buffer},
    {"nvnCommandBufferCopyTextureToTexture", cmd_copy_texture_to_texture},
    {"nvnCommandBufferCopyBufferToBuffer", cmd_copy_buffer},
    {"nvnCommandBufferClearBuffer", cmd_clear_buffer},
    {"nvnCommandBufferDownsample", cmd_downsample},
    {"nvnCommandBufferTiledDownsample", cmd_downsample},
    {"nvnCommandBufferDownsampleTextureView", cmd_downsample_view},
    {"nvnCommandBufferTiledDownsampleTextureView", cmd_downsample_view},
    {"nvnCommandBufferBarrier", cmd_barrier},
    {"nvnCommandBufferFenceSync", cmd_fence_sync},
    {"nvnCommandBufferWaitSync", cmd_wait_sync},
    {"nvnCommandBufferSignalEvent", cmd_signal_event},
    {"nvnCommandBufferWaitEvent", cmd_wait_event},
    {"nvnCommandBufferReportCounter", cmd_report_counter},
    {"nvnCommandBufferResetCounter", cmd_reset_counter},
    {"nvnCommandBufferReportValue", cmd_report_value},
    {"nvnCommandBufferSetTexturePool", cmd_texture_pool},
    {"nvnCommandBufferSetSamplerPool", cmd_sampler_pool},
    {"nvnCommandBufferSetShaderScratchMemory", nothing},
    {"nvnCommandBufferCallCommands", cmd_call_commands},
    {"nvnCommandBufferCopyCommands", cmd_copy_commands},
    {"nvnCommandBufferBeginTransformFeedback", cmd_xfb_begin},
    {"nvnCommandBufferEndTransformFeedback", cmd_xfb_end},
    {"nvnCommandBufferPauseTransformFeedback", cmd_xfb_pause},
    {"nvnCommandBufferResumeTransformFeedback", cmd_xfb_resume},
    {"nvnCommandBufferSetViewportSwizzles", nothing},
    {"nvnCommandBufferSetTiledCacheAction", nothing},
    {"nvnCommandBufferSetTiledCacheTileSize", nothing},
    {"nvnCommandBufferBindCoverageModulationTable", nothing},
    {"nvnCommandBufferResolveDepthBuffer", nothing},
    {"nvnCommandBufferSaveZCullData", nothing},
    {"nvnCommandBufferRestoreZCullData", nothing},
    {"nvnCommandBufferSetConservativeRasterEnable", nothing},
    {"nvnCommandBufferSetConservativeRasterDilate", nothing},
    {"nvnCommandBufferSetSubpixelPrecisionBias", nothing},
    {"nvnCommandBufferSetProgramSubroutines", nothing},
    {"nvnCommandBufferInsertDebugMarker", nothing},
    {"nvnCommandBufferInsertDebugMarkerStatic", nothing},
    {"nvnCommandBufferInsertDebugMarkerDynamic", nothing},
    {"nvnCommandBufferPushDebugGroup", nothing},
    {"nvnCommandBufferPushDebugGroupStatic", nothing},
    {"nvnCommandBufferPushDebugGroupDynamic", nothing},
    {"nvnCommandBufferPopDebugGroup", nothing},
    {"nvnCommandBufferPopDebugGroupId", nothing},

    {"nvnBlendStateSetDefaults", blend_defaults},
    {"nvnBlendStateSetBlendTarget", blend_target},
    {"nvnBlendStateSetBlendFunc", blend_func},
    {"nvnBlendStateSetBlendEquation", blend_equation},
    {"nvnBlendStateSetAdvancedMode", blend_advanced_mode},
    {"nvnBlendStateSetAdvancedOverlap", blend_advanced_overlap},
    {"nvnBlendStateSetAdvancedPremultipliedSrc", blend_advanced_premul},
    {"nvnBlendStateSetAdvancedNormalizedDst", blend_advanced_normdst},
    {"nvnBlendStateGetBlendTarget", blend_get_target},
    {"nvnBlendStateGetBlendFunc", blend_get_func},
    {"nvnBlendStateGetBlendEquation", blend_get_equation},
    {"nvnBlendStateGetAdvancedMode", blend_get_advanced_mode},
    {"nvnBlendStateGetAdvancedOverlap", blend_get_advanced_overlap},
    {"nvnBlendStateGetAdvancedPremultipliedSrc", blend_get_advanced_premul},
    {"nvnBlendStateGetAdvancedNormalizedDst", blend_get_advanced_normdst},
    {"nvnColorStateSetDefaults", color_defaults},
    {"nvnColorStateSetBlendEnable", color_blend_enable},
    {"nvnColorStateSetLogicOp", color_logic_op},
    {"nvnColorStateSetAlphaTest", color_alpha_test},
    {"nvnColorStateGetBlendEnable", color_get_blend_enable},
    {"nvnColorStateGetLogicOp", color_get_logic_op},
    {"nvnColorStateGetAlphaTest", color_get_alpha_test},
    {"nvnChannelMaskStateSetDefaults", mask_defaults},
    {"nvnChannelMaskStateSetChannelMask", mask_set},
    {"nvnChannelMaskStateGetChannelMask", mask_get},
    {"nvnDepthStencilStateSetDefaults", ds_defaults},
    {"nvnDepthStencilStateSetDepthTestEnable", ds_depth_test},
    {"nvnDepthStencilStateSetDepthWriteEnable", ds_depth_write},
    {"nvnDepthStencilStateSetDepthFunc", ds_depth_func},
    {"nvnDepthStencilStateSetStencilTestEnable", ds_stencil_test},
    {"nvnDepthStencilStateSetStencilFunc", ds_stencil_func},
    {"nvnDepthStencilStateSetStencilOp", ds_stencil_op},
    {"nvnDepthStencilStateGetDepthTestEnable", ds_get_depth_test},
    {"nvnDepthStencilStateGetDepthWriteEnable", ds_get_depth_write},
    {"nvnDepthStencilStateGetDepthFunc", ds_get_depth_func},
    {"nvnDepthStencilStateGetStencilTestEnable", ds_get_stencil_test},
    {"nvnDepthStencilStateGetStencilFunc", ds_get_stencil_func},
    {"nvnDepthStencilStateGetStencilOp", ds_get_stencil_op},
    {"nvnPolygonStateSetDefaults", poly_defaults},
    {"nvnPolygonStateSetCullFace", poly_cull},
    {"nvnPolygonStateSetFrontFace", poly_front_face},
    {"nvnPolygonStateSetPolygonMode", poly_mode},
    {"nvnPolygonStateSetPolygonOffsetEnables", poly_offset_enables},
    {"nvnPolygonStateGetCullFace", poly_get_cull},
    {"nvnPolygonStateGetFrontFace", poly_get_front_face},
    {"nvnPolygonStateGetPolygonMode", poly_get_mode},
    {"nvnPolygonStateGetPolygonOffsetEnables", poly_get_offset_enables},
    {"nvnMultisampleStateSetDefaults", ms_defaults},
    {"nvnMultisampleStateSetMultisampleEnable", ms_enable},
    {"nvnMultisampleStateSetSamples", ms_samples},
    {"nvnMultisampleStateSetAlphaToCoverageEnable", ms_a2c},
    {"nvnMultisampleStateSetAlphaToCoverageDither", ms_a2c_dither},
    {"nvnMultisampleStateSetRasterSamples", ms_raster_samples},
    {"nvnMultisampleStateSetCoverageModulationMode", ms_cov_mod},
    {"nvnMultisampleStateSetCoverageToColorEnable", ms_cov_to_color},
    {"nvnMultisampleStateSetCoverageToColorOutput", ms_cov_to_color_out},
    {"nvnMultisampleStateSetSampleLocationsEnable", ms_sample_loc},
    {"nvnMultisampleStateSetSampleLocationsGridEnable", ms_sample_loc_grid},
    {"nvnMultisampleStateSetSampleLocations", nothing},
    {"nvnMultisampleStateGetMultisampleEnable", ms_enable_get},
    {"nvnMultisampleStateGetSamples", ms_samples_get},
    {"nvnMultisampleStateGetAlphaToCoverageEnable", ms_a2c_get},
    {"nvnMultisampleStateGetAlphaToCoverageDither", ms_a2c_dither_get},
    {"nvnMultisampleStateGetRasterSamples", ms_raster_samples_get},
    {"nvnMultisampleStateGetCoverageModulationMode", ms_cov_mod_get},
    {"nvnMultisampleStateGetCoverageToColorEnable", ms_cov_to_color_get},
    {"nvnMultisampleStateGetCoverageToColorOutput", ms_cov_to_color_out_get},
    {"nvnMultisampleStateGetSampleLocationsEnable", ms_sample_loc_get},
    {"nvnMultisampleStateGetSampleLocationsGridEnable", ms_sample_loc_grid_get},
    {"nvnMultisampleStateGetSampleLocationsGrid", return_zero},
    {"nvnVertexAttribStateSetDefaults", va_defaults},
    {"nvnVertexAttribStateSetFormat", va_format},
    {"nvnVertexAttribStateSetStreamIndex", va_stream},
    {"nvnVertexAttribStateGetFormat", va_get_format},
    {"nvnVertexAttribStateGetStreamIndex", va_get_stream},
    {"nvnVertexStreamStateSetDefaults", vs_defaults},
    {"nvnVertexStreamStateSetStride", vs_stride},
    {"nvnVertexStreamStateSetDivisor", vs_divisor},
    {"nvnVertexStreamStateGetStride", vs_get_stride},
    {"nvnVertexStreamStateGetDivisor", vs_get_divisor},

    {"nvnTextureBuilderSetDefaults", tb_defaults},
    {"nvnTextureBuilderSetDevice", tb_device},
    {"nvnTextureBuilderSetFlags", tb_flags},
    {"nvnTextureBuilderSetTarget", tb_target},
    {"nvnTextureBuilderSetWidth", tb_width},
    {"nvnTextureBuilderSetHeight", tb_height},
    {"nvnTextureBuilderSetDepth", tb_depth},
    {"nvnTextureBuilderSetSize1D", tb_size1d},
    {"nvnTextureBuilderSetSize2D", tb_size2d},
    {"nvnTextureBuilderSetSize3D", tb_size3d},
    {"nvnTextureBuilderSetLevels", tb_levels},
    {"nvnTextureBuilderSetFormat", tb_format},
    {"nvnTextureBuilderSetSamples", tb_samples},
    {"nvnTextureBuilderSetSwizzle", tb_swizzle},
    {"nvnTextureBuilderSetDepthStencilMode", tb_ds_mode},
    {"nvnTextureBuilderSetStorage", tb_storage},
    {"nvnTextureBuilderSetStride", tb_stride},
    {"nvnTextureBuilderSetPackagedTextureData", tb_packaged_data},
    {"nvnTextureBuilderSetPackagedTextureLayout", tb_packaged_layout},
    {"nvnTextureBuilderSetGLTextureName", tb_gl_name},
    {"nvnTextureBuilderGetStorageSize", tb_storage_size},
    {"nvnTextureBuilderGetStorageAlignment", tb_storage_alignment},
    {"nvnTextureBuilderGetStorageClass", tb_storage_class},
    {"nvnTextureBuilderGetDevice", tb_get_device},
    {"nvnTextureBuilderGetFlags", tb_get_flags},
    {"nvnTextureBuilderGetTarget", tb_get_target},
    {"nvnTextureBuilderGetWidth", tb_get_width},
    {"nvnTextureBuilderGetHeight", tb_get_height},
    {"nvnTextureBuilderGetDepth", tb_get_depth},
    {"nvnTextureBuilderGetLevels", tb_get_levels},
    {"nvnTextureBuilderGetFormat", tb_get_format},
    {"nvnTextureBuilderGetSamples", tb_get_samples},
    {"nvnTextureBuilderGetSwizzle", tb_get_swizzle},
    {"nvnTextureBuilderGetDepthStencilMode", tb_get_ds_mode},
    {"nvnTextureBuilderGetStride", tb_get_stride},
    {"nvnTextureBuilderGetMemoryPool", tb_get_pool},
    {"nvnTextureBuilderGetMemoryOffset", tb_get_pool_offset},
    {"nvnTextureBuilderGetMemoryPoolOffset", tb_get_pool_offset},
    {"nvnTextureBuilderGetPackagedTextureData", tb_get_packaged},
    {"nvnTextureBuilderGetGLTextureName", tb_get_gl_name},
    {"nvnTextureBuilderGetZCullStorageSize", return_zero},
    {"nvnTextureBuilderGetSparseTileLayout", return_zero},
    {"nvnTextureInitialize", tex_initialize},
    {"nvnTextureFinalize", tex_finalize},
    {"nvnTextureSetDebugLabel", nothing},
    {"nvnTextureGetWidth", tex_get_width},
    {"nvnTextureGetHeight", tex_get_height},
    {"nvnTextureGetDepth", tex_get_depth},
    {"nvnTextureGetLevels", tex_get_levels},
    {"nvnTextureGetFormat", tex_get_format},
    {"nvnTextureGetTarget", tex_get_target},
    {"nvnTextureGetFlags", tex_get_flags},
    {"nvnTextureGetSamples", tex_get_samples},
    {"nvnTextureGetStride", tex_get_stride},
    {"nvnTextureGetStorageSize", tex_get_storage_size},
    {"nvnTextureGetStorageClass", tex_get_storage_class},
    {"nvnTextureGetTextureAddress", tex_get_address},
    {"nvnTextureGetMemoryPool", tex_get_pool},
    {"nvnTextureGetMemoryOffset", tex_get_pool_offset},
    {"nvnTextureGetDepthStencilMode", tex_get_ds_mode},
    {"nvnTextureGetSwizzle", tex_get_swizzle},
    {"nvnTextureGetDebugID", next_debug_id},
    {"nvnTextureGetViewOffset", return_zero},
    {"nvnTextureGetZCullStorageSize", return_zero},
    {"nvnTextureGetSparseTileLayout", return_zero},
    {"nvnTextureCompare", tex_compare},
    {"nvnTextureFlushTexels", nothing},
    {"nvnTextureInvalidateTexels", nothing},
    {"nvnTextureWriteTexels", tex_write_texels},
    {"nvnTextureWriteTexelsStrided", tex_write_texels_strided},
    {"nvnTextureReadTexels", tex_read_texels},
    {"nvnTextureReadTexelsStrided", tex_read_texels_strided},
    {"nvnTextureViewSetDefaults", view_defaults},
    {"nvnTextureViewSetLevels", view_levels},
    {"nvnTextureViewSetLayers", view_layers},
    {"nvnTextureViewSetFormat", view_format},
    {"nvnTextureViewSetSwizzle", view_swizzle},
    {"nvnTextureViewSetDepthStencilMode", view_ds_mode},
    {"nvnTextureViewSetTarget", view_target},
    {"nvnTextureViewGetLevels", view_get_levels},
    {"nvnTextureViewGetLayers", view_get_layers},
    {"nvnTextureViewGetFormat", view_get_format},
    {"nvnTextureViewGetTarget", view_get_target},
    {"nvnTextureViewGetDepthStencilMode", view_get_ds_mode},
    {"nvnTextureViewGetSwizzle", view_get_swizzle},
    {"nvnTextureViewCompare", view_compare},

    {"nvnSamplerBuilderSetDefaults", sb_defaults},
    {"nvnSamplerBuilderSetDevice", sb_device},
    {"nvnSamplerBuilderSetMinMagFilter", sb_min_mag},
    {"nvnSamplerBuilderSetWrapMode", sb_wrap},
    {"nvnSamplerBuilderSetLodClamp", sb_lod_clamp},
    {"nvnSamplerBuilderSetLodBias", sb_lod_bias},
    {"nvnSamplerBuilderSetCompare", sb_compare},
    {"nvnSamplerBuilderSetBorderColor", sb_border_f},
    {"nvnSamplerBuilderSetBorderColori", sb_border_i},
    {"nvnSamplerBuilderSetBorderColorui", sb_border_ui},
    {"nvnSamplerBuilderSetMaxAnisotropy", sb_aniso},
    {"nvnSamplerBuilderSetReductionFilter", sb_reduction},
    {"nvnSamplerBuilderSetLodSnap", sb_lod_snap},
    {"nvnSamplerBuilderGetMinMagFilter", s_get_min_mag},
    {"nvnSamplerBuilderGetWrapMode", s_get_wrap},
    {"nvnSamplerBuilderGetLodClamp", s_get_lod_clamp},
    {"nvnSamplerBuilderGetLodBias", s_get_lod_bias},
    {"nvnSamplerBuilderGetCompare", s_get_compare},
    {"nvnSamplerBuilderGetBorderColor", s_get_border},
    {"nvnSamplerBuilderGetBorderColori", s_get_border},
    {"nvnSamplerBuilderGetBorderColorui", s_get_border},
    {"nvnSamplerBuilderGetMaxAnisotropy", s_get_aniso},
    {"nvnSamplerBuilderGetReductionFilter", s_get_reduction},
    {"nvnSamplerBuilderGetLodSnap", s_get_lod_snap},
    {"nvnSamplerInitialize", smp_initialize},
    {"nvnSamplerFinalize", smp_finalize},
    {"nvnSamplerSetDebugLabel", nothing},
    {"nvnSamplerGetMinMagFilter", s_get_min_mag},
    {"nvnSamplerGetWrapMode", s_get_wrap},
    {"nvnSamplerGetLodClamp", s_get_lod_clamp},
    {"nvnSamplerGetLodBias", s_get_lod_bias},
    {"nvnSamplerGetCompare", s_get_compare},
    {"nvnSamplerGetBorderColor", s_get_border},
    {"nvnSamplerGetBorderColori", s_get_border},
    {"nvnSamplerGetBorderColorui", s_get_border},
    {"nvnSamplerGetMaxAnisotropy", s_get_aniso},
    {"nvnSamplerGetReductionFilter", s_get_reduction},
    {"nvnSamplerCompare", smp_compare},
    {"nvnSamplerGetDebugID", next_debug_id},

    {"nvnTexturePoolInitialize", texpool_initialize},
    {"nvnTexturePoolFinalize", descpool_finalize},
    {"nvnTexturePoolSetDebugLabel", nothing},
    {"nvnTexturePoolGetMemoryPool", descpool_pool},
    {"nvnTexturePoolGetMemoryOffset", descpool_offset},
    {"nvnTexturePoolGetSize", descpool_size},
    {"nvnTexturePoolRegisterTexture", texpool_register},
    {"nvnTexturePoolRegisterImage", texpool_register},
    {"nvnSamplerPoolInitialize", smppool_initialize},
    {"nvnSamplerPoolFinalize", descpool_finalize},
    {"nvnSamplerPoolSetDebugLabel", nothing},
    {"nvnSamplerPoolGetMemoryPool", descpool_pool},
    {"nvnSamplerPoolGetMemoryOffset", descpool_offset},
    {"nvnSamplerPoolGetSize", descpool_size},
    {"nvnSamplerPoolRegisterSampler", smppool_register},
    {"nvnSamplerPoolRegisterSamplerBuilder", smppool_register_builder},

    {"nvnProgramInitialize", program_initialize},
    {"nvnProgramFinalize", program_finalize},
    {"nvnProgramSetDebugLabel", nothing},
    {"nvnProgramSetShaders", program_set_shaders},
    {"nvnProgramSetSubroutineLinkage", return_true},

    {"nvnWindowBuilderSetDefaults", wb_defaults},
    {"nvnWindowBuilderSetDevice", wb_device},
    {"nvnWindowBuilderSetNativeWindow", wb_native},
    {"nvnWindowBuilderSetTextures", wb_textures},
    {"nvnWindowBuilderSetPresentInterval", wb_interval},
    {"nvnWindowBuilderGetNativeWindow", wb_get_native},
    {"nvnWindowBuilderGetPresentInterval", wb_get_interval},
    {"nvnWindowInitialize", win_initialize},
    {"nvnWindowFinalize", win_finalize},
    {"nvnWindowSetDebugLabel", nothing},
    {"nvnWindowAcquireTexture", win_acquire},
    {"nvnWindowGetNativeWindow", win_get_native},
    {"nvnWindowGetPresentInterval", win_get_interval},
    {"nvnWindowSetPresentInterval", win_set_interval},
    {"nvnWindowSetCrop", win_set_crop},
    {"nvnWindowGetCrop", win_get_crop},
    {"nvnQueueAcquireTexture", queue_acquire},
    {"nvnQueuePresentTexture", queue_present},

    {"nvnEventBuilderSetDefaults", eb_defaults},
    {"nvnEventBuilderSetStorage", eb_storage},
    {"nvnEventInitialize", event_initialize},
    {"nvnEventFinalize", event_finalize},
    {"nvnEventGetValue", event_get_value},
    {"nvnEventSignal", nvn_event_signal},
};

/* ---------------------------------------------------------------------------------------------- lookup */
static void nvn_get_proc_address(Ctx *c);
static void bootstrap_loader(Ctx *c);

/* names handed out so far, each with its host-call address */
static struct { char name[96]; u64 addr; bool warned; } given[1024];
static int ngiven;
static SRWLOCK lock = SRWLOCK_INIT;

static void unimplemented(Ctx *c) {
    for (int i = 0; i < ngiven; i++)
        if (given[i].addr == c->pc) {
            if (!given[i].warned) {
                char b[200];
                given[i].warned = true;
                hlog(0, "nvn: %s(%llx, %llx, %llx, %llx) not written yet: returns 0 (from %s)", given[i].name, A(0),
                     A(1), A(2), A(3), describe_addr(c->x[30], b, sizeof b));
            }
            break;
        }
    RET(0);
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
    if (!strcmp(name, "nvnDeviceGetProcAddress")) fn = nvn_get_proc_address;
    else if (!strcmp(name, "nvnBootstrapLoader")) fn = bootstrap_loader;
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
    gpu_init();
}
