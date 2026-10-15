/* pl:u: the shared fonts, in a 0x1100000-byte shared memory. Each font there is [magic][size][data], the words
 * xor'd with a fixed key (the BFTTF format); the service reports where the data starts and its size.
 * The fonts are firmware data: build/sysdata/<data id>/<file>.bfttf (from the user's console) is used as is when
 * present; otherwise a Windows font stands in (so the button glyphs of the Nintendo extension font are missing). */
#include "../ipc.h"
#include <stdlib.h>

#define FONT_MEM_SIZE 0x1100000
#define FONT_MAGIC    0x36f81a1eu            /* the first word, encrypted */
#define FONT_RESULT   0x7f9a0218u            /* ... and decrypted */

static const struct { u64 id; const char *file; } fonts[] = {
    {0x0100000000000811ull, "nintendo_udsg-r_std_003.bfttf"},           /* Japan, US, Europe */
    {0x0100000000000814ull, "nintendo_udsg-r_org_zh-cn_003.bfttf"},     /* Chinese (simplified) */
    {0x0100000000000814ull, "nintendo_udsg-r_ext_zh-cn_003.bfttf"},     /* extended Chinese (simplified) */
    {0x0100000000000813ull, "nintendo_udjxh-db_zh-tw_003.bfttf"},       /* Chinese (traditional) */
    {0x0100000000000812ull, "nintendo_udsg-r_ko_003.bfttf"},            /* Korean */
    {0x0100000000000810ull, "nintendo_ext_003.bfttf"},                  /* Nintendo extended (button glyphs) */
};
#define NFONTS 6

static KShmem *font_shmem;
static u32 region_off[NFONTS], region_size[NFONTS];
static char sysdata_dir[1024];

static u8 *read_all(const char *path, u64 *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    _fseeki64(f, 0, SEEK_END);
    *size = (u64)_ftelli64(f);
    _fseeki64(f, 0, SEEK_SET);
    u8 *p = malloc(*size + 4);
    memset(p, 0, *size + 4);
    if (fread(p, 1, *size, f) != *size) { free(p); p = NULL; }
    fclose(f);
    return p;
}

static u32 bswap(u32 v) { return __builtin_bswap32(v); }

/* a BFTTF file -> the shared memory form: the first word becomes the decrypted magic, the size is kept encrypted
 * (with the key the magic gives), the data is plain */
static void bfttf_to_shared(const u32 *fw, u64 words, u32 *out) {
    u32 key = bswap(fw[0]) ^ FONT_RESULT;
    for (u64 i = 0; i < words; i++) out[i] = fw[i] ^ bswap(key);
    out[1] = bswap(out[1]) ^ key;
}

/* a TTF -> a BFTTF file */
static u32 *ttf_to_bfttf(const u8 *ttf, u64 size, u64 *words) {
    u64 n = (size + 3) / 4;
    u32 *w = calloc(n + 2, 4);
    u32 key = bswap(FONT_RESULT ^ FONT_MAGIC);
    w[0] = bswap(FONT_MAGIC);
    w[1] = bswap((u32)(n * 4)) ^ key;
    for (u64 k = 0; k < n; k++) {
        u32 v = 0;
        memcpy(&v, ttf + 4 * k, size - 4 * k < 4 ? size - 4 * k : 4);
        w[2 + k] = v ^ key;
    }
    *words = n + 2;
    return w;
}

static void load_fonts(void) {
    u8 *mem = shmem_ptr(font_shmem);
    u64 off = 0;
    for (int i = 0; i < NFONTS; i++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%016llx/%s", sysdata_dir, fonts[i].id, fonts[i].file);
        u64 size = 0, words = 0;
        u8 *data = read_all(path, &size);
        u32 *bfttf;
        if (data) {
            words = size / 4;
            bfttf = (u32 *)data;
        } else {
            data = read_all("C:/Windows/Fonts/arial.ttf", &size);
            if (!data) { hlog(0, "pl: no font for type %d", i); continue; }
            bfttf = ttf_to_bfttf(data, size, &words);
            free(data);
            data = (u8 *)bfttf;
        }
        if (words < 2 || off + words * 4 > FONT_MEM_SIZE) { hlog(0, "pl: font %d does not fit", i); free(data); break; }
        bfttf_to_shared(bfttf, words, (u32 *)(mem + off));
        region_off[i] = (u32)off + 8;
        region_size[i] = (u32)(words * 4 - 8);
        off += words * 4;
        free(data);
    }
    hlog(1, "pl: shared fonts loaded (%u bytes)", (u32)off);
}

static void pl_load_state(SvcObj *o, IpcReq *r) { out_u32(r, 1); }      /* loaded */
static void pl_size(SvcObj *o, IpcReq *r) { u32 t = in_u32(r, 0); out_u32(r, t < NFONTS ? region_size[t] : 0); }
static void pl_offset(SvcObj *o, IpcReq *r) { u32 t = in_u32(r, 0); out_u32(r, t < NFONTS ? region_off[t] : 0); }
static void pl_shmem(SvcObj *o, IpcReq *r) {
    u32 h;
    if (handle_new(&font_shmem->obj, &h)) fatal("out of handles");
    out_copy_handle(r, h);
}
/* GetSharedFontInOrderOfPriority(language code): types, offsets and sizes in three output arrays */
static void pl_in_order(SvcObj *o, IpcReq *r) {
    u32 n = 0;
    const IpcBuf *codes = buf_out(r, 0), *offs = buf_out(r, 1), *sizes = buf_out(r, 2);
    for (u32 i = 0; i < NFONTS; i++) {
        if (!region_size[i]) continue;
        if (!codes || !offs || !sizes || (n + 1) * 4 > codes->size || (n + 1) * 4 > offs->size || (n + 1) * 4 > sizes->size) break;
        ST32(codes->addr + 4 * n, i);
        ST32(offs->addr + 4 * n, region_off[i]);
        ST32(sizes->addr + 4 * n, region_size[i]);
        n++;
    }
    out_u8(r, 1);
    out_u32(r, n);
}

static const Iface iface_pl = {"pl:u", (const IpcCmd[]){
    {0, ipc_noop, "RequestLoad"},
    {1, pl_load_state, "GetLoadState"},
    {2, pl_size, "GetSize"},
    {3, pl_offset, "GetSharedMemoryAddressOffset"},
    {4, pl_shmem, "GetSharedMemoryNativeHandle"},
    {5, pl_in_order, "GetSharedFontInOrderOfPriority"},
    {6, pl_in_order, "GetSharedFontInOrderOfPriorityForSystem"},
    IPC_END}, NULL};

static SvcObj *open_pl(const char *name) { return svcobj_new(&iface_pl, NULL, name); }

void pl_register(const char *save_dir) {
    snprintf(sysdata_dir, sizeof sysdata_dir, "%s/../sysdata", save_dir);
    font_shmem = shmem_create(FONT_MEM_SIZE, "fonts");
    load_fonts();
    service_register("pl:u", open_pl);
}
