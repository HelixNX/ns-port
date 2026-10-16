/* The audio renderer's frame: what the console's DSP does with the command list the renderer builds, done directly
 * from the renderer state (audio_dsp.h). Each frame:
 *   voices   each started voice's channels are decoded from their wave buffers (PCM16, float, DSP-ADPCM), resampled
 *            to the renderer's rate (cubic), filtered (two biquads), ramped from the last frame's volume and mix
 *            volumes to this frame's and added into their mix's buffers. A voice that stops or pauses leaves a
 *            decaying copy of its last samples (depop) so it doesn't click.
 *   mixes    farthest from the final mix first: depop, effects in their process order, then into the destination
 *            mix through the volume matrix. The final mix applies its volume.
 *   sinks    the device sink's channels (6 are downmixed) go to the host's audio device; circular buffer sinks
 *            are written to guest memory as 16-bit planes.
 * Also the host's audio device: an SDL3 stream that the renderer thread keeps a few frames ahead of the device. */
#include "audio_dsp.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <math.h>
#include <windows.h>

#define MAX_RATIO 16                        /* source samples per output sample */

static inline float clamp16(float v) { return v < -32768.0f ? -32768.0f : v > 32767.0f ? 32767.0f : v; }

/* ---------------------------------------------------------------------------------------------- decoding */
/* decodes n samples of channel ch from wave buffer w, from sample start + pos */
static u32 decode(const ArVoice *v, ArChannel *cs, int ch, const ArWaveBuf *w, u32 pos, u32 n, float *out) {
    if (!w->addr || !n) return 0;
    u64 first = (u64)w->start + pos;
    u32 chans = v->channels > 0 ? (u32)v->channels : 1;
    switch (v->format) {
    case AR_PCM16: {
        if ((first + n) * chans * 2 > w->size) return 0;
        const s16 *p = (const s16 *)(uintptr_t)w->addr + first * chans + ch;
        for (u32 i = 0; i < n; i++) out[i] = p[i * chans];
        return n;
    }
    case AR_PCMFLOAT: {
        if ((first + n) * chans * 4 > w->size) return 0;
        const float *p = (const float *)(uintptr_t)w->addr + first * chans + ch;
        for (u32 i = 0; i < n; i++) out[i] = clamp16(p[i * chans] * 32767.0f);
        return n;
    }
    case AR_ADPCM: {                        /* 8-byte frames: header (scale, coefficient pair), 14 nibbles */
        u64 last = first + n - 1;
        if ((last / 14) * 8 + 1 + (last % 14) / 2 >= w->size || !v->data_addr || v->data_size < 32) return 0;
        const s16 *coef = (const s16 *)(uintptr_t)v->data_addr;
        const u8 *p = (const u8 *)(uintptr_t)w->addr;
        s32 yn1 = cs->yn1, yn2 = cs->yn2;
        for (u32 i = 0; i < n; i++) {
            u64 s = first + i;
            const u8 *f = p + (s / 14) * 8;
            u32 k = (u32)(s % 14);
            s32 scale = 1 << (f[0] & 15), c0 = coef[(f[0] >> 4 & 7) * 2], c1 = coef[(f[0] >> 4 & 7) * 2 + 1];
            s32 nib = k & 1 ? f[1 + k / 2] & 15 : f[1 + k / 2] >> 4;
            if (nib >= 8) nib -= 16;
            s32 y = ((nib * scale) * 2048 + 0x400 + c0 * yn1 + c1 * yn2) >> 11;
            y = y < -32768 ? -32768 : y > 32767 ? 32767 : y;
            yn2 = yn1;
            yn1 = y;
            out[i] = (float)y;
        }
        cs->yn1 = (s16)yn1;
        cs->yn2 = (s16)yn2;
        return n;
    }
    }
    return 0;
}

static void end_wavebuffer(const ArVoice *v, ArChannel *cs) {
    if (v->wb[cs->index].stream_ended) cs->played = 0;
    cs->valid[cs->index] = false;
    cs->index = (cs->index + 1) % AR_WAVEBUFFERS;
    cs->consumed++;
}

