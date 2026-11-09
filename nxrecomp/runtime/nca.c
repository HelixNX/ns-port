/* The game straight from the user's own files (nca.h): NSPs (PFS0) of the base game, its update and DLC, firmware
 * NCAs for system data, decrypted as they are read with the user's keys (prod.keys; title keys from the NSPs'
 * tickets or title.keys). Nothing is extracted.
 *
 * NCA3 only: header AES-XTS (header_key, Nintendo's big-endian sector tweak), sections AES-CTR (counter: the
 * section's 8-byte value from its header, then the absolute NCA offset / 16, big-endian), the key from the key area
 * (key_area_key_<kind>_<gen>) or the ticket's title key (titlekek_<gen>). The update's RomFS is a patch (BKTR):
 * an indirect table maps its virtual section onto the base game's section (0) or its own (1), whose data is AES-CTR
 * with a per-range generation (the AES-CTR-EX table). The RomFS image is IVFC level 6 (the data) of the section.
 * Behaviour as documented on switchbrew; the implementation is ours. */
#include "nca.h"
#include "hos.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

/* ---------------------------------------------------------------------------------------------- keys and AES */
typedef struct { char name[64]; u8 v[32]; int n; } Key;
static Key *keys;
static int nkeys;
static struct { u8 rights[16], key[16]; } *tkeys;  /* title keys: rights id -> encrypted title key */
static int ntkeys;

static int hexval(int c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int unhex(const char *s, u8 *out, int max) {
    int n = 0;
    while (s[0] && s[1] && n < max) {
        int a = hexval(s[0]), b = hexval(s[1]);
        if (a < 0 || b < 0) break;
        out[n++] = (u8)(a << 4 | b);
        s += 2;
    }
    return n;
}

/* "name = hex" lines; title.keys has rights id = title key */
static bool load_keys(const char *path, bool title) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char ln[512];
    while (fgets(ln, sizeof ln, f)) {
        char *eq = strchr(ln, '=');
        if (!eq) continue;
        char name[128];
        int nn = 0;
        for (char *p = ln; p < eq && nn < 127; p++) if (*p != ' ' && *p != '\t') name[nn++] = (char)tolower(*p);
        name[nn] = 0;
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        if (title) {
            u8 r[16];
            if (unhex(name, r, 16) != 16) continue;
            tkeys = realloc(tkeys, (ntkeys + 1) * sizeof *tkeys);
            memcpy(tkeys[ntkeys].rights, r, 16);
            if (unhex(v, tkeys[ntkeys].key, 16) == 16) ntkeys++;
            continue;
        }
        keys = realloc(keys, (nkeys + 1) * sizeof *keys);
        Key *k = &keys[nkeys];
        snprintf(k->name, sizeof k->name, "%s", name);
        k->n = unhex(v, k->v, 32);
        if (k->n == 16 || k->n == 32) nkeys++;
    }
    fclose(f);
    return true;
}

static const u8 *key_get(const char *name, int n) {
    for (int i = 0; i < nkeys; i++)
        if (!strcmp(keys[i].name, name) && keys[i].n == n) return keys[i].v;
    return NULL;
}

static BCRYPT_ALG_HANDLE aes_alg;

static BCRYPT_KEY_HANDLE aes_key(const u8 *k) {
    if (!aes_alg) {
        if (BCryptOpenAlgorithmProvider(&aes_alg, BCRYPT_AES_ALGORITHM, NULL, 0) < 0) fatal("nca: no AES provider");
        BCryptSetProperty(aes_alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof BCRYPT_CHAIN_MODE_ECB, 0);
    }
    BCRYPT_KEY_HANDLE h;
    if (BCryptGenerateSymmetricKey(aes_alg, &h, NULL, 0, (PUCHAR)k, 16, 0) < 0) fatal("nca: AES key setup failed");
    return h;
}

