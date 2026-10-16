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
#include "astc.h"
#include <stdlib.h>
#include <windows.h>

static CRITICAL_SECTION gpu_cs;
static u64 frame_no;                    /* frames presented so far */
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
    VkBuf extra[256];                   /* staging that did not fit */
    int nextra;
    u64 extra_used;
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
    u64 rt_frame;                       /* the frame (frame_no + 1) it was last a render target in */
    bool gpu_written;                   /* rendered, cleared or copied into: guest memory is not its source */
    u64 size;                           /* bytes of guest memory it covers */
    View *views;
    int nviews, capviews;
    struct Image *next;
} Image;
#define IMAGE_BUCKETS 4096
static Image *buckets[IMAGE_BUCKETS];
static int nimages;

/* what identifies an image: its storage and shape. Textures over the same memory that differ only in how many
 * mip levels they declare (a render target view and the mipmapped texture it fills) share one image. */
static void image_key(const TexInfo *t, TexInfo *k) {
    *k = *t;
    k->levels = 0;
    k->flags &= TEX_FLAG_LINEAR;
}

static u32 info_hash(const TexInfo *t0) {
    TexInfo kk;
    image_key(t0, &kk);
    const TexInfo *t = &kk;
    const u8 *b = (const u8 *)t;
    u32 h = 2166136261u;
    for (size_t i = 0; i < sizeof *t; i++) h = (h ^ b[i]) * 16777619u;
    return h % IMAGE_BUCKETS;
}

static u64 image_epoch;                 /* bumped whenever images are destroyed */

/* every image by storage address (sub-resource lookups) */
static Image **byaddr;
static int nbyaddr, capbyaddr;

static int byaddr_lower(u64 a) {          /* first index with addr >= a */
    int lo = 0, hi = nbyaddr;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (byaddr[m]->info.addr < a) lo = m + 1; else hi = m;
    }
    return lo;
}
static void byaddr_add(Image *im) {
    if (nbyaddr == capbyaddr) byaddr = realloc(byaddr, (capbyaddr = capbyaddr * 2 + 1024) * sizeof *byaddr);
    int i = byaddr_lower(im->info.addr);
    memmove(&byaddr[i + 1], &byaddr[i], (nbyaddr - i) * sizeof *byaddr);
    byaddr[i] = im;
    nbyaddr++;
}
static void byaddr_remove(Image *im) {
    for (int i = byaddr_lower(im->info.addr); i < nbyaddr && byaddr[i]->info.addr == im->info.addr; i++)
        if (byaddr[i] == im) {
            memmove(&byaddr[i], &byaddr[i + 1], (nbyaddr - i - 1) * sizeof *byaddr);
            nbyaddr--;
            return;
        }
}

