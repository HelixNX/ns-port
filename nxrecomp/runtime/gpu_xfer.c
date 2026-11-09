/* Work outside draws: clears, buffer/texture copies, blits (and the depth blit pipeline), downsamples, writes
 * of small data, and the CPU's texel reads and writes (gpu_texture_written, gpu_texture_read).
 * Part of the NVN executor (gpu.c, gpu_int.h). */
#include "gpu_int.h"

VkRect2D clear_rect(void) {
    s64 S = st.rt_scale ? st.rt_scale : 1;  /* the scissor is in the game's pixels */
    s64 x0 = (s64)st.scissor[0][0] * S, y0 = (s64)st.scissor[0][1] * S;
    s64 x1 = x0 + (s64)st.scissor[0][2] * S, y1 = y0 + (s64)st.scissor[0][3] * S;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (s64)st.area.extent.width) x1 = st.area.extent.width;
    if (y1 > (s64)st.area.extent.height) y1 = st.area.extent.height;
    if (x1 <= x0 || y1 <= y0) return (VkRect2D){{0, 0}, {0, 0}};
    return (VkRect2D){{(s32)x0, (s32)y0}, {(u32)(x1 - x0), (u32)(y1 - y0)}};
}

static VkClearColorValue clear_value(u32 type, const u32 v[4]) {
    VkClearColorValue c;
    memcpy(&c, v, 16);
    (void)type;                         /* float, int and uint values are all 4 x 32 bits as given */
    return c;
}

void do_clear_color(const CmdClearColor *p) {
    if (p->index >= st.nrt || !st.rt[p->index].t.addr || !begin_rendering()) return;
    VkRect2D r = clear_rect();
    if (!r.extent.width) return;
    if ((p->mask & 0xf) == 0) return;
    if ((p->mask & 0xf) != 0xf) {
        static bool logged;
        if (!logged) { logged = true; hlog(1, "gpu: ClearColor with channel mask %x: clearing all channels", p->mask); }
    }
    static int logged;
    if (logged < 12 || trace_frame()) {        /* every clear of a traced frame (F12, NX_TRACE) */
        logged++;
        float f[4];
        memcpy(f, p->v, 16);
        hlog(1, "gpu: clear %ux%u fmt %u at %llx to (%g %g %g %g) rect %d,%d %ux%u", st.rt[p->index].t.width,
             st.rt[p->index].t.height, st.rt[p->index].t.format, st.rt[p->index].t.addr, f[0], f[1], f[2], f[3],
             r.offset.x, r.offset.y, r.extent.width, r.extent.height);
    }
    VkClearAttachment a = {VK_IMAGE_ASPECT_COLOR_BIT, p->index, {.color = clear_value(p->type, p->v)}};
    VkClearRect cr = {r, 0, 1};
    vkCmdClearAttachments(cmd(), 1, &a, 1, &cr);
    pass_att_writes = true;
}

