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
#include "game_config.h"
#include <process.h>
#include <stdlib.h>
#include <windows.h>
#include <zstd.h>

static CRITICAL_SECTION gpu_cs;
static u64 frame_no;                    /* frames presented so far */
static bool gpu_ready;

/* NX_* switches, read once at start (env_init, from gpu_init): none is looked up while translating */
static struct {
    bool dump_astc, ssbo_log, pipe_log, no_discard, refresh_pools, shader_jit, pool_mirror, feedback_log, timeline,
        view_log, mutable_all, full_barriers, async_shaders, refresh_wait;
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
} env;

/* ---------------------------------------------------------------------------------------------- submissions */
#define SLOTS 12
#define STAGING_SIZE (32ull << 20)
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
static Slot slots[SLOTS];
static int cur = -1;                    /* the slot being recorded, or -1 */
static u64 slot_serial;                 /* bumped with each slot opened (staging from older ones is gone) */
static u64 submitted;                   /* last timeline value submitted */
static u32 work_unflushed;              /* draws and dispatches recorded since the last submission */
static u32 head_flush;                  /* the next early flush, in draws (NX_FLUSH_FIRST, doubling; 0: none) */
static VkSemaphore timeline;


/* things to destroy once the GPU is past `value` */
typedef struct Dead { u64 value; int kind; void *a, *b; VkAlloc alloc; } Dead;
enum { DEAD_HOSTBUF, DEAD_IMAGE, DEAD_VIEW, DEAD_MIRROR };
static void mirror_free(void *m);
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
        case DEAD_MIRROR: mirror_free(d->a); break;
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

/* NX_GPU_PROF=1: GPU time by pass. A timestamp goes in at each render pass, dispatch and submission edge, the
 * interval after it charged to its label; read back when the slot is reused, logged every 300 frames in ms per
 * frame, with the render thread's time waiting for slots and guest threads' waits for the GPU */
#define PROF_MAX 8192
static int prof_on;                     /* NX_GPU_PROF (env_init) */
static VkQueryPool prof_pool[SLOTS];
static u32 prof_n[SLOTS];
static u16 prof_label[SLOTS][PROF_MAX];
typedef struct { char name[80]; double ms; u64 count; } ProfName;
#define PROF_NAMES 4096
static ProfName prof_names[PROF_NAMES];
static u32 nprof_names;
static u16 prof_hash[2 * PROF_NAMES];   /* open addressing: name index + 1 */
static double prof_slot_wait_ms, prof_guest_wait_ms;
/* guest waits for the GPU by how many submissions behind the newest the awaited one was (0: the newest) */
static double prof_wait_ms[4];
static u64 prof_wait_n[4];
__declspec(thread) u64 gpu_wait_site;
static struct { u64 site; double ms; u64 n; } prof_sites[16];      /* guest waits by caller */
/* passes ended, by the gpu.c line that ended them */
static struct { u32 line; u64 count; } prof_ends[64];

static u16 prof_intern(const char *name) {
    u32 h = 2166136261u;
    for (const char *c = name; *c; c++) h = (h ^ (u8)*c) * 16777619u;
    for (u32 k = h % (2 * PROF_NAMES);; k = (k + 1) % (2 * PROF_NAMES)) {
        u16 e = prof_hash[k];
        if (!e) break;
        if (!strcmp(prof_names[e - 1].name, name)) return e - 1;
    }
    if (nprof_names == PROF_NAMES) return PROF_NAMES - 1;
    snprintf(prof_names[nprof_names].name, sizeof prof_names[0].name, "%s", name);
    for (u32 k = h % (2 * PROF_NAMES);; k = (k + 1) % (2 * PROF_NAMES))
        if (!prof_hash[k]) { prof_hash[k] = (u16)(nprof_names + 1); break; }
    return (u16)nprof_names++;
}

static void prof_mark_in(int slot, const char *label) {
    if (prof_n[slot] >= PROF_MAX) return;
    vkCmdWriteTimestamp2(slots[slot].cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_pool[slot], prof_n[slot]);
    prof_label[slot][prof_n[slot]++] = prof_intern(label);
}

static void prof_mark(const char *label) {
    if (prof_on <= 0) return;
    cmd();
    prof_mark_in(cur, label);
}