static void image_destroy(Image *im) {
    image_epoch++;
    byaddr_remove(im);
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
                static int logged;
                if (im->gpu_written && logged < 20) {
                    logged++;
                    hlog(1, "gpu: frame %llu: rendered image %ux%ux%u f%u at %llx dropped (pool %llx-%llx changed)", frame_no + 1,
                         im->info.width, im->info.height, im->info.depth, im->info.format, im->info.addr, lo, hi);
                }
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

static Image *image_create(const TexInfo *t, u32 h);

/* is texture t a part of image I: its storage starts at one of I's (layer, level) and the shapes agree?
 * (cube faces rendered as 2D textures, then sampled as the cube; mip levels as textures of their own) */
static bool sub_of(const Image *I, const TexInfo *t, u32 *layer, u32 *level) {
    if (t->addr < I->info.addr || t->addr >= I->info.addr + I->size || I->info.target == TEX_3D || t->target == TEX_3D)
        return false;
    if ((t->flags ^ I->info.flags) & TEX_FLAG_LINEAR || t->flags & TEX_FLAG_LINEAR) return false;
    if ((t->samples > 1 ? t->samples : 1) != (I->info.samples > 1 ? I->info.samples : 1)) return false;
    u32 b1, w1, h1, b2, w2, h2;
    format_info(t->format, &b1, &w1, &h1);
    format_info(I->info.format, &b2, &w2, &h2);
    if (b1 != b2 || w1 != w2 || h1 != h2 || !b1) return false;
    if (format_aspects(usable_format(t->format)) != I->aspects) return false;
    TexLayout L, M;
    tex_layout(&I->info, &L);
    tex_layout(t, &M);
    u64 off = t->addr - I->info.addr;
    u32 ly = (u32)(off / L.layer_size);
    u64 rem = off % L.layer_size;
    for (u32 l = 0; l < L.levels; l++) {
        if (L.level[l].offset != rem) continue;
        u32 iw = I->info.width >> l ? I->info.width >> l : 1, ih = tex_height(&I->info) >> l ? tex_height(&I->info) >> l : 1;
        if (iw != t->width || ih != tex_height(t)) return false;
        if (M.level[0].bh != L.level[l].bh) return false;
        if (ly + M.layers > I->layers || l + M.levels > I->levels) return false;
        *layer = ly;
        *level = l;
        return true;
    }
    return false;
}

static void bucket_unlink(Image *im) {
    for (Image **pp = &buckets[info_hash(&im->info)]; *pp; pp = &(*pp)->next)
        if (*pp == im) { *pp = im->next; return; }
}

static Image *image_get(const TexInfo *t);

/* the image holding texture t, and where in it t starts */
static Image *image_find(const TexInfo *t, u32 *layer, u32 *level) {
    *layer = *level = 0;
    u32 h = info_hash(t);
    TexInfo k, ki;
    image_key(t, &k);
    for (Image *im = buckets[h]; im; im = im->next) {
        image_key(&im->info, &ki);
        if (!memcmp(&ki, &k, sizeof k)) return image_get(t);
    }
    /* inside a bigger image? (scan back over images that start at or below t) */
    int i = byaddr_lower(t->addr + 1);
    for (int n = 0; i > 0 && n < 64; n++) {
        Image *I = byaddr[--i];
        if (sub_of(I, t, layer, level)) return I;
    }
    Image *im = image_get(t);
    if (!im) return NULL;
    /* a new container takes over the parts of it that exist as images of their own */
    bool took = false;
    for (int j = byaddr_lower(im->info.addr); j < nbyaddr && byaddr[j]->info.addr < im->info.addr + im->size;) {
        Image *o = byaddr[j];
        u32 ly, lv;
        if (o == im || !sub_of(im, &o->info, &ly, &lv)) { j++; continue; }
        if (o->valid) {
            end_rendering();
            if (!took) barrier();
            for (u32 l = 0; l < o->levels && lv + l < im->levels; l++) {
                u32 w = o->info.width >> l ? o->info.width >> l : 1, hh = tex_height(&o->info) >> l ? tex_height(&o->info) >> l : 1;
                VkImageCopy c = {{o->aspects, l, 0, o->layers}, {0, 0, 0}, {im->aspects, lv + l, ly, o->layers}, {0, 0, 0},
                                 {w, hh, 1}};
                vkCmdCopyImage(cmd(), o->image, VK_IMAGE_LAYOUT_GENERAL, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            }
            if (o->gpu_written) im->gpu_written = true;
            im->valid = true;
            took = true;
        }
        bucket_unlink(o);
        image_destroy(o);               /* removes it from byaddr: j now names the next one */
    }
    if (took) barrier();
    return im;
}

/* a texture reference resolved to its image: the view moved to t's place in the image */
static Image *image_ref(const TexRef *r, ViewInfo *v) {
    u32 layer, level;
    Image *im = image_find(&r->t, &layer, &level);
    *v = r->v;
    v->base_level = (u16)(v->base_level + level);
    v->min_layer = (u16)(v->min_layer + layer);
    return im;
}

static Image *image_get(const TexInfo *t) {
    u32 h = info_hash(t);
    TexInfo k, ki;
    image_key(t, &k);
    Image **pp;
    for (pp = &buckets[h]; *pp; pp = &(*pp)->next) {
        image_key(&(*pp)->info, &ki);
        if (!memcmp(&ki, &k, sizeof k)) break;
    }
    Image *old = *pp;
    if (old && (t->levels ? t->levels : 1) <= old->levels) return old;
    if (!old) return image_create(t, h);
    /* more mip levels than the image has: a bigger image, with the levels so far copied over */
    TexInfo big = old->info;
    big.levels = t->levels;
    *pp = old->next;                    /* out of the bucket: image_create inserts the new one */
    Image *im = image_create(&big, h);
    if (!im) { old->next = buckets[h]; buckets[h] = old; return old; }
    end_rendering();
    for (u32 l = 0; l < old->levels; l++) {
        u32 w = old->info.width >> l ? old->info.width >> l : 1, hh = tex_height(&old->info) >> l ? tex_height(&old->info) >> l : 1;
        u32 d = tex_depth(&old->info) >> l ? tex_depth(&old->info) >> l : 1;
        VkImageCopy c = {{old->aspects, l, 0, old->layers}, {0, 0, 0}, {im->aspects, l, 0, im->layers}, {0, 0, 0}, {w, hh, d}};
        vkCmdCopyImage(cmd(), old->image, VK_IMAGE_LAYOUT_GENERAL, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    barrier();
    im->valid = old->valid;
    im->rt_frame = old->rt_frame;
    image_destroy(old);
    return im;
}

static Image *image_create(const TexInfo *t, u32 h) {
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
    im->size = texture_storage_size(t);
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
    end_rendering();
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = im->image, .subresourceRange = {im->aspects, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cmd(), &d);
    /* defined contents (zeros) until something writes it: undefined memory can hold NaNs that spread */
    VkImageSubresourceRange all = {im->aspects, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
    if (im->aspects & VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue z = {{0, 0, 0, 0}};
        vkCmdClearColorImage(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &all);
    } else {
        VkClearDepthStencilValue z = {1, 0};
        vkCmdClearDepthStencilImage(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &all);
    }
    barrier();
    im->next = buckets[h];
    buckets[h] = im;
    byaddr_add(im);
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
    /* a 3D image drawn into is seen as a 2D array of its depth slices (the view's layers pick slices) */
    u32 nl_img = kind == VIEW_ATTACHMENT && im->info.target == TEX_3D ? tex_depth(&im->info) >> v->base_level : im->layers;
    if (!nl_img) nl_img = 1;
    u32 base_layer = v->min_layer < nl_img ? v->min_layer : 0;
    u32 nlayers = v->num_layers && base_layer + v->num_layers <= nl_img ? v->num_layers : nl_img - base_layer;
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
/* ASTC: deswizzle the blocks, decode them to RGBA8 (the image's format), copy level by level */
static void upload_astc(Image *im, const TexLayout *L, const u8 *src) {
    bool srgb = im->info.format >= 135;
    VkBufferImageCopy regions[16];
    u32 nr = 0;
    for (u32 l = 0; l < L->levels; l++) {
        const TexLevel *lv = &L->level[l];
        u32 w = im->info.width >> l ? im->info.width >> l : 1, h = tex_height(&im->info) >> l ? tex_height(&im->info) >> l : 1;
        u64 bpitch = (u64)lv->w * 16, lsize = (u64)w * h * 4;
        u8 *blocks = malloc(bpitch * lv->h);
        VkBuffer buf;
        u64 off;
        u8 *dst = staging(lsize * L->layers, &buf, &off);
        for (u32 layer = 0; layer < L->layers; layer++) {
            u64 at = (u64)layer * L->layer_size + lv->offset;
            if (L->linear) {
                for (u32 y = 0; y < lv->h; y++) memcpy(blocks + y * bpitch, src + at + y * lv->row_pitch, bpitch);
            } else {
                deswizzle_level(lv, 16, src + at, L->size - at, blocks, bpitch);
            }
            astc_decode(blocks, lv->w, lv->h, L->bw, L->bh, srgb, dst + layer * lsize, w, h);
            static int dumped;
            if (l == 0 && layer == 0 && getenv("MK8_DUMP_ASTC") && dumped < 40) {
                char path[MAX_PATH];
                snprintf(path, sizeof path, "E:/projects/ns-port/build/frames/astc_%02d_%ux%u_f%u.pam", dumped++, w, h,
                         im->info.format);
                FILE *f = fopen(path, "wb");
                if (f) {
                    fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", w, h);
                    fwrite(dst, 1, lsize, f);
                    fclose(f);
                }
            }
        }
        free(blocks);
        regions[nr++] = (VkBufferImageCopy){off, w, h, {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, L->layers}, {0, 0, 0}, {w, h, 1}};
        end_rendering();
        vkCmdCopyBufferToImage(cmd(), buf, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &regions[nr - 1]);
    }
    barrier();
}

static void image_upload(Image *im) {
    if (im->valid) return;
    im->valid = true;
    TexLayout L;
    tex_layout(&im->info, &L);
    const u8 *src = gpu_cpu(im->info.addr, L.size);
    if (!src) return;                   /* GPU-only memory: nothing to read */
    if (format_is_astc(im->info.format)) {
        upload_astc(im, &L, src);
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
    VkFormat rt_format[8], depth_format, stencil_format;     /* of the rendering in progress */
    u32 samples;
} State;

static State st;                        /* the GPU's state carries over between command buffers, as on NVN */
static u64 draws, draws_skipped, dispatches;

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

/* MK8_WATCH=<hex address>: log every GPU write to that texture storage */
static u64 watch_addr = ~0ull;
static void watch(const char *what, const TexRef *r) {
    if (watch_addr == ~0ull) {
        const char *e = getenv("MK8_WATCH");
        watch_addr = e ? strtoull(e, NULL, 16) : 0;
    }
    if (!watch_addr || r->t.addr != watch_addr) return;
    hlog(0, "watch: frame %llu %s %ux%ux%u f%u target %u view level %u layers %u+%u", frame_no + 1, what, r->t.width,
         r->t.height, r->t.depth, r->v.format, r->t.target, r->v.base_level, r->v.min_layer, r->v.num_layers);
}

static VkImageView rt_view(const TexRef *r, Image **out, u32 *level) {
    watch("render target", r);
    ViewInfo v;
    Image *im = image_ref(r, &v);
    if (out) *out = im;
    if (level) *level = v.base_level;
    if (!im) return NULL;
    return image_view(im, &v, VIEW_ATTACHMENT);
}

static bool begin_rendering(void) {
    if (st.rendering) return true;
    VkRenderingAttachmentInfo color[8], depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    u32 w = ~0u, h = ~0u, layers = ~0u;
    memset(st.rt_format, 0, sizeof st.rt_format);
    st.depth_format = st.stencil_format = VK_FORMAT_UNDEFINED;
    st.samples = 1;
    bool any = false;
    for (u32 i = 0; i < st.nrt; i++) {
        color[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        Image *im;
        u32 l;
        VkImageView v = st.rt[i].t.addr ? rt_view(&st.rt[i], &im, &l) : NULL;
        if (!v) continue;
        color[i].imageView = v;
        st.rt_format[i] = usable_format(st.rt[i].v.format) != VK_FORMAT_UNDEFINED ? usable_format(st.rt[i].v.format) : im->format;
        st.samples = im->info.samples > 1 ? im->info.samples : 1;
        color[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        color[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        u32 iw = im->info.width >> l, ih = tex_height(&im->info) >> l;
        if (iw < w) w = iw ? iw : 1;
        if (ih < h) h = ih ? ih : 1;
        u32 nl = st.rt[i].v.num_layers ? st.rt[i].v.num_layers : 1;
        if (nl < layers) layers = nl;
        im->valid = true;
        im->rt_frame = frame_no + 1;
        im->gpu_written = true;
        any = true;
    }
    bool has_d = false, has_s = false;
    if (st.has_depth) {
        Image *im;
        u32 l;
        VkImageView v = rt_view(&st.depth, &im, &l);
        if (v) {
            depth.imageView = v;
            depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            has_d = im->aspects & VK_IMAGE_ASPECT_DEPTH_BIT;
            has_s = im->aspects & VK_IMAGE_ASPECT_STENCIL_BIT;
            if (has_d) st.depth_format = im->format;
            if (has_s) st.stencil_format = im->format;
            st.samples = im->info.samples > 1 ? im->info.samples : 1;
            u32 iw = im->info.width >> l, ih = tex_height(&im->info) >> l;
            if (iw < w) w = iw ? iw : 1;
            if (ih < h) h = ih ? ih : 1;
            u32 nl = st.depth.v.num_layers ? st.depth.v.num_layers : 1;
            if (nl < layers) layers = nl;
            im->valid = true;
            im->rt_frame = frame_no + 1;
            im->gpu_written = true;
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
    u32 ly, lv;
    Image *im = image_find(&st.depth.t, &ly, &lv);
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
    watch("ClearTexture", &p->tex);
    ViewInfo v;
    Image *im = image_ref(&p->tex, &v);
    if (!im) return;
    end_rendering();
    u32 level = v.base_level;
    u32 layer = v.min_layer + (p->tex.t.target == TEX_3D ? 0 : (u32)p->r.z);
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
    im->gpu_written = true;
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
    watch("CopyBufferToTexture", &p->tex);
    CmdCopyBT q = *p;
    Image *im = image_ref(&p->tex, &q.tex.v);
    p = &q;
    VkBuffer buf;
    u64 off;
    if (!im || !gpu_buffer(p->buffer, &buf, &off) || p->r.w <= 0) return;
    if (format_is_astc(p->tex.t.format)) return;
    end_rendering();
    VkBufferImageCopy r = bt_region(p, im, off);
    vkCmdCopyBufferToImage(cmd(), buf, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    im->valid = true;
    im->gpu_written = true;
    barrier();
}

static void do_copy_texture_to_buffer(const CmdCopyBT *p) {
    CmdCopyBT q = *p;
    Image *im = image_ref(&p->tex, &q.tex.v);
    p = &q;
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
    watch("CopyTextureToTexture (dst)", &p->dst);
    CmdCopyTT q = *p;
    Image *s = image_ref(&p->src, &q.src.v), *d = image_ref(&p->dst, &q.dst.v);
    p = &q;
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
    d->gpu_written = true;
    barrier();
}

static void do_downsample(const CmdDownsample *p) {
    watch("Downsample (dst)", &p->dst);
    CmdDownsample q = *p;
    Image *s = image_ref(&p->src, &q.src.v), *d = image_ref(&p->dst, &q.dst.v);
    p = &q;
    if (!s || !d) return;
    end_rendering();
    u32 w = p->dst.t.width, h = tex_height(&p->dst.t);
    VkImageSubresourceLayers sl = {copy_aspect(s), p->src.v.base_level, p->src.v.min_layer, 1};
    VkImageSubresourceLayers dl = {copy_aspect(d), p->dst.v.base_level, p->dst.v.min_layer, 1};
    if (s->info.samples > 1 && d->info.samples <= 1 && s->aspects == VK_IMAGE_ASPECT_COLOR_BIT) {
        VkImageResolve r = {sl, {0, 0, 0}, dl, {0, 0, 0}, {w, h, 1}};
        vkCmdResolveImage(cmd(), s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else {
        VkImageBlit b = {sl, {{0, 0, 0}, {(s32)p->src.t.width, (s32)tex_height(&p->src.t), 1}},
                         dl, {{0, 0, 0}, {(s32)w, (s32)h, 1}}};
        vkCmdBlitImage(cmd(), s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       VK_FILTER_LINEAR);
    }
    d->valid = true;
    d->gpu_written = true;
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

/* ---------------------------------------------------------------------------------------------- draws
 * One pipeline layout for every graphics pipeline:
 *   set 0  push descriptors: uniform buffers c[0..15] per stage (vertex at bindings 0-15, fragment 16-31);
 *          c[0] and c[2] are ours (storage buffer table, texture handles), c[1] the shader's embedded constants,
 *          c[3 + b] the game's uniform buffer b
 *   set 1  the current texture pool: one sampled image per pool entry (bindless, indexed by texture id)
 *   set 2  the current sampler pool: one sampler per entry
 *   push constants: the alpha test (fragment)
 * Pipelines depend only on the shaders and the attachment formats: all other state is dynamic. */
#define TEXPOOL_MAX 65536
#define SMPPOOL_MAX 4096
static VkDescriptorSetLayout set_ubo, set_tex, set_smp;
static VkPipelineLayout layout_gfx;
static VkDescriptorPool desc_pool;
static VkDescriptorSet empty_tex, empty_smp;
static VkPipelineCache pcache;
static u64 submit_serial;
static u32 window_origin, depth_mode;   /* NVN device modes (0: lower left, 0: near at -w) */

void gpu_device_modes(u32 origin, u32 depth) { window_origin = origin; depth_mode = depth; }

static void layouts_init(void) {
    VkDescriptorSetLayoutBinding ub[32];
    for (int i = 0; i < 32; i++)
        ub[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_ALL, NULL};
    VkDescriptorSetLayoutCreateInfo uci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 32, .pBindings = ub};
    VKCHECK(vkCreateDescriptorSetLayout(vk.dev, &uci, NULL, &set_ubo));
    VkDescriptorBindingFlags bf = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                  VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT |
                                  VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                                                        .bindingCount = 1, .pBindingFlags = &bf};
    VkDescriptorSetLayoutBinding tb = {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, TEXPOOL_MAX, VK_SHADER_STAGE_ALL, NULL};
    VkDescriptorSetLayoutCreateInfo tci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bfi,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, .bindingCount = 1, .pBindings = &tb};
    VKCHECK(vkCreateDescriptorSetLayout(vk.dev, &tci, NULL, &set_tex));
    VkDescriptorSetLayoutBinding sb = {0, VK_DESCRIPTOR_TYPE_SAMPLER, SMPPOOL_MAX, VK_SHADER_STAGE_ALL, NULL};
    tci.pBindings = &sb;
    VKCHECK(vkCreateDescriptorSetLayout(vk.dev, &tci, NULL, &set_smp));
    VkDescriptorSetLayout sets[3] = {set_ubo, set_tex, set_smp};
    VkPushConstantRange pr = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 3,
                                      .pSetLayouts = sets, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VKCHECK(vkCreatePipelineLayout(vk.dev, &lci, NULL, &layout_gfx));
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, TEXPOOL_MAX * 16}, {VK_DESCRIPTOR_TYPE_SAMPLER, SMPPOOL_MAX * 16}};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 64, .poolSizeCount = 2, .pPoolSizes = ps};
    VKCHECK(vkCreateDescriptorPool(vk.dev, &pci, NULL, &desc_pool));
    VkPipelineCacheCreateInfo cci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    VKCHECK(vkCreatePipelineCache(vk.dev, &cci, NULL, &pcache));
}

static VkDescriptorSet alloc_set(VkDescriptorSetLayout l, u32 count) {
    VkDescriptorSetVariableDescriptorCountAllocateInfo vi = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO, .descriptorSetCount = 1,
        .pDescriptorCounts = &count};
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, &vi, desc_pool, 1, &l};
    VkDescriptorSet s;
    if (vkAllocateDescriptorSets(vk.dev, &ai, &s) != VK_SUCCESS) fatal("gpu: out of descriptor sets");
    return s;
}

/* ---- samplers (one VkSampler per interned sampler state) */
static VkSampler *samplers;
static u32 nsamplers;

static VkSamplerAddressMode wrap_mode(u8 w) {
    switch (w) {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: case 3: case 4: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case 6: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

static VkSampler sampler_get(u32 idx) {
    if (idx >= nsamplers) {
        u32 n = idx + 64;
        samplers = realloc(samplers, n * sizeof *samplers);
        memset(samplers + nsamplers, 0, (n - nsamplers) * sizeof *samplers);
        nsamplers = n;
    }
    if (samplers[idx]) return samplers[idx];
    const SamplerState *s = sdesc_get(idx);
    if (!s) return NULL;
    VkSamplerCreateInfo ci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = s->mag_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.minFilter = (s->min_filter == 1 || s->min_filter == 3 || s->min_filter == 5) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = s->min_filter >= 4 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = wrap_mode(s->wrap[0]);
    ci.addressModeV = wrap_mode(s->wrap[1]);
    ci.addressModeW = wrap_mode(s->wrap[2]);
    ci.mipLodBias = s->lod_bias;
    ci.anisotropyEnable = s->max_aniso > 1;
    ci.maxAnisotropy = s->max_aniso > 16 ? 16 : s->max_aniso;
    ci.compareEnable = s->compare_mode == 1;
    ci.compareOp = s->compare_func >= 1 && s->compare_func <= 8 ? (VkCompareOp)(s->compare_func - 1) : VK_COMPARE_OP_ALWAYS;
    ci.minLod = s->lod_min < 0 ? 0 : s->lod_min;
    ci.maxLod = s->min_filter < 2 ? 0.25f : s->lod_max;     /* no mipmapping: the base level only */
    if (ci.maxLod < ci.minLod) ci.maxLod = ci.minLod;
    VkSamplerCustomBorderColorCreateInfoEXT bc = {VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (ci.addressModeU == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER || ci.addressModeV == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
        ci.addressModeW == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER) {
        memcpy(&bc.customBorderColor, s->border, 16);
        ci.borderColor = s->border_type ? VK_BORDER_COLOR_INT_CUSTOM_EXT : VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
        ci.pNext = &bc;
    }
    VkSamplerReductionModeCreateInfo rm = {VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
        .reductionMode = s->reduction == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : s->reduction == 2 ? VK_SAMPLER_REDUCTION_MODE_MAX :
                         VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE};
    if (s->reduction) { rm.pNext = ci.pNext; ci.pNext = &rm; }
    if (vkCreateSampler(vk.dev, &ci, NULL, &samplers[idx]) != VK_SUCCESS) {
        hlog(0, "gpu: vkCreateSampler failed (sampler %u)", idx);
        return NULL;
    }
    return samplers[idx];
}

/* ---- texture and sampler pools as descriptor sets, kept in step with the descriptors the game registers */
typedef struct DescSet {
    u64 addr;                           /* the pool's descriptor memory */
    u32 count;
    bool smp;
    VkDescriptorSet set;
    u32 *idx;                           /* interned descriptor written into each entry, ~0 none */
    u64 epoch, synced;
    struct DescSet *next;
} DescSet;
static DescSet *descsets;
static VkImageView dummy_view;
static VkSampler dummy_sampler;

/* every entry starts as a 1x1 black texture / a default sampler, so that no index a shader computes is unwritten */
static void dummies_init(void) {
    TexInfo t = {0};
    t.addr = 1;                         /* no guest memory */
    t.format = 37;                      /* RGBA8 */
    t.target = TEX_2D;
    t.width = t.height = t.depth = t.levels = 1;
    t.block_height = 0xff;
    Image *im = image_get(&t);
    ViewInfo v = {37, TEX_2D, 0, 1, 0, 1, {2, 3, 4, 5}, 0};
    dummy_view = image_view(im, &v, VIEW_SAMPLED);
    VkClearColorValue black = {{0, 0, 0, 1}};
    VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &r);
    im->valid = true;
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                               .minFilter = VK_FILTER_LINEAR, .maxLod = 16};
    VKCHECK(vkCreateSampler(vk.dev, &sci, NULL, &dummy_sampler));
}

static void descset_fill(VkDescriptorSet set, bool smp, u32 count) {
    VkDescriptorImageInfo info = {dummy_sampler, dummy_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo infos[256];
    for (int i = 0; i < 256; i++) infos[i] = info;
    for (u32 i = 0; i < count; i += 256) {
        VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
            .dstArrayElement = i, .descriptorCount = count - i < 256 ? count - i : 256,
            .descriptorType = smp ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = infos};
        vkUpdateDescriptorSets(vk.dev, 1, &w, 0, NULL);
    }
}

static DescSet *descset_get(const CmdPool *p, bool smp) {
    if (!p->addr) return NULL;
    for (DescSet *d = descsets; d; d = d->next)
        if (d->addr == p->addr && d->smp == smp) return d;
    DescSet *d = calloc(1, sizeof *d);
    d->addr = p->addr;
    d->smp = smp;
    d->count = p->count > (smp ? SMPPOOL_MAX : TEXPOOL_MAX) ? (smp ? SMPPOOL_MAX : TEXPOOL_MAX) : p->count;
    if (!d->count) d->count = 1;
    d->set = alloc_set(smp ? set_smp : set_tex, smp ? SMPPOOL_MAX : TEXPOOL_MAX);
    descset_fill(d->set, smp, smp ? SMPPOOL_MAX : TEXPOOL_MAX);
    d->idx = malloc(d->count * sizeof *d->idx);
    memset(d->idx, 0xff, d->count * sizeof *d->idx);
    d->epoch = ~0ull;
    d->next = descsets;
    descsets = d;
    hlog(1, "gpu: %s pool at %llx: %u entries", smp ? "sampler" : "texture", p->addr, d->count);
    return d;
}

static void descset_sync(DescSet *d) {
    if (!d || d->synced == submit_serial) return;
    d->synced = submit_serial;
    bool full = d->epoch != image_epoch;
    d->epoch = image_epoch;
    VkDescriptorImageInfo infos[256];
    VkWriteDescriptorSet writes[256];
    int n = 0;
    for (u32 i = 0; i < d->count; i++) {
        u64 a = d->addr + 32ull * i;
        u32 magic = LD32(a), x = LD32(a + 4);
        u32 want = magic == (d->smp ? SDESC_MAGIC : TDESC_MAGIC) ? x : ~0u;
        if (want == d->idx[i] && !full) continue;
        d->idx[i] = want;
        if (want == ~0u) continue;
        memset(&infos[n], 0, sizeof infos[n]);
        if (d->smp) {
            infos[n].sampler = sampler_get(want);
            if (!infos[n].sampler) continue;
        } else {
            const TexDesc *td = tdesc_get(want);
            if (!td || !pool_find(td->t.addr)) continue;      /* storage gone: leave the entry alone */
            TexRef tr = {td->t, td->v};
            ViewInfo v;
            Image *im = image_ref(&tr, &v);
            if (!im) continue;
            image_upload(im);
            infos[n].imageView = image_view(im, &v, VIEW_SAMPLED);
            infos[n].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            if (!infos[n].imageView) continue;
        }
        writes[n] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = d->set, .dstBinding = 0,
            .dstArrayElement = i, .descriptorCount = 1,
            .descriptorType = d->smp ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &infos[n]};
        if (++n == 256) { vkUpdateDescriptorSets(vk.dev, n, writes, 0, NULL); n = 0; }
    }
    if (n) vkUpdateDescriptorSets(vk.dev, n, writes, 0, NULL);
}

/* ---- shaders */
static char spv_dir[MAX_PATH];

static VkShaderModule shader_module(ShaderCode *s) {
    if (s->module) return s->module;
    if (s->missing) return NULL;
    if (!spv_dir[0]) {
        char exe[MAX_PATH];
        GetModuleFileNameA(NULL, exe, sizeof exe);
        char *slash = strrchr(exe, '\\');
        if (slash) *slash = 0;
        snprintf(spv_dir, sizeof spv_dir, "%s\\..\\shaders\\spv", exe);
    }
    char path[MAX_PATH + 32];
    snprintf(path, sizeof path, "%s\\%016llx.spv", spv_dir, s->hash);
    FILE *f = fopen(path, "rb");
    if (!f) {
        s->missing = true;
        hlog(1, "gpu: shader %016llx has no translation yet (run tools/shader/translate.py --spv)", s->hash);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u32 *code = malloc(n);
    fread(code, 1, n, f);
    fclose(f);
    for (long i = 5; i < n / 4;) {      /* what the shader touches: PhysicalStorageBufferAddresses, sampled images */
        u32 op = code[i] & 0xffff, wc = code[i] >> 16;
        if (op == 17 && wc == 2 && code[i + 1] == 5347) s->caps |= SHC_GLOBAL;
        if (op == 27) s->caps |= SHC_TEXTURE;
        i += wc ? wc : 1;
    }
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)n, .pCode = code};
    VkShaderModule m;
    VkResult r = vkCreateShaderModule(vk.dev, &ci, NULL, &m);
    free(code);
    if (r != VK_SUCCESS) {
        s->missing = true;
        hlog(0, "gpu: shader %016llx: vkCreateShaderModule failed", s->hash);
        return NULL;
    }
    s->module = m;
    return m;
}

/* ---- pipelines */
typedef struct PipeKey {
    VkShaderModule vs, fs;
    VkFormat color[8], depth, stencil;
    u32 ncolor, samples, minus_one;
} PipeKey;
typedef struct Pipe { PipeKey key; VkPipeline pipe; struct Pipe *next; } Pipe;
#define PIPE_BUCKETS 1024
static Pipe *pipes[PIPE_BUCKETS];
static u32 npipes;

static VkPipeline pipeline_get(const PipeKey *k) {
    const u8 *b = (const u8 *)k;
    u32 h = 2166136261u;
    for (size_t i = 0; i < sizeof *k; i++) h = (h ^ b[i]) * 16777619u;
    h %= PIPE_BUCKETS;
    for (Pipe *p = pipes[h]; p; p = p->next)
        if (!memcmp(&p->key, k, sizeof *k)) return p->pipe;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = k->vs, .pName = "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = k->fs, .pName = "main"}};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportDepthClipControlCreateInfoEXT dcc = {
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT, .negativeOneToOne = k->minus_one};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, &dcc};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState att[8];
    memset(att, 0, sizeof att);
    for (int i = 0; i < 8; i++) att[i].colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                              .attachmentCount = k->ncolor, .pAttachments = att};
    static const VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_LINE_WIDTH,
        VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BOUNDS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
        VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT, VK_DYNAMIC_STATE_POLYGON_MODE_EXT,
        VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT, VK_DYNAMIC_STATE_SAMPLE_MASK_EXT,
        VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT, VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT, VK_DYNAMIC_STATE_LOGIC_OP_EXT,
        VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
        VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT, VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                           .dynamicStateCount = sizeof dyn / sizeof *dyn, .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .colorAttachmentCount = k->ncolor,
        .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth, .stencilAttachmentFormat = k->stencil};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri,
        .stageCount = k->fs ? 2 : 1, .pStages = stages, .pInputAssemblyState = &ia, .pViewportState = &vp,
        .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = layout_gfx};
    VkPipeline pipe = NULL;
    VkResult r = vkCreateGraphicsPipelines(vk.dev, pcache, 1, &ci, NULL, &pipe);
    if (r != VK_SUCCESS) hlog(0, "gpu: vkCreateGraphicsPipelines: %d", r);
    Pipe *p = calloc(1, sizeof *p);
    p->key = *k;
    p->pipe = pipe;
    p->next = pipes[h];
    pipes[h] = p;
    if (++npipes % 100 == 0) hlog(1, "gpu: %u pipelines", npipes);
    return pipe;
}

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

static void skip_draw(const char *why) {
    draws_skipped++;
    static const char *seen[16];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == why) return;
    if (n < 16) seen[n++] = why;
    hlog(1, "gpu: draw skipped: %s", why);
}

/* the uniform buffers of one stage (push descriptors at binding base + i) */
static int stage_ubos(u32 stage, u32 base, const ShaderCode *sc, VkWriteDescriptorSet *w, VkDescriptorBufferInfo *bi) {
    VkBuffer sbuf;
    u64 soff;
    u8 *drv = staging(0x800, &sbuf, &soff);    /* c[0] at 0, c[2] at 0x600 */
    memset(drv, 0, 0x800);
    /* c[0]: buffer tables with device addresses {lo, hi, size}. Graphics: storage buffers at 0x110.
     * Compute (no cbuf slots for the game's uniform buffers there): uniform buffers at 0x220, storage at 0x310 */
    u32 ssbo_at = stage == ST_COMPUTE ? 0x310 : 0x110;
    for (u32 k = 0; k < 16 + 14; k++) {
        const BufferRange *r = k < 16 ? &st.ssbo[stage][k] : &st.ubo[stage][k - 16];
        if (k >= 16 && stage != ST_COMPUTE) break;
        PoolEnt *p = r->addr ? pool_find(r->addr) : NULL;
        if (!p || !p->hb) continue;
        u64 dev = p->hb->addr + (r->addr - p->gpu);
        u32 v[4] = {(u32)dev, (u32)(dev >> 32), (u32)r->size, 0};
        memcpy(drv + (k < 16 ? ssbo_at + 16 * k : 0x220 + 16 * (k - 16)), v, 16);
    }
    for (u32 k = 0; k < 32; k++) {
        u32 h = (u32)st.tex[stage][k];
        memcpy(drv + 0x600 + 0x20 + 8 * k, &h, 4);
    }
    for (u32 i = 0; i < 16; i++) {
        VkDescriptorBufferInfo *b = &bi[i];
        *b = (VkDescriptorBufferInfo){NULL, 0, VK_WHOLE_SIZE};
        if (i == 0) *b = (VkDescriptorBufferInfo){sbuf, soff, 0x600};
        else if (i == 2) *b = (VkDescriptorBufferInfo){sbuf, soff + 0x600, 0x200};
        else {
            u64 addr = 0, size = 0;
            if (i == 1 && sc && sc->const_size) { addr = sc->const_addr; size = (sc->const_size + 15) & ~15u; }
            else if (i >= 3) { addr = st.ubo[stage][i - 3].addr; size = st.ubo[stage][i - 3].size; }
            VkBuffer buf;
            u64 off;
            if (addr && size && gpu_buffer(addr, &buf, &off) && !(off & 63)) {
                if (size > 65536) size = 65536;
                *b = (VkDescriptorBufferInfo){buf, off, size};
            }
        }
        w[i] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = base + i, .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = b};
    }
    return 16;
}

