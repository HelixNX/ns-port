/* NX_TRACE=<first frame>[,<frames>] (default 20 frames): a timeline of those frames, for finding what a frame waits
 * on (the CPU report and NX_GPU_TIMELINE are averages). Written a few frames after the last one to
 * <build>\trace-<first frame>.json in the Chrome trace format (ui.perfetto.dev, chrome://tracing;
 * tools/trace_summary.py prints it per frame). Tracks:
 *   guest threads: each thread's state (running on a core, waiting for a core, blocked: sync / mutex / condvar /
 *     arbiter / sleep, with the guest callers), and under it the runtime's work on that thread (translating and
 *     submitting, presenting, waiting for the GPU, waiting for a slot, each vkQueueSubmit call), which runs without
 *     the core;
 *   host threads: the same runtime work on threads that are no guest's;
 *   GPU: each submission from its first command to its last (NX_GPU_TIMELINE's timestamps, turned on with this),
 *     on the CPU's clock (lined up by the smallest submit -> start);
 *   a marker at the end of each present.
 * Events go into one array (an atomic index); spans begun before the first frame are cut at it, and those ending
 * after the last are dropped. */
#include "hos.h"
#include "trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define TR_FRAME TR_KINDS       /* a present's end (a0: the frame) */

typedef struct {
    u64 q0, q1, a0, a1;
    const KThread *t;           /* NULL: the host thread host_tid */
    u32 host_tid;
    u8 kind, runtime;
    u64 chain[TR_CHAIN];        /* blocked: the guest's callers */
    const KThread *waker;       /* blocked: who ended the wait (a1: when) */
} TrEv;

typedef struct { double submit, start, end; bool first; } TrGpu;

volatile int trace_live;
static int state;               /* 0 waiting, 1 recording, 2 waiting for the GPU's timestamps, 3 written */
static u64 first_frame, nframes = 20, end_frame;
static u64 t_start, t_end, qf;
static TrEv *evs;
static volatile LONG64 nev;
static u64 cap;
static TrGpu *gpus;
static u32 ngpu, gpu_cap;

bool trace_enabled(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("NX_TRACE");
        on = 0;
        if (e && *e) {
            first_frame = strtoull(e, NULL, 10);
            const char *c = strchr(e, ',');
            if (c) nframes = strtoull(c + 1, NULL, 10);
            if (!nframes) nframes = 1;
            on = first_frame > 0;
        }
    }
    return on;
}

u64 trace_qpc(void) { LARGE_INTEGER q; QueryPerformanceCounter(&q); return (u64)q.QuadPart; }

void trace_span(const void *thread, bool runtime, int kind, u64 q0, u64 q1, u64 a0, u64 a1) {
    if (!trace_live) return;
    LONG64 i = InterlockedIncrement64(&nev) - 1;
    if ((u64)i >= cap) return;
    evs[i] = (TrEv){q0, q1, a0, a1, thread, thread ? 0 : GetCurrentThreadId(), (u8)kind, runtime};
}

void trace_block(const void *thread, int kind, u64 q0, u64 q1, const u64 *chain, const void *waker, u64 wake_q) {
    if (!trace_live) return;
    LONG64 i = InterlockedIncrement64(&nev) - 1;
    if ((u64)i >= cap) return;
    TrEv *e = &evs[i];
    *e = (TrEv){q0, q1, 0, wake_q, thread, 0, (u8)kind, 0};
    memcpy(e->chain, chain, sizeof e->chain);
    e->waker = waker;
}

bool trace_wants_gpu(void) { return state == 1 || state == 2; }

void trace_gpu(double submit_ms, double start_gpu_ms, double end_gpu_ms, bool first) {
    double s0 = (double)t_start * 1000.0 / (double)qf, s1 = state == 1 ? 1e300 : (double)t_end * 1000.0 / (double)qf;
    if (submit_ms < s0 || submit_ms > s1) return;
    if (ngpu == gpu_cap) gpus = realloc(gpus, (gpu_cap = gpu_cap * 2 + 1024) * sizeof *gpus);
    gpus[ngpu++] = (TrGpu){submit_ms, start_gpu_ms, end_gpu_ms, first};
}

