/* NVN, intercepted (CLAUDE.md, decision 3): nvnBootstrapLoader and nv:: never reach the sdk's driver. Every NVN
 * function the game asks for (nvnBootstrapLoader, nvnDeviceGetProcAddress) gets a host-call address.
 *
 * This is the headless front end: objects, memory, sizes and queries behave as the API says, commands are
 * accepted and dropped (M5 records them for Vulkan). Storage:
 * - builders, texture views and the small state blocks are values the game copies, so their fields live inline
 *   in the game's memory (layouts below, each within the API's size for that type);
 * - objects (device, queue, pools, buffers, textures, ...) keep a magic word and a pointer to host state;
 * - a memory pool's GPU address is the CPU address of the storage the game gave it (identity, like the CPU side).
 * Functions not written yet log once and return 0. */
#include "hos.h"
#include <stdlib.h>
#include <windows.h>

#define A(i) (c->x[i])
#define F32(i) (((v4f32)c->v[i])[0])
#define RET(v) (c->x[0] = (u64)(v))
#define P(T, i) ((T *)(uintptr_t)c->x[i])

typedef struct NvnProc { const char *name; GuestFn fn; } NvnProc;

/* ---------------------------------------------------------------------------------------------- objects */
#define NVN_MAGIC 0x4e564e48u          /* 'HNVN' */
enum { T_DEVICE = 1, T_QUEUE, T_POOL, T_BUFFER, T_TEXTURE, T_SAMPLER, T_PROGRAM, T_CMDBUF, T_WINDOW, T_SYNC,
       T_TEXPOOL, T_SMPPOOL, T_EVENT };
typedef struct ObjHdr { u32 magic, type; void *host; } ObjHdr;

static void *obj_new(u64 addr, u32 type, size_t size) {
    void *h = calloc(1, size);
    ObjHdr *o = (ObjHdr *)(uintptr_t)addr;
    o->magic = NVN_MAGIC;
    o->type = type;
    o->host = h;
    return h;
}

static void *obj_get(u64 addr, u32 type) {
    ObjHdr *o = (ObjHdr *)(uintptr_t)addr;
    if (!addr || o->magic != NVN_MAGIC || o->type != type) return NULL;
    return o->host;
}

static void obj_free(u64 addr) {
    ObjHdr *o = (ObjHdr *)(uintptr_t)addr;
    if (!addr || o->magic != NVN_MAGIC) return;
    free(o->host);
    o->magic = 0;
    o->host = NULL;
}

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

static u64 now_ns(void);
static void device_timestamp(Ctx *c) { RET(now_ns()); }
/* handles: texture id in the low 20 bits, sampler id above, as on the console */
static void device_texture_handle(Ctx *c) { RET((u64)(u32)A(1) | (u64)(u32)A(2) << 20); }
static void device_texel_fetch_handle(Ctx *c) { RET((u64)(u32)A(1)); }
static void device_image_handle(Ctx *c) { RET((u64)(u32)A(1)); }
static void device_set_window_origin(Ctx *c) { Device *d = obj_get(A(0), T_DEVICE); if (d) d->window_origin = (u32)A(1); }
static void device_set_depth_mode(Ctx *c) { Device *d = obj_get(A(0), T_DEVICE); if (d) d->depth_mode = (u32)A(1); }
static void device_get_window_origin(Ctx *c) { Device *d = obj_get(A(0), T_DEVICE); RET(d ? d->window_origin : 0); }
static void device_get_depth_mode(Ctx *c) { Device *d = obj_get(A(0), T_DEVICE); RET(d ? d->depth_mode : 0); }
static void return_true(Ctx *c) { RET(1); }
static void return_zero(Ctx *c) { RET(0); }
static void nothing(Ctx *c) {}