static void set_dynamic_state(VkCommandBuffer c, u32 mode) {
    VkViewport vp = {st.viewport[0][0], st.viewport[0][1], st.viewport[0][2], st.viewport[0][3],
                     st.depth_range[0][0], st.depth_range[0][1]};
    if (vp.width <= 0 || vp.height == 0) {
        vp.x = 0; vp.y = 0;
        vp.width = (float)st.area.extent.width;
        vp.height = (float)st.area.extent.height;
    }
    if (window_origin == 1) {
        /* NVN's upper-left origin puts NDC y = +1 at row 0, Vulkan puts -1 there: flip, so that every render
         * target holds rows exactly as on the console (scissors and gl_FragCoord already count from row 0) */
        vp.y += vp.height;
        vp.height = -vp.height;
    }
    vkCmdSetViewportWithCount(c, 1, &vp);
    VkRect2D sc = clear_rect();
    if (!sc.extent.width || !sc.extent.height) sc = (VkRect2D){{0, 0}, {1, 1}};
    vkCmdSetScissorWithCount(c, 1, &sc);
    vkCmdSetLineWidth(c, st.line_width > 0 ? st.line_width : 1);
    vkCmdSetDepthBias(c, st.poly_offset[1], st.poly_offset[2], st.poly_offset[0]);
    vkCmdSetBlendConstants(c, st.blend_color);
    vkCmdSetDepthBounds(c, st.depth_bounds[0], st.depth_bounds[1] > st.depth_bounds[0] ? st.depth_bounds[1] : 1);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_FRONT_BIT, st.stencil_value_mask[0]);
    vkCmdSetStencilCompareMask(c, VK_STENCIL_FACE_BACK_BIT, st.stencil_value_mask[1]);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_FRONT_BIT, st.stencil_mask[0]);
    vkCmdSetStencilWriteMask(c, VK_STENCIL_FACE_BACK_BIT, st.stencil_mask[1]);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_FRONT_BIT, st.stencil_ref[0]);
    vkCmdSetStencilReference(c, VK_STENCIL_FACE_BACK_BIT, st.stencil_ref[1]);
    vkCmdSetCullMode(c, st.poly.cull & 3);
    /* the viewport flip for the upper-left origin mirrors the winding too */
    vkCmdSetFrontFace(c, (st.poly.front_face != 0) != (window_origin == 1) ? VK_FRONT_FACE_CLOCKWISE
                                                                            : VK_FRONT_FACE_COUNTER_CLOCKWISE);
    vkCmdSetPrimitiveTopology(c, topology(mode));
    vkCmdSetDepthTestEnable(c, st.ds.flags & 1);
    vkCmdSetDepthWriteEnable(c, (st.ds.flags & 2) != 0);
    vkCmdSetDepthCompareOp(c, compare_op(st.ds.depth_func));
    vkCmdSetDepthBoundsTestEnable(c, st.depth_bounds_test);
    vkCmdSetStencilTestEnable(c, (st.ds.flags & 4) != 0);
    vkCmdSetStencilOp(c, VK_STENCIL_FACE_FRONT_BIT, stencil_op(st.ds.fail & 15), stencil_op(st.ds.zpass & 15),
                      stencil_op(st.ds.zfail & 15), compare_op(st.ds.stencil_func & 15));
    vkCmdSetStencilOp(c, VK_STENCIL_FACE_BACK_BIT, stencil_op(st.ds.fail >> 4), stencil_op(st.ds.zpass >> 4),
                      stencil_op(st.ds.zfail >> 4), compare_op(st.ds.stencil_func >> 4));
    vkCmdSetRasterizerDiscardEnable(c, st.rasterizer_discard);
    vkCmdSetDepthBiasEnable(c, (st.poly.offset_enables & (mode == 0 ? 1 : mode <= 3 ? 2 : 4)) != 0);
    vkCmdSetPrimitiveRestartEnable(c, st.primitive_restart && (mode == 3 || mode == 5 || mode == 6));
    vkCmdSetPolygonModeEXT(c, st.poly.mode == 0 ? VK_POLYGON_MODE_POINT : st.poly.mode == 1 ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL);
    vkCmdSetRasterizationSamplesEXT(c, (VkSampleCountFlagBits)st.samples);
    VkSampleMask sm = st.sample_mask;
    vkCmdSetSampleMaskEXT(c, (VkSampleCountFlagBits)st.samples, &sm);
    vkCmdSetAlphaToCoverageEnableEXT(c, st.ms.a2c);
    vkCmdSetLogicOpEnableEXT(c, st.color.logic_op != 3);
    vkCmdSetLogicOpEXT(c, (VkLogicOp)(st.color.logic_op & 15));
    vkCmdSetDepthClampEnableEXT(c, st.depth_clamp);
    if (st.nrt) {
        VkBool32 en[8];
        VkColorBlendEquationEXT eq[8];
        VkColorComponentFlags wm[8];
        for (u32 i = 0; i < st.nrt; i++) {
            const BlendState *b = &st.blend[i];
            en[i] = st.color.blend_enable >> i & 1;
            eq[i] = (VkColorBlendEquationEXT){blend_factor(b->src_rgb), blend_factor(b->dst_rgb), blend_op(b->eq & 15),
                                              blend_factor(b->src_a), blend_factor(b->dst_a), blend_op(b->eq >> 4)};
            wm[i] = st.mask.mask >> (4 * i) & 0xf;
        }
        vkCmdSetColorBlendEnableEXT(c, 0, st.nrt, en);
        vkCmdSetColorBlendEquationEXT(c, 0, st.nrt, eq);
        vkCmdSetColorWriteMaskEXT(c, 0, st.nrt, wm);
    }
}

