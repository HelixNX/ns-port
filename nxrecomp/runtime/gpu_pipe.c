/* Pipelines: built by key (shaders and attachment formats; all other state is dynamic), the pipeline cache kept
 * between sessions (pipelines.bin), pipeline binaries (VK_KHR_pipeline_binary), async builds, and the prebuild of
 * the pipelines listed in earlier sessions (pipelines.list).
 * Part of the NVN executor (gpu.c, gpu_int.h). */
#include "gpu_int.h"

static VkPipelineCache pcache;

/* The pipeline cache is kept between sessions in pipelines.bin next to the shaders (build/), so pipelines built
 * once are not compiled again: pipeline builds were most of the stalls our side of a hitch (NX_HITCH_MS). A file
 * from another device or driver (header: vendor, device, cache UUID) is ignored. Saved by a thread every minute
 * after new builds and at exit (gpu_save_pipelines), written to a temporary file first so a killed process does
 * not leave half a file */
static char pcache_path[MAX_PATH];
static volatile long pcache_builds;     /* pipelines built, against the count at the last save */
static long pcache_saved;
static SRWLOCK pcache_lock = SRWLOCK_INIT;

void pcache_load(void) {
    if (vk.pipeline_binary) return;     /* pipeline binaries instead (bins_load) */
    build_file(pcache_path, sizeof pcache_path, "pipelines.bin");
    void *data = NULL;
    size_t size = 0;
    FILE *f = fopen(pcache_path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        size = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        data = malloc(size);
        if (fread(data, 1, size, f) != size) size = 0;
        fclose(f);
    }
    const VkPipelineCacheHeaderVersionOne *h = data;
    const char *why = !f ? "none saved" : NULL;
    if (!why && (size < sizeof *h || h->headerVersion != VK_PIPELINE_CACHE_HEADER_VERSION_ONE)) why = "not a cache";
    if (!why && (h->vendorID != vk.props.vendorID || h->deviceID != vk.props.deviceID ||
                 memcmp(h->pipelineCacheUUID, vk.props.pipelineCacheUUID, VK_UUID_SIZE)))
        why = "another device or driver";
    VkPipelineCacheCreateInfo cci = {VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                     .initialDataSize = why ? 0 : size, .pInitialData = why ? NULL : data};
    VKCHECK(vkCreatePipelineCache(vk.dev, &cci, NULL, &pcache));
    if (why) hlog(1, "gpu: pipeline cache %s: %s", pcache_path, why);
    else hlog(1, "gpu: pipeline cache %s: loaded (%llu KB)", pcache_path, (unsigned long long)size >> 10);
    free(data);
}

void gpu_save_pipelines(void) {
    if (!pcache || pcache_builds == pcache_saved) return;
    AcquireSRWLockExclusive(&pcache_lock);
    long builds = pcache_builds;
    size_t size = 0;
    void *data = NULL;
    if (vkGetPipelineCacheData(vk.dev, pcache, &size, NULL) == VK_SUCCESS && size && (data = malloc(size)) &&
        vkGetPipelineCacheData(vk.dev, pcache, &size, data) == VK_SUCCESS) {
        char tmp[MAX_PATH];
        snprintf(tmp, sizeof tmp, "%s.tmp", pcache_path);
        FILE *f = fopen(tmp, "wb");
        bool ok = f && fwrite(data, 1, size, f) == size;
        if (f && fclose(f)) ok = false;
        if (ok && MoveFileExA(tmp, pcache_path, MOVEFILE_REPLACE_EXISTING)) {
            pcache_saved = builds;
            hlog(1, "gpu: pipeline cache saved (%llu KB)", (unsigned long long)size >> 10);
        } else hlog(0, "gpu: pipeline cache: could not write %s", pcache_path);
    }
    free(data);
    ReleaseSRWLockExclusive(&pcache_lock);
}

unsigned __stdcall pcache_thread(void *arg) {
    (void)arg;
    for (;;) {
        Sleep(60000);
        gpu_save_pipelines();
    }
}
enum { P_LISTED, P_BUILDING, P_READY };   /* recorded, not built / being built / built (pipe NULL: failed) */
typedef struct Pipe {
    PipeKey key;
    VkPipeline pipe;
    volatile LONG state;
    bool listed;
    bool no_async;                      /* a worker could not build it from disk: the render thread does */
    struct Pipe *next;
    struct Pipe *qnext;                 /* in the async queue */
} Pipe;
#define PIPE_BUCKETS 1024
static Pipe *pipes[PIPE_BUCKETS];       /* under pipes_lock */

