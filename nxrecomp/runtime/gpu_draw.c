/* Draws and dispatches: the GPU state carried between command buffers, render passes (begin_rendering), NVN state
 * as Vulkan dynamic state, vertex input, uniform buffers, transform feedback, queries and the debug switches around
 * them. Part of the NVN executor (gpu.c, gpu_int.h). */
#include "gpu_int.h"

State st;                               /* the GPU's state carries over between command buffers, as on NVN */
u64 draws, draws_skipped, dispatches;
u64 rt_kept;                            /* render-target sets that kept the pass (the same targets; NX_SAME_RT) */

void state_reset(State *s) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < 16; i++) {
        s->scissor[i][2] = s->scissor[i][3] = 0x10000;
        s->depth_range[i][1] = 1;
    }
    s->mask.mask = ~0u;
    s->line_width = s->point_size = 1;
    s->sample_mask = ~0u;
    s->render_enable = true;
    s->stencil_value_mask[0] = s->stencil_value_mask[1] = 0xff;
    s->stencil_mask[0] = s->stencil_mask[1] = 0xff;
}
/* the captures since the last barrier one by one (st.xfb_dirty is their hull per buffer, the quick test): Violet's
 * skinning draws capture into scattered places of one pool, and the hull made each look like it overwrote the
 * others (26 passes a frame ended for that). Full: the hull decides */
#define XFB_RANGES 1024
static u64 xfb_ranges[XFB_RANGES][2];
static u32 nxfb_ranges;

PendingWrite pending[1024];
u32 npending;

void end_rendering_at(const char *at) {
    if (!st.rendering) return;
    if (prof_on > 0)
        for (u32 i = 0; i < 64; i++)
            if (!prof_ends[i].at || !strcmp(prof_ends[i].at, at)) { prof_ends[i].at = at; prof_ends[i].count++; break; }
    label_draw_end();
    vkCmdEndRendering(cmd());
    if (vk.labels) {                    /* the pass's region, then who ended it */
        vkCmdEndDebugUtilsLabelEXT(cmd());
        label_begin(cmd(), "end pass %s", at);
        vkCmdEndDebugUtilsLabelEXT(cmd());
    }
    st.rendering = false;
    prof_mark("outside passes (copies, clears, barriers)");
    bool full = pass_att_writes || env.full_barriers;
    if (npending) {
        barrier();                      /* after the draws before them */
        for (u32 i = 0; i < npending; i++) {
            vkCmdUpdateBuffer(cmd(), pending[i].buf, pending[i].off, pending[i].size, pending[i].data);
            if (pending[i].mbuf) vkCmdUpdateBuffer(cmd(), pending[i].mbuf, pending[i].moff, pending[i].size, pending[i].data);
        }
        npending = 0;
        full = true;                    /* and the updates (transfer writes) */
    }
    barrier_at(GPU_AT, full ? VK_ACCESS_2_MEMORY_WRITE_BIT : ACCESS_NO_ATTACHMENTS);
    st.xfb_captured = false;
    memset(st.xfb_dirty, 0, sizeof st.xfb_dirty);
    nxfb_ranges = 0;
}

/* NX_WATCH=<hex address>: log every GPU write to that texture storage */
void watch(const char *what, const TexRef *r) {
    if (!env.watch || r->t.addr != env.watch) return;
    hlog(0, "watch: frame %llu %s %ux%ux%u f%u target %u view level %u layers %u+%u", frame_no + 1, what, r->t.width,
         r->t.height, r->t.depth, r->v.format, r->t.target, r->v.base_level, r->v.min_layer, r->v.num_layers);
}

/* a render target's view, its image, level, first layer and layer count (layered rendering: gl_Layer picks among
 * them) */
static VkImageView rt_view(const TexRef *r, Image **out, u32 *level, u32 *first, u32 *layers, bool promote) {
    watch("render target", r);
    ViewInfo v;
    rs_hint = true;                     /* made now: at the render scale */
    Image *im = image_ref(r, &v);
    rs_hint = false;
    if (im && promote) image_promote(im);                       /* drawn with a scaled depth (image_promote) */
    if (im && im->can_scale && im->scale != rs_scale) image_rescale(im, rs_scale);   /* a texture first drawn into */
    if (out) *out = im;
    if (level) *level = v.base_level;
    if (!im) return NULL;
    u32 base, n = view_layers(im, &v, VIEW_ATTACHMENT, &base);
    if (layers) *layers = n;
    if (first) *first = base;
    return image_view(im, &v, VIEW_ATTACHMENT);
}

/* the pass's scale from its targets' (they should agree: every screen-shaped target is scaled) */
static void rt_scale_of(const Image *im) {
    static const Image *first;
    if (!st.rt_scale) { st.rt_scale = im->scale; first = im; return; }
    if (st.rt_scale == im->scale) return;
    static int warned;
    if (warned++ < 2)
        for (int k = 0; k < 2; k++) {
            const Image *x = k ? im : first;
            hlog(0, "gpu: render targets at scales %u and %u in one pass: %ux%ux%u f%u (vk %d) target %u flags %x "
                    "samples %u levels %u at %llx, scale %u%s", st.rt_scale, im->scale, x->info.width,
                 tex_height(&x->info), x->layers, x->info.format, x->format, x->info.target, x->info.flags,
                 x->info.samples, x->levels, x->info.addr, x->scale, x->can_scale ? "" : ", not a candidate");
        }
    /* the smaller: Violet draws small square targets (360x360) with its 1080p depth still bound, and the pass area
     * is the smallest target's */
    if (im->scale < st.rt_scale) st.rt_scale = im->scale;
}