static void aes_ecb(BCRYPT_KEY_HANDLE h, bool enc, const u8 *in, u8 *out, u32 n) {
    ULONG got;
    NTSTATUS s = enc ? BCryptEncrypt(h, (PUCHAR)in, n, NULL, NULL, 0, out, n, &got, 0)
                     : BCryptDecrypt(h, (PUCHAR)in, n, NULL, NULL, 0, out, n, &got, 0);
    if (s < 0) fatal("nca: AES failed (%lx)", (unsigned long)s);
}

static void aes_ecb_dec_key(const u8 *key, const u8 *in, u8 *out) {
    BCRYPT_KEY_HANDLE h = aes_key(key);
    aes_ecb(h, false, in, out, 16);
    BCryptDestroyKey(h);
}

/* AES-XTS decryption of 0x200-byte sectors from `sector` on, Nintendo's tweak (the sector number big-endian) */
static void xts_decrypt(const u8 *key32, u8 *buf, u32 size, u64 sector) {
    BCRYPT_KEY_HANDLE k1 = aes_key(key32), k2 = aes_key(key32 + 16);
    for (u32 s = 0; s < size; s += 0x200, sector++) {
        u8 tw[16] = {0}, t[16];
        for (int i = 0; i < 8; i++) tw[15 - i] = (u8)(sector >> (8 * i));
        aes_ecb(k2, true, tw, t, 16);
        for (u32 b = 0; b < 0x200; b += 16) {
            u8 x[16];
            for (int i = 0; i < 16; i++) x[i] = buf[s + b + i] ^ t[i];
            aes_ecb(k1, false, x, x, 16);
            for (int i = 0; i < 16; i++) buf[s + b + i] = x[i] ^ t[i];
            int carry = t[15] >> 7;                   /* t *= x in GF(2^128), little-endian */
            for (int i = 15; i > 0; i--) t[i] = (u8)(t[i] << 1 | t[i - 1] >> 7);
            t[0] = (u8)(t[0] << 1);
            if (carry) t[0] ^= 0x87;
        }
    }
    BCryptDestroyKey(k1);
    BCryptDestroyKey(k2);
}

/* the calling thread's own copy of a key: the game's threads read the RomFS at the same time, and one key object is
 * not used by several threads at once */
static BCRYPT_KEY_HANDLE key_for_thread(BCRYPT_KEY_HANDLE key) {
    static __declspec(thread) struct { BCRYPT_KEY_HANDLE orig, dup; } cache[16];
    static __declspec(thread) int ncache;
    for (int i = 0; i < ncache; i++)
        if (cache[i].orig == key) return cache[i].dup;
    BCRYPT_KEY_HANDLE d;
    if (BCryptDuplicateKey(key, &d, NULL, 0, 0) < 0) fatal("nca: AES key copy failed");
    if (ncache == 16) BCryptDestroyKey(cache[--ncache].dup);
    cache[ncache].orig = key;
    cache[ncache++].dup = d;
    return d;
}

/* XOR buf (bytes at absolute offset `abs`, 16-aligned) with the AES-CTR key stream: counter = hi8 | abs / 16 */
static void ctr_xor(BCRYPT_KEY_HANDLE key, const u8 hi8[8], u64 abs, u8 *buf, u64 size) {
    key = key_for_thread(key);
    u8 ctr[0x4000], ks[0x4000];
    u64 blk = abs >> 4;
    for (u64 done = 0; done < size;) {
        u32 n = size - done > sizeof ctr ? (u32)sizeof ctr : (u32)((size - done + 15) & ~15ull);
        for (u32 i = 0; i < n; i += 16, blk++) {
            memcpy(ctr + i, hi8, 8);
            for (int j = 0; j < 8; j++) ctr[i + 15 - j] = (u8)(blk >> (8 * j));
        }
        aes_ecb(key, true, ctr, ks, n);
        u64 m = size - done < n ? size - done : n;
        for (u64 i = 0; i < m; i++) buf[done + i] ^= ks[i];
        done += m;
    }
}

/* ---------------------------------------------------------------------------------------------- files */
typedef struct { HANDLE h; char path[MAX_PATH * 2]; } HostFile;