/* A pipeline leaves P_BUILDING: the render thread may be waiting for it (pipeline_get). It used to poll with Sleep(1),
 * which sleeps to the next timer tick (up to 15.6 ms: nothing here raises the timer resolution) for each pipeline a
 * worker was building */
static SRWLOCK pipe_wait_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE pipe_wait_cv = CONDITION_VARIABLE_INIT;
static void pipe_set_state(Pipe *p, LONG state) {
    AcquireSRWLockExclusive(&pipe_wait_lock);
    InterlockedExchange(&p->state, state);
    ReleaseSRWLockExclusive(&pipe_wait_lock);
    WakeAllConditionVariable(&pipe_wait_cv);
}
static SRWLOCK pipes_lock = SRWLOCK_INIT;
u32 npipes;
static FILE *pipe_list;                 /* pipelines.list, appended to */
#define PIPE_LIST_MAGIC 0x4c50584eu     /* "NXPL", then the version and the key size */
#define PIPE_LIST_VERSION 2u
/* NX_PIPE_LOG (env.pipe_log): log each pipeline's shaders before it is made (a driver that aborts while compiling one) */
u64 pipe_vs_hash, pipe_fs_hash;

/* looked up for every draw: hashed a word at a time (the key has no padding, and its size is a multiple of 8) */
static u32 pipe_bucket(const PipeKey *k) {
    _Static_assert(sizeof(PipeKey) % 8 == 0, "PipeKey hashed in words");
    u64 h = 0x9e3779b97f4a7c15ull;
    for (size_t i = 0; i < sizeof *k; i += 8) {
        u64 w;
        memcpy(&w, (const u8 *)k + i, 8);
        h = (h ^ w) * 0xff51afd7ed558ccdull;
    }
    return (u32)((h ^ h >> 29) % PIPE_BUCKETS);
}

static Pipe *pipe_find(const PipeKey *k, u32 h) {
    for (Pipe *p = pipes[h]; p; p = p->next)
        if (!memcmp(&p->key, k, sizeof *k)) return p;
    return NULL;
}

/* the entry for k, added in state `add` if there is none; *claimed: the caller is to build it (it was added, or
 * was P_LISTED and is now P_BUILDING) */
static Pipe *pipe_claim(const PipeKey *k, LONG add, bool *claimed) {
    u32 h = pipe_bucket(k);
    *claimed = false;
    AcquireSRWLockShared(&pipes_lock);
    Pipe *p = pipe_find(k, h);
    ReleaseSRWLockShared(&pipes_lock);
    if (p && p->state != P_LISTED) return p;
    AcquireSRWLockExclusive(&pipes_lock);
    if (!(p = pipe_find(k, h))) {
        p = calloc(1, sizeof *p);
        p->key = *k;
        p->state = add;
        p->listed = add == P_LISTED;
        p->next = pipes[h];
        pipes[h] = p;
        *claimed = add == P_BUILDING;
    } else if (add == P_BUILDING && p->state == P_LISTED) {
        p->state = P_BUILDING;
        *claimed = true;
    }
    ReleaseSRWLockExclusive(&pipes_lock);
    return p;
}

