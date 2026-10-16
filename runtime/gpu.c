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
 * semaphore whose values are the NVN syncs. */
#include "tex.h"
#include <stdlib.h>
#include <windows.h>

static CRITICAL_SECTION gpu_cs;
static bool gpu_ready;

/* ---------------------------------------------------------------------------------------------- submissions */
#define SLOTS 6
#define STAGING_SIZE (32ull << 20)
typedef struct Slot {
    VkCommandPool pool;
    VkCommandBuffer cmd;
    u64 value;                          /* timeline value that marks it done */
    VkBuf staging;
    u64 staging_used;
    VkBuf extra[16];                    /* staging that did not fit */
    int nextra;
} Slot;
static Slot slots[SLOTS];
static int cur = -1;                    /* the slot being recorded, or -1 */
static u64 submitted;                   /* last timeline value submitted */
static VkSemaphore timeline;

/* things to destroy once the GPU is past `value` */
typedef struct Dead { u64 value; int kind; void *a, *b; VkAlloc alloc; } Dead;
enum { DEAD_HOSTBUF, DEAD_IMAGE, DEAD_VIEW };
static Dead *dead;
static int ndead, capdead;

u64 gpu_completed(void) {
    u64 v = 0;
    if (timeline) vkGetSemaphoreCounterValue(vk.dev, timeline, &v);
    return v;
}

static void reap(void) {
    u64 done = gpu_completed();
    int j = 0;
    for (int i = 0; i < ndead; i++) {
        Dead *d = &dead[i];
        if (d->value > done) { dead[j++] = *d; continue; }
        switch (d->kind) {
        case DEAD_HOSTBUF: vk_release_host(d->a); free(d->a); break;
        case DEAD_IMAGE: vkDestroyImage(vk.dev, d->a, NULL); vk_free(&d->alloc); break;
        case DEAD_VIEW: vkDestroyImageView(vk.dev, d->a, NULL); break;
        }
    }
    ndead = j;
}

static void bury(int kind, void *a, const VkAlloc *alloc) {
    if (ndead == capdead) dead = realloc(dead, (capdead = capdead * 2 + 64) * sizeof *dead);
    Dead *d = &dead[ndead++];
    memset(d, 0, sizeof *d);
    d->value = submitted + 1;           /* the work being recorded may still use it */
    d->kind = kind;
    d->a = a;
    if (alloc) d->alloc = *alloc;
}

static VkCommandBuffer cmd(void);

static void slot_open(void) {
    static int next;
    Slot *s = &slots[next];
    if (s->value) {
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1,
                                  .pSemaphores = &timeline, .pValues = &s->value};
        VKCHECK(vkWaitSemaphores(vk.dev, &wi, UINT64_MAX));
    }
    for (int i = 0; i < s->nextra; i++) vk_buffer_free(&s->extra[i]);
    s->nextra = 0;
    s->staging_used = 0;
    VKCHECK(vkResetCommandPool(vk.dev, s->pool, 0));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKCHECK(vkBeginCommandBuffer(s->cmd, &bi));
    cur = next;
    next = (next + 1) % SLOTS;
}

static VkCommandBuffer cmd(void) {
    if (cur < 0) slot_open();
    return slots[cur].cmd;
}

/* host-visible scratch for this submission: uploads and inline data */
static void *staging(u64 size, VkBuffer *buf, u64 *off) {
    cmd();
    Slot *s = &slots[cur];
    size = (size + 255) & ~255ull;
    if (s->staging_used + size <= STAGING_SIZE) {
        *buf = s->staging.buf;
        *off = s->staging_used;
        s->staging_used += size;
        return s->staging.a.map + *off;
    }
    if (s->nextra == 16) fatal("gpu: too much staging in one submission");
    VkBuf *e = &s->extra[s->nextra++];
    vk_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, e);
    *buf = e->buf;
    *off = 0;
    return e->a.map;
}

static void barrier(void) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cmd(), &d);
}

static void end_rendering(void);

static void flush(void) {
    end_rendering();
    if (cur < 0) return;
    Slot *s = &slots[cur];
    VKCHECK(vkEndCommandBuffer(s->cmd));
    s->value = ++submitted;
    VkSemaphoreSubmitInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = timeline, .value = s->value,
                                 .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->cmd};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi,
                        .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &sig};
    VkResult r = vkQueueSubmit2(vk.queue, 1, &si, NULL);
    if (r < 0) fatal("vkQueueSubmit2: %d (device lost?)", r);
    cur = -1;
    reap();
}

/* ---------------------------------------------------------------------------------------------- pools */
typedef struct PoolEnt { u64 gpu, end, cpu; u32 flags; HostBuffer *hb; } PoolEnt;
static PoolEnt *pools;
static int npools, cappools;

static PoolEnt *pool_find(u64 addr) {
    int lo = 0, hi = npools - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (addr < pools[m].gpu) hi = m - 1;
        else if (addr >= pools[m].end) lo = m + 1;
        else return &pools[m];
    }
    return NULL;
}