/* one channel of a voice for a frame: reads the source samples the frame needs from the queued wave buffers (zeros
 * once they run out) and resamples them. tmp[0..3] holds the last four samples read, so output k sits at
 * 1 + frac + k * ratio in tmp and a cubic needs tmp[i - 1 .. i + 2]. */
static void play_channel(ArRenderer *r, ArVoice *v, int ch, ArChannel *cs, float *out) {
    static __declspec(thread) float tmp[4 + AR_FRAME_MAX * MAX_RATIO + 4];
    u32 n = r->sample_count;
    double ratio = (double)(v->rate ? v->rate : r->sample_rate) / r->sample_rate * v->pitch;
    if (!(ratio > 0)) ratio = 0;
    if (ratio > MAX_RATIO) ratio = MAX_RATIO;
    double end = cs->frac + n * ratio;
    u32 need = (u32)end, got = 0;
    memcpy(tmp, cs->hist, sizeof cs->hist);
    while (got < need) {
        if (!cs->valid[cs->index]) break;   /* starved */
        const ArWaveBuf *w = &v->wb[cs->index];
        if (cs->offset == 0 && v->format == AR_ADPCM && w->ctx_addr && w->ctx_size >= 6) {
            const s16 *ctx = (const s16 *)(uintptr_t)w->ctx_addr;   /* {header, yn1, yn2} */
            cs->yn1 = ctx[1];
            cs->yn2 = ctx[2];
        }
        u32 len = w->end > w->start ? (u32)(w->end - w->start) : 0, k = 0;
        if (cs->offset < len) {
            k = need - got < len - cs->offset ? need - got : len - cs->offset;
            k = decode(v, cs, ch, w, cs->offset, k, tmp + 4 + got);
        }
        cs->played += k;
        got += k;
        cs->offset += k;
        if (k && cs->offset < len) continue;
        cs->offset = 0;
        if (!w->loop || !k) end_wavebuffer(v, cs);  /* a looping buffer plays until the game flushes or stops it */
        if (!k) break;
    }
    for (u32 i = got; i < need; i++) tmp[4 + i] = 0;
    for (u32 k = 0; k < n; k++) {
        double x = 1 + cs->frac + k * ratio;
        u32 i = (u32)x;
        float t = (float)(x - i), p0 = tmp[i - 1], p1 = tmp[i], p2 = tmp[i + 1], p3 = tmp[i + 2];
        out[k] = p1 + 0.5f * t * (p2 - p0 + t * (2 * p0 - 5 * p1 + 4 * p2 - p3 + t * (3 * (p1 - p2) + p3 - p0)));
    }
    cs->frac = end - need;
    memcpy(cs->hist, tmp + need, sizeof cs->hist);
}

/* y = b0 x + b1 x1 + b2 x2 + a0 y1 + a1 y2 (the a coefficients come negated), Q14 coefficients */
static void biquad(double *s, const s16 *b, const s16 *a, float *in, float *out, u32 n) {
    double b0 = b[0] / 16384.0, b1 = b[1] / 16384.0, b2 = b[2] / 16384.0, a0 = a[0] / 16384.0, a1 = a[1] / 16384.0;
    for (u32 i = 0; i < n; i++) {
        double x = in[i], y = b0 * x + b1 * s[0] + b2 * s[1] + a0 * s[2] + a1 * s[3];
        s[1] = s[0];
        s[0] = x;
        s[3] = s[2];
        s[2] = y;
        out[i] = (float)y;
    }
}

static ArMix *voice_mix(ArRenderer *r, const ArVoice *v) {
    if (v->mix_id < 0 || (u32)v->mix_id >= r->mix_count || !r->mixes[v->mix_id].in_use) return NULL;
    return &r->mixes[v->mix_id];
}

