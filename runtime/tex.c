/* Texture formats and layouts: NVN formats (sizes, Vulkan equivalents) and the Tegra X1's block-linear layout,
 * which is how textures sit in guest memory (packaged textures from the RomFS, and anything the game writes).
 *
 * Block-linear: a GOB is 64 bytes x 8 rows (512 bytes, its own swizzle inside); GOBs stack into blocks of
 * 2^bh GOBs high and 2^bd deep; blocks are laid out row-major. Mip levels follow each other, each with a block
 * height shrunk to fit; array layers / cube faces follow the whole mip chain, aligned to a level-0 block. */
#include "tex.h"
#include <stdlib.h>

/* bytes per block and block size of each NVNformat (0 = unknown) */
void format_info(u32 f, u32 *bpb, u32 *bw, u32 *bh) {
    *bw = *bh = 1;
    static const u8 plain[] = {
        0, 1, 1, 1, 1, 2, 2, 2, 2, 2, 4, 4, 4, 2, 2, 2, 2, 4, 4, 4, 4, 4, 8, 8, 8,   /* NONE .. RG32I */
        3, 3, 3, 3, 6, 6, 6, 6, 6, 12, 12, 12,                                      /* RGB8 .. RGB32I */
        4, 4, 4, 4, 8, 8, 8, 8, 8, 16, 16, 16,                                      /* RGBA8 .. RGBA32I */
        1, 2, 4, 4, 4, 8,                                                           /* STENCIL8 .. DEPTH32F_STENCIL8 */
        4, 4, 2, 2, 2, 2, 4, 4, 4, 4,                                               /* RGBX8_SRGB .. RGB9E5F */
    };
    if (f < sizeof plain) { *bpb = plain[f]; return; }
    if (f >= 65 && f <= 80) {                                                      /* DXT, RGTC, BPTC */
        *bw = *bh = 4;
        *bpb = (f == 65 || f == 66 || f == 69 || f == 70 || f == 73 || f == 74) ? 8 : 16;
        return;
    }
    if (f >= 81 && f <= 104) {                                                     /* vertex formats */
        static const u8 v[] = {1, 1, 2, 2, 4, 4, 2, 2, 4, 4, 8, 8, 3, 3, 6, 6, 12, 12, 4, 4, 8, 8, 16, 16};
        *bpb = v[f - 81];
        return;
    }
    if (f >= 105 && f <= 108) { *bpb = 4; return; }                               /* RGB10A2 variants */
    if (f >= 109 && f <= 120) {                                                    /* RGBX */
        static const u8 x[] = {4, 4, 4, 4, 8, 8, 8, 8, 8, 16, 16, 16};
        *bpb = x[f - 109];
        return;
    }
    if (f >= 121 && f <= 148) {                                                    /* ASTC, then ASTC sRGB */
        u32 k = (f - 121) % 14;
        *bw = astc_dims[k][0]; *bh = astc_dims[k][1]; *bpb = 16;
        return;
    }
    if (f >= 149 && f <= 152) { *bpb = 2; return; }                               /* BGR5 .. BGR565 */
    if (f >= 153 && f <= 156) { *bpb = 4; return; }                               /* BGRA8 .. BGRX8_SRGB */
    *bpb = 0;
}
const u8 astc_dims[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8},
                             {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};

static u64 div_up(u64 a, u64 b) { return (a + b - 1) / b; }
static u64 align_up(u64 a, u64 b) { return div_up(a, b) * b; }

u32 tex_layers(const TexInfo *t) {
    switch (t->target) {
    case TEX_1D_ARRAY: return t->height;
    case TEX_2D_ARRAY: case TEX_2D_MS_ARRAY: case TEX_CUBE: case TEX_CUBE_ARRAY: return t->depth;
    }
    return 1;
}
u32 tex_depth(const TexInfo *t) { return t->target == TEX_3D ? t->depth : 1; }
u32 tex_height(const TexInfo *t) { return t->target == TEX_1D || t->target == TEX_1D_ARRAY || t->target == TEX_BUFFER ? 1 : t->height; }

/* block height / depth of level 0, in GOBs (log2) */
static void block_dims(const TexInfo *t, u32 hb0, u32 depth, u32 *lbh, u32 *lbd) {
    u32 gh = 0, gd = 0;
    if (t->block_height != 0xff) gh = t->block_height;
    else while (gh < 4 && hb0 > 8u << gh) gh++;
    while (gd < 5 && depth > 1u << gd) gd++;
    *lbh = gh;
    *lbd = gd;
}

