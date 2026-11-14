/* Descriptors: the one pipeline layout, samplers, and the game's texture and sampler pools as descriptor sets kept in
 * step with what it registers (descset_get, descset_use, descset_sync), with dummy images for empty entries.
 * Part of the NVN executor (gpu.c, gpu_int.h). */
#include "gpu_int.h"

enum { TB_2D, TB_3D, TB_CUBE, TB_2D_ARRAY, TB_CUBE_ARRAY, TB_1D, TB_1D_ARRAY, TB_U2D, TB_U2D_ARRAY, TB_COUNT };
static const u32 tb_target[TB_COUNT] = {TEX_2D, TEX_3D, TEX_CUBE, TEX_2D_ARRAY, TEX_CUBE_ARRAY, TEX_1D, TEX_1D_ARRAY,
                                        TEX_2D, TEX_2D_ARRAY};
#define TB_IS_INT(b) ((b) >= TB_U2D)
VkDescriptorSetLayout set_ubo, set_tex, set_smp, set_img;
VkPipelineLayout layout_gfx;
static VkDescriptorPool desc_pool;
VkDescriptorSet empty_tex, empty_smp, empty_img;

void layouts_init(void) {
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
    /* texture arrays: fixed size (only a set's last binding may have a variable count) */
    VkDescriptorBindingFlags tbf[TB_COUNT];
    VkDescriptorSetLayoutBinding tb[TB_COUNT];
    for (u32 i = 0; i < TB_COUNT; i++) {
        tbf[i] = bf & ~VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT;
        tb[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, TEXPOOL_MAX, VK_SHADER_STAGE_ALL, NULL};
    }
    VkDescriptorSetLayoutBindingFlagsCreateInfo tbfi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
                                                         .bindingCount = TB_COUNT, .pBindingFlags = tbf};
    VkDescriptorSetLayoutCreateInfo tci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &tbfi,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, .bindingCount = TB_COUNT, .pBindings = tb};
    VKCHECK(vkCreateDescriptorSetLayout(vk.dev, &tci, NULL, &set_tex));
    tci.pNext = &bfi;
    tci.bindingCount = 1;
    VkDescriptorSetLayoutBinding sb = {0, VK_DESCRIPTOR_TYPE_SAMPLER, SMPPOOL_MAX, VK_SHADER_STAGE_ALL, NULL};
    tci.pBindings = &sb;
    VKCHECK(vkCreateDescriptorSetLayout(vk.dev, &tci, NULL, &set_smp));
    VkDescriptorSetLayoutBinding ib = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, TEXPOOL_MAX, VK_SHADER_STAGE_ALL, NULL};
    tci.pBindings = &ib;
    VKCHECK(vkCreateDescriptorSetLayout(vk.dev, &tci, NULL, &set_img));
    VkDescriptorSetLayout sets[4] = {set_ubo, set_tex, set_smp, set_img};
    VkPushConstantRange pr = {PC_STAGES, 0, PC_SIZE + PC_XFB_SIZE};
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 4,
                                      .pSetLayouts = sets, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    VKCHECK(vkCreatePipelineLayout(vk.dev, &lci, NULL, &layout_gfx));
    /* sampler and storage image sets; texture sets have a pool each (alloc_tex_set) */
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_SAMPLER, SMPPOOL_MAX * 16}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, TEXPOOL_MAX * 8}};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 64, .poolSizeCount = 2, .pPoolSizes = ps};
    VKCHECK(vkCreateDescriptorPool(vk.dev, &pci, NULL, &desc_pool));
    pcache_load();
}

VkDescriptorSet alloc_set(VkDescriptorSetLayout l, u32 count) {
    VkDescriptorSetVariableDescriptorCountAllocateInfo vi = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO, .descriptorSetCount = 1,
        .pDescriptorCounts = &count};
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, &vi, desc_pool, 1, &l};
    VkDescriptorSet s;
    if (vkAllocateDescriptorSets(vk.dev, &ai, &s) != VK_SUCCESS) fatal("gpu: out of descriptor sets");
    return s;
}

/* a texture set (TB_COUNT arrays of TEXPOOL_MAX) in a pool of its own, sized for it */
VkDescriptorSet alloc_tex_set(void) {
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, TB_COUNT * TEXPOOL_MAX};
    VkDescriptorPoolCreateInfo pci = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ps};
    VkDescriptorPool pool;
    VKCHECK(vkCreateDescriptorPool(vk.dev, &pci, NULL, &pool));
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &set_tex};
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
DescSet *descsets;
static VkImageView dummy_views[TB_COUNT], dummy_storage;
static VkSampler dummy_sampler;