bool begin_rendering(void) {
    if (st.rendering) return true;
    VkRenderingAttachmentInfo color[8], depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    u32 w = ~0u, h = ~0u, layers = ~0u;
    memset(st.rt_format, 0, sizeof st.rt_format);
    memset(st.rt_sub, 0, sizeof st.rt_sub);
    st.depth_format = st.stencil_format = VK_FORMAT_UNDEFINED;
    st.samples = 1;
    st.rt_scale = 0;
    bool any = false;
    /* a scaled screen depth bound: the colour targets join its scale (image_promote) */
    bool promote = false;
    if (st.has_depth && rs_scale > 1) {
        ViewInfo dv;
        rs_hint = true;
        Image *d = image_ref(&st.depth, &dv);
        rs_hint = false;
        promote = d && d->can_scale && !(d->aspects & VK_IMAGE_ASPECT_COLOR_BIT);
    }
    for (u32 i = 0; i < st.nrt; i++) {
        color[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        Image *im;
        u32 l, l0, nl;
        VkImageView v = st.rt[i].t.addr ? rt_view(&st.rt[i], &im, &l, &l0, &nl, promote) : NULL;
        if (!v) continue;
        st.rt_sub[i].addr = im->info.addr, st.rt_sub[i].size = im->size;
        st.rt_sub[i].level = l, st.rt_sub[i].layer = l0, st.rt_sub[i].nlayers = nl;
        color[i].imageView = v;
        st.rt_format[i] = usable_format(st.rt[i].v.format) != VK_FORMAT_UNDEFINED ? usable_format(st.rt[i].v.format) : im->format;
        st.samples = im->info.samples > 1 ? im->info.samples : 1;
        color[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        color[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        u32 iw = lvl_w(im, l), ih = lvl_h(im, l);
        if (iw < w) w = iw;
        if (ih < h) h = ih;
        rt_scale_of(im);
        if (nl < layers) layers = nl;
        im->valid = true;
        im->rt_frame = frame_no + 1;
        im->gpu_written = true;
        linear_wrote(im);
        any = true;
    }
    bool has_d = false, has_s = false;
    if (st.has_depth) {
        Image *im;
        u32 l, l0, nl;
        VkImageView v = rt_view(&st.depth, &im, &l, &l0, &nl, false);
        if (v) {
            st.rt_sub[8].addr = im->info.addr, st.rt_sub[8].size = im->size;
            st.rt_sub[8].level = l, st.rt_sub[8].layer = l0, st.rt_sub[8].nlayers = nl;
            depth.imageView = v;
            depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            has_d = im->aspects & VK_IMAGE_ASPECT_DEPTH_BIT;
            has_s = im->aspects & VK_IMAGE_ASPECT_STENCIL_BIT;
            if (has_d) st.depth_format = im->format;
            if (has_s) st.stencil_format = im->format;
            st.samples = im->info.samples > 1 ? im->info.samples : 1;
            u32 iw = lvl_w(im, l), ih = lvl_h(im, l);
            if (iw < w) w = iw;
            if (ih < h) h = ih;
            rt_scale_of(im);
            if (nl < layers) layers = nl;
            im->valid = true;
            im->rt_frame = frame_no + 1;
            im->gpu_written = true;
            any = true;
        }
    }
    if (!any) return false;
    if (!st.rt_scale) st.rt_scale = 1;
    st.area = (VkRect2D){{0, 0}, {w, h}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = st.area, .layerCount = layers,
                          .colorAttachmentCount = st.nrt, .pColorAttachments = color,
                          .pDepthAttachment = has_d ? &depth : NULL, .pStencilAttachment = has_s ? &depth : NULL};
    if (prof_on > 0) {                  /* the pass by its size and formats */
        char lb[80];
        int n = snprintf(lb, sizeof lb, "pass %ux%u", w, h);
        if (layers > 1) n += snprintf(lb + n, sizeof lb - n, "x%u", layers);
        for (u32 i = 0; i < st.nrt && n < 60; i++)
            if (st.rt_format[i]) n += snprintf(lb + n, sizeof lb - n, " c%u:%d", i, st.rt_format[i]);
        if (has_d || has_s) snprintf(lb + n, sizeof lb - n, " ds:%d", has_d ? st.depth_format : st.stencil_format);
        prof_mark(lb);
    }
    if (vk.labels) {                    /* the pass by its size, formats and the first target's address */
        char lb[128];
        int n = snprintf(lb, sizeof lb, "pass %ux%u", w, h);
        if (layers > 1) n += snprintf(lb + n, sizeof lb - n, "x%u", layers);
        for (u32 i = 0; i < st.nrt && n < 80; i++)
            if (st.rt_format[i]) n += snprintf(lb + n, sizeof lb - n, " c%u:%d", i, st.rt_format[i]);
        if (has_d || has_s) n += snprintf(lb + n, sizeof lb - n, " ds:%d", has_d ? st.depth_format : st.stencil_format);
        u64 a = 0;
        for (u32 i = 0; i < 9 && !a; i++) a = st.rt_sub[i].addr;
        if (n < (int)sizeof lb) snprintf(lb + n, sizeof lb - n, " @%llx", a);
        label_begin(cmd(), "%s", lb);
    }
    vkCmdBeginRendering(cmd(), &ri);
    st.rendering = true;
    pass_att_writes = false;
    return true;
}
u32 window_origin, depth_mode;          /* NVN device modes (0: lower left, 0: near at -w) */

void gpu_device_modes(u32 origin, u32 depth) { window_origin = origin; depth_mode = depth; }

/* ---- NVN state -> Vulkan */
static VkBlendFactor blend_factor(u8 f) {
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 16: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 17: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 18: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 19: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 97: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 98: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 99: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 100: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp blend_op(u8 e) { return e >= 1 && e <= 5 ? (VkBlendOp)(e - 1) : VK_BLEND_OP_ADD; }
static VkCompareOp compare_op(u8 f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }
static VkStencilOp stencil_op(u8 o) { return o >= 1 && o <= 8 ? (VkStencilOp)(o - 1) : VK_STENCIL_OP_KEEP; }

static VkPrimitiveTopology topology(u32 mode) {
    switch (mode) {
    case 0: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case 1: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case 3: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case 5: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case 6: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    case 10: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY;
    case 11: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
    case 12: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY;
    case 13: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
    case 14: return VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;     /* 4, and quads after conversion */
    }
}

/* NX_TRACE=n or n-m: the frames whose draws and dispatches are logged (NX_TRACE_FILTER: only draws whose
 * trace contains that text, e.g. a texture's "128x128x1 f56") */
volatile LONG capture_request;             /* F12 (vk.c): dump and trace the next whole frame */
u64 capture_frame;
void gpu_capture_next(void) { InterlockedExchange(&capture_request, 1); }
volatile LONG64 capture_done_ms;          /* GetTickCount64 when the last capture was written */
/* vk.c's message box: what F12 is doing (NULL: nothing to say) */
const char *gpu_capture_status(void) {
    if (capture_request || capture_frame) return "CAPTURING FRAME...";
    LONG64 done = capture_done_ms;
    return done && GetTickCount64() - (u64)done < 3000 ? "FRAME CAPTURED" : NULL;
}
/* F11 (vk.c): upload every texture again from guest memory at the next present (are copies stale?) */
volatile LONG reload_request;
void gpu_reload_textures(void) { InterlockedExchange(&reload_request, 1); }

bool trace_frame(void) {
    if (capture_frame && frame_no + 1 == capture_frame) return true;
    long f = (long)frame_no + 1;
    return env.trace_lo >= 0 && f >= env.trace_lo && f <= env.trace_hi;
}

/* skipped draws by reason (the reasons are string literals), reported with the statistics every 600 frames */
const char *skip_why[16];
u64 skip_count[16];
int nskip_why;
static void skip_draw(const char *why) {
    draws_skipped++;
    for (int i = 0; i < nskip_why; i++) if (skip_why[i] == why) { skip_count[i]++; return; }
    if (nskip_why < 16) { skip_count[nskip_why] = 1; skip_why[nskip_why++] = why; }
    hlog(1, "gpu: draw skipped: %s", why);
}

/* c[0] / c[2] (driver data) depend on the storage buffers, texture and image handles (and compute's uniform
 * buffers), which most draws leave as the last draw had them: then its staging copy is used again */
typedef struct {
    u64 serial, pools_gen;
    BufferRange ssbo[16], ubo[18];
    u32 tex[32], img[8];
    VkBuffer sbuf;
    u64 soff;
} DrvCache;

/* entry id of a texture or image set holds a scaled image (its pointer only while no image died since the set
 * was used: descset_use resolved the bound entries) */
static bool desc_scaled(const DescSet *d, u32 id) {
    return d && id < d->count && d->epoch == image_epoch && d->res[id] && d->img[id] && d->img[id]->scale > 1;
}

/* the uniform buffers of one stage (push descriptors at binding base + slot; the shader says which c[i] each slot
 * holds: c[i] in slot i, c[16] in one the shader leaves free) */
static int stage_ubos(u32 stage, u32 base, const ShaderCode *sc, VkWriteDescriptorSet *w, VkDescriptorBufferInfo *bi) {
    static DrvCache cache[ST_COUNT];
    cmd();
    DescSet *ts = descset_get(&st.texpool, DS_TEX), *is = NULL;
    for (DescSet *d = descsets; d; d = d->next)
        if (d->addr == st.texpool.addr && d->kind == DS_IMG) is = d;
    u32 tex[32], img[8];
    for (u32 k = 0; k < 32; k++) {
        u32 h = (u32)st.tex[stage][k], id = h & 0xfffff;
        if (ts && id < ts->count && ts->intv[id]) h |= HANDLE_INT;
        if (h && desc_scaled(ts, id)) h |= HANDLE_SCALED;
        tex[k] = h;
    }
    for (u32 k = 0; k < 8; k++) {
        u32 h = (u32)st.img[stage][k], id = h & 0xfffff;
        if (h && desc_scaled(is, id)) h |= HANDLE_SCALED;
        img[k] = h;
    }
    DrvCache *cc = &cache[stage % ST_COUNT];
    /* compared in place (twice a draw, ~2,000 draws a frame): the key is built only when it changed */
    bool hit = !env.ssbo_log && cc->serial == slot_serial && cc->pools_gen == pools_gen &&
               !memcmp(cc->ssbo, st.ssbo[stage], sizeof cc->ssbo) && !memcmp(cc->tex, tex, sizeof tex) &&
               (stage != ST_COMPUTE || !memcmp(cc->ubo, st.ubo[stage], sizeof cc->ubo));
    for (u32 k = 0; hit && k < 8; k++) hit = cc->img[k] == img[k];
    if (!hit) {
        DrvCache key;
        memset(&key, 0, sizeof key);
        key.serial = slot_serial;
        key.pools_gen = pools_gen;
        memcpy(key.ssbo, st.ssbo[stage], sizeof key.ssbo);
        if (stage == ST_COMPUTE) memcpy(key.ubo, st.ubo[stage], sizeof key.ubo);
        memcpy(key.tex, tex, sizeof tex);
        memcpy(key.img, img, sizeof img);
        key.sbuf = NULL;
        key.soff = 0;
        u8 *drv = staging(0x800, &key.sbuf, &key.soff);    /* c[0] at 0, c[2] at 0x600 */
        memset(drv, 0, 0x800);
        /* c[0]: buffer tables with device addresses {lo, hi, size}. Graphics: storage buffers at 0x110.
         * Compute (no cbuf slots for the game's uniform buffers there): uniform buffers at 0x220, storage at 0x310 */
        u32 ssbo_at = stage == ST_COMPUTE ? 0x310 : 0x110;
        for (u32 k = 0; k < 16 + 14; k++) {
            const BufferRange *r = k < 16 ? &st.ssbo[stage][k] : &st.ubo[stage][k - 16];
            if (k >= 16 && stage != ST_COMPUTE) break;
            PoolEnt *p = r->addr ? pool_find(r->addr) : NULL;
            if (r->addr && (!p || !p->hb)) {
                static u64 warned[64];
                u64 wkey = (sc ? sc->hash : 0) ^ k;
                int seen = 0;
                for (int j = 0; j < 64 && warned[j] && !seen; j++) seen = warned[j] == wkey;
                for (int j = 0; j < 64 && !seen; j++) if (!warned[j]) { warned[j] = wkey; break; }
                if (!seen)
                    hlog(0, "gpu: shader %016llx %s %u at %llx (size %llx) is in no host-mapped pool: left 0",
                         sc ? sc->hash : 0, k < 16 ? "ssbo" : "ubo", k < 16 ? k : k - 16, r->addr, (u64)r->size);
            }
            if (!p || !p->hb) continue;
            u64 dev = p->hb->addr + (r->addr - p->gpu);
            if (env.ssbo_log && sc && stage == ST_COMPUTE)
                hlog(0, "gpu: cs %016llx %s %u: %llx size %llx -> dev %llx", sc->hash, k < 16 ? "ssbo" : "ubo",
                     k < 16 ? k : k - 16, r->addr, (u64)r->size, dev);
            u32 v[4] = {(u32)dev, (u32)(dev >> 32), (u32)r->size, 0};
            memcpy(drv + (k < 16 ? ssbo_at + 16 * k : 0x220 + 16 * (k - 16)), v, 16);
        }
        for (u32 k = 0; k < 32; k++) memcpy(drv + 0x600 + 0x20 + 8 * k, &key.tex[k], 4);
        for (u32 k = 0; k < 8; k++) memcpy(drv + 0x600 + 0x120 + 8 * k, &key.img[k], 4);   /* image handles at 0x120 */
        *cc = key;
    }
    VkBuffer sbuf = cc->sbuf;
    u64 soff = cc->soff;
    u32 used = sc && sc->slots_used ? sc->slots_used : 0xffff, n = 0;   /* only the slots the shader declares */
    for (u32 slot = 0; slot < 16; slot++) {
        if (!(used >> slot & 1)) continue;
        u32 i = sc ? sc->cb_of_slot[slot] : slot;
        VkDescriptorBufferInfo *b = &bi[n];
        *b = (VkDescriptorBufferInfo){NULL, 0, VK_WHOLE_SIZE};
        if (i == 0) *b = (VkDescriptorBufferInfo){sbuf, soff, 0x600};
        else if (i == 2) *b = (VkDescriptorBufferInfo){sbuf, soff + 0x600, 0x200};
        else {
            u64 addr = 0, size = 0;
            if (i == 1 && sc && sc->const_size) { addr = sc->const_addr; size = (sc->const_size + 15) & ~15u; }
            else if (i >= 3) { addr = st.ubo[stage][i - 3].addr; size = st.ubo[stage][i - 3].size; }
            VkBuffer buf;
            u64 off;
            bool found = addr && size && gpu_buffer_ro(addr, size > 65536 ? 65536 : size, &buf, &off);
            if (found && !(off & 63)) {
                if (size > 65536) size = 65536;
                *b = (VkDescriptorBufferInfo){buf, off, size};
            } else if (trace_frame())
                hlog(0, "gpu:   stage %u c[%u] at %llx size %llx left unbound: %s", stage, i, addr, size,
                     !addr || !size ? "none set" : !found ? "in no buffer" : "offset not 64-byte aligned");
        }
        w[n++] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = base + slot, .descriptorCount = 1,
                                        .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = b};
    }
    return (int)n;
}
struct Recorded rec = {~0ull};

/* forget what was recorded once a new command buffer is being built (its state is undefined) */
static void rec_check(void) {
    if (rec.serial == slot_serial) return;
    memset(&rec, 0, sizeof rec);
    rec.serial = slot_serial;
}

static bool debug_rt_hit(void) { return env.debug_rt && st.nrt && st.rt[0].t.addr == env.debug_rt; }

static void set_dynamic_state(VkCommandBuffer c, u32 mode) {
    DynState d;
    memset(&d, 0, sizeof d);
    float S = (float)(st.rt_scale ? st.rt_scale : 1);  /* the viewport is in the game's pixels */
    d.vp = (VkViewport){st.viewport[0][0] * S, st.viewport[0][1] * S, st.viewport[0][2] * S, st.viewport[0][3] * S,
                        st.depth_range[0][0], st.depth_range[0][1]};
    if (d.vp.width <= 0 || d.vp.height == 0) {
        d.vp.x = 0; d.vp.y = 0;
        d.vp.width = (float)st.area.extent.width;
        d.vp.height = (float)st.area.extent.height;
    }
    if (window_origin == 1) {
        /* NVN's upper-left origin puts NDC y = +1 at row 0, Vulkan puts -1 there: flip, so that every render
         * target holds rows exactly as on the console (scissors and gl_FragCoord already count from row 0) */
        d.vp.y += d.vp.height;
        d.vp.height = -d.vp.height;
    }
    d.sc = clear_rect();
    if (!d.sc.extent.width || !d.sc.extent.height) d.sc = (VkRect2D){{0, 0}, {1, 1}};
    d.line_width = st.line_width > 0 ? st.line_width : 1;
    d.bias[0] = st.poly_offset[1]; d.bias[1] = st.poly_offset[2]; d.bias[2] = st.poly_offset[0];
    memcpy(d.blend, st.blend_color, sizeof d.blend);
    d.bounds[0] = st.depth_bounds[0];
    d.bounds[1] = st.depth_bounds[1] > st.depth_bounds[0] ? st.depth_bounds[1] : 1;
    for (int f = 0; f < 2; f++) {
        d.cmp_mask[f] = st.stencil_value_mask[f];
        d.wr_mask[f] = st.stencil_mask[f];
        d.ref[f] = st.stencil_ref[f];
    }
    d.cull = st.poly.cull & 3;
    /* the viewport flip for the upper-left origin mirrors the winding too */
    d.front = (st.poly.front_face != 0) != (window_origin == 1) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    d.topo = topology(mode);
    d.dtest = st.ds.flags & 1;
    if (debug_rt_hit() && env.debug_rt_bits & 2) d.dtest = 0;
    d.dwrite = (st.ds.flags & 2) != 0;
    d.dcmp = compare_op(st.ds.depth_func);
    d.dbtest = st.depth_bounds_test;
    d.stest = (st.ds.flags & 4) != 0;
    d.sop[0][0] = stencil_op(st.ds.fail & 15); d.sop[0][1] = stencil_op(st.ds.zpass & 15);
    d.sop[0][2] = stencil_op(st.ds.zfail & 15); d.sop[0][3] = compare_op(st.ds.stencil_func & 15);
    d.sop[1][0] = stencil_op(st.ds.fail >> 4); d.sop[1][1] = stencil_op(st.ds.zpass >> 4);
    d.sop[1][2] = stencil_op(st.ds.zfail >> 4); d.sop[1][3] = compare_op(st.ds.stencil_func >> 4);
    d.discard = st.rasterizer_discard;
    d.bias_en = (st.poly.offset_enables & (mode == 0 ? 1 : mode <= 3 ? 2 : 4)) != 0;
    d.restart = st.primitive_restart && (mode == 3 || mode == 5 || mode == 6);
    d.poly_mode = st.poly.mode == 0 ? VK_POLYGON_MODE_POINT : st.poly.mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    d.samples = st.samples;
    d.sample_mask = st.sample_mask;
    /* single-sampled: no alpha to coverage (AMD dithers it across pixels into a dot pattern; Maxwell keeps or drops
     * the pixel): draw() tests alpha >= 0.5 in the shader instead */
    d.a2c = st.ms.a2c && st.samples > 1;
    d.logic_en = st.color.logic_op != 3;
    d.logic_op = st.color.logic_op & 15;
    d.clamp = st.depth_clamp;
    if (debug_rt_hit() && env.debug_rt_bits & 4) d.clamp = 1;
    d.nrt = st.nrt;
    for (u32 i = 0; i < st.nrt; i++) {
        const BlendState *b = &st.blend[i];
        d.en[i] = st.color.blend_enable >> i & 1;
        d.eq[i] = (VkColorBlendEquationEXT){blend_factor(b->src_rgb), blend_factor(b->dst_rgb), blend_op(b->eq & 15),
                                            blend_factor(b->src_a), blend_factor(b->dst_a), blend_op(b->eq >> 4)};
        d.wm[i] = st.mask.mask >> (4 * i) & 0xf;
    }
    rec_check();
    bool all = !rec.dyn_ok;
    rec.dyn_ok = true;
    const DynState *o = &rec.d;
#define CH(f) (all || memcmp(&d.f, &o->f, sizeof d.f))
    if (CH(vp)) vkCmdSetViewportWithCount(c, 1, &d.vp);
    if (CH(sc)) vkCmdSetScissorWithCount(c, 1, &d.sc);
    if (CH(line_width)) vkCmdSetLineWidth(c, d.line_width);
    if (CH(bias)) vkCmdSetDepthBias(c, d.bias[0], d.bias[1], d.bias[2]);
    if (CH(blend)) vkCmdSetBlendConstants(c, d.blend);
    if (CH(bounds)) vkCmdSetDepthBounds(c, d.bounds[0], d.bounds[1]);
    for (int f = 0; f < 2; f++) {
        VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        if (all || d.cmp_mask[f] != o->cmp_mask[f]) vkCmdSetStencilCompareMask(c, face, d.cmp_mask[f]);
        if (all || d.wr_mask[f] != o->wr_mask[f]) vkCmdSetStencilWriteMask(c, face, d.wr_mask[f]);
        if (all || d.ref[f] != o->ref[f]) vkCmdSetStencilReference(c, face, d.ref[f]);
        if (CH(sop[f])) vkCmdSetStencilOp(c, face, d.sop[f][0], d.sop[f][1], d.sop[f][2], d.sop[f][3]);
    }
    if (CH(cull)) vkCmdSetCullMode(c, d.cull);
    if (CH(front)) vkCmdSetFrontFace(c, d.front);
    if (CH(topo)) vkCmdSetPrimitiveTopology(c, d.topo);
    if (CH(dtest)) vkCmdSetDepthTestEnable(c, d.dtest);
    if (CH(dwrite)) vkCmdSetDepthWriteEnable(c, d.dwrite);
    if (CH(dcmp)) vkCmdSetDepthCompareOp(c, d.dcmp);
    if (CH(dbtest)) vkCmdSetDepthBoundsTestEnable(c, d.dbtest);
    if (CH(stest)) vkCmdSetStencilTestEnable(c, d.stest);
    if (CH(discard)) vkCmdSetRasterizerDiscardEnable(c, d.discard);
    if (CH(bias_en)) vkCmdSetDepthBiasEnable(c, d.bias_en);
    if (CH(restart)) vkCmdSetPrimitiveRestartEnable(c, d.restart);
    if (CH(poly_mode)) vkCmdSetPolygonModeEXT(c, d.poly_mode);
    if (CH(samples)) vkCmdSetRasterizationSamplesEXT(c, (VkSampleCountFlagBits)d.samples);
    if (CH(samples) || CH(sample_mask)) vkCmdSetSampleMaskEXT(c, (VkSampleCountFlagBits)d.samples, &d.sample_mask);
    if (CH(a2c)) vkCmdSetAlphaToCoverageEnableEXT(c, d.a2c);
    if (CH(logic_en)) vkCmdSetLogicOpEnableEXT(c, d.logic_en);
    if (vk.dyn_logic_op && CH(logic_op)) vkCmdSetLogicOpEXT(c, (VkLogicOp)d.logic_op);
    if (CH(clamp)) vkCmdSetDepthClampEnableEXT(c, d.clamp);
    if (d.nrt) {
        bool n = all || d.nrt != o->nrt;
        if (n || memcmp(d.en, o->en, d.nrt * sizeof *d.en)) vkCmdSetColorBlendEnableEXT(c, 0, d.nrt, d.en);
        if (n || memcmp(d.eq, o->eq, d.nrt * sizeof *d.eq)) vkCmdSetColorBlendEquationEXT(c, 0, d.nrt, d.eq);
        if (n || memcmp(d.wm, o->wm, d.nrt * sizeof *d.wm)) vkCmdSetColorWriteMaskEXT(c, 0, d.nrt, d.wm);
    }
#undef CH
    if (!d.nrt) {                       /* no color targets: the blend state set before stays as it was */
        d.nrt = o->nrt;
        memcpy(d.en, o->en, sizeof d.en);
        memcpy(d.eq, o->eq, sizeof d.eq);
        memcpy(d.wm, o->wm, sizeof d.wm);
    }
    rec.d = d;
}

/* the format attribute i (NVN format f) is fetched as, VK_FORMAT_UNDEFINED if none */
static VkFormat vi_format(u32 i, u32 f) {
    VkFormat vf = vk_format(f);
    if (vf == VK_FORMAT_UNDEFINED) return vf;
    /* shader inputs are all float and read as raw bits (Maxwell hands integer attributes over as integers):
     * 32-bit integer formats are fetched as the same-size float format, which keeps the bits (a UINT/SINT
     * format into a float input is undefined; AMD did not keep them and integer-packed meshes vanished).
     * 8/16-bit ones are fetched as USCALED / SSCALED (the same values, exact as floats), which the vertex
     * shader turns back into integers where the pipeline's INT_ATTRS (int_attr_mask) says so */
    switch (vf) {
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: vf = VK_FORMAT_R32_SFLOAT; break;
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: vf = VK_FORMAT_R32G32_SFLOAT; break;
    case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: vf = VK_FORMAT_R32G32B32_SFLOAT; break;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: vf = VK_FORMAT_R32G32B32A32_SFLOAT; break;
    default: {
        static u32 warned[8];      /* VkFormats are below 256 here: one bit each */
        bool small_int = (vf >= VK_FORMAT_R8_UINT && vf <= VK_FORMAT_R8G8B8A8_SINT && ((vf - VK_FORMAT_R8_UNORM) % 7 == 4 || (vf - VK_FORMAT_R8_UNORM) % 7 == 5)) ||
                         (vf >= VK_FORMAT_R16_UNORM && vf <= VK_FORMAT_R16G16B16A16_SFLOAT && ((vf - VK_FORMAT_R16_UNORM) % 7 == 4 || (vf - VK_FORMAT_R16_UNORM) % 7 == 5));
        if (!small_int) break;
        VkFormat scaled = (VkFormat)(vf - 2);          /* UINT -> USCALED, SINT -> SSCALED (int_attr_mask) */
        bool once = vf < 256 && !(warned[vf / 32] >> (vf % 32) & 1);
        if (once) warned[vf / 32] |= 1u << (vf % 32);
        if (i >= 16) {             /* INT_ATTRS holds locations 0-15 */
            if (once) hlog(0, "gpu: 8/16-bit integer vertex format %d at location %u: read as float bits", vf, i);
            break;
        }
        if (once) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(vk.phys, scaled, &fp);
            if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT))
                hlog(0, "gpu: the device cannot fetch vertex format %d (for integer format %d)", scaled, vf);
        }
        vf = scaled;
        break;
    }
    }
    return vf;
}