static const char *kind_name[TR_KINDS + 1] = {
    "running", "waiting for a core", "blocked: sync", "blocked: mutex", "blocked: condvar", "blocked: arbiter",
    "sleep", "translate + submit", "present", "wait for GPU", "wait for a slot", "vkQueueSubmit", "frame"};

static double us_of(u64 q) { return q < t_start ? 0 : (double)(q - t_start) * 1e6 / (double)qf; }

/* a guest thread's two tracks: tid 2 * id (state) and 2 * id + 1 (runtime); host threads in pid 2 */
static void ev_track(const TrEv *e, int *pid, u64 *tid) {
    if (e->t) { *pid = 1; *tid = e->t->id * 2 + e->runtime; }
    else { *pid = 2; *tid = e->host_tid; }
}

static void write_trace(void) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%s\\trace-%llu.json", nx_data_dir(), (unsigned long long)first_frame);
    FILE *f = fopen(path, "wb");
    if (!f) { hlog(0, "trace: cannot write %s", path); return; }
    static char buf[1 << 20];
    setvbuf(f, buf, _IOFBF, sizeof buf);
    u64 n = (u64)nev < cap ? (u64)nev : cap;
    fprintf(f, "{\"displayTimeUnit\":\"ms\",\"otherData\":{\"first_frame\":%llu,\"frames\":%llu,\"dropped\":%llu},"
               "\"traceEvents\":[\n", (unsigned long long)first_frame, (unsigned long long)nframes,
            (unsigned long long)((u64)nev - n));
    fprintf(f, "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":1,\"args\":{\"name\":\"guest threads\"}},\n"
               "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":2,\"args\":{\"name\":\"host threads\"}},\n"
               "{\"name\":\"process_name\",\"ph\":\"M\",\"pid\":3,\"args\":{\"name\":\"GPU\"}},\n"
               "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":3,\"tid\":1,\"args\":{\"name\":\"queue\"}},\n");
    /* track names: each thread seen once (a small open table of pointers / host ids) */
    static u64 seen[4096];
    for (u64 i = 0; i < n; i++) {
        const TrEv *e = &evs[i];
        if (e->kind == TR_FRAME) continue;
        int pid;
        u64 tid;
        ev_track(e, &pid, &tid);
        u64 key = (u64)pid << 56 | tid, h = key * 0x9e3779b97f4a7c15ull >> 52;
        while (seen[h] && seen[h] != key + 1) h = (h + 1) & 4095;
        if (seen[h]) continue;
        seen[h] = key + 1;
        if (e->t)
            fprintf(f, "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":1,\"tid\":%llu,\"args\":{\"name\":\"%s%s\"}},\n"
                       "{\"name\":\"thread_sort_index\",\"ph\":\"M\",\"pid\":1,\"tid\":%llu,\"args\":{\"sort_index\":%llu}},\n",
                    (unsigned long long)tid, e->t->name, e->runtime ? " (runtime)" : "", (unsigned long long)tid,
                    (unsigned long long)tid);
        else
            fprintf(f, "{\"name\":\"thread_name\",\"ph\":\"M\",\"pid\":2,\"tid\":%llu,\"args\":{\"name\":\"host %llu\"}},\n",
                    (unsigned long long)tid, (unsigned long long)tid);
    }
    for (u64 i = 0; i < n; i++) {
        const TrEv *e = &evs[i];
        if (e->q1 < t_start || e->q1 > t_end || e->q1 < e->q0) continue;
        double ts = us_of(e->q0), dur = us_of(e->q1) - ts;
        if (e->kind == TR_FRAME) {
            fprintf(f, "{\"name\":\"frame %llu\",\"ph\":\"i\",\"s\":\"g\",\"pid\":1,\"tid\":0,\"ts\":%.3f},\n",
                    (unsigned long long)e->a0, ts);
            continue;
        }
        int pid;
        u64 tid;
        ev_track(e, &pid, &tid);
        fprintf(f, "{\"name\":\"%s\",\"ph\":\"X\",\"pid\":%d,\"tid\":%llu,\"ts\":%.3f,\"dur\":%.3f", kind_name[e->kind], pid,
                (unsigned long long)tid, ts, dur);
        if (e->kind == TR_RUN && e->a1)
            fprintf(f, ",\"args\":{\"core\":%llu,\"host cpu %%\":%.1f}", (unsigned long long)e->a0, (double)e->a1 / 10);
        else if (e->kind == TR_RUN) fprintf(f, ",\"args\":{\"core\":%llu}", (unsigned long long)e->a0);
        else if (e->kind == TR_FLUSH || e->kind == TR_SUBMIT || e->kind == TR_PRESENT)
            fprintf(f, ",\"args\":{\"%s\":%llu}", e->kind == TR_FLUSH ? "value" : e->kind == TR_SUBMIT ? "submission" : "frame",
                    (unsigned long long)e->a0);
        else if (e->kind == TR_GPU_WAIT)
            fprintf(f, ",\"args\":{\"value\":%llu,\"behind newest\":%llu}", (unsigned long long)e->a0,
                    (unsigned long long)e->a1);
        else if (e->kind >= TR_SYNC && e->kind <= TR_SLEEP) {
            /* the chain as module+offset (symbols left out: the summary groups by address), who woke it, how long
             * from the wake to the thread running again on the host (before it waits for a core) */
            fputs(",\"args\":{\"chain\":\"", f);
            for (int k = 0; k < TR_CHAIN && e->chain[k]; k++) {
                char b[160];
                describe_addr(e->chain[k], b, sizeof b);
                char *sym = strchr(b, ' ');
                if (sym) *sym = 0;
                fprintf(f, "%s%s", k ? " < " : "", b);
            }
            fputc('"', f);
            if (e->waker) fprintf(f, ",\"woken by\":\"%s\"", e->waker->name);
            if (e->a1 && e->a1 <= e->q1) fprintf(f, ",\"wake us\":%.1f", (double)(e->q1 - e->a1) * 1e6 / (double)qf);
            fputc('}', f);
        }
        fputs("},\n", f);
    }
    /* the GPU's clock lined up with the CPU's: no submission starts before it is submitted */
    double off = 1e300, s0 = (double)t_start * 1000.0 / (double)qf, s1 = (double)t_end * 1000.0 / (double)qf;
    for (u32 i = 0; i < ngpu; i++) if (gpus[i].start - gpus[i].submit < off) off = gpus[i].start - gpus[i].submit;
    for (u32 i = 0; i < ngpu; i++) {
        double gs = gpus[i].start - off, ge = gpus[i].end - off;
        if (ge < s0 || gs > s1) continue;
        fprintf(f, "{\"name\":\"%s\",\"ph\":\"X\",\"pid\":3,\"tid\":1,\"ts\":%.3f,\"dur\":%.3f,"
                   "\"args\":{\"submitted\":%.3f}},\n", gpus[i].first ? "submission (first flush)" : "submission",
                (gs - s0) * 1000.0, (ge - gs) * 1000.0, (gpus[i].submit - s0) * 1000.0);
    }
    fputs("{\"name\":\"end\",\"ph\":\"i\",\"s\":\"g\",\"pid\":1,\"tid\":0,\"ts\":0}\n]}\n", f);
    fclose(f);
    hlog(0, "trace: frames %llu-%llu written to %s (%llu events, %llu dropped, %u GPU submissions)",
         (unsigned long long)first_frame, (unsigned long long)end_frame, path, (unsigned long long)n,
         (unsigned long long)((u64)nev - n), ngpu);
    free(evs);
    free(gpus);
    evs = NULL;
    gpus = NULL;
}

void trace_present(u64 frame) {
    if (!trace_enabled() || state == 3) return;
    u64 now = trace_qpc();
    if (state == 0) {
        if (frame < first_frame) return;
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qf = (u64)f.QuadPart;
        cap = 1u << 20;
        evs = malloc(cap * sizeof *evs);
        if (!evs) { state = 3; return; }
        first_frame = frame;
        t_start = now;
        state = 1;
        hlog(0, "trace: recording frames %llu-%llu", (unsigned long long)frame, (unsigned long long)(frame + nframes));
        trace_live = 1;
    }
    if (state == 1) {
        trace_span(NULL, false, TR_FRAME, now, now, frame, 0);
        if (frame >= first_frame + nframes) {
            trace_live = 0;
            t_end = now;
            end_frame = frame;
            state = 2;
        }
        return;
    }
    if (state == 2 && frame >= end_frame + 16) {    /* every slot of the last frames reused: their timestamps read */
        write_trace();
        state = 3;
    }
}
