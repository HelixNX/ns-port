/* NX_TRACE=<first frame>[,<frames>]: a timeline of a few frames (trace.c) */
#pragma once
#include <stdbool.h>
#include <stdint.h>

enum {
    TR_RUN,                     /* holding a core (args: the core, the host thread's share of CPU in per mille) */
    TR_READY,                   /* waiting for a core */
    TR_SYNC, TR_MUTEX, TR_CONDVAR, TR_ARBITER, TR_SLEEP,    /* blocked (W_SYNC .. W_SLEEP; args: guest callers) */
    TR_SUBMIT,                  /* runtime: gpu_submit (translating, flushes) */
    TR_PRESENT,                 /* runtime: gpu_present */
    TR_GPU_WAIT,                /* runtime: gpu_wait (the guest waits for the GPU) */
    TR_SLOT_WAIT,               /* runtime: a submission slot's last use not done yet */
    TR_FLUSH,                   /* runtime: one vkQueueSubmit call (arg: its timeline value) */
    TR_KINDS
};

extern volatile int trace_live;     /* recording: the hooks below cost a branch otherwise */

/* qpc ticks; a guest thread (KThread *, its state track, or its runtime track if runtime) or the host thread */
void trace_span(const void *thread, bool runtime, int kind, uint64_t q0, uint64_t q1, uint64_t a0, uint64_t a1);
uint64_t trace_qpc(void);
#define TR_CHAIN 6
/* a blocked span: the guest's call chain, the thread that ended the wait and when (0: a timeout or a host thread) */
void trace_block(const void *thread, int kind, uint64_t q0, uint64_t q1, const uint64_t *chain, const void *waker,
                 uint64_t wake_q);
void trace_gpu(double submit_ms, double start_gpu_ms, double end_gpu_ms, bool first);   /* gpu_prof.c, ms_now's clock */
bool trace_wants_gpu(void);         /* the GPU timestamps are needed (also after the frames, until written) */
void trace_present(uint64_t frame);   /* at each present's end */
bool trace_enabled(void);           /* NX_TRACE set (read once) */
