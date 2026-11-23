/* Temporal anti-aliasing (and later upscaling) with AMD FSR 2 (fsr2.cpp), NX_TAA=1.
 *
 * The game names two things in game.json "taa" (nxrecomp.cmake -> game_config.h): the vertex constant buffer
 * holding the scene camera's view-projection (column-major, as the G-buffer draws use it) and the fragment shader of
 * its tone-map draw, which writes the picture the HUD is then drawn over. Per frame:
 *   - the first G-buffer draw (4+ colour targets and a depth) gives the camera and the scene depth (taa_note_draw);
 *   - draws with that depth bound, but not full-screen triangles (lighting, fog: they read the G-buffer per pixel),
 *     are drawn with the frame's sub-pixel jitter, a viewport offset (taa_draw_jitter, set_dynamic_state);
 *   - after the tone-map draw (taa_after_draw) the pass ends, a compute pass makes camera motion vectors from the
 *     depth and this / the last frame's camera (shaders/taa_mv.comp), FSR 2 resolves the picture with its history
 *     into an image of ours, and that is copied back over the picture; the HUD follows unjittered.
 * A frame FSR 2 did not resolve is never shown jittered: jitter only follows a resolved frame (at most the first
 * jittered frame after a pause shows unresolved).
 * On / off and the sharpening are the menu's (Options > Video, settings.ini "taa" / "taa_sharpness"; a change takes
 * the next frame, history reset); NX_TAA=0/1 and NX_TAA_SHARPNESS (0-1, default 0.3: FSR 2's RCAS) decide at start
 * when set. NX_TAA_JITTER=0 (or the development menu's Jitter): no jitter (motion vectors and history only, to
 * compare); NX_TAA_JITTER_SIGN=x,y. */
#include "gpu_int.h"
#include "options.h"
#include <math.h>

#if defined(NX_FSR2) && defined(NX_TAA_RESOLVE_FS)
#include "fsr2.h"
#include "shaders/taa_mv_spv.h"

static struct {
    bool on, broken, jitter;
    float sharp, sx, sy;
    u64 frame;                          /* frame_no the per-frame values are for */
    float vp[16], vp_prev[16];
    u64 vp_frame, vp_prev_frame;
    u64 depth_addr;                     /* the scene depth (the G-buffer's) */
    Image *depth;                       /* its image, from the last pass drawn with it (taa_pass_depth) */
    float jx, jy;
    u32 jit_index;
    u64 ok_frame;                       /* the last frame resolved */
    LARGE_INTEGER last_qpc;
    /* counts since the last log line (every 600 frames): resolved, of those reset; scene draws jittered, scene draws
     * left unjittered as full-screen triangles, other draws */
    u32 n_res, n_reset, n_jit, n_full, n_other, n_frames;
    /* ours: motion vectors (render size), FSR 2's output (output size), views of the game's depth and picture */
    VkImage mv, out;
    VkAlloc mv_a, out_a;
    VkImageView mv_v, out_v;
    u32 w, h, ow, oh;
    VkImage depth_img, color_img;
    VkImageView depth_v, color_v;
    VkPipeline pipe;
    VkPipelineLayout layout;
    VkDescriptorSetLayout dsl;
} taa = {.ok_frame = ~0ull, .vp_frame = ~0ull, .vp_prev_frame = ~0ull};

bool taa_draw_jitter;                   /* this draw takes the jitter (set_dynamic_state) */
float taa_jit[2];

/* what the menu wants (any thread); the gpu thread takes it at the next frame */
static volatile LONG want_mode;         /* TAA_* (gpu_int.h) */
static volatile float want_sharp = 0.3f;
static volatile LONG want_jitter = 1;   /* (NX_TAA_JITTER; the development menu's switch) */

static float clamp01(float v) { return !(v > 0) ? 0 : v > 1 ? 1 : v; }

bool taa_available(void) { return true; }
bool taa_failed(void) { return taa.broken; }
int taa_mode(void) { return (int)want_mode; }
void taa_set_mode(int m) {
    m = m == TAA_NATIVE ? TAA_NATIVE : TAA_OFF;
    InterlockedExchange(&want_mode, m);
    options_set_int("taa", m);
}
bool taa_jitter(void) { return want_jitter != 0; }
void taa_set_jitter(bool on) { InterlockedExchange(&want_jitter, on); }
float taa_sharpness(void) { return want_sharp; }
void taa_set_sharpness(float s) {
    want_sharp = clamp01(s);
    options_set_float("taa_sharpness", want_sharp);
}

