/* Guest memory: the whole guest-visible address space AS_BASE..AS_END is reserved from Windows once, so pages can
 * be committed and decommitted at 4 KB granularity wherever the guest maps them. A sorted list of blocks that
 * covers all 64-bit addresses records each range's state, permission and attributes for svcQueryMemory.
 *
 * svcMapMemory (thread stacks: heap memory seen at a second address) copies instead of aliasing: while mapped,
 * the source has no access rights on the console, so nobody can observe that the two are not the same pages. */
#include "hos.h"
#include <stdlib.h>
#include <windows.h>

typedef struct Block { u64 addr, size; u32 state, perm, attr; } Block;
static Block *blocks;
static int nblocks, capblocks;
static SRWLOCK memlock = SRWLOCK_INIT;
static u64 heap_size, used_bytes;

#define PAGE 0x1000ull

static DWORD win_prot(u32 perm) {
    if (perm == MP_NONE) return PAGE_NOACCESS;
    if (perm & MP_W) return PAGE_READWRITE;
    return PAGE_READONLY;
}

/* index of the block containing addr */
static int find(u64 addr) {
    int lo = 0, hi = nblocks - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (blocks[mid].addr <= addr) lo = mid; else hi = mid - 1;
    }
    return lo;
}

static void insert_at(int i, Block b) {
    if (nblocks == capblocks) {
        capblocks = capblocks ? capblocks * 2 : 256;
        blocks = realloc(blocks, capblocks * sizeof *blocks);
    }
    memmove(&blocks[i + 1], &blocks[i], (nblocks - i) * sizeof *blocks);
    blocks[i] = b;
    nblocks++;
}

static void remove_at(int i) {
    memmove(&blocks[i], &blocks[i + 1], (nblocks - i - 1) * sizeof *blocks);
    nblocks--;
}

/* make a block boundary at addr */
static void split(u64 addr) {
    int i = find(addr);
    Block *b = &blocks[i];
    if (b->addr == addr) return;
    Block nb = *b;
    nb.addr = addr;
    nb.size = b->addr + b->size - addr;   /* wraps correctly for the last block (size measured to 2^64) */
    b->size = addr - b->addr;
    insert_at(i + 1, nb);
}

static bool same(const Block *a, const Block *b) {
    return a->state == b->state && a->perm == b->perm && a->attr == b->attr;
}

static void set_locked(u64 addr, u64 size, u32 state, u32 perm, u32 attr) {
    split(addr);
    if (addr + size) split(addr + size);
    int i = find(addr);
    while (i < nblocks && blocks[i].addr < addr + size && blocks[i].addr >= addr) {
        blocks[i].state = state; blocks[i].perm = perm; blocks[i].attr = attr;
        i++;
    }
    /* merge equal neighbours */
    for (int j = 1; j < nblocks; j++) {
        if (same(&blocks[j - 1], &blocks[j])) {
            blocks[j - 1].size += blocks[j].size;
            remove_at(j);
            j--;
        }
    }
}

void mem_set_block(u64 addr, u64 size, u32 state, u32 perm) {
    AcquireSRWLockExclusive(&memlock);
    set_locked(addr, size, state, perm, 0);
    ReleaseSRWLockExclusive(&memlock);
}

void mem_init(void) {
    insert_at(0, (Block){0, AS_BASE, MS_INACCESSIBLE, MP_NONE, 0});
    insert_at(1, (Block){AS_BASE, AS_END - AS_BASE, MS_FREE, MP_NONE, 0});
    insert_at(2, (Block){AS_END, 0 - AS_END, MS_INACCESSIBLE, MP_NONE, 0});
    void *p = VirtualAlloc((void *)(uintptr_t)AS_BASE, AS_END - AS_BASE, MEM_RESERVE, PAGE_NOACCESS);
    if (p != (void *)(uintptr_t)AS_BASE)
        fatal("cannot reserve the guest address space %llx-%llx (error %lu)", AS_BASE, AS_END, GetLastError());
    /* never handed out: the loader's page and the host-call addresses */
    mem_set_block(HOSTCALL_BASE, 0x10000, MS_INACCESSIBLE, MP_NONE);
}

static void commit(u64 addr, u64 size, u32 perm) {
    if (!VirtualAlloc((void *)(uintptr_t)addr, size, MEM_COMMIT, PAGE_READWRITE))
        fatal("commit %llx+%llx failed (error %lu)", addr, size, GetLastError());
    if (perm != MP_RW) {
        DWORD old;
        VirtualProtect((void *)(uintptr_t)addr, size, win_prot(perm), &old);
    }
}

void mem_map(u64 addr, u64 size, u32 state, u32 perm) {
    if (addr < AS_BASE || addr + size > AS_END || (addr | size) & (PAGE - 1))
        fatal("mem_map %llx+%llx outside the guest address space", addr, size);
    commit(addr, size, perm);
    AcquireSRWLockExclusive(&memlock);
    set_locked(addr, size, state, perm, 0);
    used_bytes += size;
    ReleaseSRWLockExclusive(&memlock);
}

void mem_unmap(u64 addr, u64 size) {
    VirtualFree((void *)(uintptr_t)addr, size, MEM_DECOMMIT);
    AcquireSRWLockExclusive(&memlock);
    set_locked(addr, size, MS_FREE, MP_NONE, 0);
    used_bytes -= size;
    ReleaseSRWLockExclusive(&memlock);
}

bool mem_query(u64 addr, MemInfo *out) {
    AcquireSRWLockShared(&memlock);
    Block *b = &blocks[find(addr)];
    memset(out, 0, sizeof *out);
    out->addr = b->addr; out->size = b->size; out->state = b->state; out->perm = b->perm; out->attr = b->attr;
    ReleaseSRWLockShared(&memlock);
    return true;
}