/* without dynamic vertex input: the vertex layout into the pipeline key (decoded by pipeline_create, gpu_pipe.c) */
void vi_key(PipeKey *k) {
    for (u32 i = 0; i < st.nattribs && i < 32; i++) {
        u32 f = VA_FORMAT(st.attribs[i]), s = VA_STREAM(st.attribs[i]);
        if (!f || s >= 31) continue;
        VkFormat vf = vi_format(i, f);
        if (vf == VK_FORMAT_UNDEFINED) continue;
        if ((u32)vf > 0xff) {
            static bool warned;
            if (!warned) warned = true, hlog(0, "gpu: vertex format %d does not fit the pipeline key: read as zeros", vf);
            continue;
        }
        k->vi[i] = VI_KEY(vf, s, VA_OFFSET(st.attribs[i]));
        if (s < st.nstreams && st.streams[s].divisor) k->vi_inst |= 1u << s;
    }
}

static void set_vertex_input(VkCommandBuffer c) {
    /* draws mostly keep the last one's attributes, streams and vertex buffers: then everything below would come to
     * what was recorded already (unless the command buffer, the pools or a mirror's bad ranges changed since) */
    static struct { u64 serial, pools_gen; u32 nattribs, nstreams; VertexAttribState attribs[32];
                    VertexStreamState streams[32]; BufferRange vbuf[32]; } last = {~0ull};
    rec_check();
    if (rec.vi_ok && !nmirrors && last.serial == slot_serial && last.pools_gen == pools_gen &&
        last.nattribs == st.nattribs && last.nstreams == st.nstreams &&
        !memcmp(last.attribs, st.attribs, st.nattribs * sizeof *st.attribs) &&
        !memcmp(last.streams, st.streams, st.nstreams * sizeof *st.streams) && !memcmp(last.vbuf, st.vbuf, sizeof st.vbuf))
        return;
    last.serial = slot_serial;
    last.pools_gen = pools_gen;
    last.nattribs = st.nattribs;
    last.nstreams = st.nstreams;
    memcpy(last.attribs, st.attribs, sizeof last.attribs);
    memcpy(last.streams, st.streams, sizeof last.streams);
    memcpy(last.vbuf, st.vbuf, sizeof last.vbuf);
    /* streams 0-30; binding 31 is the zero source for attributes the game leaves unset */
    VkVertexInputBindingDescription2EXT bind[32];
    VkVertexInputAttributeDescription2EXT attr[48];
    memset(bind, 0, sizeof bind);       /* compared whole with the last draw's (padding included) */
    memset(attr, 0, sizeof attr);
    u32 nb = 0, na = 0;
    bool used[32] = {0}, located[32] = {0};
    for (u32 i = 0; i < st.nattribs && i < 32; i++) {
        u32 f = VA_FORMAT(st.attribs[i]), s = VA_STREAM(st.attribs[i]);
        if (!f || s >= 31) continue;
        VkFormat vf = vi_format(i, f);
        if (vf == VK_FORMAT_UNDEFINED) continue;
        attr[na++] = (VkVertexInputAttributeDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                                                             NULL, i, s, vf, VA_OFFSET(st.attribs[i])};
        used[s] = located[i] = true;
    }
    for (u32 i = 0; i < 16; i++)
        if (!located[i])                /* the shader may read it: zeros from a null buffer */
            attr[na++] = (VkVertexInputAttributeDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                                                                 NULL, i, 31, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    VkBuffer bufs[32];
    VkDeviceSize offs[32], sizes[32], strides[32];
    for (u32 s = 0; s < 32; s++) {
        bufs[s] = NULL; offs[s] = 0; sizes[s] = VK_WHOLE_SIZE; strides[s] = 0;
        if (s < 31 && used[s]) {
            u32 stride = s < st.nstreams ? st.streams[s].stride : 0, div = s < st.nstreams ? st.streams[s].divisor : 0;
            strides[s] = stride;
            bind[nb++] = (VkVertexInputBindingDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, NULL,
                s, stride, div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, 1};
            u64 off;
            if (st.vbuf[s].addr && gpu_buffer_ro(st.vbuf[s].addr, st.vbuf[s].size, &bufs[s], &off)) {
                offs[s] = off;
                sizes[s] = st.vbuf[s].size ? st.vbuf[s].size : VK_WHOLE_SIZE;
            }
        }
    }
    bind[nb++] = (VkVertexInputBindingDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, NULL,
                                                       31, 0, VK_VERTEX_INPUT_RATE_VERTEX, 1};
    rec_check();
    bool all = !rec.vi_ok;
    rec.vi_ok = true;
    /* without dynamic vertex input the layout is in the pipeline (vi_key) and only the strides are set, with the
     * buffers */
    if (vk.vi_dynamic && (all || nb != rec.nb || na != rec.na || memcmp(bind, rec.bind, nb * sizeof *bind) ||
                          memcmp(attr, rec.attr, na * sizeof *attr))) {
        vkCmdSetVertexInputEXT(c, nb, bind, na, attr);
        rec.nb = nb;
        rec.na = na;
        memcpy(rec.bind, bind, nb * sizeof *bind);
        memcpy(rec.attr, attr, na * sizeof *attr);
    }
    if (all || memcmp(bufs, rec.bufs, sizeof bufs) || memcmp(offs, rec.offs, sizeof offs) ||
        memcmp(sizes, rec.sizes, sizeof sizes) || (!vk.vi_dynamic && memcmp(strides, rec.strides, sizeof strides))) {
        u32 top = 0;                    /* streams 0..top-1 in use, and 31: the zero source */
        for (u32 s = 0; s < 31; s++)
            if (used[s]) top = s + 1;
        const VkDeviceSize *str = vk.vi_dynamic ? NULL : strides;
        if (top) vkCmdBindVertexBuffers2(c, 0, top, bufs, offs, sizes, str);
        vkCmdBindVertexBuffers2(c, 31, 1, &bufs[31], &offs[31], &sizes[31], str ? &strides[31] : NULL);
        memcpy(rec.bufs, bufs, sizeof bufs);
        memcpy(rec.offs, offs, sizeof offs);
        memcpy(rec.sizes, sizes, sizeof sizes);
        memcpy(rec.strides, strides, sizeof strides);
    }
}

/* without VK_KHR_index_type_uint8 (vk.u8_indices): 8-bit indices widened to 16 bits in staging, the 8-bit restart
 * index 0xff becoming 16 bits' 0xffff where primitive restart is on (as set_dynamic_state sets it) */
static bool u8_indices_widened(const CmdDraw *d, VkBuffer *buf, u64 *off) {
    const u8 *src = d->count ? gpu_cpu(d->index_addr, d->count) : NULL;
    if (!src) return false;
    static volatile LONG widened;
    if (InterlockedIncrement(&widened) == 1) hlog(1, "gpu: 8-bit indices widened to 16 bits (no VK_KHR_index_type_uint8)");
    bool restart = st.primitive_restart && (d->mode == 3 || d->mode == 5 || d->mode == 6);
    u16 *out = staging((u64)d->count * 2, buf, off);
    for (u32 i = 0; i < d->count; i++) out[i] = restart && src[i] == 0xff ? 0xffff : src[i];
    return true;
}

/* quads as triangles: indices (0 1 2, 0 2 3) per quad */
static bool quad_indices(const CmdDraw *d, bool indexed, VkBuffer *buf, u64 *off, u32 *count) {
    u32 nq = d->count / 4;
    if (!nq) return false;
    const u8 *src = NULL;
    u32 isz = d->index_type == 0 ? 1 : d->index_type == 1 ? 2 : 4;
    if (indexed && !(src = gpu_cpu(d->index_addr, (u64)d->count * isz))) return false;
    u32 *out = staging((u64)nq * 6 * 4, buf, off);
    for (u32 q = 0; q < nq; q++) {
        u32 v[4];
        for (int j = 0; j < 4; j++) {
            u32 n = q * 4 + j;
            if (!src) v[j] = d->first + n;
            else if (isz == 1) v[j] = src[n];
            else if (isz == 2) { u16 x; memcpy(&x, src + 2 * n, 2); v[j] = x; }
            else memcpy(&v[j], src + 4 * n, 4);
        }
        u32 t[6] = {v[0], v[1], v[2], v[0], v[2], v[3]};
        memcpy(out + 6 * q, t, 24);
    }
    *count = nq * 6;
    return true;
}

/* Transform feedback (GPU skinning: point draws whose vertex outputs become later draws' vertex buffers). Captures
 * begin and end around each draw, since pipelines cannot change while one is active; each buffer continues from the
 * bytes captured so far, counted here from the draw (indirect draws are not counted). */
static bool xfb_capturing;
static u64 xfb_hazards[8];
static const char *const xfb_hazard_names[8];
static bool xfb_stored;                 /* the draw's shader stores its captures itself (no transform feedback span) */
u64 gpu_device_addr(u64 addr);
static u64 xfb_prev_end[4];             /* NX_GPU_PROF: where the last capture ended in each buffer */

static void xfb_begin(VkCommandBuffer c, u32 op, const void *payload) {
    const ShaderCode *vs = st.program ? st.program->stage[ST_VERTEX] : NULL;
    if (!st.xfb_active || !vs || !(vs->caps & SHC_XFB)) return;     /* the translation carries the layout */
    VkBuffer bufs[4];
    VkDeviceSize offs[4], sizes[4];
    u32 n = 0, used = 4;
    /* A point list drawn without indices captures vertex k at k * stride: the shader stores that itself where the
     * runtime gives it the addresses (translate.py PC_DECL_XFB), and the draw needs no transform feedback span.
     * Each span was a stall on AMD: Violet's 140-170 skinning draws a frame took 6.6 us each, 1 ms of GPU time
     * at any render scale. NX_XFB_STORE=0: always transform feedback */
    const CmdDraw *pd = op == OP_DRAW_ARRAYS ? payload : NULL;
    bool store = env.xfb_store && pd && pd->mode == 0 && pd->instances <= 1;
    u32 pcx[PC_XFB_SIZE / 4] = {0};
    for (u32 b = 0; b < 4; b++) {
        u64 off;
        bufs[b] = NULL, offs[b] = 0, sizes[b] = 0;
        if (!vs->xfb_stride[b]) continue;
        if (!st.xfb[b].addr || !gpu_buffer(st.xfb[b].addr, &bufs[b], &off)) return;
        mirror_bad(st.xfb[b].addr, st.xfb[b].size);
        u64 w = st.xfb_written[b] < st.xfb[b].size ? st.xfb_written[b] : st.xfb[b].size;
        offs[b] = off + w;
        sizes[b] = st.xfb[b].size - w;
        u64 da = store ? gpu_device_addr(st.xfb[b].addr) : 0;
        if (!da || (u64)pd->count * vs->xfb_stride[b] > sizes[b]) store = false;     /* (a capture stops at the end) */
        pcx[1 + 2 * b] = (u32)(da + w);
        pcx[2 + 2 * b] = (u32)((da + w) >> 32);
        n = b + 1;
        if (used == 4) used = b;
    }
    if (!n) return;
    for (u32 b = 0; b < n; b++)          /* a buffer the shader skips: any valid one, with nothing to write */
        if (!bufs[b]) bufs[b] = bufs[used], offs[b] = offs[used], sizes[b] = 0;
    if (prof_on > 0) {                  /* how the game lays its captures out (could runs share one Begin / End?) */
        static VkBuffer pb[4]; static u64 paddr[4]; static const ShaderCode *pvs; static u32 pn;
        static u64 cnt, same_buf, contig, same_bind, same_vs, last_frame;
        bool sb = n == pn, ct = n == pn, bd = n == pn;
        for (u32 b = 0; b < n; b++) {
            if (bufs[b] != pb[b]) sb = ct = false;
            if (vs->xfb_stride[b] && offs[b] != xfb_prev_end[b]) ct = false;
            if (st.xfb[b].addr != paddr[b]) bd = false;
        }
        static u64 stored;
        stored += store;
        cnt++; same_buf += sb; contig += ct; same_bind += bd; same_vs += vs == pvs;
        for (u32 b = 0; b < 4; b++) pb[b] = bufs[b], paddr[b] = st.xfb[b].addr;
        pvs = vs; pn = n;
        if (frame_no >= last_frame + 600) {
            hlog(0, "gpu prof: xfb draws %.1f/frame: same VkBuffers as the one before %.1f, same binding %.1f, starting "
                    "where it ended %.1f, same vertex shader %.1f, stored by the shader %.1f; buffers %u, first stride %u, size %llu",
                 cnt / 600.0, same_buf / 600.0, same_bind / 600.0, contig / 600.0, same_vs / 600.0, stored / 600.0, n,
                 vs->xfb_stride[used], (unsigned long long)st.xfb[used].size);
            char hz[200];
            int o = 0;
            hz[0] = 0;
            for (int k = 1; k < 8; k++)
                if (xfb_hazards[k]) o += snprintf(hz + o, sizeof hz - o, " %s %.1f", xfb_hazard_names[k], xfb_hazards[k] / 600.0);
            hlog(0, "gpu prof: passes ended before a draw that reads captures, per frame:%s", o ? hz : " none");
            memset(xfb_hazards, 0, sizeof xfb_hazards);
            cnt = same_buf = contig = same_bind = same_vs = stored = 0; last_frame = frame_no;
        }
    }
    if (!store) memset(pcx, 0, sizeof pcx);
    vkCmdPushConstants(c, layout_gfx, PC_STAGES, PC_SIZE, PC_XFB_SIZE, pcx);
    if (store) xfb_stored = true;
    else {
        vkCmdBindTransformFeedbackBuffersEXT(c, 0, n, bufs, offs, sizes);
        vkCmdBeginTransformFeedbackEXT(c, 0, 0, NULL, NULL);
        xfb_capturing = true;
    }
    /* vertices captured: whole primitives, as lists */
    u64 v = 0;
    if (op == OP_DRAW_ARRAYS || op == OP_DRAW_ELEMENTS) {
        const CmdDraw *d = payload;
        u64 k = d->count;
        switch (d->mode) {
        case 0: v = k; break;                                   /* points */
        case 1: v = k / 2 * 2; break;                           /* lines */
        case 3: v = k >= 2 ? (k - 1) * 2 : 0; break;            /* line strip */
        case 4: v = k / 3 * 3; break;                           /* triangles */
        case 5: case 6: v = k >= 3 ? (k - 2) * 3 : 0; break;    /* strip, fan */
        default: v = k; break;
        }
        v *= d->instances ? d->instances : 1;
    } else {
        static bool logged;
        if (!logged) { logged = true; hlog(0, "gpu: transform feedback in an indirect draw: its bytes are not counted"); }
    }
    for (u32 b = 0; b < 4; b++) {
        if (!vs->xfb_stride[b]) continue;
        u64 lo = st.xfb[b].addr + st.xfb_written[b], hi = v ? lo + v * vs->xfb_stride[b] : st.xfb[b].addr + st.xfb[b].size;
        if (nxfb_ranges < XFB_RANGES) xfb_ranges[nxfb_ranges][0] = lo, xfb_ranges[nxfb_ranges][1] = hi;
        nxfb_ranges++;                  /* (past XFB_RANGES: counted, not kept) */
        u64 *d = st.xfb_dirty[b];
        if (d[0] == d[1]) d[0] = lo, d[1] = hi;
        else d[0] = lo < d[0] ? lo : d[0], d[1] = hi > d[1] ? hi : d[1];
        st.xfb_written[b] += v * vs->xfb_stride[b];
        xfb_prev_end[b] = offs[b] + v * vs->xfb_stride[b];
    }
}

static bool xfb_dirty_overlaps(u64 addr, u64 size) {
    if (!addr) return false;
    if (!size) size = 1;
    for (u32 b = 0; b < 4; b++)
        if (st.xfb_dirty[b][0] < st.xfb_dirty[b][1] && addr < st.xfb_dirty[b][1] && st.xfb_dirty[b][0] < addr + size) {
            if (nxfb_ranges > XFB_RANGES) return true;
            for (u32 i = 0; i < nxfb_ranges; i++)
                if (addr < xfb_ranges[i][1] && xfb_ranges[i][0] < addr + size) return true;
            return false;
        }
    return false;
}

/* does this draw read (or capture over) what earlier draws captured since the last barrier? Captures go to
 * successive offsets, so a run of capturing draws needs no barrier between them, nor does a draw that reads none
 * of the captured bytes */
static u64 xfb_hazards[8];              /* NX_GPU_PROF: by what the draw would read (xfb_hazard_names) */
static const char *const xfb_hazard_names[8] = {"", "vertex buffer", "uniform buffer", "storage buffer", "indices",
                                                "indirect draw", "other draw kind", "captures over them"};

/* 0, or which of xfb_hazard_names. Only what the draw can read: the vertex streams its attributes name (a stream
 * left bound by an earlier draw is not read) and the buffers of the stages its program has */
static int xfb_hazard_kind(u32 op, const void *payload) {
    u32 streams = 0;
    for (u32 i = 0; i < st.nattribs && i < 32; i++)
        if (VA_FORMAT(st.attribs[i]) && VA_STREAM(st.attribs[i]) < 31) streams |= 1u << VA_STREAM(st.attribs[i]);
    for (u32 i = 0; i < 32; i++)
        if ((streams >> i & 1) &&
            xfb_dirty_overlaps(st.vbuf[i].addr, st.vbuf[i].size ? st.vbuf[i].size : ~0ull - st.vbuf[i].addr)) return 1;
    for (u32 stage = 0; stage < ST_COUNT; stage++) {
        if (stage != ST_COMPUTE && st.program && !st.program->stage[stage]) continue;
        for (u32 i = 0; i < 18; i++) if (xfb_dirty_overlaps(st.ubo[stage][i].addr, st.ubo[stage][i].size)) return 2;
        for (u32 i = 0; i < 16; i++) if (xfb_dirty_overlaps(st.ssbo[stage][i].addr, st.ssbo[stage][i].size)) return 3;
    }
    if (op == OP_DRAW_ARRAYS || op == OP_DRAW_ELEMENTS) {
        const CmdDraw *d = payload;
        if (d->index_type != ~0u && xfb_dirty_overlaps(d->index_addr, (u64)(d->first + d->count) * 4)) return 4;
    } else if (op == OP_DRAW_ARRAYS_INDIRECT || op == OP_DRAW_ELEMENTS_INDIRECT) {
        const CmdDrawIndirect *d = payload;
        if (xfb_dirty_overlaps(d->index_addr, ~0ull >> 8) || xfb_dirty_overlaps(d->indirect, 64) ||
            xfb_dirty_overlaps(d->count_addr, 4)) return 5;
    } else return 6;
    const ShaderCode *vs = st.program ? st.program->stage[ST_VERTEX] : NULL;
    if (st.xfb_active && vs && vs->caps & SHC_XFB)
        for (u32 b = 0; b < 4; b++)
            if (vs->xfb_stride[b] && st.xfb_written[b] < st.xfb[b].size &&
                xfb_dirty_overlaps(st.xfb[b].addr + st.xfb_written[b], st.xfb[b].size - st.xfb_written[b]))
                return 7;
    return 0;
}

static bool xfb_hazard(u32 op, const void *payload) {
    int k = xfb_hazard_kind(op, payload);
    xfb_hazards[k]++;
    return k != 0;
}

static void draw_one(u32 op, const void *payload);

/* NX_DUMP_DRAWS=<hex address>[,<hex address>...]: in a traced frame (F12, NX_TRACE), the render targets at those
 * addresses after every draw into them, as frames/frame_<frame>_draw_<draw>_rt<i>_f<format>_<address>_<fragment
 * shader>.ppm (+ .raw), decoded in the format the draw used (which draw makes an artifact) */
static void dump_after_draw(void) {
    if (!env.dump_draws_list[0] || !trace_frame()) return;
    for (u32 i = 0; i < st.nrt; i++) {
        u64 a = st.rt[i].t.addr;
        bool want = false;
        for (int k = 0; k < 4 && env.dump_draws_list[k]; k++) want |= a == env.dump_draws_list[k];
        if (!want) continue;
        /* the image bound there (other images may share the address: the game reuses render target memory) */
        u32 ly, lv;
        Image *im = image_find(&st.rt[i].t, &ly, &lv);
        if (!im || im->info.samples > 1) continue;
        VkFormat vf = usable_format(st.rt[i].t.format);
        const ShaderCode *fs = st.program ? st.program->stage[ST_FRAGMENT] : NULL;
        char name[160];
        snprintf(name, sizeof name, "frame_%llu_draw_%07llu_rt%u_f%u_%llx_fs_%016llx", frame_no + 1, draws, i,
                 st.rt[i].t.format, a, fs ? fs->hash : 0ull);
        dump_image_as(im, name, true, vf != VK_FORMAT_UNDEFINED ? vf : im->format);
    }
}

/* Traced frames (F12, NX_TRACE): an occlusion query around each Vulkan draw, its samples logged at present: a
 * draw with no samples while its object is in view was culled in its shaders or its geometry collapsed */
typedef struct QueryLog { u64 draw, vs, fs; } QueryLog;
#define QUERY_MAX 16384
static VkQueryPool query_pool;
static QueryLog query_draw[QUERY_MAX];
static u32 nqueries;
static bool query_on;

static void query_begin(VkCommandBuffer c) {
    query_on = trace_frame() && nqueries < QUERY_MAX && st.rendering;
    if (!query_on) return;
    if (!query_pool) {
        VkQueryPoolCreateInfo ci = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION,
                                    .queryCount = QUERY_MAX};
        VKCHECK(vkCreateQueryPool(vk.dev, &ci, NULL, &query_pool));
        vkResetQueryPool(vk.dev, query_pool, 0, QUERY_MAX);
    }
    vkCmdBeginQuery(c, query_pool, nqueries, VK_QUERY_CONTROL_PRECISE_BIT);
}

