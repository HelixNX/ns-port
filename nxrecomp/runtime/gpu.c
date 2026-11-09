/* The NVN executor: replays recorded command buffers (nvn.h) on Vulkan, inside nvnQueueSubmitCommands.
 *
 * Memory: every memory pool the GPU can reach is imported as one Vulkan buffer over the guest's own memory
 * (VK_EXT_external_memory_host), so vertex, index, uniform and storage data, reports and events are read and
 * written where the game put them, as on the console's unified memory. Textures are the exception: they live
 * in Vulkan images (an image cache keyed by TexInfo); an image is filled from guest memory (deswizzled) the first
 * time it is read if nothing on the GPU has written it.
 *
 * Images stay in VK_IMAGE_LAYOUT_GENERAL; ordering is by global memory barriers at NVN barriers, render target
 * changes and around transfers (correct first, fast later).
 *
 * One lock (gpu_cs) covers everything here; work is submitted at each nvnQueueSubmitCommands, on a timeline
 * semaphore whose values are the NVN syncs.
 *
 * Files: this one has the submissions (slots, staging, barriers, flush), the replay of command buffers, presents
 * and start-up; gpu_int.h what the parts share; gpu_pool.c memory pools and their mirrors; gpu_image.c the image
 * cache; gpu_desc.c descriptor sets; gpu_shader.c SPIR-V and the translator; gpu_pipe.c pipelines; gpu_draw.c draws
 * and dispatches; gpu_xfer.c clears, copies and blits; gpu_prof.c profiling; gpu_dump.c debug dumps. */
#include "gpu_int.h"

CRITICAL_SECTION gpu_cs;
u64 frame_no;                           /* frames presented so far */
bool gpu_ready;
struct GpuEnv env;

/* Render scale (the options panel, NX_RES_SCALE): screen-shaped render targets and storage images (image_candidate)
 * are made N times the size the game asked for; viewports, scissors, clears and copies are scaled to match, and
 * shaders learn which textures are scaled from bit 18 of their handles (HANDLE_SCALED) and the scale from the push
 * constants (tools/shader/translate.py: gl_FragCoord, texelFetch, texture sizes and image stores). Images become
 * scaled when first drawn into (rt_view) or bound as storage; a change while playing waits for the GPU and resizes
 * them in place (rescale_all, at a present). Every image is at scale 1 or rs_scale. */
u32 rs_scale = 1;                       /* the scale in use (render thread, under gpu_cs) */
static volatile LONG rs_want = 1;       /* the scale asked for */
bool rs_hint;                           /* an image made now is a render target or storage image */
void gpu_render_scale_set(u32 s) { InterlockedExchange(&rs_want, (LONG)(s < 1 ? 1 : s > 4 ? 4 : s)); }
u32 gpu_render_scale(void) { return (u32)rs_want; }
void vk_wait_idle(void);

/* ---------------------------------------------------------------------------------------------- submissions */
#define STAGING_SIZE (32ull << 20)
Slot slots[SLOTS];
int cur = -1;                           /* the slot being recorded, or -1 */
u64 slot_serial;                        /* bumped with each slot opened (staging from older ones is gone) */
u64 submitted;                          /* last timeline value submitted */
u32 work_unflushed;                     /* draws and dispatches recorded since the last submission */
static u32 head_flush;                  /* the next early flush, in draws (NX_FLUSH_FIRST, doubling; 0: none) */
VkSemaphore timeline;
Dead *dead;
int ndead, capdead;

u64 gpu_completed(void) {
    u64 v = 0;
    if (timeline) vkGetSemaphoreCounterValue(vk.dev, timeline, &v);
    return v;
}

void reap(void) {
    u64 done = gpu_completed();
    int j = 0;
    for (int i = 0; i < ndead; i++) {
        Dead *d = &dead[i];
        if (d->value > done) { dead[j++] = *d; continue; }
        switch (d->kind) {
        case DEAD_HOSTBUF: vk_release_host(d->a); free(d->a); break;
        case DEAD_IMAGE: vkDestroyImage(vk.dev, d->a, NULL); vk_free(&d->alloc); break;
        case DEAD_VIEW: vkDestroyImageView(vk.dev, d->a, NULL); break;
        case DEAD_MIRROR: mirror_free(d->a); break;
        }
    }
    ndead = j;
}

void bury(int kind, void *a, const VkAlloc *alloc) {
    if (ndead == capdead) dead = realloc(dead, (capdead = capdead * 2 + 64) * sizeof *dead);
    Dead *d = &dead[ndead++];
    memset(d, 0, sizeof *d);
    d->value = submitted + 1;           /* the work being recorded may still use it */
    d->kind = kind;
    d->a = a;
    if (alloc) d->alloc = *alloc;
}

static VkCommandBuffer cmd(void);