void do_clear_depth_stencil(const CmdClearDS *p) {
    if (!st.has_depth || !begin_rendering()) return;
    u32 ly, lv;
    Image *im = image_find(&st.depth.t, &ly, &lv);
    if (!im) return;
    VkImageAspectFlags asp = 0;
    if (p->depth_mask && im->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (p->stencil_mask & 0xff && im->aspects & VK_IMAGE_ASPECT_STENCIL_BIT) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
    VkRect2D r = clear_rect();
    if (!asp || !r.extent.width) return;
    VkClearAttachment a = {asp, 0, {.depthStencil = {p->depth, p->stencil}}};
    VkClearRect cr = {r, 0, 1};
    vkCmdClearAttachments(cmd(), 1, &a, 1, &cr);
    pass_att_writes = true;
}

/* ClearTexture: a region of a level/layers, outside the render targets */
void do_clear_texture(const CmdClearTexture *p) {
    watch("ClearTexture", &p->tex);
    ViewInfo v;
    Image *im = image_ref(&p->tex, &v);
    if (!im) return;
    end_rendering();
    u32 level = v.base_level;
    u32 layer = v.min_layer + (p->tex.t.target == TEX_3D ? 0 : (u32)p->r.z);
    u32 nlayers = p->tex.t.target == TEX_3D ? 1 : (p->r.d > 0 ? (u32)p->r.d : 1);
    VkImageSubresourceRange range = {im->aspects, level, 1, layer, nlayers};
    if (im->aspects & VK_IMAGE_ASPECT_COLOR_BIT) {
        VkClearColorValue c = clear_value(p->type, p->v);
        vkCmdClearColorImage(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &range);
    } else {
        float d;
        memcpy(&d, &p->v[0], 4);
        VkClearDepthStencilValue v = {d, p->v[1]};
        vkCmdClearDepthStencilImage(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    }
    im->valid = true;
    im->gpu_written = true;
    linear_wrote(im);
    barrier();
}

static VkBufferImageCopy bt_region(const CmdCopyBT *p, const Image *im, u64 off) {
    u32 bpb, bw, bh;
    format_info(p->tex.t.format, &bpb, &bw, &bh);
    if (!bpb) bpb = 4;
    u32 row_len = p->row_stride > 0 ? (u32)(p->row_stride / bpb * bw) : 0;
    u32 img_h = p->image_stride > 0 && p->row_stride > 0 ? (u32)(p->image_stride / p->row_stride * bh) : 0;
    bool is3d = p->tex.t.target == TEX_3D;
    u32 layer = p->tex.v.min_layer + (is3d ? 0 : (u32)p->r.z);
    VkBufferImageCopy r = {off, row_len, img_h,
        {copy_aspect(im), p->tex.v.base_level, layer, is3d ? 1 : (p->r.d > 0 ? (u32)p->r.d : 1)},
        {p->r.x, p->r.y, is3d ? p->r.z : 0},
        {(u32)p->r.w, (u32)(p->r.h > 0 ? p->r.h : 1), is3d ? (u32)(p->r.d > 0 ? p->r.d : 1) : 1}};
    return r;
}

void do_copy_buffer_to_texture(const CmdCopyBT *p) {
    watch("CopyBufferToTexture", &p->tex);
    CmdCopyBT q = *p;
    Image *im = image_ref(&p->tex, &q.tex.v);
    p = &q;
    VkBuffer buf;
    u64 off;
    if (!im || !gpu_buffer(p->buffer, &buf, &off) || p->r.w <= 0) return;
    if (format_is_astc(p->tex.t.format)) return;
    image_upload(im);                   /* the rest of the texture: its guest memory first (else zeros, for good once
                                         * it counts as GPU-written below; a glyph atlas filled a region at a time) */
    end_rendering();
    VkBufferImageCopy r = bt_region(p, im, off);
    if (im->scale > 1) {                /* into a 1x copy of the region, blitted up */
        Temp t;
        u32 n = r.imageSubresource.layerCount, S = im->scale;
        if (!scale_blittable(im) || !temp_image(im->format, r.imageExtent.width, r.imageExtent.height, 1, n, &t)) return;
        VkBufferImageCopy tr = r;
        tr.imageSubresource.mipLevel = 0;
        tr.imageSubresource.baseArrayLayer = 0;
        tr.imageOffset = (VkOffset3D){0, 0, 0};
        vkCmdCopyBufferToImage(cmd(), buf, t.image, VK_IMAGE_LAYOUT_GENERAL, 1, &tr);
        barrier();
        s32 x = r.imageOffset.x, y = r.imageOffset.y, w = (s32)r.imageExtent.width, h = (s32)r.imageExtent.height;
        VkImageBlit b = {{r.imageSubresource.aspectMask, 0, 0, n}, {{0, 0, 0}, {w, h, 1}}, r.imageSubresource,
                         {{x * (s32)S, y * (s32)S, 0}, {(x + w) * (s32)S, (y + h) * (s32)S, 1}}};
        vkCmdBlitImage(cmd(), t.image, VK_IMAGE_LAYOUT_GENERAL, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
        temp_free(&t);
    } else
        vkCmdCopyBufferToImage(cmd(), buf, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    im->valid = true;
    im->gpu_written = true;
    linear_wrote(im);
    barrier();
}

void do_copy_texture_to_buffer(const CmdCopyBT *p) {
    CmdCopyBT q = *p;
    Image *im = image_ref(&p->tex, &q.tex.v);
    p = &q;
    VkBuffer buf;
    u64 off;
    if (!im || !gpu_buffer(p->buffer, &buf, &off) || p->r.w <= 0) return;
    image_upload(im);
    end_rendering();
    VkBufferImageCopy r = bt_region(p, im, off);
    VkImage src = im->image;
    Temp t = {0};
    if (im->scale > 1) {                /* the region blitted down into a 1x copy first */
        u32 n = r.imageSubresource.layerCount, S = im->scale;
        if (!scale_blittable(im) || !temp_image(im->format, r.imageExtent.width, r.imageExtent.height, 1, n, &t)) return;
        s32 x = r.imageOffset.x, y = r.imageOffset.y, w = (s32)r.imageExtent.width, h = (s32)r.imageExtent.height;
        VkImageBlit b = {r.imageSubresource, {{x * (s32)S, y * (s32)S, 0}, {(x + w) * (s32)S, (y + h) * (s32)S, 1}},
                         {r.imageSubresource.aspectMask, 0, 0, n}, {{0, 0, 0}, {w, h, 1}}};
        vkCmdBlitImage(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, t.image, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_NEAREST);
        barrier();
        r.imageSubresource.mipLevel = 0;
        r.imageSubresource.baseArrayLayer = 0;
        r.imageOffset = (VkOffset3D){0, 0, 0};
        src = t.image;
    }
    vkCmdCopyImageToBuffer(cmd(), src, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &r);
    u64 moff;
    VkBuffer mbuf = mirror_at(p->buffer, &moff);
    if (mbuf) {
        r.bufferOffset = moff;
        vkCmdCopyImageToBuffer(cmd(), src, VK_IMAGE_LAYOUT_GENERAL, mbuf, 1, &r);
    }
    if (t.image) temp_free(&t);
    barrier();
}

/* copy the linear textures the GPU wrote in this submission back into guest memory (see linear_wrote) */
void linear_writeback(void) {
    end_rendering();
    barrier();
    for (int i = 0; i < nwb; i++) {
        Image *im = wb_list[i];
        im->wb_pending = false;
        TexLayout L;
        tex_layout(&im->info, &L);
        VkBuffer buf, mbuf;
        u64 off, moff;
        static int skipped, logged;
        if (!L.bpb || format_is_astc(im->info.format) || vk_format(im->info.format) != im->format ||
            im->info.samples > 1 ||
            !gpu_cpu(im->info.addr, L.layer_size * L.layers) || !gpu_buffer(im->info.addr, &buf, &off)) {
            if (skipped++ < 8)
                hlog(0, "gpu: linear %ux%u f%u at %llx written by the GPU: not copied back (format or memory)",
                     im->info.width, im->info.height, im->info.format, im->info.addr);
            continue;
        }
        mbuf = mirror_at(im->info.addr, &moff);
        if (logged++ < 16)
            hlog(1, "gpu: frame %llu: linear %ux%u f%u at %llx (row %llu) copied back to guest memory", frame_no + 1,
                 im->info.width, im->info.height, im->info.format, im->info.addr, L.level[0].row_pitch);
        u32 levels = im->levels < L.levels ? im->levels : L.levels;
        for (u32 layer = 0; layer < L.layers && layer < im->layers; layer++)
            for (u32 l = 0; l < levels; l++) {
                const TexLevel *lv = &L.level[l];
                if (lv->row_pitch % L.bpb) continue;
                u32 w = im->info.width >> l, h = tex_height(&im->info) >> l, d = tex_depth(&im->info) >> l;
                u64 at = (u64)layer * L.layer_size + lv->offset;
                VkBufferImageCopy r = {off + at, (u32)(lv->row_pitch / L.bpb * L.bw), lv->h * L.bh,
                                       {copy_aspect(im), l, layer, 1}, {0, 0, 0}, {w ? w : 1, h ? h : 1, d ? d : 1}};
                vkCmdCopyImageToBuffer(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &r);
                if (mbuf) {
                    r.bufferOffset = moff + at;
                    vkCmdCopyImageToBuffer(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, mbuf, 1, &r);
                }
            }
    }
    nwb = 0;
    barrier();
}

/* can s be blitted into d? Not every format can (on AMD, no depth/stencil format is a blit destination), and a
 * blit the device does not support may lose it: such copies are skipped and reported once per format pair */
bool blit_ok(const Image *s, const Image *d) {
    if (format_features(s->format) & VK_FORMAT_FEATURE_BLIT_SRC_BIT &&
        format_features(d->format) & VK_FORMAT_FEATURE_BLIT_DST_BIT)
        return true;
    static u32 seen[16];
    static int ns;
    u32 key = (u32)s->format << 16 | (u32)d->format;
    for (int i = 0; i < ns; i++) if (seen[i] == key) return false;
    if (ns < 16) seen[ns++] = key;
    hlog(0, "gpu: blit from format %d to %d is not supported here: %s", s->format, d->format,
         d->aspects & VK_IMAGE_ASPECT_DEPTH_BIT ? "drawn instead (depth_blit)" : "skipped");
    return false;
}

/* Depth blits the device cannot do (AMD: no depth format is a blit destination) are drawn instead: a full-screen
 * triangle that samples the source's depth (nearest) and writes gl_FragDepth (shaders/depth_blit.*). Violet
 * scales its 1080p depth to 960x540 this way for the soft particles' depth fade; skipped, that texture stayed
 * empty and every effects-layer particle was discarded (item ball light pillars, glows). */
#include "shaders/depth_blit_spv.h"
static VkPipelineLayout dblit_layout;
static VkDescriptorSetLayout dblit_set;
static VkSampler dblit_sampler;
static struct { VkFormat fmt; VkPipeline pipe; } dblit_pipes[8];

static VkPipeline dblit_pipeline(VkFormat fmt) {
    for (int i = 0; i < 8 && dblit_pipes[i].pipe; i++) if (dblit_pipes[i].fmt == fmt) return dblit_pipes[i].pipe;
    if (!dblit_layout) {
        VkDescriptorSetLayoutBinding b = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
        VkDescriptorSetLayoutCreateInfo sci = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 1, .pBindings = &b};
        VKCHECK(vkCreateDescriptorSetLayout(vk.dev, &sci, NULL, &dblit_set));
        VkPushConstantRange pr = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
        VkPipelineLayoutCreateInfo lci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                          .pSetLayouts = &dblit_set, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
        VKCHECK(vkCreatePipelineLayout(vk.dev, &lci, NULL, &dblit_layout));
        VkSamplerCreateInfo smci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
            .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .maxLod = 0};
        VKCHECK(vkCreateSampler(vk.dev, &smci, NULL, &dblit_sampler));
    }
    int slot = 0;
    while (slot < 7 && dblit_pipes[slot].pipe) slot++;
    VkShaderModule m[2];
    VkShaderModuleCreateInfo mci[2] = {
        {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof depth_blit_vs, .pCode = depth_blit_vs},
        {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof depth_blit_fs, .pCode = depth_blit_fs}};
    VKCHECK(vkCreateShaderModule(vk.dev, &mci[0], NULL, &m[0]));
    VKCHECK(vkCreateShaderModule(vk.dev, &mci[1], NULL, &m[1]));
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = m[0], .pName = "main"},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = m[1], .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                 .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo vp = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE, .depthWriteEnable = VK_TRUE, .depthCompareOp = VK_COMPARE_OP_ALWAYS};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT_WITH_COUNT, VK_DYNAMIC_STATE_SCISSOR_WITH_COUNT};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, .dynamicStateCount = 2,
                                           .pDynamicStates = dyn};
    VkPipelineRenderingCreateInfo ri = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, .depthAttachmentFormat = fmt};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, .stageCount = 2,
        .pStages = stages, .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vp,
        .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &ds, .pColorBlendState = &cb,
        .pDynamicState = &dy, .layout = dblit_layout};
    VkPipeline pipe = NULL;
    VkResult r = vkCreateGraphicsPipelines(vk.dev, NULL, 1, &ci, NULL, &pipe);
    vkDestroyShaderModule(vk.dev, m[0], NULL);
    vkDestroyShaderModule(vk.dev, m[1], NULL);
    if (r != VK_SUCCESS) { hlog(0, "gpu: depth blit pipeline for vk format %d failed (%d)", fmt, r); return NULL; }
    if (dblit_pipes[slot].pipe) vkDestroyPipeline(vk.dev, dblit_pipes[slot].pipe, NULL);   /* full: reuse the last */
    dblit_pipes[slot].fmt = fmt;
    dblit_pipes[slot].pipe = pipe;
    hlog(0, "gpu: depth blits into vk format %d are drawn (no blit support)", fmt);
    return pipe;
}