/* the slot's work is done: charge its intervals */
static void prof_collect(int slot) {
    u32 n = prof_n[slot];
    prof_n[slot] = 0;
    if (n < 2) return;
    static u64 t[PROF_MAX];
    if (vkGetQueryPoolResults(vk.dev, prof_pool[slot], 0, n, n * sizeof *t, t, sizeof *t, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    for (u32 i = 0; i + 1 < n; i++) {
        ProfName *p = &prof_names[prof_label[slot][i]];
        if (t[i + 1] >= t[i]) p->ms += (double)(t[i + 1] - t[i]) * vk.props.limits.timestampPeriod / 1e6;
        p->count++;
    }
}

static int prof_cmp(const void *a, const void *b) {
    const ProfName *x = a, *y = b;
    return x->ms < y->ms ? 1 : x->ms > y->ms ? -1 : 0;
}

static void prof_report(u64 frames) {
    double total = 0;
    for (u32 i = 0; i < nprof_names; i++) total += prof_names[i].ms;
    hlog(0, "gpu prof: %.2f ms of GPU work per frame; render thread waited %.2f ms for slots, guests %.2f ms for the GPU",
         total / frames, prof_slot_wait_ms / frames, prof_guest_wait_ms / frames);
    static ProfName sorted[PROF_NAMES];
    memcpy(sorted, prof_names, nprof_names * sizeof *sorted);
    qsort(sorted, nprof_names, sizeof *sorted, prof_cmp);
    for (u32 i = 0; i < nprof_names && i < (prof_on > 1 ? 50u : 25u) && sorted[i].ms > 0; i++)
        hlog(0, "gpu prof: %7.3f ms %6.1f/frame  %s", sorted[i].ms / frames, (double)sorted[i].count / frames,
             sorted[i].name);
    static const char *const behind[4] = {"the newest submission", "1 behind", "2-9 behind", "10+ behind"};
    for (u32 i = 0; i < 4; i++)
        if (prof_wait_n[i])
            hlog(0, "gpu prof: guest waits on %s: %.1f/frame, %.2f ms/frame", behind[i], (double)prof_wait_n[i] / frames,
                 prof_wait_ms[i] / frames);
    static u64 last_submitted;
    hlog(0, "gpu prof: %.1f submissions/frame", (double)(submitted - last_submitted) / frames);
    last_submitted = submitted;
    for (u32 i = 0; i < 16 && prof_sites[i].site; i++) {
        char d[160];
        hlog(0, "gpu prof:   %.2f ms/frame %.1f/frame %s from %s", prof_sites[i].ms / frames, (double)prof_sites[i].n / frames,
             prof_sites[i].site & 1 ? "nvnQueueFinish" : "nvnSyncWait", describe_addr(prof_sites[i].site & ~1ull, d, sizeof d));
    }
    memset(prof_sites, 0, sizeof prof_sites);
    memset(prof_wait_ms, 0, sizeof prof_wait_ms);
    memset(prof_wait_n, 0, sizeof prof_wait_n);
    for (u32 i = 0; i < 64 && prof_ends[i].line; i++)
        if (prof_ends[i].count) hlog(0, "gpu prof: %6.1f passes/frame ended at gpu.c:%u", (double)prof_ends[i].count / frames, prof_ends[i].line);
    for (u32 i = 0; i < 64; i++) prof_ends[i].count = 0;
    for (u32 i = 0; i < nprof_names; i++) prof_names[i].ms = 0, prof_names[i].count = 0;
    prof_slot_wait_ms = prof_guest_wait_ms = 0;
}

/* the render thread's time per frame in our code, for the report every 600 frames */
static double t_submit_ms, t_present_ms, t_wait_ms;
static const char *frames_dir(void);

/* NX_HITCH_MS=<ms>: each frame longer than that (present to present) is logged with where the render thread's time
 * went in it (gpu_present), to tell which work makes the frame-time spikes */
static struct {
    double submit, present, wait, shader, jit, pipe, image, upload, slot, mirror;
    u32 nshader, njit, npipe, nimage, nupload;
} hf;

static double ms_now(void) {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

/* NX_GPU_TIMELINE=1: when the GPU is idle and why. Each submission gets a GPU timestamp at its start and end, and the
 * CPU time of its vkQueueSubmit; they are read back when the slot is reused. The two clocks are lined up by the
 * smallest (GPU start - CPU submit) in the window (no submission starts before it is submitted). Every 600 frames:
 * GPU busy per frame (from a submission's start to its end, barrier drains inside included) and idle, split into
 * idle with nothing submitted yet, before a guest submission's first flush (the render thread was waiting for a
 * fence or running game code) or between flushes of one (translating slower than the GPU), and idle with work
 * already submitted (driver and queue latency, presents on the same queue) */
typedef struct { double submit, start, end; bool first; double woke, core, entry; } TlRec;  /* CPU ms; GPU ms on the GPU's clock */
static VkQueryPool tl_pool;
static double tl_submit[SLOTS];
static bool tl_first[SLOTS], tl_used[SLOTS], tl_guest_first;
static double tl_woke, tl_core, tl_entry, tl_rec_woke[SLOTS], tl_rec_core[SLOTS], tl_rec_entry[SLOTS];
static TlRec *tl_recs;
static int tl_n, tl_cap;

static void tl_collect(int slot) {
    if (!tl_used[slot]) return;
    tl_used[slot] = false;
    u64 t[2];
    if (vkGetQueryPoolResults(vk.dev, tl_pool, 2 * slot, 2, sizeof t, t, sizeof *t, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return;
    if (tl_n == tl_cap) tl_recs = realloc(tl_recs, (tl_cap = tl_cap * 2 + 4096) * sizeof *tl_recs);
    double per = vk.props.limits.timestampPeriod / 1e6;
    tl_recs[tl_n++] = (TlRec){tl_submit[slot], (double)t[0] * per, (double)t[1] * per, tl_first[slot], tl_rec_woke[slot],
                              tl_rec_core[slot], tl_rec_entry[slot]};
}

static void tl_report(u64 frames) {
    if (tl_n < 2) return;
    double off = 1e300;                 /* GPU clock - CPU clock */
    for (int i = 0; i < tl_n; i++) if (tl_recs[i].start - tl_recs[i].submit < off) off = tl_recs[i].start - tl_recs[i].submit;
    double busy = 0, late_head = 0, late_mid = 0, queued = 0, wall = 0, prev = tl_recs[0].start - off;
    double h_wake = 0, h_core = 0, h_code = 0, h_tr = 0;   /* a head gap's parts: GPU done -> waiter woken -> core back
                                                             * -> gpu_submit entered -> first flush submitted */
    u32 ngaps = 0;
    for (int i = 0; i < tl_n; i++) {
        const TlRec *r = &tl_recs[i];
        double gs = r->start - off, ge = r->end - off;
        if (gs > prev) {                /* the GPU was idle from prev to gs */
            double gap = gs - prev, late = r->submit > prev ? r->submit - prev : 0;
            if (late > gap) late = gap;
            if (r->first) late_head += late; else late_mid += late;
            if (r->first && late > 0) {
                double t = prev, w = r->woke > t ? r->woke : t, c = r->core > w ? r->core : w, e = r->entry > c ? r->entry : c;
                double sub = r->submit;
                if (w > sub) w = sub;
                if (c > sub) c = sub;
                if (e > sub) e = sub;
                h_wake += w - t, h_core += c - w, h_code += e - c, h_tr += sub - e;
            }
            queued += gap - late;
            if (gap > 0.05) ngaps++;
        }
        if (ge > prev) busy += ge - (gs > prev ? gs : prev), prev = ge;
    }
    wall = prev - (tl_recs[0].start - off);
    hlog(0, "gpu timeline: per frame %.2f ms: GPU busy %.2f, idle %.2f = nothing submitted yet (at a guest submission's "
            "start %.2f, between its flushes %.2f) + submitted, not started %.2f; %.1f gaps over 0.05 ms, %.1f submissions",
         wall / frames, busy / frames, (wall - busy) / frames, late_head / frames, late_mid / frames, queued / frames,
         (double)ngaps / frames, (double)tl_n / frames);
    hlog(0, "gpu timeline:   start gap: GPU done -> waiter woken %.2f, -> its core back %.2f, -> gpu_submit %.2f, "
            "-> first flush submitted %.2f", h_wake / frames, h_core / frames, h_code / frames, h_tr / frames);
    tl_n = 0;
}

void gpu_wait_core_back(void) { if (env.timeline) tl_core = ms_now(); }

static void slot_open(void) {
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

static void barrier_src(VkCommandBuffer c, VkAccessFlags2 src) {
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = src,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(c, &d);
}

static void barrier_in(VkCommandBuffer c) { barrier_src(c, VK_ACCESS_2_MEMORY_WRITE_BIT); }

/* the writes a pass without colour or depth writes can have made (transform feedback, shader stores): naming only
 * these spares the driver flushing the colour and depth caches (RGP: every barrier flushed CB / DB / L2 with a
 * full stall, ~230 a frame, many after transform-feedback passes that rasterize nothing) */
#define ACCESS_NO_ATTACHMENTS (VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFORM_FEEDBACK_WRITE_BIT_EXT | \
                               VK_ACCESS_2_TRANSFORM_FEEDBACK_COUNTER_WRITE_BIT_EXT)

/* NX_GPU_LABELS: debug-utils regions a capture shows as user events (RGP): each pass by its targets, each draw and
 * dispatch by its shaders, each barrier by the line that asked for it. Regions nest: a draw's closes before its
 * pass ends (label_draw_end, end_rendering) */
static void label_begin(VkCommandBuffer c, const char *fmt, ...) {
    char s[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s, sizeof s, fmt, ap);
    va_end(ap);
    VkDebugUtilsLabelEXT l = {VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT, .pLabelName = s};
    vkCmdBeginDebugUtilsLabelEXT(c, &l);
}

static bool label_draw_open;

static void label_draw_end(void) {
    if (!label_draw_open) return;
    vkCmdEndDebugUtilsLabelEXT(cmd());
    label_draw_open = false;
}

static void barrier_at(u32 line, VkAccessFlags2 src) {
    VkCommandBuffer c = cmd();
    if (vk.labels) label_begin(c, "barrier L%u%s", line, src == VK_ACCESS_2_MEMORY_WRITE_BIT ? "" : " (no attachment writes)");
    barrier_src(c, src);
    if (vk.labels) vkCmdEndDebugUtilsLabelEXT(c);
}
#define barrier() barrier_at(__LINE__, VK_ACCESS_2_MEMORY_WRITE_BIT)

/* the current pass has written colour or depth (a draw that rasterizes, a clear): end_rendering's barrier */
static bool pass_att_writes;

static void end_rendering_at(u32 line);
#define end_rendering() end_rendering_at(__LINE__)

static bool mirror_sync(VkCommandBuffer pre);
static void mirror_scan_start(void);
static void linear_writeback(void);
static int nwb;

static void flush(void) {
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
#define MIRROR_MIN (8ull << 20)         /* smaller pools are not worth a scan per submission */
static double mirror_mb, mirror_ms;     /* copied in and time scanning, for the report every 600 frames */
static int nmirrors;
typedef struct PoolEnt { u64 gpu, end, cpu; u32 flags; HostBuffer *hb; Mirror *mir; } PoolEnt;
static PoolEnt *pools;
static int npools, cappools;
static u64 pools_gen;                   /* bumped when pools come or go (cached device addresses) */

static PoolEnt *pool_find(u64 addr) {
    static int last = -1;               /* draws look up the same few pools over and over */
    static u64 last_gen;
    if (last >= 0 && last_gen == pools_gen && last < npools && addr >= pools[last].gpu && addr < pools[last].end)
        return &pools[last];
    int lo = 0, hi = npools - 1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (addr < pools[m].gpu) hi = m - 1;
        else if (addr >= pools[m].end) lo = m + 1;
        else {
            last = m;
            last_gen = pools_gen;
            return &pools[m];
        }
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

static Mirror *mirror_new(u64 size) {
    Mirror *m = calloc(1, sizeof *m);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    if (vkCreateBuffer(vk.dev, &bci, NULL, &m->b.buf) != VK_SUCCESS) { free(m); return NULL; }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(vk.dev, m->b.buf, &req);
    if (!vk_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &m->b.a)) {
        vkDestroyBuffer(vk.dev, m->b.buf, NULL);
        free(m);
        return NULL;
    }
    VKCHECK(vkBindBufferMemory(vk.dev, m->b.buf, m->b.a.mem, m->b.a.offset));
    m->b.size = size;
    m->fresh = true;                    /* copied whole at the next submission */
    return m;
}

static void mirror_free(void *p) {
    Mirror *m = p;
    vk_buffer_free(&m->b);
    free(m->bad);
    free(m);
}

/* the mirror's buffer and offset for a GPU address, if its pool has one */
static VkBuffer mirror_at(u64 addr, u64 *off) {
    PoolEnt *p = addr ? pool_find(addr) : NULL;
    if (!p || !p->mir) return NULL;
    *off = addr - p->gpu;
    return p->mir->b.buf;
}

/* a range the GPU writes through host memory (storage buffers, transform feedback): read from there from now on */
static void mirror_bad(u64 addr, u64 size) {
    PoolEnt *p = addr && size ? pool_find(addr) : NULL;
    if (!p || !p->mir) return;
    Mirror *m = p->mir;
    u64 lo = addr, hi = size < p->end - addr ? addr + size : p->end;
    for (int i = 0; i < m->nbad; i++)
        if (m->bad[i].lo <= lo && hi <= m->bad[i].hi) return;
    for (int i = 0; i < m->nbad;) {    /* merge what it touches */
        if (m->bad[i].lo <= hi && lo <= m->bad[i].hi) {
            if (m->bad[i].lo < lo) lo = m->bad[i].lo;
            if (m->bad[i].hi > hi) hi = m->bad[i].hi;
            m->bad[i] = m->bad[--m->nbad];
        } else i++;
    }
    if (m->nbad == m->capbad) m->bad = realloc(m->bad, (m->capbad = m->capbad * 2 + 16) * sizeof *m->bad);
    m->bad[m->nbad++] = (Range){lo, hi};
    hlog(2, "gpu: pool mirror: %llx+%llx is written by the GPU (%d ranges)", lo, hi - lo, m->nbad);
}

/* the storage buffers a stage's shader may write (it uses global memory) */
static void mirror_bad_ssbos(const BufferRange *ssbo, const ShaderCode *sc) {
    if (!nmirrors || !sc || !(sc->caps & SHC_GLOBAL)) return;
    for (u32 k = 0; k < 16; k++)
        if (ssbo[k].addr) mirror_bad(ssbo[k].addr, ssbo[k].size);
}

/* a range draws only read (vertex, index or uniform data; size 0: to the pool's end) as (buffer, offset): from the
 * pool's mirror when it has one and the GPU writes none of the range */
static bool gpu_buffer_ro(u64 addr, u64 size, VkBuffer *buf, u64 *off) {
    PoolEnt *p = pool_find(addr);
    if (!p || !p->hb) return false;
    *buf = p->hb->buf;
    *off = addr - p->gpu;
    Mirror *m = p->mir;
    if (m) {
        u64 hi = size && size < p->end - addr ? addr + size : p->end;
        bool bad = false;
        for (int i = 0; i < m->nbad && !bad; i++) bad = m->bad[i].lo < hi && addr < m->bad[i].hi;
        if (!bad) *buf = m->b.buf;
    }
    return true;
}

void gpu_pool_created(Pool *p) {
    if (!gpu_ready) return;
    /* the import first, outside gpu_cs: the driver pins the memory page by page (milliseconds for big pools), and
     * the render thread, which needs gpu_cs to submit or fence, held core 0 waiting for it, and main with it (the
     * periodic 20-100 ms frames while areas stream in). It touches nothing but this pool's own objects */
    HostBuffer *hb = NULL;
    if (p->storage && !(p->flags & (POOL_VIRTUAL | POOL_GPU_NO_ACCESS))) {
        hb = calloc(1, sizeof *hb);
        if (!vk_import_host(p->storage, p->size, hb)) {
            hlog(0, "gpu: cannot import pool memory %llx+%llx", p->storage, p->size);
            free(hb);
            hb = NULL;
        }
    }
    EnterCriticalSection(&gpu_cs);
    p->hb = hb;
    Mirror *mir = NULL;
    if (hb && env.pool_mirror && p->size >= MIRROR_MIN && p->flags & POOL_GPU_CACHED && !(p->flags & POOL_CPU_NO_ACCESS) &&
        !(p->storage & 0xfff) && !(p->size & 0xfff) && p->storage >= AS_BASE) {
        mir = mirror_new(p->size);
        if (mir) nmirrors++;
        hlog(1, "gpu: pool %llx+%llx (flags %x) %s", p->gpu, p->size, p->flags, mir ? "mirrored in video memory" : "not mirrored: no memory");
    }
    if (npools == cappools) pools = realloc(pools, (cappools = cappools * 2 + 64) * sizeof *pools);
    int i = npools;
    while (i > 0 && pools[i - 1].gpu > p->gpu) { pools[i] = pools[i - 1]; i--; }
    pools[i] = (PoolEnt){p->gpu, p->gpu + p->size, p->flags & POOL_VIRTUAL ? 0 : p->storage, p->flags, hb, mir};
    npools++;
    pools_gen++;
    images_drop_range(p->gpu, p->gpu + p->size);
    LeaveCriticalSection(&gpu_cs);
}

void gpu_pool_destroyed(Pool *p) {
    if (!gpu_ready) return;
    EnterCriticalSection(&gpu_cs);
    for (int i = 0; i < npools; i++)
        if (pools[i].gpu == p->gpu && pools[i].end == p->gpu + p->size) {
            if (pools[i].hb) bury(DEAD_HOSTBUF, pools[i].hb, NULL);
            if (pools[i].mir) bury(DEAD_MIRROR, pools[i].mir, NULL), nmirrors--;
            memmove(&pools[i], &pools[i + 1], (npools - i - 1) * sizeof *pools);
            npools--;
            pools_gen++;
            break;
        }
    images_drop_range(p->gpu, p->gpu + p->size);
    p->hb = NULL;
    LeaveCriticalSection(&gpu_cs);
}

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
} Image;
static void image_touch(Image *im);

/* Pitch-linear textures are memory the CPU may read as it is (on the console, one memory): what the GPU writes
 * into one is copied back to guest memory at the end of the submission (flush), before its syncs signal. Violet's
 * screen capture (profile photo, player icon) renders into a linear RGBA8 texture and BC1-encodes it on the CPU;
 * without this it read zeros and saved a black picture. */
#define WB_MAX 64
static Image *wb_list[WB_MAX];
static void linear_wrote(Image *im) {
    if (!im || im->wb_pending || !(im->info.flags & TEX_FLAG_LINEAR)) return;
    if (nwb == WB_MAX) { hlog(0, "gpu: more than %d linear textures written in one submission", WB_MAX); return; }
    im->wb_pending = true;
    wb_list[nwb++] = im;
}
#define IMAGE_BUCKETS 4096
static Image *buckets[IMAGE_BUCKETS];
static int nimages;
static u64 image_bytes;                 /* device memory the images hold */

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

static bool format_usable(VkFormat f) {
    VkFormatFeatureFlags ft = format_features(f);
    return ft & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT || ft & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
}

/* depth / stencil formats a device may lack, with stand-ins in order of preference: the GTX 1650 has no X8_D24 or
   S8 images; AMD has no D24 at all (a D24S8 image there is invalid, and using it loses the device) */
static VkFormat usable_format(u32 nvn) {
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

/* Integer textures (UINT/SINT formats, and depth-stencil textures in stencil mode) are read by shaders through
 * integer views: the texture arrays TB_U2D / TB_U2D_ARRAY hold them, and the handle written to c[2] gets bit 19
 * (texture ids stay below 65536), which translate.py tests at every fetch and gather (a float read of an integer
 * view is undefined: AMD returned garbage for the stencil a culling pass reads, hiding objects near the camera) */
#define HANDLE_INT (1u << 19)
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
static void images_evict(u64 target, bool wait);

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
        if (sub_of(I, t, layer, level)) { image_touch(I); return I; }
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
    if (old) old->used = frame_no + 1;
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
    VkImageFormatListCreateInfo fl = {VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
        .viewFormatCount = im->nvfmts, .pViewFormats = im->vfmts};
    if (type == VK_IMAGE_TYPE_2D && im->layers >= 6 && t->width == t->height && t->samples <= 1)
        flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (type == VK_IMAGE_TYPE_3D) flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    u32 samples = t->samples > 1 ? t->samples : 1;
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = flags, .imageType = type, .format = fmt,
        .extent = {t->width, tex_height(t), tex_depth(t)}, .mipLevels = im->levels, .arrayLayers = im->layers,
        .samples = (VkSampleCountFlagBits)samples, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (!env.mutable_all && flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) ci.pNext = &fl;
    VkResult r = vkCreateImage(vk.dev, &ci, NULL, &im->image);
    if (r != VK_SUCCESS) {
        hlog(0, "gpu: vkCreateImage %ux%ux%u format %u (vk %d) target %u: %d", t->width, t->height, t->depth,
             t->format, fmt, t->target, r);
        free(im);
        return NULL;
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
    im->next = buckets[h];
    buckets[h] = im;
    byaddr_add(im);
    if (++nimages % 500 == 0) hlog(1, "gpu: %d images", nimages);
    hlog(2, "gpu: image %ux%ux%u fmt %u target %u levels %u at %llx", t->width, t->height, t->depth, t->format,
         t->target, t->levels, t->addr);
    return im;
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
static u32 view_layers(const Image *im, const ViewInfo *v, u32 kind, u32 *base) {
    bool slices = kind == VIEW_ATTACHMENT && im->info.target == TEX_3D;
    u32 n = slices ? tex_depth(&im->info) >> v->base_level : im->layers;
    if (!n) n = 1;
    *base = v->min_layer < n ? v->min_layer : 0;
    return v->num_layers && *base + v->num_layers <= n && !slices ? v->num_layers : n - *base;
}

static VkImageView image_view(Image *im, const ViewInfo *v, u32 kind) {
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

static void image_upload_do(Image *im) {
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

static void image_upload(Image *im) {
    if (im->valid) return;
    double t0 = ms_now();
    image_upload_do(im);
    hf.upload += ms_now() - t0;
    hf.nupload++;
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
    BufferRange xfb[4];                 /* transform feedback buffers, the bytes captured into each since bound */
    u64 xfb_written[4];
    bool xfb_active;                    /* between Begin (or Resume) and End (or Pause) */
    bool xfb_captured;                  /* a draw captured since the last barrier */
    u64 xfb_dirty[4][2];                /* the bytes captured since then: [lo, hi) of each buffer's addresses */
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

/* small GPU writes made while rendering (counter reports between occlusion boxes, events), done when the pass
 * ends: Vulkan allows no buffer updates inside rendering, and ending the pass for each one split the occlusion
 * pass into hundreds */
typedef struct PendingWrite { VkBuffer buf, mbuf; u64 off, moff; u32 size; u8 data[16]; } PendingWrite;
static PendingWrite pending[1024];
static u32 npending;

static void end_rendering_at(u32 line) {
    if (!st.rendering) return;
    if (prof_on > 0)
        for (u32 i = 0; i < 64; i++)
            if (prof_ends[i].line == line || !prof_ends[i].line) { prof_ends[i].line = line; prof_ends[i].count++; break; }
    label_draw_end();
    vkCmdEndRendering(cmd());
    if (vk.labels) {                    /* the pass's region, then who ended it */
        vkCmdEndDebugUtilsLabelEXT(cmd());
        label_begin(cmd(), "end pass L%u", line);
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
    barrier_at(__LINE__, full ? VK_ACCESS_2_MEMORY_WRITE_BIT : ACCESS_NO_ATTACHMENTS);
    st.xfb_captured = false;
    memset(st.xfb_dirty, 0, sizeof st.xfb_dirty);
}

/* NX_WATCH=<hex address>: log every GPU write to that texture storage */
static void watch(const char *what, const TexRef *r) {
    if (!env.watch || r->t.addr != env.watch) return;
    hlog(0, "watch: frame %llu %s %ux%ux%u f%u target %u view level %u layers %u+%u", frame_no + 1, what, r->t.width,
         r->t.height, r->t.depth, r->v.format, r->t.target, r->v.base_level, r->v.min_layer, r->v.num_layers);
}

/* a render target's view, its image, level, first layer and layer count (layered rendering: gl_Layer picks among
 * them) */
static VkImageView rt_view(const TexRef *r, Image **out, u32 *level, u32 *first, u32 *layers) {
    watch("render target", r);
    ViewInfo v;
    Image *im = image_ref(r, &v);
    if (out) *out = im;
    if (level) *level = v.base_level;
    if (!im) return NULL;
    u32 base, n = view_layers(im, &v, VIEW_ATTACHMENT, &base);
    if (layers) *layers = n;
    if (first) *first = base;
    return image_view(im, &v, VIEW_ATTACHMENT);
}

static bool begin_rendering(void) {
    if (st.rendering) return true;
    VkRenderingAttachmentInfo color[8], depth = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    u32 w = ~0u, h = ~0u, layers = ~0u;
    memset(st.rt_format, 0, sizeof st.rt_format);
    memset(st.rt_sub, 0, sizeof st.rt_sub);
    st.depth_format = st.stencil_format = VK_FORMAT_UNDEFINED;
    st.samples = 1;
    bool any = false;
    for (u32 i = 0; i < st.nrt; i++) {
        color[i] = (VkRenderingAttachmentInfo){VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        Image *im;
        u32 l, l0, nl;
        VkImageView v = st.rt[i].t.addr ? rt_view(&st.rt[i], &im, &l, &l0, &nl) : NULL;
        if (!v) continue;
        st.rt_sub[i].addr = im->info.addr, st.rt_sub[i].size = im->size;
        st.rt_sub[i].level = l, st.rt_sub[i].layer = l0, st.rt_sub[i].nlayers = nl;
        color[i].imageView = v;
        st.rt_format[i] = usable_format(st.rt[i].v.format) != VK_FORMAT_UNDEFINED ? usable_format(st.rt[i].v.format) : im->format;
        st.samples = im->info.samples > 1 ? im->info.samples : 1;
        color[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        color[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        u32 iw = im->info.width >> l, ih = tex_height(&im->info) >> l;
        if (iw < w) w = iw ? iw : 1;
        if (ih < h) h = ih ? ih : 1;
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
        VkImageView v = rt_view(&st.depth, &im, &l, &l0, &nl);
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
            u32 iw = im->info.width >> l, ih = tex_height(&im->info) >> l;
            if (iw < w) w = iw ? iw : 1;
            if (ih < h) h = ih ? ih : 1;
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

static bool trace_frame(void);

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
    if (logged < 12 || trace_frame()) {        /* every clear of a traced frame (F12, NX_TRACE) */
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
    pass_att_writes = true;
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
    pass_att_writes = true;
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
    linear_wrote(im);
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
    linear_wrote(im);
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
    u64 moff;
    VkBuffer mbuf = mirror_at(p->buffer, &moff);
    if (mbuf) {
        r.bufferOffset = moff;
        vkCmdCopyImageToBuffer(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, mbuf, 1, &r);
    }
    barrier();
}

/* copy the linear textures the GPU wrote in this submission back into guest memory (see linear_wrote) */
static void linear_writeback(void) {
    end_rendering();
    barrier();
    for (int i = 0; i < nwb; i++) {
        Image *im = wb_list[i];
        im->wb_pending = false;
        TexLayout L;
        tex_layout(&im->info, &L);
        VkBuffer buf, mbuf;
        u64 off, moff;
        static int skipped, logged;
        if (!L.bpb || format_is_astc(im->info.format) || vk_format(im->info.format) != im->format ||
            im->info.samples > 1 ||
            !gpu_cpu(im->info.addr, L.layer_size * L.layers) || !gpu_buffer(im->info.addr, &buf, &off)) {
            if (skipped++ < 8)
                hlog(0, "gpu: linear %ux%u f%u at %llx written by the GPU: not copied back (format or memory)",
                     im->info.width, im->info.height, im->info.format, im->info.addr);
            continue;
        }
        mbuf = mirror_at(im->info.addr, &moff);
        if (logged++ < 16)
            hlog(1, "gpu: frame %llu: linear %ux%u f%u at %llx (row %llu) copied back to guest memory", frame_no + 1,
                 im->info.width, im->info.height, im->info.format, im->info.addr, L.level[0].row_pitch);
        u32 levels = im->levels < L.levels ? im->levels : L.levels;
        for (u32 layer = 0; layer < L.layers && layer < im->layers; layer++)
            for (u32 l = 0; l < levels; l++) {
                const TexLevel *lv = &L.level[l];
                if (lv->row_pitch % L.bpb) continue;
                u32 w = im->info.width >> l, h = tex_height(&im->info) >> l, d = tex_depth(&im->info) >> l;
                u64 at = (u64)layer * L.layer_size + lv->offset;
                VkBufferImageCopy r = {off + at, (u32)(lv->row_pitch / L.bpb * L.bw), lv->h * L.bh,
                                       {copy_aspect(im), l, layer, 1}, {0, 0, 0}, {w ? w : 1, h ? h : 1, d ? d : 1}};
                vkCmdCopyImageToBuffer(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &r);
                if (mbuf) {
                    r.bufferOffset = moff + at;
                    vkCmdCopyImageToBuffer(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, mbuf, 1, &r);
                }
            }
    }
    nwb = 0;
    barrier();
}

/* can s be blitted into d? Not every format can (on AMD, no depth/stencil format is a blit destination), and a
 * blit the device does not support may lose it: such copies are skipped and reported once per format pair */
static void rec_forget(void);
static bool blit_ok(const Image *s, const Image *d) {
    if (format_features(s->format) & VK_FORMAT_FEATURE_BLIT_SRC_BIT &&
        format_features(d->format) & VK_FORMAT_FEATURE_BLIT_DST_BIT)
        return true;
    static u32 seen[16];
    static int ns;
    u32 key = (u32)s->format << 16 | (u32)d->format;
    for (int i = 0; i < ns; i++) if (seen[i] == key) return false;
    if (ns < 16) seen[ns++] = key;
    hlog(0, "gpu: blit from format %d to %d is not supported here: %s", s->format, d->format,
         d->aspects & VK_IMAGE_ASPECT_DEPTH_BIT ? "drawn instead (depth_blit)" : "skipped");
    return false;
}

/* Depth blits the device cannot do (AMD: no depth format is a blit destination) are drawn instead: a full-screen
 * triangle that samples the source's depth (nearest) and writes gl_FragDepth (shaders/depth_blit.*). Violet
 * scales its 1080p depth to 960x540 this way for the soft particles' depth fade; skipped, that texture stayed
 * empty and every effects-layer particle was discarded (item ball light pillars, glows). */
#include "shaders/depth_blit_spv.h"
static VkPipelineLayout dblit_layout;
static VkDescriptorSetLayout dblit_set;
static VkSampler dblit_sampler;
static struct { VkFormat fmt; VkPipeline pipe; } dblit_pipes[8];

static VkPipeline dblit_pipeline(VkFormat fmt) {
    for (int i = 0; i < 8 && dblit_pipes[i].pipe; i++) if (dblit_pipes[i].fmt == fmt) return dblit_pipes[i].pipe;
    if (!dblit_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo sci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 1, .pBindings = &b};
        VKCHECK(vkCreateDescriptorSetLayout(vk.dev, &sci, NULL, &dblit_set));
        VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
        VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                          .pSetLayouts = &dblit_set, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
        VKCHECK(vkCreatePipelineLayout(vk.dev, &lci, NULL, &dblit_layout));
        VkSamplerCreateInfo smci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
        VKCHECK(vkCreateSampler(vk.dev, &smci, NULL, &dblit_sampler));
    }
    int slot = 0;
    while (slot < 7 && dblit_pipes[slot].pipe) slot++;
    VkShaderModule m[2];
    VkShaderModuleCreateInfo mci[2] = {
        {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof depth_blit_vs, .pCode = depth_blit_vs},
        {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof depth_blit_fs, .pCode = depth_blit_fs}};
    VKCHECK(vkCreateShaderModule(vk.dev, &mci[0], NULL, &m[0]));
    VKCHECK(vkCreateShaderModule(vk.dev, &mci[1], NULL, &m[1]));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = m[0], .pName = "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = m[1], .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_ALWAYS};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
                                           .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .depthAttachmentFormat = fmt};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, .stageCount = 2,
        .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
        .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = dblit_layout};
    VkPipeline pipe = NULL;
    VkResult r = vkCreateGraphicsPipelines(vk.dev, NULL, 1, &ci, NULL, &pipe);
    vkDestroyShaderModule(vk.dev, m[0], NULL);
    vkDestroyShaderModule(vk.dev, m[1], NULL);
    if (r != VK_SUCCESS) { hlog(0, "gpu: depth blit pipeline for vk format %d failed (%d)", fmt, r); return NULL; }
    if (dblit_pipes[slot].pipe) vkDestroyPipeline(vk.dev, dblit_pipes[slot].pipe, NULL);   /* full: reuse the last */
    dblit_pipes[slot].fmt = fmt;
    dblit_pipes[slot].pipe = pipe;
    hlog(0, "gpu: depth blits into vk format %d are drawn (no blit support)", fmt);
    return pipe;
}

/* source level / layer / rectangle -> destination level / layer / rectangle, depth only; false if it cannot */
static bool depth_blit(Image *s, const ViewInfo *sv0, u32 slevel, u32 slayer, const s32 sr[4], Image *d,
                       const ViewInfo *dv0, u32 dlevel, u32 dlayer, const s32 dr[4]) {
    if (!(s->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) || !(d->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ||
        !(s->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !(d->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ||
        s->info.samples > 1 || d->info.samples > 1)
        return false;
    VkPipeline pipe = dblit_pipeline(d->format);
    if (!pipe) return false;
    ViewInfo sv = *sv0, dv = *dv0;
    sv.target = TEX_2D;
    sv.base_level = (u16)slevel;
    sv.num_levels = 1;
    sv.min_layer = (u16)slayer;
    sv.num_layers = 1;
    memset(sv.swizzle, 0xff, sizeof sv.swizzle);    /* identity */
    sv.ds_mode = 0;                                 /* the depth aspect */
    dv.target = TEX_2D;
    dv.base_level = (u16)dlevel;
    dv.num_levels = 1;
    dv.min_layer = (u16)dlayer;
    dv.num_layers = 1;
    VkImageView src = image_view(s, &sv, VIEW_SAMPLED), dst = image_view(d, &dv, VIEW_ATTACHMENT);
    if (!src || !dst) return false;
    float sw = (float)(s->info.width >> slevel), sh = (float)(tex_height(&s->info) >> slevel);
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    float uv[4] = {sr[0] / sw, sr[1] / sh, (sr[0] + sr[2]) / sw, (sr[1] + sr[3]) / sh};
    VkCommandBuffer c = cmd();
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dst,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRect2D area = {{dr[0], dr[1]}, {(u32)dr[2], (u32)dr[3]}};
    VkRenderingInfo rinfo = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = area, .layerCount = 1, .pDepthAttachment = &da};
    vkCmdBeginRendering(c, &rinfo);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vpt = {(float)dr[0], (float)dr[1], (float)dr[2], (float)dr[3], 0, 1};
    vkCmdSetViewportWithCount(c, 1, &vpt);
    vkCmdSetScissorWithCount(c, 1, &area);
    VkDescriptorImageInfo ii = {dblit_sampler, src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkCmdPushDescriptorSetKHR(c, VK_PIPELINE_BIND_POINT_GRAPHICS, dblit_layout, 0, 1, &w);
    vkCmdPushConstants(c, dblit_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, uv);
    vkCmdDraw(c, 3, 1, 0, 0);
    vkCmdEndRendering(c);
    rec_forget();                       /* the game's draws must bind and set everything again */
    return true;
}

static void do_copy_texture_to_texture(const CmdCopyTT *p) {
    watch("CopyTextureToTexture (dst)", &p->dst);
    CmdCopyTT q = *p;
    Image *s = image_ref(&p->src, &q.src.v), *d = image_ref(&p->dst, &q.dst.v);
    p = &q;
    if (!s || !d || p->sr.w <= 0 || p->dr.w <= 0) return;
    image_upload(s);
    end_rendering();
    if (trace_frame())
        hlog(0, "gpu: copy %ux%u f%u @%llx (%d,%d %dx%d z%d) -> %ux%u f%u @%llx (%d,%d %dx%d z%d) flags %x", p->src.t.width,
             p->src.t.height, p->src.t.format, p->src.t.addr, p->sr.x, p->sr.y, p->sr.w, p->sr.h, p->sr.z, p->dst.t.width,
             p->dst.t.height, p->dst.t.format, p->dst.t.addr, p->dr.x, p->dr.y, p->dr.w, p->dr.h, p->dr.z, p->flags);
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
        if (!blit_ok(s, d)) {
            s32 sr[4] = {p->sr.x, p->sr.y, p->sr.w, sh}, dr[4] = {p->dr.x, p->dr.y, p->dr.w, dh};
            if (s3 || d3 || !depth_blit(s, &p->src.v, sl.mipLevel, sl.baseArrayLayer, sr, d, &p->dst.v, dl.mipLevel,
                                        dl.baseArrayLayer, dr))
                return;
            d->valid = true;
            d->gpu_written = true;
            linear_wrote(d);
            barrier();
            return;
        }
        vkCmdBlitImage(cmd(), s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       p->flags & 1 && !depth ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    d->valid = true;
    d->gpu_written = true;
    linear_wrote(d);
    barrier();
}

static void do_downsample(const CmdDownsample *p) {
    watch("Downsample (dst)", &p->dst);
    CmdDownsample q = *p;
    Image *s = image_ref(&p->src, &q.src.v), *d = image_ref(&p->dst, &q.dst.v);
    p = &q;
    if (!s || !d) return;
    end_rendering();
    if (trace_frame())
        hlog(0, "gpu: downsample %ux%u f%u @%llx -> %ux%u f%u @%llx", p->src.t.width, p->src.t.height, p->src.t.format,
             p->src.t.addr, p->dst.t.width, p->dst.t.height, p->dst.t.format, p->dst.t.addr);
    u32 w = p->dst.t.width, h = tex_height(&p->dst.t);
    VkImageSubresourceLayers sl = {copy_aspect(s), p->src.v.base_level, p->src.v.min_layer, 1};
    VkImageSubresourceLayers dl = {copy_aspect(d), p->dst.v.base_level, p->dst.v.min_layer, 1};
    if (s->info.samples > 1 && d->info.samples <= 1 && s->aspects == VK_IMAGE_ASPECT_COLOR_BIT) {
        VkImageResolve r = {sl, {0, 0, 0}, dl, {0, 0, 0}, {w, h, 1}};
        vkCmdResolveImage(cmd(), s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else {
        if (!blit_ok(s, d)) {
            s32 sr[4] = {0, 0, (s32)p->src.t.width, (s32)tex_height(&p->src.t)}, dr[4] = {0, 0, (s32)w, (s32)h};
            if (!depth_blit(s, &p->src.v, sl.mipLevel, sl.baseArrayLayer, sr, d, &p->dst.v, dl.mipLevel,
                            dl.baseArrayLayer, dr))
                return;
            d->valid = true;
            d->gpu_written = true;
            linear_wrote(d);
            barrier();
            return;
        }
        VkImageBlit b = {sl, {{0, 0, 0}, {(s32)p->src.t.width, (s32)tex_height(&p->src.t), 1}},
                         dl, {{0, 0, 0}, {(s32)w, (s32)h, 1}}};
        vkCmdBlitImage(cmd(), s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       VK_FILTER_LINEAR);
    }
    d->valid = true;
    d->gpu_written = true;
    linear_wrote(d);
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
    u64 moff = 0;
    VkBuffer mbuf = mirror_at(addr, &moff);
    if (st.rendering && size <= 16 && npending < sizeof pending / sizeof *pending) {
        PendingWrite *w = &pending[npending++];
        w->buf = buf;
        w->off = off;
        w->mbuf = mbuf;
        w->moff = moff;
        w->size = (size + 3) & ~3u;
        memcpy(w->data, data, size);
        return;
    }
    end_rendering();
    vkCmdUpdateBuffer(cmd(), buf, off, (size + 3) & ~3u, data);
    if (mbuf) vkCmdUpdateBuffer(cmd(), mbuf, moff, (size + 3) & ~3u, data);
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
enum { TB_2D, TB_3D, TB_CUBE, TB_2D_ARRAY, TB_CUBE_ARRAY, TB_1D, TB_1D_ARRAY, TB_U2D, TB_U2D_ARRAY, TB_COUNT };
static const u32 tb_target[TB_COUNT] = {TEX_2D, TEX_3D, TEX_CUBE, TEX_2D_ARRAY, TEX_CUBE_ARRAY, TEX_1D, TEX_1D_ARRAY,
                                        TEX_2D, TEX_2D_ARRAY};
#define TB_IS_INT(b) ((b) >= TB_U2D)
static VkDescriptorSetLayout set_ubo, set_tex, set_smp, set_img;
static VkPipelineLayout layout_gfx;
static VkDescriptorPool desc_pool;
static VkDescriptorSet empty_tex, empty_smp, empty_img;
static VkPipelineCache pcache;
static u64 submit_serial;
static u32 window_origin, depth_mode;   /* NVN device modes (0: lower left, 0: near at -w) */

void gpu_device_modes(u32 origin, u32 depth) { window_origin = origin; depth_mode = depth; }

/* The pipeline cache is kept between sessions in pipelines.bin next to the shaders (build/), so pipelines built
 * once are not compiled again: pipeline builds were most of the stalls our side of a hitch (NX_HITCH_MS). A file
 * from another device or driver (header: vendor, device, cache UUID) is ignored. Saved by a thread every minute
 * after new builds and at exit (gpu_save_pipelines), written to a temporary file first so a killed process does
 * not leave half a file */
static char pcache_path[MAX_PATH];
static volatile long pcache_builds;     /* pipelines built, against the count at the last save */
static long pcache_saved;
static SRWLOCK pcache_lock = SRWLOCK_INIT;

/* a file in the build folder (the executable's parent folder) */
static void build_file(char *path, size_t size, const char *name) {
    snprintf(path, size, "%s\\%s", nx_cache_dir(), name);
}

/* F12 captures and other image dumps: build\frames, or in a package %LOCALAPPDATA%\<title>\dumps (made on use) */
static const char *frames_dir(void) {
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
static void translator_path(char *out, size_t n) {
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

static void pcache_load(void) {
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

static unsigned __stdcall pcache_thread(void *arg) {
    (void)arg;
    for (;;) {
        Sleep(60000);
        gpu_save_pipelines();
    }
}

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
    VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
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

static VkDescriptorSet alloc_set(VkDescriptorSetLayout l, u32 count) {
    VkDescriptorSetVariableDescriptorCountAllocateInfo vi = {
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO, .descriptorSetCount = 1,
        .pDescriptorCounts = &count};
    VkDescriptorSetAllocateInfo ai = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, &vi, desc_pool, 1, &l};
    VkDescriptorSet s;
    if (vkAllocateDescriptorSets(vk.dev, &ai, &s) != VK_SUCCESS) fatal("gpu: out of descriptor sets");
    return s;
}

/* a texture set (TB_COUNT arrays of TEXPOOL_MAX) in a pool of its own, sized for it */
static VkDescriptorSet alloc_tex_set(void) {
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
static DescSet *descsets;
static VkImageView dummy_views[TB_COUNT], dummy_storage;
static VkSampler dummy_sampler;
#define DUMMY_ADDRS 5                   /* images at guest addresses 1-5 are ours: never evicted */

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
static void dummies_init(void) {
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

static void descset_fill(VkDescriptorSet set, bool smp, u32 count) {     /* (storage sets stay unwritten) */
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

static DescSet *descset_get(const CmdPool *p, u32 kind) {
    if (!p->addr) return NULL;
    for (DescSet *d = descsets; d; d = d->next)
        if (d->addr == p->addr && d->kind == kind) return d;
    bool smp = kind == DS_SMP;
    DescSet *d = calloc(1, sizeof *d);
    d->addr = p->addr;
    d->kind = kind;
    d->count = p->count > (smp ? SMPPOOL_MAX : TEXPOOL_MAX) ? (smp ? SMPPOOL_MAX : TEXPOOL_MAX) : p->count;
    if (!d->count) d->count = 1;
    d->set = kind == DS_TEX ? alloc_tex_set() : alloc_set(smp ? set_smp : set_img, smp ? SMPPOOL_MAX : TEXPOOL_MAX);
    if (kind != DS_IMG) descset_fill(d->set, smp, smp ? SMPPOOL_MAX : TEXPOOL_MAX);
    d->idx = malloc(d->count * sizeof *d->idx);
    memset(d->idx, 0xff, d->count * sizeof *d->idx);
    d->res = calloc(d->count, 1);
    d->img = calloc(d->count, sizeof *d->img);
    d->intv = calloc(d->count, 1);
    d->epoch = ~0ull;
    d->next = descsets;
    descsets = d;
    hlog(1, "gpu: %s pool at %llx: %u entries", smp ? "sampler" : kind == DS_IMG ? "image" : "texture", p->addr, d->count);
    return d;
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
static void images_evict(u64 target, bool wait) {
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
        im = image_ref(&tr, &v);
    }
    if (im) image_upload(im);
    d->res[i] = 1;
    d->gen++;
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

static void descset_use(DescSet *d, const u64 *tex, u32 n) {
    if (!d) return;
    /* draws mostly use the textures the last one did: in the same frame, with the same entries and images, the
     * images are already touched (n <= 32; one record per handle array: each stage's textures and images) */
    static struct { const u64 *tex; const DescSet *d; u64 gen, frame, epoch; u64 h[32]; } seen[8];
    int e = 0;
    while (e < 8 && seen[e].tex && seen[e].tex != tex) e++;
    if (e == 8) e = (int)(((uintptr_t)tex >> 4) & 7);
    if (seen[e].tex == tex && seen[e].d == d && seen[e].gen == d->gen && seen[e].frame == frame_no &&
        seen[e].epoch == image_epoch && n <= 32 && !memcmp(seen[e].h, tex, n * sizeof *tex))
        return;
    for (int pass = 0; pass < 4; pass++) {
        if (d->epoch != image_epoch) {          /* images were destroyed: the pointers may be stale */
            memset(d->res, 0, d->count);
            d->epoch = image_epoch;
            d->gen++;
        }
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
        memcpy(seen[e].h, tex, n * sizeof *tex);
    }
}

/* once per submission: which entries the guest changed */
static void descset_sync(DescSet *d) {
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
        if (d->kind != DS_SMP) { d->res[i] = 0; d->gen++; continue; }
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

/* ---- shaders */
static char spv_dir[MAX_PATH];
static bool have_translator;            /* translate.py is there (the JIT can run) */

/* The shader pack (tools/shader/pack.c): every shader translated at build time, one zstd frame each, so a package
 * needs no translator. shaders.pak next to the executable, else in the cache's shaders\ (NX_SHADER_PACK names
 * another). Used only when it was made by the translator version in use (a package without translate.py takes the
 * pack's); loose files in spv\<version> (the JIT's) come first */
typedef struct PakEnt { u64 hash; u32 mask, size; u64 offset; u32 packed, pad; } PakEnt;
static struct { HANDLE f; PakEnt *ents; u32 n, translator; bool usable; } pak;

static void pak_open(void) {
    char path[MAX_PATH + 32];
    const char *e = getenv("NX_SHADER_PACK");
    if (e) snprintf(path, sizeof path, "%s", e);
    else {
        GetModuleFileNameA(NULL, path, MAX_PATH);
        char *s = strrchr(path, '\\');
        if (s) s[1] = 0;
        strcat(path, "shaders.pak");
        if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES)
            snprintf(path, sizeof path, "%s\\shaders\\shaders.pak", nx_cache_dir());
    }
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    u32 hdr[4];
    DWORD got;
    PakEnt *ents = NULL;
    if (ReadFile(f, hdr, sizeof hdr, &got, NULL) && got == sizeof hdr && hdr[0] == 0x5053584e && hdr[1] == 1 &&
        hdr[3] < (1u << 24) && (ents = malloc((size_t)hdr[3] * sizeof *ents + 1)) != NULL &&
        ReadFile(f, ents, hdr[3] * (DWORD)sizeof *ents, &got, NULL) && got == hdr[3] * sizeof *ents) {
        pak.f = f;
        pak.ents = ents;
        pak.n = hdr[3];
        pak.translator = hdr[2];
        hlog(1, "gpu: shader pack %s: %u shaders, translator %08x", path, pak.n, pak.translator);
        return;
    }
    hlog(0, "gpu: %s is not a shader pack", path);
    free(ents);
    CloseHandle(f);
}

static const PakEnt *pak_find(u64 hash, u32 mask) {
    if (!pak.usable) return NULL;
    u32 lo = 0, hi = pak.n;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        const PakEnt *p = &pak.ents[mid];
        if (p->hash < hash || (p->hash == hash && p->mask < mask)) lo = mid + 1;
        else hi = mid;
    }
    return lo < pak.n && pak.ents[lo].hash == hash && pak.ents[lo].mask == mask ? &pak.ents[lo] : NULL;
}

static void spv_path(char *path, size_t size, u64 hash) {
    if (!spv_dir[0]) {
        snprintf(spv_dir, sizeof spv_dir, "%s\\shaders", nx_cache_dir());
        CreateDirectoryA(spv_dir, NULL);
        strcat(spv_dir, "\\spv");
        CreateDirectoryA(spv_dir, NULL);
        pak_open();
        /* translations live in a folder per translator version (FNV-1a of translate.py, as translate.py names it):
         * after a translator change every shader is translated again, nothing stale is used */
        char tp[MAX_PATH];
        translator_path(tp, sizeof tp);
        FILE *f = fopen(tp, "rb");
        u32 h = 0;
        if (f) {
            h = 2166136261u;
            int ch;
            while ((ch = fgetc(f)) != EOF) h = (h ^ (u8)ch) * 16777619u;
            fclose(f);
            have_translator = true;
        } else if (pak.f) h = pak.translator;
        if (pak.f) {
            pak.usable = pak.translator == h;
            if (!pak.usable) hlog(0, "gpu: the shader pack is from translator %08x, not %08x: not used", pak.translator, h);
        }
        if (f || pak.f) {
            size_t n = strlen(spv_dir);
            snprintf(spv_dir + n, sizeof spv_dir - n, "\\%08x", h);
            CreateDirectoryA(spv_dir, NULL);
            hlog(1, "gpu: shader translations in %s%s", spv_dir, have_translator ? "" : " (no translator: the pack only)");
        }
    }
    snprintf(path, size, "%s\\%016llx.spv", spv_dir, hash);
}

/* a shader's SPIR-V (malloc'd, *n bytes): the loose file, else the pack; NULL if neither has it */
static u32 *spv_read(u64 hash, long *n) {
    char path[MAX_PATH + 32];
    spv_path(path, sizeof path, hash);
    FILE *f = fopen(path, "rb");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        u32 *code = sz > 0 ? malloc(sz) : NULL;
        size_t got = code ? fread(code, 1, sz, f) : 0;
        fclose(f);
        if (code && got == (size_t)sz) { *n = sz; return code; }
        free(code);
    }
    const PakEnt *e = pak_find(hash, 0);
    if (!e) return NULL;
    void *z = malloc(e->packed);
    u32 *code = malloc(e->size + 4);
    OVERLAPPED ov = {0};                /* reads at an offset: no shared file position, any thread */
    ov.Offset = (DWORD)e->offset;
    ov.OffsetHigh = (DWORD)(e->offset >> 32);
    DWORD got = 0;
    bool ok = z && code && ReadFile(pak.f, z, e->packed, &got, &ov) && got == e->packed;
    if (ok) {
        size_t r = ZSTD_decompress(code, e->size, z, e->packed);
        ok = !ZSTD_isError(r) && r == e->size;
    }
    free(z);
    if (!ok) {
        hlog(0, "gpu: shader %016llx: bad entry in the shader pack", hash);
        free(code);
        return NULL;
    }
    *n = (long)e->size;
    return code;
}

static bool spv_exists(u64 hash) {
    char path[MAX_PATH + 32];
    spv_path(path, sizeof path, hash);
    return GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES || pak_find(hash, 0);
}

/* Shaders are translated while the game runs. A shader the game creates with no SPIR-V is queued at once
 * (gpu_shader_created) for a host thread that runs tools/shader/translate.py --spv --only=<hashes> on everything
 * queued (~0.3 s a batch: shaders created together share one). A draw that needs a shader still queued waits for it
 * (the game stalls instead of drawing without it), or with NX_ASYNC_SHADERS (env_init) is skipped until the
 * translation is there; dispatches always wait (a skipped one can leave wrong results for good). NX_SHADER_JIT=0
 * turns this off (such draws are skipped); NX_PYTHON names the interpreter (default python). */
static struct { u64 *q; u32 *qm, n, cap; u64 seq; SRWLOCK lock; HANDLE wake; } jit = {.lock = SRWLOCK_INIT};
static volatile u64 jit_done;          /* the last request a finished batch covered */

static volatile u64 jit_last_request_ms;   /* GetTickCount64 of the last request */

/* the translator's progress through the current burst of requests (vk.c: "COMPILING SHADERS done/total"); false
 * when it has nothing to do */
bool gpu_shader_progress(u32 *done, u32 *total) {
    static u64 base;                    /* jit.seq when the burst began */
    u64 seq = __atomic_load_n(&jit.seq, __ATOMIC_ACQUIRE), fin = __atomic_load_n(&jit_done, __ATOMIC_ACQUIRE);
    if (fin >= seq) { base = seq; return false; }
    if (base > fin) base = fin;
    *done = (u32)(fin - base);
    *total = (u32)(seq - base);
    return true;
}

/* the start-up's shaders: true until the translator first has nothing to do and nothing new came for 2 s (input is
 * held until then: the first start draws little while thousands are translated); false for good after that */
bool gpu_shaders_starting(void) {
    static volatile bool over;
    static u64 t0;
    if (over) return false;
    u64 now = GetTickCount64();
    if (!t0) t0 = now;
    u64 last = jit_last_request_ms ? jit_last_request_ms : t0;
    if (__atomic_load_n(&jit_done, __ATOMIC_ACQUIRE) >= __atomic_load_n(&jit.seq, __ATOMIC_ACQUIRE) && now - last > 2000)
        over = true;
    return !over;
}

static unsigned __stdcall jit_thread(void *p) {
    char dir[MAX_PATH];
    snprintf(dir, sizeof dir, "%s", spv_dir);
    char *slash = strstr(dir, "\\spv");                   /* the shader dir: above spv\<version> */
    if (slash) *slash = 0;
    const char *py = getenv("NX_PYTHON") ? getenv("NX_PYTHON") : "python";
    char tp[MAX_PATH];
    translator_path(tp, sizeof tp);
#ifdef NX_PACKAGE
    char pkpy[MAX_PATH];                                  /* the package's own Python, next to translate.py */
    snprintf(pkpy, sizeof pkpy, "%s", tp);
    char *ts = strrchr(pkpy, '\\');
    if (ts) snprintf(ts, sizeof pkpy - (size_t)(ts - pkpy), "\\python\\python.exe");
    if (!getenv("NX_PYTHON")) py = pkpy;
    /* translate.py takes glslang from %VULKAN_SDK%\Bin\glslangValidator.exe: the package's copy, translator\Bin\ */
    char sdk[MAX_PATH];
    snprintf(sdk, sizeof sdk, "%s", tp);
    if ((ts = strrchr(sdk, '\\')) != NULL) *ts = 0;
    SetEnvironmentVariableA("VULKAN_SDK", sdk);
#endif
    for (;;) {
        WaitForSingleObject(jit.wake, INFINITE);
        AcquireSRWLockExclusive(&jit.lock);
        u32 n = jit.n;
        u64 upto = jit.seq;
        u64 *hs = malloc(n * sizeof *hs + 1);
        u32 *ms = malloc(n * sizeof *ms + 1);
        memcpy(hs, jit.q, n * sizeof *hs);
        memcpy(ms, jit.qm, n * sizeof *ms);
        jit.n = 0;
        ReleaseSRWLockExclusive(&jit.lock);
        if (n) {
            char list[MAX_PATH + 16];           /* the hashes go in a file: the game makes ~2,300 shaders at once */
            snprintf(list, sizeof list, "%s\\jit_list.txt", dir);
            FILE *lf = fopen(list, "w");
            for (u32 i = 0; lf && i < n; i++)          /* <hash>:<mask>: a variant with integer inputs */
                if (ms[i]) fprintf(lf, "%016llx:%08x\n", hs[i], ms[i]);
                else fprintf(lf, "%016llx\n", hs[i]);
            if (lf) fclose(lf);
            size_t len = strlen(py) + strlen(tp) + 3 * strlen(dir) + 128;
            char *cmd = malloc(len);
            snprintf(cmd, len, "\"%s\" \"%s\" --spv \"--only=@%s\" \"%s\" \"%s\\glsl\"", py, tp, list, dir, dir);
            u64 t0 = GetTickCount64();
            /* its output through a pipe of our own (_popen needs the CRT's stdin/stdout, which may not be there) */
            SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
            HANDLE rd, wr;
            STARTUPINFOA si = {sizeof si};
            PROCESS_INFORMATION pi;
            bool ran = false;
            if (CreatePipe(&rd, &wr, &sa, 0)) {
                SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
                si.dwFlags = STARTF_USESTDHANDLES;
                si.hStdInput = NULL;
                si.hStdOutput = si.hStdError = wr;
                ran = CreateProcessA(NULL, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED, NULL, NULL, &si, &pi);
                CloseHandle(wr);
                if (ran) {
                    /* translate.py runs glslang on every core at once, which starved the game's threads (a 133-shader
                     * batch: 2.6 s frames, the main thread waiting for a core 73% of the time). A job keeps it and
                     * its glslang processes at below-normal priority on the upper half of our cores */
                    static HANDLE job;
                    if (!job && (job = CreateJobObjectA(NULL, NULL))) {
                        DWORD_PTR mine = 0, sys = 0;
                        GetProcessAffinityMask(GetCurrentProcess(), &mine, &sys);
                        int nc = 0;
                        for (DWORD_PTR m = mine; m; m &= m - 1) nc++;
                        DWORD_PTR half = 0;
                        int keep = nc / 2, seen = 0;
                        for (int b = 63; b >= 0 && seen < keep; b--)
                            if (mine >> b & 1) { half |= (DWORD_PTR)1 << b; seen++; }
                        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li = {0};
                        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PRIORITY_CLASS |
                            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | (half ? JOB_OBJECT_LIMIT_AFFINITY : 0);
                        li.BasicLimitInformation.PriorityClass = BELOW_NORMAL_PRIORITY_CLASS;
                        li.BasicLimitInformation.Affinity = half;
                        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &li, sizeof li);
                        hlog(1, "gpu: translate.py runs at below-normal priority on %d of %d cores", half ? keep : nc, nc);
                    }
                    if (job) AssignProcessToJobObject(job, pi.hProcess);
                    ResumeThread(pi.hThread);
                    char buf[4096], line[512];
                    DWORD got;
                    size_t ln = 0;
                    while (ReadFile(rd, buf, sizeof buf, &got, NULL) && got)
                        for (DWORD i = 0; i < got; i++) {
                            if (buf[i] != '\n' && buf[i] != '\r' && ln < sizeof line - 1) { line[ln++] = buf[i]; continue; }
                            if (buf[i] != '\n' && buf[i] != '\r') continue;
                            line[ln] = 0;
                            if (ln) hlog(1, "gpu: translate.py: %s", line);
                            ln = 0;
                        }
                    WaitForSingleObject(pi.hProcess, INFINITE);
                    DWORD code = 0;
                    GetExitCodeProcess(pi.hProcess, &code);
                    if (code) hlog(0, "gpu: translate.py exited with %lu", code);
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                }
                CloseHandle(rd);
            }
            if (!ran) hlog(0, "gpu: could not run %s (%lu)", py, GetLastError());
            hlog(1, "gpu: translate.py ran on %u shaders in %llu ms", n, GetTickCount64() - t0);
            free(cmd);
        }
        free(hs);
        free(ms);
        __atomic_store_n(&jit_done, upto, __ATOMIC_RELEASE);
    }
    return 0;
}

/* queue a translation: the request's number (jit_done reaches it when its batch is done), 0 if the JIT is off */
static u64 jit_request_variant(u64 hash, u32 mask) {
    static volatile LONG state;         /* 0 not started, 1 starting, 2 on, 3 off */
    if (state != 2 && state != 3) {
        if (InterlockedCompareExchange(&state, 1, 0) == 0) {
            char tmp[MAX_PATH + 32];
            spv_path(tmp, sizeof tmp, 0);           /* finds the translator and the pack */
            bool on = env.shader_jit && have_translator;
            if (!have_translator) hlog(1, "gpu: no shader translator: shaders come from the pack, others are skipped");
            if (on) {
                jit.wake = CreateEventA(NULL, FALSE, FALSE, NULL);
                _beginthreadex(NULL, 0, jit_thread, NULL, 0, NULL);
            }
            InterlockedExchange(&state, on ? 2 : 3);
        }
        while (state == 1) Sleep(0);
    }
    if (state == 3) return 0;
    AcquireSRWLockExclusive(&jit.lock);
    if (jit.n == jit.cap) {
        jit.cap = jit.cap * 2 + 64;
        jit.q = realloc(jit.q, jit.cap * sizeof *jit.q);
        jit.qm = realloc(jit.qm, jit.cap * sizeof *jit.qm);
    }
    jit.qm[jit.n] = mask;
    jit.q[jit.n++] = hash;
    u64 seq = ++jit.seq;
    jit_last_request_ms = GetTickCount64();
    ReleaseSRWLockExclusive(&jit.lock);
    SetEvent(jit.wake);
    return seq;
}

static u64 jit_request(u64 hash) { return jit_request_variant(hash, 0); }

/* nvn.c: the game made a shader (its code is dumped): translate it now if it has no SPIR-V */
void gpu_shader_created(ShaderCode *s) {
    if (!spv_exists(s->hash)) s->jit_seq = jit_request(s->hash);
}

static bool jit_pending(u64 seq) { return seq && __atomic_load_n(&jit_done, __ATOMIC_ACQUIRE) < seq; }

/* NULL either for good (s->missing) or, when !wait, while its translation is still running (try again later) */
static VkShaderModule shader_load(ShaderCode *s, bool wait) {
    long n = 0;
    u32 *code = spv_read(s->hash, &n);
    if (!code) {
        if (!s->jit_seq) s->jit_seq = jit_request(s->hash);
        u64 seq = s->jit_seq;
        if (!wait && jit_pending(seq)) return NULL;
        if (seq) {
            u64 t0 = GetTickCount64();
            while (__atomic_load_n(&jit_done, __ATOMIC_ACQUIRE) < seq) Sleep(1);
            hlog(1, "gpu: waited %llu ms for shader %016llx", GetTickCount64() - t0, s->hash);
            hf.jit += (double)(GetTickCount64() - t0);
            hf.njit++;
            code = spv_read(s->hash, &n);
        }
        if (!code) {
            s->missing = true;
            hlog(0, "gpu: shader %016llx has no translation%s", s->hash,
                 seq ? " (translate.py failed on it)" : have_translator ? "" : " (not in the shader pack: its draws are skipped)");
            return NULL;
        }
    }
    u32 cb_id[17], ncb = 0;             /* the cbN variables (OpName comes before OpDecorate) */
    u8 cb_n[17];
    for (u32 k = 0; k < 16; k++) s->cb_of_slot[k] = (u8)k;
    s->slots_used = 0;
    /* NX_FEEDBACK_LOG: the texture slots read, from the access chains into cb2 (cb2.d[i][j]: byte 16i + 4j) */
    u32 cb2 = 0, bound = n >= 20 ? code[3] : 0;
    u64 *cval = env.feedback_log && bound ? calloc(bound, sizeof *cval) : NULL;    /* OpConstant: 1 << 32 | value */
    s->tex_slots = 0;
    s->tex_dyn = false;
    for (long i = 5; i < n / 4;) {      /* what the shader touches: PhysicalStorageBufferAddresses, sampled images */
        u32 op = code[i] & 0xffff, wc = code[i] >> 16;
        if (op == 5 && wc >= 3 && ncb < 17) {                   /* OpName "cb<N>": c[N] */
            const char *nm = (const char *)&code[i + 2];
            if (nm[0] == 'c' && nm[1] == 'b' && nm[2] >= '0' && nm[2] <= '9') {
                u32 v = 0, k = 2;
                while (nm[k] >= '0' && nm[k] <= '9' && k < 5) v = v * 10 + (u32)(nm[k++] - '0');
                if (!nm[k] && v <= 16) { cb_id[ncb] = code[i + 1]; cb_n[ncb++] = (u8)v; }
                if (!nm[k] && v == 2) cb2 = code[i + 1];
            }
        }
        if (cval && op == 43 && wc == 4 && code[i + 2] < bound) cval[code[i + 2]] = 1ull << 32 | code[i + 3];
        if (cval && (op == 65 || op == 66) && wc >= 4 && cb2 && code[i + 3] == cb2) {  /* Op(InBounds)AccessChain */
            const u32 *x = &code[i + 4];
            if (wc == 7 && x[1] < bound && x[2] < bound && cval[x[1]] && cval[x[2]]) {
                u32 off = (u32)cval[x[1]] * 16 + (u32)cval[x[2]] * 4;
                if (off >= 0x20 && off < 0x120 && !(off & 7)) s->tex_slots |= 1u << ((off - 0x20) >> 3);
            } else s->tex_dyn = true;
        }
        if (op == 71 && wc == 4 && code[i + 2] == 33)          /* OpDecorate Binding: its slot */
            for (u32 k = 0; k < ncb; k++)
                if (cb_id[k] == code[i + 1]) {
                    s->cb_of_slot[code[i + 3] & 15] = cb_n[k];
                    s->slots_used |= (u16)(1u << (code[i + 3] & 15));
                }
        if (op == 17 && wc == 2 && code[i + 1] == 5347) s->caps |= SHC_GLOBAL;
        if (op == 17 && wc == 2 && code[i + 1] == 53) s->caps |= SHC_XFB;         /* TransformFeedback */
        if (op == 27) s->caps |= SHC_TEXTURE;
        if (op == 25 && wc >= 9 && code[i + 7] == 1 && (code[i + 3] != 1 || code[i + 5]))
            s->caps |= SHC_TEX_NON2D;           /* OpTypeImage, sampled, not plain 2D (Dim 1, not arrayed) */
        if (op == 25 && wc >= 9 && code[i + 7] == 2) s->caps |= SHC_IMAGE;     /* OpTypeImage, Sampled = 2: storage */
        i += wc ? wc : 1;
    }
    free(cval);
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

/* wait: false for draws with NX_ASYNC_SHADERS (shader_load) */
static VkShaderModule shader_module_w(ShaderCode *s, bool wait) {
    if (s->module) return s->module;
    if (s->missing) return NULL;
    if (!wait && jit_pending(s->jit_seq)) return NULL;     /* cheap: no file lookup while it is being translated */
    double t0 = ms_now();
    VkShaderModule m = shader_load(s, wait);
    hf.shader += ms_now() - t0;
    hf.nshader++;
    return m;
}

static VkShaderModule shader_module(ShaderCode *s) { return shader_module_w(s, true); }

/* the integer-input mask of the bound vertex attributes: 2 bits per location, 1 unsigned, 2 signed (8/16-bit
 * UINT / SINT, fetched as USCALED / SSCALED: the vertex shader's INT_ATTRS specialization constant, translate.py) */
static u32 int_attr_mask(void) {
    u32 mask = 0;
    for (u32 i = 0; i < st.nattribs && i < 16; i++) {
        u32 f = VA_FORMAT(st.attribs[i]);
        if (!f || VA_STREAM(st.attribs[i]) >= 31) continue;
        VkFormat vf = vk_format(f);
        u32 k = vf >= VK_FORMAT_R8_UNORM && vf <= VK_FORMAT_R8G8B8A8_SRGB ? (vf - VK_FORMAT_R8_UNORM) % 7 :
                vf >= VK_FORMAT_R16_UNORM && vf <= VK_FORMAT_R16G16B16A16_SFLOAT ? (vf - VK_FORMAT_R16_UNORM) % 7 : 0;
        if (k == 4) mask |= 1u << (2 * i);              /* UINT */
        else if (k == 5) mask |= 2u << (2 * i);         /* SINT */
    }
    return mask;
}

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
static SRWLOCK pipes_lock = SRWLOCK_INIT;
static u32 npipes;
static FILE *pipe_list;                 /* pipelines.list, appended to */
#define PIPE_LIST_MAGIC 0x4c50584eu     /* "NXPL", then the version and the key size */
#define PIPE_LIST_VERSION 1u
/* NX_PIPE_LOG (env.pipe_log): log each pipeline's shaders before it is made (a driver that aborts while compiling one) */
static u64 pipe_vs_hash, pipe_fs_hash;

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
                      vk.warp32 & VK_SHADER_STAGE_COMPUTE_BIT ? &vk_warp32 : NULL, .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                      .module = vs, .pName = "main"}, .layout = layout_gfx};
        r = vkCreateComputePipelines(vk.dev, pcache, 1, &ci, NULL, pipe);
    } else {
        /* the alpha test is a specialization constant, so a draw without one has no discard from it (early depth) */
        VkSpecializationMapEntry sme = {0, 0, sizeof k->alpha_func};
        VkSpecializationInfo spec = {1, &sme, sizeof k->alpha_func, &k->alpha_func};
        /* the vertex shader's integer inputs (INT_ATTRS, constant 1): which inputs are fetched as USCALED / SSCALED */
        VkSpecializationMapEntry vsme = {1, 0, sizeof k->vs_mask};
        VkSpecializationInfo vspec = {1, &vsme, sizeof k->vs_mask, &k->vs_mask};
        VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, vk.warp32 & VK_SHADER_STAGE_VERTEX_BIT ? &vk_warp32 : NULL,
             .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main", .pSpecializationInfo = &vspec},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, vk.warp32 & VK_SHADER_STAGE_FRAGMENT_BIT ? &vk_warp32 : NULL,
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
        VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri,
            .stageCount = k->fs ? 2 : 1, .pStages = stages, .pInputAssemblyState = &ia, .pViewportState = &vp,
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
    BinHdr h = {BINS_MAGIC, 1, sizeof(PipeKey), vk.props.vendorID, vk.props.deviceID, vk.props.driverVersion,
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

static void bins_load(void) {
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
            InterlockedExchange(&p->state, P_READY);
        } else {                                /* not on disk: back to the render thread */
            p->no_async = true;
            InterlockedIncrement(&async_failed);
            InterlockedExchange(&p->state, P_LISTED);
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
static VkPipeline pipeline_get(const PipeKey *k, VkShaderModule vs, VkShaderModule fs, bool async, bool *pending) {
    bool claimed;
    *pending = false;
    Pipe *p = pipe_claim(k, P_BUILDING, &claimed);
    if (!claimed) {
        if (p->state == P_READY) return p->pipe;
        if (async) { *pending = true; return NULL; }
        double t0 = ms_now();                   /* a worker is building it */
        while (p->state != P_READY) Sleep(1);
        hf.pipe += ms_now() - t0;
        hf.npipe++;
        return p->pipe;
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
            InterlockedExchange(&p->state, P_READY);
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

static void pipelines_prebuild(void) {
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
static volatile LONG capture_request;      /* F12 (vk.c): dump and trace the next whole frame */
static u64 capture_frame;
void gpu_capture_next(void) { InterlockedExchange(&capture_request, 1); }
static volatile LONG64 capture_done_ms;   /* GetTickCount64 when the last capture was written */
/* vk.c's message box: what F12 is doing (NULL: nothing to say) */
const char *gpu_capture_status(void) {
    if (capture_request || capture_frame) return "CAPTURING FRAME...";
    LONG64 done = capture_done_ms;
    return done && GetTickCount64() - (u64)done < 3000 ? "FRAME CAPTURED" : NULL;
}
/* F11 (vk.c): upload every texture again from guest memory at the next present (are copies stale?) */
static volatile LONG reload_request;
void gpu_reload_textures(void) { InterlockedExchange(&reload_request, 1); }

static bool trace_frame(void) {
    if (capture_frame && frame_no + 1 == capture_frame) return true;
    long f = (long)frame_no + 1;
    return env.trace_lo >= 0 && f >= env.trace_lo && f <= env.trace_hi;
}

/* skipped draws by reason (the reasons are string literals), reported with the statistics every 600 frames */
static const char *skip_why[16];
static u64 skip_count[16];
static int nskip_why;
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

/* the uniform buffers of one stage (push descriptors at binding base + slot; the shader says which c[i] each slot
 * holds: c[i] in slot i, c[16] in one the shader leaves free) */
static int stage_ubos(u32 stage, u32 base, const ShaderCode *sc, VkWriteDescriptorSet *w, VkDescriptorBufferInfo *bi) {
    static DrvCache cache[ST_COUNT];
    cmd();
    DescSet *ts = descset_get(&st.texpool, DS_TEX);
    u32 tex[32];
    for (u32 k = 0; k < 32; k++) {
        u32 h = (u32)st.tex[stage][k], id = h & 0xfffff;
        if (ts && id < ts->count && ts->intv[id]) h |= HANDLE_INT;
        tex[k] = h;
    }
    DrvCache *cc = &cache[stage % ST_COUNT];
    /* compared in place (twice a draw, ~2,000 draws a frame): the key is built only when it changed */
    bool hit = !env.ssbo_log && cc->serial == slot_serial && cc->pools_gen == pools_gen &&
               !memcmp(cc->ssbo, st.ssbo[stage], sizeof cc->ssbo) && !memcmp(cc->tex, tex, sizeof tex) &&
               (stage != ST_COMPUTE || !memcmp(cc->ubo, st.ubo[stage], sizeof cc->ubo));
    for (u32 k = 0; hit && k < 8; k++) hit = cc->img[k] == (u32)st.img[stage][k];
    if (!hit) {
        DrvCache key;
        memset(&key, 0, sizeof key);
        key.serial = slot_serial;
        key.pools_gen = pools_gen;
        memcpy(key.ssbo, st.ssbo[stage], sizeof key.ssbo);
        if (stage == ST_COMPUTE) memcpy(key.ubo, st.ubo[stage], sizeof key.ubo);
        memcpy(key.tex, tex, sizeof tex);
        for (u32 k = 0; k < 8; k++) key.img[k] = (u32)st.img[stage][k];
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
static struct {
    u64 serial;
    bool dyn_ok, vi_ok, bind_ok;        /* each part recorded since the reset */
    DynState d;
    VkPipeline pipe;
    VkDescriptorSet sets[3];
    u32 pc[4];
    u32 nw;
    VkDescriptorBufferInfo bi[32];
    u32 binding[32], type[32];
    u32 nb, na;
    VkVertexInputBindingDescription2EXT bind[32];
    VkVertexInputAttributeDescription2EXT attr[48];
    VkBuffer bufs[32];
    VkDeviceSize offs[32], sizes[32];
} rec = {~0ull};

/* forget what was recorded once a new command buffer is being built (its state is undefined) */
static void rec_check(void) {
    if (rec.serial == slot_serial) return;
    memset(&rec, 0, sizeof rec);
    rec.serial = slot_serial;
}

/* after a built-in pass (depth_blit) bound its own pipeline, layout and state: record everything again */
static void rec_forget(void) {
    memset(&rec, 0, sizeof rec);
    rec.serial = slot_serial;
}

static bool debug_rt_hit(void) { return env.debug_rt && st.nrt && st.rt[0].t.addr == env.debug_rt; }

static void set_dynamic_state(VkCommandBuffer c, u32 mode) {
    DynState d;
    memset(&d, 0, sizeof d);
    d.vp = (VkViewport){st.viewport[0][0], st.viewport[0][1], st.viewport[0][2], st.viewport[0][3],
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
        VkFormat vf = vk_format(f);
        if (vf == VK_FORMAT_UNDEFINED) continue;
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
    if (all || nb != rec.nb || na != rec.na || memcmp(bind, rec.bind, nb * sizeof *bind) ||
        memcmp(attr, rec.attr, na * sizeof *attr)) {
        vkCmdSetVertexInputEXT(c, nb, bind, na, attr);
        rec.nb = nb;
        rec.na = na;
        memcpy(rec.bind, bind, nb * sizeof *bind);
        memcpy(rec.attr, attr, na * sizeof *attr);
    }
    if (all || memcmp(bufs, rec.bufs, sizeof bufs) || memcmp(offs, rec.offs, sizeof offs) ||
        memcmp(sizes, rec.sizes, sizeof sizes)) {
        u32 top = 0;                    /* streams 0..top-1 in use, and 31: the zero source */
        for (u32 s = 0; s < 31; s++)
            if (used[s]) top = s + 1;
        if (top) vkCmdBindVertexBuffers2(c, 0, top, bufs, offs, sizes, NULL);
        vkCmdBindVertexBuffers2(c, 31, 1, &bufs[31], &offs[31], &sizes[31], NULL);
        memcpy(rec.bufs, bufs, sizeof bufs);
        memcpy(rec.offs, offs, sizeof offs);
        memcpy(rec.sizes, sizes, sizeof sizes);
    }
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

static void xfb_begin(VkCommandBuffer c, u32 op, const void *payload) {
    const ShaderCode *vs = st.program ? st.program->stage[ST_VERTEX] : NULL;
    if (!st.xfb_active || !vs || !(vs->caps & SHC_XFB)) return;     /* the translation carries the layout */
    VkBuffer bufs[4];
    VkDeviceSize offs[4], sizes[4];
    u32 n = 0, used = 4;
    for (u32 b = 0; b < 4; b++) {
        u64 off;
        bufs[b] = NULL, offs[b] = 0, sizes[b] = 0;
        if (!vs->xfb_stride[b]) continue;
        if (!st.xfb[b].addr || !gpu_buffer(st.xfb[b].addr, &bufs[b], &off)) return;
        mirror_bad(st.xfb[b].addr, st.xfb[b].size);
        u64 w = st.xfb_written[b] < st.xfb[b].size ? st.xfb_written[b] : st.xfb[b].size;
        offs[b] = off + w;
        sizes[b] = st.xfb[b].size - w;
        n = b + 1;
        if (used == 4) used = b;
    }
    if (!n) return;
    for (u32 b = 0; b < n; b++)          /* a buffer the shader skips: any valid one, with nothing to write */
        if (!bufs[b]) bufs[b] = bufs[used], offs[b] = offs[used], sizes[b] = 0;
    vkCmdBindTransformFeedbackBuffersEXT(c, 0, n, bufs, offs, sizes);
    vkCmdBeginTransformFeedbackEXT(c, 0, 0, NULL, NULL);
    xfb_capturing = true;
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
        u64 *d = st.xfb_dirty[b];
        if (d[0] == d[1]) d[0] = lo, d[1] = hi;
        else d[0] = lo < d[0] ? lo : d[0], d[1] = hi > d[1] ? hi : d[1];
        st.xfb_written[b] += v * vs->xfb_stride[b];
    }
}

static bool xfb_dirty_overlaps(u64 addr, u64 size) {
    if (!addr) return false;
    if (!size) size = 1;
    for (u32 b = 0; b < 4; b++)
        if (st.xfb_dirty[b][0] < st.xfb_dirty[b][1] && addr < st.xfb_dirty[b][1] && st.xfb_dirty[b][0] < addr + size)
            return true;
    return false;
}

/* does this draw read (or capture over) what earlier draws captured since the last barrier? Captures go to
 * successive offsets, so a run of capturing draws needs no barrier between them, nor does a draw that reads none
 * of the captured bytes */
static bool xfb_hazard(u32 op, const void *payload) {
    for (u32 i = 0; i < 32; i++)
        if (xfb_dirty_overlaps(st.vbuf[i].addr, st.vbuf[i].size ? st.vbuf[i].size : ~0ull - st.vbuf[i].addr)) return true;
    for (u32 stage = 0; stage < ST_COUNT; stage++) {
        for (u32 i = 0; i < 18; i++) if (xfb_dirty_overlaps(st.ubo[stage][i].addr, st.ubo[stage][i].size)) return true;
        for (u32 i = 0; i < 16; i++) if (xfb_dirty_overlaps(st.ssbo[stage][i].addr, st.ssbo[stage][i].size)) return true;
    }
    if (op == OP_DRAW_ARRAYS || op == OP_DRAW_ELEMENTS) {
        const CmdDraw *d = payload;
        if (d->index_type != ~0u && xfb_dirty_overlaps(d->index_addr, (u64)(d->first + d->count) * 4)) return true;
    } else if (op == OP_DRAW_ARRAYS_INDIRECT || op == OP_DRAW_ELEMENTS_INDIRECT) {
        const CmdDrawIndirect *d = payload;
        if (xfb_dirty_overlaps(d->index_addr, ~0ull >> 8) || xfb_dirty_overlaps(d->indirect, 64) ||
            xfb_dirty_overlaps(d->count_addr, 4)) return true;
    } else return true;
    const ShaderCode *vs = st.program ? st.program->stage[ST_VERTEX] : NULL;
    if (st.xfb_active && vs && vs->caps & SHC_XFB)
        for (u32 b = 0; b < 4; b++)
            if (vs->xfb_stride[b] && st.xfb_written[b] < st.xfb[b].size &&
                xfb_dirty_overlaps(st.xfb[b].addr + st.xfb_written[b], st.xfb[b].size - st.xfb_written[b]))
                return true;
    return false;
}

static void draw_one(u32 op, const void *payload);
static void dump_image_as(Image *im, const char *name, bool raw, VkFormat fmt);

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

static void draw(u32 op, const void *payload) {
    if (st.xfb_captured && xfb_hazard(op, payload)) {   /* what was captured is read (or captured into) next */
        bool in_pass = st.rendering;
        end_rendering();
        if (!in_pass) barrier();        /* end_rendering has made one */
        st.xfb_captured = false;
        memset(st.xfb_dirty, 0, sizeof st.xfb_dirty);
    }
    draw_one(op, payload);
    label_draw_end();
    dump_after_draw();
    if (xfb_capturing) {
        vkCmdEndTransformFeedbackEXT(cmd(), 0, 0, NULL, NULL);
        xfb_capturing = false;
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
        char b[512];
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
    struct { u32 func; float ref, point_size; u32 debug; } pc = {st.color.alpha_test, st.alpha_ref, st.point_size,
                                                                (u32)(no_discard && (st.ds.flags & 2))};
    if (debug_rt_hit() && env.debug_rt_bits & 1) pc.debug |= 1;
    /* only with a color target 0: without one (the G-buffer passes, targets 1-4) its alpha means nothing, and
     * testing it hid whole objects */
    if (st.ms.a2c && st.samples <= 1 && (pc.func == 0 || pc.func == 8) && st.nrt && st.rt[0].t.addr) {
        pc.func = 7;                    /* GEQUAL 0.5: alpha to coverage on one sample */
        pc.ref = 0.5f;
    }
    if (all || memcmp(&pc, rec.pc, 16)) {
        vkCmdPushConstants(c, layout_gfx, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, &pc);
        memcpy(rec.pc, &pc, 16);
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
    if (!gpu_buffer_ro(d->index_addr, 1, &ib, &io)) { skip_draw("index buffer not GPU memory"); return; }
    VkIndexType it = d->index_type == 0 ? VK_INDEX_TYPE_UINT8_KHR : d->index_type == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    vkCmdBindIndexBuffer(c, ib, io, it);
    QUERIED(vkCmdDrawIndexedIndirect(c, buf, off, 1, 20));
}

/* ---- compute */
static u64 pr_hash(void) { return st.bound[ST_COMPUTE] ? st.bound[ST_COMPUTE]->hash : 0; }

static void dispatch(u32 op, const u8 *pay) {
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
            end_rendering();
            /* games submit a whole frame at once: hand the GPU what is recorded so far, at a pass boundary, so
             * that it works while the rest is translated (NX_FLUSH_DRAWS, 0 for one submission per guest one) */
            if (env.flush_draws && work_unflushed >= (u32)env.flush_draws) flush();
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
    if (value > submitted) gpu_flush(0);
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
        linear_wrote(im);
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

/* the CPU reads texels (nvnTextureReadTexels): copy the region out of the image after the work submitted so far */
void gpu_texture_read(const TexInfo *t, const ViewInfo *v, const CopyRegion *r, void *data,
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
        VkBuf b;
        vk_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &b);
        image_upload(im);
        CmdCopyBT c = {0};
        c.tex.t = *t;
        c.tex.v = *v;
        c.r = *r;
        c.row_stride = (s64)row;
        c.image_stride = (s64)img;
        VkBufferImageCopy reg = bt_region(&c, im, 0);
        end_rendering();
        barrier();
        vkCmdCopyImageToBuffer(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, b.buf, 1, &reg);
        flush();
        gpu_wait(submitted, UINT64_MAX);
        memcpy(data, b.a.map, size);
        vk_buffer_free(&b);
    } else {
        hlog(0, "gpu: ReadTexels of %ux%u f%u: no image (left as is)", t->width, t->height, t->format);
    }
    LeaveCriticalSection(&gpu_cs);
}

/* ---------------------------------------------------------------------------------------------- present */
/* NX_DUMP=n[,m...]: at those frames, write the presented window texture (frame_<n>.ppm) and every render target
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
    case VK_FORMAT_R32G32B32A32_SFLOAT: {
        float c[3];
        memcpy(c, p, 12);
        for (int i = 0; i < 3; i++) out[i] = to8(c[i]);
        return true;
    }
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

/* fmt: the format to decode the pixels as (a view's, which may differ from the image's); raw: also the bytes */
static void dump_image_as(Image *im, const char *name, bool raw, VkFormat fmt) {
    u8 probe[3], zero[16] = {0};
    if (!decode_pixel(fmt, zero, probe) || im->info.samples > 1 || format_bytes(fmt) != format_bytes(im->format)) return;
    u32 w = im->info.width, h = tex_height(&im->info), bpp = format_bytes(fmt);
    VkBuf b;
    vk_buffer((u64)w * h * bpp, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &b);
    VkBufferImageCopy r = {0, 0, 0, {copy_aspect(im), 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    end_rendering();
    vkCmdCopyImageToBuffer(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, b.buf, 1, &r);
    flush();
    gpu_wait(submitted, UINT64_MAX);
    char path[MAX_PATH];
    const char *dir = frames_dir();
    if (im->format == VK_FORMAT_R32G32B32A32_SFLOAT && w * h <= 4) {       /* tiny float targets (exposure): logged */
        const float *v = (const float *)b.a.map;
        for (u32 i = 0; i < w * h; i++) hlog(0, "gpu: %s texel %u = %g %g %g %g", name, i, v[4 * i], v[4 * i + 1], v[4 * i + 2], v[4 * i + 3]);
    }
    if (raw) {
        snprintf(path, sizeof path, "%s\\%s.raw", dir, name);              /* float targets: exact values too */
        FILE *rf = fopen(path, "wb");
        if (rf) { fwrite(b.a.map, 1, (size_t)w * h * bpp, rf); fclose(rf); }
    }
    snprintf(path, sizeof path, "%s\\%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        const u8 *px = b.a.map;
        u8 *row = malloc(w * 3);
        for (u32 y = 0; y < h; y++) {
            for (u32 x = 0; x < w; x++) decode_pixel(fmt, px + ((u64)y * w + x) * bpp, row + 3 * x);
            fwrite(row, 1, w * 3, f);
        }
        free(row);
        fclose(f);
    }
    vk_buffer_free(&b);
}

static void dump_image(Image *im, const char *name, bool raw) {
    /* raw bytes for the float targets only (exact values) */
    bool fl = im->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || im->format == VK_FORMAT_R16G16B16A16_SFLOAT ||
              im->format == VK_FORMAT_R32G32B32A32_SFLOAT;
    dump_image_as(im, name, raw && fl, im->format);
}

static void dump_frame(Image *win, u64 frame) {
    const char *list = env.dump;
    u64 dump_tex = env.dump_tex;                        /* NX_DUMP_TEX: and every image at this address */
    char key[32], name[128];
    snprintf(key, sizeof key, ",%llu,", frame);
    if (frame != capture_frame && (!list[0] || !strstr(list, key))) return;
    int n = 0;
    for (int bkt = 0; bkt < IMAGE_BUCKETS; bkt++)
        for (Image *im = buckets[bkt]; im; im = im->next)
            if ((im->rt_frame == frame || (dump_tex && im->info.addr == dump_tex)) && im != win) {
                snprintf(name, sizeof name, "frame_%llu_%ux%u_f%u_%llx", frame, im->info.width, im->info.height,
                         im->info.format, im->info.addr);
                dump_image(im, name, true);
                n++;
            }
    if (win) {
        snprintf(name, sizeof name, "frame_%llu", frame);
        dump_image(win, name, true);
    }
    hlog(1, "gpu: frame %llu and %d render targets written to %s", frame, n, frames_dir());
}

/* textures the CPU rewrote since the last frame (write watch over the GPU-visible pools): upload them again */
static int range_cmp(const void *a, const void *b) {
    u64 x = ((const Range *)a)->lo, y = ((const Range *)b)->lo;
    return x < y ? -1 : x > y;
}

static double refresh_ms, refresh_mb;   /* time and bytes asked about, for the report every 600 frames */

/* the pages written in [lo, hi), appended to pages[] (write watch reset) */
static size_t written_pages(u64 lo, u64 hi, u64 **pages, size_t *cap, size_t n, double *mb) {
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
static int refresh_pages(const u64 *pages, size_t n) {
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

static void image_touch(Image *im) {
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
static double scan_ms, scan_ms_max;     /* the scans' time (the thread's), for the report every 600 frames */
static u64 scan_skips;                  /* presents that found the last scan still running and did not wait */

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

static void refresh_cpu_textures(void) {
    double t0 = ms_now();
    refresh_scan();
    refresh_ms += ms_now() - t0;
}

/* Which pages of the mirrored pools the CPU wrote is asked on a thread of its own (~0.5 ms for 367 MB, whatever is
 * written), started when the guest submits (mirror_scan_start) and awaited at the first submission after it, while
 * the render thread translates. Only the guest's submissions start one: the data their commands read was written
 * before they were submitted, as on the console (where the GPU may start on them at once) */
typedef struct { u64 lo, hi; size_t first, n; } MScan;
static MScan *msr;
static int nmsr, capmsr;
static u64 *ms_pages;
static size_t ms_cap;
static HANDLE ms_go, ms_done;
static bool ms_running;

static unsigned __stdcall mirror_scan_thread(void *arg) {
    (void)arg;
    for (;;) {
        WaitForSingleObject(ms_go, INFINITE);
        size_t n = 0;
        double mb = 0;
        for (int i = 0; i < nmsr; i++) {
            msr[i].first = n;
            n = written_pages(msr[i].lo, msr[i].hi, &ms_pages, &ms_cap, n, &mb);
            msr[i].n = n - msr[i].first;
        }
        SetEvent(ms_done);
    }
}

static void mirror_scan_start(void) {
    if (!nmirrors || ms_running) return;
    if (!ms_go) {
        ms_go = CreateEventW(NULL, FALSE, FALSE, NULL);
        ms_done = CreateEventW(NULL, FALSE, FALSE, NULL);
        _beginthreadex(NULL, 0, mirror_scan_thread, NULL, 0, NULL);
    }
    nmsr = 0;
    for (int i = 0; i < npools; i++) {
        if (!pools[i].mir || pools[i].mir->fresh) continue;
        if (nmsr == capmsr) msr = realloc(msr, (capmsr = capmsr * 2 + 8) * sizeof *msr);
        msr[nmsr++] = (MScan){pools[i].cpu, pools[i].cpu + (pools[i].end - pools[i].gpu)};
    }
    if (!nmsr) return;
    ms_running = true;
    SetEvent(ms_go);
}

/* the copies into the mirrors, recorded into `pre`, which runs before the submission's commands and after the work
 * submitted before it (what that read or wrote is done): the pages the last scan found, and new mirrors whole.
 * Images on those pages are uploaded again here: the write watch over them is the scan's */
static bool mirror_sync(VkCommandBuffer pre) {
    if (!nmirrors) return false;
    static VkBufferCopy *rg;
    static size_t caprg;
    bool begun = false;
    double t0 = ms_now();
    if (ms_running) WaitForSingleObject(ms_done, INFINITE);
    for (int i = 0; i < npools; i++) {
        PoolEnt *p = &pools[i];
        Mirror *m = p->mir;
        if (!m) continue;
        u64 size = p->end - p->gpu;
        size_t nr = 0;
        if (m->fresh) {                 /* all of it, and the write watch starts over */
            static u64 *pages;
            static size_t cap;
            double mb = 0;
            written_pages(p->cpu, p->cpu + size, &pages, &cap, 0, &mb);
            if (!caprg) rg = realloc(rg, (caprg = 1024) * sizeof *rg);
            rg[nr++] = (VkBufferCopy){0, 0, size};
            m->fresh = false;
        } else if (ms_running) {
            const MScan *sc = NULL;
            for (int k = 0; k < nmsr && !sc; k++) if (msr[k].lo == p->cpu && msr[k].hi == p->cpu + size) sc = &msr[k];
            if (!sc || !sc->n) continue;
            const u64 *pg = ms_pages + sc->first;
            for (size_t k = 0; k < sc->n; k++) {
                u64 o = pg[k] - p->cpu;
                if (nr && rg[nr - 1].srcOffset + rg[nr - 1].size == o) { rg[nr - 1].size += 4096; continue; }
                if (nr == caprg) rg = realloc(rg, (caprg = caprg * 2 + 1024) * sizeof *rg);
                rg[nr++] = (VkBufferCopy){o, o, 4096};
            }
            refresh_pages(pg, sc->n);
        }
        if (!nr) continue;
        if (!begun) {
            VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            VKCHECK(vkBeginCommandBuffer(pre, &bi));
            barrier_in(pre);
            begun = true;
        }
        for (size_t k = 0; k < nr; k++) mirror_mb += (double)rg[k].size / (1 << 20);
        vkCmdCopyBuffer(pre, p->hb->buf, m->b.buf, (u32)nr, rg);
    }
    ms_running = false;
    if (begun) {
        barrier_in(pre);
        VKCHECK(vkEndCommandBuffer(pre));
    }
    mirror_ms += ms_now() - t0;
    hf.mirror += ms_now() - t0;
    return begun;
}

/* the indirect draws of a traced frame, logged with the arguments the GPU wrote for them once it is done (instance
 * counts from the game's GPU culling: 0 means culled) */
static void ind_log_report(void) {
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

static void query_report(void) {
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
    if (frames % 600 == 0) hlog(1, "gpu: %llu draws so far (%llu skipped), %llu dispatches, %d images (%llu MB), %u pipelines",
                                  draws, draws_skipped, dispatches, nimages, image_bytes >> 20, npipes);
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
    vk_present(im ? im->image : NULL, w->crop[0], w->crop[1], cw, ch, window_origin == 0, timeline, v);
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