/* the pipeline for k, `next` chained into its create info (binaries, capture) */
static VkResult pipeline_create(const PipeKey *k, const void *next, VkShaderModule vs, VkShaderModule fs, VkPipeline *pipe) {
    VkResult r;
    if (k->compute) {
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, next,
            .stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                      vk.warp_stages & VK_SHADER_STAGE_COMPUTE_BIT ? &vk_warp : NULL, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = vs, .pName = "main"}, .layout = layout_gfx};
        r = vkCreateComputePipelines(vk.dev, pcache, 1, &ci, NULL, pipe);
    } else {
        /* the alpha test is a specialization constant, so a draw without one has no discard from it (early depth) */
        /* constant 2, both stages: TEX_INT, the texture slots that hold integer textures (a shader without the
         * constant ignores it) */
        u32 fdata[2] = {k->alpha_func, k->fs_texint}, vdata[2] = {k->vs_mask, k->vs_texint};
        VkSpecializationMapEntry sme[2] = {{0, 0, 4}, {2, 4, 4}};
        VkSpecializationInfo spec = {2, sme, sizeof fdata, fdata};
        /* the vertex shader's integer inputs (INT_ATTRS, constant 1): which inputs are fetched as USCALED / SSCALED */
        VkSpecializationMapEntry vsme[2] = {{1, 0, 4}, {2, 4, 4}};
        VkSpecializationInfo vspec = {2, vsme, sizeof vdata, vdata};
        VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, vk.warp_stages & VK_SHADER_STAGE_VERTEX_BIT ? &vk_warp : NULL,
             .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main", .pSpecializationInfo = &vspec},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, vk.warp_stages & VK_SHADER_STAGE_FRAGMENT_BIT ? &vk_warp : NULL,
             .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main", .pSpecializationInfo = &spec}};
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
                                                  .logicOp = (VkLogicOp)k->logic_op,
                                                  .attachmentCount = k->ncolor, .pAttachments = att};
        static const VkDynamicState dyn[] = {   /* the logic op last: left out without a dynamic logic op */
            VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT, VK_DYNAMIC_STATE_LINE_WIDTH,
            VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_DEPTH_BOUNDS,
            VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
            VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
            VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
            VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
            VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
            VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT, VK_DYNAMIC_STATE_POLYGON_MODE_EXT,
            VK_DYNAMIC_STATE_RASTERIZATION_SAMPLES_EXT, VK_DYNAMIC_STATE_SAMPLE_MASK_EXT,
            VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT, VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT,
            VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT,
            VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT, VK_DYNAMIC_STATE_DEPTH_CLAMP_ENABLE_EXT, VK_DYNAMIC_STATE_LOGIC_OP_EXT};
        VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                               .dynamicStateCount = sizeof dyn / sizeof *dyn - !vk.dyn_logic_op,
                                               .pDynamicStates = dyn};
        VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, next, .colorAttachmentCount = k->ncolor,
            .pColorAttachmentFormats = k->color, .depthAttachmentFormat = k->depth, .stencilAttachmentFormat = k->stencil};
        /* without dynamic vertex input the layout comes from the key (vi_key); strides stay dynamic (bound with the
         * buffers), and VERTEX_INPUT_EXT in dyn[] becomes VERTEX_INPUT_BINDING_STRIDE */
        VkDynamicState dyn2[sizeof dyn / sizeof *dyn];
        VkVertexInputBindingDescription vb[32];
        VkVertexInputAttributeDescription va[48];
        VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                   .pVertexBindingDescriptions = vb, .pVertexAttributeDescriptions = va};
        if (!vk.vi_dynamic) {
            memcpy(dyn2, dyn, sizeof dyn);
            for (u32 i = 0; i < sizeof dyn / sizeof *dyn; i++)
                if (dyn2[i] == VK_DYNAMIC_STATE_VERTEX_INPUT_EXT) dyn2[i] = VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
            dy.pDynamicStates = dyn2;
            u32 used = 0;
            for (u32 i = 0; i < 32; i++) {
                u32 e = k->vi[i];
                if (e & 0xff) va[vi.vertexAttributeDescriptionCount++] = (VkVertexInputAttributeDescription){
                    i, e >> 8 & 31, (VkFormat)(e & 0xff), e >> 13 & 0x3fff};
                else if (i < 16)        /* the shader may read it: zeros from binding 31 (a null buffer) */
                    va[vi.vertexAttributeDescriptionCount++] = (VkVertexInputAttributeDescription){
                        i, 31, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
                if (e & 0xff) used |= 1u << (e >> 8 & 31);
            }
            used |= 1u << 31;
            for (u32 s = 0; s < 32; s++)
                if (used >> s & 1)
                    vb[vi.vertexBindingDescriptionCount++] = (VkVertexInputBindingDescription){s, 0,
                        s < 31 && k->vi_inst >> s & 1 ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX};
        }
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri,
            .stageCount = k->fs ? 2 : 1, .pStages = stages, .pVertexInputState = vk.vi_dynamic ? NULL : &vi,
            .pInputAssemblyState = &ia, .pViewportState = &vp,
            .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
            .pDynamicState = &dy, .layout = layout_gfx};
        r = vkCreateGraphicsPipelines(vk.dev, pcache, 1, &ci, NULL, pipe);
    }
    return r;
}