/* is [addr, addr+size) one block with this state (and, if perm_mask, permissions perm under the mask)? */
static bool range_is(u64 addr, u64 size, u32 state, u32 perm_mask, u32 perm, u32 attr_mask, u32 attr) {
    if (!size || addr + size < addr) return false;
    u64 a = addr;
    while (a < addr + size) {
        Block *b = &blocks[find(a)];
        if (b->state != state || (b->perm & perm_mask) != perm || (b->attr & attr_mask) != attr) return false;
        a = b->addr + b->size;
        if (!a) break;
    }
    return true;
}

bool mem_is_free(u64 addr, u64 size) {
    AcquireSRWLockShared(&memlock);
    bool r = range_is(addr, size, MS_FREE, 0, 0, 0, 0);
    ReleaseSRWLockShared(&memlock);
    return r;
}

u64 mem_heap_size(void) { return heap_size; }
u64 mem_used(void) { return used_bytes; }

u32 mem_set_heap_size(u64 size, u64 *addr) {
    if (size & 0x1fffff || size > HEAP_MAX) return R_INVALID_SIZE;
    if (size > heap_size) {
        commit(HEAP_BASE + heap_size, size - heap_size, MP_RW);
        AcquireSRWLockExclusive(&memlock);
        set_locked(HEAP_BASE + heap_size, size - heap_size, MS_HEAP, MP_RW, 0);
        used_bytes += size - heap_size;
        ReleaseSRWLockExclusive(&memlock);
    } else if (size < heap_size) {
        mem_unmap(HEAP_BASE + size, heap_size - size);
    }
    heap_size = size;
    *addr = HEAP_BASE;
    return R_OK;
}

#define ATTR_LOCKED 1

u32 mem_map_memory(u64 dst, u64 src, u64 size) {
    if ((dst | src | size) & (PAGE - 1) || !size) return R_INVALID_ADDRESS;
    if (dst < STACK_BASE || dst + size > STACK_BASE + STACK_SIZE) return R_INVALID_RANGE;
    AcquireSRWLockExclusive(&memlock);
    if (!range_is(src, size, MS_HEAP, MP_RW, MP_RW, ATTR_LOCKED, 0) || !range_is(dst, size, MS_FREE, 0, 0, 0, 0)) {
        ReleaseSRWLockExclusive(&memlock);
        return R_INVALID_MEMSTATE;
    }
    commit(dst, size, MP_RW);
    memcpy((void *)(uintptr_t)dst, (void *)(uintptr_t)src, size);
    DWORD old;
    VirtualProtect((void *)(uintptr_t)src, size, PAGE_NOACCESS, &old);
    set_locked(src, size, MS_HEAP, MP_NONE, ATTR_LOCKED);
    set_locked(dst, size, MS_STACK, MP_RW, 0);
    ReleaseSRWLockExclusive(&memlock);
    return R_OK;
}

u32 mem_unmap_memory(u64 dst, u64 src, u64 size) {
    if ((dst | src | size) & (PAGE - 1) || !size) return R_INVALID_ADDRESS;
    AcquireSRWLockExclusive(&memlock);
    if (!range_is(src, size, MS_HEAP, 0, 0, ATTR_LOCKED, ATTR_LOCKED) || !range_is(dst, size, MS_STACK, 0, 0, 0, 0)) {
        ReleaseSRWLockExclusive(&memlock);
        return R_INVALID_MEMSTATE;
    }
    DWORD old;
    VirtualProtect((void *)(uintptr_t)src, size, PAGE_READWRITE, &old);
    memcpy((void *)(uintptr_t)src, (void *)(uintptr_t)dst, size);
    VirtualFree((void *)(uintptr_t)dst, size, MEM_DECOMMIT);
    set_locked(src, size, MS_HEAP, MP_RW, 0);
    set_locked(dst, size, MS_FREE, MP_NONE, 0);
    ReleaseSRWLockExclusive(&memlock);
    return R_OK;
}

u32 mem_set_perm(u64 addr, u64 size, u32 perm) {
    if ((addr | size) & (PAGE - 1) || !size) return R_INVALID_ADDRESS;
    if (perm & MP_X || perm == MP_W) return R_INVALID_PERM;
    AcquireSRWLockExclusive(&memlock);
    Block *b = &blocks[find(addr)];
    u32 state = b->state;
    if (state != MS_HEAP && state != MS_CODEDATA && state != MS_ALIAS) {
        ReleaseSRWLockExclusive(&memlock);
        return R_INVALID_MEMSTATE;
    }
    DWORD old;
    VirtualProtect((void *)(uintptr_t)addr, size, win_prot(perm), &old);
    set_locked(addr, size, state, perm, b->attr);
    ReleaseSRWLockExclusive(&memlock);
    return R_OK;
}

u32 mem_set_attr(u64 addr, u64 size, u32 mask, u32 value) {
    if ((addr | size) & (PAGE - 1) || !size) return R_INVALID_ADDRESS;
    AcquireSRWLockExclusive(&memlock);
    Block *b = &blocks[find(addr)];
    set_locked(addr, size, b->state, b->perm, (b->attr & ~mask) | (value & mask));
    ReleaseSRWLockExclusive(&memlock);
    return R_OK;
}

void mem_protect(u64 addr, u64 size, u32 perm) {
    DWORD old;
    if (!VirtualProtect((void *)(uintptr_t)addr, size, win_prot(perm), &old))
        fatal("protect %llx+%llx failed (error %lu)", addr, size, GetLastError());
}
