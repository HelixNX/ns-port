/* nxshpack: every translated shader in one file for the runtime (gpu.c spv_read), so a player's package needs no
 * translator (Python, glslang) and no 3 GB of loose files.
 *
 * Usage: nxshpack <spv dir> <out.pak> [zstd level, default 12]
 *   <spv dir>: translate.py --spv's output folder for one translator version (spv\<version>, the version being
 *   FNV-1a 32 of translate.py, as the folder is named): <hash>.spv and <hash>_<mask>.spv (integer-input variants).
 *
 * Format (little-endian): header {"NXSP", u32 version 1, u32 translator version, u32 count}, then count entries
 * {u64 hash, u32 mask, u32 size, u64 offset, u32 packed size, u32 0} sorted by hash then mask, then the data: one
 * zstd frame per shader at its offset (from the file start). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <zstd.h>

typedef struct { uint64_t hash; uint32_t mask, size; uint64_t offset; uint32_t packed, pad; } Ent;
typedef struct { uint64_t hash; uint32_t mask; char name[64]; } Item;

static int cmp(const void *a, const void *b) {
    const Item *x = a, *y = b;
    if (x->hash != y->hash) return x->hash < y->hash ? -1 : 1;
    return x->mask < y->mask ? -1 : x->mask > y->mask;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: nxshpack <spv dir> <out.pak> [zstd level]\n"); return 2; }
    const char *dir = argv[1];
    int level = argc > 3 ? atoi(argv[3]) : 12;
    size_t dl = strlen(dir);
    while (dl && (dir[dl - 1] == '\\' || dir[dl - 1] == '/')) dl--;
    const char *base = dir + dl;
    while (base > dir && base[-1] != '\\' && base[-1] != '/') base--;
    char vers[16] = {0};
    memcpy(vers, base, (size_t)(dir + dl - base) < 15 ? (size_t)(dir + dl - base) : 15);
    char *end;
    uint32_t translator = (uint32_t)strtoul(vers, &end, 16);
    if (end != vers + 8) { fprintf(stderr, "nxshpack: %s is not a spv\\<translator version> folder\n", dir); return 1; }

    char pat[MAX_PATH + 16];
    snprintf(pat, sizeof pat, "%.*s\\*.spv", (int)dl, dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { fprintf(stderr, "nxshpack: no .spv files in %s\n", dir); return 1; }
    Item *items = NULL;
    uint32_t n = 0, cap = 0;
    do {
        Item it = {0};
        unsigned long long hv;
        unsigned mv = 0;
        char tail[8];
        if (strlen(fd.cFileName) >= sizeof it.name) continue;
        if (sscanf(fd.cFileName, "%16llx_%8x%4s", &hv, &mv, tail) == 3 && strlen(fd.cFileName) == 29 && !strcmp(tail, ".spv")) {}
        else if (strlen(fd.cFileName) == 20 && sscanf(fd.cFileName, "%16llx", &hv) == 1) mv = 0;
        else continue;
        it.hash = hv;
        it.mask = mv;
        strcpy(it.name, fd.cFileName);
        if (n == cap) items = realloc(items, (cap = cap * 2 + 1024) * sizeof *items);
        items[n++] = it;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    qsort(items, n, sizeof *items, cmp);

    FILE *out = fopen(argv[2], "wb");
    if (!out) { fprintf(stderr, "nxshpack: cannot write %s\n", argv[2]); return 1; }
    uint32_t hdr[4] = {0x5053584e /* "NXSP" */, 1, translator, n};
    Ent *ents = calloc(n ? n : 1, sizeof *ents);
    fwrite(hdr, sizeof hdr, 1, out);
    fwrite(ents, sizeof *ents, n, out);                       /* filled in at the end */
    uint64_t off = sizeof hdr + (uint64_t)n * sizeof *ents, raw = 0;
    ZSTD_CCtx *cc = ZSTD_createCCtx();
    void *src = NULL, *dst = NULL;
    size_t srccap = 0, dstcap = 0;
    for (uint32_t i = 0; i < n; i++) {
        char path[MAX_PATH + 80];
        snprintf(path, sizeof path, "%.*s\\%s", (int)dl, dir, items[i].name);
        FILE *f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "nxshpack: cannot read %s\n", path); return 1; }
        fseek(f, 0, SEEK_END);
        size_t sz = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > srccap) src = realloc(src, srccap = sz);
        size_t got = fread(src, 1, sz, f);
        fclose(f);
        if (got != sz) { fprintf(stderr, "nxshpack: short read %s\n", path); return 1; }
        size_t bound = ZSTD_compressBound(sz);
        if (bound > dstcap) dst = realloc(dst, dstcap = bound);
        size_t c = ZSTD_compressCCtx(cc, dst, dstcap, src, sz, level);
        if (ZSTD_isError(c)) { fprintf(stderr, "nxshpack: zstd: %s\n", ZSTD_getErrorName(c)); return 1; }
        fwrite(dst, 1, c, out);
        ents[i] = (Ent){items[i].hash, items[i].mask, (uint32_t)sz, off, (uint32_t)c, 0};
        off += c;
        raw += sz;
        if ((i + 1) % 2000 == 0) printf("nxshpack: %u / %u\n", i + 1, n);
    }
    fseek(out, sizeof hdr, SEEK_SET);
    fwrite(ents, sizeof *ents, n, out);
    if (fclose(out)) { fprintf(stderr, "nxshpack: write failed\n"); return 1; }
    printf("nxshpack: %u shaders (translator %08x), %.1f MB -> %.1f MB\n", n, translator, raw / 1048576.0, off / 1048576.0);
    return 0;
}