static void file_read(HostFile *f, u64 off, void *dst, u64 size) {
    u8 *out = dst;
    while (size) {
        OVERLAPPED ov = {0};
        ov.Offset = (DWORD)off;
        ov.OffsetHigh = (DWORD)(off >> 32);
        DWORD n = size > 0x10000000 ? 0x10000000 : (DWORD)size, got = 0;
        if (!ReadFile(f->h, out, n, &got, &ov) || !got) {
            void *tmp = GetLastError() == ERROR_NOACCESS ? malloc(n) : NULL;     /* NX_WATCH_MEM pages: see romfs.c */
            if (!tmp || !ReadFile(f->h, tmp, n, &got, &ov) || !got) fatal("nca: read error in %s", f->path);
            memcpy(out, tmp, got);
            free(tmp);
        }
        off += got; out += got; size -= got;
    }
}

/* ---------------------------------------------------------------------------------------------- NCAs */
typedef struct Entry { u64 off, end, phys; u32 idx; } Entry;   /* a bucket-tree entry: [off, end) */

typedef struct Section {
    u64 start, size;                    /* in the NCA */
    u8 fs_type, hash_type, enc;
    u8 ctr_hi[8];
    u64 data_off, data_size;            /* RomFS: IVFC level 6; PFS0: the PFS0 */
    u8 hdr[0x200];
    /* BKTR (enc 4) */
    Entry *rel, *sub;
    int nrel, nsub;
    u64 rel_size;
    struct Section *base;               /* the base game's RomFS section the patch reads from */
} Section;

typedef struct Nca {
    HostFile *file;
    u64 off;                            /* in the host file */
    u8 hdr[0xc00];
    u8 type, keygen;
    u64 program_id;
    BCRYPT_KEY_HANDLE key;
    Section sec[4];
    int nsec;
} Nca;

static Nca *ncas;
static int nncas;

/* the tickets found in the NSPs: rights id -> encrypted title key */
static void ticket_add(const u8 *t, u64 n) {
    if (n < 0x2b0) return;
    u32 sig = *(const u32 *)t;
    u64 body = sig == 0x10004 ? 0x140 : sig == 0x10003 ? 0x240 : sig == 0x10005 ? 0x80 : 0;
    if (!body || n < body + 0x170) return;
    tkeys = realloc(tkeys, (ntkeys + 1) * sizeof *tkeys);
    memcpy(tkeys[ntkeys].key, t + body + 0x40, 16);        /* title key block (common tickets: the key itself) */
    memcpy(tkeys[ntkeys].rights, t + body + 0x160, 16);
    ntkeys++;
}

static void section_read_raw(Nca *n, Section *s, u64 off, void *dst, u64 size);

static bool nca_key(Nca *n, char *err, size_t errn) {
    const u8 *h = n->hdr;
    int gen = h[0x206] > h[0x220] ? h[0x206] : h[0x220];
    if (gen) gen--;
    n->keygen = (u8)gen;
    u8 key[16];
    static const u8 zero[16];
    char name[64];
    if (memcmp(h + 0x230, zero, 16)) {               /* rights id: the title key */
        int i;
        for (i = 0; i < ntkeys && memcmp(tkeys[i].rights, h + 0x230, 16); i++) {}
        if (i == ntkeys) { snprintf(err, errn, "no ticket or title key for an NCA of %016llx", n->program_id); return false; }
        snprintf(name, sizeof name, "titlekek_%02x", gen);
        const u8 *kek = key_get(name, 16);
        if (!kek) { snprintf(err, errn, "prod.keys has no %s", name); return false; }
        aes_ecb_dec_key(kek, tkeys[i].key, key);
    } else {
        static const char *kind[] = {"application", "ocean", "system"};
        snprintf(name, sizeof name, "key_area_key_%s_%02x", kind[h[0x207] < 3 ? h[0x207] : 0], gen);
        const u8 *kak = key_get(name, 16);
        if (!kak) { snprintf(err, errn, "prod.keys has no %s", name); return false; }
        aes_ecb_dec_key(kak, h + 0x300 + 2 * 16, key);          /* key area slot 2: AES-CTR */
    }
    n->key = aes_key(key);
    return true;
}