static void set_vertex_input(VkCommandBuffer c) {
    /* streams 0-30; binding 31 is the zero source for attributes the game leaves unset */
    VkVertexInputBindingDescription2EXT bind[32];
    VkVertexInputAttributeDescription2EXT attr[48];
    u32 nb = 0, na = 0;
    bool used[32] = {0}, located[32] = {0};
    for (u32 i = 0; i < st.nattribs && i < 32; i++) {
        u32 f = VA_FORMAT(st.attribs[i]), s = VA_STREAM(st.attribs[i]);
        if (!f || s >= 31) continue;
        VkFormat vf = vk_format(f);
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
    VkDeviceSize offs[32], sizes[32];
    for (u32 s = 0; s < 32; s++) {
        bufs[s] = NULL; offs[s] = 0; sizes[s] = VK_WHOLE_SIZE;
        if (s < 31 && used[s]) {
            u32 stride = s < st.nstreams ? st.streams[s].stride : 0, div = s < st.nstreams ? st.streams[s].divisor : 0;
            bind[nb++] = (VkVertexInputBindingDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, NULL,
                s, stride, div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, 1};
            u64 off;
            if (st.vbuf[s].addr && gpu_buffer(st.vbuf[s].addr, &bufs[s], &off)) {
                offs[s] = off;
                sizes[s] = st.vbuf[s].size ? st.vbuf[s].size : VK_WHOLE_SIZE;
            }
        }
    }
    bind[nb++] = (VkVertexInputBindingDescription2EXT){VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, NULL,
                                                       31, 0, VK_VERTEX_INPUT_RATE_VERTEX, 1};
    vkCmdSetVertexInputEXT(c, nb, bind, na, attr);
    vkCmdBindVertexBuffers2(c, 0, 32, bufs, offs, sizes, NULL);
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

static void draw(u32 op, const void *payload) {
    draws++;
    Shaders *pr = st.program;
    if (!st.render_enable) return;
    if (op == OP_DRAW_TEXTURE || op == OP_DRAW_XFB || op == OP_MULTI_DRAW_INDIRECT) { skip_draw("DrawTexture / transform feedback / multi-draw"); return; }
    if (!pr || !pr->stage[ST_VERTEX]) { skip_draw("no vertex shader"); return; }
    if (pr->mask & ~(1u << ST_VERTEX | 1u << ST_FRAGMENT)) { skip_draw("geometry or tessellation shaders"); return; }
    VkShaderModule vs = shader_module(pr->stage[ST_VERTEX]);
    VkShaderModule fs = pr->stage[ST_FRAGMENT] ? shader_module(pr->stage[ST_FRAGMENT]) : NULL;
    if (!vs || (pr->stage[ST_FRAGMENT] && !fs)) { skip_draw("shader not translated"); return; }
    /* MK8_GPU_SKIP: bisecting GPU faults (1 all draws, 2 global memory, 4 textures) */
    static int skip = -1;
    if (skip < 0) skip = getenv("MK8_GPU_SKIP") ? atoi(getenv("MK8_GPU_SKIP")) : 0;
    u32 caps = pr->stage[ST_VERTEX]->caps | (pr->stage[ST_FRAGMENT] ? pr->stage[ST_FRAGMENT]->caps : 0);
    if (skip & 1 || (skip & 2 && caps & SHC_GLOBAL) || (skip & 4 && caps & SHC_TEXTURE)) { skip_draw("MK8_GPU_SKIP"); return; }
    DescSet *ts = descset_get(&st.texpool, false), *ss = descset_get(&st.smppool, true);
    descset_sync(ts);
    descset_sync(ss);
    if (!begin_rendering()) { skip_draw("no render targets"); return; }
    VkCommandBuffer c = cmd();
    PipeKey k;
    memset(&k, 0, sizeof k);
    k.vs = vs;
    k.fs = fs;
    k.ncolor = st.nrt;
    memcpy(k.color, st.rt_format, sizeof k.color);
    k.depth = st.depth_format;
    k.stencil = st.stencil_format;
    k.samples = st.samples;
    k.minus_one = depth_mode == 0;
    VkPipeline pipe = pipeline_get(&k);
    if (!pipe) { skip_draw("pipeline creation failed"); return; }
    static long trace = -2;
    if (trace == -2) trace = getenv("MK8_TRACE") ? atol(getenv("MK8_TRACE")) : -1;
    if ((long)frame_no + 1 == trace) {
        const CmdDraw *d = payload;
        hlog(0, "draw %llu: op %u mode %u count %u inst %u idx %u@%llx | vs %016llx fs %016llx | rt0 %ux%u f%u @%llx n%u ds %s | vp %g %g %g %g dr %g %g sc %d %d %d %d | ds %x df %u cull %u ff %u",
             draws, op, d->mode, d->count, d->instances, d->index_type, d->index_addr, pr->stage[ST_VERTEX]->hash,
             pr->stage[ST_FRAGMENT] ? pr->stage[ST_FRAGMENT]->hash : 0, st.rt[0].t.width, st.rt[0].t.height,
             st.rt[0].t.format, st.rt[0].t.addr, st.nrt, st.has_depth ? "yes" : "no", st.viewport[0][0], st.viewport[0][1],
             st.viewport[0][2], st.viewport[0][3], st.depth_range[0][0], st.depth_range[0][1], st.scissor[0][0],
             st.scissor[0][1], st.scissor[0][2], st.scissor[0][3], st.ds.flags, st.ds.depth_func, st.poly.cull, st.poly.front_face);
        char b[512];
        int o = 0;
        for (u32 i = 0; i < st.nattribs && o < 400; i++)
            if (VA_FORMAT(st.attribs[i]))
                o += snprintf(b + o, sizeof b - o, " a%u:f%u s%u+%u", i, VA_FORMAT(st.attribs[i]), VA_STREAM(st.attribs[i]),
                              VA_OFFSET(st.attribs[i]));
        hlog(0, "   attribs%s", b);
        o = 0;
        for (u32 i = 0; i < 4; i++)
            o += snprintf(b + o, sizeof b - o, " v%u:%llx+%llx/%u", i, st.vbuf[i].addr, st.vbuf[i].size,
                          i < st.nstreams ? st.streams[i].stride : 0);
        for (u32 i = 0; i < 6; i++)
            o += snprintf(b + o, sizeof b - o, " u%u:%llx+%llx", i, st.ubo[ST_VERTEX][i].addr, st.ubo[ST_VERTEX][i].size);
        hlog(0, "  %s", b);
        o = 0;
        for (u32 k = 0; k < 8 && st.texpool.addr; k++) {
            u32 h = (u32)st.tex[ST_FRAGMENT][k], id = h & 0xfffff;
            if (!h || id >= st.texpool.count) continue;
            u64 a = st.texpool.addr + 32ull * id;
            const TexDesc *td = LD32(a) == TDESC_MAGIC ? tdesc_get(LD32(a + 4)) : NULL;
            if (td) o += snprintf(b + o, sizeof b - o, " t%u:%ux%ux%u f%u L%u/%u+%u tg%u @%llx", k, td->t.width, td->t.height,
                                  td->t.depth, td->v.format, td->v.base_level, td->t.levels, td->v.num_levels, td->v.target, td->t.addr);
        }
        if (o) hlog(0, "   tex%s", b);
    }
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    set_dynamic_state(c, ((const CmdDraw *)payload)->mode);
    VkWriteDescriptorSet w[32];
    VkDescriptorBufferInfo bi[32];
    int nw = stage_ubos(ST_VERTEX, 0, pr->stage[ST_VERTEX], w, bi);
    nw += stage_ubos(ST_FRAGMENT, 16, pr->stage[ST_FRAGMENT], w + nw, bi + nw);
    vkCmdPushDescriptorSetKHR(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_gfx, 0, nw, w);
    VkDescriptorSet sets[2] = {ts ? ts->set : empty_tex, ss ? ss->set : empty_smp};
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_gfx, 1, 2, sets, 0, NULL);
    struct { u32 func; float ref; u32 pad[2]; } pc = {st.color.alpha_test, st.alpha_ref, {0, 0}};
    vkCmdPushConstants(c, layout_gfx, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, &pc);
    set_vertex_input(c);
    if (op == OP_DRAW_ARRAYS || op == OP_DRAW_ELEMENTS) {
        const CmdDraw *d = payload;
        bool indexed = op == OP_DRAW_ELEMENTS;
        if (d->mode == 7) {             /* quads */
            VkBuffer ib;
            u64 io;
            u32 n;
            if (!quad_indices(d, indexed, &ib, &io, &n)) { skip_draw("quads"); return; }
            vkCmdBindIndexBuffer(c, ib, io, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(c, n, d->instances, 0, indexed ? d->base_vertex : 0, d->base_instance);
            return;
        }
        if (d->mode == 2 || d->mode == 8 || d->mode == 9) { skip_draw("line loop / quad strip / polygon"); return; }
        if (!indexed) {
            vkCmdDraw(c, d->count, d->instances, d->first, d->base_instance);
            return;
        }
        VkBuffer ib;
        u64 io;
        if (!gpu_buffer(d->index_addr, &ib, &io)) { skip_draw("index buffer not GPU memory"); return; }
        VkIndexType it = d->index_type == 0 ? VK_INDEX_TYPE_UINT8_KHR : d->index_type == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        vkCmdBindIndexBuffer(c, ib, io, it);
        vkCmdDrawIndexed(c, d->count, d->instances, 0, d->base_vertex, d->base_instance);
        return;
    }
    const CmdDrawIndirect *d = payload;
    VkBuffer buf, ib;
    u64 off, io;
    if (!gpu_buffer(d->indirect, &buf, &off)) { skip_draw("indirect buffer not GPU memory"); return; }
    if (op == OP_DRAW_ARRAYS_INDIRECT) {
        vkCmdDrawIndirect(c, buf, off, 1, 16);
        return;
    }
    if (!gpu_buffer(d->index_addr, &ib, &io)) { skip_draw("index buffer not GPU memory"); return; }
    VkIndexType it = d->index_type == 0 ? VK_INDEX_TYPE_UINT8_KHR : d->index_type == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    vkCmdBindIndexBuffer(c, ib, io, it);
    vkCmdDrawIndexedIndirect(c, buf, off, 1, 20);
}

/* ---- compute */
static u64 pr_hash(void) { return st.program && st.program->stage[ST_COMPUTE] ? st.program->stage[ST_COMPUTE]->hash : 0; }
typedef struct CPipe { VkShaderModule m; VkPipeline pipe; struct CPipe *next; } CPipe;
static CPipe *cpipes;

static void dispatch(u32 op, const u8 *pay) {
    dispatches++;
    static long trace = -2;
    if (trace == -2) trace = getenv("MK8_TRACE") ? atol(getenv("MK8_TRACE")) : -1;
    if ((long)frame_no + 1 == trace) {
        char b[700];
        int o = 0;
        for (u32 i = 0; i < 14; i++)
            if (st.ubo[ST_COMPUTE][i].addr) o += snprintf(b + o, sizeof b - o, " u%u:%llx+%llx", i, st.ubo[ST_COMPUTE][i].addr, st.ubo[ST_COMPUTE][i].size);
        for (u32 i = 0; i < 16; i++)
            if (st.ssbo[ST_COMPUTE][i].addr) o += snprintf(b + o, sizeof b - o, " s%u:%llx+%llx", i, st.ssbo[ST_COMPUTE][i].addr, st.ssbo[ST_COMPUTE][i].size);
        hlog(0, "dispatch %llu: cs %016llx%s", dispatches, pr_hash(), b);
    }
    Shaders *pr = st.program;
    if (!pr || !pr->stage[ST_COMPUTE]) { skip_draw("dispatch without a compute shader"); return; }
    VkShaderModule m = shader_module(pr->stage[ST_COMPUTE]);
    if (!m) { skip_draw("shader not translated"); return; }
    CPipe *cp;
    for (cp = cpipes; cp && cp->m != m; cp = cp->next) {}
    if (!cp) {
        cp = calloc(1, sizeof *cp);
        cp->m = m;
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = m, .pName = "main"}, .layout = layout_gfx};
        if (vkCreateComputePipelines(vk.dev, pcache, 1, &ci, NULL, &cp->pipe) != VK_SUCCESS)
            hlog(0, "gpu: compute pipeline for %016llx failed", pr->stage[ST_COMPUTE]->hash);
        cp->next = cpipes;
        cpipes = cp;
    }
    if (!cp->pipe) return;
    DescSet *ts = descset_get(&st.texpool, false), *ss = descset_get(&st.smppool, true);
    descset_sync(ts);
    descset_sync(ss);
    end_rendering();
    VkCommandBuffer c = cmd();
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, cp->pipe);
    VkWriteDescriptorSet w[16];
    VkDescriptorBufferInfo bi[16];
    int nw = stage_ubos(ST_COMPUTE, 0, pr->stage[ST_COMPUTE], w, bi);
    vkCmdPushDescriptorSetKHR(c, VK_PIPELINE_BIND_POINT_COMPUTE, layout_gfx, 0, nw, w);
    VkDescriptorSet sets[2] = {ts ? ts->set : empty_tex, ss ? ss->set : empty_smp};
    vkCmdBindDescriptorSets(c, VK_PIPELINE_BIND_POINT_COMPUTE, layout_gfx, 1, 2, sets, 0, NULL);
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
    barrier();
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
        case OP_MULTI_DRAW_INDIRECT: case OP_DRAW_TEXTURE: case OP_DRAW_XFB:
            draw(op, pay);
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
    submit_serial++;
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
    TexRef tr = {*t, *v};
    ViewInfo vv;
    Image *im = image_ref(&tr, &vv);
    v = &vv;
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
        im->gpu_written = true;         /* guest memory does not hold these texels */
        barrier();
        static int logged;
        if (logged++ < 8) {
            const u32 *px = data;
            hlog(1, "gpu: WriteTexels %ux%ux%u f%u target %u region %d,%d,%d %dx%dx%d row %llu: %08x %08x %08x %08x",
                 t->width, t->height, t->depth, t->format, t->target, r->x, r->y, r->z, r->w, r->h, r->d, row, px[0],
                 px[1], px[2], px[3]);
        }
    }
    LeaveCriticalSection(&gpu_cs);
}