/* Pipeline binaries (vk.pipeline_binary): each pipeline compiled here is captured and its binaries appended to
 * pipelines.bins (build/) under its key, and a pipeline whose key is there is made from them instead of compiled.
 * The file belongs to one device, driver and shader translator version (its header); for another it starts over.
 * Pipelines made from binaries or captured may not use a pipeline cache, so there is none then (pcache_load).
 * A record: the key, the number of binaries, then each one's key size, key (32 bytes), data size and data. */
typedef struct BinHdr { u32 magic, version, key_size, vendor, device, driver, translator; u8 uuid[VK_UUID_SIZE]; } BinHdr;
typedef struct BinEnt { PipeKey key; const u8 *rec; struct BinEnt *next; } BinEnt;
#define BINS_MAGIC 0x42505842u          /* "BXPB" */
#define BINS_MAX 8                      /* binaries per pipeline (one per stage in practice) */
static BinEnt *bins[PIPE_BUCKETS];      /* written before the prebuild threads start, read-only after */
static u8 *bins_data;
static FILE *bins_file;
static SRWLOCK bins_lock = SRWLOCK_INIT;
static volatile LONG bins_made, bins_compiled;

static u32 translator_version(void) {
    const char *t = strrchr(spv_dir, '\\');
    return t ? (u32)strtoul(t + 1, NULL, 16) : 0;
}

static BinHdr bins_header(void) {
    /* (the version carries the subgroup size the pipelines were made for: NX_WARP64 runs keep their own) */
    BinHdr h = {BINS_MAGIC, 1 | vk_warp.requiredSubgroupSize << 8, sizeof(PipeKey), vk.props.vendorID, vk.props.deviceID, vk.props.driverVersion,
                translator_version()};
    memcpy(h.uuid, vk.props.pipelineCacheUUID, VK_UUID_SIZE);
    return h;
}

/* the size of the record at q (after its key), 0 if it does not fit in the n bytes left */
static size_t bins_record_size(const u8 *q, size_t n) {
    u32 count, size;
    if (n < 4) return 0;
    memcpy(&count, q, 4);
    if (!count || count > BINS_MAX) return 0;
    size_t at = 4;
    for (u32 i = 0; i < count; i++) {
        if (n - at < 4 + VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR + 4) return 0;
        memcpy(&size, q + at + 4 + VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR, 4);
        at += 8 + VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR;
        if (n - at < size) return 0;
        at += size;
    }
    return at;
}

void bins_load(void) {
    char path[MAX_PATH];
    build_file(path, sizeof path, "pipelines.bins");
    BinHdr want = bins_header();
    size_t size = 0, good = 0;
    u32 n = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        size = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        bins_data = malloc(size + 1);
        if (fread(bins_data, 1, size, f) != size) size = 0;
        fclose(f);
    }
    if (size >= sizeof want && !memcmp(bins_data, &want, sizeof want)) {
        good = sizeof want;
        while (size - good >= sizeof(PipeKey)) {
            size_t rs = bins_record_size(bins_data + good + sizeof(PipeKey), size - good - sizeof(PipeKey));
            if (!rs) break;
            BinEnt *e = malloc(sizeof *e);
            memcpy(&e->key, bins_data + good, sizeof e->key);
            e->rec = bins_data + good + sizeof(PipeKey);
            u32 h = pipe_bucket(&e->key);
            e->next = bins[h];
            bins[h] = e;
            good += sizeof(PipeKey) + rs;
            n++;
        }
    }
    if (good && good == size) bins_file = fopen(path, "ab");
    else if ((bins_file = fopen(path, "wb")) != NULL) {    /* new, another device / driver / translator, or cut short */
        if (good) fwrite(bins_data, 1, good, bins_file);
        else fwrite(&want, sizeof want, 1, bins_file);
        fflush(bins_file);
    }
    hlog(1, "gpu: %u pipelines' binaries in %s (%llu KB)%s", n, path, (unsigned long long)good >> 10,
         size && !good ? ": another device, driver or translator, starting over" : "");
}