static Image *dummy_image(u64 addr, u32 target, u32 depth, u32 flags, u32 format) {
    TexInfo t = {0};
    t.addr = addr;                      /* no guest memory */
    t.format = format;                  /* 37 RGBA8, 39 RGBA8UI */
    t.target = target;
    t.width = t.height = t.levels = 1;
    t.depth = depth;
    t.block_height = 0xff;
    t.flags = flags;
    Image *im = image_get(&t);
    VkClearColorValue black = {{0, 0, 0, 1}};
    if (format == 39) black = (VkClearColorValue){.uint32 = {0, 0, 0, 1}};
    VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, VK_REMAINING_ARRAY_LAYERS};
    vkCmdClearColorImage(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &r);
    im->valid = true;
    return im;
}

/* every entry starts as a 1x1 black texture of each view type / a default sampler, so that no index a shader
   computes is unwritten */
void dummies_init(void) {
    Image *im2 = dummy_image(1, TEX_2D_ARRAY, 6, 0, 37);   /* 2D, 2D array, cube and cube array views */
    Image *im3 = dummy_image(3, TEX_3D, 1, 0, 37);
    Image *im1 = dummy_image(4, TEX_1D_ARRAY, 1, 0, 37);
    Image *imu = dummy_image(5, TEX_2D_ARRAY, 1, 0, 39);   /* integer 2D and 2D array views */
    for (u32 b = 0; b < TB_COUNT; b++) {
        u32 tg = tb_target[b];
        bool cube = tg == TEX_CUBE || tg == TEX_CUBE_ARRAY;
        ViewInfo v = {TB_IS_INT(b) ? 39 : 37, tg, 0, 1, 0, cube ? 6 : 1, {2, 3, 4, 5}, 0};
        dummy_views[b] = TB_IS_INT(b) ? image_view(imu, &v, VIEW_SAMPLED_INT) :
                         image_view(tg == TEX_3D ? im3 : tg == TEX_1D || tg == TEX_1D_ARRAY ? im1 : im2, &v, VIEW_SAMPLED);
        if (!dummy_views[b]) fatal("gpu: no dummy texture for view type %u", b);
    }
    ViewInfo v = {37, TEX_2D, 0, 1, 0, 1, {2, 3, 4, 5}, 0};
    Image *im = dummy_image(2, TEX_2D, 1, TEX_FLAG_IMAGE, 37);  /* and a 1x1 storage image */
    dummy_storage = image_view(im, &v, VIEW_STORAGE);
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                               .minFilter = VK_FILTER_LINEAR, .maxLod = 16};
    VKCHECK(vkCreateSampler(vk.dev, &sci, NULL, &dummy_sampler));
}

void descset_fill(VkDescriptorSet set, bool smp, u32 count) {            /* (storage sets stay unwritten) */
    VkDescriptorImageInfo infos[256];
    for (u32 b = 0; b < (smp ? 1 : TB_COUNT); b++) {
        VkDescriptorImageInfo info = {dummy_sampler, smp ? NULL : dummy_views[b], VK_IMAGE_LAYOUT_GENERAL};
        for (int i = 0; i < 256; i++) infos[i] = info;
        for (u32 i = 0; i < count; i += 256) {
            VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = b,
                .dstArrayElement = i, .descriptorCount = count - i < 256 ? count - i : 256,
                .descriptorType = smp ? VK_DESCRIPTOR_TYPE_SAMPLER : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = infos};
            vkUpdateDescriptorSets(vk.dev, 1, &w, 0, NULL);
        }
    }
}

/* d's entries from `from` on start over as `count` empty ones (the Vulkan set has room for the most a pool can
 * have) */
static void descset_entries(DescSet *d, u32 from, u32 count) {
    u32 max = d->kind == DS_SMP ? SMPPOOL_MAX : TEXPOOL_MAX;
    if (count > max) count = max;
    if (!count) count = 1;
    if (from > count) from = count;
    d->idx = realloc(d->idx, count * sizeof *d->idx);
    d->res = realloc(d->res, count);
    d->img = realloc(d->img, count * sizeof *d->img);
    d->intv = realloc(d->intv, count);
    memset(d->idx + from, 0xff, (count - from) * sizeof *d->idx);
    memset(d->res + from, 0, count - from);
    memset(d->img + from, 0, (count - from) * sizeof *d->img);
    memset(d->intv + from, 0, count - from);
    d->count = count;
    d->synced = 0;                      /* the guest's entries are read again (descset_sync) */
    d->gen++;
    desc_gen++;
}