static void query_end(VkCommandBuffer c) {
    if (!query_on) return;
    vkCmdEndQuery(c, query_pool, nqueries);
    query_draw[nqueries++] = (QueryLog){draws, pipe_vs_hash, pipe_fs_hash};
    query_on = false;
}
#define QUERIED(stmt) do { query_begin(c); stmt; query_end(c); } while (0)

void draw(u32 op, const void *payload) {
    if (st.xfb_captured && xfb_hazard(op, payload)) {   /* what was captured is read (or captured into) next */
        bool in_pass = st.rendering;
        end_rendering();
        if (!in_pass) barrier();        /* end_rendering has made one */
        st.xfb_captured = false;
        memset(st.xfb_dirty, 0, sizeof st.xfb_dirty);
    nxfb_ranges = 0;
    }
    draw_one(op, payload);
    label_draw_end();
    dump_after_draw();
    if (xfb_capturing) {
        vkCmdEndTransformFeedbackEXT(cmd(), 0, 0, NULL, NULL);
        xfb_capturing = false;
        st.xfb_captured = true;
    }
    if (xfb_stored) {
        xfb_stored = false;
        st.xfb_captured = true;
    }
}

typedef struct IndLog { u64 draw, vs, fs, addr; bool arrays; } IndLog;
#define IND_LOG_MAX 8192
static IndLog ind_log[IND_LOG_MAX];
static u32 nind_log;