static const BinEnt *bins_find(const PipeKey *k) {
    for (const BinEnt *e = bins[pipe_bucket(k)]; e; e = e->next)
        if (!memcmp(&e->key, k, sizeof *k)) return e;
    return NULL;
}

/* the pipeline made from e's binaries; false if the driver takes them no more */
static bool bins_make(const PipeKey *k, const BinEnt *e, VkShaderModule vs, VkShaderModule fs, VkPipeline *pipe) {
    VkPipelineBinaryKeyKHR keys[BINS_MAX];
    VkPipelineBinaryDataKHR data[BINS_MAX];
    VkPipelineBinaryKHR hs[BINS_MAX] = {0};
    u32 count, size;
    memcpy(&count, e->rec, 4);
    const u8 *q = e->rec + 4;
    for (u32 i = 0; i < count; i++) {
        keys[i] = (VkPipelineBinaryKeyKHR){VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
        memcpy(&keys[i].keySize, q, 4);
        memcpy(keys[i].key, q + 4, VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR);
        memcpy(&size, q + 4 + VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR, 4);
        data[i] = (VkPipelineBinaryDataKHR){size, (void *)(q + 8 + VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR)};
        q += 8 + VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR + size;
    }
    VkPipelineBinaryKeysAndDataKHR kd = {count, keys, data};
    VkPipelineBinaryCreateInfoKHR ci = {VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, .pKeysAndDataInfo = &kd};
    VkPipelineBinaryHandlesInfoKHR hi = {VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR,
                                         .pipelineBinaryCount = count, .pPipelineBinaries = hs};
    VkResult r = vkCreatePipelineBinariesKHR(vk.dev, &ci, NULL, &hi);
    if (r == VK_SUCCESS) {
        VkPipelineBinaryInfoKHR bi = {VK_STRUCTURE_TYPE_PIPELINE_BINARY_INFO_KHR, NULL, count, hs};
        r = pipeline_create(k, &bi, vs, fs, pipe);
    }
    for (u32 i = 0; i < count; i++)
        if (hs[i]) vkDestroyPipelineBinaryKHR(vk.dev, hs[i], NULL);
    return r == VK_SUCCESS;
}

/* append the binaries of pipe (created with CAPTURE_DATA) to pipelines.bins, then let the driver drop them */
static void bins_capture(const PipeKey *k, VkPipeline pipe) {
    VkPipelineBinaryCreateInfoKHR ci = {VK_STRUCTURE_TYPE_PIPELINE_BINARY_CREATE_INFO_KHR, .pipeline = pipe};
    VkPipelineBinaryKHR hs[BINS_MAX] = {0};
    VkPipelineBinaryHandlesInfoKHR hi = {VK_STRUCTURE_TYPE_PIPELINE_BINARY_HANDLES_INFO_KHR};
    if (bins_file && vkCreatePipelineBinariesKHR(vk.dev, &ci, NULL, &hi) == VK_SUCCESS && hi.pipelineBinaryCount &&
        hi.pipelineBinaryCount <= BINS_MAX) {
        hi.pPipelineBinaries = hs;
        u32 count = hi.pipelineBinaryCount;
        if (vkCreatePipelineBinariesKHR(vk.dev, &ci, NULL, &hi) == VK_SUCCESS) {
            size_t cap = sizeof *k + 4 + count * (8 + VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR), at = sizeof *k + 4;
            u8 *rec = malloc(cap);
            bool ok = rec != NULL;
            memcpy(rec, k, sizeof *k);
            memcpy(rec + sizeof *k, &count, 4);
            for (u32 i = 0; i < count && ok; i++) {
                VkPipelineBinaryDataInfoKHR di = {VK_STRUCTURE_TYPE_PIPELINE_BINARY_DATA_INFO_KHR, .pipelineBinary = hs[i]};
                VkPipelineBinaryKeyKHR bk = {VK_STRUCTURE_TYPE_PIPELINE_BINARY_KEY_KHR};
                size_t size = 0;
                ok = vkGetPipelineBinaryDataKHR(vk.dev, &di, &bk, &size, NULL) == VK_SUCCESS && size < (1u << 31);
                if (!ok) break;
                cap += size;
                u8 *grown = realloc(rec, cap);
                if (!(ok = grown != NULL)) break;
                rec = grown;
                u32 ks = bk.keySize, ds = (u32)size;
                memcpy(rec + at, &ks, 4);
                memset(rec + at + 4, 0, VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR);
                memcpy(rec + at + 4, bk.key, ks <= VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR ? ks : VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR);
                memcpy(rec + at + 4 + VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR, &ds, 4);
                at += 8 + VK_MAX_PIPELINE_BINARY_KEY_SIZE_KHR;
                ok = vkGetPipelineBinaryDataKHR(vk.dev, &di, &bk, &size, rec + at) == VK_SUCCESS && size == ds;
                at += ds;
            }
            if (ok) {
                AcquireSRWLockExclusive(&bins_lock);
                fwrite(rec, 1, at, bins_file);
                fflush(bins_file);
                ReleaseSRWLockExclusive(&bins_lock);
            }
            free(rec);
            for (u32 i = 0; i < count; i++)
                if (hs[i]) vkDestroyPipelineBinaryKHR(vk.dev, hs[i], NULL);
        }
    }
    VkReleaseCapturedPipelineDataInfoKHR ri = {VK_STRUCTURE_TYPE_RELEASE_CAPTURED_PIPELINE_DATA_INFO_KHR, .pipeline = pipe};
    vkReleaseCapturedPipelineDataKHR(vk.dev, &ri, NULL);
}

/* the pipeline for k: from its binaries if they are kept, else compiled (and captured) */
static VkPipeline pipeline_build(const PipeKey *k, VkShaderModule vs, VkShaderModule fs) {
    VkPipeline pipe = NULL;
    VkResult r;
    const BinEnt *e = vk.pipeline_binary ? bins_find(k) : NULL;
    if (e && bins_make(k, e, vs, fs, &pipe)) {
        InterlockedIncrement(&bins_made);
        return pipe;
    }
    if (vk.pipeline_binary) {
        VkPipelineCreateFlags2CreateInfoKHR fl = {VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR,
                                                  .flags = VK_PIPELINE_CREATE_2_CAPTURE_DATA_BIT_KHR};
        r = pipeline_create(k, &fl, vs, fs, &pipe);
        if (r == VK_SUCCESS) bins_capture(k, pipe);
        InterlockedIncrement(&bins_compiled);
    } else r = pipeline_create(k, NULL, vs, fs, &pipe);
    InterlockedIncrement(&pcache_builds);
    if (r != VK_SUCCESS) {
        hlog(0, "gpu: %s pipeline for %016llx / %016llx failed: %d", k->compute ? "compute" : "graphics", k->vs, k->fs, r);
        pipe = NULL;
    }
    return pipe;
}

static VkShaderModule spv_module(u64 hash);

/* With NX_ASYNC_SHADERS, a graphics pipeline first needed during play is built by a worker (from the SPIR-V on
 * disk, as the prebuild does) and its draws are skipped until it is ready, as for shaders being translated: the
 * render thread compiled it in the draw before, ~100 ms a pipeline on amdvlk (hitches of 0.1-1 s). Draws also skip
 * a pipeline the prebuild is still building. Compute pipelines are still built in the dispatch (skipping one would
 * leave later draws without its results). */
#define ASYNC_PIPE_THREADS 2
static Pipe *async_head, *async_tail;
static SRWLOCK async_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE async_cv = CONDITION_VARIABLE_INIT;
static volatile LONG async_built, async_failed;

static unsigned __stdcall async_pipe_thread(void *arg) {
    (void)arg;
    for (;;) {
        AcquireSRWLockExclusive(&async_lock);
        while (!async_head) SleepConditionVariableSRW(&async_cv, &async_lock, INFINITE, 0);
        Pipe *p = async_head;
        async_head = p->qnext;
        if (!async_head) async_tail = NULL;
        ReleaseSRWLockExclusive(&async_lock);
        const PipeKey *k = &p->key;
        VkShaderModule vs = spv_module(k->vs), fs = k->fs ? spv_module(k->fs) : NULL;
        if (vs && (!k->fs || fs)) {
            p->pipe = pipeline_build(k, vs, fs);
            InterlockedIncrement(&async_built);
            pipe_set_state(p, P_READY);
        } else {                                /* not on disk: back to the render thread */
            p->no_async = true;
            InterlockedIncrement(&async_failed);
            pipe_set_state(p, P_LISTED);
        }
        if (vs) vkDestroyShaderModule(vk.dev, vs, NULL);
        if (fs) vkDestroyShaderModule(vk.dev, fs, NULL);
    }
}

static void async_pipe_queue(Pipe *p) {
    static bool started;
    if (!started) {
        started = true;
        for (int i = 0; i < ASYNC_PIPE_THREADS; i++) {
            HANDLE t = (HANDLE)_beginthreadex(NULL, 0, async_pipe_thread, NULL, 0, NULL);
            if (t) { SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(t); }
        }
    }
    AcquireSRWLockExclusive(&async_lock);
    p->qnext = NULL;
    if (async_tail) async_tail->qnext = p; else async_head = p;
    async_tail = p;
    ReleaseSRWLockExclusive(&async_lock);
    WakeConditionVariable(&async_cv);
}

/* the render thread's pipeline for k, from modules it has loaded. async (graphics draws with NX_ASYNC_SHADERS): NULL
 * with *pending set while a worker builds it */
VkPipeline pipeline_get(const PipeKey *k, VkShaderModule vs, VkShaderModule fs, bool async, bool *pending) {
    /* most draws repeat the previous draw's key: the last ready pipeline, without the hash and the lock (only the
     * render thread calls this; pipelines are never destroyed, and a ready one never changes) */
    static PipeKey last_key;
    static Pipe *last;
    *pending = false;
    if (last && !memcmp(&last_key, k, sizeof *k)) return last->pipe;
    bool claimed;
    Pipe *p = pipe_claim(k, P_BUILDING, &claimed);
    while (!claimed) {
        if (p->state == P_READY) {
            if (p->pipe) last_key = *k, last = p;
            return p->pipe;
        }
        if (async) { *pending = true; return NULL; }
        double t0 = ms_now();                   /* a worker is building it */
        AcquireSRWLockExclusive(&pipe_wait_lock);
        while (p->state == P_BUILDING) SleepConditionVariableSRW(&pipe_wait_cv, &pipe_wait_lock, INFINITE, 0);
        ReleaseSRWLockExclusive(&pipe_wait_lock);
        hf.pipe += ms_now() - t0;
        hf.npipe++;
        if (p->state == P_READY) return p->pipe;
        p = pipe_claim(k, P_BUILDING, &claimed);    /* the worker gave it back (no SPIR-V on disk): built here */
    }
    if (env.pipe_log && !k->compute) {
        char fb[256];
        int fn = 0;
        for (u32 i = 0; i < k->ncolor; i++) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(vk.phys, k->color[i], &fp);
            fn += snprintf(fb + fn, sizeof fb - fn, " c%u=%d%s", i, k->color[i],
                           !k->color[i] || fp.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT ? "" : "(not renderable)");
        }
        VkFormatProperties dp = {0};
        if (k->depth) vkGetPhysicalDeviceFormatProperties(vk.phys, k->depth, &dp);
        hlog(0, "gpu: pipeline %u: vs %016llx fs %016llx%s depth %d%s stencil %d samples %u logic op %u",
             npipes + 1, k->vs, k->fs, fb, k->depth,
             !k->depth || dp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ? "" : "(not renderable)",
             k->stencil, k->samples, k->logic_op);
    } else if (env.pipe_log) hlog(0, "gpu: compute pipeline: cs %016llx", k->vs);
    if (async && !p->no_async) {
        if (pipe_list && !p->listed) {          /* only the render thread writes the list */
            fwrite(k, sizeof *k, 1, pipe_list);
            fflush(pipe_list);
            p->listed = true;
        }
        if (++npipes % 100 == 0) hlog(1, "gpu: %u pipelines (%ld built by workers during play)", npipes, async_built);
        async_pipe_queue(p);
        *pending = true;
        return NULL;
    }
    double t0 = ms_now();
    VkPipeline pipe = pipeline_build(k, vs, fs);
    hf.pipe += ms_now() - t0;
    hf.npipe++;
    p->pipe = pipe;
    InterlockedExchange(&p->state, P_READY);
    if (pipe_list && !p->listed) {              /* only the render thread writes the list */
        fwrite(k, sizeof *k, 1, pipe_list);
        fflush(pipe_list);
    }
    if (++npipes % 100 == 0) hlog(1, "gpu: %u pipelines", npipes);
    return pipe;
}

