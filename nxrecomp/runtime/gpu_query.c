/* Samples-passed counts (NVN counter 1) from Vulkan occlusion queries.
 *
 * NVN's counter runs on from ResetCounter; ReportCounter has the GPU write its value (and a timestamp) into a buffer
 * when it gets there. Games draw a test shape between two reports (or after a reset) and read how much of it passed
 * the depth test: occlusion culling, and how bright a light's glare is. Vulkan has no running counter, only queries
 * that begin and end inside one pass, so the counter is their sum: one query is open whenever the game renders, cut
 * at every report and reset. The sums cannot be made on the GPU; they are made here once the submission is done,
 * before any guest thread can learn that it is (gpu_completed, gpu_wait), and written to the report's place then.
 *
 * Counts are in the game's pixels: a pass at render scale N counts N * N samples for each.
 *
 * The render thread records (under gpu_cs); guest threads resolve as well, under occ_lock only. */
#include "gpu_int.h"

#define OCC_MAX 8192                    /* queries per submission */
enum { OCC_QUERY, OCC_RESET, OCC_REPORT };
typedef struct OccEvent { u8 kind, scale; u8 *dst; u64 ts; } OccEvent;
typedef struct OccSlot { VkQueryPool pool; OccEvent *ev; u32 nev, cap, nq; u64 value; } OccSlot;
static OccSlot occ[SLOTS];
static int fifo[SLOTS];                 /* submitted slots with events, oldest first */
static u32 fifo_head, fifo_n;
static volatile LONG occ_waiting;       /* fifo_n, for the check without the lock */
static SRWLOCK occ_lock = SRWLOCK_INIT;
static u64 occ_total;                   /* the counter after everything resolved so far */
static bool occ_wanted;                 /* the game uses the counter: passes count from now on */
static bool occ_open;                   /* a query is open in the pass being recorded */

static void event(u8 kind, u8 *dst) {
    OccSlot *o = &occ[cur];
    if (o->nev == o->cap) o->ev = realloc(o->ev, (o->cap = o->cap * 2 + 256) * sizeof *o->ev);
    o->ev[o->nev++] = (OccEvent){kind, (u8)(st.rt_scale ? st.rt_scale : 1), dst, kind == OCC_REPORT ? now_ns() : 0};
}

static void start(void) {
    OccSlot *o = &occ[cur];
    if (o->nq == OCC_MAX) {
        static bool logged;
        if (!logged) { logged = true; hlog(0, "gpu: more than %d occlusion queries in a submission: the rest count nothing", OCC_MAX); }
        return;
    }
    vkCmdBeginQuery(cmd(), o->pool, o->nq++, VK_QUERY_CONTROL_PRECISE_BIT);
    event(OCC_QUERY, NULL);
    occ_open = true;
}

static void stop(void) {
    if (!occ_open) return;
    vkCmdEndQuery(cmd(), occ[cur].pool, occ[cur].nq - 1);
    occ_open = false;
}

/* a pass has begun / is about to end */
void occ_begin(void) { if (occ_wanted) start(); }
void occ_end(void) { stop(); }

/* ResetCounter / ReportCounter of counter 1, in the command stream's order. False: not ours (NX_OCCLUSION=0) */
bool occ_mark(bool report, u64 addr) {
    if (!env.occlusion) return false;
    if (!occ_wanted) {
        occ_wanted = true;
        hlog(1, "gpu: the game counts samples passed: occlusion queries from here on (NX_OCCLUSION=0: every test passes)");
    }
    cmd();                              /* a slot to record in */
    stop();
    event(report ? OCC_REPORT : OCC_RESET, report && addr ? gpu_cpu(addr, 16) : NULL);
    if (st.rendering) start();
    return true;
}

void occ_slot_open(int slot) {
    if (!env.occlusion) return;
    OccSlot *o = &occ[slot];
    if (!o->pool) {
        VkQueryPoolCreateInfo qi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION,
                                    .queryCount = OCC_MAX};
        VKCHECK(vkCreateQueryPool(vk.dev, &qi, NULL, &o->pool));
        o->nq = OCC_MAX;
    }
    occ_resolve();                      /* its last use is done (slot_open waited): read it before the reset */
    if (o->nq) vkResetQueryPool(vk.dev, o->pool, 0, o->nq);
    o->nev = o->nq = 0;
}

/* the slot was submitted and is done at timeline `value` */
void occ_submitted(int slot, u64 value) {
    OccSlot *o = &occ[slot];
    if (!o->nev) return;
    AcquireSRWLockExclusive(&occ_lock);
    o->value = value;
    fifo[(fifo_head + fifo_n++) % SLOTS] = slot;
    occ_waiting = (LONG)fifo_n;
    ReleaseSRWLockExclusive(&occ_lock);
}

/* write the reports of every submission the GPU has finished */
void occ_resolve(void) {
    if (!occ_waiting) return;
    u64 done = 0;
    vkGetSemaphoreCounterValue(vk.dev, timeline, &done);
    AcquireSRWLockExclusive(&occ_lock);
    static u64 res[OCC_MAX];
    while (fifo_n) {
        OccSlot *o = &occ[fifo[fifo_head]];
        if (o->value > done) break;
        if (o->nq && vkGetQueryPoolResults(vk.dev, o->pool, 0, o->nq, o->nq * sizeof *res, res, sizeof *res,
                                           VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS)
            memset(res, 0, o->nq * sizeof *res);
        u32 q = 0, reports = 0, resets = 0;
        u64 first[8];
        for (u32 i = 0; i < o->nev; i++) {
            const OccEvent *e = &o->ev[i];
            if (e->kind == OCC_QUERY) {
                u64 s2 = (u64)e->scale * e->scale;
                occ_total += (res[q++] + s2 - 1) / s2;
            } else if (e->kind == OCC_RESET) {
                occ_total = 0;
                resets++;
            } else {
                u64 v[2] = {occ_total, e->ts};
                if (e->dst) memcpy(e->dst, v, sizeof v);
                if (reports < 8) first[reports] = occ_total;
                reports++;
            }
        }
        static u32 logged;
        if (reports && logged < 4) {
            logged++;
            char s[256];
            int n = 0;
            for (u32 i = 0; i < reports && i < 8; i++) n += snprintf(s + n, sizeof s - n, " %llu", first[i]);
            hlog(1, "gpu: samples passed: a submission with %u queries, %u reports, %u resets; reports:%s%s", o->nq,
                 reports, resets, s, reports > 8 ? " ..." : "");
        }
        o->nev = 0;                     /* (the pool is reset when the slot opens again) */
        fifo_head = (fifo_head + 1) % SLOTS;
        fifo_n--;
    }
    occ_waiting = (LONG)fifo_n;
    ReleaseSRWLockExclusive(&occ_lock);
}