static void voice_frame(ArRenderer *r, ArVoice *v) {
    if (!v->in_use || !v->wb_count || v->channels <= 0 || v->channels > AR_CHANNELS) return;
    for (int c = 0; c < v->channels; c++)
        if (v->res[c] >= r->voice_count) return;
    ArChannel *c0 = &r->chans[v->res[0]];
    bool play = false;
    switch (v->state) {
    case AR_STARTED:
        for (int i = 0; i < AR_WAVEBUFFERS; i++)
            if (!v->wb[i].sent) {
                v->wb[i].sent = true;
                for (int c = 0; c < v->channels; c++) r->chans[v->res[c]].valid[i] = true;
            }
        v->was_playing = false;
        for (int i = 0; i < AR_WAVEBUFFERS; i++) play |= c0->valid[i];
        break;
    case AR_STOPPED:
    case AR_PAUSED:
        v->was_playing = v->last_state == AR_STARTED;
        break;
    case AR_REQUEST_STOP:                   /* every queued buffer counts as consumed; playback starts over */
        for (int i = 0; i < AR_WAVEBUFFERS; i++) {
            v->wb[i].sent = true;
            for (int c = 0; c < v->channels; c++) {
                ArChannel *cs = &r->chans[v->res[c]];
                if (cs->valid[i]) {
                    cs->index = (cs->index + 1) % AR_WAVEBUFFERS;
                    cs->consumed++;
                }
                cs->valid[i] = false;
            }
        }
        for (int c = 0; c < v->channels; c++) {
            ArChannel *cs = &r->chans[v->res[c]];
            cs->offset = 0;
            cs->played = 0;
            cs->frac = 0;
            cs->yn1 = cs->yn2 = 0;
            memset(cs->hist, 0, sizeof cs->hist);
        }
        v->state = AR_STOPPED;
        v->was_playing = v->last_state == AR_STARTED;
        break;
    }
    ArMix *m = voice_mix(r, v);
    if (v->was_playing) {                   /* stopped or paused this frame: fade out where it left off */
        v->last_state = v->state;
        v->prev_volume = 0;
        for (int c = 0; c < v->channels; c++) {
            ArChannel *cs = &r->chans[v->res[c]];
            if (m)
                for (u32 i = 0; i < m->buffers; i++) r->depop[m->offset + i] += cs->last[i];
            memset(cs->last, 0, sizeof cs->last);
        }
        return;
    }
    if (!play) return;
    u32 n = r->sample_count;
    float s[AR_FRAME_MAX];
    for (int c = 0; c < v->channels; c++) {
        ArChannel *cs = &r->chans[v->res[c]];
        play_channel(r, v, c, cs, s);
        if (!m) continue;
        for (int b = 0; b < 2; b++) {
            if (!v->bq[b].enabled) continue;
            if (!v->bq_on[b]) memset(cs->bq[b], 0, sizeof cs->bq[b]);
            biquad(cs->bq[b], v->bq[b].b, v->bq[b].a, s, s, n);
        }
        float dv = (v->volume - v->prev_volume) / n;
        for (u32 k = 0; k < n; k++) s[k] *= v->prev_volume + dv * k;
        for (u32 i = 0; i < m->buffers; i++) {
            float g = cs->prev_vol[i], dg = (cs->vol[i] - g) / n;
            if (g == 0 && dg == 0) { cs->last[i] = 0; continue; }
            float *d = r->buf + (size_t)(m->offset + i) * n;
            for (u32 k = 0; k < n; k++) d[k] += s[k] * (g + dg * k);
            cs->last[i] = s[n - 1] * cs->vol[i];
        }
        memcpy(cs->prev_vol, cs->vol, sizeof cs->vol);
    }
    v->bq_on[0] = v->bq[0].enabled;
    v->bq_on[1] = v->bq[1].enabled;
    v->prev_volume = v->volume;
}