void slot_open(void) {
    static int next;
    Slot *s = &slots[next];
    if (prof_on && !prof_pool[0]) {
        for (int i = 0; i < SLOTS; i++) {
            VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                        .queryCount = PROF_MAX};
            VKCHECK(vkCreateQueryPool(vk.dev, &qi, NULL, &prof_pool[i]));
            vkResetQueryPool(vk.dev, prof_pool[i], 0, PROF_MAX);
        }
    }
    if (s->value) {
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                                  .pSemaphores = &timeline, .pValues = &s->value};
        double t0 = ms_now();
        VKCHECK(vkWaitSemaphores(vk.dev, &wi, UINT64_MAX));
        hf.slot += ms_now() - t0;
        if (prof_on) prof_slot_wait_ms += ms_now() - t0;
    }
    if (prof_on) {
        prof_collect(next);
        vkResetQueryPool(vk.dev, prof_pool[next], 0, PROF_MAX);
    }
    if (env.timeline) {
        if (!tl_pool) {
            VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                        .queryCount = 2 * SLOTS};
            VKCHECK(vkCreateQueryPool(vk.dev, &qi, NULL, &tl_pool));
            vkResetQueryPool(vk.dev, tl_pool, 0, 2 * SLOTS);
        }
        tl_collect(next);
        vkResetQueryPool(vk.dev, tl_pool, 2 * next, 2);
    }
    for (int i = 0; i < s->nextra; i++) vk_buffer_free(&s->extra[i]);
    s->nextra = 0;
    s->staging_used = 0;
    VKCHECK(vkResetCommandPool(vk.dev, s->pool, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKCHECK(vkBeginCommandBuffer(s->cmd, &bi));
    if (prof_on) prof_mark_in(next, "outside passes (copies, clears, barriers)");
    if (env.timeline) vkCmdWriteTimestamp2(s->cmd, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, tl_pool, 2 * next);
    cur = next;
    next = (next + 1) % SLOTS;
    slot_serial++;
}

/* host-visible scratch for this submission: uploads and inline data */
void *staging(u64 size, VkBuffer *buf, u64 *off) {
    cmd();
    Slot *s = &slots[cur];
    size = (size + 255) & ~255ull;
    if (s->staging_used + size <= STAGING_SIZE) {
        *buf = s->staging.buf;
        *off = s->staging_used;
        s->staging_used += size;
        return s->staging.a.map + *off;
    }
    if (s->nextra && s->extra_used + size <= s->extra[s->nextra - 1].size) {
        VkBuf *e = &s->extra[s->nextra - 1];
        *buf = e->buf;
        *off = s->extra_used;
        s->extra_used += size;
        return e->a.map + *off;
    }
    if (s->nextra == 256) fatal("gpu: too much staging in one submission");
    VkBuf *e = &s->extra[s->nextra++];
    u64 chunk = size > STAGING_SIZE ? size : STAGING_SIZE;
    s->extra_used = size;
    vk_buffer(chunk, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, e);
    *buf = e->buf;
    *off = 0;
    return e->a.map;
}

void barrier_src(VkCommandBuffer c, VkAccessFlags2 src) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = src,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(c, &d);
}

/* NX_GPU_LABELS: debug-utils regions a capture shows as user events (RGP): each pass by its targets, each draw and
 * dispatch by its shaders, each barrier by the line that asked for it. Regions nest: a draw's closes before its
 * pass ends (label_draw_end, end_rendering) */
void label_begin(VkCommandBuffer c, const char *fmt, ...) {
    char s[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    VkDebugUtilsLabelEXT l = {VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, .pLabelName = s};
    vkCmdBeginDebugUtilsLabelEXT(c, &l);
}

bool label_draw_open;

void label_draw_end(void) {
    if (!label_draw_open) return;
    vkCmdEndDebugUtilsLabelEXT(cmd());
    label_draw_open = false;
}

void barrier_at(const char *at, VkAccessFlags2 src) {
    VkCommandBuffer c = cmd();
    if (vk.labels) label_begin(c, "barrier %s%s", at, src == VK_ACCESS_2_MEMORY_WRITE_BIT ? "" : " (no attachment writes)");
    barrier_src(c, src);
    if (vk.labels) vkCmdEndDebugUtilsLabelEXT(c);
}

/* the current pass has written colour or depth (a draw that rasterizes, a clear): end_rendering's barrier */
bool pass_att_writes;

int nwb;

void flush(void) {
    end_rendering();
    if (cur < 0) return;
    if (nwb) linear_writeback();
    Slot *s = &slots[cur];
    bool pre = mirror_sync(s->pre);
    prof_mark("(end)");
    if (env.timeline) {
        vkCmdWriteTimestamp2(s->cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, tl_pool, 2 * cur + 1);
        tl_first[cur] = tl_guest_first;
        tl_rec_woke[cur] = tl_woke, tl_rec_core[cur] = tl_core, tl_rec_entry[cur] = tl_entry;
        tl_guest_first = false;
        tl_used[cur] = true;
        tl_submit[cur] = ms_now();
    }
    VKCHECK(vkEndCommandBuffer(s->cmd));
    s->value = ++submitted;
    VkSemaphoreSubmitInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = timeline, .value = s->value,
                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkCommandBufferSubmitInfo cbi[2] = {{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->pre},
                                        {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->cmd}};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = pre ? 2 : 1, .pCommandBufferInfos = cbi + !pre,
                        .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &sig};
    VkResult r = vk_submit(&si, NULL);
    if (r < 0) fatal("vkQueueSubmit2: %d (device lost?)", r);
    cur = -1;
    work_unflushed = 0;
    reap();
}
u64 submit_serial;

/* F12 captures and other image dumps: build\frames, or in a package %LOCALAPPDATA%\<title>\dumps (made on use) */
const char *frames_dir(void) {
    static char dir[MAX_PATH];
    if (!dir[0]) {
#ifdef NX_PACKAGE
        snprintf(dir, sizeof dir, "%s\\dumps", nx_data_dir());
#else
        snprintf(dir, sizeof dir, "%s\\frames", nx_data_dir());
#endif
    }
    CreateDirectoryA(dir, NULL);
    return dir;
}

/* the shader translator: tools/shader/translate.py in a development build; in a package (NX_PACKAGE) its copy in
 * translator\ next to the executable, run by the Python there (translator\python\python.exe) */
void translator_path(char *out, size_t n) {
#ifdef NX_PACKAGE
    char exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *slash = strrchr(exe, '\\');
    if (slash) *slash = 0;
    snprintf(out, n, "%s\\translator\\translate.py", exe);
#else
    snprintf(out, n, "%s/tools/shader/translate.py", NX_ROOT);
#endif
}

