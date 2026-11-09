/* NX_GPU_PROF (GPU time per label, guest waits by caller) and NX_GPU_TIMELINE (submit, GPU start and end, wake-ups
 * per submission); ms_now and the times behind the slow-frame lines (NX_HITCH_MS).
 * Part of the NVN executor (gpu.c, gpu_int.h). */
#include "gpu_int.h"

int prof_on;                            /* NX_GPU_PROF (env_init) */
VkQueryPool prof_pool[SLOTS];
static u32 prof_n[SLOTS];
static u16 prof_label[SLOTS][PROF_MAX];
typedef struct { char name[80]; double ms; u64 count; } ProfName;
#define PROF_NAMES 4096
static ProfName prof_names[PROF_NAMES];
static u32 nprof_names;
static u16 prof_hash[2 * PROF_NAMES];   /* open addressing: name index + 1 */
double prof_slot_wait_ms, prof_guest_wait_ms;
/* guest waits for the GPU by how many submissions behind the newest the awaited one was (0: the newest) */
double prof_wait_ms[4];
u64 prof_wait_n[4];
__declspec(thread) u64 gpu_wait_site;
struct ProfSite prof_sites[16];      /* guest waits by caller */
struct ProfEnd prof_ends[64];

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

void prof_mark_in(int slot, const char *label) {
    if (prof_n[slot] >= PROF_MAX) return;
    vkCmdWriteTimestamp2(slots[slot].cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof_pool[slot], prof_n[slot]);
    prof_label[slot][prof_n[slot]++] = prof_intern(label);
}

void prof_mark(const char *label) {
    if (prof_on <= 0) return;
    cmd();
    prof_mark_in(cur, label);
}

/* the slot's work is done: charge its intervals */
void prof_collect(int slot) {
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

void prof_report(u64 frames) {
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
    for (u32 i = 0; i < 64 && prof_ends[i].at; i++)
        if (prof_ends[i].count) hlog(0, "gpu prof: %6.1f passes/frame ended at %s", (double)prof_ends[i].count / frames, prof_ends[i].at);
    for (u32 i = 0; i < 64; i++) prof_ends[i].count = 0;
    for (u32 i = 0; i < nprof_names; i++) prof_names[i].ms = 0, prof_names[i].count = 0;
    prof_slot_wait_ms = prof_guest_wait_ms = 0;
}

/* the render thread's time per frame in our code, for the report every 600 frames */
double t_submit_ms, t_present_ms, t_wait_ms;
struct HitchTimes hf;

double ms_now(void) {
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
VkQueryPool tl_pool;
double tl_submit[SLOTS];
bool tl_first[SLOTS], tl_used[SLOTS], tl_guest_first;
double tl_woke, tl_core, tl_entry, tl_rec_woke[SLOTS], tl_rec_core[SLOTS], tl_rec_entry[SLOTS];
static TlRec *tl_recs;
static int tl_n, tl_cap;

void tl_collect(int slot) {
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

void tl_report(u64 frames) {
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
