/* Our NVN: objects shared by the API front end (nvn.c), which records commands, and the executor (gpu.c), which
 * replays them on Vulkan at nvnQueueSubmitCommands.
 *
 * Command buffers are recorded into the memory the game gives them (nvnCommandBufferAddCommandMemory), in our own
 * encoding: a u32 header (opcode | payload bytes << 8) and the payload. A command handle is the guest address of
 * its first command. Nothing points at host objects that the game can finalise: textures are recorded by value
 * (TexInfo) and texture / sampler pool entries name interned descriptors. Replay happens inside the submit, so
 * the game may reuse command memory as soon as nvnQueueSubmitCommands returns (it waits longer on the console). */
#ifndef NVN_H
#define NVN_H
#include "hos.h"

#define NVN_MAGIC 0x4e564e48u          /* 'HNVN': first word of every object we initialise */
enum { T_DEVICE = 1, T_QUEUE, T_POOL, T_BUFFER, T_TEXTURE, T_SAMPLER, T_PROGRAM, T_CMDBUF, T_WINDOW, T_SYNC,
       T_TEXPOOL, T_SMPPOOL, T_EVENT };
typedef struct ObjHdr { u32 magic, type; void *host; } ObjHdr;
void *obj_get(u64 addr, u32 type);

/* ---------------------------------------------------------------------------------------------- memory pools */
enum { POOL_CPU_NO_ACCESS = 1, POOL_CPU_UNCACHED = 2, POOL_CPU_CACHED = 4, POOL_GPU_NO_ACCESS = 8,
       POOL_GPU_UNCACHED = 0x10, POOL_GPU_CACHED = 0x20, POOL_SHADER_CODE = 0x40, POOL_COMPRESSIBLE = 0x80,
       POOL_PHYSICAL = 0x100, POOL_VIRTUAL = 0x200 };
typedef struct Pool {
    u64 storage, size, gpu;             /* gpu = storage, or for a virtual pool an address range of its own */
    u32 flags;
    void *hb;                           /* gpu.c: the imported buffer (HostBuffer), if the GPU can access it */
} Pool;
/* GPU address -> CPU pointer of guest memory (identity outside virtual pools) */
u64 pool_storage(u64 pool);
u64 pool_gpu(u64 pool);

/* ---------------------------------------------------------------------------------------------- textures */
enum { TEX_1D, TEX_2D, TEX_3D, TEX_1D_ARRAY, TEX_2D_ARRAY, TEX_2D_MS, TEX_2D_MS_ARRAY, TEX_RECT, TEX_CUBE,
       TEX_CUBE_ARRAY, TEX_BUFFER };
#define TEX_FLAG_DISPLAY 0x1
#define TEX_FLAG_VIDEO 0x2
#define TEX_FLAG_IMAGE 0x4
#define TEX_FLAG_COMPRESSIBLE 0x8
#define TEX_FLAG_LINEAR 0x10
#define TEX_FLAG_SPARSE 0x20
#define TEX_FLAG_LINEAR_RENDER_TARGET 0x40

/* what a texture is: storage and layout. The key of the image cache (gpu.c): game objects come and go (some are
 * re-created every frame), images stay as long as their storage is used the same way. */
typedef struct TexInfo {
    u64 addr;                           /* GPU address of the storage */
    u32 format, target, width, height, depth, levels, samples, flags;
    s32 stride;                         /* linear textures */
    u32 block_height;                   /* block-linear: log2 of the block height in GOBs (0xff: derived) */
} TexInfo;

/* a view: the texture's own values where the view does not say otherwise */
typedef struct ViewInfo {
    u32 format, target;
    u16 base_level, num_levels;
    u16 min_layer, num_layers;
    u8 swizzle[4];
    u32 ds_mode;
} ViewInfo;

/* NVNtextureView as the game holds it (0x28 bytes) */
typedef struct TexView {
    u32 flags, target, format;
    u16 base_level, num_levels;
    u32 min_layer, num_layers;
    u8 swizzle[4];
    u32 ds_mode;
} TexView;
enum { VIEW_LEVELS = 1, VIEW_LAYERS = 2, VIEW_FORMAT = 4, VIEW_SWIZZLE = 8, VIEW_DS_MODE = 16, VIEW_TARGET = 32 };

/* NVNtextureBuilder (0x80 bytes) */
typedef struct TexBuilder {
    u64 device;
    u32 flags, target, width, height, depth, levels, format, samples;
    u8 swizzle[4];
    u32 ds_mode;
    u64 pool, offset;
    s64 stride;
    u64 packaged;
    u32 gl_name, has_layout;
    u8 layout[8];                       /* NVNpackagedTextureLayout */
} TexBuilder;
typedef struct Texture { TexBuilder b; TexInfo info; u64 size; } Texture;

void view_resolve(const TexInfo *t, const TexView *v, ViewInfo *out);

/* interned descriptors: texture pool entries hold {TDESC_MAGIC, index} */
#define TDESC_MAGIC 0x43534454u         /* 'TDSC' */
#define SDESC_MAGIC 0x43534453u         /* 'SDSC' */
typedef struct TexDesc { TexInfo t; ViewInfo v; } TexDesc;