void tex_layout(const TexInfo *t, TexLayout *L) {
    memset(L, 0, sizeof *L);
    format_info(t->format, &L->bpb, &L->bw, &L->bh);
    if (!L->bpb) L->bpb = 16;
    u32 w = t->width, h = tex_height(t), d = tex_depth(t);
    L->layers = tex_layers(t);
    L->samples = t->samples > 1 ? t->samples : 1;
    L->levels = t->levels ? t->levels : 1;
    if (L->levels > 16) L->levels = 16;
    L->linear = (t->flags & TEX_FLAG_LINEAR) || t->target == TEX_BUFFER;
    u64 off = 0;
    if (L->linear) {
        u64 stride = t->stride ? (u64)t->stride : align_up(div_up(w, L->bw) * L->bpb, 32);
        L->level[0] = (TexLevel){0, (u32)div_up(w, L->bw), (u32)div_up(h, L->bh), d, stride, 0, 0};
        L->layer_size = stride * div_up(h, L->bh) * d * L->samples;
        L->size = align_up(L->layer_size * L->layers, 0x200);
        L->levels = 1;
        return;
    }
    u32 wb0 = (u32)div_up(w, L->bw), hb0 = (u32)div_up(h, L->bh);
    u32 lbh, lbd;
    block_dims(t, hb0, d, &lbh, &lbd);
    for (u32 l = 0; l < L->levels; l++) {
        u32 lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, ld = d >> l ? d >> l : 1;
        u32 wb = (u32)div_up(lw, L->bw), hb = (u32)div_up(lh, L->bh);
        u32 bh = lbh, bd = lbd;
        while (bh > 0 && hb <= 8u << (bh - 1)) bh--;
        while (bd > 0 && ld <= 1u << (bd - 1)) bd--;
        u64 row = align_up((u64)wb * L->bpb * L->samples, 64);
        u64 size = row * align_up(hb, 8ull << bh) * align_up(ld, 1ull << bd);
        L->level[l] = (TexLevel){off, wb, hb, ld, row, bh, bd};
        off += size;
    }
    (void)wb0;
    L->layer_size = L->layers > 1 ? align_up(off, 512ull << (lbh + lbd)) : off;
    L->size = align_up(L->layer_size * L->layers, 0x200);
}

u64 texture_storage_size(const TexInfo *t) {
    TexLayout L;
    tex_layout(t, &L);
    return L.size;
}

/* byte offset of (x bytes, y rows, z) inside a block-linear surface */
static inline u64 gob_offset(u32 x, u32 y) {
    return (x % 64 / 32) * 256 + (y % 8 / 2) * 64 + (x % 32 / 16) * 32 + (y % 2) * 16 + (x % 16);
}

/* one level of one layer: block-linear `src` (at the level's offset) -> rows of `pitch` bytes in dst */
void deswizzle_level(const TexLevel *lv, u32 bpb, const u8 *src, u64 src_size, u8 *dst, u64 pitch) {
    u32 gobs_w = (u32)(lv->row_pitch / 64);
    u32 bh = 1u << lv->bh, bd = 1u << lv->bd;
    u64 block = 512ull * bh * bd;
    u32 blocks_h = (lv->h + 8 * bh - 1) / (8 * bh);
    u32 row_bytes = lv->w * bpb;
    for (u32 z = 0; z < lv->d; z++)
        for (u32 y = 0; y < lv->h; y++) {
            u64 base = ((u64)(z / bd) * blocks_h + y / (8 * bh)) * gobs_w * block +
                       ((u64)(z % bd) * bh + (y / 8) % bh) * 512;
            u8 *out = dst + ((u64)z * lv->h + y) * pitch;
            for (u32 x = 0; x < row_bytes; x += 16) {          /* 16-byte runs are contiguous in a GOB */
                u64 o = base + (u64)(x / 64) * block + gob_offset(x % 64, y);
                u32 n = row_bytes - x < 16 ? row_bytes - x : 16;
                if (o + n <= src_size) memcpy(out + x, src + o, n);
                else memset(out + x, 0, n);
            }
        }
}

