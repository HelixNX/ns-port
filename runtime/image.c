/* Module images from the game's own NSOs (the ExeFS in the user's NSPs), as tools/lifter/a64image.py writes them
 * from a dump: segments decompressed (LZ4), every relocation applied for the fixed bases, exported symbols listed.
 * Written to <dir>/<module>.img for loader.c the first time the game starts from NSPs. Same rules as a64image.py
 * (and nso2elf.py / nxmod.py): byte-identical output. */
#include "hos.h"
#include "nca.h"
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#define IMG_MAGIC 0x4d494b4du
extern const ModInfo lifted_modules[];

/* LZ4 block format */
static bool lz4_block(const u8 *src, u32 n, u8 *dst, u32 cap) {
    const u8 *s = src, *end = src + n;
    u8 *d = dst, *dend = dst + cap;
    while (s < end) {
        u32 tok = *s++, lit = tok >> 4;
        if (lit == 15) { u8 b; do { if (s >= end) return false; b = *s++; lit += b; } while (b == 255); }
        if (s + lit > end || d + lit > dend) return false;
        memcpy(d, s, lit); d += lit; s += lit;
        if (s >= end) break;                        /* the last sequence has literals only */
        if (s + 2 > end) return false;
        u32 off = s[0] | s[1] << 8;
        s += 2;
        u32 len = (tok & 15) + 4;
        if ((tok & 15) == 15) { u8 b; do { if (s >= end) return false; b = *s++; len += b; } while (b == 255); }
        if (!off || d - dst < (ptrdiff_t)off || d + len > dend) return false;
        for (u32 i = 0; i < len; i++, d++) *d = d[-(ptrdiff_t)off];    /* overlapping copies byte by byte */
    }
    return d == dend;
}

typedef struct { const char *name; u64 value, size; u8 info; u16 shndx; } Sym;
typedef struct { u64 off; u32 type, sym; s64 add; } Rel;

typedef struct Mod {
    const ModInfo *mi;
    u8 *mem;
    u64 tv, tsz, rv, rsz, dv, dsz, dmsz;            /* segments: text, ro, data (memory size with bss) */
    u64 tags[32];                                   /* DT_* < 32: the first value of each */
    bool has[32];
    Sym *syms;
    u32 nsym;
    Rel *rels;
    u32 nrel;
} Mod;

static bool mod_load(Mod *m, const ModInfo *mi, char *err, size_t errn) {
    m->mi = mi;
    u64 n;
    u8 *f = nx_game_exefs_file(mi->name, &n);
    if (!f || n < 0x100 || memcmp(f, "NSO0", 4)) { snprintf(err, errn, "the game's ExeFS has no %s module", mi->name); return false; }
    u32 flags = *(u32 *)(f + 12);
    u32 hdr[3][3], csz[3];
    for (int i = 0; i < 3; i++) { memcpy(hdr[i], f + 0x10 + i * 0x10, 12); csz[i] = *(u32 *)(f + 0x60 + i * 4); }
    u32 bss = *(u32 *)(f + 0x3c);
    u64 dv = hdr[2][1], dsz = hdr[2][2];
    m->mem = calloc(1, dv + dsz);
    for (int i = 0; i < 3; i++) {
        u32 foff = hdr[i][0], moff = hdr[i][1], size = hdr[i][2];
        if ((u64)foff + csz[i] > n) { snprintf(err, errn, "%s: truncated", mi->name); return false; }
        if (flags & (1u << i)) {
            if (!lz4_block(f + foff, csz[i], m->mem + moff, size)) { snprintf(err, errn, "%s: bad LZ4 data", mi->name); return false; }
        } else memcpy(m->mem + moff, f + foff, size);
    }
    free(f);
    m->tv = hdr[0][1]; m->tsz = hdr[0][2];
    m->rv = hdr[1][1]; m->rsz = hdr[1][2];
    m->dv = dv; m->dsz = dsz;
    u8 *mem = m->mem;
    u32 mod0 = *(u32 *)(mem + 4);
    if (memcmp(mem + mod0, "MOD0", 4)) { snprintf(err, errn, "%s: no MOD0", mi->name); return false; }
    s32 dyn_off = *(s32 *)(mem + mod0 + 4), bss_e = *(s32 *)(mem + mod0 + 12);
    u64 dyn = mod0 + dyn_off;
    /* nso2elf.py: data's memory size (its MOD0 bss end taken as an address, as there) */
    u64 msz = dsz;
    if ((s64)bss_e - (s64)dv > (s64)msz) msz = (u64)((s64)bss_e - (s64)dv);
    if (dsz + bss > msz) msz = dsz + bss;
    m->dmsz = msz;
    for (u64 p = dyn;; p += 16) {
        s64 t = *(s64 *)(mem + p);
        u64 v = *(u64 *)(mem + p + 8);
        if (!t) break;
        if (t >= 0 && t < 32 && !m->has[t]) { m->has[t] = true; m->tags[t] = v; }
    }
    u64 symtab = m->tags[6], strtab = m->tags[5];
    m->nsym = m->has[4] ? *(u32 *)(mem + m->tags[4] + 4) : (u32)((strtab - symtab) / 24);
    m->syms = calloc(m->nsym, sizeof *m->syms);
    for (u32 i = 0; i < m->nsym; i++) {
        const u8 *s = mem + symtab + i * 24;
        Sym *y = &m->syms[i];
        y->name = (const char *)mem + strtab + *(u32 *)s;
        y->info = s[4];
        y->shndx = *(u16 *)(s + 6);
        y->value = *(u64 *)(s + 8);
        y->size = *(u64 *)(s + 16);
    }
    static const int pairs[2][2] = {{7, 8}, {23, 2}};          /* RELA / RELASZ, JMPREL / PLTRELSZ */
    for (int k = 0; k < 2; k++) {
        if (!m->has[pairs[k][0]]) continue;
        u64 off = m->tags[pairs[k][0]], sz = m->tags[pairs[k][1]];
        m->rels = realloc(m->rels, (m->nrel + sz / 24) * sizeof *m->rels);
        for (u64 i = 0; i < sz / 24; i++) {
            const u8 *r = mem + off + i * 24;
            Rel *x = &m->rels[m->nrel++];
            x->off = *(u64 *)r;
            u64 info = *(u64 *)(r + 8);
            x->type = (u32)info;
            x->sym = (u32)(info >> 32);
            x->add = *(s64 *)(r + 16);
        }
    }
    return true;
}