/* ---------------------------------------------------------------------------------------------- effects */
static float *mixbuf(ArRenderer *r, const ArMix *m, s32 i) {
    return i >= 0 && (u32)i < m->buffers ? r->buf + (size_t)(m->offset + i) * r->sample_count : NULL;
}

/* Aux: the game's send/return rings of s32 samples (info at +0x40 is the DSP's {read, write, lost, total}, the
 * samples from +0x80); channel i of a frame goes at i * n past the ring's position, which advances once per frame */
static void aux_ring(u64 base, u32 max, u32 at, s32 *frame, u32 n, bool write) {
    u32 *info = (u32 *)(uintptr_t)(base + 0x40);
    s32 *ring = (s32 *)(uintptr_t)(base + 0x80);
    u32 pos = (write ? info[1] : info[0]) + at;
    for (u32 k = 0; k < n; k++) {
        u32 p = (pos + k) % max;
        if (write) ring[p] = frame[k];
        else frame[k] = ring[p];
    }
}

static void fx_aux(ArRenderer *r, ArMix *m, ArEffect *e) {
    const s8 *ins = (const s8 *)e->param, *outs = (const s8 *)e->param + 0x18;
    u32 chans, max;
    memcpy(&chans, e->param + 0x30, 4);
    memcpy(&max, e->param + 0x38, 4);
    if (chans > AR_MIX_BUFFERS) chans = AR_MIX_BUFFERS;
    u32 n = r->sample_count;
    if (!e->aux_send || !e->aux_return) return;
    if (!e->enabled) {
        for (int h = 0; h < 2; h++) {
            u32 *info = (u32 *)(uintptr_t)((h ? e->aux_return : e->aux_send) + 0x40);
            info[0] = info[1] = info[3] = 0;
        }
        for (u32 c = 0; c < chans; c++) {
            float *in = mixbuf(r, m, ins[c]), *out = mixbuf(r, m, outs[c]);
            if (in && out && in != out) memcpy(out, in, n * sizeof(float));
        }
        return;
    }
    if (!max || n * chans > max) return;
    s32 frame[AR_FRAME_MAX];
    for (u32 c = 0; c < chans; c++) {
        float *in = mixbuf(r, m, ins[c]), *out = mixbuf(r, m, outs[c]);
        if (!in || !out) continue;
        for (u32 k = 0; k < n; k++) frame[k] = (s32)lrintf(in[k]);
        aux_ring(e->aux_send, max, c * n, frame, n, true);
        aux_ring(e->aux_return, max, c * n, frame, n, false);
        for (u32 k = 0; k < n; k++) out[k] = (float)frame[k];
    }
    u32 *send = (u32 *)(uintptr_t)(e->aux_send + 0x40), *ret = (u32 *)(uintptr_t)(e->aux_return + 0x40);
    send[1] = (send[1] + n * chans) % max;
    ret[0] = (ret[0] + n * chans) % max;
}

static void fx_buffer_mixer(ArRenderer *r, ArMix *m, ArEffect *e) {
    if (!e->enabled) return;
    const s8 *ins = (const s8 *)e->param, *outs = (const s8 *)e->param + 0x18;
    const float *vol = (const float *)(e->param + 0x30);
    u32 count;
    memcpy(&count, e->param + 0x90, 4);
    for (u32 i = 0; i < count && i < AR_MIX_BUFFERS; i++) {
        float *in = mixbuf(r, m, ins[i]), *out = mixbuf(r, m, outs[i]);
        if (!in || !out || vol[i] == 0) continue;
        for (u32 k = 0; k < r->sample_count; k++) out[k] += in[k] * vol[i];
    }
}

