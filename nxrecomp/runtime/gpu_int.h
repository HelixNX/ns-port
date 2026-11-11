/* Internal to the NVN executor (gpu.c and gpu_*.c): the state and helpers its parts share. One lock (gpu_cs)
 * covers all of it. The public interface is in nvn.h. */
#pragma once

#include "tex.h"
#include "astc.h"
#include "game_config.h"
#include <process.h>
#include <stdlib.h>
#include <windows.h>
#include <zstd.h>

extern CRITICAL_SECTION gpu_cs;
extern u64 frame_no;
extern bool gpu_ready;

/* NX_* switches, read once at start (env_init, from gpu_init): none is looked up while translating */
struct GpuEnv {
    bool dump_astc, ssbo_log, pipe_log, no_discard, refresh_pools, shader_jit, pool_mirror, feedback_log, timeline,
        view_log, mutable_all, full_barriers, async_shaders, refresh_wait, same_rt;
    int gpu_skip, flush_draws, flush_first;
    double hitch_ms;
    long trace_lo, trace_hi;
    u64 watch, dump_draws, dump_ssbo, dump_tex;
    u64 dump_draws_list[4];
    const char *skip_shaders, *trace_ubo, *trace_fubo, *trace_vcb, *trace_filter;
    u64 debug_rt;                       /* NX_DEBUG_RT=<rt0 address>[:bits]: draws into it with 1 discards off, 2 depth test off,
                                         * 4 depth clamp on */
    u32 debug_rt_bits;
    char dump[256];
};
extern struct GpuEnv env;
extern u32 rs_scale;
extern bool rs_hint;

/* ---------------------------------------------------------------------------------------------- submissions */
#define SLOTS 12
typedef struct Slot {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkCommandBuffer pre;                /* runs before cmd: the pool mirrors' copies (mirror_sync) */
    u64 value;                          /* timeline value that marks it done */
    VkBuf staging;
    u64 staging_used;
    VkBuf extra[256];                   /* staging that did not fit */
    int nextra;
    u64 extra_used;
} Slot;
extern Slot slots[SLOTS];
extern int cur;
extern u64 slot_serial;
extern u64 submitted;
extern u32 work_unflushed;
extern VkSemaphore timeline;

/* things to destroy once the GPU is past `value` */
typedef struct Dead { u64 value; int kind; void *a, *b; VkAlloc alloc; } Dead;
enum { DEAD_HOSTBUF, DEAD_IMAGE, DEAD_VIEW, DEAD_MIRROR };
extern Dead *dead;
extern int ndead, capdead;
void reap(void);
void bury(int kind, void *a, const VkAlloc *alloc);

/* NX_GPU_PROF=1: GPU time by pass. A timestamp goes in at each render pass, dispatch and submission edge, the
 * interval after it charged to its label; read back when the slot is reused, logged every 300 frames in ms per
 * frame, with the render thread's time waiting for slots and guest threads' waits for the GPU */
#define PROF_MAX 8192
extern int prof_on;
extern VkQueryPool prof_pool[SLOTS];
extern double prof_slot_wait_ms, prof_guest_wait_ms;
extern double prof_wait_ms[4];
extern u64 prof_wait_n[4];
struct ProfSite { u64 site; double ms; u64 n; };
extern struct ProfSite prof_sites[16];
/* passes ended, by the source line that ended them (GPU_AT) */
struct ProfEnd { const char *at; u64 count; };
extern struct ProfEnd prof_ends[64];
void prof_mark_in(int slot, const char *label);
void prof_mark(const char *label);
void prof_collect(int slot);
void prof_report(u64 frames);
extern double t_submit_ms, t_present_ms, t_wait_ms;

/* NX_HITCH_MS=<ms>: each frame longer than that (present to present) is logged with where the render thread's time
 * went in it (gpu_present), to tell which work makes the frame-time spikes */