void taa_init(void) {
    const char *e = getenv("NX_TAA");
    want_mode = (e ? atoi(e) : options_get_int("taa", TAA_OFF)) ? TAA_NATIVE : TAA_OFF;
    want_sharp = clamp01((e = getenv("NX_TAA_SHARPNESS")) ? (float)atof(e) : options_get_float("taa_sharpness", 0.3f));
    want_jitter = !(e = getenv("NX_TAA_JITTER")) || atoi(e);
    taa.sx = taa.sy = 1;
    if ((e = getenv("NX_TAA_JITTER_SIGN"))) sscanf(e, "%f,%f", &taa.sx, &taa.sy);
    fsr2_init(vk.phys, vk.dev, vkGetDeviceProcAddr, vkCmdPipelineBarrier2);
}

/* a new frame: on / off and the sharpening from the menu, the camera moves to last frame's slot, the jitter steps
 * (only after a resolved frame: so the first frame after switching on is drawn unjittered, and FSR 2 resets) */
static void taa_frame(void) {
    if (taa.frame == frame_no) return;
    taa.jitter = want_jitter != 0;
    bool on = want_mode != TAA_OFF;
    if (on != taa.on) {
        taa.on = on;
        hlog(0, "taa: FSR 2 temporal anti-aliasing %s%s", on ? "on" : "off", on && !taa.jitter ? " (no jitter)" : "");
    }
    taa.sharp = want_sharp;
    if (taa.vp_frame == taa.frame) {
        memcpy(taa.vp_prev, taa.vp, sizeof taa.vp);
        taa.vp_prev_frame = taa.vp_frame;
    }
    taa.frame = frame_no;
    if (taa.on && ++taa.n_frames >= 600) {
        hlog(0, "taa: last 600 frames: %u resolved (%u reset), per frame %.1f jittered draws, %.1f full-screen, %.1f other",
             taa.n_res, taa.n_reset, taa.n_jit / 600.0, taa.n_full / 600.0, taa.n_other / 600.0);
        taa.n_res = taa.n_reset = taa.n_jit = taa.n_full = taa.n_other = taa.n_frames = 0;
    }
    taa.jx = taa.jy = 0;
    if (taa.jitter && taa.ok_frame + 1 == frame_no && taa.w) {
        fsr2_jitter(taa.jit_index++, taa.w, taa.ow, &taa.jx, &taa.jy);
        taa.jx *= taa.sx, taa.jy *= taa.sy;
    }
}

/* before each draw: the camera from the first G-buffer draw; whether this draw is jittered */
void taa_note_draw(u32 op, const void *payload) {
    if (taa.broken) return;
    taa_frame();
    if (!taa.on) {
        if (taa_draw_jitter) { taa_draw_jitter = false; dyn_gen++; }
        return;
    }
    if (taa.vp_frame != frame_no && st.has_depth) {
        u32 n = 0;
        for (u32 i = 0; i < st.nrt && i < 8; i++) n += st.rt[i].t.addr != 0;
        const BufferRange *u = &st.ubo[ST_VERTEX][NX_TAA_CAMERA_CB - 3];
        const float *m = n >= 4 && u->addr && u->size >= 64 ? (const float *)gpu_cpu(u->addr, 64) : NULL;
        bool ok = m != NULL;
        for (int i = 0; ok && i < 16; i++) ok = isfinite(m[i]);
        if (ok && (m[7] != 0 || m[11] != 0)) {         /* perspective: w depends on the position */
            memcpy(taa.vp, m, sizeof taa.vp);
            taa.vp_frame = frame_no;
            taa.depth_addr = st.depth.t.addr;
        }
    }
    bool full = op == OP_DRAW_ARRAYS && ((const CmdDraw *)payload)->count == 3;
    bool scene = st.has_depth && st.depth.t.addr == taa.depth_addr;
    bool j = (taa.jx != 0 || taa.jy != 0) && scene && !full;
    if (scene) (*(full ? &taa.n_full : j ? &taa.n_jit : &taa.n_other))++;
    else taa.n_other++;
    if (j != taa_draw_jitter || (j && (taa_jit[0] != taa.jx || taa_jit[1] != taa.jy))) {
        taa_draw_jitter = j;
        taa_jit[0] = taa.jx, taa_jit[1] = taa.jy;
        dyn_gen++;                      /* (set_dynamic_state: the viewport) */
    }
}

/* begin_rendering: a pass drawn with the scene depth names its image */
void taa_pass_depth(Image *d) {
    if (taa.on && d && d->info.addr == taa.depth_addr) taa.depth = d;   /* (taa.on: set at the frame's first draw) */
}

static bool make_img(VkFormat f, u32 w, u32 h, VkImageUsageFlags usage, VkImage *img, VkAlloc *a, VkImageView *v) {
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
        .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(vk.dev, &ci, NULL, img) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(vk.dev, *img, &req);
    if (!vk_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, a)) { vkDestroyImage(vk.dev, *img, NULL); return false; }
    vkBindImageMemory(vk.dev, *img, a->mem, a->offset);
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = f, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(vk.dev, &vci, NULL, v) != VK_SUCCESS) return false;
    /* (UNDEFINED -> GENERAL) */
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = *img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cmd(), &di);
    return true;
}