/* a bucket tree (indirect: 0x14-byte entries {virtual, physical, storage}; AES-CTR-EX: 0x10 {offset, -, generation}) */
static int bucket_tree(const u8 *t, u64 size, u32 esz, Entry **out) {
    u32 nb = *(const u32 *)(t + 4);
    u64 total = *(const u64 *)(t + 8);              /* the end of the last entry */
    Entry *e = NULL;
    int n = 0, cap = 0;
    for (u32 b = 0; b < nb; b++) {
        u64 bo = 0x4000 + (u64)b * 0x4000;
        if (bo + 0x10 > size) break;
        u32 cnt = *(const u32 *)(t + bo + 4);
        for (u32 i = 0; i < cnt && bo + 0x10 + (i + 1) * esz <= size; i++) {
            const u8 *p = t + bo + 0x10 + i * esz;
            if (n == cap) e = realloc(e, (cap = cap * 2 + 256) * sizeof *e);
            e[n].off = *(const u64 *)p;
            if (esz == 0x14) { e[n].phys = *(const u64 *)(p + 8); e[n].idx = *(const u32 *)(p + 16); }
            else { e[n].phys = 0; e[n].idx = *(const u32 *)(p + 12); }
            n++;
        }
    }
    for (int i = 0; i < n; i++) e[i].end = i + 1 < n ? e[i + 1].off : total;
    *out = e;
    return n;
}

static const Entry *entry_find(const Entry *e, int n, u64 off) {
    int lo = 0, hi = n - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (e[mid].off <= off) lo = mid; else hi = mid - 1;
    }
    return n && e[lo].off <= off && off < e[lo].end ? &e[lo] : NULL;
}

static Nca *nca_of(Section *s);

/* the section's bytes [off, off+size) as stored (AES-CTR, or for a patch's data AES-CTR-EX), relative to it */
static void section_read_raw(Nca *n, Section *s, u64 off, void *dst, u64 size) {
    u8 *out = dst;
    while (size) {
        u64 a = off & ~15ull, lead = off - a;
        u64 chunk = size + lead > 0x100000 ? 0x100000 - lead : size;
        u8 hi[8];
        memcpy(hi, s->ctr_hi, 8);
        if (s->enc == 4 && s->nsub) {                 /* the generation of the AES-CTR-EX range */
            const Entry *e = entry_find(s->sub, s->nsub, a);
            if (e) {
                for (int j = 0; j < 4; j++) hi[7 - j] = (u8)(e->idx >> (8 * j));
                if (a + lead + chunk > e->end) chunk = e->end - a - lead;
            }
        }
        u64 m = (lead + chunk + 15) & ~15ull;
        u8 *tmp = malloc(m);
        file_read(n->file, n->off + s->start + a, tmp, m);
        if (s->enc == 3 || s->enc == 4) ctr_xor(n->key, hi, s->start + a, tmp, m);
        memcpy(out, tmp + lead, chunk);
        free(tmp);
        off += chunk; out += chunk; size -= chunk;
    }
}

/* the section as its file system sees it: a patch section through its indirect table */
static void section_read(Section *s, u64 off, void *dst, u64 size) {
    Nca *n = nca_of(s);
    if (!s->nrel) { section_read_raw(n, s, off, dst, size); return; }
    u8 *out = dst;
    while (size) {
        const Entry *e = entry_find(s->rel, s->nrel, off);
        if (!e) fatal("nca: patch has no mapping for offset %llx", off);
        u64 m = e->end - off < size ? e->end - off : size;
        u64 p = e->phys + (off - e->off);
        if (e->idx == 0) {
            if (!s->base) fatal("nca: the update needs the base game's NSP");
            section_read(s->base, p, out, m);
        } else section_read_raw(n, s, p, out, m);
        off += m; out += m; size -= m;
    }
}

static Nca *nca_of(Section *s) {
    for (int i = 0; i < nncas; i++)
        if (s >= ncas[i].sec && s < ncas[i].sec + 4) return &ncas[i];
    fatal("nca: section without an NCA");
}