struct HitchTimes {
    double submit, present, wait, shader, jit, pipe, image, upload, slot, mirror;
    u32 nshader, njit, npipe, nimage, nupload;
};
extern struct HitchTimes hf;
double ms_now(void);
extern VkQueryPool tl_pool;
extern double tl_submit[SLOTS];
extern bool tl_first[SLOTS], tl_used[SLOTS], tl_guest_first;
extern double tl_woke, tl_core, tl_entry, tl_rec_woke[SLOTS], tl_rec_core[SLOTS], tl_rec_entry[SLOTS];
void tl_collect(int slot);
void tl_report(u64 frames);
void slot_open(void);

static inline VkCommandBuffer cmd(void) {
    if (cur < 0) slot_open();
    return slots[cur].cmd;
}
void *staging(u64 size, VkBuffer *buf, u64 *off);
void barrier_src(VkCommandBuffer c, VkAccessFlags2 src);

static inline void barrier_in(VkCommandBuffer c) { barrier_src(c, VK_ACCESS_2_MEMORY_WRITE_BIT); }

/* the writes a pass without colour or depth writes can have made (transform feedback, shader stores): naming only
 * these spares the driver flushing the colour and depth caches (RGP: every barrier flushed CB / DB / L2 with a
 * full stall, ~230 a frame, many after transform-feedback passes that rasterize nothing) */
#define ACCESS_NO_ATTACHMENTS (VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT | \
                               VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT)
void label_begin(VkCommandBuffer c, const char *fmt, ...);
extern bool label_draw_open;
void label_draw_end(void);
void barrier_at(const char *at, VkAccessFlags2 src);
/* the source line a barrier or pass end comes from ("gpu_draw.c:123"), for debug labels and NX_GPU_PROF */
#define GPU_STR_(x) #x
#define GPU_STR(x) GPU_STR_(x)
#define GPU_AT (__FILE_NAME__ ":" GPU_STR(__LINE__))
#define barrier() barrier_at(GPU_AT, VK_ACCESS_2_MEMORY_WRITE_BIT)
extern bool pass_att_writes;
#define end_rendering() end_rendering_at(GPU_AT)
extern int nwb;
void flush(void);

/* ---------------------------------------------------------------------------------------------- pools */
/* Pools the GPU reads most also get a copy in video memory (a mirror), which draws read their vertex, index and
 * uniform buffers from. Over host memory the GPU fetched ~120 MB a frame through PCIe in Mesagoza (one 367 MB
 * CPU_UNCACHED pool holds every mesh and uniform buffer), and a laptop's x8 link starved the GTX 1650 Ti while its
 * video memory sat idle. The mirror follows guest memory: pages the CPU wrote (write watch) are copied in at each
 * submission before its commands run (mirror_sync), and the GPU's own copies, clears and small writes there go to
 * both. Storage buffers, transform feedback and indirect arguments stay on host memory; a range they write is read
 * from host memory from then on (Mirror.bad). Mirrors are on for slow PCIe links (env_init); NX_POOL_MIRROR=0/1
 * forces them off or on */
typedef struct { u64 lo, hi; } Range;
typedef struct Mirror { VkBuf b; bool fresh; Range *bad; int nbad, capbad; } Mirror;
extern double mirror_mb, mirror_ms;
extern int nmirrors;
typedef struct PoolEnt { u64 gpu, end, cpu; u32 flags; HostBuffer *hb; Mirror *mir; } PoolEnt;
extern PoolEnt *pools;
extern int npools, cappools;
extern u64 pools_gen;
PoolEnt *pool_find(u64 addr);
bool gpu_buffer(u64 addr, VkBuffer *buf, u64 *off);
u8 *gpu_cpu(u64 addr, u64 size);
void mirror_free(void *p);
VkBuffer mirror_at(u64 addr, u64 *off);
void mirror_bad(u64 addr, u64 size);
void mirror_bad_ssbos(const BufferRange *ssbo, const ShaderCode *sc);
bool gpu_buffer_ro(u64 addr, u64 size, VkBuffer *buf, u64 *off);