/* ---------------------------------------------------------------------------------------------- replay */
#define PL(T) const T *p = (const T *)(uintptr_t)(at + 4)

static void run(u64 at, int depth) {
    for (;;) {
        u32 hdr = LD32(at), op = hdr & 0xff, len = hdr >> 8;
        const u8 *pay = (const u8 *)(uintptr_t)(at + 4);
        switch (op) {
        case OP_END: return;
        case OP_JUMP: at = LD64(at + 4); continue;
        case OP_CALL:
            if (depth < 8) run(LD64(at + 4), depth + 1);
            break;
        case OP_BLEND: { PL(BlendState); st.blend[p->target & 7] = *p; break; }
        case OP_CHANNEL_MASK: { PL(ChannelMaskState); st.mask = *p; break; }
        case OP_COLOR: { PL(ColorState); st.color = *p; break; }
        case OP_MULTISAMPLE: { PL(MultisampleState); st.ms = *p; break; }
        case OP_POLYGON: { PL(PolygonState); st.poly = *p; break; }
        case OP_DEPTH_STENCIL: { PL(DepthStencilState); st.ds = *p; break; }
        case OP_VERTEX_ATTRIBS: {
            u32 n;
            memcpy(&n, pay, 4);
            st.nattribs = n > 32 ? 32 : n;
            memcpy(st.attribs, pay + 4, 4 * st.nattribs);
            break;
        }
        case OP_VERTEX_STREAMS: {
            u32 n;
            memcpy(&n, pay, 4);
            st.nstreams = n > 32 ? 32 : n;
            memcpy(st.streams, pay + 4, 8 * st.nstreams);
            break;
        }
        case OP_PROGRAM: {              /* binds the program's shaders for the given stages only */
            u64 s;
            u32 stages;
            memcpy(&s, pay, 8);
            memcpy(&stages, pay + 8, 4);
            const Shaders *pr = (const Shaders *)(uintptr_t)s;
            for (int i = 0; i < ST_COUNT; i++)
                if (stages >> i & 1) st.bound[i] = pr ? pr->stage[i] : NULL;
            if (!(stages & 0x1f)) break;
            ShaderCode *gfx[ST_COUNT];
            memcpy(gfx, st.bound, sizeof gfx);
            gfx[ST_COMPUTE] = NULL;
            /* usually exactly the program's own stages end up bound: its set needs no lookup */
            st.program = pr && !memcmp(pr->stage, gfx, sizeof gfx) ? (Shaders *)pr : shaders_intern(gfx);
            break;
        }
        case OP_VERTEX_BUFFERS: case OP_UNIFORM_BUFFERS: case OP_STORAGE_BUFFERS: {
            CmdBind b;
            memcpy(&b, pay, sizeof b);
            const BufferRange *r = (const BufferRange *)(pay + sizeof b);
            BufferRange *dst = op == OP_VERTEX_BUFFERS ? st.vbuf : op == OP_UNIFORM_BUFFERS ? st.ubo[b.stage % ST_COUNT] : st.ssbo[b.stage % ST_COUNT];
            u32 cap = op == OP_VERTEX_BUFFERS ? 32 : op == OP_UNIFORM_BUFFERS ? 18 : 16;
            for (u32 i = 0; i < b.count && b.first + i < cap; i++) dst[b.first + i] = r[i];
            break;
        }
        case OP_TEXTURES: case OP_IMAGES: case OP_SEP_TEXTURES: case OP_SEP_SAMPLERS: {
            CmdBind b;
            memcpy(&b, pay, sizeof b);
            const u64 *h = (const u64 *)(pay + sizeof b);
            u64 *dst = op == OP_IMAGES ? st.img[b.stage % ST_COUNT] : st.tex[b.stage % ST_COUNT];
            u32 cap = op == OP_IMAGES ? 8 : 32;
            for (u32 i = 0; i < b.count && b.first + i < cap; i++) {
                if (op == OP_SEP_TEXTURES) dst[b.first + i] = (dst[b.first + i] & ~0xfffffull) | (h[i] & 0xfffff);
                else if (op == OP_SEP_SAMPLERS) dst[b.first + i] = (dst[b.first + i] & 0xfffff) | (h[i] & ~0xfffffull);
                else dst[b.first + i] = h[i];
            }
            break;
        }
        case OP_XFB_BUFFERS: {
            CmdBind b;
            memcpy(&b, pay, sizeof b);
            const BufferRange *r = (const BufferRange *)(pay + sizeof b);
            for (u32 i = 0; i < b.count && b.first + i < 4; i++) {
                st.xfb[b.first + i] = r[i];
                st.xfb_written[b.first + i] = 0;
            }
            break;
        }
        case OP_UPDATE_UBO: {
            CmdUpdateUbo u;
            memcpy(&u, pay, sizeof u);
            VkBuffer buf;
            u64 off;
            if (u.len && gpu_buffer(u.buffer + u.offset, &buf, &off)) {
                end_rendering();
                vkCmdUpdateBuffer(cmd(), buf, off, (u.len + 3) & ~3u, pay + sizeof u);
                VkBuffer mbuf = mirror_at(u.buffer + u.offset, &off);
                if (mbuf) vkCmdUpdateBuffer(cmd(), mbuf, off, (u.len + 3) & ~3u, pay + sizeof u);
                barrier();
            }
            break;
        }
        case OP_DRAW_ARRAYS: case OP_DRAW_ELEMENTS: case OP_DRAW_ARRAYS_INDIRECT: case OP_DRAW_ELEMENTS_INDIRECT:
        case OP_MULTI_DRAW_INDIRECT: case OP_DRAW_TEXTURE: case OP_DRAW_XFB:
            draw(op, pay);
            /* the game waits for its last frame before submitting the next, so the GPU idles until the first
             * flush: hand it the first draws early, even in the middle of a pass (that pass splits), then twice as
             * many each time, so that it has the next part before it runs out, until flushes come at pass
             * boundaries (NX_FLUSH_DRAWS) */
            if (head_flush && work_unflushed >= head_flush) {
                flush();
                head_flush *= 2;
                if (head_flush >= (u32)env.flush_draws) head_flush = 0;
            }
            break;
        case OP_DISPATCH: case OP_DISPATCH_INDIRECT:
            dispatch(op, pay);
            break;
        case OP_VIEWPORTS: case OP_SCISSORS: case OP_DEPTH_RANGES: {
            CmdRange r;
            memcpy(&r, pay, sizeof r);
            for (u32 i = 0; i < r.count && r.first + i < 16; i++) {
                const u8 *v = pay + sizeof r + i * (op == OP_DEPTH_RANGES ? 8 : 16);
                if (op == OP_VIEWPORTS) memcpy(st.viewport[r.first + i], v, 16);
                else if (op == OP_SCISSORS) memcpy(st.scissor[r.first + i], v, 16);
                else memcpy(st.depth_range[r.first + i], v, 8);
            }
            break;
        }
        case OP_DEPTH_BOUNDS: {
            u32 e;
            memcpy(&e, pay, 4);
            st.depth_bounds_test = e;
            memcpy(st.depth_bounds, pay + 4, 8);
            break;
        }
        case OP_STENCIL_VALUE_MASK: case OP_STENCIL_MASK: case OP_STENCIL_REF: {
            u32 v[2];
            memcpy(v, pay, 8);
            u32 *d = op == OP_STENCIL_VALUE_MASK ? st.stencil_value_mask : op == OP_STENCIL_MASK ? st.stencil_mask : st.stencil_ref;
            if (v[0] & 1) d[0] = v[1];
            if (v[0] & 2) d[1] = v[1];
            break;
        }
        case OP_BLEND_COLOR: memcpy(st.blend_color, pay, 16); break;
        case OP_POINT_SIZE: memcpy(&st.point_size, pay, 4); break;
        case OP_LINE_WIDTH: memcpy(&st.line_width, pay, 4); break;
        case OP_POLYGON_OFFSET: memcpy(st.poly_offset, pay, 12); break;
        case OP_ALPHA_REF: memcpy(&st.alpha_ref, pay, 4); break;
        case OP_SAMPLE_MASK: memcpy(&st.sample_mask, pay, 4); break;
        case OP_RASTERIZER_DISCARD: st.rasterizer_discard = pay[0]; break;
        case OP_DEPTH_CLAMP: st.depth_clamp = pay[0]; break;
        case OP_PRIMITIVE_RESTART: st.primitive_restart = pay[0]; memcpy(&st.restart_index, pay + 4, 4); break;
        case OP_PATCH_SIZE: memcpy(&st.patch_size, pay, 4); break;
        case OP_TESS_LEVELS: break;
        case OP_RENDER_ENABLE: st.render_enable = pay[0]; break;
        case OP_RENDER_TARGETS: {
            CmdRenderTargets h;
            memcpy(&h, pay, sizeof h);
            const TexRef *refs = (const TexRef *)(pay + sizeof h);
            /* the same targets set again (the game re-sets them around its own state changes): the pass goes on.
             * Ending it cost a store, a full barrier (a GPU drain) and a reload for nothing: NVN orders work at its
             * barriers, not at a target change (ref/PokeZ-ARecomp-Online keeps such passes too). NX_SAME_RT=0 ends
             * the pass as before */
            if (st.rendering && env.same_rt && h.ncolor == st.nrt && (bool)h.has_depth == st.has_depth &&
                !memcmp(refs, st.rt, st.nrt * sizeof *refs) &&
                (!h.has_depth || !memcmp(&refs[h.ncolor], &st.depth, sizeof st.depth))) {
                rt_kept++;
                break;
            }
            end_rendering();
            /* games submit a whole frame at once: hand the GPU what is recorded so far, at a pass boundary, so
             * that it works while the rest is translated (NX_FLUSH_DRAWS, 0 for one submission per guest one) */
            if (env.flush_draws && work_unflushed >= (u32)env.flush_draws) flush();
            st.nrt = h.ncolor > 8 ? 8 : h.ncolor;
            for (u32 i = 0; i < st.nrt; i++) st.rt[i] = refs[i];
            st.has_depth = h.has_depth;
            if (h.has_depth) st.depth = refs[h.ncolor];
            break;
        }
        case OP_CLEAR_COLOR: { PL(CmdClearColor); do_clear_color(p); break; }
        case OP_CLEAR_DEPTH_STENCIL: { PL(CmdClearDS); do_clear_depth_stencil(p); break; }
        case OP_CLEAR_TEXTURE: { PL(CmdClearTexture); do_clear_texture(p); break; }
        case OP_DISCARD: break;
        case OP_COPY_BUFFER_TO_TEXTURE: { PL(CmdCopyBT); do_copy_buffer_to_texture(p); break; }
        case OP_COPY_TEXTURE_TO_BUFFER: { PL(CmdCopyBT); do_copy_texture_to_buffer(p); break; }
        case OP_COPY_TEXTURE_TO_TEXTURE: { PL(CmdCopyTT); do_copy_texture_to_texture(p); break; }
        case OP_COPY_BUFFER: {
            PL(CmdCopyBuffer);
            VkBuffer sb, db;
            u64 so, dof;
            if (p->size && gpu_buffer(p->src, &sb, &so) && gpu_buffer(p->dst, &db, &dof)) {
                end_rendering();
                VkBufferCopy c = {so, dof, p->size};
                vkCmdCopyBuffer(cmd(), sb, db, 1, &c);
                VkBuffer mbuf = mirror_at(p->dst, &c.dstOffset);
                if (mbuf) vkCmdCopyBuffer(cmd(), sb, mbuf, 1, &c);
                barrier();
            }
            break;
        }
        case OP_CLEAR_BUFFER: {
            PL(CmdClearBuffer);
            VkBuffer b;
            u64 o;
            if (p->size && gpu_buffer(p->addr, &b, &o)) {
                end_rendering();
                vkCmdFillBuffer(cmd(), b, o, p->size & ~3ull, p->value);
                VkBuffer mbuf = mirror_at(p->addr, &o);
                if (mbuf) vkCmdFillBuffer(cmd(), mbuf, o, p->size & ~3ull, p->value);
                barrier();
            }
            break;
        }
        case OP_DOWNSAMPLE: { PL(CmdDownsample); do_downsample(p); break; }
        case OP_BARRIER:                /* the game's: ends the pass (with its barrier), or nothing outside one */
            if (vk.labels) { label_begin(cmd(), "nvn barrier%s", st.rendering ? "" : " (no pass)"); vkCmdEndDebugUtilsLabelEXT(cmd()); }
            end_rendering();
            break;
        case OP_FENCE_SYNC: {
            PL(CmdSync);
            Sync *s = obj_get(p->sync, T_SYNC);
            if (s) { s->value = submitted + 1; s->fenced = true; }
            break;
        }
        case OP_WAIT_SYNC: break;       /* one Vulkan queue: everything is in order already */
        case OP_SIGNAL_EVENT: {
            PL(CmdEvent);
            u32 v = p->value;
            if (p->mode == 1) {
                static bool logged;
                if (!logged) { logged = true; hlog(1, "gpu: SignalEvent with ADD: done on the CPU side"); }
                v += LD32(p->addr);
            }
            gpu_write(p->addr, &v, 4);
            break;
        }
        case OP_WAIT_EVENT: {
            /* not waited on yet: log the waits, above all those not yet satisfied when the replay reaches them
             * (mode 0 equal, 1 greater-or-equal with wrap) */
            PL(CmdEvent);
            static u32 seen, unmet;
            u32 cur = LD32(p->addr);
            bool met = p->mode == 1 ? (s32)(cur - p->value) >= 0 : cur == p->value;
            seen++;
            if (!met) unmet++;
            if (seen <= 20 || (!met && unmet <= 50))
                hlog(0, "gpu: WaitEvent %llx mode %u value %x: now %x, %s (%u waits, %u unmet)", p->addr, p->mode,
                     p->value, cur, met ? "met" : "NOT MET", seen, unmet);
            break;
        }
        case OP_REPORT_COUNTER: {
            PL(CmdReport);
            /* NVNcounterData {u64 value, u64 timestamp}: timestamps are ours, other counters report "visible" */
            /* Counters are running totals the game subtracts (samples passed after a test draw minus before). A
             * constant answer made every difference 0, "occluded": trees and other occlusion-tested objects were
             * never drawn near the camera. Each report now counts up by 0x10000, so every test passes (no occlusion
             * culling; real counts would need Vulkan occlusion queries). Timestamps stay ours. */
            static u64 running;
            running += 0x10000;
            u64 v[2] = {p->counter == 0 ? 0 : running, now_ns()};
            if (p->addr) gpu_write(p->addr, v, 16);
            static u32 seen[8];                 /* which counter types the game reads, once each, with how often */
            static u64 n_reports;
            n_reports++;
            if (p->counter < 256 && !(seen[p->counter / 32] >> (p->counter % 32) & 1)) {
                seen[p->counter / 32] |= 1u << (p->counter % 32);
                hlog(0, "gpu: the game reads GPU counter type %u (answered with %llu; %llu reports so far)",
                     p->counter, v[0], n_reports);
            }
            break;
        }
        case OP_RESET_COUNTER: break;
        case OP_REPORT_VALUE: { PL(CmdReport); if (p->addr) gpu_write(p->addr, &p->value, 4); break; }
        case OP_TEXTURE_POOL: { PL(CmdPool); st.texpool = *p; break; }
        case OP_SAMPLER_POOL: { PL(CmdPool); st.smppool = *p; break; }
        case OP_XFB: {                  /* Begin starts the buffers over; Pause and Resume keep their offsets */
            u32 kind;
            memcpy(&kind, pay, 4);
            if (kind == 0) memset(st.xfb_written, 0, sizeof st.xfb_written);
            st.xfb_active = kind == 0 || kind == 3;
            break;
        }
        default:
            hlog(0, "gpu: unknown command %u (length %u) at %llx", op, len, at);
            return;
        }
        at += 4 + len;
    }
}