/* ---------------------------------------------------------------------------------------------- memory pools */
typedef struct PoolBuilder { u64 device, storage, size; u32 flags; } PoolBuilder;   /* 0x40 */
typedef struct Pool { u64 storage, size; u32 flags; } Pool;

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
    hlog(2, "nvn: pool %llx: storage %llx+%llx flags %x", A(0), p->storage, p->size, p->flags);
    RET(1);
}
static void pool_finalize(Ctx *c) { obj_free(A(0)); }
static void pool_map(Ctx *c) { Pool *p = obj_get(A(0), T_POOL); RET(p ? p->storage : 0); }
static void pool_address(Ctx *c) { Pool *p = obj_get(A(0), T_POOL); RET(p ? p->storage : 0); }
static void pool_size(Ctx *c) { Pool *p = obj_get(A(0), T_POOL); RET(p ? p->size : 0); }
static void pool_flags(Ctx *c) { Pool *p = obj_get(A(0), T_POOL); RET(p ? p->flags : 0); }

static u64 pool_storage(u64 pool) { Pool *p = obj_get(pool, T_POOL); return p ? p->storage : 0; }

/* ---------------------------------------------------------------------------------------------- buffers */
typedef struct BufferBuilder { u64 device, pool, offset, size; } BufferBuilder;   /* 0x40 */
typedef struct Buffer { u64 pool, offset, size, addr; } Buffer;

static void buffer_builder_defaults(Ctx *c) { memset(P(void, 0), 0, 0x30); }
static void buffer_builder_device(Ctx *c) { P(BufferBuilder, 0)->device = A(1); }
static void buffer_builder_storage(Ctx *c) {
    BufferBuilder *b = P(BufferBuilder, 0);
    b->pool = A(1); b->offset = A(2); b->size = A(3);
}
static void buffer_initialize(Ctx *c) {
    BufferBuilder *b = P(BufferBuilder, 1);
    Buffer *x = obj_new(A(0), T_BUFFER, sizeof(Buffer));
    x->pool = b->pool; x->offset = b->offset; x->size = b->size;
    x->addr = pool_storage(b->pool) + b->offset;
    RET(1);
}
static void buffer_finalize(Ctx *c) { obj_free(A(0)); }
static void buffer_map(Ctx *c) { Buffer *b = obj_get(A(0), T_BUFFER); RET(b ? b->addr : 0); }
static void buffer_address(Ctx *c) { Buffer *b = obj_get(A(0), T_BUFFER); RET(b ? b->addr : 0); }
static void buffer_size(Ctx *c) { Buffer *b = obj_get(A(0), T_BUFFER); RET(b ? b->size : 0); }

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
static void queue_finalize(Ctx *c) { obj_free(A(0)); }
static void queue_get_error(Ctx *c) { if (A(1)) memset(P(void, 1), 0, 0x18); RET(0); }

/* ---------------------------------------------------------------------------------------------- sync */
typedef struct Sync { bool signaled; } Sync;
static void sync_initialize(Ctx *c) { obj_new(A(0), T_SYNC, sizeof(Sync)); RET(1); }
static void sync_finalize(Ctx *c) { obj_free(A(0)); }
/* headless: work completes at once */
static void queue_fence_sync(Ctx *c) { Sync *s = obj_get(A(1), T_SYNC); if (s) s->signaled = true; }
static void sync_wait(Ctx *c) { RET(0); }                                     /* ALREADY_SIGNALED */

/* ---------------------------------------------------------------------------------------------- command buffers */
typedef struct CmdBuf { u64 handles; u64 cmd_size, ctrl_size; } CmdBuf;
static void cmdbuf_initialize(Ctx *c) { obj_new(A(0), T_CMDBUF, sizeof(CmdBuf)); RET(1); }
static void cmdbuf_finalize(Ctx *c) { obj_free(A(0)); }
static void cmdbuf_add_command_memory(Ctx *c) { CmdBuf *b = obj_get(A(0), T_CMDBUF); if (b) b->cmd_size += A(3); }
static void cmdbuf_add_control_memory(Ctx *c) { CmdBuf *b = obj_get(A(0), T_CMDBUF); if (b) b->ctrl_size += A(2); }
static void cmdbuf_command_size(Ctx *c) { CmdBuf *b = obj_get(A(0), T_CMDBUF); RET(b ? b->cmd_size : 0); }
static void cmdbuf_control_size(Ctx *c) { CmdBuf *b = obj_get(A(0), T_CMDBUF); RET(b ? b->ctrl_size : 0); }
static void cmdbuf_end_recording(Ctx *c) { CmdBuf *b = obj_get(A(0), T_CMDBUF); RET(b ? ++b->handles | (u64)1 << 48 : 1); }

