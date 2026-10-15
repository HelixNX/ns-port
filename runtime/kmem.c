/* Memory objects. Transfer memory is a range of the guest's own memory lent to a service: the in-process service
 * simply uses it in place. Shared memory is created by a service and mapped by the guest at an address of its
 * choosing; until then the service writes to a host buffer, which is copied in when the guest maps it (and back
 * when it unmaps it), so shmem_ptr always gives where the contents are now. */
#include "hos.h"
#include <stdlib.h>
#include <windows.h>

static void tmem_destroy(KObj *o) { free(o); }

u32 tmem_create(u64 addr, u64 size, u32 perm, u32 *handle) {
    if ((addr | size) & 0xfff || !size) return R_INVALID_ADDRESS;
    KTmem *t = calloc(1, sizeof *t);
    kobj_init(&t->obj, KO_TMEM, tmem_destroy, NULL);
    t->addr = addr;
    t->size = size;
    t->perm = perm;
    u32 r = handle_new(&t->obj, handle);
    kobj_unref(&t->obj);
    return r;
}

static void shmem_destroy(KObj *o) {
    KShmem *s = (KShmem *)o;
    VirtualFree(s->staging, 0, MEM_RELEASE);
    free(s);
}

KShmem *shmem_create(u64 size, const char *name) {
    KShmem *s = calloc(1, sizeof *s);
    kobj_init(&s->obj, KO_SHMEM, shmem_destroy, NULL);
    s->size = (size + 0xfff) & ~0xfffull;
    s->staging = VirtualAlloc(NULL, s->size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    snprintf(s->name, sizeof s->name, "%s", name);
    return s;
}

void *shmem_ptr(KShmem *s) {
    u64 a = __atomic_load_n(&s->addr, __ATOMIC_ACQUIRE);
    return a ? (void *)(uintptr_t)a : s->staging;
}

u32 shmem_map(u32 handle, u64 addr, u64 size, u32 perm) {
    KShmem *s = (KShmem *)handle_get(handle, KO_SHMEM);
    if (!s) return R_INVALID_HANDLE;
    u32 r = R_OK;
    if ((addr & 0xfff) || size != s->size) r = R_INVALID_SIZE;
    else if (s->addr) r = R_INVALID_STATE;
    else if (!mem_is_free(addr, size)) r = R_INVALID_MEMSTATE;
    else {
        mem_map(addr, size, MS_SHARED, perm & MP_RW);
        memcpy((void *)(uintptr_t)addr, s->staging, size);
        __atomic_store_n(&s->addr, addr, __ATOMIC_RELEASE);
        hlog(1, "shared memory %s mapped at %llx (%llx bytes)", s->name, addr, size);
    }
    kobj_unref(&s->obj);
    return r;
}

u32 shmem_unmap(u32 handle, u64 addr, u64 size) {
    KShmem *s = (KShmem *)handle_get(handle, KO_SHMEM);
    if (!s) return R_INVALID_HANDLE;
    u32 r = R_OK;
    if (s->addr != addr || size != s->size) r = R_INVALID_MEMSTATE;
    else {
        memcpy(s->staging, (void *)(uintptr_t)addr, size);
        __atomic_store_n(&s->addr, 0, __ATOMIC_RELEASE);
        mem_unmap(addr, size);
    }
    kobj_unref(&s->obj);
    return r;
}