void gpu_submit(u64 queue, u32 count, const u64 *handles) {
    (void)queue;
    double t0 = ms_now();
    EnterCriticalSection(&gpu_cs);
    mirror_scan_start();
    submit_serial++;
    tl_guest_first = true;
    head_flush = (u32)env.flush_first;
    if (env.timeline) tl_entry = t0;
    for (u32 i = 0; i < count; i++)
        if (handles[i]) run(handles[i], 0);
    flush();
    static u64 submits;
    if (++submits == 1) hlog(1, "gpu: first submission");
    t_submit_ms += ms_now() - t0;
    hf.submit += ms_now() - t0;
    LeaveCriticalSection(&gpu_cs);
}

void gpu_flush(u64 queue) {
    (void)queue;
    EnterCriticalSection(&gpu_cs);
    flush();
    LeaveCriticalSection(&gpu_cs);
}

void gpu_finish(u64 queue) {
    EnterCriticalSection(&gpu_cs);
    flush();
    u64 v = submitted;
    LeaveCriticalSection(&gpu_cs);
    (void)queue;
    gpu_wait(v, UINT64_MAX);
}

u64 gpu_fence(u64 queue) {
    (void)queue;
    EnterCriticalSection(&gpu_cs);
    flush();
    u64 v = submitted;
    LeaveCriticalSection(&gpu_cs);
    return v;
}