/* sampler state (NVNsamplerBuilder keeps it after the device pointer) */
typedef struct SamplerState {
    u8 min_filter, mag_filter, wrap[3], compare_mode, compare_func, reduction;
    u8 border_type, lod_snap, pad[2];
    float lod_min, lod_max, lod_bias, max_aniso;
    u32 border[4];
} SamplerState;
u32 tdesc_intern(const TexDesc *d);
u32 sdesc_intern(const SamplerState *s);
const TexDesc *tdesc_get(u32 i);
const SamplerState *sdesc_get(u32 i);

/* ---------------------------------------------------------------------------------------------- state blocks
 * (values the game copies; each layout fits the size the API gives the type) */
typedef struct BlendState {             /* 8 bytes */
    u8 target, src_rgb, dst_rgb, src_a, dst_a;
    u8 eq;                              /* rgb | alpha << 4 */
    u8 advanced, adv_flags;
} BlendState;
typedef struct ColorState { u8 blend_enable, logic_op, alpha_test, pad; } ColorState;          /* 4 bytes */
typedef struct ChannelMaskState { u32 mask; } ChannelMaskState;  /* 4 bits per target: r g b a */
typedef struct DepthStencilState {      /* 8 bytes; per-face fields are front | back << 4 */
    u8 flags;                           /* 1 depth test, 2 depth write, 4 stencil test */
    u8 depth_func, stencil_func, fail, zfail, zpass, pad[2];
} DepthStencilState;
typedef struct PolygonState { u8 cull, front_face, mode, offset_enables; } PolygonState;      /* 4 bytes */
typedef struct MultisampleState {       /* 0x18 bytes */
    u8 enable, a2c, a2c_dither, samples, raster_samples, cov_mod, cov_to_color, cov_to_color_out;
    u8 sample_loc, sample_loc_grid, pad[14];
} MultisampleState;
typedef struct VertexAttribState { u32 v; } VertexAttribState;  /* format:10 offset:14 stream:6 (4 bytes) */
#define VA_FORMAT(a) ((a).v & 0x3ff)
#define VA_OFFSET(a) ((a).v >> 10 & 0x3fff)
#define VA_STREAM(a) ((a).v >> 24 & 0x3f)
typedef struct VertexStreamState { u32 stride, divisor; } VertexStreamState;                   /* 8 bytes */

/* ---------------------------------------------------------------------------------------------- programs */
enum { ST_VERTEX, ST_FRAGMENT, ST_GEOMETRY, ST_TESS_CONTROL, ST_TESS_EVAL, ST_COMPUTE, ST_COUNT };
/* one stage of a program: the code the game gave (header 0x30, SPH, instructions, embedded constants) */
typedef struct ShaderCode {
    u64 hash;                           /* of the whole code: names the translated shader */
    u32 stage;                          /* ST_* */
    u64 code, size;                     /* guest address and size of the code */
    u64 const_addr;                     /* embedded constants (GPU address) and their size */
    u32 const_size;
    void *module;                       /* gpu.c: the translated shader, or NULL */
    u32 caps;                           /* gpu.c: SHC_* found in the translation */
    bool missing;
} ShaderCode;
/* the shaders a program has now: interned, so recorded commands stay valid when the program changes */
enum { SHC_GLOBAL = 1, SHC_TEXTURE = 2 };
typedef struct Shaders { ShaderCode *stage[ST_COUNT]; u32 mask; u64 key; } Shaders;

/* ---------------------------------------------------------------------------------------------- windows, syncs */
typedef struct Window {
    u64 native, textures[8];
    TexInfo info[8];
    u32 count, interval, next;
    s32 crop[4];
    s64 last;
} Window;
typedef struct Sync { u64 value; bool fenced; } Sync;  /* value: the queue timeline value that signals it */