/* ---------------------------------------------------------------------------------------------- images */
typedef struct View { ViewInfo v; u32 kind; VkImageView view; } View;
enum { VIEW_SAMPLED, VIEW_ATTACHMENT, VIEW_STORAGE, VIEW_SAMPLED_INT };
typedef struct Image {
    TexInfo info;
    VkImage image;
    VkAlloc alloc;
    VkFormat format;
    VkFormat vfmts[6];                  /* the formats its views may use (format_list); vfmts[0] is format */
    u32 nvfmts;
    VkImageAspectFlags aspects;
    VkImageUsageFlags usage;
    u32 levels, layers;
    bool valid;                         /* contents defined (rendered, copied, cleared or uploaded) */
    u64 rt_frame;                       /* the frame (frame_no + 1) it was last a render target in */
    bool gpu_written;                   /* rendered, cleared or copied into: guest memory is not its source */
    u64 size;                           /* bytes of guest memory it covers */
    u64 used;                           /* the frame (frame_no + 1) it was last looked up in */
    bool wb_pending;                    /* pitch-linear, GPU-written since its last copy back (linear_wrote) */
    View *views;
    int nviews, capviews;
    struct Image *next;
    u32 scale;                          /* render scale it was made at: its Vulkan size is the guest size times this */
    bool can_scale;                     /* screen-shaped (image_candidate), or drawn with a scaled depth (image_promote) */
    VkImageCreateFlags cflags;
} Image;

/* level l's Vulkan size */
static inline u32 lvl_w(const Image *im, u32 l) { u32 w = (im->info.width * im->scale) >> l; return w ? w : 1; }
static inline u32 lvl_h(const Image *im, u32 l) { u32 h = (tex_height(&im->info) * im->scale) >> l; return h ? h : 1; }

/* Pitch-linear textures are memory the CPU may read as it is (on the console, one memory): what the GPU writes
 * into one is copied back to guest memory at the end of the submission (flush), before its syncs signal. Violet's
 * screen capture (profile photo, player icon) renders into a linear RGBA8 texture and BC1-encodes it on the CPU;
 * without this it read zeros and saved a black picture. */
#define WB_MAX 64
extern Image *wb_list[WB_MAX];
void linear_wrote(Image *im);
#define IMAGE_BUCKETS 4096
extern Image *buckets[IMAGE_BUCKETS];
extern int nimages;
extern u64 image_bytes;
extern u64 image_epoch;
void images_drop_range(u64 lo, u64 hi);
VkFormatFeatureFlags format_features(VkFormat f);

/* a blit's filter: linear where the format allows it (not integer formats, nor depth) */
static inline VkFilter smooth(VkFormat f) {
    return format_features(f) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT && !(format_aspects(f) & ~VK_IMAGE_ASPECT_COLOR_BIT) ?
           VK_FILTER_LINEAR : VK_FILTER_NEAREST;
}
VkFormat usable_format(u32 nvn);

/* Integer textures (UINT/SINT formats, and depth-stencil textures in stencil mode) are read by shaders through
 * integer views: the texture arrays TB_U2D / TB_U2D_ARRAY hold them, and the handle written to c[2] gets bit 19
 * (texture ids stay below 65536), which translate.py tests at every fetch and gather (a float read of an integer
 * view is undefined: AMD returned garbage for the stencil a culling pass reads, hiding objects near the camera) */
#define HANDLE_INT (1u << 19)
#define HANDLE_SCALED (1u << 18)        /* the texture or image is at the render scale (translate.py SCALED_FLAG) */
Image *image_find(const TexInfo *t, u32 *layer, u32 *level);
Image *image_ref(const TexRef *r, ViewInfo *v);
Image *image_get(const TexInfo *t);
u32 view_layers(const Image *im, const ViewInfo *v, u32 kind, u32 *base);
VkImageView image_view(Image *im, const ViewInfo *v, u32 kind);

/* a plain 2D (array) image for staging copies into and out of scaled images, in GENERAL layout */
typedef struct { VkImage image; VkAlloc alloc; } Temp;
bool temp_image(VkFormat f, u32 w, u32 h, u32 levels, u32 layers, Temp *t);
static inline void temp_free(Temp *t) { bury(DEAD_IMAGE, t->image, &t->alloc); }
bool scale_blittable(const Image *im);
void image_rescale(Image *im, u32 s);
bool image_promote(Image *im);
void image_upload(Image *im);