bool gpu_wait(u64 value, u64 timeout_ns) {
    if (value > submitted) {
        gpu_flush(0);
        /* still not submitted: nothing was recorded after the fence (a command buffer's FenceSync takes submitted + 1,
         * and a submission that records no Vulkan work submits nothing), so the value would never be signalled and
         * the wait lasted its whole timeout. Everything before it is what there is to wait for */
        if (value > submitted) value = submitted;
    }
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &timeline,
                              .pValues = &value};
    double t0 = prof_on > 0 ? ms_now() : 0;
    u64 back = submitted - value;
    double tw = ms_now();
    VkResult r = vkWaitSemaphores(vk.dev, &wi, timeout_ns);
    if (env.timeline) tl_woke = ms_now();
    t_wait_ms += ms_now() - tw;
    hf.wait += ms_now() - tw;
    if (prof_on > 0) {
        double ms = ms_now() - t0;
        u32 k = back == 0 ? 0 : back == 1 ? 1 : back < 10 ? 2 : 3;
        prof_guest_wait_ms += ms;
        prof_wait_ms[k] += ms;
        prof_wait_n[k]++;
        EnterCriticalSection(&gpu_cs);
        u32 i = 0;
        while (i < 15 && prof_sites[i].site && prof_sites[i].site != gpu_wait_site) i++;
        prof_sites[i].site = gpu_wait_site;
        prof_sites[i].ms += ms;
        prof_sites[i].n++;
        LeaveCriticalSection(&gpu_cs);
    }
    if (r < 0) fatal("vkWaitSemaphores: %d (device lost?)", r);
    return r == VK_SUCCESS;
}

