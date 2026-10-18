/* hwopus: the "hardware" Opus decoder. Not decoded yet (silence): each packet is parsed far enough (the 8-byte
 * header: u32 big-endian size, u32 final range; then the Opus TOC byte) to consume it exactly and return the
 * number of samples it holds, so that a sound engine's timing and buffering are as on the console.
 * Command ids are from the SDK proxies (IHardwareOpusDecoderManager, IHardwareOpusDecoder). */
#include "../ipc.h"
#include <stdlib.h>

typedef struct OpusDec { u32 rate, channels; } OpusDec;

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

/* samples per channel in one Opus packet at `rate` (RFC 6716 3.1: TOC configuration and frame count code) */
static u32 packet_samples(const u8 *pkt, u32 size, u32 rate) {
    if (!size) return 0;
    u32 config = pkt[0] >> 3, code = pkt[0] & 3, frames;
    static const u32 silk_us[4] = {10000, 20000, 40000, 60000}, celt_us[4] = {2500, 5000, 10000, 20000};
    u32 us = config < 12 ? silk_us[config & 3] : config < 16 ? (config & 1 ? 20000 : 10000) : celt_us[config & 3];
    if (code == 0) frames = 1;
    else if (code < 3) frames = 2;
    else frames = size > 1 ? pkt[1] & 0x3f : 0;
    return (u32)((u64)frames * us * rate / 1000000);
}

/* in: A (or X) packets, out: B (or C) PCM16 interleaved; replies consumed bytes, samples (per channel) */
static void decode(SvcObj *o, IpcReq *r, bool perf) {
    OpusDec *d = o->data;
    const IpcBuf *in = buf_in(r, 0), *out = buf_out(r, 0);
    u32 consumed = 0, samples = 0;
    if (in && in->size >= 8) {
        const u8 *p = (const u8 *)(uintptr_t)in->addr;
        u32 size = be32(p);
        if (8 + (u64)size <= in->size) {
            consumed = 8 + size;
            samples = packet_samples(p + 8, size, d->rate);
        }
    }
    if (out && out->size) {
        u64 bytes = (u64)samples * d->channels * 2;
        if (bytes > out->size) { samples = (u32)(out->size / (d->channels * 2)); bytes = (u64)samples * d->channels * 2; }
        memset((void *)(uintptr_t)out->addr, 0, (size_t)bytes);
    }
    out_u32(r, consumed);
    out_u32(r, samples);
    if (perf) out_u64(r, 0);
}
static void dec_old(SvcObj *o, IpcReq *r) { decode(o, r, false); }
static void dec_perf(SvcObj *o, IpcReq *r) { decode(o, r, true); }
static void dec_destroy(SvcObj *o) { free(o->data); }

static const Iface iface_decoder = {"IHardwareOpusDecoder", (const IpcCmd[]){
    {0, dec_old, "DecodeInterleavedOld"},
    {1, ipc_noop, "SetContext"},
    {2, dec_old, "DecodeInterleavedForMultiStreamOld"},
    {3, ipc_noop, "SetContextForMultiStream"},
    {4, dec_perf, "DecodeInterleavedWithPerfOld"},
    {5, dec_perf, "DecodeInterleavedForMultiStreamWithPerfOld"},
    {6, dec_perf, "DecodeInterleavedWithPerfAndResetOld"},
    {7, dec_perf, "DecodeInterleavedForMultiStreamWithPerfAndResetOld"},
    {8, dec_perf, "DecodeInterleaved"},
    {9, dec_perf, "DecodeInterleavedForMultiStream"},
    IPC_END}, dec_destroy};

/* OpusParameters(Ex): u32 sample rate, u32 channel count[, u8 large frame size]; MultiStream: the same first */
static void open_decoder(SvcObj *o, IpcReq *r) {
    OpusDec *d = calloc(1, sizeof *d);
    d->rate = in_u32(r, 0) ? in_u32(r, 0) : 48000;
    d->channels = in_u32(r, 4) ? in_u32(r, 4) : 1;
    const IpcBuf *ms = buf_in(r, 0);                    /* multistream parameters come in a buffer */
    if (r->cmd == 2 || r->cmd == 6) {
        if (ms && ms->size >= 8) { d->rate = LD32(ms->addr); d->channels = LD32(ms->addr + 4); }
    }
    if (!d->channels || d->channels > 255) d->channels = 2;
    hlog(1, "hwopus: decoder %u Hz, %u channels (silent: not decoded yet)", d->rate, d->channels);
    out_object(r, svcobj_new(&iface_decoder, d, "IHardwareOpusDecoder"));
}
static void work_size(SvcObj *o, IpcReq *r) {
    u32 ch = in_u32(r, 4);
    if (r->cmd == 3 || r->cmd == 7 || r->cmd == 9) {
        const IpcBuf *ms = buf_in(r, 0);
        ch = ms && ms->size >= 8 ? LD32(ms->addr + 4) : 2;
    }
    if (!ch || ch > 255) ch = 2;
    out_u32(r, 0x10000 + ch * 0x8000);
}

static const Iface iface_manager = {"hwopus", (const IpcCmd[]){
    {0, open_decoder, "OpenHardwareOpusDecoder"},
    {1, work_size, "GetWorkBufferSize"},
    {2, open_decoder, "OpenHardwareOpusDecoderForMultiStream"},
    {3, work_size, "GetWorkBufferSizeForMultiStream"},
    {4, open_decoder, "OpenHardwareOpusDecoderEx"},
    {5, work_size, "GetWorkBufferSizeEx"},
    {6, open_decoder, "OpenHardwareOpusDecoderForMultiStreamEx"},
    {7, work_size, "GetWorkBufferSizeForMultiStreamEx"},
    {8, work_size, "GetWorkBufferSizeExEx"},
    {9, work_size, "GetWorkBufferSizeForMultiStreamExEx"},
    IPC_END}, NULL};

static SvcObj *open_hwopus(const char *name) { return svcobj_new(&iface_manager, NULL, name); }

void hwopus_register(void) { service_register("hwopus", open_hwopus); }