/* a GPU address as (buffer, offset), if the GPU can reach it */
static bool gpu_buffer(u64 addr, VkBuffer *buf, u64 *off) {
    PoolEnt *p = pool_find(addr);
    if (!p || !p->hb) return false;
    *buf = p->hb->buf;
    *off = addr - p->gpu;
    return true;
}

/* a GPU address as a CPU pointer to guest memory, if it has one */
static u8 *gpu_cpu(u64 addr, u64 size) {
    PoolEnt *p = pool_find(addr);
    if (!p || !p->cpu || addr + size > p->end) return NULL;
    return (u8 *)(uintptr_t)(p->cpu + (addr - p->gpu));
}

static void images_drop_range(u64 lo, u64 hi);

void gpu_pool_created(Pool *p) {
    if (!gpu_ready) return;
    EnterCriticalSection(&gpu_cs);
    HostBuffer *hb = NULL;
    if (p->storage && !(p->flags & (POOL_VIRTUAL | POOL_GPU_NO_ACCESS))) {
        hb = calloc(1, sizeof *hb);
        if (!vk_import_host(p->storage, p->size, hb)) {
            hlog(0, "gpu: cannot import pool memory %llx+%llx", p->storage, p->size);
            free(hb);
            hb = NULL;
        }
    }
    p->hb = hb;
    if (npools == cappools) pools = realloc(pools, (cappools = cappools * 2 + 64) * sizeof *pools);
    int i = npools;
    while (i > 0 && pools[i - 1].gpu > p->gpu) { pools[i] = pools[i - 1]; i--; }
    pools[i] = (PoolEnt){p->gpu, p->gpu + p->size, p->flags & POOL_VIRTUAL ? 0 : p->storage, p->flags, hb};
    npools++;
    images_drop_range(p->gpu, p->gpu + p->size);
    LeaveCriticalSection(&gpu_cs);
}

void gpu_pool_destroyed(Pool *p) {
    if (!gpu_ready) return;
    EnterCriticalSection(&gpu_cs);
    for (int i = 0; i < npools; i++)
        if (pools[i].gpu == p->gpu && pools[i].end == p->gpu + p->size) {
            if (pools[i].hb) bury(DEAD_HOSTBUF, pools[i].hb, NULL);
            memmove(&pools[i], &pools[i + 1], (npools - i - 1) * sizeof *pools);
            npools--;
            break;
        }
    images_drop_range(p->gpu, p->gpu + p->size);
    p->hb = NULL;
    LeaveCriticalSection(&gpu_cs);
}

/* ---------------------------------------------------------------------------------------------- images */
typedef struct View { ViewInfo v; u32 kind; VkImageView view; } View;
enum { VIEW_SAMPLED, VIEW_ATTACHMENT };
typedef struct Image {
    TexInfo info;
    VkImage image;
    VkAlloc alloc;
    VkFormat format;
    VkImageAspectFlags aspects;
    VkImageUsageFlags usage;
    u32 levels, layers;
    bool valid;                         /* contents defined (rendered, copied, cleared or uploaded) */
    View *views;
    int nviews, capviews;
    struct Image *next;
} Image;
#define IMAGE_BUCKETS 4096
static Image *buckets[IMAGE_BUCKETS];
static int nimages;

static u32 info_hash(const TexInfo *t) {
    const u8 *b = (const u8 *)t;
    u32 h = 2166136261u;
    for (size_t i = 0; i < sizeof *t; i++) h = (h ^ b[i]) * 16777619u;
    return h % IMAGE_BUCKETS;
}

static void image_destroy(Image *im) {
    for (int i = 0; i < im->nviews; i++) bury(DEAD_VIEW, im->views[i].view, NULL);
    free(im->views);
    bury(DEAD_IMAGE, im->image, &im->alloc);
    free(im);
    nimages--;
}

static void images_drop_range(u64 lo, u64 hi) {
    for (int b = 0; b < IMAGE_BUCKETS; b++) {
        Image **pp = &buckets[b];
        while (*pp) {
            Image *im = *pp;
            if (im->info.addr >= lo && im->info.addr < hi) {
                *pp = im->next;
                image_destroy(im);
            } else {
                pp = &im->next;
            }
        }
    }
}

static VkFormatFeatureFlags format_features(VkFormat f) {
    static VkFormatFeatureFlags cache[256];
    static bool known[256];
    if ((u32)f < 256 && known[f]) return cache[f];
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(vk.phys, f, &p);
    if ((u32)f < 256) { known[f] = true; cache[f] = p.optimalTilingFeatures; }
    return p.optimalTilingFeatures;
}

static VkFormat usable_format(u32 nvn) {
    VkFormat f = vk_format(nvn);
    if (f == VK_FORMAT_UNDEFINED) return f;
    VkFormatFeatureFlags ft = format_features(f);
    if (ft & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT || ft & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) return f;
    switch (f) {                        /* the few the GTX 1650 lacks */
    case VK_FORMAT_X8_D24_UNORM_PACK32: return VK_FORMAT_D24_UNORM_S8_UINT;
    case VK_FORMAT_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
    default: return f;
    }
}