/* ---------------------------------------------------------------------------------------------- present */
/* a new render scale, between frames: everything submitted has run (nothing recorded uses the old images, and
 * descriptor sets can be rewritten), then every scaled image and every render or storage target the game has used is
 * remade at the new scale with its contents blitted over */
static void rescale_all(u32 s) {
    vk_wait_idle();
    reap();
    u32 from = rs_scale;
    rs_scale = s;
    Image **list = malloc((nimages + 1) * sizeof *list);
    int n = 0;
    for (int b = 0; b < IMAGE_BUCKETS; b++)
        for (Image *im = buckets[b]; im; im = im->next)
            if (n < nimages && im->can_scale && (im->scale != s) && (im->scale > 1 || im->rt_frame || im->gpu_written)) {
                im->used = frame_no + 1;        /* not evicted while the others are made */
                list[n++] = im;
            }
    u64 before = image_bytes;
    for (int i = 0; i < n; i++) image_rescale(list[i], s);
    free(list);
    hlog(0, "gpu: render scale %ux -> %ux: %d images remade, %llu -> %llu MB", from, s, n, before >> 20, image_bytes >> 20);
}

u64 gpu_present(u64 queue, Window *w, u32 index) {
    (void)queue;
    double t_present0 = ms_now();
    EnterCriticalSection(&gpu_cs);
    double t_cs = ms_now();             /* the steps' times for the slow-frame line (NX_HITCH_MS) */
    refresh_cpu_textures();
    double t_refresh = ms_now();
    if (InterlockedExchange(&reload_request, 0)) {
        int n = 0;
        for (int b = 0; b < IMAGE_BUCKETS; b++)
            for (Image *ri = buckets[b]; ri; ri = ri->next)
                if (ri->valid && !ri->gpu_written && ri->size && ri->info.addr >= AS_BASE) { ri->valid = false; image_upload(ri); n++; }
        hlog(0, "gpu: F11: %d textures uploaded again from guest memory", n);
    }
    Image *im = image_get(&w->info[index]);
    if (im) image_upload(im);
    double t_flush0 = ms_now();
    flush();
    if ((u32)rs_want != rs_scale) {     /* the options panel changed it: im is remade in place */
        rescale_all((u32)rs_want);
        flush();
    }
    double t_flush = ms_now();
    ind_log_report();
    query_report();
    static u64 budget;
    if (!budget) {
        for (u32 i = 0; i < vk.mem.memoryHeapCount; i++)
            if (vk.mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT && vk.mem.memoryHeaps[i].size > budget)
                budget = vk.mem.memoryHeaps[i].size;
        budget = budget / 100 * 55;
        hlog(1, "gpu: images may use %llu MB of video memory", budget >> 20);
    }
    if (image_bytes > budget) images_evict(budget / 4 * 3, false);
    dump_frame(im, frame_no + 1);
    if (capture_frame && frame_no + 1 >= capture_frame) {
        capture_frame = 0;
        InterlockedExchange64(&capture_done_ms, (LONG64)GetTickCount64());
    }
    if (InterlockedExchange(&capture_request, 0)) {
        capture_frame = frame_no + 2;
        hlog(0, "gpu: F12: frame %llu is traced and dumped to %s", capture_frame, frames_dir());
    }
    u64 frames = ++frame_no;
    if (frames % 600 == 0) hlog(1, "gpu: %llu draws so far (%llu skipped), %llu dispatches, %d images (%llu MB), %u pipelines, "
                                  "%llu passes kept over a re-set of the same targets",
                                  draws, draws_skipped, dispatches, nimages, image_bytes >> 20, npipes, rt_kept);
    if (frames % 600 == 0) {
        hlog(1, "gpu: CPU-written texture check %.2f ms/frame over %.0f MB; its thread %.2f ms/frame (longest %.1f), "
                "%llu presents did not wait for it", refresh_ms / 600, refresh_mb / 600, scan_ms / 600, scan_ms_max,
             scan_skips);
        scan_ms = scan_ms_max = 0;
        scan_skips = 0;
        if (nmirrors) hlog(1, "gpu: pool mirrors: %.2f MB/frame copied in, render thread %.2f ms/frame on them", mirror_mb / 600, mirror_ms / 600);
        mirror_mb = mirror_ms = 0;
        static u64 dropped0;
        u64 dropped = vk_presents_dropped();
        hlog(1, "gpu: per frame: translating and submitting %.2f ms, presenting %.2f ms, waiting for the GPU %.2f ms; %llu of 600 not shown",
             t_submit_ms / 600, t_present_ms / 600, t_wait_ms / 600, dropped - dropped0);
        dropped0 = dropped;
        refresh_ms = refresh_mb = t_submit_ms = t_present_ms = t_wait_ms = 0;
    }
    for (int i = 0; frames % 600 == 0 && i < nskip_why; i++) hlog(1, "gpu:   %llu skipped: %s", skip_count[i], skip_why[i]);
    if (prof_on > 0 && frames % 300 == 0) prof_report(300);
    if (env.timeline && frames % 600 == 0) tl_report(600);
    s32 cw = w->crop[2] > 0 ? w->crop[2] : (s32)w->info[index].width;
    s32 ch = w->crop[3] > 0 ? w->crop[3] : (s32)w->info[index].height;
    /* lower-left origin: row 0 is the bottom of the picture */
    u64 v = submitted;
    double t_vk = ms_now();
    s32 S = im ? (s32)im->scale : 1;    /* the crop is in the game's pixels */
    vk_present(im ? im->image : NULL, im ? im->format : VK_FORMAT_UNDEFINED, im ? lvl_w(im, 0) : 0, im ? lvl_h(im, 0) : 0,
               w->crop[0] * S, w->crop[1] * S, cw * S, ch * S, window_origin == 0, timeline, v);
    double t_end = ms_now();
    t_present_ms += t_end - t_present0;
    hf.present += t_end - t_present0;
    static double last_end;
    double frame = last_end ? t_end - last_end : 0;
    if (env.hitch_ms > 0 && frame > env.hitch_ms)
        hlog(0, "gpu: slow frame %llu: %.1f ms = translating %.1f + presenting %.1f + waiting for the GPU %.1f + the rest "
                "(game code, waits for a core) %.1f; in them: shaders %.1f ms (%u), JIT waits %.1f ms (%u), pipelines "
                "%.1f ms (%u), images %.1f ms (%u), uploads %.1f ms (%u), slot waits %.1f ms, pool mirrors %.1f ms",
             frames, frame, hf.submit, hf.present, hf.wait, frame - hf.submit - hf.present - hf.wait, hf.shader,
             hf.nshader, hf.jit, hf.njit, hf.pipe, hf.npipe, hf.image, hf.nimage, hf.upload, hf.nupload, hf.slot,
             hf.mirror);
    if (env.hitch_ms > 0 && frame > env.hitch_ms) {
        /* this present's steps (the frame's last one; earlier presents in it are in hf.present only) */
        const VkPresentTimes *pt = &vk_present_times;
        hlog(0, "gpu:   presenting: gpu_cs wait %.2f, texture check %.2f, before the flush %.2f, flush %.2f, "
                "before vk_present %.2f, vk_present %.2f (sw_lock %.2f, last present %.2f, fence %.2f, acquire %.2f, "
                "submit %.2f) ms", t_cs - t_present0, t_refresh - t_cs, t_flush0 - t_refresh, t_flush - t_flush0,
             t_vk - t_flush, t_end - t_vk, pt->lock, pt->present, pt->fence, pt->acquire, pt->submit);
    }
    memset(&vk_present_times, 0, sizeof vk_present_times);
    if (env.hitch_ms > 0) {
        /* which guest threads ran (or waited for a core) in a slow frame */
        char busy[512];
        bool slow = frame > env.hitch_ms;
        kernel_frame_threads(slow, busy, sizeof busy);
        if (slow) hlog(0, "gpu:   threads this frame: %s", busy);
    }
    /* the profile's slow-frame samples: not the frames around a 600-frame report (it runs on this thread) */
    profile_frame_end(env.hitch_ms > 0 && frame > env.hitch_ms && frames % 600 > 1);
    if (env.hitch_ms > 0) {
        /* with NX_PROFILE=<thread>: that thread's waits in this frame (the longest names where it blocked) */
        double blocked, longest;
        int waits;
        char where[1024];
        if (waitprof_frame(&blocked, &waits, &longest, where, sizeof where) && frame > env.hitch_ms)
            hlog(0, "gpu:   profiled thread: blocked %.1f ms in %d waits, longest %.1f ms: %s", blocked, waits, longest, where);
    }
    memset(&hf, 0, sizeof hf);
    last_end = t_end;
    LeaveCriticalSection(&gpu_cs);
    return v;
}