static bool nca_open(HostFile *f, u64 off, char *err, size_t errn) {
    const u8 *hk = key_get("header_key", 32);
    if (!hk) { snprintf(err, errn, "prod.keys has no header_key"); return false; }
    Nca n = {.file = f, .off = off};
    file_read(f, off, n.hdr, 0xc00);
    xts_decrypt(hk, n.hdr, 0xc00, 0);
    if (memcmp(n.hdr + 0x200, "NCA3", 4)) { snprintf(err, errn, "%s: not an NCA3 (wrong keys?)", f->path); return false; }
    n.type = n.hdr[0x205];
    memcpy(&n.program_id, n.hdr + 0x210, 8);
    if (!nca_key(&n, err, errn)) return false;
    for (int i = 0; i < 4; i++) {
        u32 a, b;
        memcpy(&a, n.hdr + 0x240 + i * 0x10, 4);
        memcpy(&b, n.hdr + 0x244 + i * 0x10, 4);
        if (!b) continue;
        Section *s = &n.sec[i];
        n.nsec = i + 1;
        s->start = (u64)a * 0x200;
        s->size = (u64)(b - a) * 0x200;
        memcpy(s->hdr, n.hdr + 0x400 + i * 0x200, 0x200);
        s->fs_type = s->hdr[2];
        s->hash_type = s->hdr[3];
        s->enc = s->hdr[4];
        for (int j = 0; j < 8; j++) s->ctr_hi[j] = s->hdr[0x140 + 7 - j];
        if (s->hash_type == 3) {                     /* IVFC: level 6 is the RomFS */
            memcpy(&s->data_off, s->hdr + 0x08 + 0x10 + 5 * 0x18, 8);
            memcpy(&s->data_size, s->hdr + 0x08 + 0x10 + 5 * 0x18 + 8, 8);
        } else if (s->hash_type == 2) {              /* hierarchical SHA-256: the PFS0 region */
            memcpy(&s->data_off, s->hdr + 0x08 + 0x38, 8);
            memcpy(&s->data_size, s->hdr + 0x08 + 0x40, 8);
        }
        static const u8 zero[0x28];
        if (memcmp(s->hdr + 0x148, zero, 0x28) || memcmp(s->hdr + 0x178, zero, 0x28))
            hlog(1, "nca: %016llx section %d has sparse or compression info (not supported)", n.program_id, i);
    }
    ncas = realloc(ncas, (nncas + 1) * sizeof *ncas);
    ncas[nncas++] = n;
    return true;
}

/* the patch tables of a BKTR section (after every NCA is open: the base section is found by program id) */
static void bktr_setup(Nca *n, Section *s) {
    u64 ro, rs, so, ss;
    memcpy(&ro, s->hdr + 0x100, 8); memcpy(&rs, s->hdr + 0x108, 8);
    memcpy(&so, s->hdr + 0x120, 8); memcpy(&ss, s->hdr + 0x128, 8);
    if (memcmp(s->hdr + 0x110, "BKTR", 4) || memcmp(s->hdr + 0x130, "BKTR", 4)) fatal("nca: patch tables not found");
    /* the AES-CTR-EX table itself is plain AES-CTR; the indirect table is read through AES-CTR-EX */
    u8 *t = malloc(ss);
    u8 enc = s->enc;
    s->enc = 3;
    section_read_raw(n, s, so, t, ss);
    s->enc = enc;
    s->nsub = bucket_tree(t, ss, 0x10, &s->sub);
    free(t);
    t = malloc(rs);
    section_read_raw(n, s, ro, t, rs);
    s->rel_size = *(const u64 *)(t + 8);
    s->nrel = bucket_tree(t, rs, 0x14, &s->rel);
    free(t);
    for (int i = 0; i < nncas; i++) {               /* the base game's RomFS: same program, not a patch */
        Nca *b = &ncas[i];
        if (b == n || b->program_id != n->program_id || b->type != 0) continue;
        for (int j = 0; j < b->nsec; j++)
            if (b->sec[j].hash_type == 3 && b->sec[j].enc != 4) s->base = &b->sec[j];
    }
    hlog(1, "nca: update RomFS: %d mappings, %d AES-CTR-EX ranges, base %s", s->nrel, s->nsub, s->base ? "found" : "MISSING");
}