/* The game's pool objects are not followed (nothing says when the GPU is done with one), so a set stays with its
 * address. Past a few sets of a kind, one no draw has used for ten seconds is given to the new pool instead of
 * making another: each is big (a texture set has 9 x 65536 descriptors), and the pool the sampler and image sets
 * come from holds 16 and 8 */
#define DESCSET_IDLE 600
static DescSet *descset_recycle(u32 kind) {
    static const u32 keep[3] = {[DS_TEX] = 8, [DS_SMP] = 12, [DS_IMG] = 6};
    DescSet *old = NULL;
    u32 n = 0;
    for (DescSet *d = descsets; d; d = d->next) {
        if (d->kind != kind) continue;
        n++;
        if (d->used + DESCSET_IDLE < frame_no && (!old || d->used < old->used)) old = d;
    }
    return n >= keep[kind] ? old : NULL;
}

DescSet *descset_get(const CmdPool *p, u32 kind) {
    if (!p->addr) return NULL;
    bool smp = kind == DS_SMP;
    for (DescSet *d = descsets; d; d = d->next)
        if (d->addr == p->addr && d->kind == kind) {
            d->used = frame_no;
            /* a pool made again at the same place with more entries (the new ones showed the black texture) */
            if (p->count > d->count && d->count < (smp ? SMPPOOL_MAX : TEXPOOL_MAX)) descset_entries(d, d->count, p->count);
            return d;
        }
    DescSet *d = descset_recycle(kind);
    if (d) {
        hlog(1, "gpu: %s pool at %llx takes the set of the one at %llx, unused for %llu frames",
             smp ? "sampler" : kind == DS_IMG ? "image" : "texture", p->addr, d->addr, frame_no - d->used);
        if (kind != DS_IMG) descset_fill(d->set, smp, smp ? SMPPOOL_MAX : TEXPOOL_MAX);
    } else {
        d = calloc(1, sizeof *d);
        d->kind = kind;
        d->set = kind == DS_TEX ? alloc_tex_set() : alloc_set(smp ? set_smp : set_img, smp ? SMPPOOL_MAX : TEXPOOL_MAX);
        if (kind != DS_IMG) descset_fill(d->set, smp, smp ? SMPPOOL_MAX : TEXPOOL_MAX);
        d->next = descsets;
        descsets = d;
    }
    d->addr = p->addr;
    d->used = frame_no;
    d->epoch = ~0ull;
    descset_entries(d, 0, p->count);
    hlog(1, "gpu: %s pool at %llx: %u entries", smp ? "sampler" : kind == DS_IMG ? "image" : "texture", p->addr, d->count);
    return d;
}

/* a sampled view of im as view type tg (a tb_target), from the entry's view v, or NULL if the image cannot be seen
 * that way: a 3D image only as 3D (whatever the entry says: the game draws its colour grading LUT as a 2D array
 * and samples it as 3D), a 1D image as 1D or 1D array, a 2D image as 2D (the entry's first layer), 2D array, and
 * cube or cube array if it was made cube compatible */
static VkImageView tex_view_as(Image *im, const ViewInfo *v, u32 tg, u32 kind) {
    ViewInfo w = *v;
    w.target = tg;
    u32 it = im->info.target;
    u32 n = im->layers ? im->layers : 1, base = v->min_layer < n ? v->min_layer : 0;
    u32 vn = v->num_layers && base + v->num_layers <= n ? v->num_layers : n - base;
    if (im->info.samples > 1) {         /* multisampled: only as before (2D, 2D array) */
        if (tg != TEX_2D && tg != TEX_2D_ARRAY) return NULL;
        w.min_layer = base;
        w.num_layers = tg == TEX_2D ? 1 : vn;
        return image_view(im, &w, kind);
    }
    if (it == TEX_3D) {
        if (tg != TEX_3D) return NULL;
        w.min_layer = 0;
        w.num_layers = 1;
        return image_view(im, &w, kind);
    }
    bool is1d = it == TEX_1D || it == TEX_1D_ARRAY;
    if (is1d != (tg == TEX_1D || tg == TEX_1D_ARRAY) || tg == TEX_3D) return NULL;
    w.min_layer = base;
    switch (tg) {
    case TEX_1D: case TEX_2D: w.num_layers = 1; break;
    case TEX_1D_ARRAY: case TEX_2D_ARRAY: w.num_layers = vn; break;
    case TEX_CUBE: case TEX_CUBE_ARRAY:
        if (n < 6 || im->info.width != im->info.height) return NULL;
        if (base + 6 > n) base = w.min_layer = 0;
        w.num_layers = tg == TEX_CUBE ? 6 : (vn >= 6 && base + vn <= n ? vn : n - base) / 6 * 6;
        break;
    }
    return image_view(im, &w, kind);
}