void gpu_window_created(Window *w) { (void)w; }
u64 gpu_frames(void) { return frame_no; }
void gpu_shaders_set(ShaderCode *s) { (void)s; }

static void env_init(void) {
    const char *e;
    prof_on = (e = getenv("NX_GPU_PROF")) ? atoi(e) : 0;
    env.dump_astc = getenv("NX_DUMP_ASTC") != NULL;
    env.ssbo_log = getenv("NX_SSBO_LOG") != NULL;
    env.pipe_log = getenv("NX_PIPE_LOG") != NULL;
    env.no_discard = getenv("NX_NO_DISCARD") != NULL;
    if ((e = getenv("NX_DEBUG_RT")) != NULL) {
        char *end;
        env.debug_rt = strtoull(e, &end, 16);
        env.debug_rt_bits = *end == ':' ? (u32)strtoul(end + 1, NULL, 10) : 1;
    }
    env.refresh_pools = getenv("NX_REFRESH_POOLS") != NULL;
    env.refresh_wait = getenv("NX_REFRESH_WAIT") != NULL;
    env.same_rt = !(e = getenv("NX_SAME_RT")) || atoi(e);
    /* Mirrors pay where the GPU reads host memory slowly (the GTX 1650 Ti laptop's x8 link: ~3 GB/s of pool reads
     * at 25 fps) and cost fps where it reads it fast (RX 7800 XT: 155-205 fps off vs 137-167 on). So they are on for
     * discrete GPUs on links under 20 GB/s (PCIe 4.0 x16 is 31.5) or whose link is not known; integrated GPUs read
     * host memory as their own */
    bool slow = vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && (!vk.pcie_gbps || vk.pcie_gbps < 20);
    env.pool_mirror = (e = getenv("NX_POOL_MIRROR")) ? atoi(e) != 0 : slow;
    hlog(1, "gpu: pool mirrors %s%s", env.pool_mirror ? "on" : "off", e ? " (NX_POOL_MIRROR)" : "");
    env.shader_jit = !(e = getenv("NX_SHADER_JIT")) || atoi(e);
    /* draws skip shaders still being translated instead of waiting (see shader_load): on in netplay builds, where a
     * stalled game drops out of local play; off elsewhere, since a skipped draw into a texture drawn only once stays
     * missing */
#ifdef NX_NETPLAY
    env.async_shaders = (e = getenv("NX_ASYNC_SHADERS")) ? atoi(e) != 0 : true;
#else
    env.async_shaders = (e = getenv("NX_ASYNC_SHADERS")) ? atoi(e) != 0 : false;
#endif
    hlog(1, "gpu: draws %s for shaders being translated%s", env.async_shaders ? "do not wait" : "wait",
         e ? " (NX_ASYNC_SHADERS)" : "");
    env.gpu_skip = (e = getenv("NX_GPU_SKIP")) ? atoi(e) : 0;
    env.flush_draws = (e = getenv("NX_FLUSH_DRAWS")) ? atoi(e) : 250;
    env.flush_first = (e = getenv("NX_FLUSH_FIRST")) ? atoi(e) : 32;
    env.hitch_ms = (e = getenv("NX_HITCH_MS")) ? atof(e) : 0;
    env.feedback_log = getenv("NX_FEEDBACK_LOG") != NULL;
    env.view_log = getenv("NX_VIEW_LOG") != NULL;
    env.mutable_all = getenv("NX_MUTABLE_ALL") != NULL;
    env.full_barriers = getenv("NX_FULL_BARRIERS") != NULL;
    env.timeline = getenv("NX_GPU_TIMELINE") != NULL;
    e = getenv("NX_TRACE");
    env.trace_lo = e ? atol(e) : -1;
    env.trace_hi = e && strchr(e, '-') ? atol(strchr(e, '-') + 1) : env.trace_lo;
    env.watch = (e = getenv("NX_WATCH")) ? strtoull(e, NULL, 16) : 0;
    env.dump_draws = (e = getenv("NX_DUMP_DRAWS")) ? strtoull(e, NULL, 16) : 0;
    for (int k = 0; e && *e && k < 4; k++) {
        char *end;
        env.dump_draws_list[k] = strtoull(e, &end, 16);
        e = *end == ',' ? end + 1 : end + strlen(end);
    }
    env.dump_ssbo = (e = getenv("NX_DUMP_SSBO")) ? strtoull(e, NULL, 16) : 0;
    env.dump_tex = (e = getenv("NX_DUMP_TEX")) ? strtoull(e, NULL, 16) : 0;
    env.skip_shaders = getenv("NX_SKIP_SHADERS");
    env.trace_ubo = getenv("NX_TRACE_UBO");
    env.trace_fubo = getenv("NX_TRACE_FUBO");
    env.trace_vcb = getenv("NX_TRACE_VCB");
    env.trace_filter = (e = getenv("NX_TRACE_FILTER")) ? e : "";
    if ((e = getenv("NX_DUMP")) != NULL) snprintf(env.dump, sizeof env.dump, ",%s,", e);
}

