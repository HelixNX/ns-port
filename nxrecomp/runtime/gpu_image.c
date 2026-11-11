/* The image cache: Vulkan images keyed by TexInfo, their views, formats, uploads from guest memory (deswizzled,
 * ASTC decoded), render scale, eviction, and the scan for textures the CPU rewrote (refresh_cpu_textures).
 * Part of the NVN executor (gpu.c, gpu_int.h). */
#include "gpu_int.h"

/* Images at the render scale: 2D color or depth targets shaped like the screen (16:9, from 120x68 up: the 1080p
 * targets and their half- to sixteenth-size chains), not pitch-linear (the CPU reads those) or compressed */
static bool image_candidate(const TexInfo *t, VkFormat fmt, VkImageUsageFlags usage) {
    if (t->flags & (TEX_FLAG_LINEAR | TEX_FLAG_LINEAR_RENDER_TARGET)) return false;
    if (t->target != TEX_2D && t->target != TEX_2D_ARRAY && t->target != TEX_2D_MS && t->target != TEX_2D_MS_ARRAY)
        return false;
    if (fmt >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && fmt <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) return false;
    if (!(usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT))) return false;
    u32 w = t->width, h = tex_height(t);
    if (w < 120 || h < 64) return false;
    s64 d = (s64)w * 9 - (s64)h * 16;
    return d >= -16 && d <= 16;
}

/* A colour target of another shape drawn while a scaled (screen) depth target is bound joins the render scale: Violet
 * draws the menus' 3D player (400x800) and mons (360x360) that way, with the 1080p depth still bound, and a pass runs
 * at its smallest target's scale, so they stayed 1x. Same limits as image_candidate apart from the shape; true if im
 * can scale now. */
bool image_promote(Image *im) {
    if (im->can_scale) return true;
    const TexInfo *t = &im->info;
    if (t->flags & (TEX_FLAG_LINEAR | TEX_FLAG_LINEAR_RENDER_TARGET)) return false;
    if (t->target != TEX_2D && t->target != TEX_2D_ARRAY && t->target != TEX_2D_MS && t->target != TEX_2D_MS_ARRAY)
        return false;
    if (!(im->aspects & VK_IMAGE_ASPECT_COLOR_BIT) || !(im->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) return false;
    if (t->width < 64 || tex_height(t) < 64) return false;
    im->can_scale = true;
    return true;
}
Image *wb_list[WB_MAX];
void linear_wrote(Image *im) {
    if (!im || im->wb_pending || !(im->info.flags & TEX_FLAG_LINEAR)) return;
    if (nwb == WB_MAX) { hlog(0, "gpu: more than %d linear textures written in one submission", WB_MAX); return; }
    im->wb_pending = true;
    wb_list[nwb++] = im;
}
Image *buckets[IMAGE_BUCKETS];
int nimages;
u64 image_bytes;                        /* device memory the images hold */

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

u64 image_epoch;                        /* bumped whenever images are destroyed */

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
    if (im->wb_pending)
        for (int i = 0; i < nwb; i++)
            if (wb_list[i] == im) { wb_list[i] = wb_list[--nwb]; break; }
    byaddr_remove(im);
    for (int i = 0; i < im->nviews; i++) bury(DEAD_VIEW, im->views[i].view, NULL);
    free(im->views);
    image_bytes -= im->alloc.size;
    bury(DEAD_IMAGE, im->image, &im->alloc);
    free(im);
    nimages--;
}