/* NX_FEEDBACK_LOG: draws that sample a texture they render into (each shader, slot and target once). Vulkan leaves
 * such reads undefined (a vendor may show the old texel, the new one or garbage) unless the subresources differ or
 * the target is not written (write mask 0, no depth writes); the log says which. */
static void feedback_check(DescSet *ts) {
    static struct { u64 hash; u32 slot; } seen[1024];
    static u32 nseen;
    if (!ts) return;
    for (u32 stage = ST_VERTEX; stage <= ST_FRAGMENT; stage++) {
        const ShaderCode *s = st.program->stage[stage];
        if (!s || !(s->caps & SHC_TEXTURE)) continue;
        u32 used = s->tex_dyn ? ~0u : s->tex_slots;
        for (u32 k = 0; k < 32; k++) {
            u32 id = (u32)st.tex[stage][k] & 0xfffff;
            if (!(used >> k & 1) || !st.tex[stage][k] || id >= ts->count || !ts->res[id] || !ts->img[id]) continue;
            Image *im = ts->img[id];
            for (u32 t = 0; t < 9; t++) {
                const u64 lo = st.rt_sub[t].addr, hi = lo + st.rt_sub[t].size;
                if (!st.rt_sub[t].size || im->info.addr >= hi || im->info.addr + im->size <= lo) continue;
                bool written = t < 8 ? (st.mask.mask >> (4 * t) & 0xf) != 0 : (st.ds.flags & 2) != 0;
                /* the sampled view's levels and layers (same image only: an alias numbers them its own way) */
                const TexDesc *td = ts->idx[id] != ~0u ? tdesc_get(ts->idx[id]) : NULL;
                ViewInfo v = {0};
                u32 l0 = 0, nl = 0, y0 = 0, ny = 0;
                bool same = im->info.addr == lo && td;
                if (same) {
                    TexRef tr = {td->t, td->v};
                    if (image_ref(&tr, &v) != im) same = false;
                }
                if (same) {
                    l0 = v.base_level < im->levels ? v.base_level : im->levels - 1;
                    nl = v.num_levels && l0 + v.num_levels <= im->levels ? v.num_levels : im->levels - l0;
                    ny = view_layers(im, &v, VIEW_SAMPLED, &y0);
                    if (st.rt_sub[t].level < l0 || st.rt_sub[t].level >= l0 + nl ||
                        st.rt_sub[t].layer + st.rt_sub[t].nlayers <= y0 || y0 + ny <= st.rt_sub[t].layer)
                        continue;                       /* another level or layer: defined */
                }
                u32 key = stage << 12 | k << 4 | t, i;
                for (i = 0; i < nseen && (seen[i].hash != s->hash || seen[i].slot != key); i++) {}
                if (i < nseen || nseen == 1024) continue;
                seen[nseen].hash = s->hash;
                seen[nseen++].slot = key;
                char tgt[8];
                if (t < 8) snprintf(tgt, sizeof tgt, "c%u", t);
                else snprintf(tgt, sizeof tgt, "depth");
                hlog(0, "gpu: feedback: frame %llu draw %llu %s %016llx slot %u samples %s (%ux%u f%u at %llx%s, "
                     "rendering level %u layers %u+%u, sampling levels %u+%u layers %u+%u)%s", frame_no + 1, draws,
                     stage == ST_VERTEX ? "vs" : "fs", s->hash, k, tgt, im->info.width, tex_height(&im->info),
                     im->info.format, im->info.addr, same ? "" : ", overlapping memory", st.rt_sub[t].level,
                     st.rt_sub[t].layer, st.rt_sub[t].nlayers, l0, nl, y0, ny,
                     written ? "" : "; target not written (defined)");
            }
        }
    }
}