static Image *image_get(const TexInfo *t) {
    u32 h = info_hash(t);
    for (Image *im = buckets[h]; im; im = im->next)
        if (!memcmp(&im->info, t, sizeof *t)) return im;
    VkFormat fmt = usable_format(t->format);
    if (fmt == VK_FORMAT_UNDEFINED) {
        static u32 warned[8];
        static int nw;
        bool seen = false;
        for (int i = 0; i < nw; i++) seen |= warned[i] == t->format;
        if (!seen && nw < 8) { warned[nw++] = t->format; hlog(0, "gpu: texture format %u has no Vulkan format", t->format); }
        return NULL;
    }
    if (t->target == TEX_BUFFER) return NULL;
    Image *im = calloc(1, sizeof *im);
    im->info = *t;
    im->format = fmt;
    im->aspects = format_aspects(fmt);
    im->levels = t->levels ? t->levels : 1;
    im->layers = tex_layers(t);
    if (!im->layers) im->layers = 1;
    VkFormatFeatureFlags ft = format_features(fmt);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ft & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ft & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ft & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (ft & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT && t->flags & TEX_FLAG_IMAGE) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    im->usage = usage;
    VkImageType type = t->target == TEX_3D ? VK_IMAGE_TYPE_3D :
                       t->target == TEX_1D || t->target == TEX_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
    VkImageCreateFlags flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    if (type == VK_IMAGE_TYPE_2D && im->layers >= 6 && t->width == t->height && t->samples <= 1)
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (type == VK_IMAGE_TYPE_3D) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    u32 samples = t->samples > 1 ? t->samples : 1;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = type, .format = fmt,
        .extent = {t->width, tex_height(t), tex_depth(t)}, .mipLevels = im->levels, .arrayLayers = im->layers,
        .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkResult r = vkCreateImage(vk.dev, &ci, NULL, &im->image);
    if (r != VK_SUCCESS) {
        hlog(0, "gpu: vkCreateImage %ux%ux%u format %u (vk %d) target %u: %d", t->width, t->height, t->depth,
             t->format, fmt, t->target, r);
        free(im);
        return NULL;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.dev, im->image, &req);
    if (!vk_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &im->alloc)) fatal("gpu: out of video memory");
    VKCHECK(vkBindImageMemory(vk.dev, im->image, im->alloc.mem, im->alloc.offset));
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = im->image, .subresourceRange = {im->aspects, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cmd(), &d);
    im->next = buckets[h];
    buckets[h] = im;
    if (++nimages % 500 == 0) hlog(1, "gpu: %d images", nimages);
    hlog(2, "gpu: image %ux%ux%u fmt %u target %u levels %u at %llx", t->width, t->height, t->depth, t->format,
         t->target, t->levels, t->addr);
    return im;
}