void images_drop_range(u64 lo, u64 hi) {
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

VkFormatFeatureFlags format_features(VkFormat f) {
    static VkFormatFeatureFlags cache[256];
    static bool known[256];
    if ((u32)f < 256 && known[f]) return cache[f];
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(vk.phys, f, &p);
    if ((u32)f < 256) { known[f] = true; cache[f] = p.optimalTilingFeatures; }
    return p.optimalTilingFeatures;
}

static bool format_usable(VkFormat f) {
    VkFormatFeatureFlags ft = format_features(f);
    return ft & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT || ft & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
}

/* depth / stencil formats a device may lack, with stand-ins in order of preference: the GTX 1650 has no X8_D24 or
   S8 images; AMD has no D24 at all (a D24S8 image there is invalid, and using it loses the device) */
VkFormat usable_format(u32 nvn) {
    VkFormat f = vk_format(nvn);
    if (f == VK_FORMAT_UNDEFINED || format_usable(f)) return f;
    static const VkFormat x8d24[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT};
    static const VkFormat d24s8[] = {VK_FORMAT_D32_SFLOAT_S8_UINT};
    static const VkFormat s8[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
    const VkFormat *alt = NULL;
    size_t n = 0;
    switch (f) {
    case VK_FORMAT_X8_D24_UNORM_PACK32: alt = x8d24; n = sizeof x8d24 / sizeof *x8d24; break;
    case VK_FORMAT_D24_UNORM_S8_UINT: alt = d24s8; n = sizeof d24s8 / sizeof *d24s8; break;
    case VK_FORMAT_S8_UINT: alt = s8; n = sizeof s8 / sizeof *s8; break;
    default: return f;
    }
    for (size_t i = 0; i < n; i++)
        if (format_usable(alt[i])) return alt[i];
    return f;
}

/* a format a storage view of f can use: f itself, or the UNORM twin of an sRGB format (MUTABLE_FORMAT images) */
static VkFormat storage_format(VkFormat f) {
    if (format_features(f) & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) return f;
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}

/* the format of a sampled view of f: shaders declare every texture as float (a Maxwell texture instruction does
 * not say what the texture holds; the registers get its raw bits), so an integer format with 32-bit channels is
 * seen through the float format with the same bits (MUTABLE_FORMAT images). Narrower integer formats have no such
 * twin: they keep their format (a mismatch the device may not forgive) and are reported */
static VkFormat sampled_format(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: return VK_FORMAT_R32_SFLOAT;
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: return VK_FORMAT_R32G32_SFLOAT;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: return VK_FORMAT_R32G32B32A32_SFLOAT;
    default: break;
    }
    if (format_numeric(f) && !(format_aspects(f) & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))) {
        static VkFormat warned[16];
        static int nw;
        bool seen = false;
        for (int i = 0; i < nw; i++) seen |= warned[i] == f;
        if (!seen && nw < 16) { warned[nw++] = f; hlog(0, "gpu: integer texture format %d sampled as float", f); }
    }
    return f;
}
static VkFormat uint_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_SINT: return VK_FORMAT_R8_UINT;
    case VK_FORMAT_R16_SINT: return VK_FORMAT_R16_UINT;
    case VK_FORMAT_R32_SINT: return VK_FORMAT_R32_UINT;
    case VK_FORMAT_R8G8_SINT: return VK_FORMAT_R8G8_UINT;
    case VK_FORMAT_R16G16_SINT: return VK_FORMAT_R16G16_UINT;
    case VK_FORMAT_R32G32_SINT: return VK_FORMAT_R32G32_UINT;
    case VK_FORMAT_R8G8B8A8_SINT: return VK_FORMAT_R8G8B8A8_UINT;
    case VK_FORMAT_R16G16B16A16_SINT: return VK_FORMAT_R16G16B16A16_UINT;
    case VK_FORMAT_R32G32B32A32_SINT: return VK_FORMAT_R32G32B32A32_UINT;
    case VK_FORMAT_A2B10G10R10_SINT_PACK32: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    default: return f;
    }
}

/* the sRGB format of a UNORM one and back (the game samples an sRGB target as UNORM; NX_VIEW_LOG) */
static VkFormat srgb_twin(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    default: return f;
    }
}

/* The formats an image's views may use: its own, and the twins image_view picks (sampled, integer, storage,
 * sRGB / UNORM). Only an image with more than one is made MUTABLE_FORMAT, with these as its format list: a
 * mutable image without a list loses colour compression (DCC) on AMD. A guest view in any other format falls back
 * to the image's own (logged). NX_MUTABLE_ALL=1: every image mutable, no lists (as before 2026-10-31) */
static void format_list(Image *im) {
    VkFormat f = im->format, c[5] = {sampled_format(f), uint_twin(f), srgb_twin(f),
        im->usage & VK_IMAGE_USAGE_STORAGE_BIT ? storage_format(f) : f, f};
    im->vfmts[0] = f;
    im->nvfmts = 1;
    for (int i = 0; i < 5; i++) {
        bool dup = c[i] == VK_FORMAT_UNDEFINED;
        for (u32 j = 0; j < im->nvfmts; j++) dup |= im->vfmts[j] == c[i];
        if (!dup) im->vfmts[im->nvfmts++] = c[i];
    }
}