/* the effects with up to six channels: inputs, outputs (s8 each) at +0, +6 and the channel count at +0xe */
static void fx_channels(ArRenderer *r, ArMix *m, ArEffect *e) {
    float *in[AR_CHANNELS] = {0}, *out[AR_CHANNELS] = {0};
    u16 chans;
    memcpy(&chans, e->param + 0xe, 2);
    if (e->type == FX_BIQUAD) chans = (u8)e->param[0x16];
    if (chans > AR_CHANNELS) chans = AR_CHANNELS;
    for (u32 c = 0; c < chans; c++) {
        in[c] = mixbuf(r, m, (s8)e->param[c]);
        out[c] = mixbuf(r, m, (s8)e->param[6 + c]);
        if (!in[c] || !out[c]) return;
    }
    u32 n = r->sample_count;
    if (!e->enabled) {                      /* bypassed; starts afresh when enabled again */
        for (u32 c = 0; c < chans; c++)
            if (in[c] != out[c]) memcpy(out[c], in[c], n * sizeof(float));
        ar_effect_reset(e);
        return;
    }
    if (chans == 6 && e->type != FX_BIQUAD && ((r->revision >> 24) - '0') < 11) {
        float *t[4] = {in[2], in[3], out[2], out[3]};   /* FL FR BL BR C LFE before REV11 */
        in[2] = in[4], in[3] = in[5], in[4] = t[0], in[5] = t[1];
        out[2] = out[4], out[3] = out[5], out[4] = t[2], out[5] = t[3];
    }
    switch (e->type) {
    case FX_REVERB: fx_reverb(e, r->sample_rate, in, out, n); break;
    case FX_I3DL2: fx_i3dl2(e, r->sample_rate, in, out, n); break;
    case FX_DELAY: fx_delay(e, r->sample_rate, in, out, n); break;
    case FX_BIQUAD:
        if (!e->state) e->state = calloc(AR_CHANNELS * 4, sizeof(double));
        for (u32 c = 0; c < chans; c++)
            biquad((double *)e->state + c * 4, (const s16 *)(e->param + 0xc), (const s16 *)(e->param + 0x12), in[c],
                   out[c], n);
        break;
    }
}

void ar_effect_reset(ArEffect *e) {
    fx_free(e);
    free(e->state);
    e->state = NULL;
}

static void mix_effects(ArRenderer *r, u32 id) {
    ArMix *m = &r->mixes[id];
    s32 last = -1;
    for (;;) {                              /* in process order: the next larger order each time */
        ArEffect *next = NULL;
        for (u32 i = 0; i < r->effect_count; i++) {
            ArEffect *e = &r->effects[i];
            if (e->type == FX_NONE || e->mix_id != (s32)id || e->order <= last) continue;
            if (!next || e->order < next->order) next = e;
        }
        if (!next) return;
        last = next->order;
        switch (next->type) {
        case FX_AUX: fx_aux(r, m, next); break;
        case FX_MIX: fx_buffer_mixer(r, m, next); break;
        default: fx_channels(r, m, next); break;
        }
        next->changed = false;
    }
}

/* ---------------------------------------------------------------------------------------------- mixes, sinks */
static void mix_layout(ArRenderer *r) {
    for (u32 i = 0; i < r->mix_count; i++) {
        ArMix *m = &r->mixes[i];
        m->dist = -1;
        if (!m->in_use) continue;
        int d = 0;
        for (u32 j = i; j != 0 && d <= (int)r->mix_count; d++) {
            s32 next = r->mixes[j].dest;
            if (next < 0 || (u32)next >= r->mix_count || !r->mixes[next].in_use) { d = -1; break; }
            j = (u32)next;
        }
        m->dist = d > (int)r->mix_count ? -1 : d;
    }
    u32 n = 0, offset = 0;
    for (int d = (int)r->mix_count; d >= 0; d--)
        for (u32 i = 0; i < r->mix_count; i++)
            if (r->mixes[i].dist == d) r->order[n++] = (int)i;
    for (u32 i = 0; i < r->mix_count; i++) {
        ArMix *m = &r->mixes[i];
        m->offset = offset;
        if (m->in_use) {
            if (m->buffers > AR_MIX_BUFFERS) m->buffers = AR_MIX_BUFFERS;
            if (offset + m->buffers > r->mix_buffers) m->buffers = r->mix_buffers - offset;
            offset += m->buffers;
        }
    }
    for (; n < r->mix_count; n++) r->order[n] = -1;
    r->layout_dirty = false;
}