/* ---------------------------------------------------------------------------------------------- storages */
struct NxStorage { Section *sec; u64 off, size; };

u64 nx_storage_size(NxStorage *s) { return s->size; }

bool nx_storage_read(NxStorage *s, u64 off, void *dst, u64 size) {
    if (off + size > s->size || off + size < off) return false;
    section_read(s->sec, s->off + off, dst, size);
    return true;
}

static NxStorage *storage_new(Section *s) {
    NxStorage *st = malloc(sizeof *st);
    st->sec = s; st->off = s->data_off; st->size = s->data_size;
    return st;
}

/* ---------------------------------------------------------------------------------------------- PFS0 (NSP, ExeFS) */
typedef struct { char name[256]; u64 off, size; } PfsFile;

/* the files of a PFS0 read by `rd` from offset `base`; returns the count (n allocated in *out) */
typedef void (*ReadFn)(void *ctx, u64 off, void *dst, u64 size);
static int pfs0_list(ReadFn rd, void *ctx, u64 base, PfsFile **out) {
    u8 h[16];
    rd(ctx, base, h, 16);
    if (memcmp(h, "PFS0", 4)) return -1;
    u32 nf = *(u32 *)(h + 4), strsz = *(u32 *)(h + 8);
    u64 tab = 16 + (u64)nf * 0x18;
    u8 *t = malloc(tab + strsz);
    rd(ctx, base, t, tab + strsz);
    PfsFile *f = calloc(nf ? nf : 1, sizeof *f);
    for (u32 i = 0; i < nf; i++) {
        const u8 *e = t + 16 + i * 0x18;
        u32 so = *(const u32 *)(e + 16);
        f[i].off = base + tab + strsz + *(const u64 *)e;
        f[i].size = *(const u64 *)(e + 8);
        snprintf(f[i].name, sizeof f[i].name, "%s", so < strsz ? (const char *)t + tab + so : "");
    }
    free(t);
    *out = f;
    return (int)nf;
}

static void host_rd(void *ctx, u64 off, void *dst, u64 size) { file_read(ctx, off, dst, size); }
static void sec_rd(void *ctx, u64 off, void *dst, u64 size) { section_read(ctx, off, dst, size); }

/* ---------------------------------------------------------------------------------------------- the game */
static bool has_ext(const char *name, const char *ext) {
    size_t a = strlen(name), b = strlen(ext);
    return a >= b && !_stricmp(name + a - b, ext);
}

static HostFile *host_open(const char *path) {
    HANDLE h = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    HostFile *f = calloc(1, sizeof *f);
    f->h = h;
    snprintf(f->path, sizeof f->path, "%s", path);
    return f;
}

typedef struct { char path[MAX_PATH * 2]; } Found;

static int find_files(const char *dir, const char *ext, Found **out, int n) {
    char pat[MAX_PATH * 2];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return n;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!has_ext(fd.cFileName, ext)) continue;
        *out = realloc(*out, (n + 1) * sizeof **out);
        snprintf((*out)[n++].path, sizeof (*out)[n - 1].path, "%s\\%s", dir, fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return n;
}

bool nx_game_present(const char *dir) {
    Found *f = NULL;
    int n = find_files(dir, ".nsp", &f, 0);
    free(f);
    return n > 0;
}

static Section *romfs_sec;          /* the program's RomFS: the update's patch if there is one */
static Section *exefs_sec;          /* the newest program's ExeFS */
static PfsFile *exefs_files;
static int nexefs;