/* a64lift's symbol map: the first module (load order) whose exports (object, function, no type) define it */
typedef struct { const char *name; u64 addr; } Def;
static Def *defs;
static u32 ndefs;

static int def_cmp(const void *a, const void *b) { return strcmp(((const Def *)a)->name, ((const Def *)b)->name); }

typedef struct { const char *name; u64 value; u32 mod, idx; } CandT;
static int cand_cmp(const void *a, const void *b) {
    const CandT *x = a, *y = b;
    int c = strcmp(x->name, y->name);
    if (c) return c;
    if (x->mod != y->mod) return x->mod < y->mod ? -1 : 1;
    return x->idx < y->idx ? -1 : x->idx > y->idx;
}

static const Def *def_find(const char *name) {
    Def k = {name, 0};
    return bsearch(&k, defs, ndefs, sizeof *defs, def_cmp);
}

typedef struct { u64 addr; u32 name; u32 size; const char *s; } OutSym;

static int outsym_cmp(const void *a, const void *b) {
    const OutSym *x = a, *y = b;
    if (x->addr != y->addr) return x->addr < y->addr ? -1 : 1;
    int c = strcmp(x->s, y->s);
    if (c) return c;
    return x->size < y->size ? -1 : x->size > y->size;
}

static bool build(Mod *m, const char *dir, char *err, size_t errn) {
    u64 base = m->mi->base, file_end = m->dv + m->dsz;
    u8 *img = m->mem;
    for (u32 i = 0; i < m->nrel; i++) {
        Rel *r = &m->rels[i];
        u64 val;
        if (r->type == 1027) val = base + (u64)r->add;
        else if (r->type == 257 || r->type == 1025 || r->type == 1026) {
            Sym *y = &m->syms[r->sym];
            const Def *d;
            if (y->shndx && y->info >> 4 == 0) val = base + y->value + (u64)r->add;
            else if ((d = def_find(y->name)) != NULL) val = d->addr + (u64)r->add;
            else {
                if (y->info >> 4 != 2) { snprintf(err, errn, "%s: strong symbol %s is unresolved", m->mi->name, y->name); return false; }
                val = (u64)r->add;
            }
        } else { snprintf(err, errn, "%s: relocation type %u", m->mi->name, r->type); return false; }
        memcpy(img + r->off, &val, 8);
    }
    /* exported symbols (non-zero), sorted by address, name, size */
    OutSym *os = malloc((m->nsym + 1) * sizeof *os);
    u32 no = 0;
    for (u32 i = 1; i < m->nsym; i++) {
        Sym *y = &m->syms[i];
        if (!y->shndx || !y->name[0] || !y->value) continue;
        os[no].addr = base + y->value;
        os[no].s = y->name;
        os[no].size = y->size > 0xffffffffull ? 0xffffffffu : (u32)y->size;
        no++;
    }
    qsort(os, no, sizeof *os, outsym_cmp);
    size_t strsz = 0;
    for (u32 i = 0; i < no; i++) { os[i].name = (u32)strsz; strsz += strlen(os[i].s) + 1; }
    size_t strpad = (strsz + 7) & ~7ull;
    struct {
        u32 magic, version;
        u64 base, text_end, ro_start, ro_end, data_start, data_file_end, bss_end, dt_init, dt_fini, reserved;
        u32 nsym, strsz;
    } h = {IMG_MAGIC, 1, base, base + m->tv + m->tsz, base + m->rv, base + m->rv + m->rsz, base + m->dv,
           base + file_end, base + m->dv + m->dmsz, m->has[12] && m->tags[12] ? base + m->tags[12] : 0,
           m->has[13] && m->tags[13] ? base + m->tags[13] : 0, 0, no, (u32)strpad};
    char path[MAX_PATH * 2], tmp[MAX_PATH * 2 + 8];
    snprintf(path, sizeof path, "%s\\%s.img", dir, m->mi->name);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { snprintf(err, errn, "cannot write %s", tmp); return false; }
    fwrite(&h, sizeof h, 1, f);
    for (u32 i = 0; i < no; i++) {
        struct { u64 addr; u32 name, size; } e = {os[i].addr, os[i].name, os[i].size};
        fwrite(&e, sizeof e, 1, f);
    }
    for (u32 i = 0; i < no; i++) fwrite(os[i].s, 1, strlen(os[i].s) + 1, f);
    static const u8 zero[8];
    fwrite(zero, 1, strpad - strsz, f);
    fwrite(img, 1, file_end, f);
    bool ok = !ferror(f);
    fclose(f);
    free(os);
    if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING)) { snprintf(err, errn, "cannot write %s", path); return false; }
    /* the lifted code was made from these modules: a different game version does not fit it */
    if (h.text_end - base != m->mi->text_end - m->mi->base) {
        snprintf(err, errn, "%s is not the version this build was made for (Pokemon Violet 4.0.0 is needed)", m->mi->name);
        return false;
    }
    return true;
}