static VkImageViewType view_type(u32 target) {
    switch (target) {
    case TEX_1D: return VK_IMAGE_VIEW_TYPE_1D;
    case TEX_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case TEX_3D: return VK_IMAGE_VIEW_TYPE_3D;
    case TEX_2D_ARRAY: case TEX_2D_MS_ARRAY: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TEX_CUBE: return VK_IMAGE_VIEW_TYPE_CUBE;
    case TEX_CUBE_ARRAY: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

static VkImageView image_view(Image *im, const ViewInfo *v, u32 kind) {
    for (int i = 0; i < im->nviews; i++)
        if (im->views[i].kind == kind && !memcmp(&im->views[i].v, v, sizeof *v)) return im->views[i].view;
    VkFormat fmt = usable_format(v->format);
    if (fmt == VK_FORMAT_UNDEFINED || format_aspects(fmt) != im->aspects) fmt = im->format;
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->image, .format = fmt};
    u32 base_level = v->base_level < im->levels ? v->base_level : im->levels - 1;
    u32 nlevels = v->num_levels && base_level + v->num_levels <= im->levels ? v->num_levels : im->levels - base_level;
    u32 base_layer = v->min_layer < im->layers ? v->min_layer : 0;
    u32 nlayers = v->num_layers && base_layer + v->num_layers <= im->layers ? v->num_layers : im->layers - base_layer;
    VkImageViewUsageCreateInfo uci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    if (kind == VIEW_ATTACHMENT) {
        bool is3d = im->info.target == TEX_3D;
        ci.viewType = nlayers > 1 || is3d ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        if (im->info.target == TEX_1D || im->info.target == TEX_1D_ARRAY) ci.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        nlevels = 1;
        ci.subresourceRange = (VkImageSubresourceRange){im->aspects, base_level, 1, base_layer, nlayers};
        uci.usage = im->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    } else {
        ci.viewType = view_type(v->target);
        if (ci.viewType == VK_IMAGE_VIEW_TYPE_CUBE && nlayers < 6) ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        VkImageAspectFlags asp = im->aspects;
        if (asp == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
            asp = v->ds_mode == 1 ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        ci.subresourceRange = (VkImageSubresourceRange){asp, base_level, nlevels, base_layer, nlayers};
        VkComponentSwizzle sw[4];
        for (int i = 0; i < 4; i++) {
            u8 s = v->swizzle[i];
            sw[i] = s <= 5 ? (VkComponentSwizzle)(s + 1) : VK_COMPONENT_SWIZZLE_IDENTITY;
            if (s == 5 && format_has_x(v->format)) sw[i] = VK_COMPONENT_SWIZZLE_ONE;
        }
        ci.components = (VkComponentMapping){sw[0], sw[1], sw[2], sw[3]};
        uci.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if (!uci.usage) return NULL;
    ci.pNext = &uci;
    VkImageView view;
    if (vkCreateImageView(vk.dev, &ci, NULL, &view) != VK_SUCCESS) {
        hlog(0, "gpu: image view (format %u, kind %u) failed", v->format, kind);
        return NULL;
    }
    if (im->nviews == im->capviews) im->views = realloc(im->views, (im->capviews = im->capviews * 2 + 4) * sizeof *im->views);
    im->views[im->nviews++] = (View){*v, kind, view};
    return view;
}

/* fill an image from the guest memory under it (block-linear or pitch layout) */
static void image_upload(Image *im) {
    if (im->valid) return;
    im->valid = true;
    TexLayout L;
    tex_layout(&im->info, &L);
    const u8 *src = gpu_cpu(im->info.addr, L.size);
    if (!src) return;                   /* GPU-only memory: nothing to read */
    if (format_is_astc(im->info.format)) {
        static bool logged;
        if (!logged) { logged = true; hlog(0, "gpu: ASTC textures are not decoded yet"); }
        return;
    }
    if (L.bpb != 1 && L.bpb != 2 && L.bpb != 4 && L.bpb != 8 && L.bpb != 16) return;
    u64 total = 0;
    for (u32 l = 0; l < L.levels; l++) total += (u64)L.level[l].w * L.level[l].h * L.level[l].d * L.bpb;
    total *= L.layers;
    VkBuffer buf;
    u64 off;
    u8 *dst = staging(total, &buf, &off);
    VkBufferImageCopy regions[16];
    u64 at = 0;
    u32 nr = 0;
    for (u32 l = 0; l < L.levels; l++) {
        const TexLevel *lv = &L.level[l];
        u64 pitch = (u64)lv->w * L.bpb;
        for (u32 layer = 0; layer < L.layers; layer++) {
            const u8 *ls = src + (u64)layer * L.layer_size + lv->offset;
            u64 avail = L.size - ((u64)layer * L.layer_size + lv->offset);
            u8 *ld = dst + at + (u64)layer * pitch * lv->h * lv->d;
            if (L.linear) {
                for (u32 y = 0; y < lv->h * lv->d; y++) memcpy(ld + y * pitch, ls + y * lv->row_pitch, pitch);
            } else {
                deswizzle_level(lv, L.bpb, ls, avail, ld, pitch);
            }
        }
        u32 w = im->info.width >> l ? im->info.width >> l : 1;
        u32 h = tex_height(&im->info) >> l ? tex_height(&im->info) >> l : 1;
        u32 d = tex_depth(&im->info) >> l ? tex_depth(&im->info) >> l : 1;
        VkImageAspectFlags asp = im->aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ?
                                 VK_IMAGE_ASPECT_DEPTH_BIT : im->aspects;
        regions[nr++] = (VkBufferImageCopy){off + at, lv->w * L.bw, lv->h * L.bh, {asp, l, 0, L.layers}, {0, 0, 0},
                                            {w, h, d}};
        at += pitch * lv->h * lv->d * L.layers;
    }
    if (im->aspects & VK_IMAGE_ASPECT_STENCIL_BIT && im->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) return;  /* packed D+S */
    end_rendering();
    vkCmdCopyBufferToImage(cmd(), buf, im->image, VK_IMAGE_LAYOUT_GENERAL, nr, regions);
    barrier();
}

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
    Shaders *program;
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
} State;

static State st;                        /* the GPU's state carries over between command buffers, as on NVN */
static u64 draws, draws_skipped;

static void state_reset(State *s) {
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

static void end_rendering(void) {
    if (!st.rendering) return;
    vkCmdEndRendering(cmd());
    st.rendering = false;
    barrier();
}

static VkImageView rt_view(const TexRef *r, Image **out) {
    Image *im = image_get(&r->t);
    if (out) *out = im;
    if (!im) return NULL;
    return image_view(im, &r->v, VIEW_ATTACHMENT);
}

static bool begin_rendering(void) {
    if (st.rendering) return true;
    VkRenderingAttachmentInfo color[8], depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    u32 w = ~0u, h = ~0u, layers = ~0u;
    bool any = false;
    for (u32 i = 0; i < st.nrt; i++) {
        color[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        Image *im;
        VkImageView v = st.rt[i].t.addr ? rt_view(&st.rt[i], &im) : NULL;
        if (!v) continue;
        color[i].imageView = v;
        color[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        color[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        u32 l = st.rt[i].v.base_level;
        u32 iw = im->info.width >> l, ih = tex_height(&im->info) >> l;
        if (iw < w) w = iw ? iw : 1;
        if (ih < h) h = ih ? ih : 1;
        u32 nl = st.rt[i].v.num_layers ? st.rt[i].v.num_layers : 1;
        if (nl < layers) layers = nl;
        im->valid = true;
        any = true;
    }
    bool has_d = false, has_s = false;
    if (st.has_depth) {
        Image *im;
        VkImageView v = rt_view(&st.depth, &im);
        if (v) {
            depth.imageView = v;
            depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            has_d = im->aspects & VK_IMAGE_ASPECT_DEPTH_BIT;
            has_s = im->aspects & VK_IMAGE_ASPECT_STENCIL_BIT;
            u32 l = st.depth.v.base_level;
            u32 iw = im->info.width >> l, ih = tex_height(&im->info) >> l;
            if (iw < w) w = iw ? iw : 1;
            if (ih < h) h = ih ? ih : 1;
            u32 nl = st.depth.v.num_layers ? st.depth.v.num_layers : 1;
            if (nl < layers) layers = nl;
            im->valid = true;
            any = true;
        }
    }
    if (!any) return false;
    st.area = (VkRect2D){{0, 0}, {w, h}};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = st.area, .layerCount = layers,
                          .colorAttachmentCount = st.nrt, .pColorAttachments = color,
                          .pDepthAttachment = has_d ? &depth : NULL, .pStencilAttachment = has_s ? &depth : NULL};
    vkCmdBeginRendering(cmd(), &ri);
    st.rendering = true;
    return true;
}

static VkRect2D clear_rect(void) {
    s32 x0 = st.scissor[0][0], y0 = st.scissor[0][1];
    s64 x1 = (s64)x0 + st.scissor[0][2], y1 = (s64)y0 + st.scissor[0][3];
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (s64)st.area.extent.width) x1 = st.area.extent.width;
    if (y1 > (s64)st.area.extent.height) y1 = st.area.extent.height;
    if (x1 <= x0 || y1 <= y0) return (VkRect2D){{0, 0}, {0, 0}};
    return (VkRect2D){{x0, y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}};
}

static VkClearColorValue clear_value(u32 type, const u32 v[4]) {
    VkClearColorValue c;
    memcpy(&c, v, 16);
    (void)type;                         /* float, int and uint values are all 4 x 32 bits as given */
    return c;
}

static void do_clear_color(const CmdClearColor *p) {
    if (p->index >= st.nrt || !st.rt[p->index].t.addr || !begin_rendering()) return;
    VkRect2D r = clear_rect();
    if (!r.extent.width) return;
    if ((p->mask & 0xf) == 0) return;
    if ((p->mask & 0xf) != 0xf) {
        static bool logged;
        if (!logged) { logged = true; hlog(1, "gpu: ClearColor with channel mask %x: clearing all channels", p->mask); }
    }
    static int logged;
    if (logged < 12) {
        logged++;
        float f[4];
        memcpy(f, p->v, 16);
        hlog(1, "gpu: clear %ux%u fmt %u at %llx to (%g %g %g %g) rect %d,%d %ux%u", st.rt[p->index].t.width,
             st.rt[p->index].t.height, st.rt[p->index].t.format, st.rt[p->index].t.addr, f[0], f[1], f[2], f[3],
             r.offset.x, r.offset.y, r.extent.width, r.extent.height);
    }
    VkClearAttachment a = {VK_IMAGE_ASPECT_COLOR_BIT, p->index, {.color = clear_value(p->type, p->v)}};
    VkClearRect cr = {r, 0, 1};
    vkCmdClearAttachments(cmd(), 1, &a, 1, &cr);
}

static void do_clear_depth_stencil(const CmdClearDS *p) {
    if (!st.has_depth || !begin_rendering()) return;
    Image *im = image_get(&st.depth.t);
    if (!im) return;
    VkImageAspectFlags asp = 0;
    if (p->depth_mask && im->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (p->stencil_mask & 0xff && im->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
    VkRect2D r = clear_rect();
    if (!asp || !r.extent.width) return;
    VkClearAttachment a = {asp, 0, {.depthStencil = {p->depth, p->stencil}}};
    VkClearRect cr = {r, 0, 1};
    vkCmdClearAttachments(cmd(), 1, &a, 1, &cr);
}

/* ClearTexture: a region of a level/layers, outside the render targets */
static void do_clear_texture(const CmdClearTexture *p) {
    Image *im = image_get(&p->tex.t);
    if (!im) return;
    end_rendering();
    u32 level = p->tex.v.base_level;
    u32 layer = p->tex.v.min_layer + (p->tex.t.target == TEX_3D ? 0 : (u32)p->r.z);
    u32 nlayers = p->tex.t.target == TEX_3D ? 1 : (p->r.d > 0 ? (u32)p->r.d : 1);
    VkImageSubresourceRange range = {im->aspects, level, 1, layer, nlayers};
    if (im->aspects & VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue c = clear_value(p->type, p->v);
        vkCmdClearColorImage(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &range);
    } else {
        float d;
        memcpy(&d, &p->v[0], 4);
        VkClearDepthStencilValue v = {d, p->v[1]};
        vkCmdClearDepthStencilImage(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    }
    im->valid = true;
    barrier();
}

static VkImageAspectFlags copy_aspect(const Image *im) {
    return im->aspects == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) ? VK_IMAGE_ASPECT_DEPTH_BIT : im->aspects;
}

static VkBufferImageCopy bt_region(const CmdCopyBT *p, const Image *im, u64 off) {
    u32 bpb, bw, bh;
    format_info(p->tex.t.format, &bpb, &bw, &bh);
    if (!bpb) bpb = 4;
    u32 row_len = p->row_stride > 0 ? (u32)(p->row_stride / bpb * bw) : 0;
    u32 img_h = p->image_stride > 0 && p->row_stride > 0 ? (u32)(p->image_stride / p->row_stride * bh) : 0;
    bool is3d = p->tex.t.target == TEX_3D;
    u32 layer = p->tex.v.min_layer + (is3d ? 0 : (u32)p->r.z);
    VkBufferImageCopy r = {off, row_len, img_h,
        {copy_aspect(im), p->tex.v.base_level, layer, is3d ? 1 : (p->r.d > 0 ? (u32)p->r.d : 1)},
        {p->r.x, p->r.y, is3d ? p->r.z : 0},
        {(u32)p->r.w, (u32)(p->r.h > 0 ? p->r.h : 1), is3d ? (u32)(p->r.d > 0 ? p->r.d : 1) : 1}};
    return r;
}

static void do_copy_buffer_to_texture(const CmdCopyBT *p) {
    Image *im = image_get(&p->tex.t);
    VkBuffer buf;
    u64 off;
    if (!im || !gpu_buffer(p->buffer, &buf, &off) || p->r.w <= 0) return;
    if (format_is_astc(p->tex.t.format)) return;
    end_rendering();
    VkBufferImageCopy r = bt_region(p, im, off);
    vkCmdCopyBufferToImage(cmd(), buf, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    im->valid = true;
    barrier();
}

static void do_copy_texture_to_buffer(const CmdCopyBT *p) {
    Image *im = image_get(&p->tex.t);
    VkBuffer buf;
    u64 off;
    if (!im || !gpu_buffer(p->buffer, &buf, &off) || p->r.w <= 0) return;
    image_upload(im);
    end_rendering();
    VkBufferImageCopy r = bt_region(p, im, off);
    vkCmdCopyImageToBuffer(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &r);
    barrier();
}

static void do_copy_texture_to_texture(const CmdCopyTT *p) {
    Image *s = image_get(&p->src.t), *d = image_get(&p->dst.t);
    if (!s || !d || p->sr.w <= 0 || p->dr.w <= 0) return;
    image_upload(s);
    end_rendering();
    bool s3 = p->src.t.target == TEX_3D, d3 = p->dst.t.target == TEX_3D;
    VkImageSubresourceLayers sl = {copy_aspect(s), p->src.v.base_level, p->src.v.min_layer + (s3 ? 0 : (u32)p->sr.z),
                                   s3 ? 1 : (p->sr.d > 0 ? (u32)p->sr.d : 1)};
    VkImageSubresourceLayers dl = {copy_aspect(d), p->dst.v.base_level, p->dst.v.min_layer + (d3 ? 0 : (u32)p->dr.z),
                                   d3 ? 1 : (p->dr.d > 0 ? (u32)p->dr.d : 1)};
    s32 sh = p->sr.h > 0 ? p->sr.h : 1, dh = p->dr.h > 0 ? p->dr.h : 1;
    s32 sd = s3 ? (p->sr.d > 0 ? p->sr.d : 1) : 1, dd = d3 ? (p->dr.d > 0 ? p->dr.d : 1) : 1;
    u32 sb, db, bw, bh;
    format_info(p->src.t.format, &sb, &bw, &bh);
    format_info(p->dst.t.format, &db, &bw, &bh);
    bool same = p->sr.w == p->dr.w && sh == dh && sd == dd;
    if (same && (sb == db || s->format == d->format) && s->info.samples == d->info.samples) {
        VkImageCopy c = {sl, {p->sr.x, p->sr.y, s3 ? p->sr.z : 0}, dl, {p->dr.x, p->dr.y, d3 ? p->dr.z : 0},
                         {(u32)p->sr.w, (u32)sh, (u32)sd}};
        vkCmdCopyImage(cmd(), s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    } else {
        VkImageBlit b = {sl, {{p->sr.x, p->sr.y, s3 ? p->sr.z : 0}, {p->sr.x + p->sr.w, p->sr.y + sh, (s3 ? p->sr.z : 0) + sd}},
                         dl, {{p->dr.x, p->dr.y, d3 ? p->dr.z : 0}, {p->dr.x + p->dr.w, p->dr.y + dh, (d3 ? p->dr.z : 0) + dd}}};
        bool depth = s->aspects & ~VK_IMAGE_ASPECT_COLOR_BIT;
        vkCmdBlitImage(cmd(), s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       p->flags & 1 && !depth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    d->valid = true;
    barrier();
}

static void do_downsample(const CmdDownsample *p) {
    Image *s = image_get(&p->src.t), *d = image_get(&p->dst.t);
    if (!s || !d) return;
    end_rendering();
    u32 w = d->info.width, h = tex_height(&d->info);
    VkImageSubresourceLayers sl = {copy_aspect(s), p->src.v.base_level, p->src.v.min_layer, 1};
    VkImageSubresourceLayers dl = {copy_aspect(d), p->dst.v.base_level, p->dst.v.min_layer, 1};
    if (s->info.samples > 1 && d->info.samples <= 1 && s->aspects == VK_IMAGE_ASPECT_COLOR_BIT) {
        VkImageResolve r = {sl, {0, 0, 0}, dl, {0, 0, 0}, {w, h, 1}};
        vkCmdResolveImage(cmd(), s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else {
        VkImageBlit b = {sl, {{0, 0, 0}, {(s32)s->info.width, (s32)tex_height(&s->info), 1}},
                         dl, {{0, 0, 0}, {(s32)w, (s32)h, 1}}};
        vkCmdBlitImage(cmd(), s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       VK_FILTER_LINEAR);
    }
    d->valid = true;
    barrier();
}

/* GPU writes of small values into guest memory, in GPU order (outside rendering, as Vulkan requires) */
static void gpu_write(u64 addr, const void *data, u32 size) {
    VkBuffer buf;
    u64 off;
    if (!gpu_buffer(addr, &buf, &off)) {
        u8 *p = gpu_cpu(addr, size);
        if (p) memcpy(p, data, size);  /* not GPU-reachable: write it now */
        return;
    }
    end_rendering();
    vkCmdUpdateBuffer(cmd(), buf, off, (size + 3) & ~3u, data);
    barrier();
}

static u64 now_ns(void) {
    LARGE_INTEGER f, n;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&n);
    return (u64)((double)n.QuadPart * 1e9 / (double)f.QuadPart);
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
        case OP_PROGRAM: {
            u64 s;
            memcpy(&s, pay, 8);
            st.program = (Shaders *)(uintptr_t)s;
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
        case OP_XFB_BUFFERS: break;
        case OP_UPDATE_UBO: {
            CmdUpdateUbo u;
            memcpy(&u, pay, sizeof u);
            VkBuffer buf;
            u64 off;
            if (u.len && gpu_buffer(u.buffer + u.offset, &buf, &off)) {
                end_rendering();
                vkCmdUpdateBuffer(cmd(), buf, off, (u.len + 3) & ~3u, pay + sizeof u);
                barrier();
            }
            break;
        }
        case OP_DRAW_ARRAYS: case OP_DRAW_ELEMENTS: case OP_DRAW_ARRAYS_INDIRECT: case OP_DRAW_ELEMENTS_INDIRECT:
        case OP_MULTI_DRAW_INDIRECT: case OP_DRAW_TEXTURE: case OP_DRAW_XFB: case OP_DISPATCH: case OP_DISPATCH_INDIRECT:
            draws++;
            draws_skipped++;            /* shaders come with the translator */
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
            end_rendering();
            const TexRef *refs = (const TexRef *)(pay + sizeof h);
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
                barrier();
            }
            break;
        }
        case OP_DOWNSAMPLE: { PL(CmdDownsample); do_downsample(p); break; }
        case OP_BARRIER: end_rendering(); break;
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
        case OP_WAIT_EVENT: break;
        case OP_REPORT_COUNTER: {
            PL(CmdReport);
            /* NVNcounterData {u64 value, u64 timestamp}: timestamps are ours, other counters report "visible" */
            u64 v[2] = {p->counter == 0 ? 0 : 0x10000, now_ns()};
            if (p->addr) gpu_write(p->addr, v, 16);
            break;
        }
        case OP_RESET_COUNTER: break;
        case OP_REPORT_VALUE: { PL(CmdReport); if (p->addr) gpu_write(p->addr, &p->value, 4); break; }
        case OP_TEXTURE_POOL: { PL(CmdPool); st.texpool = *p; break; }
        case OP_SAMPLER_POOL: { PL(CmdPool); st.smppool = *p; break; }
        case OP_XFB: break;
        default:
            hlog(0, "gpu: unknown command %u (length %u) at %llx", op, len, at);
            return;
        }
        at += 4 + len;
    }
}

void gpu_submit(u64 queue, u32 count, const u64 *handles) {
    (void)queue;
    EnterCriticalSection(&gpu_cs);
    for (u32 i = 0; i < count; i++)
        if (handles[i]) run(handles[i], 0);
    flush();
    static u64 submits;
    if (++submits == 1) hlog(1, "gpu: first submission");
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
    if (value > submitted) gpu_flush(0);
    VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &timeline,
                              .pValues = &value};
    VkResult r = vkWaitSemaphores(vk.dev, &wi, timeout_ns);
    if (r < 0) fatal("vkWaitSemaphores: %d (device lost?)", r);
    return r == VK_SUCCESS;
}

/* the CPU wrote texels (nvnTextureWriteTexels): copy them into the image */
void gpu_texture_written(const TexInfo *t, const ViewInfo *v, const CopyRegion *r, const void *data,
                         s64 row_stride, s64 image_stride) {
    EnterCriticalSection(&gpu_cs);
    Image *im = image_get(t);
    if (im && r->w > 0 && !format_is_astc(t->format)) {
        u32 bpb, bw, bh;
        format_info(t->format, &bpb, &bw, &bh);
        u32 h = r->h > 0 ? (u32)r->h : 1, d = r->d > 0 ? (u32)r->d : 1;
        u64 row = row_stride > 0 ? (u64)row_stride : (u64)(r->w + bw - 1) / bw * bpb;
        u64 img = image_stride > 0 ? (u64)image_stride : row * ((h + bh - 1) / bh);
        u64 size = img * d;
        VkBuffer buf;
        u64 off;
        void *dst = staging(size, &buf, &off);
        memcpy(dst, data, size);
        CmdCopyBT c = {0};
        c.tex.t = *t;
        c.tex.v = *v;
        c.r = *r;
        c.row_stride = (s64)row;
        c.image_stride = (s64)img;
        VkBufferImageCopy reg = bt_region(&c, im, off);
        end_rendering();
        vkCmdCopyBufferToImage(cmd(), buf, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &reg);
        im->valid = true;
        barrier();
    }
    LeaveCriticalSection(&gpu_cs);
}

/* ---------------------------------------------------------------------------------------------- present */
/* MK8_DUMP=n[,m...]: write the presented window texture of those frames to build/frames/frame_<n>.ppm */
static void dump_frame(Image *im, u64 frame) {
    static char list[256];
    static int init;
    if (!init) {
        init = 1;
        const char *e = getenv("MK8_DUMP");
        if (e) snprintf(list, sizeof list, ",%s,", e);
    }
    char key[32];
    snprintf(key, sizeof key, ",%llu,", frame);
    if (!list[0] || !strstr(list, key) || !im || im->format != VK_FORMAT_R8G8B8A8_UNORM) return;
    u32 w = im->info.width, h = tex_height(&im->info);
    VkBuf b;
    vk_buffer((u64)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &b);
    VkBufferImageCopy r = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    vkCmdCopyImageToBuffer(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, b.buf, 1, &r);
    flush();
    gpu_wait(submitted, UINT64_MAX);
    char path[MAX_PATH], exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *slash = strrchr(exe, '\\');
    if (slash) *slash = 0;
    snprintf(path, sizeof path, "%s\\..\\frames", exe);
    CreateDirectoryA(path, NULL);
    snprintf(path, sizeof path, "%s\\..\\frames\\frame_%llu.ppm", exe, frame);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        const u8 *px = b.a.map;
        u8 *row = malloc(w * 3);
        for (u32 y = 0; y < h; y++) {
            for (u32 x = 0; x < w; x++) memcpy(row + 3 * x, px + ((u64)y * w + x) * 4, 3);
            fwrite(row, 1, w * 3, f);
        }
        free(row);
        fclose(f);
        hlog(1, "gpu: frame %llu written to %s", frame, path);
    }
    vk_buffer_free(&b);
}

void gpu_present(u64 queue, Window *w, u32 index) {
    (void)queue;
    EnterCriticalSection(&gpu_cs);
    Image *im = image_get(&w->info[index]);
    if (im) image_upload(im);
    flush();
    static u64 frames;
    dump_frame(im, frames + 1);
    if (++frames % 600 == 0) hlog(1, "gpu: %llu draws so far (%llu skipped: no shaders yet), %d images", draws,
                                  draws_skipped, nimages);
    s32 cw = w->crop[2] > 0 ? w->crop[2] : (s32)w->info[index].width;
    s32 ch = w->crop[3] > 0 ? w->crop[3] : (s32)w->info[index].height;
    vk_present(im ? im->image : NULL, w->crop[0], w->crop[1], cw, ch, timeline, submitted);
    LeaveCriticalSection(&gpu_cs);
}

void gpu_window_created(Window *w) { (void)w; }
void gpu_shaders_set(ShaderCode *s) { (void)s; }

void gpu_init(void) {
    InitializeCriticalSection(&gpu_cs);
    VkSemaphoreTypeCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                     .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci};
    VKCHECK(vkCreateSemaphore(vk.dev, &sci, NULL, &timeline));
    for (int i = 0; i < SLOTS; i++) {
        VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = vk.family,
                                       .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT};
        VKCHECK(vkCreateCommandPool(vk.dev, &pci, NULL, &slots[i].pool));
        VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = slots[i].pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VKCHECK(vkAllocateCommandBuffers(vk.dev, &ai, &slots[i].cmd));
        vk_buffer(STAGING_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &slots[i].staging);
    }
    state_reset(&st);
    gpu_ready = true;
    hlog(1, "gpu: ready");
}