/* ---------------------------------------------------------------------------------------------- state */
typedef struct State {
    BlendState blend[8];
    ColorState color;
    ChannelMaskState mask;
    MultisampleState ms;
    PolygonState poly;
    DepthStencilState ds;
    VertexAttribState attribs[32];
    u32 nattribs;
    VertexStreamState streams[32];
    u32 nstreams;
    Shaders *program;                   /* the graphics shaders bound for each stage, as one interned set */
    ShaderCode *bound[ST_COUNT];        /* by stage; compute is bound independently of the graphics stages */
    BufferRange vbuf[32];
    BufferRange ubo[ST_COUNT][18], ssbo[ST_COUNT][16];
    u64 tex[ST_COUNT][32], img[ST_COUNT][8];
    float viewport[16][4], depth_range[16][2];
    s32 scissor[16][4];
    float blend_color[4], line_width, point_size, poly_offset[3], alpha_ref, depth_bounds[2];
    u32 stencil_value_mask[2], stencil_mask[2], stencil_ref[2], sample_mask;
    bool depth_bounds_test, rasterizer_discard, depth_clamp, primitive_restart, render_enable;
    u32 restart_index, patch_size;
    TexRef rt[8];
    u32 nrt;
    TexRef depth;
    bool has_depth;
    CmdPool texpool, smppool;
    bool rendering;
    VkRect2D area;
    VkFormat rt_format[8], depth_format, stencil_format;     /* of the rendering in progress */
    struct { u64 addr, size; u32 level, layer, nlayers; } rt_sub[9];    /* ... its targets (8: depth; size 0: none) */
    u32 samples;
    u32 rt_scale;                       /* ... the scale its targets were made at (1 or rs_scale) */
    BufferRange xfb[4];                 /* transform feedback buffers, the bytes captured into each since bound */
    u64 xfb_written[4];
    bool xfb_active;                    /* between Begin (or Resume) and End (or Pause) */
    bool xfb_captured;                  /* a draw captured since the last barrier */
    u64 xfb_dirty[4][2];                /* the bytes captured since then: [lo, hi) of each buffer's addresses */
} State;
extern State st;
extern u64 draws, draws_skipped, dispatches;
extern u64 rt_kept;
void state_reset(State *s);

/* small GPU writes made while rendering (counter reports between occlusion boxes, events), done when the pass
 * ends: Vulkan allows no buffer updates inside rendering, and ending the pass for each one split the occlusion
 * pass into hundreds */
typedef struct PendingWrite { VkBuffer buf, mbuf; u64 off, moff; u32 size; u8 data[16]; } PendingWrite;
extern PendingWrite pending[1024];
extern u32 npending;
void end_rendering_at(const char *at);
void watch(const char *what, const TexRef *r);
bool begin_rendering(void);
VkRect2D clear_rect(void);
void do_clear_color(const CmdClearColor *p);
void do_clear_depth_stencil(const CmdClearDS *p);
void do_clear_texture(const CmdClearTexture *p);

static inline VkImageAspectFlags copy_aspect(const Image *im) {
    return im->aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : im->aspects;
}
void do_copy_buffer_to_texture(const CmdCopyBT *p);
void do_copy_texture_to_buffer(const CmdCopyBT *p);
void linear_writeback(void);
bool blit_ok(const Image *s, const Image *d);
void do_copy_texture_to_texture(const CmdCopyTT *p);
void do_downsample(const CmdDownsample *p);
void gpu_write(u64 addr, const void *data, u32 size);
u64 now_ns(void);

/* ---------------------------------------------------------------------------------------------- draws
 * One pipeline layout for every graphics pipeline:
 *   set 0  push descriptors: uniform buffers c[0..15] per stage (vertex at bindings 0-15, fragment 16-31);
 *          c[0] and c[2] are ours (storage buffer table, texture handles), c[1] the shader's embedded constants,
 *          c[3 + b] the game's uniform buffer b
 *   set 1  the current texture pool: one sampled image per pool entry (bindless, indexed by texture id), in an
 *          array per view type (binding TB_*): a descriptor's view type must match the shader's image dimension
 *          (AMD loses the device on a 3D read of a 2D array view), so each array holds the views of that type the
 *          entry's image allows and a 1x1 black texture of that type elsewhere; translate.py declares t2d, t3d...
 *          at these bindings
 *   set 2  the current sampler pool: one sampler per entry
 *   set 3  the current texture pool's entries as storage images (NVN images, indexed by image id)
 *   push constants: the alpha test (fragment)
 * Pipelines depend only on the shaders and the attachment formats: all other state is dynamic. */
