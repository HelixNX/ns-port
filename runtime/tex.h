/* Texture formats and the block-linear layout (tex.c). */
#ifndef TEX_H
#define TEX_H
#include "nvn.h"
#include "vk.h"

typedef struct TexLevel {
    u64 offset;                         /* from the start of the layer */
    u32 w, h, d;                        /* in blocks (compressed formats) or texels */
    u64 row_pitch;                      /* bytes per row of blocks (block-linear: a multiple of 64) */
    u32 bh, bd;                         /* log2 block height / depth in GOBs */
} TexLevel;
typedef struct TexLayout {
    u32 bpb, bw, bh;                    /* bytes per block, block width and height in texels */
    u32 levels, layers, samples;
    bool linear;
    TexLevel level[16];
    u64 layer_size, size;
} TexLayout;

extern const u8 astc_dims[14][2];
void tex_layout(const TexInfo *t, TexLayout *out);
u32 tex_layers(const TexInfo *t);
u32 tex_depth(const TexInfo *t);
u32 tex_height(const TexInfo *t);
void deswizzle_level(const TexLevel *lv, u32 bpb, const u8 *src, u64 src_size, u8 *dst, u64 pitch);

VkFormat vk_format(u32 nvn_format);
bool format_is_astc(u32 f);
bool format_has_x(u32 f);                /* RGBX-style: alpha reads as 1 */
VkImageAspectFlags format_aspects(VkFormat f);
int format_numeric(VkFormat f);          /* 0 float/unorm, 1 sint, 2 uint */

#endif