/* <dir>\<module>.img for every module, unless all are there already */
bool image_build(const char *dir, char *err, size_t errn) {
    char path[MAX_PATH * 2];
    int n = 0, have = 0;
    for (const ModInfo *mi = lifted_modules; mi->name; mi++, n++) {
        snprintf(path, sizeof path, "%s\\%s.img", dir, mi->name);
        have += GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    }
    if (have == n) return true;
    hlog(0, "image: building the module images in %s", dir);
    Mod *mods = calloc(n, sizeof *mods);
    for (int i = 0; i < n; i++) {
        if (!mod_load(&mods[i], &lifted_modules[i], err, errn)) return false;
        hlog(2, "image: %s: %u symbols, %u relocations", mods[i].mi->name, mods[i].nsym, mods[i].nrel);
    }
    /* a64lift's sym_addr: each module's exports (object, function, no type) as a dict (a name's last value wins),
     * modules in load order, the first module whose value is non-zero defines the name */
    u32 nc = 0;
    for (int i = 0; i < n; i++) nc += mods[i].nsym;
    CandT *c = malloc((nc + 1) * sizeof *c);
    nc = 0;
    for (int i = 0; i < n; i++)
        for (u32 k = 1; k < mods[i].nsym; k++) {
            Sym *y = &mods[i].syms[k];
            u8 t = y->info & 0xf;
            if (!y->shndx || !y->name[0] || (t != 0 && t != 1 && t != 2)) continue;
            c[nc++] = (CandT){y->name, y->value, (u32)i, k};
        }
    qsort(c, nc, sizeof *c, cand_cmp);
    defs = malloc((nc + 1) * sizeof *defs);
    for (u32 a = 0; a < nc;) {
        u32 b = a;
        while (b < nc && !strcmp(c[b].name, c[a].name)) b++;
        for (u32 j = a; j < b;) {                   /* per module (ascending), its last entry's value */
            u32 e = j;
            while (e + 1 < b && c[e + 1].mod == c[j].mod) e++;
            if (c[e].value) { defs[ndefs++] = (Def){c[a].name, mods[c[e].mod].mi->base + c[e].value}; break; }
            j = e + 1;
        }
        a = b;
    }
    free(c);
    for (int i = 0; i < n; i++)
        if (!build(&mods[i], dir, err, errn)) return false;
    hlog(0, "image: %d module images written", n);
    return true;
}