/* ---------------------------------------------------------------------------------------------- commands */
enum {
    OP_END, OP_JUMP, OP_CALL,
    OP_BLEND, OP_CHANNEL_MASK, OP_COLOR, OP_MULTISAMPLE, OP_POLYGON, OP_DEPTH_STENCIL,
    OP_VERTEX_ATTRIBS, OP_VERTEX_STREAMS, OP_PROGRAM,
    OP_VERTEX_BUFFERS, OP_UNIFORM_BUFFERS, OP_STORAGE_BUFFERS, OP_TEXTURES, OP_IMAGES, OP_SEP_TEXTURES,
    OP_SEP_SAMPLERS, OP_XFB_BUFFERS, OP_UPDATE_UBO,
    OP_DRAW_ARRAYS, OP_DRAW_ELEMENTS, OP_DRAW_ARRAYS_INDIRECT, OP_DRAW_ELEMENTS_INDIRECT, OP_MULTI_DRAW_INDIRECT,
    OP_DISPATCH, OP_DISPATCH_INDIRECT, OP_DRAW_TEXTURE, OP_DRAW_XFB,
    OP_VIEWPORTS, OP_SCISSORS, OP_DEPTH_RANGES, OP_DEPTH_BOUNDS, OP_STENCIL_VALUE_MASK, OP_STENCIL_MASK,
    OP_STENCIL_REF, OP_BLEND_COLOR, OP_POINT_SIZE, OP_LINE_WIDTH, OP_POLYGON_OFFSET, OP_ALPHA_REF, OP_SAMPLE_MASK,
    OP_RASTERIZER_DISCARD, OP_DEPTH_CLAMP, OP_PRIMITIVE_RESTART, OP_PATCH_SIZE, OP_TESS_LEVELS,
    OP_RENDER_TARGETS, OP_CLEAR_COLOR, OP_CLEAR_DEPTH_STENCIL, OP_CLEAR_TEXTURE, OP_DISCARD,
    OP_COPY_BUFFER_TO_TEXTURE, OP_COPY_TEXTURE_TO_BUFFER, OP_COPY_TEXTURE_TO_TEXTURE, OP_COPY_BUFFER,
    OP_CLEAR_BUFFER, OP_DOWNSAMPLE, OP_BARRIER, OP_FENCE_SYNC, OP_WAIT_SYNC, OP_SIGNAL_EVENT, OP_WAIT_EVENT,
    OP_REPORT_COUNTER, OP_RESET_COUNTER, OP_REPORT_VALUE, OP_TEXTURE_POOL, OP_SAMPLER_POOL, OP_RENDER_ENABLE,
    OP_XFB, OP_COUNT
};

typedef struct BufferRange { u64 addr, size; } BufferRange;
typedef struct CopyRegion { s32 x, y, z, w, h, d; } CopyRegion;
typedef struct TexRef { TexInfo t; ViewInfo v; } TexRef;

/* payloads */
typedef struct CmdBind { u32 stage, first, count; } CmdBind;            /* + count ranges / handles */
typedef struct CmdUpdateUbo { u64 buffer, size, offset; u32 len; } CmdUpdateUbo;  /* + len bytes */
typedef struct CmdDraw {
    u32 mode, index_type, first, count; s32 base_vertex; u32 base_instance, instances; u64 index_addr;
} CmdDraw;
typedef struct CmdDrawIndirect {
    u32 mode, index_type; u64 index_addr, indirect, count_addr; u32 max_draws, stride;
} CmdDrawIndirect;
typedef struct CmdRange { u32 first, count; } CmdRange;                /* + values */
typedef struct CmdRenderTargets { u32 ncolor, has_depth; } CmdRenderTargets;  /* + TexRef per target */
typedef struct CmdClearColor { u32 index, mask, type; u32 v[4]; } CmdClearColor;  /* type 0 float 1 int 2 uint */
typedef struct CmdClearDS { float depth; u32 depth_mask, stencil, stencil_mask; } CmdClearDS;
typedef struct CmdClearTexture { TexRef tex; CopyRegion r; u32 mask, type; u32 v[4]; } CmdClearTexture;
typedef struct CmdCopyBT {              /* buffer <-> texture */
    u64 buffer; TexRef tex; CopyRegion r; u32 flags; s64 row_stride, image_stride;
} CmdCopyBT;
typedef struct CmdCopyTT { TexRef src, dst; CopyRegion sr, dr; u32 flags; } CmdCopyTT;
typedef struct CmdCopyBuffer { u64 src, dst, size; u32 flags; } CmdCopyBuffer;
typedef struct CmdClearBuffer { u64 addr, size; u32 value; } CmdClearBuffer;
typedef struct CmdDownsample { TexRef src, dst; } CmdDownsample;
typedef struct CmdSync { u64 sync; u32 cond, flags; } CmdSync;
typedef struct CmdEvent { u64 addr; u32 mode, value, location, flags; } CmdEvent;
typedef struct CmdReport { u32 counter, value; u64 addr; } CmdReport;
typedef struct CmdPool { u64 addr; u32 count; } CmdPool;           /* descriptor memory (CPU address) */
typedef struct CmdDrawTexture { u64 handle; float dst[4], src[4]; } CmdDrawTexture;

/* executor (gpu.c) */
void gpu_init(void);
void gpu_submit(u64 queue, u32 count, const u64 *handles);
void gpu_flush(u64 queue);
void gpu_finish(u64 queue);
u64 gpu_fence(u64 queue);                               /* a timeline value signalled after the work so far */
bool gpu_wait(u64 value, u64 timeout_ns);               /* true once reached */
u64 gpu_completed(void);
void gpu_present(u64 queue, Window *w, u32 index);
void gpu_pool_created(Pool *p);
void gpu_pool_destroyed(Pool *p);
void gpu_texture_written(const TexInfo *t, const ViewInfo *v, const CopyRegion *r, const void *data,
                         s64 row_stride, s64 image_stride);
void gpu_shaders_set(ShaderCode *s);                    /* a new shader: load its translation */
void gpu_window_created(Window *w);
void gpu_device_modes(u32 window_origin, u32 depth_mode);

/* formats (tex.c) */
void format_info(u32 f, u32 *bpb, u32 *bw, u32 *bh);
u64 texture_storage_size(const TexInfo *t);

#endif