static void draw_one(u32 op, const void *payload) {
    draws++;
    work_unflushed++;
    Shaders *pr = st.program;
    if (!st.render_enable) { skip_draw("rendering disabled (SetRenderEnable)"); return; }
    if (op == OP_DRAW_TEXTURE || op == OP_DRAW_XFB || op == OP_MULTI_DRAW_INDIRECT) { skip_draw("DrawTexture / transform feedback / multi-draw"); return; }
    if (!pr || !pr->stage[ST_VERTEX]) { skip_draw("no vertex shader"); return; }
    if (pr->mask & ~(1u << ST_VERTEX | 1u << ST_FRAGMENT)) { skip_draw("geometry or tessellation shaders"); return; }
    u32 vs_mask = int_attr_mask();
    bool wait = !env.async_shaders;
    VkShaderModule vs = shader_module_w(pr->stage[ST_VERTEX], wait);
    VkShaderModule fs = pr->stage[ST_FRAGMENT] ? shader_module_w(pr->stage[ST_FRAGMENT], wait) : NULL;
    if (!vs || (pr->stage[ST_FRAGMENT] && !fs)) {
        bool gone = pr->stage[ST_VERTEX]->missing || (pr->stage[ST_FRAGMENT] && pr->stage[ST_FRAGMENT]->missing);
        if (!wait && !gone) skip_draw("shader being translated (NX_ASYNC_SHADERS)");
        else skip_draw("shader not translated");
        return;
    }
    pipe_vs_hash = pr->stage[ST_VERTEX]->hash;
    pipe_fs_hash = pr->stage[ST_FRAGMENT] ? pr->stage[ST_FRAGMENT]->hash : 0;
    /* NX_SKIP_SHADERS=hash,...: skip draws using these shaders (finding one a driver cannot compile) */
    const char *skip_shaders = env.skip_shaders;
    if (skip_shaders) {
        char hv[17], hf[17];
        snprintf(hv, sizeof hv, "%016llx", pipe_vs_hash);
        snprintf(hf, sizeof hf, "%016llx", pipe_fs_hash);
        if (strstr(skip_shaders, hv) || (pipe_fs_hash && strstr(skip_shaders, hf))) { skip_draw("NX_SKIP_SHADERS"); return; }
    }
    /* NX_GPU_SKIP: bisecting GPU faults (1 all draws, 2 global memory, 4 textures, 8 textures not plain 2D) */
    int skip = env.gpu_skip;
    u32 caps = pr->stage[ST_VERTEX]->caps | (pr->stage[ST_FRAGMENT] ? pr->stage[ST_FRAGMENT]->caps : 0);
    if (skip & 1 || (skip & 2 && caps & SHC_GLOBAL) || (skip & 4 && caps & SHC_TEXTURE) ||
        (skip & 8 && caps & SHC_TEX_NON2D)) { skip_draw("NX_GPU_SKIP"); return; }
    DescSet *ts = descset_get(&st.texpool, DS_TEX), *ss = descset_get(&st.smppool, DS_SMP);
    descset_sync(ts);
    descset_sync(ss);
    descset_use(ts, st.tex[ST_VERTEX], 32);
    descset_use(ts, st.tex[ST_FRAGMENT], 32);
    DescSet *is = caps & SHC_IMAGE ? descset_get(&st.texpool, DS_IMG) : NULL;
    if (is) {
        descset_sync(is);
        descset_use(is, st.img[ST_VERTEX], 8);
        descset_use(is, st.img[ST_FRAGMENT], 8);
    }
    if (!begin_rendering()) { skip_draw("no render targets"); return; }
    if (env.feedback_log) feedback_check(ts);
    VkCommandBuffer c = cmd();
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.vs = pipe_vs_hash;
    k.vs_mask = vs_mask;
    k.fs = pipe_fs_hash;
    k.ncolor = st.nrt;
    memcpy(k.color, st.rt_format, sizeof k.color);
    k.depth = st.depth_format;
    k.stencil = st.stencil_format;
    k.samples = st.samples;
    k.minus_one = depth_mode == 0;
    if (!vk.dyn_logic_op) k.logic_op = st.color.logic_op & 15;
    if (fs && st.color.alpha_test >= 1 && st.color.alpha_test <= 7) k.alpha_func = st.color.alpha_test;
    if (!vk.vi_dynamic) vi_key(&k);
    bool pending;
    VkPipeline pipe = pipeline_get(&k, vs, fs, env.async_shaders, &pending);
    if (!pipe) { skip_draw(pending ? "pipeline being built (NX_ASYNC_SHADERS)" : "pipeline creation failed"); return; }
    if (trace_frame()) {
        const CmdDraw *d = payload;
        static char tb[8192];
        int tn = 0;
#define TP(...) (tn += snprintf(tb + tn, tn < (int)sizeof tb ? sizeof tb - tn : 0, __VA_ARGS__))
        TP("draw %llu: op %u mode %u count %u inst %u first %u base %d/%u idx %u@%llx | vs %016llx fs %016llx | rt0 %ux%u f%u @%llx n%u ds %s | vp %g %g %g %g dr %g %g sc %d %d %d %d | ds %x df %u cull %u ff %u clamp %u",
             draws, op, d->mode, d->count, d->instances, d->first, d->base_vertex, d->base_instance, d->index_type, d->index_addr, pr->stage[ST_VERTEX]->hash,
             pr->stage[ST_FRAGMENT] ? pr->stage[ST_FRAGMENT]->hash : 0, st.rt[0].t.width, st.rt[0].t.height,
             st.rt[0].t.format, st.rt[0].t.addr, st.nrt, st.has_depth ? "yes" : "no", st.viewport[0][0], st.viewport[0][1],
             st.viewport[0][2], st.viewport[0][3], st.depth_range[0][0], st.depth_range[0][1], st.scissor[0][0],
             st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], st.ds.flags, st.ds.depth_func, st.poly.cull, st.poly.front_face, (u32)st.depth_clamp);
        TP("\n");
        char b[512] = "";               /* (printed even when no attribute is set) */
        int o = 0;
        for (u32 i = 0; i < st.nattribs && o < 400; i++)
            if (VA_FORMAT(st.attribs[i]))
                o += snprintf(b + o, sizeof b - o, " a%u:f%u s%u+%u", i, VA_FORMAT(st.attribs[i]), VA_STREAM(st.attribs[i]),
                              VA_OFFSET(st.attribs[i]));
        TP("   attribs%s\n", b);
        o = 0;
        for (u32 i = 0; i < 4; i++)
            o += snprintf(b + o, sizeof b - o, " v%u:%llx+%llx/%u", i, st.vbuf[i].addr, st.vbuf[i].size,
                          i < st.nstreams ? st.streams[i].stride : 0);
        for (u32 i = 0; i < 6; i++)
            o += snprintf(b + o, sizeof b - o, " u%u:%llx+%llx", i, st.ubo[ST_VERTEX][i].addr, st.ubo[ST_VERTEX][i].size);
        TP("  %s\n", b);
        /* NX_TRACE_UBO=<16-byte row>[,row...]: those rows of the vertex stage's uniform buffer 0 */
        const char *rows = env.trace_ubo;
        u8 *u0 = rows && st.ubo[ST_VERTEX][0].addr ? gpu_cpu(st.ubo[ST_VERTEX][0].addr, st.ubo[ST_VERTEX][0].size) : NULL;
        for (const char *q = rows; u0 && q && *q;) {
            u32 row = (u32)strtoul(q, (char **)&q, 10);
            if (*q == ',') q++;
            if (16ull * row + 16 > st.ubo[ST_VERTEX][0].size) continue;
            float f[4];
            memcpy(f, u0 + 16 * row, 16);
            TP("   u0[%u] = %g %g %g %g\n", row, f[0], f[1], f[2], f[3]);
        }
        /* NX_TRACE_VCB=<c index>:<row>[,row...][;<c index>:...]: rows of the vertex stage's c[i] (i >= 3), and the
         * first element of each vertex stream as floats */
        for (const char *q = env.trace_vcb; q && *q;) {
            u32 cb = (u32)strtoul(q, (char **)&q, 10);
            const BufferRange *ub = cb >= 3 && cb < 18 ? &st.ubo[ST_VERTEX][cb - 3] : NULL;
            u8 *vu = ub && ub->addr ? gpu_cpu(ub->addr, ub->size) : NULL;
            if (*q == ':') q++;
            while (*q && *q != ';') {
                u32 row = (u32)strtoul(q, (char **)&q, 10);
                if (*q == ',') q++;
                else if (*q && *q != ';') { q += strlen(q); break; }
                if (!vu) { TP("   vc%u: unset\n", cb); continue; }
                if (16ull * row + 16 > ub->size) continue;
                float f[4];
                memcpy(f, vu + 16 * row, 16);
                TP("   vc%u[%u] = %g %g %g %g\n", cb, row, f[0], f[1], f[2], f[3]);
            }
            if (*q == ';') q++;
        }
        for (u32 i = 0; env.trace_vcb && i < st.nstreams && i < 6; i++) {
            u8 *vb = st.vbuf[i].addr ? gpu_cpu(st.vbuf[i].addr, 16) : NULL;
            float f[4];
            if (vb) {
                memcpy(f, vb, 16);
                TP("   vb%u[0] = %g %g %g %g (stride %u divisor %u)\n", i, f[0], f[1], f[2], f[3], st.streams[i].stride,
                   st.streams[i].divisor);
            }
        }
        o = 0;
        char tb2[2048];
        for (u32 k = 0; k < 32 && st.texpool.addr && o < 1900; k++) {
            u32 h = (u32)st.tex[ST_FRAGMENT][k], id = h & 0xfffff;
            if (!h || id >= st.texpool.count) continue;
            u64 a = st.texpool.addr + 32ull * id;
            const TexDesc *td = LD32(a) == TDESC_MAGIC ? tdesc_get(LD32(a + 4)) : NULL;
            if (td) o += snprintf(tb2 + o, sizeof tb2 - o, " t%u:%ux%ux%u f%u L%u/%u+%u tg%u @%llx", k, td->t.width, td->t.height,
                                  td->t.depth, td->v.format, td->v.base_level, td->t.levels, td->v.num_levels, td->v.target, td->t.addr);
        }
        if (o) TP("   tex%s\n", tb2);
        o = 0;      /* render targets as viewed: texture shape and target, view level and layers */
        for (u32 i = 0; i < st.nrt && i < 8; i++)
            o += snprintf(b + o, sizeof b - o, " rt%u:%ux%ux%u tg%u L%u ly%u+%u @%llx", i, st.rt[i].t.width, st.rt[i].t.height,
                          st.rt[i].t.depth, st.rt[i].t.target, st.rt[i].v.base_level, st.rt[i].v.min_layer,
                          st.rt[i].v.num_layers, st.rt[i].t.addr);
        if (st.has_depth)
            o += snprintf(b + o, sizeof b - o, " ds:%ux%ux%u f%u L%u ly%u+%u @%llx", st.depth.t.width, st.depth.t.height,
                          st.depth.t.depth, st.depth.v.format, st.depth.v.base_level, st.depth.v.min_layer,
                          st.depth.v.num_layers, st.depth.t.addr);
        if (o) TP("   rts%s\n", b);
        /* NX_TRACE_FUBO=<bank>:<row>[,row...]: those 16-byte rows of the fragment stage's uniform buffer c[3 + bank] */
        const char *fub = env.trace_fubo;
        if (fub && strchr(fub, ':')) {
            u32 bank = (u32)strtoul(fub, NULL, 10);
            const BufferRange *ub = bank < 15 ? &st.ubo[ST_FRAGMENT][bank] : NULL;
            u8 *fu = ub && ub->addr ? gpu_cpu(ub->addr, ub->size) : NULL;
            for (const char *q = strchr(fub, ':') + 1; fu && *q;) {
                u32 row = (u32)strtoul(q, (char **)&q, 10);
                if (*q == ',') q++;
                else if (*q) break;
                if (16ull * row + 16 > ub->size) continue;
                float f[4];
                memcpy(f, fu + 16 * row, 16);
                TP("   fc%u[%u] = %g %g %g %g\n", 3 + bank, row, f[0], f[1], f[2], f[3]);
            }
        }
#undef TP
        const char *filter = env.trace_filter;
        if (tn > 0 && tn < (int)sizeof tb && tb[tn - 1] == '\n') tb[--tn] = 0;
        if (!filter[0] || strstr(tb, filter)) hlog(0, "%s", tb);
    }
    if (prof_on > 1) {                  /* NX_GPU_PROF=2: by draw, its shaders and pass size */
        char lb[80];
        snprintf(lb, sizeof lb, "  draw %ux%u vs %016llx fs %016llx", st.area.extent.width, st.area.extent.height,
                 pr->stage[ST_VERTEX]->hash, pr->stage[ST_FRAGMENT] ? pr->stage[ST_FRAGMENT]->hash : 0ull);
        prof_mark(lb);
    }
    if (!st.rasterizer_discard || env.no_discard) pass_att_writes = true;
    if (vk.labels) {                    /* closed by draw() after the draw, or by end_rendering */
        label_draw_end();
        label_begin(c, "draw vs %016llx fs %016llx", pr->stage[ST_VERTEX]->hash,
                    pr->stage[ST_FRAGMENT] ? pr->stage[ST_FRAGMENT]->hash : 0ull);
        label_draw_open = true;
    }
    rec_check();
    bool all = !rec.bind_ok;
    rec.bind_ok = true;
    if (all || pipe != rec.pipe) {
        vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        rec.pipe = pipe;
    }
    set_dynamic_state(c, ((const CmdDraw *)payload)->mode);
    VkWriteDescriptorSet w[32];
    VkDescriptorBufferInfo bi[32];
    mirror_bad_ssbos(st.ssbo[ST_VERTEX], pr->stage[ST_VERTEX]);
    mirror_bad_ssbos(st.ssbo[ST_FRAGMENT], pr->stage[ST_FRAGMENT]);
    int nw = stage_ubos(ST_VERTEX, 0, pr->stage[ST_VERTEX], w, bi);
    nw += stage_ubos(ST_FRAGMENT, 16, pr->stage[ST_FRAGMENT], w + nw, bi + nw);
    bool same = !all && (u32)nw == rec.nw;
    for (int i = 0; same && i < nw; i++)
        same = w[i].dstBinding == rec.binding[i] && w[i].descriptorType == rec.type[i] && !memcmp(w[i].pBufferInfo, &rec.bi[i], sizeof rec.bi[i]);
    if (!same) {
        vkCmdPushDescriptorSetKHR(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_gfx, 0, nw, w);
        rec.nw = nw;
        for (int i = 0; i < nw; i++) {
            rec.binding[i] = w[i].dstBinding;
            rec.type[i] = w[i].descriptorType;
            rec.bi[i] = *w[i].pBufferInfo;
        }
    }
    VkDescriptorSet sets[3] = {ts ? ts->set : empty_tex, ss ? ss->set : empty_smp, is ? is->set : empty_img};
    if (all || memcmp(sets, rec.sets, sizeof sets)) {
        vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_gfx, 1, 3, sets, 0, NULL);
        memcpy(rec.sets, sets, sizeof sets);
    }
    /* debug word (fragment shaders' pc.debug): bit 0 NX_NO_DISCARD, discards off in draws that write depth (is an
     * object drawn but discarded?); 2D layers keep theirs (with every discard off, the title screen covered all) */
    bool no_discard = env.no_discard;
    /* scale: textures' (bits 0-7: HANDLE_SCALED ones are at it) and this pass's targets' (8-15) */
    u32 rts = st.rt_scale ? st.rt_scale : 1;
    struct { u32 func; float ref, point_size; u32 debug, scale; } pc = {st.color.alpha_test, st.alpha_ref,
        st.point_size * (float)rts, (u32)(no_discard && (st.ds.flags & 2)), rs_scale | rts << 8};
    if (debug_rt_hit() && env.debug_rt_bits & 1) pc.debug |= 1;
    /* only with a color target 0: without one (the G-buffer passes, targets 1-4) its alpha means nothing, and
     * testing it hid whole objects */
    if (st.ms.a2c && st.samples <= 1 && (pc.func == 0 || pc.func == 8) && st.nrt && st.rt[0].t.addr) {
        pc.func = 7;                    /* GEQUAL 0.5: alpha to coverage on one sample */
        pc.ref = 0.5f;
    }
    if (all || memcmp(&pc, rec.pc, PC_SIZE)) {
        vkCmdPushConstants(c, layout_gfx, PC_STAGES, 0, PC_SIZE, &pc);
        memcpy(rec.pc, &pc, PC_SIZE);
    }
    set_vertex_input(c);
    xfb_begin(c, op, payload);
    if (op == OP_DRAW_ARRAYS || op == OP_DRAW_ELEMENTS) {
        const CmdDraw *d = payload;
        bool indexed = op == OP_DRAW_ELEMENTS;
        if (d->mode == 7) {             /* quads */
            VkBuffer ib;
            u64 io;
            u32 n;
            if (!quad_indices(d, indexed, &ib, &io, &n)) { skip_draw("quads"); return; }
            vkCmdBindIndexBuffer(c, ib, io, VK_INDEX_TYPE_UINT32);
            QUERIED(vkCmdDrawIndexed(c, n, d->instances, 0, indexed ? d->base_vertex : 0, d->base_instance));
            return;
        }
        if (d->mode == 2 || d->mode == 8 || d->mode == 9) { skip_draw("line loop / quad strip / polygon"); return; }
        if (!indexed) {
            QUERIED(vkCmdDraw(c, d->count, d->instances, d->first, d->base_instance));
            return;
        }
        VkBuffer ib;
        u64 io;
        if (d->index_type == 0 && !vk.u8_indices) {
            if (!u8_indices_widened(d, &ib, &io)) { skip_draw("8-bit indices not readable"); return; }
            vkCmdBindIndexBuffer(c, ib, io, VK_INDEX_TYPE_UINT16);
            QUERIED(vkCmdDrawIndexed(c, d->count, d->instances, 0, d->base_vertex, d->base_instance));
            return;
        }
        if (!gpu_buffer_ro(d->index_addr, (u64)d->count << d->index_type, &ib, &io)) { skip_draw("index buffer not GPU memory"); return; }
        VkIndexType it = d->index_type == 0 ? VK_INDEX_TYPE_UINT8_KHR : d->index_type == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        vkCmdBindIndexBuffer(c, ib, io, it);
        QUERIED(vkCmdDrawIndexed(c, d->count, d->instances, 0, d->base_vertex, d->base_instance));
        return;
    }
    const CmdDrawIndirect *d = payload;
    VkBuffer buf, ib;
    u64 off, io;
    if (!gpu_buffer(d->indirect, &buf, &off)) { skip_draw("indirect buffer not GPU memory"); return; }
    if (trace_frame() && nind_log < IND_LOG_MAX)
        ind_log[nind_log++] = (IndLog){draws, pipe_vs_hash, pipe_fs_hash, d->indirect, op == OP_DRAW_ARRAYS_INDIRECT};
    if (op == OP_DRAW_ARRAYS_INDIRECT) {
        QUERIED(vkCmdDrawIndirect(c, buf, off, 1, 16));
        return;
    }
    if (d->index_type == 0 && !vk.u8_indices) {
        /* the count is in GPU memory: not widened on the CPU (a compute pass would be the fix if these show up) */
        static volatile LONG n;
        if (InterlockedIncrement(&n) == 1) hlog(0, "gpu: indirect draw with 8-bit indices skipped (no VK_KHR_index_type_uint8)");
        skip_draw("indirect draw with 8-bit indices");
        return;
    }
    if (!gpu_buffer_ro(d->index_addr, 1, &ib, &io)) { skip_draw("index buffer not GPU memory"); return; }
    VkIndexType it = d->index_type == 0 ? VK_INDEX_TYPE_UINT8_KHR : d->index_type == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    vkCmdBindIndexBuffer(c, ib, io, it);
    QUERIED(vkCmdDrawIndexedIndirect(c, buf, off, 1, 20));
}