/* ---------------------------------------------------------------------------------------------- present */
/* MK8_DUMP=n[,m...]: at those frames, write the presented window texture (frame_<n>.ppm) and every render target
 * the frame drew into (frame_<n>_<w>x<h>_<format>_<address>.ppm) to build/frames */

static float half_to_float(u16 h) {
    u32 s = h >> 15, e = h >> 10 & 31, m = h & 1023;
    float v = e == 0 ? m / 16777216.0f : e == 31 ? 65504.0f : (1.0f + m / 1024.0f) * (float)(1u << e) / 32768.0f;
    return s ? -v : v;
}
static u8 to8(float f) { return f <= 0 ? 0 : f >= 1 ? 255 : (u8)(f * 255.0f + 0.5f); }
static float uf11(u32 v, int mbits) {
    u32 e = v >> mbits, m = v & ((1u << mbits) - 1);
    if (e == 0) return m / (float)(1u << mbits) / 16384.0f;
    return (1.0f + m / (float)(1u << mbits)) * (float)(1u << e) / 32768.0f;
}

static bool decode_pixel(VkFormat f, const u8 *p, u8 out[3]) {
    u32 v;
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: memcpy(out, p, 3); return true;
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; return true;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        memcpy(&v, p, 4);
        out[0] = (u8)((v & 1023) >> 2); out[1] = (u8)((v >> 10 & 1023) >> 2); out[2] = (u8)((v >> 20 & 1023) >> 2);
        return true;
    case VK_FORMAT_R16G16B16A16_SFLOAT: {
        u16 h[3];
        memcpy(h, p, 6);
        for (int i = 0; i < 3; i++) out[i] = to8(half_to_float(h[i]));
        return true;
    }
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        memcpy(&v, p, 4);
        out[0] = to8(uf11(v & 2047, 6)); out[1] = to8(uf11(v >> 11 & 2047, 6)); out[2] = to8(uf11(v >> 22, 5));
        return true;
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: {
        float d;
        memcpy(&d, p, 4);
        out[0] = out[1] = out[2] = to8(d);
        return true;
    }
    case VK_FORMAT_R8_UNORM: out[0] = out[1] = out[2] = p[0]; return true;
    case VK_FORMAT_R8G8_UNORM: out[0] = p[0]; out[1] = p[1]; out[2] = 0; return true;
    default: return false;
    }
}