/* a module from the SPIR-V on disk or in the pack, for the prebuild; NULL if not there */
static VkShaderModule spv_module(u64 hash) {
    long n = 0;
    u32 *code = spv_read(hash, &n);
    if (!code) return NULL;
    VkShaderModule m = NULL;
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = (size_t)n, .pCode = code};
    if (vkCreateShaderModule(vk.dev, &ci, NULL, &m) != VK_SUCCESS) m = NULL;
    free(code);
    return m;
}

static PipeKey *pre_keys;               /* pipelines.list as loaded */
static u32 pre_n;
static volatile LONG pre_next, pre_built, pre_left;
static double pre_t0;

static unsigned __stdcall prebuild_thread(void *arg) {
    (void)arg;
    for (LONG i; (i = InterlockedIncrement(&pre_next) - 1) < (LONG)pre_n;) {
        const PipeKey *k = &pre_keys[i];
        VkShaderModule vs = spv_module(k->vs), fs = k->fs ? spv_module(k->fs) : NULL;
        bool claimed = false;
        Pipe *p = vs && (!k->fs || fs) ? pipe_claim(k, P_BUILDING, &claimed) : NULL;
        if (claimed) {
            p->pipe = pipeline_build(k, vs, fs);
            pipe_set_state(p, P_READY);
            InterlockedIncrement(&pre_built);
        }
        if (vs) vkDestroyShaderModule(vk.dev, vs, NULL);
        if (fs) vkDestroyShaderModule(vk.dev, fs, NULL);
    }
    if (!InterlockedDecrement(&pre_left))
        hlog(1, "gpu: prebuilt %ld of %u recorded pipelines in %.1f s (%ld from binaries, %ld compiled)", pre_built,
             pre_n, (ms_now() - pre_t0) / 1000, bins_made, bins_compiled);
    return 0;
}