#define TEXPOOL_MAX 65536
#define SMPPOOL_MAX 4096
extern VkDescriptorSetLayout set_ubo, set_tex, set_smp, set_img;
extern VkPipelineLayout layout_gfx;
/* push constants (translate.py): alpha function and reference, point size, debug bits, render scale; one range for
 * every stage (compute reads only the scale) */
#define PC_STAGES (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT)
#define PC_SIZE 20
extern VkDescriptorSet empty_tex, empty_smp, empty_img;
extern u64 submit_serial;
extern u32 window_origin, depth_mode;

/* a file in the build folder (the executable's parent folder) */
static inline void build_file(char *path, size_t size, const char *name) {
    snprintf(path, size, "%s\\%s", nx_cache_dir(), name);
}
const char *frames_dir(void);
void translator_path(char *out, size_t n);
void pcache_load(void);
unsigned __stdcall pcache_thread(void *arg);
void layouts_init(void);
VkDescriptorSet alloc_set(VkDescriptorSetLayout l, u32 count);
VkDescriptorSet alloc_tex_set(void);

/* ---- texture and sampler pools as descriptor sets, kept in step with the descriptors the game registers */
typedef struct DescSet {
    u64 addr;                           /* the pool's descriptor memory */
    u32 count;
    u32 kind;                           /* DS_TEX, DS_SMP, DS_IMG */
    VkDescriptorSet set;
    u32 *idx;                           /* interned descriptor in each entry (as last seen), ~0 none */
    u8 *res;                            /* texture entries: written into the set since idx or the images changed */
    Image **img;                        /* ... and the image written (NULL: the dummy) */
    u8 *intv;                           /* texture entries: read through integer views (HANDLE_INT) */
    u64 epoch, synced;
    u64 gen;                            /* bumped whenever an entry's image may change (res set or cleared) */
    struct DescSet *next;
} DescSet;
enum { DS_TEX, DS_SMP, DS_IMG };
extern DescSet *descsets;
#define DUMMY_ADDRS 5                   /* images at guest addresses 1-5 are ours: never evicted */
void dummies_init(void);
void descset_fill(VkDescriptorSet set, bool smp, u32 count);
DescSet *descset_get(const CmdPool *p, u32 kind);
void images_evict(u64 target, bool wait);
void descset_use(DescSet *d, const u64 *tex, u32 n);
void descset_sync(DescSet *d);
extern char spv_dir[MAX_PATH];
void spv_path(char *path, size_t size, u64 hash);
u32 *spv_read(u64 hash, long *n);

/* Shaders are translated while the game runs. A shader the game creates with no SPIR-V is queued at once
 * (gpu_shader_created) for a host thread that runs tools/shader/translate.py --spv --only=<hashes> on everything
 * queued (~0.3 s a batch: shaders created together share one). A draw that needs a shader still queued waits for it
 * (the game stalls instead of drawing without it), or with NX_ASYNC_SHADERS (env_init) is skipped until the
 * translation is there; dispatches always wait (a skipped one can leave wrong results for good). NX_SHADER_JIT=0
 * turns this off (such draws are skipped); NX_PYTHON names the interpreter (default python). */
struct JitQueue { u64 *q; u32 *qm, n, cap; u64 seq; SRWLOCK lock; HANDLE wake; };
extern struct JitQueue jit;
VkShaderModule shader_module_w(ShaderCode *s, bool wait);

static inline VkShaderModule shader_module(ShaderCode *s) { return shader_module_w(s, true); }
u32 int_attr_mask(void);