/* ---------------------------------------------------------------------------------------------- formats */
/* bytes per block and block size of each NVNformat (0 = unknown) */
static void format_info(u32 f, u32 *bpb, u32 *bw, u32 *bh) {
    *bw = *bh = 1;
    static const u8 plain[] = {
        0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 4, 4, 4, 2, 2, 2, 2, 4, 4, 4, 4, 4, 8, 8, 8,   /* NONE .. RG32I */
        3, 3, 3, 3, 6, 6, 6, 6, 6, 12, 12, 12,                                      /* RGB8 .. RGB32I */
        4, 4, 4, 4, 8, 8, 8, 8, 8, 16, 16, 16,                                      /* RGBA8 .. RGBA32I */
        1, 2, 4, 4, 4, 8,                                                           /* STENCIL8 .. DEPTH32F_STENCIL8 */
        4, 4, 2, 2, 2, 2, 4, 4, 4, 4,                                               /* RGBX8_SRGB .. RGB9E5F */
    };
    if (f < sizeof plain) { *bpb = plain[f]; return; }
    if (f >= 65 && f <= 80) {                                                      /* DXT, RGTC, BPTC */
        *bw = *bh = 4;
        *bpb = (f == 65 || f == 66 || f == 69 || f == 70 || f == 73 || f == 74) ? 8 : 16;
        return;
    }
    if (f >= 81 && f <= 104) {                                                     /* vertex formats */
        static const u8 v[] = {1, 1, 2, 2, 4, 4, 2, 2, 4, 4, 8, 8, 3, 3, 6, 6, 12, 12, 4, 4, 8, 8, 16, 16};
        *bpb = v[f - 81];
        return;
    }
    if (f >= 105 && f <= 108) { *bpb = 4; return; }                               /* RGB10A2 variants */
    if (f >= 109 && f <= 120) {                                                    /* RGBX */
        static const u8 x[] = {4, 4, 4, 4, 8, 8, 8, 8, 8, 16, 16, 16};
        *bpb = x[f - 109];
        return;
    }
    if (f >= 121 && f <= 148) {                                                    /* ASTC, then ASTC sRGB */
        static const u8 dims[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8},
                                       {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
        u32 k = (f - 121) % 14;
        *bw = dims[k][0]; *bh = dims[k][1]; *bpb = 16;
        return;
    }
    if (f >= 149 && f <= 152) { *bpb = 2; return; }                               /* BGR5 .. BGR565 */
    if (f >= 153 && f <= 156) { *bpb = 4; return; }                               /* BGRA8 .. BGRX8_SRGB */
    *bpb = 0;
}

/* ---------------------------------------------------------------------------------------------- textures */
typedef struct TexBuilder {                                                     /* 0x80 */
    u64 device;
    u32 flags, target, width, height, depth, levels, format, samples;
    u8 swizzle[4];
    u32 ds_mode;
    u64 pool, offset;
    s64 stride;
    u64 packaged;
    u32 gl_name, pad;
} TexBuilder;
typedef struct Texture { TexBuilder b; u64 addr, size; } Texture;

enum { TEX_1D, TEX_2D, TEX_3D, TEX_1D_ARRAY, TEX_2D_ARRAY, TEX_2D_MS, TEX_2D_MS_ARRAY, TEX_RECT, TEX_CUBE,
       TEX_CUBE_ARRAY, TEX_BUFFER };
#define TEX_FLAG_LINEAR 0x10                                                    /* NVN_TEXTURE_FLAGS_LINEAR */

static u64 div_up(u64 a, u64 b) { return (a + b - 1) / b; }
static u64 align_up(u64 a, u64 b) { return div_up(a, b) * b; }

/* storage of a block-linear texture as the Tegra X1 lays it out: GOBs of 64 bytes x 8 rows, stacked into blocks
 * of up to 16 GOBs high (32 deep for 3D), the block shrinking with the mip level */
static u64 texture_storage_size(const TexBuilder *b) {
    u32 bpb, bw, bh;
    format_info(b->format, &bpb, &bw, &bh);
    if (!bpb) {
        hlog(0, "nvn: unknown texture format %u: assuming 16 bytes per pixel", b->format);
        bpb = 16;
    }
    u32 w = b->width ? b->width : 1, h = b->height ? b->height : 1, d = b->depth ? b->depth : 1;
    u32 layers = 1, depth = 1;
    switch (b->target) {
    case TEX_1D: h = 1; break;
    case TEX_3D: depth = d; break;
    case TEX_1D_ARRAY: layers = h; h = 1; break;
    case TEX_2D_ARRAY: case TEX_2D_MS_ARRAY: layers = d; break;
    case TEX_CUBE: layers = 6; break;
    case TEX_CUBE_ARRAY: layers = d * 6; break;
    }
    u32 samples = b->samples > 1 ? b->samples : 1;
    if (b->flags & TEX_FLAG_LINEAR || b->target == TEX_BUFFER) {
        u64 stride = b->stride ? (u64)b->stride : align_up(div_up(w, bw) * bpb, 32);
        return align_up(stride * div_up(h, bh) * depth * layers * samples, 0x200);
    }
    u32 levels = b->levels ? b->levels : 1;
    u64 wb0 = div_up(w, bw), hb0 = div_up(h, bh);
    u32 gob_h = 1, gob_d = 1;                                    /* block height / depth in GOBs */
    while (gob_h < 16 && hb0 > 8ull * gob_h) gob_h <<= 1;
    while (gob_d < 32 && depth > gob_d) gob_d <<= 1;
    u64 layer = 0;
    for (u32 l = 0; l < levels; l++) {
        u64 wb = wb0 >> l ? wb0 >> l : 1, hb = hb0 >> l ? hb0 >> l : 1, db = depth >> l ? depth >> l : 1;
        u32 lh = gob_h, ld = gob_d;
        while (lh > 1 && hb <= 8ull * (lh / 2)) lh >>= 1;
        while (ld > 1 && db <= ld / 2) ld >>= 1;
        layer += align_up(wb * bpb * samples, 64) * align_up(hb, 8ull * lh) * align_up(db, ld);
    }
    if (layers > 1) layer = align_up(layer, 512ull * gob_h * gob_d);
    return align_up(layer * layers, 0x200);
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
static void tb_gl_name(Ctx *c) { P(TexBuilder, 0)->gl_name = (u32)A(1); }
static void tb_storage_size(Ctx *c) { RET(texture_storage_size(P(TexBuilder, 0))); }
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
static void tb_get_swizzle(Ctx *c) {
    TexBuilder *b = P(TexBuilder, 0);
    for (int i = 0; i < 4; i++) if (A(1 + i)) ST32(A(1 + i), b->swizzle[i]);
}

static void tex_initialize(Ctx *c) {
    TexBuilder *b = P(TexBuilder, 1);
    Texture *t = obj_new(A(0), T_TEXTURE, sizeof(Texture));
    t->b = *b;
    t->addr = pool_storage(b->pool) + b->offset;
    t->size = texture_storage_size(b);
    hlog(2, "nvn: texture %llx: %ux%ux%u fmt %u target %u levels %u at %llx+%llx", A(0), b->width, b->height,
         b->depth, b->format, b->target, b->levels, t->addr, t->size);
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
TEX_GET(tex_get_address, t->addr)
TEX_GET(tex_get_pool, t->b.pool)
TEX_GET(tex_get_pool_offset, t->b.offset)
TEX_GET(tex_get_ds_mode, t->b.ds_mode)
static void tex_get_swizzle(Ctx *c) {
    Texture *t = obj_get(A(0), T_TEXTURE);
    for (int i = 0; i < 4; i++) if (A(1 + i)) ST32(A(1 + i), t ? t->b.swizzle[i] : 0);
}

typedef struct TexView {                                                        /* 0x28 */
    u32 flags, target, format;
    u16 base_level, num_levels;
    u32 min_layer, num_layers;
    u8 swizzle[4];
    u32 ds_mode;
} TexView;
enum { VIEW_LEVELS = 1, VIEW_LAYERS = 2, VIEW_FORMAT = 4, VIEW_SWIZZLE = 8, VIEW_DS_MODE = 16, VIEW_TARGET = 32 };
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
static void view_compare(Ctx *c) { RET(memcmp(P(void, 0), P(void, 1), sizeof(TexView)) == 0); }

/* ---------------------------------------------------------------------------------------------- samplers */
typedef struct SmpBuilder { u64 device; u8 state[0x58]; } SmpBuilder;           /* 0x60: the state is kept raw */
typedef struct Sampler { u8 state[0x58]; } Sampler;
static void sb_defaults(Ctx *c) { SmpBuilder *b = P(SmpBuilder, 0); memset(b->state, 0, sizeof b->state); }
static void sb_device(Ctx *c) { P(SmpBuilder, 0)->device = A(1); }
static void smp_initialize(Ctx *c) {
    Sampler *s = obj_new(A(0), T_SAMPLER, sizeof(Sampler));
    memcpy(s->state, P(SmpBuilder, 1)->state, sizeof s->state);
    RET(1);
}
static void smp_finalize(Ctx *c) { obj_free(A(0)); }

/* ---------------------------------------------------------------------------------------------- descriptor pools */
typedef struct DescPool { u64 pool, offset; u32 count; } DescPool;
static void texpool_initialize(Ctx *c) {
    DescPool *p = obj_new(A(0), T_TEXPOOL, sizeof(DescPool));
    p->pool = A(1); p->offset = A(2); p->count = (u32)A(3);
    RET(1);
}
static void smppool_initialize(Ctx *c) {
    DescPool *p = obj_new(A(0), T_SMPPOOL, sizeof(DescPool));
    p->pool = A(1); p->offset = A(2); p->count = (u32)A(3);
    RET(1);
}
static void descpool_finalize(Ctx *c) { obj_free(A(0)); }
static DescPool *descpool(u64 a) {
    ObjHdr *o = (ObjHdr *)(uintptr_t)a;
    return a && o->magic == NVN_MAGIC && (o->type == T_TEXPOOL || o->type == T_SMPPOOL) ? o->host : NULL;
}
static void descpool_pool(Ctx *c) { DescPool *p = descpool(A(0)); RET(p ? p->pool : 0); }
static void descpool_offset(Ctx *c) { DescPool *p = descpool(A(0)); RET(p ? p->offset : 0); }
static void descpool_size(Ctx *c) { DescPool *p = descpool(A(0)); RET(p ? p->count : 0); }

/* ---------------------------------------------------------------------------------------------- programs */
static void program_initialize(Ctx *c) { obj_new(A(0), T_PROGRAM, 8); RET(1); }
static void program_finalize(Ctx *c) { obj_free(A(0)); }

/* ---------------------------------------------------------------------------------------------- windows */
typedef struct WinBuilder { u64 device, native, textures; u32 count, interval; } WinBuilder;   /* 0x40 */
typedef struct Window { u64 native, textures[8]; u32 count, interval, next; s32 crop[4]; LARGE_INTEGER last; } Window;

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
    for (u32 i = 0; i < w->count; i++) w->textures[i] = LD64(b->textures + 8 * i);
    w->interval = b->interval;
    Texture *t = w->count ? obj_get(w->textures[0], T_TEXTURE) : NULL;
    w->crop[2] = t ? (s32)t->b.width : 1920;
    w->crop[3] = t ? (s32)t->b.height : 1080;
    hlog(1, "nvn: window with %u textures, present interval %u", w->count, w->interval);
    RET(1);
}
static void win_finalize(Ctx *c) { obj_free(A(0)); }
/* acquire the next texture in turn; the sync is signalled at once */
static void win_acquire(Ctx *c) {
    Window *w = obj_get(A(0), T_WINDOW);
    Sync *s = obj_get(A(1), T_SYNC);
    if (s) s->signaled = true;
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
/* present: headless, so only keep the pace of the display (60 Hz / interval) */
static void queue_present(Ctx *c) {
    Window *w = obj_get(A(1), T_WINDOW);
    if (!w) return;
    LARGE_INTEGER f, now;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&now);
    s64 period = f.QuadPart / 60 * (w->interval ? w->interval : 1);
    if (w->last.QuadPart) {
        s64 due = w->last.QuadPart + period - now.QuadPart;
        if (due > 0) thread_sleep(due * 1000000000 / f.QuadPart);
        if (due > -period) now.QuadPart = w->last.QuadPart + period;
        else QueryPerformanceCounter(&now);
    }
    w->last = now;
    static u64 frames;
    if (++frames == 1) hlog(1, "nvn: first frame presented");
    if (frames % 600 == 0) hlog(1, "nvn: %llu frames presented", frames);
}
static void win_get_native(Ctx *c) { Window *w = obj_get(A(0), T_WINDOW); RET(w ? w->native : 0); }
static void win_get_interval(Ctx *c) { Window *w = obj_get(A(0), T_WINDOW); RET(w ? w->interval : 0); }
static void win_set_interval(Ctx *c) { Window *w = obj_get(A(0), T_WINDOW); if (w) w->interval = (u32)A(1); }
static void win_set_crop(Ctx *c) { Window *w = obj_get(A(0), T_WINDOW); if (w) for (int i = 0; i < 4; i++) w->crop[i] = (s32)A(1 + i); }
static void win_get_crop(Ctx *c) { Window *w = obj_get(A(0), T_WINDOW); if (w && A(1)) memcpy(P(void, 1), w->crop, 16); }

/* ---------------------------------------------------------------------------------------------- events */
typedef struct EventBuilder { u64 pool, offset; } EventBuilder;
typedef struct Event { u64 addr; } Event;
static void eb_defaults(Ctx *c) { memset(P(void, 0), 0, sizeof(EventBuilder)); }
static void eb_storage(Ctx *c) { P(EventBuilder, 0)->pool = A(1); P(EventBuilder, 0)->offset = A(2); }
static void event_initialize(Ctx *c) {
    EventBuilder *b = P(EventBuilder, 1);
    Event *e = obj_new(A(0), T_EVENT, sizeof(Event));
    e->addr = pool_storage(b->pool) + b->offset;
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
/* nvnCommandBufferSignalEvent(cmd, event, mode, location, flags, value): done at once, headless */
static void cmdbuf_signal_event(Ctx *c) {
    Event *e = obj_get(A(1), T_EVENT);
    if (!e || !e->addr) return;
    ST32(e->addr, (u32)A(2) == 1 ? LD32(e->addr) + (u32)A(5) : (u32)A(5));
}

/* ---------------------------------------------------------------------------------------------- GPU-written results */
/* the work is done at once, headless: what the GPU would write appears when the command is recorded */
static u64 now_ns(void) {
    LARGE_INTEGER f, n;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&n);
    return (u64)((double)n.QuadPart * 1e9 / (double)f.QuadPart);
}
/* nvnCommandBufferReportCounter(cmd, counter, address): NVNcounterData {u64 value, u64 timestamp} */
static void cmdbuf_report_counter(Ctx *c) {
    if (!A(2)) return;
    ST64(A(2), 0);
    ST64(A(2) + 8, now_ns());
}
/* nvnCommandBufferReportValue(cmd, u32 value, address) */
static void cmdbuf_report_value(Ctx *c) { if (A(2)) ST32(A(2), (u32)A(1)); }
/* nvnDeviceGetTimestampInNanoseconds(device, const NVNcounterData *) */
static void device_counter_ns(Ctx *c) { RET(A(1) ? LD64(A(1) + 8) : 0); }

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

    {"nvnBufferBuilderSetDefaults", buffer_builder_defaults},
    {"nvnBufferBuilderSetDevice", buffer_builder_device},
    {"nvnBufferBuilderSetStorage", buffer_builder_storage},
    {"nvnBufferInitialize", buffer_initialize},
    {"nvnBufferFinalize", buffer_finalize},
    {"nvnBufferSetDebugLabel", nothing},
    {"nvnBufferMap", buffer_map},
    {"nvnBufferGetAddress", buffer_address},
    {"nvnBufferGetSize", buffer_size},
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
    {"nvnQueueSubmitCommands", nothing},
    {"nvnQueueFlush", nothing},
    {"nvnQueueFinish", nothing},
    {"nvnQueueGetError", queue_get_error},
    {"nvnQueueFenceSync", queue_fence_sync},
    {"nvnQueueWaitSync", return_true},

    {"nvnSyncInitialize", sync_initialize},
    {"nvnSyncFinalize", sync_finalize},
    {"nvnSyncSetDebugLabel", nothing},
    {"nvnSyncWait", sync_wait},

    {"nvnCommandBufferInitialize", cmdbuf_initialize},
    {"nvnCommandBufferFinalize", cmdbuf_finalize},
    {"nvnCommandBufferAddCommandMemory", cmdbuf_add_command_memory},
    {"nvnCommandBufferAddControlMemory", cmdbuf_add_control_memory},
    {"nvnCommandBufferGetCommandMemorySize", cmdbuf_command_size},
    {"nvnCommandBufferGetControlMemorySize", cmdbuf_control_size},
    {"nvnCommandBufferGetCommandMemoryFree", cmdbuf_command_size},
    {"nvnCommandBufferGetControlMemoryFree", cmdbuf_control_size},
    {"nvnCommandBufferGetCommandMemoryUsed", return_zero},
    {"nvnCommandBufferGetControlMemoryUsed", return_zero},
    {"nvnCommandBufferEndRecording", cmdbuf_end_recording},
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
    {"nvnTextureBuilderSetGLTextureName", tb_gl_name},
    {"nvnTextureBuilderGetStorageSize", tb_storage_size},
    {"nvnTextureBuilderGetStorageAlignment", tb_storage_alignment},
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
    {"nvnTextureBuilderGetMemoryPoolOffset", tb_get_pool_offset},
    {"nvnTextureBuilderGetPackagedTextureData", tb_get_packaged},
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
    {"nvnTextureGetTextureAddress", tex_get_address},
    {"nvnTextureGetMemoryPool", tex_get_pool},
    {"nvnTextureGetMemoryOffset", tex_get_pool_offset},
    {"nvnTextureGetDepthStencilMode", tex_get_ds_mode},
    {"nvnTextureGetSwizzle", tex_get_swizzle},
    {"nvnTextureFlushTexels", nothing},
    {"nvnTextureInvalidateTexels", nothing},
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
    {"nvnTextureViewCompare", view_compare},

    {"nvnSamplerBuilderSetDefaults", sb_defaults},
    {"nvnSamplerBuilderSetDevice", sb_device},
    {"nvnSamplerInitialize", smp_initialize},
    {"nvnSamplerFinalize", smp_finalize},
    {"nvnSamplerSetDebugLabel", nothing},

    {"nvnTexturePoolInitialize", texpool_initialize},
    {"nvnTexturePoolFinalize", descpool_finalize},
    {"nvnTexturePoolSetDebugLabel", nothing},
    {"nvnTexturePoolGetMemoryPool", descpool_pool},
    {"nvnTexturePoolGetMemoryOffset", descpool_offset},
    {"nvnTexturePoolGetSize", descpool_size},
    {"nvnTexturePoolRegisterTexture", nothing},
    {"nvnTexturePoolRegisterImage", nothing},
    {"nvnSamplerPoolInitialize", smppool_initialize},
    {"nvnSamplerPoolFinalize", descpool_finalize},
    {"nvnSamplerPoolSetDebugLabel", nothing},
    {"nvnSamplerPoolGetMemoryPool", descpool_pool},
    {"nvnSamplerPoolGetMemoryOffset", descpool_offset},
    {"nvnSamplerPoolGetSize", descpool_size},
    {"nvnSamplerPoolRegisterSampler", nothing},
    {"nvnSamplerPoolRegisterSamplerBuilder", nothing},

    {"nvnProgramInitialize", program_initialize},
    {"nvnProgramFinalize", program_finalize},
    {"nvnProgramSetDebugLabel", nothing},
    {"nvnProgramSetShaders", return_true},

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
    {"nvnCommandBufferSignalEvent", cmdbuf_signal_event},
    {"nvnCommandBufferSetMemoryCallback", nothing},
    {"nvnCommandBufferSetMemoryCallbackData", nothing},
    {"nvnCommandBufferBeginRecording", nothing},
    {"nvnCommandBufferReportCounter", cmdbuf_report_counter},
    {"nvnCommandBufferReportValue", cmdbuf_report_value},
    {"nvnDeviceGetTimestampInNanoseconds", device_counter_ns},
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
}