void pipelines_prebuild(void) {
    char path[MAX_PATH];
    build_file(path, sizeof path, "pipelines.list");
    const u32 hdr[3] = {PIPE_LIST_MAGIC, PIPE_LIST_VERSION, sizeof(PipeKey)};
    u32 got[3] = {0};
    long size = 0;
    FILE *f = fopen(path, "rb");
    bool ok = f && fread(got, sizeof got, 1, f) == 1 && !memcmp(got, hdr, sizeof hdr);
    if (ok) {
        fseek(f, 0, SEEK_END);
        size = ftell(f) - (long)sizeof hdr;
        fseek(f, sizeof hdr, SEEK_SET);
        pre_n = (u32)(size / sizeof(PipeKey));
        pre_keys = malloc(pre_n * sizeof *pre_keys + 1);
        pre_n = (u32)fread(pre_keys, sizeof *pre_keys, pre_n, f);
    }
    if (f) fclose(f);
    if (ok && size == (long)(pre_n * sizeof(PipeKey))) pipe_list = fopen(path, "ab");
    else if ((pipe_list = fopen(path, "wb")) != NULL) {    /* new, an older layout, or a key cut short (killed) */
        fwrite(hdr, sizeof hdr, 1, pipe_list);
        fwrite(pre_keys, sizeof *pre_keys, pre_n, pipe_list);
        fflush(pipe_list);
    }
    if (!pipe_list) hlog(0, "gpu: could not open %s: pipelines are not recorded", path);
    for (u32 i = 0; i < pre_n; i++) {
        bool claimed;
        pipe_claim(&pre_keys[i], P_LISTED, &claimed);
    }
    if (!pre_n) { hlog(1, "gpu: no recorded pipelines (%s)", path); return; }
    DWORD cpus = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    u32 n = cpus > 3 ? cpus - 2 : 1;
    pre_left = (LONG)n;
    pre_t0 = ms_now();
    hlog(1, "gpu: prebuilding %u recorded pipelines on %u threads", pre_n, n);
    for (u32 i = 0; i < n; i++) {
        HANDLE t = (HANDLE)_beginthreadex(NULL, 0, prebuild_thread, NULL, 0, NULL);
        if (t) {
            SetThreadPriority(t, THREAD_PRIORITY_BELOW_NORMAL);
            CloseHandle(t);
        } else InterlockedDecrement(&pre_left);
    }
}