static void drop_img(VkImage *img, VkAlloc *a, VkImageView *v) {
    if (*v) bury(DEAD_VIEW, *v, NULL);
    if (*img) bury(DEAD_IMAGE, *img, a);
    *img = NULL, *v = NULL;
}

/* our images at these sizes */
static bool images(u32 w, u32 h, u32 ow, u32 oh) {
    if (taa.mv && taa.w == w && taa.h == h && taa.ow == ow && taa.oh == oh) return true;
    drop_img(&taa.mv, &taa.mv_a, &taa.mv_v);
    drop_img(&taa.out, &taa.out_a, &taa.out_v);
    taa.w = w, taa.h = h, taa.ow = ow, taa.oh = oh;
    return make_img(VK_FORMAT_R16G16_SFLOAT, w, h, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &taa.mv, &taa.mv_a, &taa.mv_v) &&
           make_img(VK_FORMAT_R16G16B16A16_SFLOAT, ow, oh, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &taa.out, &taa.out_a, &taa.out_v);
}

/* a view of one of the game's images (kept while its Vulkan image is the same) */
static VkImageView game_view(Image *im, VkImageAspectFlags asp, VkImage *had, VkImageView *v) {
    if (*v && *had == im->image) return *v;
    if (*v) bury(DEAD_VIEW, *v, NULL);
    VkImageViewCreateInfo vci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = im->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = im->format, .subresourceRange = {asp, 0, 1, 0, 1}};
    if (vkCreateImageView(vk.dev, &vci, NULL, v) != VK_SUCCESS) { *v = NULL; return NULL; }
    *had = im->image;
    return *v;
}

static bool mv_pipeline(void) {
    if (taa.pipe) return true;
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo sci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 2, .pBindings = b};
    if (vkCreateDescriptorSetLayout(vk.dev, &sci, NULL, &taa.dsl) != VK_SUCCESS) return false;
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 80};
    VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &taa.dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(vk.dev, &lci, NULL, &taa.layout) != VK_SUCCESS) return false;
    VkShaderModule m;
    VkShaderModuleCreateInfo mci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof taa_mv_cs, .pCode = taa_mv_cs};
    if (vkCreateShaderModule(vk.dev, &mci, NULL, &m) != VK_SUCCESS) return false;
    VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
        .layout = taa.layout};
    VkResult r = vkCreateComputePipelines(vk.dev, NULL, 1, &ci, NULL, &taa.pipe);
    vkDestroyShaderModule(vk.dev, m, NULL);
    return r == VK_SUCCESS;
}

/* 4x4 column-major: out = a * b, in double: the camera's translation far from the world's origin makes large terms
 * that cancel (in float the reprojection was off by a fraction of a pixel, differently each frame as the camera moved) */
static void mat_mul(float *out, const float *a, const double *b) {
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += (double)a[k * 4 + rr] * b[c * 4 + k];
            out[c * 4 + rr] = (float)s;
        }
}

/* 4x4 inverse (cofactors, double); false if singular */
static bool mat_inv(double *o, const float *m) {
    double inv[16], det;
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabs(det) < 1e-12) return false;
    for (int i = 0; i < 16; i++) o[i] = inv[i] / det;
    return true;
}

static void off(const char *why) {
    hlog(0, "taa: %s: off", why);
    taa.broken = true;
    if (taa_draw_jitter) { taa_draw_jitter = false; dyn_gen++; }
}