bool nx_game_open(const char *dir, char *err, size_t errn) {
    char path[MAX_PATH * 2];
    snprintf(path, sizeof path, "%s\\prod.keys", dir);
    bool k = load_keys(path, false);
    snprintf(path, sizeof path, "%s\\title.keys", dir);
    load_keys(path, true);
    const char *home = getenv("USERPROFILE");
    if (home) {                                      /* where the usual dumping tools put them */
        snprintf(path, sizeof path, "%s\\.switch\\prod.keys", home);
        if (!k) k = load_keys(path, false);
        snprintf(path, sizeof path, "%s\\.switch\\title.keys", home);
        load_keys(path, true);
    }
    if (!k) { snprintf(err, errn, "No prod.keys in %s (or %%USERPROFILE%%\\.switch).", dir); return false; }
    /* tickets first: NCAs with a rights id need them */
    Found *nsps = NULL;
    int nn = find_files(dir, ".nsp", &nsps, 0);
    if (!nn) { snprintf(err, errn, "No .nsp files in %s.", dir); return false; }
    HostFile **hf = calloc(nn, sizeof *hf);
    PfsFile **pf = calloc(nn, sizeof *pf);
    int *np = calloc(nn, sizeof *np);
    for (int i = 0; i < nn; i++) {
        if (!(hf[i] = host_open(nsps[i].path))) { snprintf(err, errn, "Cannot open %s.", nsps[i].path); return false; }
        np[i] = pfs0_list(host_rd, hf[i], 0, &pf[i]);
        if (np[i] < 0) { snprintf(err, errn, "%s is not an NSP.", nsps[i].path); return false; }
        for (int j = 0; j < np[i]; j++)
            if (has_ext(pf[i][j].name, ".tik") && pf[i][j].size < 0x1000) {
                u8 t[0x1000];
                file_read(hf[i], pf[i][j].off, t, pf[i][j].size);
                ticket_add(t, pf[i][j].size);
            }
    }
    for (int i = 0; i < nn; i++)
        for (int j = 0; j < np[i]; j++)
            if (has_ext(pf[i][j].name, ".nca") && !has_ext(pf[i][j].name, ".cnmt.nca")) {
                char e[300];
                if (!nca_open(hf[i], pf[i][j].off, e, sizeof e)) {
                    snprintf(err, errn, "%s: %s", nsps[i].path, e);
                    return false;
                }
            }
    /* firmware NCAs (system data), headers only until used */
    snprintf(path, sizeof path, "%s\\firmware", dir);
    Found *fw = NULL;
    int nf = find_files(path, ".nca", &fw, 0);
    for (int i = 0; i < nf; i++) {
        if (has_ext(fw[i].path, ".cnmt.nca")) continue;
        HostFile *f = host_open(fw[i].path);
        char e[300];
        if (f && !nca_open(f, 0, e, sizeof e)) hlog(1, "nca: firmware %s: %s", fw[i].path, e);
    }
    free(fw);
    hlog(1, "nca: %d NSPs, %d NCAs, %d firmware NCAs, %d title keys", nn, nncas, nf, ntkeys);
    /* the program: its RomFS (the update's patch over the base's) and ExeFS (the update's, else the base's) */
    for (int i = 0; i < nncas; i++) {
        Nca *n = &ncas[i];
        if (n->type != 0) continue;
        for (int j = 0; j < n->nsec; j++) {
            Section *s = &n->sec[j];
            if (s->hash_type == 3 && s->enc == 4) { bktr_setup(n, s); romfs_sec = s; }
            else if (s->hash_type == 3 && !romfs_sec) romfs_sec = s;
            else if (s->fs_type == 1 && s->hash_type == 2) {
                /* the update's ExeFS wins: the program NCA whose RomFS is a patch */
                bool patch = false;
                for (int k = 0; k < n->nsec; k++) patch |= n->sec[k].enc == 4;
                if (patch || !exefs_sec) exefs_sec = s;
            }
        }
    }
    if (!romfs_sec || !exefs_sec) { snprintf(err, errn, "No Pokemon Violet program found in the NSPs in %s.", dir); return false; }
    if (romfs_sec->enc == 4 && !romfs_sec->base) {
        snprintf(err, errn, "The update was found, but not the base game's NSP (put both in %s).", dir);
        return false;
    }
    nexefs = pfs0_list(sec_rd, exefs_sec, exefs_sec->data_off, &exefs_files);
    if (nexefs < 0) { snprintf(err, errn, "The program's ExeFS could not be read (wrong keys?)."); return false; }
    return true;
}