/* ---- compute */
static u64 pr_hash(void) { return st.bound[ST_COMPUTE] ? st.bound[ST_COMPUTE]->hash : 0; }

void dispatch(u32 op, const u8 *pay) {
    dispatches++;
    work_unflushed++;
    u64 dump_hash = env.dump_ssbo;
    static u32 dumped;
    if (dump_hash && dump_hash == pr_hash() && dumped < 40) {
        /* the first dispatches of NX_DUMP_SSBO's shader, traced or not: its work list (s0: count, then 16-byte
         * entries whose first word indexes 128-byte records in s1) and the s1 records the first entries name */
        dumped++;
        const BufferRange *r0 = &st.ssbo[ST_COMPUTE][0], *r1 = &st.ssbo[ST_COMPUTE][1];
        const u32 *l = r0->addr ? (const u32 *)gpu_cpu(r0->addr, 16 + 16 * 8) : NULL;
        hlog(0, "ssbo dump %u (frame %llu): groups %u, s0 %llx count %u", dumped, frame_no,
             op == OP_DISPATCH ? ((const u32 *)pay)[0] : 0, r0->addr, l ? l[0] : 0);
        for (u32 i = 0; l && i < 8 && i < l[0]; i++) {
            u32 e = l[4 + 4 * i];
            const u32 *rec = r1->addr && (u64)e * 128 + 128 <= r1->size ? (const u32 *)gpu_cpu(r1->addr + (u64)e * 128, 128) : NULL;
            if (!rec) { hlog(0, "  entry %u: %x (record outside s1)", i, e); continue; }
            hlog(0, "  entry %u: %x  s1: %08x %08x %08x %08x %08x %08x %08x %08x", i, e, rec[0], rec[1], rec[2], rec[3],
                 rec[4], rec[5], rec[6], rec[7]);
        }
    }
    if (trace_frame()) {
        char b[700];
        int o = 0;
        for (u32 i = 0; i < 14; i++)
            if (st.ubo[ST_COMPUTE][i].addr) o += snprintf(b + o, sizeof b - o, " u%u:%llx+%llx", i, st.ubo[ST_COMPUTE][i].addr, st.ubo[ST_COMPUTE][i].size);
        for (u32 i = 0; i < 16; i++)
            if (st.ssbo[ST_COMPUTE][i].addr) o += snprintf(b + o, sizeof b - o, " s%u:%llx+%llx", i, st.ssbo[ST_COMPUTE][i].addr, st.ssbo[ST_COMPUTE][i].size);
        hlog(0, "dispatch %llu: cs %016llx%s", dispatches, pr_hash(), b);
        /* NX_DUMP_SSBO=<shader hash>: that shader's storage buffers as the CPU sees them at replay (what earlier
         * GPU work in the same submission writes is not there yet), the first 16 words each, and its group count */
        if (dump_hash && dump_hash == pr_hash()) {
            if (op == OP_DISPATCH) hlog(0, "  groups %u %u %u", ((const u32 *)pay)[0], ((const u32 *)pay)[1], ((const u32 *)pay)[2]);
            for (u32 i = 0; i < 16; i++) {
                const BufferRange *r = &st.ssbo[ST_COMPUTE][i];
                const u32 *w = r->addr ? (const u32 *)gpu_cpu(r->addr, 64) : NULL;
                if (!w) continue;
                o = 0;
                for (int k = 0; k < 16; k++) o += snprintf(b + o, sizeof b - o, " %08x", w[k]);
                hlog(0, "  s%u:%s", i, b);
            }
        }
    }
    ShaderCode *cs = st.bound[ST_COMPUTE];
    if (!cs) { skip_draw("dispatch without a compute shader"); return; }
    VkShaderModule m = shader_module(cs);
    mirror_bad_ssbos(st.ssbo[ST_COMPUTE], cs);
    if (!m) { skip_draw("shader not translated"); return; }
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.vs = cs->hash;
    k.compute = 1;
    bool pending;
    VkPipeline cpipe = pipeline_get(&k, m, NULL, false, &pending);
    if (!cpipe) return;
    DescSet *ts = descset_get(&st.texpool, DS_TEX), *ss = descset_get(&st.smppool, DS_SMP);
    descset_sync(ts);
    descset_sync(ss);
    descset_use(ts, st.tex[ST_COMPUTE], 32);
    DescSet *is = cs->caps & SHC_IMAGE ? descset_get(&st.texpool, DS_IMG) : NULL;
    if (is) {
        descset_sync(is);
        descset_use(is, st.img[ST_COMPUTE], 8);
    }
    end_rendering();
    VkCommandBuffer c = cmd();
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, cpipe);
    VkWriteDescriptorSet w[16];
    VkDescriptorBufferInfo bi[16];
    int nw = stage_ubos(ST_COMPUTE, 0, cs, w, bi);
    vkCmdPushDescriptorSetKHR(c, VK_PIPELINE_BIND_POINT_COMPUTE, layout_gfx, 0, nw, w);
    VkDescriptorSet sets[3] = {ts ? ts->set : empty_tex, ss ? ss->set : empty_smp, is ? is->set : empty_img};
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, layout_gfx, 1, 3, sets, 0, NULL);
    u32 cpc[PC_SIZE / 4] = {0, 0, 0, 0, rs_scale | 1u << 8};       /* the render scale (see draw_one) */
    vkCmdPushConstants(c, layout_gfx, PC_STAGES, 0, PC_SIZE, cpc);
    rec_check();
    memset(rec.pc, 0xff, sizeof rec.pc);    /* push constants are shared with graphics: theirs are gone */
    if (prof_on > 0) {
        char lb[48];
        snprintf(lb, sizeof lb, "compute %016llx", pr_hash());
        prof_mark(lb);
    }
    if (vk.labels) label_begin(c, "compute %016llx", pr_hash());
    if (op == OP_DISPATCH) {
        u32 g[3];
        memcpy(g, pay, 12);
        if (g[0] && g[1] && g[2]) vkCmdDispatch(c, g[0], g[1], g[2]);
    } else {
        u64 a;
        memcpy(&a, pay, 8);
        VkBuffer buf;
        u64 off;
        if (gpu_buffer(a, &buf, &off)) vkCmdDispatchIndirect(c, buf, off);
    }
    if (vk.labels) vkCmdEndDebugUtilsLabelEXT(c);
    prof_mark("outside passes (copies, clears, barriers)");
    barrier();
}