/* after each draw: the tone-map draw (game.json "taa".resolve_fs) is followed by the resolve. pic: its target */
void taa_after_draw(u64 fs_hash, Image *pic) {
    if (!taa.on || taa.broken || fs_hash != NX_TAA_RESOLVE_FS || !pic || taa.ok_frame == frame_no) return;
    Image *d = taa.depth;
    if (taa.vp_frame != frame_no || !d || d->info.addr != taa.depth_addr) return;   /* no camera / depth this frame */
    u32 w = d->info.width * d->scale, h = d->info.height * d->scale;
    u32 ow = pic->info.width * pic->scale, oh = pic->info.height * pic->scale;
    if (w > ow || h > oh || !w || !h) return;
    if (!mv_pipeline()) { off("motion vector pipeline not made"); return; }
    end_rendering();
    if (!images(w, h, ow, oh)) { off("images not made"); return; }
    VkImageView dv = game_view(d, VK_IMAGE_ASPECT_DEPTH_BIT, &taa.depth_img, &taa.depth_v);
    VkImageView cv = game_view(pic, VK_IMAGE_ASPECT_COLOR_BIT, &taa.color_img, &taa.color_v);
    if (!dv || !cv) { off("views not made"); return; }
    VkCommandBuffer c = cmd();
    barrier();                          /* the depth and the picture are written */
    /* motion vectors: last VP x inverse of this VP */
    bool have_prev = taa.vp_prev_frame + 1 == frame_no;
    struct { float reproj[16]; float size[2], flip, zminus1; } pc;
    double inv[16];
    if (!have_prev || !mat_inv(inv, taa.vp)) memcpy(pc.reproj, (float[16]){1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, 64);
    else mat_mul(pc.reproj, taa.vp_prev, inv);
    pc.size[0] = (float)w, pc.size[1] = (float)h;
    pc.flip = window_origin == 1 ? 1.0f : 0.0f;
    pc.zminus1 = depth_mode == 0 ? 1.0f : 0.0f;
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_COMPUTE, taa.pipe);
    VkDescriptorImageInfo ii[2] = {{NULL, dv, VK_IMAGE_LAYOUT_GENERAL}, {NULL, taa.mv_v, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet wr[2] = {
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii[0]},
        {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .pImageInfo = &ii[1]}};
    vkCmdPushDescriptorSetKHR(c, VK_PIPELINE_BIND_POINT_COMPUTE, taa.layout, 0, 2, wr);
    vkCmdPushConstants(c, taa.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(c, (w + 7) / 8, (h + 7) / 8, 1);
    barrier_src(c, VK_ACCESS_2_MEMORY_WRITE_BIT);
    /* FSR 2 */
    LARGE_INTEGER q, fq;
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&fq);
    float dt = taa.last_qpc.QuadPart ? (float)((q.QuadPart - taa.last_qpc.QuadPart) * 1000.0 / fq.QuadPart) : 16.7f;
    taa.last_qpc = q;
    /* the camera: near from the reversed-Z column (clip z = near for every point), the vertical field of view from
     * the y row's length */
    const float *m = taa.vp;
    float ysc = sqrtf(m[1] * m[1] + m[5] * m[5] + m[9] * m[9]);
    Fsr2Frame f = {.color = pic->image, .depth = d->image, .mv = taa.mv, .color_view = cv, .depth_view = dv, .mv_view = taa.mv_v,
                   .color_format = pic->format, .depth_format = d->format, .out = taa.out, .out_view = taa.out_v,
                   .out_format = VK_FORMAT_R16G16B16A16_SFLOAT, .w = w, .h = h, .ow = ow, .oh = oh,
                   .jx = taa.jx, .jy = taa.jy, .dt_ms = fminf(fmaxf(dt, 1), 100), .near_z = m[14] > 0 ? m[14] : 0.1f,
                   .far_z = 1e6f, .fov_y = ysc > 0 ? 2 * atanf(1 / ysc) : 0.8f, .sharpness = taa.sharp,
                   .depth_inverted = true, .depth_infinite = true, .reset = !have_prev || taa.ok_frame + 1 != frame_no};
    if (!fsr2_record(c, &f)) { off("FSR 2 failed"); return; }
    barrier_src(c, VK_ACCESS_2_MEMORY_WRITE_BIT);
    /* back over the picture (the HUD is drawn on it next) */
    VkImageBlit bl = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(s32)ow, (s32)oh, 1}},
                      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {{0, 0, 0}, {(s32)ow, (s32)oh, 1}}};
    vkCmdBlitImage(c, taa.out, VK_IMAGE_LAYOUT_GENERAL, pic->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bl, VK_FILTER_NEAREST);
    barrier();
    taa.ok_frame = frame_no;
    taa.n_res++;
    taa.n_reset += f.reset;
    static bool told;
    if (!told) {
        told = true;
        hlog(0, "taa: first frame resolved: %ux%u -> %ux%u, near %.3f, fov %.1f deg", w, h, ow, oh, f.near_z, f.fov_y * 57.2958f);
    }
}
#else
void taa_init(void) {
    if (getenv("NX_TAA")) hlog(0, "taa: not in this build (needs FSR 2 and game.json \"taa\")");
}
bool taa_available(void) { return false; }
bool taa_failed(void) { return false; }
bool taa_jitter(void) { return false; }
void taa_set_jitter(bool on) { (void)on; }
int taa_mode(void) { return 0; }
void taa_set_mode(int m) { (void)m; }
float taa_sharpness(void) { return 0; }
void taa_set_sharpness(float s) { (void)s; }
void taa_note_draw(u32 op, const void *payload) { (void)op; (void)payload; }
void taa_pass_depth(Image *d) { (void)d; }
void taa_after_draw(u64 fs_hash, Image *pic) { (void)fs_hash; (void)pic; }
bool taa_draw_jitter;
float taa_jit[2];
#endif