NxStorage *nx_game_romfs(void) {
    static NxStorage *s;
    if (!s && romfs_sec) {
        s = storage_new(romfs_sec);
        if (romfs_sec->enc == 4 && s->off + s->size > romfs_sec->rel_size) fatal("nca: patched RomFS past its mapping");
    }
    return s;
}

void *nx_game_exefs_file(const char *name, u64 *size) {
    for (int i = 0; i < nexefs; i++)
        if (!strcmp(exefs_files[i].name, name)) {
            void *p = malloc(exefs_files[i].size ? exefs_files[i].size : 1);
            section_read(exefs_sec, exefs_files[i].off, p, exefs_files[i].size);
            *size = exefs_files[i].size;
            return p;
        }
    return NULL;
}

/* a file of the control data (control.nacp, icon_AmericanEnglish.dat): the update's NSP's, else the base game's */
void *nx_game_control_file(const char *name, u64 *size) {
    Nca *best = NULL;
    for (int i = 0; i < nncas; i++) {
        Nca *n = &ncas[i];
        if (n->type != 2 || !n->nsec || n->sec[0].hash_type != 3) continue;
        if (!best || (romfs_sec && romfs_sec->enc == 4 && n->file == nca_of(romfs_sec)->file)) best = n;
    }
    if (!best) return NULL;
    NxStorage st = {&best->sec[0], best->sec[0].data_off, best->sec[0].data_size};
    char path[300];
    snprintf(path, sizeof path, "/%s", name);
    u64 off, sz;
    if (!nx_romfs_find(&st, path, &off, &sz)) return NULL;
    void *p = malloc(sz ? sz : 1);
    nx_storage_read(&st, off, p, sz);
    *size = sz;
    return p;
}

NxStorage *nx_data_romfs(u64 id) {
    /* add-on content (public data, type 5) or system data (data, type 4) with that id */
    for (int i = 0; i < nncas; i++) {
        Nca *n = &ncas[i];
        if (n->program_id != id || (n->type != 4 && n->type != 5)) continue;
        for (int j = 0; j < n->nsec; j++)
            if (n->sec[j].hash_type == 3) return storage_new(&n->sec[j]);
    }
    return NULL;
}

/* ---------------------------------------------------------------------------------------------- RomFS lookup */
bool nx_romfs_find(NxStorage *rs, const char *path, u64 *off, u64 *size) {
    u64 h[10];                  /* header size, dir hash, dir meta, file hash, file meta (offset, size), data offset */
    if (!nx_storage_read(rs, 0, h, sizeof h)) return false;
    u64 dmeta = h[3], dsize = h[4], fmeta = h[7], fsize = h[8], data = h[9];
    u8 *dt = malloc(dsize), *ft = malloc(fsize);
    nx_storage_read(rs, dmeta, dt, dsize);
    nx_storage_read(rs, fmeta, ft, fsize);
    u32 dir = 0;                                    /* root */
    bool found = false;
    while (*path == '/') path++;
    for (;;) {
        const char *slash = strchr(path, '/');
        size_t len = slash ? (size_t)(slash - path) : strlen(path);
        if (slash) {                                /* a directory: search the children of dir */
            u32 c = *(u32 *)(dt + dir + 8);
            while (c != 0xffffffff) {
                u32 nl = *(u32 *)(dt + c + 0x14);
                if (nl == len && !memcmp(dt + c + 0x18, path, len)) break;
                c = *(u32 *)(dt + c + 4);
            }
            if (c == 0xffffffff) break;
            dir = c;
            path = slash + 1;
            continue;
        }
        u32 f = *(u32 *)(dt + dir + 12);
        while (f != 0xffffffff) {
            u32 nl = *(u32 *)(ft + f + 0x1c);
            if (nl == len && !memcmp(ft + f + 0x20, path, len)) {
                *off = data + *(u64 *)(ft + f + 8);
                *size = *(u64 *)(ft + f + 0x10);
                found = true;
                break;
            }
            f = *(u32 *)(ft + f + 4);
        }
        break;
    }
    free(dt);
    free(ft);
    return found;
}