static u32 format_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    case VK_FORMAT_R8_UNORM: return 1;
    case VK_FORMAT_R8G8_UNORM: return 2;
    default: return 4;
    }
}

static void dump_image(Image *im, const char *name) {
    u8 probe[3], zero[16] = {0};
    if (!decode_pixel(im->format, zero, probe) || im->info.samples > 1) return;
    u32 w = im->info.width, h = tex_height(&im->info), bpp = format_bytes(im->format);
    VkBuf b;
    vk_buffer((u64)w * h * bpp, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &b);
    VkBufferImageCopy r = {0, 0, 0, {copy_aspect(im), 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    end_rendering();
    vkCmdCopyImageToBuffer(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, b.buf, 1, &r);
    flush();
    gpu_wait(submitted, UINT64_MAX);
    char path[MAX_PATH], exe[MAX_PATH];
    GetModuleFileNameA(NULL, exe, sizeof exe);
    char *slash = strrchr(exe, '\\');
    if (slash) *slash = 0;
    snprintf(path, sizeof path, "%s\\..\\frames", exe);
    CreateDirectoryA(path, NULL);
    snprintf(path, sizeof path, "%s\\..\\frames\\%s.ppm", exe, name);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        const u8 *px = b.a.map;
        u8 *row = malloc(w * 3);
        for (u32 y = 0; y < h; y++) {
            for (u32 x = 0; x < w; x++) decode_pixel(im->format, px + ((u64)y * w + x) * bpp, row + 3 * x);
            fwrite(row, 1, w * 3, f);
        }
        free(row);
        fclose(f);
    }
    vk_buffer_free(&b);
}

static void dump_frame(Image *win, u64 frame) {
    static char list[256];
    static int init;
    if (!init) {
        init = 1;
        const char *e = getenv("MK8_DUMP");
        if (e) snprintf(list, sizeof list, ",%s,", e);
    }
    char key[32], name[128];
    snprintf(key, sizeof key, ",%llu,", frame);
    if (!list[0] || !strstr(list, key)) return;
    int n = 0;
    for (int bkt = 0; bkt < IMAGE_BUCKETS; bkt++)
        for (Image *im = buckets[bkt]; im; im = im->next)
            if (im->rt_frame == frame && im != win) {
                snprintf(name, sizeof name, "frame_%llu_%ux%u_f%u_%llx", frame, im->info.width, im->info.height,
                         im->info.format, im->info.addr);
                dump_image(im, name);
                n++;
            }
    if (win) {
        snprintf(name, sizeof name, "frame_%llu", frame);
        dump_image(win, name);
    }
    hlog(1, "gpu: frame %llu and %d render targets written to build/frames", frame, n);
}

/* textures the CPU rewrote since the last frame (write watch over the GPU-visible pools): upload them again */
static void refresh_cpu_textures(void) {
    static u64 *pages;
    static size_t cap;
    size_t n = 0;
    for (int i = 0; i < npools; i++) {
        if (!pools[i].cpu || !pools[i].hb) continue;
        for (;;) {
            if (cap - n < 4096) pages = realloc(pages, (cap = cap * 2 + 65536) * sizeof *pages);
            ULONG_PTR count = cap - n;
            ULONG gran;
            if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)(uintptr_t)pools[i].cpu, pools[i].end - pools[i].gpu,
                              (void **)(pages + n), &count, &gran) != 0) break;
            n += count;
            if (count < cap - n + count) break;     /* all of them */
        }
    }
    if (!n) return;
    /* pages come sorted per pool and pools are sorted: one sorted list */
    int refreshed = 0;
    for (int b = 0; b < IMAGE_BUCKETS; b++)
        for (Image *im = buckets[b]; im; im = im->next) {
            if (!im->valid || im->gpu_written || !im->size) continue;
            u64 lo = im->info.addr, hi = lo + im->size;
            size_t a = 0, z = n;
            while (a < z) { size_t m = (a + z) / 2; if (pages[m] + 4096 <= lo) a = m + 1; else z = m; }
            if (a < n && pages[a] < hi) {
                im->valid = false;
                image_upload(im);
                refreshed++;
            }
        }
    static int logged;
    if (refreshed && logged < 5) { logged++; hlog(1, "gpu: %d textures written by the CPU uploaded again", refreshed); }
}