/* the indirect draws of a traced frame, logged with the arguments the GPU wrote for them once it is done (instance
 * counts from the game's GPU culling: 0 means culled) */
void ind_log_report(void) {
    if (!nind_log) return;
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &timeline,
                              .pValues = &submitted};
    VKCHECK(vkWaitSemaphores(vk.dev, &wi, UINT64_MAX));
    u32 culled = 0;
    for (u32 i = 0; i < nind_log; i++) {
        const IndLog *l = &ind_log[i];
        const u32 *a = (const u32 *)gpu_cpu(l->addr, 20);
        if (!a) continue;
        if (a[1] == 0) culled++;
        if (l->arrays)
            hlog(0, "indirect draw %llu: vs %016llx fs %016llx: vertices %u instances %u first %u base instance %u",
                 l->draw, l->vs, l->fs, a[0], a[1], a[2], a[3]);
        else
            hlog(0, "indirect draw %llu: vs %016llx fs %016llx: indices %u instances %u first %u base vertex %d base instance %u",
                 l->draw, l->vs, l->fs, a[0], a[1], a[2], (s32)a[3], a[4]);
    }
    hlog(0, "indirect draws: %u, %u with no instances", nind_log, culled);
    nind_log = 0;
}

void query_report(void) {
    if (!nqueries) return;
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &timeline,
                              .pValues = &submitted};
    VKCHECK(vkWaitSemaphores(vk.dev, &wi, UINT64_MAX));
    u64 *r = malloc(nqueries * sizeof *r);
    VkResult res = vkGetQueryPoolResults(vk.dev, query_pool, 0, nqueries, nqueries * sizeof *r, r, sizeof *r,
                                         VK_QUERY_RESULT_64_BIT);
    u32 none = 0;
    for (u32 i = 0; res == VK_SUCCESS && i < nqueries; i++) {
        if (!r[i]) none++;
        hlog(0, "samples: draw %llu vs %016llx fs %016llx: %llu", query_draw[i].draw, query_draw[i].vs,
             query_draw[i].fs, r[i]);
    }
    hlog(0, "samples: %u draws queried, %u drew nothing%s", nqueries, none, res == VK_SUCCESS ? "" : " (results not ready)");
    free(r);
    vkResetQueryPool(vk.dev, query_pool, 0, nqueries);
    nqueries = 0;
}