/* entry i of a texture set: im's views of every type it allows, the black texture of the type elsewhere; returns
 * im, or NULL if no view of it was written */
static bool tex_is_int(const Image *im, const ViewInfo *v) {
    if (im->aspects & VK_IMAGE_ASPECT_STENCIL_BIT)
        return !(im->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) || v->ds_mode == 1;
    VkFormat f = usable_format(v->format);
    if (f == VK_FORMAT_UNDEFINED || format_aspects(f) != im->aspects) f = im->format;
    return format_numeric(f) != 0;
}

static Image *tex_write(VkDescriptorSet set, u32 i, Image *im, const ViewInfo *v) {
    VkDescriptorImageInfo info[TB_COUNT];
    VkWriteDescriptorSet w[TB_COUNT];
    bool any = false, isint = im && tex_is_int(im, v);
    for (u32 b = 0; b < TB_COUNT; b++) {             /* an integer texture only in the integer arrays */
        VkImageView view = im && isint == TB_IS_INT(b) ?
                           tex_view_as(im, v, tb_target[b], isint ? VIEW_SAMPLED_INT : VIEW_SAMPLED) : NULL;
        any |= view != NULL;
        info[b] = (VkDescriptorImageInfo){NULL, view ? view : dummy_views[b], VK_IMAGE_LAYOUT_GENERAL};
        w[b] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = b,
            .dstArrayElement = i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .pImageInfo = &info[b]};
    }
    vkUpdateDescriptorSets(vk.dev, TB_COUNT, w, 0, NULL);
    return any ? im : NULL;
}

/* Texture pools: an entry is resolved to an image (made and uploaded if need be) when a draw or dispatch binds its
 * handle: shaders read handles only from c[2], i.e. from the bound textures, so an entry nothing binds is never
 * sampled and may hold anything (the sets are PARTIALLY_BOUND). Binding also marks the image used (eviction). */
static void descset_resolve(DescSet *d, u32 i) {
    const TexDesc *td = d->idx[i] != ~0u ? tdesc_get(d->idx[i]) : NULL;
    Image *im = NULL;
    ViewInfo v;
    if (td && pool_find(td->t.addr)) {
        TexRef tr = {td->t, td->v};
        rs_hint = d->kind == DS_IMG;    /* storage images are scaled as render targets are */
        im = image_ref(&tr, &v);
        rs_hint = false;
        if (im && d->kind == DS_IMG && im->can_scale && im->scale != rs_scale) image_rescale(im, rs_scale);
    }
    if (im) image_upload(im);
    d->res[i] = 1;
    d->gen++;
    desc_gen++;
    if (d->kind == DS_TEX) {
        d->img[i] = tex_write(d->set, i, im, &v);
        d->intv[i] = d->img[i] && tex_is_int(d->img[i], &v);
        return;
    }
    VkDescriptorImageInfo info = {NULL, im ? image_view(im, &v, VIEW_STORAGE) : NULL, VK_IMAGE_LAYOUT_GENERAL};
    if (im && !info.imageView)
        hlog(0, "gpu: image %u (%ux%u format %u, usage %x) has no storage view", i, im->info.width, im->info.height,
             v.format, im->usage);
    if (!info.imageView) { info.imageView = dummy_storage; im = NULL; }
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = d->set, .dstBinding = 0, .dstArrayElement = i,
        .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &info};
    vkUpdateDescriptorSets(vk.dev, 1, &w, 0, NULL);
    d->img[i] = im;
}

/* Images were destroyed or remade since d last looked: the entries that held one are looked up again at their next
 * use (images_gone has each one since; past what it keeps, every entry) */