static void depop(ArRenderer *r, ArMix *m) {
    float decay = r->sample_rate >= 48000 ? 0.962189f : 0.943695f;
    for (u32 i = 0; i < m->buffers; i++) {
        float *dp = &r->depop[m->offset + i], v = *dp;
        if (v == 0) continue;
        float *d = r->buf + (size_t)(m->offset + i) * r->sample_count;
        for (u32 k = 0; k < r->sample_count; k++) {
            d[k] += v;
            v *= decay;
        }
        *dp = fabsf(v) < 1.0f / 256 ? 0 : v;
    }
}

static void device_sink(ArRenderer *r, const ArSink *s) {
    const ArMix *f = &r->mixes[0];
    u32 n = r->sample_count;
    const float *ch[AR_CHANNELS] = {0};
    for (u32 c = 0; c < s->count && c < AR_CHANNELS; c++) {
        ch[c] = mixbuf(r, f, s->inputs[c]);
        if (!ch[c]) return;
    }
    s16 lr[AR_FRAME_MAX * 2];
    for (u32 k = 0; k < n; k++) {
        float l = 0, rr = 0;
        if (s->count >= 6) {
            const float *cf = s->coeff;
            l = ch[0][k] * cf[0] + ch[2][k] * cf[1] + ch[3][k] * cf[2] + ch[4][k] * cf[3];
            rr = ch[1][k] * cf[0] + ch[2][k] * cf[1] + ch[3][k] * cf[2] + ch[5][k] * cf[3];
        } else if (s->count >= 2) {
            l = ch[0][k];
            rr = ch[1][k];
        } else if (s->count == 1) {
            l = rr = ch[0][k];
        }
        lr[2 * k] = (s16)lrintf(clamp16(l));
        lr[2 * k + 1] = (s16)lrintf(clamp16(rr));
    }
    if (r->sample_rate == 48000) audio_output(lr, n);
}

static void circular_sink(ArRenderer *r, ArSink *s) {
    const ArMix *f = &r->mixes[0];
    u32 n = r->sample_count;
    if (!s->addr || s->size < n * 2) return;
    for (u32 c = 0; c < s->count && c < AR_CHANNELS; c++) {
        const float *in = mixbuf(r, f, s->inputs[c]);
        if (s->pos + n * 2 > s->size) s->pos = 0;
        s16 *o = (s16 *)(uintptr_t)(s->addr + s->pos);
        for (u32 k = 0; k < n; k++) o[k] = in ? (s16)lrintf(clamp16(in[k])) : 0;
        s->pos += n * 2;
        if (s->pos >= s->size) s->pos = 0;
    }
}