static bool format_listed(const Image *im, VkFormat f) {
    if (env.mutable_all) return true;
    for (u32 i = 0; i < im->nvfmts; i++)
        if (im->vfmts[i] == f) return true;
    return false;
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

/* a whole level (guest size w x h x depth, n layers) of s into d: copied at one scale, blitted between two */
static void img_copy(Image *s, u32 sl, u32 slayer, Image *d, u32 dl, u32 dlayer, u32 n, u32 w, u32 h, u32 depth) {
    if (s->scale == d->scale) {
        VkImageCopy c = {{s->aspects, sl, slayer, n}, {0, 0, 0}, {d->aspects, dl, dlayer, n}, {0, 0, 0},
                         {w * s->scale, h * s->scale, depth}};
        vkCmdCopyImage(cmd(), s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
        return;
    }
    if (s->info.samples > 1 || d->info.samples > 1 || !blit_ok(s, d)) return;
    VkImageBlit b = {{s->aspects, sl, slayer, n}, {{0, 0, 0}, {(s32)(w * s->scale), (s32)(h * s->scale), (s32)depth}},
                     {d->aspects, dl, dlayer, n}, {{0, 0, 0}, {(s32)(w * d->scale), (s32)(h * d->scale), (s32)depth}}};
    vkCmdBlitImage(cmd(), s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                   smooth(s->format));
}

/* the image holding texture t, and where in it t starts */
Image *image_find(const TexInfo *t, u32 *layer, u32 *level) {
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
        if (sub_of(I, t, layer, level)) { image_touch(I); return I; }
    }
    Image *im = image_get(t);
    if (!im) return NULL;
    /* A new container takes over the parts of it that exist as images of their own. Its own guest memory is uploaded
     * first, and only the parts the GPU wrote (which guest memory does not hold) are copied in over it. It used to
     * copy every valid part and then count as valid: the layers / levels that were no image of their own stayed
     * zero until the CPU happened to write those pages again, which a cache written once never does (a glyph atlas:
     * letters blank wherever they appear; the reference port's notes describe the same symptom) */
    bool took = false;
    for (int j = byaddr_lower(im->info.addr); j < nbyaddr && byaddr[j]->info.addr < im->info.addr + im->size;) {
        Image *o = byaddr[j];
        u32 ly, lv;
        if (o == im || !sub_of(im, &o->info, &ly, &lv)) { j++; continue; }
        if (o->valid && !im->valid) image_upload(im);
        if (o->valid && o->gpu_written) {
            end_rendering();
            if (!took) barrier();
            for (u32 l = 0; l < o->levels && lv + l < im->levels; l++) {
                u32 w = o->info.width >> l ? o->info.width >> l : 1, hh = tex_height(&o->info) >> l ? tex_height(&o->info) >> l : 1;
                img_copy(o, l, 0, im, lv + l, ly, o->layers, w, hh, 1);
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
Image *image_ref(const TexRef *r, ViewInfo *v) {
    u32 layer, level;
    Image *im = image_find(&r->t, &layer, &level);
    *v = r->v;
    v->base_level = (u16)(v->base_level + level);
    v->min_layer = (u16)(v->min_layer + layer);
    return im;
}

Image *image_get(const TexInfo *t) {
    u32 h = info_hash(t);
    TexInfo k, ki;
    image_key(t, &k);
    Image **pp;
    for (pp = &buckets[h]; *pp; pp = &(*pp)->next) {
        image_key(&(*pp)->info, &ki);
        if (!memcmp(&ki, &k, sizeof k)) break;
    }
    Image *old = *pp;
    if (old) old->used = frame_no + 1;
    if (old && (t->levels ? t->levels : 1) <= old->levels) return old;
    if (!old) return image_create(t, h);
    /* more mip levels than the image has: a bigger image, with the levels so far copied over */
    TexInfo big = old->info;
    big.levels = t->levels;
    *pp = old->next;                    /* out of the bucket: image_create inserts the new one */
    bool hint = rs_hint;
    rs_hint = hint || old->scale > 1;   /* at the old one's scale */
    Image *im = image_create(&big, h);
    rs_hint = hint;
    if (!im) { old->next = buckets[h]; buckets[h] = old; return old; }
    /* not written by the GPU: guest memory holds every level, so the new image is uploaded whole at its first use
     * (copying the old levels left the new ones blank, for good unless the CPU wrote them again) */
    if (!old->gpu_written) {
        im->rt_frame = old->rt_frame;
        image_destroy(old);
        return im;
    }
    end_rendering();
    for (u32 l = 0; l < old->levels; l++) {
        u32 w = old->info.width >> l ? old->info.width >> l : 1, hh = tex_height(&old->info) >> l ? tex_height(&old->info) >> l : 1;
        u32 d = tex_depth(&old->info) >> l ? tex_depth(&old->info) >> l : 1;
        img_copy(old, l, 0, im, l, 0, old->layers < im->layers ? old->layers : im->layers, w, hh, d);
    }
    barrier();
    im->valid = old->valid;
    im->rt_frame = old->rt_frame;
    image_destroy(old);
    return im;
}

static bool image_vk_create(Image *im);

static Image *image_make(const TexInfo *t, u32 h) {
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
    if (!format_usable(fmt)) {          /* an image the device cannot make: invalid, and using it loses the device */
        static u32 warned[16];
        static int nw;
        bool seen = false;
        for (int i = 0; i < nw; i++) seen |= warned[i] == t->format;
        if (!seen && nw < 16) {
            warned[nw++] = t->format;
            hlog(0, "gpu: texture format %u (Vulkan %d) is not supported by this device", t->format, fmt);
        }
    }
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
    if (t->flags & TEX_FLAG_IMAGE && storage_format(fmt) != VK_FORMAT_UNDEFINED) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    im->usage = usage;
    VkImageType type = t->target == TEX_3D ? VK_IMAGE_TYPE_3D :
                       t->target == TEX_1D || t->target == TEX_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
    format_list(im);
    VkImageCreateFlags flags = 0;
    if (env.mutable_all) flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    else if (im->nvfmts > 1) flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    /* a usage the image's own format lacks (storage through the UNORM twin of an sRGB format) */
    if (usage & VK_IMAGE_USAGE_STORAGE_BIT && !(ft & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    if (type == VK_IMAGE_TYPE_2D && im->layers >= 6 && t->width == t->height && t->samples <= 1)
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (type == VK_IMAGE_TYPE_3D) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    im->cflags = flags;
    im->can_scale = image_candidate(t, fmt, usage);
    im->scale = im->can_scale && rs_hint ? rs_scale : 1;
    if (!image_vk_create(im)) {
        free(im);
        return NULL;
    }
    im->next = buckets[h];
    buckets[h] = im;
    byaddr_add(im);
    if (++nimages % 500 == 0) hlog(1, "gpu: %d images", nimages);
    hlog(2, "gpu: image %ux%ux%u fmt %u target %u levels %u at %llx%s", t->width, t->height, t->depth, t->format,
         t->target, t->levels, t->addr, im->scale > 1 ? " (scaled)" : "");
    return im;
}

/* im's Vulkan image and memory at im->scale, its contents defined (zeros, depth 1) */
static bool image_vk_create(Image *im) {
    const TexInfo *t = &im->info;
    VkFormat fmt = im->format;
    VkImageType type = t->target == TEX_3D ? VK_IMAGE_TYPE_3D :
                       t->target == TEX_1D || t->target == TEX_1D_ARRAY ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D;
    VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount = im->nvfmts, .pViewFormats = im->vfmts};
    u32 samples = t->samples > 1 ? t->samples : 1;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = im->cflags, .imageType = type, .format = fmt,
        .extent = {t->width * im->scale, tex_height(t) * im->scale, tex_depth(t)}, .mipLevels = im->levels,
        .arrayLayers = im->layers, .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = im->usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (!env.mutable_all && im->cflags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) ci.pNext = &fl;
    VkResult r = vkCreateImage(vk.dev, &ci, NULL, &im->image);
    if (r != VK_SUCCESS) {
        hlog(0, "gpu: vkCreateImage %ux%ux%u (x%u) format %u (vk %d) target %u: %d", t->width, t->height, t->depth,
             im->scale, t->format, fmt, t->target, r);
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.dev, im->image, &req);
    if (!vk_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &im->alloc)) {
        images_evict(image_bytes / 2, true);
        if (!vk_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &im->alloc))
            fatal("gpu: out of video memory (%d images, %llu MB)", nimages, image_bytes >> 20);
    }
    image_bytes += im->alloc.size;
    im->used = frame_no + 1;
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
    bool compressed = fmt >= VK_FORMAT_BC1_RGB_UNORM_BLOCK && fmt <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK;
    if (compressed) {
        /* not clearable: the CPU's data is uploaded before use */
    } else if (im->aspects & VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue z = {{0, 0, 0, 0}};
        vkCmdClearColorImage(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &all);
    } else {
        VkClearDepthStencilValue z = {1, 0};
        vkCmdClearDepthStencilImage(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, &z, 1, &all);
    }
    barrier();
    return true;
}

static Image *image_create(const TexInfo *t, u32 h) {
    double t0 = ms_now();
    Image *im = image_make(t, h);
    hf.image += ms_now() - t0;
    hf.nimage++;
    /* another image over the same memory start with the same size that could not be shared (the game reuses
     * memory in other formats): its contents and this one's drift apart (log the first few) */
    static int overlaps;
    if (im && overlaps < 40)
        for (int j = byaddr_lower(im->info.addr); j < nbyaddr && byaddr[j]->info.addr == im->info.addr; j++) {
            const Image *o = byaddr[j];
            if (o == im || o->info.width != im->info.width || tex_height(&o->info) != tex_height(&im->info)) continue;
            overlaps++;
            u32 b1, b2, bw, bh;
            format_info(o->info.format, &b1, &bw, &bh);
            format_info(im->info.format, &b2, &bw, &bh);
            hlog(1, "gpu: image %ux%u f%u bh%u at %llx made next to %ux%u f%u bh%u%s (not shared: %s)", im->info.width,
                 tex_height(&im->info), im->info.format, im->info.block_height, im->info.addr, o->info.width,
                 tex_height(&o->info), o->info.format, o->info.block_height, o->gpu_written ? ", rendered" : "",
                 b1 != b2 ? "texel sizes differ" : "layout differs");
        }
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

/* the layers a view covers (none given: all from its first). A 3D image drawn into is seen as a 2D array of its
 * depth slices from the view's first: a shader that writes no layer draws into that slice, one that writes
 * gl_Layer picks among them (color grading LUTs drawn one slice per instance) */
u32 view_layers(const Image *im, const ViewInfo *v, u32 kind, u32 *base) {
    bool slices = kind == VIEW_ATTACHMENT && im->info.target == TEX_3D;
    u32 n = slices ? tex_depth(&im->info) >> v->base_level : im->layers;
    if (!n) n = 1;
    *base = v->min_layer < n ? v->min_layer : 0;
    return v->num_layers && *base + v->num_layers <= n && !slices ? v->num_layers : n - *base;
}

VkImageView image_view(Image *im, const ViewInfo *v, u32 kind) {
    for (int i = 0; i < im->nviews; i++)
        if (im->views[i].kind == kind && !memcmp(&im->views[i].v, v, sizeof *v)) return im->views[i].view;
    VkFormat fmt = usable_format(v->format);
    if (fmt == VK_FORMAT_UNDEFINED || format_aspects(fmt) != im->aspects) fmt = im->format;
    if (!format_listed(im, fmt)) {      /* not in the image's format list (format_list): seen as its own format */
        static int warned;
        if (warned++ < 16)
            hlog(0, "gpu: view of image vk %d (guest f%u, %ux%u) as vk %d (guest f%u) not in its format list",
                 im->format, im->info.format, im->info.width, tex_height(&im->info), fmt, v->format);
        fmt = im->format;
    }
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->image, .format = fmt};
    u32 base_level = v->base_level < im->levels ? v->base_level : im->levels - 1;
    u32 nlevels = v->num_levels && base_level + v->num_levels <= im->levels ? v->num_levels : im->levels - base_level;
    u32 base_layer, nlayers = view_layers(im, v, kind, &base_layer);
    VkImageViewUsageCreateInfo uci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
    if (kind == VIEW_ATTACHMENT) {
        bool is3d = im->info.target == TEX_3D;
        ci.viewType = nlayers > 1 || is3d ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        if (im->info.target == TEX_1D || im->info.target == TEX_1D_ARRAY) ci.viewType = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        nlevels = 1;
        ci.subresourceRange = (VkImageSubresourceRange){im->aspects, base_level, 1, base_layer, nlayers};
        uci.usage = im->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    } else if (kind == VIEW_STORAGE) {  /* one level, no swizzle, a format storage allows */
        ci.viewType = view_type(v->target);
        if (ci.viewType == VK_IMAGE_VIEW_TYPE_CUBE || ci.viewType == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY)
            ci.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        ci.format = storage_format(fmt);
        if (ci.format == VK_FORMAT_UNDEFINED) return NULL;
        ci.subresourceRange = (VkImageSubresourceRange){im->aspects, base_level, 1, base_layer, nlayers};
        uci.usage = im->usage & VK_IMAGE_USAGE_STORAGE_BIT;
    } else {
        ci.viewType = view_type(v->target);
        if (ci.viewType == VK_IMAGE_VIEW_TYPE_CUBE && nlayers < 6) ci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ci.format = kind == VIEW_SAMPLED_INT ? uint_twin(fmt) : sampled_format(fmt);
        VkImageAspectFlags asp = im->aspects;
        if (asp == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT))
            asp = v->ds_mode == 1 ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        if (kind == VIEW_SAMPLED_INT && (asp & VK_IMAGE_ASPECT_DEPTH_BIT)) return NULL;      /* depth is float */
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
    /* NX_VIEW_LOG: views in a format other than the image's (each image format, view format and kind once): the
     * formats a VkImageFormatListCreateInfo would have to name, and which images need MUTABLE_FORMAT at all */
    if (env.view_log && ci.format != im->format) {
        static struct { VkFormat i, v; u32 kind; } seen[256];
        static int ns;
        bool dup = false;
        for (int i = 0; i < ns; i++) dup |= seen[i].i == im->format && seen[i].v == ci.format && seen[i].kind == kind;
        if (!dup && ns < 256) {
            seen[ns].i = im->format; seen[ns].v = ci.format; seen[ns].kind = kind; ns++;
            hlog(0, "gpu: view: image vk %d (guest f%u, %ux%u%s) seen as vk %d (guest f%u), kind %u", im->format,
                 im->info.format, im->info.width, tex_height(&im->info),
                 im->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ?
                 ", attachment" : "", ci.format, v->format, kind);
        }
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
            if (l == 0 && layer == 0 && env.dump_astc && dumped < 40) {
                char path[MAX_PATH];
                snprintf(path, sizeof path, "%s\\astc_%02d_%ux%u_f%u.pam", frames_dir(), dumped++, w, h,
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
bool temp_image(VkFormat f, u32 w, u32 h, u32 levels, u32 layers, Temp *t) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
        .extent = {w ? w : 1, h ? h : 1, 1}, .mipLevels = levels ? levels : 1, .arrayLayers = layers ? layers : 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(vk.dev, &ci, NULL, &t->image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.dev, t->image, &req);
    if (!vk_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &t->alloc)) { vkDestroyImage(vk.dev, t->image, NULL); return false; }
    VKCHECK(vkBindImageMemory(vk.dev, t->image, t->alloc.mem, t->alloc.offset));
    end_rendering();
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_NONE, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = t->image, .subresourceRange = {format_aspects(f), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cmd(), &d);
    return true;
}

/* scaled images are filled and read through 1x copies, blitted: only where the format can be blitted */
bool scale_blittable(const Image *im) {
    VkFormatFeatureFlags f = format_features(im->format);
    if (im->info.samples <= 1 && f & VK_FORMAT_FEATURE_BLIT_SRC_BIT && f & VK_FORMAT_FEATURE_BLIT_DST_BIT) return true;
    static int warned;
    if (warned++ < 8)
        hlog(0, "gpu: scaled image %ux%u f%u (vk %d): cannot blit, a copy into or out of it is skipped",
             im->info.width, tex_height(&im->info), im->info.format, im->format);
    return false;
}

/* every level and layer of s (made at scale ss) into d (scale ds); w x h: level 0's guest size */
static void blit_levels(VkImage s, u32 ss, VkImage d, u32 ds, VkImageAspectFlags asp, u32 w, u32 h, u32 levels,
                        u32 layers, VkFilter filter) {
    for (u32 l = 0; l < levels; l++) {
        s32 sw = (s32)((w * ss) >> l), sh = (s32)((h * ss) >> l), dw = (s32)((w * ds) >> l), dh = (s32)((h * ds) >> l);
        VkImageBlit b = {{asp, l, 0, layers}, {{0, 0, 0}, {sw ? sw : 1, sh ? sh : 1, 1}},
                         {asp, l, 0, layers}, {{0, 0, 0}, {dw ? dw : 1, dh ? dh : 1, 1}}};
        vkCmdBlitImage(cmd(), s, VK_IMAGE_LAYOUT_GENERAL, d, VK_IMAGE_LAYOUT_GENERAL, 1, &b, filter);
    }
}

/* im remade at scale s (its old contents blitted over where the format allows; otherwise cleared). The Image stays
 * (descriptor sets hold it): its views go, and the image epoch makes every set look its entries up again */
void image_rescale(Image *im, u32 s) {
    if (im->scale == s) return;
    end_rendering();
    VkImage old = im->image;
    VkAlloc olda = im->alloc;
    u32 os = im->scale;
    im->scale = s;
    image_bytes -= olda.size;
    if (!image_vk_create(im)) {
        im->image = old;
        im->alloc = olda;
        im->scale = os;
        image_bytes += olda.size;
        return;
    }
    VkFormatFeatureFlags f = format_features(im->format);
    if (im->valid && im->info.samples <= 1 && f & VK_FORMAT_FEATURE_BLIT_SRC_BIT && f & VK_FORMAT_FEATURE_BLIT_DST_BIT) {
        bool color = im->aspects & VK_IMAGE_ASPECT_COLOR_BIT;
        blit_levels(old, os, im->image, s, im->aspects, im->info.width, tex_height(&im->info), im->levels, im->layers,
                    color ? smooth(im->format) : VK_FILTER_NEAREST);
        barrier();
    }
    for (int i = 0; i < im->nviews; i++) bury(DEAD_VIEW, im->views[i].view, NULL);
    im->nviews = 0;
    bury(DEAD_IMAGE, old, &olda);
    image_epoch++;
}

static bool image_upload_into(Image *im, VkImage target);

static void image_upload_do(Image *im) {
    if (im->scale <= 1) { image_upload_into(im, im->image); return; }
    im->valid = true;
    if (!scale_blittable(im)) return;
    Temp t;
    if (!temp_image(im->format, im->info.width, tex_height(&im->info), im->levels, im->layers, &t)) return;
    if (image_upload_into(im, t.image)) {
        blit_levels(t.image, 1, im->image, im->scale, im->aspects, im->info.width, tex_height(&im->info), im->levels,
                    im->layers, smooth(im->format));
        barrier();
    }
    temp_free(&t);
}

/* false if nothing was copied */
static bool image_upload_into(Image *im, VkImage target) {
    im->valid = true;
    TexLayout L;
    tex_layout(&im->info, &L);
    const u8 *src = gpu_cpu(im->info.addr, L.size);
    if (!src) return false;             /* GPU-only memory: nothing to read */
    if (format_is_astc(im->info.format)) {
        upload_astc(im, &L, src);       /* (never scaled: compressed) */
        return true;
    }
    if (L.bpb != 1 && L.bpb != 2 && L.bpb != 4 && L.bpb != 8 && L.bpb != 16) return false;
    /* packed D+S: not uploaded (checked here, before the texels are deswizzled into staging for nothing) */
    if (im->aspects & VK_IMAGE_ASPECT_STENCIL_BIT && im->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) return false;
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
    end_rendering();
    vkCmdCopyBufferToImage(cmd(), buf, target, VK_IMAGE_LAYOUT_GENERAL, nr, regions);
    barrier();
    return true;
}

void image_upload(Image *im) {
    if (im->valid) return;
    double t0 = ms_now();
    image_upload_do(im);
    hf.upload += ms_now() - t0;
    hf.nupload++;
}

/* Video memory: images are made on first use and nothing tells us when the game is done with one (it reuses pool
 * memory for the next course's textures, leaves old entries in its texture pools, and changes the size of its
 * render targets with dynamic resolution). Over budget, or when an allocation fails, the least recently used
 * images go, except those used in the last EVICT_KEEP frames (bound by a draw, rendered to, copied; their work
 * has been submitted, so they die once the GPU is past it). One that is needed again is made again and
 * uploaded from guest memory (what the GPU had written into it is lost). */
#define EVICT_KEEP 60
static int used_order(const void *a, const void *b) {
    u64 x = (*(Image *const *)a)->used, y = (*(Image *const *)b)->used;
    return x < y ? -1 : x > y;
}
void images_evict(u64 target, bool wait) {
    Image **c = malloc(nimages * sizeof *c);
    int n = 0;
    for (int b = 0; b < IMAGE_BUCKETS; b++)
        for (Image *im = buckets[b]; im; im = im->next)
            if (n < nimages && im->used + EVICT_KEEP <= frame_no + 1 && im->info.addr > DUMMY_ADDRS) c[n++] = im;
    qsort(c, n, sizeof *c, used_order);
    u64 before = image_bytes;
    int first = ndead, gone = 0;
    for (int i = 0; i < n && image_bytes > target; i++, gone++) {
        bucket_unlink(c[i]);
        image_destroy(c[i]);
    }
    for (int i = first; i < ndead; i++) dead[i].value = submitted;     /* not in the work being recorded */
    free(c);
    hlog(1, "gpu: frame %llu: %d of %d images unused for %d frames evicted, %llu -> %llu MB", frame_no + 1, gone, n,
         EVICT_KEEP, before >> 20, image_bytes >> 20);
    if (wait && submitted) {
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &timeline,
                                  .pValues = &submitted};
        VKCHECK(vkWaitSemaphores(vk.dev, &wi, UINT64_MAX));
    }
    reap();
}

/* textures the CPU rewrote since the last frame (write watch over the GPU-visible pools): upload them again */
static int range_cmp(const void *a, const void *b) {
    u64 x = ((const Range *)a)->lo, y = ((const Range *)b)->lo;
    return x < y ? -1 : x > y;
}

double refresh_ms, refresh_mb;          /* time and bytes asked about, for the report every 600 frames */

/* the pages written in [lo, hi), appended to pages[] (write watch reset) */
size_t written_pages(u64 lo, u64 hi, u64 **pages, size_t *cap, size_t n, double *mb) {
    *mb += (double)(hi - lo) / (1 << 20);
    for (;;) {
        if (*cap - n < 4096) *pages = realloc(*pages, (*cap = *cap * 2 + 65536) * sizeof **pages);
        ULONG_PTR count = *cap - n;
        ULONG gran;
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, (void *)(uintptr_t)lo, hi - lo, (void **)(*pages + n), &count, &gran) != 0)
            return n;
        n += count;
        if (count < *cap - n + count) return n;     /* all of them */
    }
}

/* upload again every image (used or not) on the written pages, sorted */
int refresh_pages(const u64 *pages, size_t n) {
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
    return refreshed;
}

/* An image is asked about at each present while it was used in that frame or the one before. One unused for longer
 * is asked about when it is used again (image_touch): until then the write watch keeps its pages' bits, unless an
 * image sharing them was asked about, and that one's refresh_pages uploads it too. Asking about every image each
 * frame took 2 ms of the render thread in Mesagoza (180 MB of pages) */
static bool image_watched(const Image *im) {
    if (!im->valid || im->gpu_written || !im->size || im->info.addr < AS_BASE) return false;
    PoolEnt *p = nmirrors ? pool_find(im->info.addr) : NULL;
    return !p || !p->mir;
}

void image_touch(Image *im) {
    bool idle = im->used + 1 < frame_no;
    im->used = frame_no + 1;
    if (!idle || !image_watched(im)) return;
    static u64 *pages;
    static size_t cap;
    size_t n = written_pages(im->info.addr & ~0xfffull, (im->info.addr + im->size + 0xfff) & ~0xfffull, &pages, &cap, 0,
                             &refresh_mb);
    if (n) refresh_pages(pages, n);
}

/* The asking runs on a thread of its own (2 ms a frame in Mesagoza, on the render thread's critical path): at each
 * present the render thread takes the pages found since the last one, uploads their images again, and hands the
 * thread the ranges of the images to ask about next. A texture the CPU rewrote is so seen one frame later than
 * before */
static Range *scan_rs;
static int scan_nr, scan_caprs;
static u64 *scan_pages;
static size_t scan_n, scan_cap;
static double scan_mb;
static HANDLE scan_go, scan_done;
double scan_ms, scan_ms_max;            /* the scans' time (the thread's), for the report every 600 frames */
u64 scan_skips;                         /* presents that found the last scan still running and did not wait */

static unsigned __stdcall scan_thread(void *arg) {
    (void)arg;
    for (;;) {
        WaitForSingleObject(scan_go, INFINITE);
        double t0 = ms_now();
        /* the ranges sorted and merged (here, not on the render thread: ~0.1 ms a frame) */
        qsort(scan_rs, scan_nr, sizeof *scan_rs, range_cmp);
        int m = 0;
        for (int i = 1; i < scan_nr; i++) {
            if (scan_rs[i].lo <= scan_rs[m].hi + 0x10000) { if (scan_rs[i].hi > scan_rs[m].hi) scan_rs[m].hi = scan_rs[i].hi; }
            else scan_rs[++m] = scan_rs[i];
        }
        scan_nr = m + 1;
        size_t n = 0;
        double mb = 0;
        for (int i = 0; i < scan_nr; i++) n = written_pages(scan_rs[i].lo, scan_rs[i].hi, &scan_pages, &scan_cap, n, &mb);
        scan_n = n;
        scan_mb = mb;
        double t = ms_now() - t0;
        scan_ms += t;
        if (t > scan_ms_max) scan_ms_max = t;
        SetEvent(scan_done);
    }
}

static void refresh_scan(void) {
    if (!scan_go) {
        scan_go = CreateEventW(NULL, FALSE, FALSE, NULL);
        scan_done = CreateEventW(NULL, FALSE, TRUE, NULL);
        _beginthreadex(NULL, 0, scan_thread, NULL, 0, NULL);
    }
    /* the scan started at an earlier present. While it is still running (GetWriteWatch took 5-20 ms a scan in
     * Violet's loads and area streaming, the render thread waiting for it at each present), this present goes on
     * without it: its pages are taken at a later present. Nothing is lost (the write watch keeps the bits of pages
     * not asked about yet); rewritten textures show a frame or more later then. NX_REFRESH_WAIT waits as before */
    if (WaitForSingleObject(scan_done, env.refresh_wait ? INFINITE : 0) != WAIT_OBJECT_0) { scan_skips++; return; }
    refresh_mb += scan_mb;
    scan_mb = 0;
    if (scan_n) {
        /* pages come sorted per range and the ranges are sorted: one sorted list */
        int refreshed = refresh_pages(scan_pages, scan_n);
        static int logged;
        if (refreshed && logged < 5) { logged++; hlog(1, "gpu: %d textures written by the CPU uploaded again", refreshed); }
        scan_n = 0;
    }
    int nr = 0;
    bool whole = env.refresh_pools;                     /* NX_REFRESH_POOLS: ask over whole pools (the old way) */
    for (int i = 0; whole && i < npools; i++) {
        if (!pools[i].cpu || !pools[i].hb || pools[i].mir) continue;
        if (nr == scan_caprs) scan_rs = realloc(scan_rs, (scan_caprs = scan_caprs * 2 + 1024) * sizeof *scan_rs);
        scan_rs[nr++] = (Range){pools[i].cpu, pools[i].cpu + (pools[i].end - pools[i].gpu)};
    }
    for (int b = 0; b < IMAGE_BUCKETS && !whole; b++)
        for (Image *im = buckets[b]; im; im = im->next) {
            if (!image_watched(im) || im->used < frame_no) continue;
            if (nr == scan_caprs) scan_rs = realloc(scan_rs, (scan_caprs = scan_caprs * 2 + 1024) * sizeof *scan_rs);
            scan_rs[nr++] = (Range){im->info.addr & ~0xfffull, (im->info.addr + im->size + 0xfff) & ~0xfffull};
        }
    if (!nr) { SetEvent(scan_done); return; }
    scan_nr = nr;
    SetEvent(scan_go);
}

void refresh_cpu_textures(void) {
    double t0 = ms_now();
    refresh_scan();
    refresh_ms += ms_now() - t0;
}