/* ---------------------------------------------------------------------------------------------- Vulkan formats */
static const struct { u16 nvn; u32 vk; } fmt_table[] = {
    {1, VK_FORMAT_R8_UNORM}, {2, VK_FORMAT_R8_SNORM}, {3, VK_FORMAT_R8_UINT}, {4, VK_FORMAT_R8_SINT},
    {5, VK_FORMAT_R16_SFLOAT}, {6, VK_FORMAT_R16_UNORM}, {7, VK_FORMAT_R16_SNORM}, {8, VK_FORMAT_R16_UINT},
    {9, VK_FORMAT_R16_SINT}, {10, VK_FORMAT_R32_SFLOAT}, {11, VK_FORMAT_R32_UINT}, {12, VK_FORMAT_R32_SINT},
    {13, VK_FORMAT_R8G8_UNORM}, {14, VK_FORMAT_R8G8_SNORM}, {15, VK_FORMAT_R8G8_UINT}, {16, VK_FORMAT_R8G8_SINT},
    {17, VK_FORMAT_R16G16_SFLOAT}, {18, VK_FORMAT_R16G16_UNORM}, {19, VK_FORMAT_R16G16_SNORM},
    {20, VK_FORMAT_R16G16_UINT}, {21, VK_FORMAT_R16G16_SINT}, {22, VK_FORMAT_R32G32_SFLOAT},
    {23, VK_FORMAT_R32G32_UINT}, {24, VK_FORMAT_R32G32_SINT},
    {25, VK_FORMAT_R8G8B8_UNORM}, {26, VK_FORMAT_R8G8B8_SNORM}, {27, VK_FORMAT_R8G8B8_UINT}, {28, VK_FORMAT_R8G8B8_SINT},
    {29, VK_FORMAT_R16G16B16_SFLOAT}, {30, VK_FORMAT_R16G16B16_UNORM}, {31, VK_FORMAT_R16G16B16_SNORM},
    {32, VK_FORMAT_R16G16B16_UINT}, {33, VK_FORMAT_R16G16B16_SINT}, {34, VK_FORMAT_R32G32B32_SFLOAT},
    {35, VK_FORMAT_R32G32B32_UINT}, {36, VK_FORMAT_R32G32B32_SINT},
    {37, VK_FORMAT_R8G8B8A8_UNORM}, {38, VK_FORMAT_R8G8B8A8_SNORM}, {39, VK_FORMAT_R8G8B8A8_UINT},
    {40, VK_FORMAT_R8G8B8A8_SINT}, {41, VK_FORMAT_R16G16B16A16_SFLOAT}, {42, VK_FORMAT_R16G16B16A16_UNORM},
    {43, VK_FORMAT_R16G16B16A16_SNORM}, {44, VK_FORMAT_R16G16B16A16_UINT}, {45, VK_FORMAT_R16G16B16A16_SINT},
    {46, VK_FORMAT_R32G32B32A32_SFLOAT}, {47, VK_FORMAT_R32G32B32A32_UINT}, {48, VK_FORMAT_R32G32B32A32_SINT},
    {49, VK_FORMAT_S8_UINT}, {50, VK_FORMAT_D16_UNORM}, {51, VK_FORMAT_X8_D24_UNORM_PACK32},
    {52, VK_FORMAT_D32_SFLOAT}, {53, VK_FORMAT_D24_UNORM_S8_UINT}, {54, VK_FORMAT_D32_SFLOAT_S8_UINT},
    {55, VK_FORMAT_R8G8B8A8_SRGB}, {56, VK_FORMAT_R8G8B8A8_SRGB}, {57, VK_FORMAT_A4B4G4R4_UNORM_PACK16},
    {58, VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR}, {59, VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR},
    {60, VK_FORMAT_B5G6R5_UNORM_PACK16}, {61, VK_FORMAT_A2B10G10R10_UNORM_PACK32},
    {62, VK_FORMAT_A2B10G10R10_UINT_PACK32}, {63, VK_FORMAT_B10G11R11_UFLOAT_PACK32},
    {64, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32},
    {65, VK_FORMAT_BC1_RGB_UNORM_BLOCK}, {66, VK_FORMAT_BC1_RGBA_UNORM_BLOCK}, {67, VK_FORMAT_BC2_UNORM_BLOCK},
    {68, VK_FORMAT_BC3_UNORM_BLOCK}, {69, VK_FORMAT_BC1_RGB_SRGB_BLOCK}, {70, VK_FORMAT_BC1_RGBA_SRGB_BLOCK},
    {71, VK_FORMAT_BC2_SRGB_BLOCK}, {72, VK_FORMAT_BC3_SRGB_BLOCK}, {73, VK_FORMAT_BC4_UNORM_BLOCK},
    {74, VK_FORMAT_BC4_SNORM_BLOCK}, {75, VK_FORMAT_BC5_UNORM_BLOCK}, {76, VK_FORMAT_BC5_SNORM_BLOCK},
    {77, VK_FORMAT_BC7_UNORM_BLOCK}, {78, VK_FORMAT_BC7_SRGB_BLOCK}, {79, VK_FORMAT_BC6H_SFLOAT_BLOCK},
    {80, VK_FORMAT_BC6H_UFLOAT_BLOCK},
    /* vertex formats: integer converted to float */
    {81, VK_FORMAT_R8_USCALED}, {82, VK_FORMAT_R8_SSCALED}, {83, VK_FORMAT_R16_USCALED}, {84, VK_FORMAT_R16_SSCALED},
    {85, VK_FORMAT_R32_UINT}, {86, VK_FORMAT_R32_SINT}, {87, VK_FORMAT_R8G8_USCALED}, {88, VK_FORMAT_R8G8_SSCALED},
    {89, VK_FORMAT_R16G16_USCALED}, {90, VK_FORMAT_R16G16_SSCALED}, {91, VK_FORMAT_R32G32_UINT},
    {92, VK_FORMAT_R32G32_SINT}, {93, VK_FORMAT_R8G8B8_USCALED}, {94, VK_FORMAT_R8G8B8_SSCALED},
    {95, VK_FORMAT_R16G16B16_USCALED}, {96, VK_FORMAT_R16G16B16_SSCALED}, {97, VK_FORMAT_R32G32B32_UINT},
    {98, VK_FORMAT_R32G32B32_SINT}, {99, VK_FORMAT_R8G8B8A8_USCALED}, {100, VK_FORMAT_R8G8B8A8_SSCALED},
    {101, VK_FORMAT_R16G16B16A16_USCALED}, {102, VK_FORMAT_R16G16B16A16_SSCALED}, {103, VK_FORMAT_R32G32B32A32_UINT},
    {104, VK_FORMAT_R32G32B32A32_SINT},
    {105, VK_FORMAT_A2B10G10R10_SNORM_PACK32}, {106, VK_FORMAT_A2B10G10R10_SINT_PACK32},
    {107, VK_FORMAT_A2B10G10R10_USCALED_PACK32}, {108, VK_FORMAT_A2B10G10R10_SSCALED_PACK32},
    /* RGBX: RGBA with alpha read as 1 (tex_swizzle) */
    {109, VK_FORMAT_R8G8B8A8_UNORM}, {110, VK_FORMAT_R8G8B8A8_SNORM}, {111, VK_FORMAT_R8G8B8A8_UINT},
    {112, VK_FORMAT_R8G8B8A8_SINT}, {113, VK_FORMAT_R16G16B16A16_SFLOAT}, {114, VK_FORMAT_R16G16B16A16_UNORM},
    {115, VK_FORMAT_R16G16B16A16_SNORM}, {116, VK_FORMAT_R16G16B16A16_UINT}, {117, VK_FORMAT_R16G16B16A16_SINT},
    {118, VK_FORMAT_R32G32B32A32_SFLOAT}, {119, VK_FORMAT_R32G32B32A32_UINT}, {120, VK_FORMAT_R32G32B32A32_SINT},
    {149, VK_FORMAT_A1R5G5B5_UNORM_PACK16}, {150, VK_FORMAT_A1R5G5B5_UNORM_PACK16},
    {151, VK_FORMAT_R5G6B5_UNORM_PACK16}, {152, VK_FORMAT_A1R5G5B5_UNORM_PACK16},
    {153, VK_FORMAT_B8G8R8A8_UNORM}, {154, VK_FORMAT_B8G8R8A8_UNORM}, {155, VK_FORMAT_B8G8R8A8_SRGB},
    {156, VK_FORMAT_B8G8R8A8_SRGB},
};

VkFormat vk_format(u32 f) {
    for (size_t i = 0; i < sizeof fmt_table / sizeof *fmt_table; i++)
        if (fmt_table[i].nvn == f) return (VkFormat)fmt_table[i].vk;
    if (f >= 121 && f <= 134) return VK_FORMAT_R8G8B8A8_UNORM;        /* ASTC: decoded on upload */
    if (f >= 135 && f <= 148) return VK_FORMAT_R8G8B8A8_SRGB;
    return VK_FORMAT_UNDEFINED;
}

bool format_is_astc(u32 f) { return f >= 121 && f <= 148; }
bool format_has_x(u32 f) { return f == 55 || (f >= 109 && f <= 120) || f == 154 || f == 156 || f == 58 || f == 149; }

VkImageAspectFlags format_aspects(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: case VK_FORMAT_D16_UNORM_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

/* the format class of a clear value / shader output: 0 float, 1 sint, 2 uint */
int format_numeric(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
    case VK_FORMAT_S8_UINT:
        return 2;
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32G32B32A32_SINT: case VK_FORMAT_A2B10G10R10_SINT_PACK32:
        return 1;
    default: return 0;
    }
}
