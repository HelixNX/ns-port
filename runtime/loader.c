/* The loader: maps the module images that tools/lifter/a64image.py wrote (already relocated for their fixed
 * addresses), and gives the sdk what rtld would: pointers to rtld's module lists and lookup function. */
#include "hos.h"
#include <stdlib.h>

typedef struct ImgHeader {
    u32 magic, version;
    u64 base, text_end, ro_start, ro_end, data_start, data_file_end, bss_end, dt_init, dt_fini, reserved;
    u32 nsym, strsz;
} ImgHeader;

#define IMG_MAGIC 0x4d494b4du
static const char *const mod_names[] = {"main", "subsdk0", "sdk"};      /* load order = lookup order */

LoadedMod loaded_mods[3];
int num_loaded_mods;

static void *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) fatal("cannot open %s", path);
    _fseeki64(f, 0, SEEK_END);
    size_t n = (size_t)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    void *p = malloc(n);
    if (fread(p, 1, n, f) != n) fatal("cannot read %s", path);
    fclose(f);
    *size = n;
    return p;
}

static void load_one(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.img", dir, name);
    size_t size;
    u8 *file = read_file(path, &size);
    ImgHeader *h = (ImgHeader *)file;
    if (h->magic != IMG_MAGIC || h->version != 1) fatal("%s: not an image from a64image.py", path);
    LoadedMod *m = &loaded_mods[num_loaded_mods++];
    m->name = name;
    m->base = h->base; m->text_end = h->text_end; m->ro_start = h->ro_start; m->ro_end = h->ro_end;
    m->data_start = h->data_start; m->data_end = h->data_file_end; m->bss_end = h->bss_end;
    m->dt_init = h->dt_init; m->dt_fini = h->dt_fini;
    m->nsym = h->nsym;
    m->syms = (const ImgSym *)(h + 1);
    m->strtab = (const char *)(m->syms + h->nsym);
    const u8 *img = (const u8 *)m->strtab + h->strsz;
    u64 img_size = h->data_file_end - h->base;
    if ((u64)(file + size - img) != img_size) fatal("%s: truncated", path);
    u64 page = 0xfff;
    u64 text_end = (h->text_end + page) & ~page, ro_end = (h->ro_end + page) & ~page;
    u64 bss_end = (h->bss_end + page) & ~page;
    /* commit everything writable, copy, then drop write access to code and read-only data */
    mem_map(h->base, bss_end - h->base, MS_CODEDATA, MP_RW);
    memcpy((void *)(uintptr_t)h->base, img, img_size);
    mem_set_block(h->base, text_end - h->base, MS_CODE, MP_RX);
    mem_set_block(h->ro_start, ro_end - h->ro_start, MS_CODE, MP_R);
    mem_protect(h->base, ro_end - h->base, MP_R);
    hlog(1, "loaded %-8s at %llx (text %llx, data %llx-%llx)", name, h->base, h->text_end - h->base, h->data_start,
         h->bss_end);
}

u64 sym_lookup(const char *name) {
    for (int i = 0; i < num_loaded_mods; i++) {
        LoadedMod *m = &loaded_mods[i];
        for (u32 k = 0; k < m->nsym; k++)
            if (!strcmp(m->strtab + m->syms[k].name, name)) return m->syms[k].addr;
    }
    return 0;
}

const char *sym_name(u64 addr, u64 *offset) {
    for (int i = 0; i < num_loaded_mods; i++) {
        LoadedMod *m = &loaded_mods[i];
        if (addr < m->base || addr >= m->bss_end || !m->nsym) continue;
        u32 lo = 0, hi = m->nsym;                        /* last symbol at or below addr */
        while (hi - lo > 1) {
            u32 mid = (lo + hi) / 2;
            if (m->syms[mid].addr <= addr) lo = mid; else hi = mid;
        }
        if (m->syms[lo].addr > addr) return NULL;
        *offset = addr - m->syms[lo].addr;
        return m->strtab + m->syms[lo].name;
    }
    return NULL;
}

/* rtld's nn::ro hooks: LookupGlobalAuto(name) -> address of the first definition in load order */
static void h_lookup_global_auto(Ctx *c) {
    const char *name = (const char *)(uintptr_t)c->x[0];
    c->x[0] = sym_lookup(name);
    hlog(1, "rtld lookup %s -> %llx", name, c->x[0]);
}

static void set_global(const char *sym, u64 value) {
    u64 a = sym_lookup(sym);
    if (!a) { hlog(1, "loader: no %s", sym); return; }
    ST64(a, value);
}

void loader_load(const char *dir) {
    for (int i = 0; i < 3; i++) load_one(dir, mod_names[i]);
    /* rtld's data the sdk points at: two empty module lists (no ModuleObjects: nothing loads NROs), the debug flag
     * and the manual-lookup slot that nn::ro fills in */
    mem_map(MISC_BASE, 0x1000, MS_CODEDATA, MP_RW);
    u64 auto_list = MISC_BASE, manual_list = MISC_BASE + 0x10, debug_flag = MISC_BASE + 0x20, manual_fn = MISC_BASE + 0x28;
    ST64(auto_list, auto_list); ST64(auto_list + 8, auto_list);
    ST64(manual_list, manual_list); ST64(manual_list + 8, manual_list);
    set_global("_ZN2nn2ro6detail15g_pAutoLoadListE", auto_list);
    set_global("_ZN2nn2ro6detail17g_pManualLoadListE", manual_list);
    set_global("_ZN2nn2ro6detail14g_pRoDebugFlagE", debug_flag);
    set_global("_ZN2nn2ro6detail34g_pLookupGlobalAutoFunctionPointerE",
               hostcall_register("rtld LookupGlobalAuto", h_lookup_global_auto));
    set_global("_ZN2nn2ro6detail36g_pLookupGlobalManualFunctionPointerE", manual_fn);
}
