/* Memory pools: each one the GPU can reach is a Vulkan buffer over guest memory (gpu_buffer, gpu_cpu), and on
 * GPUs that read host memory slowly a device-local mirror kept in step with the pages the CPU wrote (mirror_sync).
 * Part of the NVN executor (gpu.c, gpu_int.h). */
#include "gpu_int.h"

#define MIRROR_MIN (8ull << 20)         /* smaller pools are not worth a scan per submission */
double mirror_mb, mirror_ms;            /* copied in and time scanning, for the report every 600 frames */
int nmirrors;
PoolEnt *pools;
int npools, cappools;
u64 pools_gen;                          /* bumped when pools come or go (cached device addresses) */
u64 bad_gen;
u64 ro_room;                            /* gpu_buffer_ro's last answer: the bytes from there to its pool's end */

PoolEnt *pool_find(u64 addr) {
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
bool gpu_buffer(u64 addr, VkBuffer *buf, u64 *off) {
    PoolEnt *p = pool_find(addr);
    if (!p || !p->hb) return false;
    *buf = p->hb->buf;
    *off = addr - p->gpu;
    return true;
}

/* a GPU address as the device address shaders reach it by (0: none) */
u64 gpu_device_addr(u64 addr) {
    PoolEnt *p = pool_find(addr);
    return p && p->hb && p->hb->addr ? p->hb->addr + (addr - p->gpu) : 0;
}

/* a GPU address as a CPU pointer to guest memory, if it has one */
u8 *gpu_cpu(u64 addr, u64 size) {
    PoolEnt *p = pool_find(addr);
    if (!p || !p->cpu || addr + size > p->end) return NULL;
    return (u8 *)(uintptr_t)(p->cpu + (addr - p->gpu));
}

u64 mirror_bytes;                       /* device memory the mirrors hold */

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
    mirror_bytes += m->b.a.size;
    m->fresh = true;                    /* copied whole at the next submission */
    return m;
}

void mirror_free(void *p) {
    Mirror *m = p;
    mirror_bytes -= m->b.a.size;
    vk_buffer_free(&m->b);
    free(m->bad);
    free(m);
}

/* the mirror's buffer and offset for a GPU address, if its pool has one */
VkBuffer mirror_at(u64 addr, u64 *off) {
    PoolEnt *p = addr ? pool_find(addr) : NULL;
    if (!p || !p->mir) return NULL;
    *off = addr - p->gpu;
    return p->mir->b.buf;
}

/* a range the GPU writes through host memory (storage buffers, transform feedback): read from there from now on */
void mirror_bad(u64 addr, u64 size) {
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
    bad_gen++;
    hlog(2, "gpu: pool mirror: %llx+%llx is written by the GPU (%d ranges)", lo, hi - lo, m->nbad);
}

/* the storage buffers a stage's shader may write (it uses global memory) */
void mirror_bad_ssbos(const BufferRange *ssbo, const ShaderCode *sc) {
    if (!nmirrors || !sc || !(sc->caps & SHC_GLOBAL)) return;
    for (u32 k = 0; k < 16; k++)
        if (ssbo[k].addr) mirror_bad(ssbo[k].addr, ssbo[k].size);
}

/* a range draws only read (vertex, index or uniform data; size 0: to the pool's end) as (buffer, offset): from the
 * pool's mirror when it has one and the GPU writes none of the range */
bool gpu_buffer_ro(u64 addr, u64 size, VkBuffer *buf, u64 *off) {
    /* asked ~15 times a draw, mostly for the ranges of the draws before: the answers are kept while the pools and
     * the mirrors' written ranges stay as they are (the pool search and the scan of those ranges were 5% of the
     * render thread) */
    static struct { u64 addr, size, pools_gen, bad_gen, off, room; VkBuffer buf; } memo[256];
    u32 slot = (u32)((addr >> 6) ^ (addr >> 14) ^ (addr >> 22) ^ size) & 255;
    if (memo[slot].buf && memo[slot].addr == addr && memo[slot].size == size && memo[slot].pools_gen == pools_gen &&
        memo[slot].bad_gen == bad_gen) {
        *buf = memo[slot].buf;
        *off = memo[slot].off;
        ro_room = memo[slot].room;
        return true;
    }
    PoolEnt *p = pool_find(addr);
    if (!p || !p->hb) return false;
    *buf = p->hb->buf;
    *off = addr - p->gpu;
    memo[slot].addr = addr, memo[slot].size = size, memo[slot].pools_gen = pools_gen, memo[slot].bad_gen = bad_gen;
    memo[slot].off = *off;
    memo[slot].room = ro_room = p->end - addr;
    Mirror *m = p->mir;
    if (m) {
        u64 hi = size && size < p->end - addr ? addr + size : p->end;
        bool bad = false;
        for (int i = 0; i < m->nbad && !bad; i++) bad = m->bad[i].lo < hi && addr < m->bad[i].hi;
        if (!bad) *buf = m->b.buf;
    }
    memo[slot].buf = *buf;
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
            if (pools[i].cpu)
                occ_forget((const u8 *)(uintptr_t)pools[i].cpu, (const u8 *)(uintptr_t)(pools[i].cpu + p->size));
            memmove(&pools[i], &pools[i + 1], (npools - i - 1) * sizeof *pools);
            npools--;
            pools_gen++;
            break;
        }
    images_drop_range(p->gpu, p->gpu + p->size);
    p->hb = NULL;
    LeaveCriticalSection(&gpu_cs);
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

void mirror_scan_start(void) {
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

/* the copies into the mirrors, recorded into the slot's pre command buffer (pre_cmd), which runs before the
 * submission's commands and after the work submitted before it (what that read or wrote is done): the pages the
 * last scan found, and new mirrors whole. Images on those pages are uploaded again here: the write watch over them
 * is the scan's */
void mirror_sync(void) {
    if (!nmirrors) return;
    static VkBufferCopy *rg;
    static size_t caprg;
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
        for (size_t k = 0; k < nr; k++) mirror_mb += (double)rg[k].size / (1 << 20);
        vkCmdCopyBuffer(pre_cmd(), p->hb->buf, m->b.buf, (u32)nr, rg);
    }
    ms_running = false;
    mirror_ms += ms_now() - t0;
    hf.mirror += ms_now() - t0;
}
