/* Image and frame dumps for debugging (NX_DUMP, NX_DUMP_DRAWS, F12 captures): images decoded to PPM files, or
 * written raw. Part of the NVN executor (gpu.c, gpu_int.h). */
#include "gpu_int.h"

/* NX_DUMP=n[,m...]: at those frames, write the presented window texture (frame_<n>.ppm) and every render target
 * the frame drew into (frame_<n>_<w>x<h>_<format>_<address>.ppm) to build/frames */

static float half_to_float(u16 h) {
    u32 s = h >> 15, e = h >> 10 & 31, m = h & 1023;
    float v = e == 0 ? m / 16777216.0f : e == 31 ? 65504.0f : (1.0f + m / 1024.0f) * (float)(1u << e) / 32768.0f;
    return s ? -v : v;
}
static u8 to8(float f) { return f <= 0 ? 0 : f >= 1 ? 255 : (u8)(f * 255.0f + 0.5f); }
static float uf11(u32 v, int mbits) {
    u32 e = v >> mbits, m = v & ((1u << mbits) - 1);
    if (e == 0) return m / (float)(1u << mbits) / 16384.0f;
    return (1.0f + m / (float)(1u << mbits)) * (float)(1u << e) / 32768.0f;
}

static bool decode_pixel(VkFormat f, const u8 *p, u8 out[3]) {
    u32 v;
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: case VK_FORMAT_R8G8B8A8_SRGB: memcpy(out, p, 3); return true;
    case VK_FORMAT_B8G8R8A8_UNORM: case VK_FORMAT_B8G8R8A8_SRGB: out[0] = p[2]; out[1] = p[1]; out[2] = p[0]; return true;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        memcpy(&v, p, 4);
        out[0] = (u8)((v & 1023) >> 2); out[1] = (u8)((v >> 10 & 1023) >> 2); out[2] = (u8)((v >> 20 & 1023) >> 2);
        return true;
    case VK_FORMAT_R16G16B16A16_SFLOAT: {
        u16 h[3];
        memcpy(h, p, 6);
        for (int i = 0; i < 3; i++) out[i] = to8(half_to_float(h[i]));
        return true;
    }
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        memcpy(&v, p, 4);
        out[0] = to8(uf11(v & 2047, 6)); out[1] = to8(uf11(v >> 11 & 2047, 6)); out[2] = to8(uf11(v >> 22, 5));
        return true;
    case VK_FORMAT_R32G32B32A32_SFLOAT: {
        float c[3];
        memcpy(c, p, 12);
        for (int i = 0; i < 3; i++) out[i] = to8(c[i]);
        return true;
    }
    case VK_FORMAT_R32_SFLOAT: case VK_FORMAT_D32_SFLOAT: {
        float d;
        memcpy(&d, p, 4);
        out[0] = out[1] = out[2] = to8(d);
        return true;
    }
    case VK_FORMAT_R8_UNORM: out[0] = out[1] = out[2] = p[0]; return true;
    case VK_FORMAT_R8G8_UNORM: out[0] = p[0]; out[1] = p[1]; out[2] = 0; return true;
    default: return false;
    }
}

static u32 format_bytes(VkFormat f) {
    switch (f) {
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    case VK_FORMAT_R8_UNORM: return 1;
    case VK_FORMAT_R8G8_UNORM: return 2;
    default: return 4;
    }
}

/* fmt: the format to decode the pixels as (a view's, which may differ from the image's); raw: also the bytes */
void dump_image_as(Image *im, const char *name, bool raw, VkFormat fmt) {
    u8 probe[3], zero[16] = {0};
    if (!decode_pixel(fmt, zero, probe) || im->info.samples > 1 || format_bytes(fmt) != format_bytes(im->format)) return;
    u32 w = lvl_w(im, 0), h = lvl_h(im, 0), bpp = format_bytes(fmt);      /* (at its render scale) */
    VkBuf b;
    vk_buffer((u64)w * h * bpp, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &b);
    VkBufferImageCopy r = {0, 0, 0, {copy_aspect(im), 0, 0, 1}, {0, 0, 0}, {w, h, 1}};
    end_rendering();
    vkCmdCopyImageToBuffer(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, b.buf, 1, &r);
    flush();
    gpu_wait(submitted, UINT64_MAX);
    char path[MAX_PATH];
    const char *dir = frames_dir();
    if (im->format == VK_FORMAT_R32G32B32A32_SFLOAT && w * h <= 4) {       /* tiny float targets (exposure): logged */
        const float *v = (const float *)b.a.map;
        for (u32 i = 0; i < w * h; i++) hlog(0, "gpu: %s texel %u = %g %g %g %g", name, i, v[4 * i], v[4 * i + 1], v[4 * i + 2], v[4 * i + 3]);
    }
    if (raw) {
        snprintf(path, sizeof path, "%s\\%s.raw", dir, name);              /* float targets: exact values too */
        FILE *rf = fopen(path, "wb");
        if (rf) { fwrite(b.a.map, 1, (size_t)w * h * bpp, rf); fclose(rf); }
    }
    snprintf(path, sizeof path, "%s\\%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        const u8 *px = b.a.map;
        u8 *row = malloc(w * 3);
        for (u32 y = 0; y < h; y++) {
            for (u32 x = 0; x < w; x++) decode_pixel(fmt, px + ((u64)y * w + x) * bpp, row + 3 * x);
            fwrite(row, 1, w * 3, f);
        }
        free(row);
        fclose(f);
    }
    vk_buffer_free(&b);
}

static void dump_image(Image *im, const char *name, bool raw) {
    /* raw bytes for the float targets only (exact values) */
    bool fl = im->format == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || im->format == VK_FORMAT_R16G16B16A16_SFLOAT ||
              im->format == VK_FORMAT_R32G32B32A32_SFLOAT;
    dump_image_as(im, name, raw && fl, im->format);
}

void dump_frame(Image *win, u64 frame) {
    const char *list = env.dump;
    u64 dump_tex = env.dump_tex;                        /* NX_DUMP_TEX: and every image at this address */
    char key[32], name[128];
    snprintf(key, sizeof key, ",%llu,", frame);
    if (frame != capture_frame && (!list[0] || !strstr(list, key))) return;
    int n = 0;
    for (int bkt = 0; bkt < IMAGE_BUCKETS; bkt++)
        for (Image *im = buckets[bkt]; im; im = im->next)
            if ((im->rt_frame == frame || (dump_tex && im->info.addr == dump_tex)) && im != win) {
                snprintf(name, sizeof name, "frame_%llu_%ux%u_f%u_%llx", frame, im->info.width, im->info.height,
                         im->info.format, im->info.addr);
                dump_image(im, name, true);
                n++;
            }
    if (win) {
        snprintf(name, sizeof name, "frame_%llu", frame);
        dump_image(win, name, true);
    }
    hlog(1, "gpu: frame %llu and %d render targets written to %s", frame, n, frames_dir());
}