void gpu_init(void) {
    InitializeCriticalSection(&gpu_cs);
    env_init();
    VkSemaphoreTypeCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                     .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci};
    VKCHECK(vkCreateSemaphore(vk.dev, &sci, NULL, &timeline));
    for (int i = 0; i < SLOTS; i++) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = vk.family,
                                       .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT};
        VKCHECK(vkCreateCommandPool(vk.dev, &pci, NULL, &slots[i].pool));
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = slots[i].pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 2};
        VkCommandBuffer two[2];
        VKCHECK(vkAllocateCommandBuffers(vk.dev, &ai, two));
        slots[i].cmd = two[0];
        slots[i].pre = two[1];
        vk_buffer(STAGING_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &slots[i].staging);
    }
    state_reset(&st);
    layouts_init();
    dummies_init();
    empty_tex = alloc_tex_set();
    empty_smp = alloc_set(set_smp, SMPPOOL_MAX);
    empty_img = alloc_set(set_img, TEXPOOL_MAX);
    descset_fill(empty_tex, false, TEXPOOL_MAX);
    descset_fill(empty_smp, true, SMPPOOL_MAX);
    _beginthreadex(NULL, 0, pcache_thread, NULL, 0, NULL);
    char spv[MAX_PATH + 32];
    spv_path(spv, sizeof spv, 0);               /* sets spv_dir before the prebuild threads read it */
    if (vk.pipeline_binary) bins_load();
    pipelines_prebuild();
    gpu_ready = true;
    hlog(1, "gpu: ready");
}