void gpu_present(u64 queue, Window *w, u32 index) {
    (void)queue;
    EnterCriticalSection(&gpu_cs);
    refresh_cpu_textures();
    Image *im = image_get(&w->info[index]);
    if (im) image_upload(im);
    flush();
    dump_frame(im, frame_no + 1);
    u64 frames = ++frame_no;
    if (frames % 600 == 0) hlog(1, "gpu: %llu draws so far (%llu skipped), %llu dispatches, %d images, %u pipelines",
                                  draws, draws_skipped, dispatches, nimages, npipes);
    s32 cw = w->crop[2] > 0 ? w->crop[2] : (s32)w->info[index].width;
    s32 ch = w->crop[3] > 0 ? w->crop[3] : (s32)w->info[index].height;
    /* lower-left origin: row 0 is the bottom of the picture */
    vk_present(im ? im->image : NULL, w->crop[0], w->crop[1], cw, ch, window_origin == 0, timeline, submitted);
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
        vk_buffer(STAGING_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &slots[i].staging);
    }
    state_reset(&st);
    layouts_init();
    dummies_init();
    empty_tex = alloc_set(set_tex, TEXPOOL_MAX);
    empty_smp = alloc_set(set_smp, SMPPOOL_MAX);
    descset_fill(empty_tex, false, TEXPOOL_MAX);
    descset_fill(empty_smp, true, SMPPOOL_MAX);
    gpu_ready = true;
    hlog(1, "gpu: ready");
}