static void descset_drop_gone(DescSet *d) {
    u64 n = image_epoch - d->epoch;
    if (d->pools != pools_gen) {        /* the pools changed: an entry that found no image may find one now */
        for (u32 i = 0; i < d->count; i++)
            if (!d->img[i]) d->res[i] = 0;
        d->pools = pools_gen;
    }
    if (d->epoch == image_epoch) {
        /* (only the pools) */
    } else if (d->epoch == ~0ull || n > GONE_MAX) {
        memset(d->res, 0, d->count);
        memset(d->img, 0, d->count * sizeof *d->img);
    } else {
        enum { TAB = 2 * GONE_MAX };    /* the gone ones, hashed by address (open addressing, at most half full) */
        const Image *tab[TAB] = {0};
#define GONE_HASH(p) ((u32)(((uintptr_t)(p) >> 4) * 0x9e3779b1u) >> 21 & (TAB - 1))
        for (u64 e = d->epoch; e != image_epoch; e++) {
            const Image *im = images_gone[e % GONE_MAX];
            u32 h = GONE_HASH(im);
            while (tab[h] && tab[h] != im) h = (h + 1) & (TAB - 1);
            tab[h] = im;
        }
        for (u32 i = 0; i < d->count; i++) {
            const Image *im = d->img[i];
            if (!im) continue;
            for (u32 h = GONE_HASH(im); tab[h]; h = (h + 1) & (TAB - 1))
                if (tab[h] == im) { d->res[i] = 0; d->img[i] = NULL; break; }
        }
#undef GONE_HASH
    }
    d->epoch = image_epoch;
    d->gen++;
    desc_gen++;
}

void descset_use(DescSet *d, const u64 *tex, u32 n) {
    if (!d) return;
    /* draws mostly use the textures the last one did: in the same frame and command buffer, with the same entries
     * and images, the images are already touched and the linear ones listed for their copy back (n <= 32; one
     * record per handle array: each stage's textures and images) */
    static struct { const u64 *tex; const DescSet *d; u64 gen, frame, epoch, bind, serial; u64 h[32]; } seen[8];
    int e = 0;
    while (e < 8 && seen[e].tex && seen[e].tex != tex) e++;
    if (e == 8) e = (int)(((uintptr_t)tex >> 4) & 7);
    /* the stage's handles as the last draw had them (bind_gen), or set again to the same ones */
    u64 bind = 0;
    for (int s = 0; s < ST_COUNT; s++)
        if (tex == st.tex[s] || tex == st.img[s]) bind = bind_gen[s] + 1;
    if (seen[e].tex == tex && seen[e].d == d && seen[e].gen == d->gen && seen[e].frame == frame_no &&
        seen[e].epoch == image_epoch && seen[e].serial == slot_serial && d->pools == pools_gen && n <= 32 &&
        ((bind && seen[e].bind == bind) || !memcmp(seen[e].h, tex, n * sizeof *tex))) {
        seen[e].bind = bind;
        return;
    }
    for (int pass = 0; pass < 4; pass++) {
        if (d->epoch != image_epoch || d->pools != pools_gen) descset_drop_gone(d);    /* some pointers may be stale */
        for (u32 k = 0; k < n; k++) {
            u32 h = (u32)tex[k], id = h & 0xfffff;
            if (!h || id >= d->count) continue;
            if (!d->res[id]) descset_resolve(d, id);
            Image *im = d->img[id];
            if (!im) continue;
            image_touch(im);
            if (d->kind == DS_IMG) { im->gpu_written = true; linear_wrote(im); }      /* shaders may store into it */
        }
        if (d->epoch == image_epoch) break;    /* else resolving destroyed an image (a container took it over) */
    }
    if (n <= 32) {
        seen[e].tex = tex;
        seen[e].d = d;
        seen[e].gen = d->gen;
        seen[e].frame = frame_no;
        seen[e].epoch = image_epoch;
        seen[e].serial = slot_serial;
        seen[e].bind = bind;
        memcpy(seen[e].h, tex, n * sizeof *tex);
    }
}

/* once per submission: which entries the guest changed */
void descset_sync(DescSet *d) {
    if (!d || d->synced == submit_serial) return;
    d->synced = submit_serial;
    VkDescriptorImageInfo infos[256];
    VkWriteDescriptorSet writes[256];
    int n = 0;
    for (u32 i = 0; i < d->count; i++) {
        u64 a = d->addr + 32ull * i;
        u32 magic = LD32(a), x = LD32(a + 4);
        u32 want = magic == (d->kind == DS_SMP ? SDESC_MAGIC : TDESC_MAGIC) ? x : ~0u;
        if (want == d->idx[i]) continue;
        d->idx[i] = want;
        if (d->kind != DS_SMP) { d->res[i] = 0; d->gen++; desc_gen++; continue; }
        if (want == ~0u) continue;
        memset(&infos[n], 0, sizeof infos[n]);
        infos[n].sampler = sampler_get(want);
        if (!infos[n].sampler) continue;
        writes[n] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = d->set, .dstBinding = 0,
            .dstArrayElement = i, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &infos[n]};
        if (++n == 256) { vkUpdateDescriptorSets(vk.dev, n, writes, 0, NULL); n = 0; }
    }
    if (n) vkUpdateDescriptorSets(vk.dev, n, writes, 0, NULL);
}