void ar_render_frame(ArRenderer *r) {
    u32 n = r->sample_count;
    if (n > AR_FRAME_MAX || !r->mix_count) return;
    if (r->layout_dirty) mix_layout(r);
    memset(r->buf, 0, (size_t)r->mix_buffers * n * sizeof(float));
    for (u32 i = 0; i < r->voice_count; i++) voice_frame(r, &r->voices[i]);
    for (u32 o = 0; o < r->mix_count; o++) {
        int id = r->order[o];
        if (id < 0) break;
        ArMix *m = &r->mixes[id];
        depop(r, m);
        mix_effects(r, (u32)id);
        if (id == 0) {
            for (u32 i = 0; i < m->buffers; i++) {
                float *d = mixbuf(r, m, (s32)i);
                for (u32 k = 0; k < n; k++) d[k] *= m->volume;
            }
            continue;
        }
        ArMix *dst = &r->mixes[m->dest];
        for (u32 i = 0; i < m->buffers; i++) {
            const float *src = mixbuf(r, m, (s32)i);
            for (u32 j = 0; j < dst->buffers; j++) {
                float g = m->volume * m->vol[i][j];
                if (g == 0) continue;
                float *d = mixbuf(r, dst, (s32)j);
                for (u32 k = 0; k < n; k++) d[k] += src[k] * g;
            }
        }
    }
    bool device = false;
    for (u32 i = 0; i < r->sink_count; i++) {
        ArSink *s = &r->sinks[i];
        if (!s->in_use) continue;
        if (s->type == SINK_DEVICE && !device) {
            device_sink(r, s);
            device = true;
        } else if (s->type == SINK_CIRCULAR) circular_sink(r, s);
    }
    if (!device) {                          /* keep the device fed */
        static const s16 silence[AR_FRAME_MAX * 2];
        if (r->sample_rate == 48000) audio_output(silence, n);
    }
    if (++r->frames % 1000 == 0 && log_level >= 1) {   /* every 5 s */
        u32 in_use = 0, started = 0, playing = 0;
        for (u32 i = 0; i < r->voice_count; i++) {
            const ArVoice *v = &r->voices[i];
            in_use += v->in_use;
            started += v->in_use && v->state == AR_STARTED;
            if (v->in_use && v->state == AR_STARTED && v->res[0] < r->voice_count)
                for (int b = 0; b < AR_WAVEBUFFERS; b++)
                    if (r->chans[v->res[0]].valid[b]) { playing++; break; }
        }
        float peak = 0;
        for (size_t i = 0; i < (size_t)r->mixes[0].buffers * n; i++) peak = fmaxf(peak, fabsf(r->buf[r->mixes[0].offset * n + i]));
        hlog(1, "audren: frame %llu: %u voices in use, %u started, %u with data; final mix volume %.2f, peak %.0f",
             r->frames, in_use, started, playing, r->mixes[0].volume, peak);
    }
}

/* ---------------------------------------------------------------------------------------------- host device */
static SDL_AudioStream *stream;
static FILE *dump;                          /* MK8_AUDIO_DUMP=<file.wav>: everything sent to the device */
static u32 dumped;
static SRWLOCK out_lock = SRWLOCK_INIT;

static void dump_header(void) {
    u32 data = dumped * 4;
    u8 h[44] = "RIFF....WAVEfmt \x10\0\0\0\x01\0\x02\0\x80\xbb\0\0\0\xee\x02\0\x04\0\x10\0data";
    u32 riff = 36 + data;
    memcpy(h + 4, &riff, 4);
    memcpy(h + 40, &data, 4);
    fseek(dump, 0, SEEK_SET);
    fwrite(h, 1, 44, dump);
    fseek(dump, 0, SEEK_END);
}

void audio_init(void) {
    const char *path = getenv("MK8_AUDIO_DUMP");
    if (path && *path && (dump = fopen(path, "wb"))) dump_header();
    if (getenv("MK8_NOAUDIO")) return;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        hlog(0, "audio: no SDL audio: %s", SDL_GetError());
        return;
    }
    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 48000};
    stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (!stream) {
        hlog(0, "audio: no output device: %s", SDL_GetError());
        return;
    }
    SDL_ResumeAudioStreamDevice(stream);
    hlog(1, "audio: output on %s", SDL_GetAudioDeviceName(SDL_GetAudioStreamDevice(stream)));
}

u32 audio_queued_frames(u32 sample_count) {
    if (!stream || !sample_count) return ~0u;
    int q = SDL_GetAudioStreamQueued(stream);
    return q < 0 ? ~0u : (u32)q / (sample_count * 4);
}

void audio_output(const s16 *lr, u32 n) {
    AcquireSRWLockExclusive(&out_lock);
    if (stream) SDL_PutAudioStreamData(stream, lr, (int)(n * 4));
    if (dump) {
        fwrite(lr, 4, n, dump);
        dumped += n;
        if (dumped % (48000 * 2) < n) {     /* keep the header current every 2 s */
            dump_header();
            fflush(dump);
        }
    }
    ReleaseSRWLockExclusive(&out_lock);
}
