/* ASTC LDR decoding to RGBA8 (astc.c). */
#ifndef ASTC_H
#define ASTC_H
#include "a64rt.h"
#include <stdbool.h>

void astc_decode_block(const u8 *block, u32 bw, u32 bh, bool srgb, u8 *out, u32 pitch);
/* a level: wblocks x hblocks blocks of bw x bh texels -> width x height RGBA8 texels */
void astc_decode(const u8 *blocks, u32 wblocks, u32 hblocks, u32 bw, u32 bh, bool srgb, u8 *out, u32 width,
                 u32 height);

#endif