/* ---- pipelines */
/* Pipelines are keyed by shader hashes, not modules, so the keys mean the same in every session: each pipeline the
 * game needs is recorded in pipelines.list (build/), and at start worker threads build the recorded ones before
 * the game asks for them (pipelines_prebuild), in the order they were first needed. A draw needing one being built
 * waits for it. Recorded pipelines whose SPIR-V is not on disk yet are left to the draws. Compute pipelines are
 * keys too (compute = 1, the shader in vs). */
typedef struct PipeKey {
    u64 vs, fs;                         /* shader hashes (fs 0: none) */
    u32 vs_mask;                        /* the vertex shader's integer inputs (int_attr_mask, INT_ATTRS) */
    VkFormat color[8], depth, stencil;
    u32 ncolor, samples, minus_one;
    u32 logic_op;                       /* VkLogicOp, only without a dynamic logic op (vk.dyn_logic_op) */
    u32 alpha_func;                     /* NVN's alpha test (1-7; 0 = always passes): the fragment shader's ALPHA_FUNC */
    u32 compute, pad;                   /* pad (0): no padding bytes in the key (memcmp, hashing, the file) */
} PipeKey;
extern u32 npipes;
extern u64 pipe_vs_hash, pipe_fs_hash;
void bins_load(void);
VkPipeline pipeline_get(const PipeKey *k, VkShaderModule vs, VkShaderModule fs, bool async, bool *pending);
void pipelines_prebuild(void);
extern volatile LONG capture_request;
extern u64 capture_frame;
extern volatile LONG64 capture_done_ms;
extern volatile LONG reload_request;
bool trace_frame(void);
extern const char *skip_why[16];
extern u64 skip_count[16];
extern int nskip_why;

/* Graphics state as last recorded into the command buffer being built (valid while serial == slot_serial: state is
 * undefined in a new one). The driver does real work for every vkCmdSet / bind, ~40 of them a draw; draws in a
 * pass mostly repeat the last one's, so only what changed is recorded */
typedef struct DynState {
    VkViewport vp;
    VkRect2D sc;
    float line_width, bias[3], blend[4], bounds[2];
    u32 cmp_mask[2], wr_mask[2], ref[2], cull, front, topo, dtest, dwrite, dcmp, dbtest, stest, sop[2][4];
    u32 discard, bias_en, restart, poly_mode, samples, sample_mask, a2c, logic_en, logic_op, clamp, nrt;
    VkBool32 en[8];
    VkColorBlendEquationEXT eq[8];
    VkColorComponentFlags wm[8];
} DynState;
struct Recorded {
    u64 serial;
    bool dyn_ok, vi_ok, bind_ok;        /* each part recorded since the reset */
    DynState d;
    VkPipeline pipe;
    VkDescriptorSet sets[3];
    u32 pc[PC_SIZE / 4];
    u32 nw;
    VkDescriptorBufferInfo bi[32];
    u32 binding[32], type[32];
    u32 nb, na;
    VkVertexInputBindingDescription2EXT bind[32];
    VkVertexInputAttributeDescription2EXT attr[48];
    VkBuffer bufs[32];
    VkDeviceSize offs[32], sizes[32];
};
extern struct Recorded rec;

/* after a built-in pass (depth_blit) bound its own pipeline, layout and state: record everything again */
static inline void rec_forget(void) {
    memset(&rec, 0, sizeof rec);
    rec.serial = slot_serial;
}
void draw(u32 op, const void *payload);
void dispatch(u32 op, const u8 *pay);
void dump_image_as(Image *im, const char *name, bool raw, VkFormat fmt);
void dump_frame(Image *win, u64 frame);
extern double refresh_ms, refresh_mb;
size_t written_pages(u64 lo, u64 hi, u64 **pages, size_t *cap, size_t n, double *mb);
int refresh_pages(const u64 *pages, size_t n);
void image_touch(Image *im);
extern double scan_ms, scan_ms_max;
extern u64 scan_skips;
void refresh_cpu_textures(void);
void mirror_scan_start(void);
bool mirror_sync(VkCommandBuffer pre);
void ind_log_report(void);
void query_report(void);