/* source level / layer / rectangle -> destination level / layer / rectangle, depth only; false if it cannot */
static bool depth_blit(Image *s, const ViewInfo *sv0, u32 slevel, u32 slayer, const s32 sr[4], Image *d,
                       const ViewInfo *dv0, u32 dlevel, u32 dlayer, const s32 dr[4]) {
    if (!(s->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) || !(d->aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ||
        !(s->usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !(d->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) ||
        s->info.samples > 1 || d->info.samples > 1)
        return false;
    VkPipeline pipe = dblit_pipeline(d->format);
    if (!pipe) return false;
    ViewInfo sv = *sv0, dv = *dv0;
    sv.target = TEX_2D;
    sv.base_level = (u16)slevel;
    sv.num_levels = 1;
    sv.min_layer = (u16)slayer;
    sv.num_layers = 1;
    memset(sv.swizzle, 0xff, sizeof sv.swizzle);    /* identity */
    sv.ds_mode = 0;                                 /* the depth aspect */
    dv.target = TEX_2D;
    dv.base_level = (u16)dlevel;
    dv.num_levels = 1;
    dv.min_layer = (u16)dlayer;
    dv.num_layers = 1;
    VkImageView src = image_view(s, &sv, VIEW_SAMPLED), dst = image_view(d, &dv, VIEW_ATTACHMENT);
    if (!src || !dst) return false;
    float sw = (float)lvl_w(s, slevel), sh = (float)lvl_h(s, slevel);     /* (the rectangles are scaled) */
    float uv[4] = {sr[0] / sw, sr[1] / sh, (sr[0] + sr[2]) / sw, (sr[1] + sr[3]) / sh};
    VkCommandBuffer c = cmd();
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = dst,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    VkRect2D area = {{dr[0], dr[1]}, {(u32)dr[2], (u32)dr[3]}};
    VkRenderingInfo rinfo = {VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = area, .layerCount = 1, .pDepthAttachment = &da};
    vkCmdBeginRendering(c, &rinfo);
    vkCmdBindPipeline(c, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    VkViewport vpt = {(float)dr[0], (float)dr[1], (float)dr[2], (float)dr[3], 0, 1};
    vkCmdSetViewportWithCount(c, 1, &vpt);
    vkCmdSetScissorWithCount(c, 1, &area);
    VkDescriptorImageInfo ii = {dblit_sampler, src, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1,
                              .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &ii};
    vkCmdPushDescriptorSetKHR(c, VK_PIPELINE_BIND_POINT_GRAPHICS, dblit_layout, 0, 1, &w);
    vkCmdPushConstants(c, dblit_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, uv);
    vkCmdDraw(c, 3, 1, 0, 0);
    vkCmdEndRendering(c);
    rec_forget();                       /* the game's draws must bind and set everything again */
    return true;
}

void do_copy_texture_to_texture(const CmdCopyTT *p) {
    watch("CopyTextureToTexture (dst)", &p->dst);
    CmdCopyTT q = *p;
    Image *s = image_ref(&p->src, &q.src.v), *d = image_ref(&p->dst, &q.dst.v);
    p = &q;
    if (!s || !d || p->sr.w <= 0 || p->dr.w <= 0) return;
    image_upload(s);
    end_rendering();
    if (trace_frame())
        hlog(0, "gpu: copy %ux%u f%u @%llx (%d,%d %dx%d z%d) -> %ux%u f%u @%llx (%d,%d %dx%d z%d) flags %x", p->src.t.width,
             p->src.t.height, p->src.t.format, p->src.t.addr, p->sr.x, p->sr.y, p->sr.w, p->sr.h, p->sr.z, p->dst.t.width,
             p->dst.t.height, p->dst.t.format, p->dst.t.addr, p->dr.x, p->dr.y, p->dr.w, p->dr.h, p->dr.z, p->flags);
    bool s3 = p->src.t.target == TEX_3D, d3 = p->dst.t.target == TEX_3D;
    VkImageSubresourceLayers sl = {copy_aspect(s), p->src.v.base_level, p->src.v.min_layer + (s3 ? 0 : (u32)p->sr.z),
                                   s3 ? 1 : (p->sr.d > 0 ? (u32)p->sr.d : 1)};
    VkImageSubresourceLayers dl = {copy_aspect(d), p->dst.v.base_level, p->dst.v.min_layer + (d3 ? 0 : (u32)p->dr.z),
                                   d3 ? 1 : (p->dr.d > 0 ? (u32)p->dr.d : 1)};
    s32 sh = p->sr.h > 0 ? p->sr.h : 1, dh = p->dr.h > 0 ? p->dr.h : 1;
    s32 sd = s3 ? (p->sr.d > 0 ? p->sr.d : 1) : 1, dd = d3 ? (p->dr.d > 0 ? p->dr.d : 1) : 1;
    /* rectangles in each image's own (scaled) pixels */
    s32 ss = (s32)s->scale, ds = (s32)d->scale;
    s32 sx = p->sr.x * ss, sy = p->sr.y * ss, sw = p->sr.w * ss, dx = p->dr.x * ds, dy = p->dr.y * ds, dw = p->dr.w * ds;
    sh *= ss;
    dh *= ds;
    u32 sb, db, bw, bh;
    format_info(p->src.t.format, &sb, &bw, &bh);
    format_info(p->dst.t.format, &db, &bw, &bh);
    bool same = sw == dw && sh == dh && sd == dd;
    if (same && (sb == db || s->format == d->format) && s->info.samples == d->info.samples) {
        VkImageCopy c = {sl, {sx, sy, s3 ? p->sr.z : 0}, dl, {dx, dy, d3 ? p->dr.z : 0}, {(u32)sw, (u32)sh, (u32)sd}};
        vkCmdCopyImage(cmd(), s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    } else {
        VkImageBlit b = {sl, {{sx, sy, s3 ? p->sr.z : 0}, {sx + sw, sy + sh, (s3 ? p->sr.z : 0) + sd}},
                         dl, {{dx, dy, d3 ? p->dr.z : 0}, {dx + dw, dy + dh, (d3 ? p->dr.z : 0) + dd}}};
        bool depth = s->aspects & ~VK_IMAGE_ASPECT_COLOR_BIT;
        if (!blit_ok(s, d)) {
            s32 sr[4] = {sx, sy, sw, sh}, dr[4] = {dx, dy, dw, dh};
            if (s3 || d3 || !depth_blit(s, &p->src.v, sl.mipLevel, sl.baseArrayLayer, sr, d, &p->dst.v, dl.mipLevel,
                                        dl.baseArrayLayer, dr))
                return;
            d->valid = true;
            d->gpu_written = true;
            linear_wrote(d);
            barrier();
            return;
        }
        vkCmdBlitImage(cmd(), s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       p->flags & 1 && !depth ? smooth(s->format) : VK_FILTER_NEAREST);
    }
    d->valid = true;
    d->gpu_written = true;
    linear_wrote(d);
    barrier();
}

void do_downsample(const CmdDownsample *p) {
    watch("Downsample (dst)", &p->dst);
    CmdDownsample q = *p;
    Image *s = image_ref(&p->src, &q.src.v), *d = image_ref(&p->dst, &q.dst.v);
    p = &q;
    if (!s || !d) return;
    end_rendering();
    if (trace_frame())
        hlog(0, "gpu: downsample %ux%u f%u @%llx -> %ux%u f%u @%llx", p->src.t.width, p->src.t.height, p->src.t.format,
             p->src.t.addr, p->dst.t.width, p->dst.t.height, p->dst.t.format, p->dst.t.addr);
    u32 w = p->dst.t.width * d->scale, h = tex_height(&p->dst.t) * d->scale;      /* in each image's own pixels */
    u32 srw = p->src.t.width * s->scale, srh = tex_height(&p->src.t) * s->scale;
    VkImageSubresourceLayers sl = {copy_aspect(s), p->src.v.base_level, p->src.v.min_layer, 1};
    VkImageSubresourceLayers dl = {copy_aspect(d), p->dst.v.base_level, p->dst.v.min_layer, 1};
    if (s->info.samples > 1 && d->info.samples <= 1 && s->aspects == VK_IMAGE_ASPECT_COLOR_BIT) {
        VkImageResolve r = {sl, {0, 0, 0}, dl, {0, 0, 0}, {w < srw ? w : srw, h < srh ? h : srh, 1}};
        vkCmdResolveImage(cmd(), s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else {
        if (!blit_ok(s, d)) {
            s32 sr[4] = {0, 0, (s32)srw, (s32)srh}, dr[4] = {0, 0, (s32)w, (s32)h};
            if (!depth_blit(s, &p->src.v, sl.mipLevel, sl.baseArrayLayer, sr, d, &p->dst.v, dl.mipLevel,
                            dl.baseArrayLayer, dr))
                return;
            d->valid = true;
            d->gpu_written = true;
            linear_wrote(d);
            barrier();
            return;
        }
        VkImageBlit b = {sl, {{0, 0, 0}, {(s32)srw, (s32)srh, 1}}, dl, {{0, 0, 0}, {(s32)w, (s32)h, 1}}};
        vkCmdBlitImage(cmd(), s->image, VK_IMAGE_LAYOUT_GENERAL, d->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       smooth(s->format));
    }
    d->valid = true;
    d->gpu_written = true;
    linear_wrote(d);
    barrier();
}

/* GPU writes of small values into guest memory, in GPU order (outside rendering, as Vulkan requires) */
void gpu_write(u64 addr, const void *data, u32 size) {
    VkBuffer buf;
    u64 off;
    if (!gpu_buffer(addr, &buf, &off)) {
        u8 *p = gpu_cpu(addr, size);
        if (p) memcpy(p, data, size);  /* not GPU-reachable: write it now */
        return;
    }
    u64 moff = 0;
    VkBuffer mbuf = mirror_at(addr, &moff);
    if (st.rendering && size <= 16 && npending < sizeof pending / sizeof *pending) {
        PendingWrite *w = &pending[npending++];
        w->buf = buf;
        w->off = off;
        w->mbuf = mbuf;
        w->moff = moff;
        w->size = (size + 3) & ~3u;
        memcpy(w->data, data, size);
        return;
    }
    end_rendering();
    vkCmdUpdateBuffer(cmd(), buf, off, (size + 3) & ~3u, data);
    if (mbuf) vkCmdUpdateBuffer(cmd(), mbuf, moff, (size + 3) & ~3u, data);
    barrier();
}

u64 now_ns(void) {
    LARGE_INTEGER f, n;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&n);
    return (u64)((double)n.QuadPart * 1e9 / (double)f.QuadPart);
}

/* the CPU wrote texels (nvnTextureWriteTexels): copy them into the image */
void gpu_texture_written(const TexInfo *t, const ViewInfo *v, const CopyRegion *r, const void *data,
                         s64 row_stride, s64 image_stride) {
    EnterCriticalSection(&gpu_cs);
    TexRef tr = {*t, *v};
    ViewInfo vv;
    Image *im = image_ref(&tr, &vv);
    v = &vv;
    if (im && r->w > 0 && !format_is_astc(t->format)) {
        u32 bpb, bw, bh;
        format_info(t->format, &bpb, &bw, &bh);
        u32 h = r->h > 0 ? (u32)r->h : 1, d = r->d > 0 ? (u32)r->d : 1;
        u64 row = row_stride > 0 ? (u64)row_stride : (u64)(r->w + bw - 1) / bw * bpb;
        u64 img = image_stride > 0 ? (u64)image_stride : row * ((h + bh - 1) / bh);
        u64 size = img * d;
        VkBuffer buf;
        u64 off;
        void *dst = staging(size, &buf, &off);
        memcpy(dst, data, size);
        CmdCopyBT c = {0};
        c.tex.t = *t;
        c.tex.v = *v;
        c.r = *r;
        c.row_stride = (s64)row;
        c.image_stride = (s64)img;
        VkBufferImageCopy reg = bt_region(&c, im, off);
        image_upload(im);               /* the rest of the texture first (as do_copy_buffer_to_texture) */
        end_rendering();
        vkCmdCopyBufferToImage(cmd(), buf, im->image, VK_IMAGE_LAYOUT_GENERAL, 1, &reg);
        im->valid = true;
        im->gpu_written = true;         /* guest memory does not hold these texels */
        linear_wrote(im);
        barrier();
        static int logged;
        if (logged++ < 8) {
            const u32 *px = data;
            hlog(1, "gpu: WriteTexels %ux%ux%u f%u target %u region %d,%d,%d %dx%dx%d row %llu: %08x %08x %08x %08x",
                 t->width, t->height, t->depth, t->format, t->target, r->x, r->y, r->z, r->w, r->h, r->d, row, px[0],
                 px[1], px[2], px[3]);
        }
    }
    LeaveCriticalSection(&gpu_cs);
}

/* the CPU reads texels (nvnTextureReadTexels): copy the region out of the image after the work submitted so far */
void gpu_texture_read(const TexInfo *t, const ViewInfo *v, const CopyRegion *r, void *data,
                      s64 row_stride, s64 image_stride) {
    EnterCriticalSection(&gpu_cs);
    TexRef tr = {*t, *v};
    ViewInfo vv;
    Image *im = image_ref(&tr, &vv);
    v = &vv;
    if (im && r->w > 0 && !format_is_astc(t->format)) {
        u32 bpb, bw, bh;
        format_info(t->format, &bpb, &bw, &bh);
        u32 h = r->h > 0 ? (u32)r->h : 1, d = r->d > 0 ? (u32)r->d : 1;
        u64 row = row_stride > 0 ? (u64)row_stride : (u64)(r->w + bw - 1) / bw * bpb;
        u64 img = image_stride > 0 ? (u64)image_stride : row * ((h + bh - 1) / bh);
        u64 size = img * d;
        VkBuf b;
        vk_buffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, &b);
        image_upload(im);
        CmdCopyBT c = {0};
        c.tex.t = *t;
        c.tex.v = *v;
        c.r = *r;
        c.row_stride = (s64)row;
        c.image_stride = (s64)img;
        VkBufferImageCopy reg = bt_region(&c, im, 0);
        end_rendering();
        barrier();
        vkCmdCopyImageToBuffer(cmd(), im->image, VK_IMAGE_LAYOUT_GENERAL, b.buf, 1, &reg);
        flush();
        gpu_wait(submitted, UINT64_MAX);
        memcpy(data, b.a.map, size);
        vk_buffer_free(&b);
    } else {
        hlog(0, "gpu: ReadTexels of %ux%u f%u: no image (left as is)", t->width, t->height, t->format);
    }
    LeaveCriticalSection(&gpu_cs);
}
