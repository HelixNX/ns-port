/* hwopus: the "hardware" Opus decoder, decoded with libopus (vcpkg `opus`). Each request carries one packet after
 * an 8-byte header (u32 big-endian size, u32 final range); the reply is the bytes consumed and the samples (per
 * channel) written, PCM16 interleaved at the decoder's rate. A packet libopus rejects is answered with silence of
 * its length (from the TOC byte), so a sound engine's timing and buffering stay as on the console.
 * Command ids are from the SDK proxies (IHardwareOpusDecoderManager, IHardwareOpusDecoder). */
#include "../ipc.h"
#include <stdlib.h>
#include <opus/opus.h>
#include <opus/opus_multistream.h>

typedef struct OpusDec {
    u32 rate, channels;
    OpusDecoder *one;                   /* one stream, or */
    OpusMSDecoder *ms;                  /* several (multistream) */
    u32 errors;
} OpusDec;

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

static void reset(OpusDec *d) {
    if (d->one) opus_decoder_ctl(d->one, OPUS_RESET_STATE);
    if (d->ms) opus_multistream_decoder_ctl(d->ms, OPUS_RESET_STATE);
}

/* in: A (or X) packets, out: B (or C) PCM16 interleaved; replies consumed bytes, samples (per channel) */
static void decode(SvcObj *o, IpcReq *r, bool perf) {
    OpusDec *d = o->data;
    const IpcBuf *in = buf_in(r, 0), *out = buf_out(r, 0);
    /* the ...AndReset commands (6, 7) and DecodeInterleaved (8, 9) carry a reset flag */
    if (r->cmd >= 6 && in_u8(r, 0)) reset(d);
    u32 consumed = 0, samples = 0;
    u32 room = out && out->size ? (u32)(out->size / (d->channels * 2)) : 0;
    if (in && in->size >= 8) {
        const u8 *p = (const u8 *)(uintptr_t)in->addr;
        u32 size = be32(p);
        if (8 + (u64)size <= in->size) {
            consumed = 8 + size;
            int n = -1;
            if (room) {
                opus_int16 *pcm = (opus_int16 *)(uintptr_t)out->addr;
                n = d->ms ? opus_multistream_decode(d->ms, p + 8, (opus_int32)size, pcm, (int)room, 0)
                          : d->one ? opus_decode(d->one, p + 8, (opus_int32)size, pcm, (int)room, 0) : -1;
            }
            if (n >= 0) samples = (u32)n;
            else {
                samples = packet_samples(p + 8, size, d->rate);
                if (samples > room) samples = room;
                if (room) memset((void *)(uintptr_t)out->addr, 0, (size_t)samples * d->channels * 2);
                if (d->errors++ < 4) hlog(0, "hwopus: packet of %u bytes not decoded (%d): silence", size, n);
            }
        }
    }
    out_u32(r, consumed);
    out_u32(r, samples);
    if (perf) out_u64(r, 0);
}
static void dec_old(SvcObj *o, IpcReq *r) { decode(o, r, false); }
static void dec_perf(SvcObj *o, IpcReq *r) { decode(o, r, true); }
static void dec_destroy(SvcObj *o) {
    OpusDec *d = o->data;
    if (d->one) opus_decoder_destroy(d->one);
    if (d->ms) opus_multistream_decoder_destroy(d->ms);
    free(d);
}

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

static bool valid_rate(u32 rate) { return rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000; }

/* OpusParameters(Ex): u32 sample rate, u32 channel count[, u8 large frame size], as raw data.
 * MultiStream (2, 6): a buffer {u32 rate, channels, streams, coupled (stereo) streams, [Ex: u8 large frame size,
 * 7 bytes padding,] u8 mapping[256]} (layout from the SDK's proxies as other emulators describe it: unverified) */
static void open_decoder(SvcObj *o, IpcReq *r) {
    OpusDec *d = calloc(1, sizeof *d);
    d->rate = in_u32(r, 0) ? in_u32(r, 0) : 48000;
    d->channels = in_u32(r, 4) ? in_u32(r, 4) : 1;
    bool multi = r->cmd == 2 || r->cmd == 6;
    u32 streams = 0, coupled = 0;
    const u8 *mapping = NULL;
    const IpcBuf *ms = buf_in(r, 0);
    if (multi && ms && ms->size >= 16) {
        const u8 *p = (const u8 *)(uintptr_t)ms->addr;
        memcpy(&d->rate, p, 4); memcpy(&d->channels, p + 4, 4);
        memcpy(&streams, p + 8, 4); memcpy(&coupled, p + 12, 4);
        u32 moff = r->cmd == 6 ? 0x18 : 0x10;
        if (ms->size >= moff + d->channels) mapping = p + moff;
    }
    if (!d->channels || d->channels > 255) d->channels = 2;
    int err = OPUS_BAD_ARG;
    if (valid_rate(d->rate)) {
        if (multi && mapping && streams && streams <= 255 && coupled <= streams)
            d->ms = opus_multistream_decoder_create((opus_int32)d->rate, (int)d->channels, (int)streams, (int)coupled,
                                                    mapping, &err);
        else if (!multi && d->channels <= 2)
            d->one = opus_decoder_create((opus_int32)d->rate, (int)d->channels, &err);
    }
    if (d->one || d->ms)
        hlog(1, "hwopus: decoder %u Hz, %u channels%s", d->rate, d->channels, d->ms ? " (multistream)" : "");
    else
        hlog(0, "hwopus: decoder %u Hz, %u channels%s not created (%d): silence", d->rate, d->channels,
             multi ? " (multistream)" : "", err);
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
