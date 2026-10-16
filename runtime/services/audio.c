/* audren:u: the audio renderer. RequestUpdate takes the game's update data (header, then behaviour, memory pools,
 * voice channel resources, voices, effects, splitters, mixes, sinks, performance) into the renderer state
 * (audio_dsp.h) and answers with each part's status. A host thread per renderer renders frames (audio_dsp.c) as
 * the host's audio device needs them, one every 5 ms, and signals the renderer's event after each.
 * Also IAudioDevice (device names, volume). */
#include "../ipc.h"
#include "../audio_dsp.h"
#include <stdlib.h>
#include <windows.h>
#include <process.h>

/* ---------------------------------------------------------------------------------------------- update data */
typedef struct UpdateHeader {
    u32 revision, behaviour_size, memory_pool_size, voices_size, voice_resources_size, effects_size, mix_size,
        sinks_size, performance_size, splitter_size, render_info_size, unk[4], size;
} UpdateHeader;

enum { PLAY_STARTED, PLAY_STOPPED, PLAY_PAUSED };  /* as the game sets them */
enum { POOL_REQUEST_DETACH = 2, POOL_DETACHED = 3, POOL_REQUEST_ATTACH = 4, POOL_ATTACHED = 5 };
enum { EFFECT_USED = 3, EFFECT_REMOVED = 4 };

#define VOICE_IN 0x170
#define RESOURCE_IN 0x70
#define EFFECT_IN 0xc0
#define MIX_IN 0x930
#define SINK_IN 0x140

static bool trace;                         /* MK8_AUDIO_TRACE: voices as the game sets them up */

static u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u64 rd64(const u8 *p) { u64 v; memcpy(&v, p, 8); return v; }

static void reset_channel(ArChannel *cs) {
    float vol[AR_MIX_BUFFERS];
    memcpy(vol, cs->vol, sizeof vol);
    memset(cs, 0, sizeof *cs);
    memcpy(cs->vol, vol, sizeof vol);
    memcpy(cs->prev_vol, vol, sizeof vol);
}

static void update_voice(ArRenderer *r, const u8 *in, u32 rev) {
    u32 id = rd32(in);
    if (id >= r->voice_count) return;
    ArVoice *v = &r->voices[id];
    bool is_new = in[8];
    u32 chans = rd32(in + 0x18);
    if (chans > AR_CHANNELS) chans = AR_CHANNELS;
    if (is_new) {
        memset(v, 0, sizeof *v);
        v->state = AR_STOPPED;
        v->last_state = AR_STARTED;
        for (int i = 0; i < AR_WAVEBUFFERS; i++) v->wb[i].sent = true;
    }
    v->in_use = true;
    switch (in[0xa]) {                      /* stopping takes a frame: buffers are released then */
    case PLAY_STARTED: v->last_state = v->state; v->state = AR_STARTED; break;
    case PLAY_STOPPED: v->last_state = v->state; if (v->state != AR_STOPPED) v->state = AR_REQUEST_STOP; break;
    case PLAY_PAUSED: v->last_state = v->state; v->state = AR_PAUSED; break;
    }
    v->format = in[0xb];
    v->rate = rd32(in + 0xc);
    v->channels = (s8)chans;
    memcpy(&v->pitch, in + 0x1c, 4);
    memcpy(&v->volume, in + 0x20, 4);
    for (int b = 0; b < 2; b++) {
        const u8 *q = in + 0x24 + b * 0xc;
        v->bq[b].enabled = q[0];
        memcpy(v->bq[b].b, q + 2, 6);
        memcpy(v->bq[b].a, q + 8, 4);
    }
    v->wb_count = rd32(in + 0x3c);
    v->data_addr = rd64(in + 0x48);
    v->data_size = rd64(in + 0x50);
    v->mix_id = (s32)rd32(in + 0x58);
    for (u32 c = 0; c < AR_CHANNELS; c++) v->res[c] = rd32(in + 0x140 + c * 4);
    if (is_new && trace)
        hlog(0, "audren: voice %u new: format %u, %u Hz, %u channels, pitch %.2f, volume %.2f, mix %d, state %u", id,
             v->format, v->rate, chans, v->pitch, v->volume, v->mix_id, in[0xa]);
    if (is_new) {
        v->prev_volume = v->volume;
        for (u32 c = 0; c < chans; c++)
            if (v->res[c] < r->voice_count) reset_channel(&r->chans[v->res[c]]);
    }
    for (int i = 0; i < AR_WAVEBUFFERS; i++) {  /* new wave buffers: the ones the game hasn't seen sent */
        const u8 *w = in + 0x60 + i * 0x38;
        if (w[0x1a]) continue;
        ArWaveBuf *b = &v->wb[i];
        b->addr = rd64(w);
        b->size = rd64(w + 8);
        b->start = (s32)rd32(w + 0x10);
        b->end = (s32)rd32(w + 0x14);
        b->loop = w[0x18];
        b->stream_ended = w[0x19];
        b->ctx_addr = rd64(w + 0x20);
        b->ctx_size = rd64(w + 0x28);
        b->sent = false;
        if (trace)
            hlog(0, "audren: voice %u buffer %d: %llx size %llx, samples %d-%d%s%s", id, i, b->addr, b->size, b->start,
                 b->end, b->loop ? ", loop" : "", b->stream_ended ? ", end" : "");
    }
    u8 flush = rev >= 5 ? in[0x159] : 0;    /* dropped by the game: count them as consumed */
    for (u32 wi = rd32(in + 0x40) & 0xffff; flush; flush--, wi = (wi + 1) % AR_WAVEBUFFERS) {
        if (wi >= AR_WAVEBUFFERS) break;
        v->wb[wi].sent = true;
        for (u32 c = 0; c < chans; c++) {
            if (v->res[c] >= r->voice_count) continue;
            ArChannel *cs = &r->chans[v->res[c]];
            if (cs->index == wi) {
                cs->index = (cs->index + 1) % AR_WAVEBUFFERS;
                cs->consumed++;
            }
            cs->valid[wi] = false;
        }
    }
}

static void update_effect(ArRenderer *r, ArEffect *e, const u8 *in) {
    u8 type = in[0];
    if (type != e->type || in[1]) {
        ar_effect_reset(e);
        memset(e, 0, sizeof *e);
        e->type = type;
        e->is_new = true;
        e->changed = true;
    }
    e->enabled = in[2];
    e->mix_id = (s32)rd32(in + 4);
    e->wb_addr = rd64(in + 8);
    e->wb_size = rd64(in + 0x10);
    e->order = (s32)rd32(in + 0x18);
    if (memcmp(e->param, in + 0x20, sizeof e->param)) {
        memcpy(e->param, in + 0x20, sizeof e->param);
        e->changed = true;
    }
    if (type == FX_AUX) {
        e->aux_send = rd64(e->param + 0x40);
        e->aux_return = rd64(e->param + 0x50);
    }
    if (e->is_new && type != FX_NONE) {
        static const char *const names[] = {"none", "mix", "aux", "delay", "reverb", "i3dl2", "biquad"};
        hlog(1, "audren: effect %u: %s on mix %d, order %d, %s, inputs %d %d %d %d %d %d",
             (u32)(e - r->effects), type < 7 ? names[type] : "?", e->mix_id, e->order, e->enabled ? "on" : "off",
             (s8)e->param[0], (s8)e->param[1], (s8)e->param[2], (s8)e->param[3], (s8)e->param[4], (s8)e->param[5]);
        e->is_new = false;
    }
}

static void update_mix(ArRenderer *r, ArMix *m, const u8 *in) {
    bool in_use = in[0xc];
    u32 buffers = rd32(in + 8);
    s32 dest = (s32)rd32(in + 0x924);
    if (in_use != m->in_use || buffers != m->buffers || dest != m->dest) r->layout_dirty = true;
    m->in_use = in_use;
    m->buffers = buffers;
    m->dest = dest;
    memcpy(&m->volume, in, 4);
    memcpy(m->vol, in + 0x24, sizeof m->vol);
}

static void update_sink(ArSink *s, const u8 *in) {
    s->type = in[0];
    s->in_use = in[1];
    const u8 *p = in + 0x20;
    if (s->type == SINK_DEVICE) {
        s->count = rd32(p + 0x100);
        memcpy(s->inputs, p + 0x104, AR_CHANNELS);
        s->downmix = p[0x10b];
        static const float plain[4] = {1.0f, 0.707f, 0.251f, 0.707f};
        if (s->downmix) memcpy(s->coeff, p + 0x10c, sizeof s->coeff);
        else memcpy(s->coeff, plain, sizeof s->coeff);
    } else if (s->type == SINK_CIRCULAR) {
        u64 addr = rd64(p);
        u32 size = rd32(p + 8);
        if (addr != s->addr || size != s->size) s->pos = 0;
        s->addr = addr;
        s->size = size;
        s->count = rd32(p + 0xc);
        s->samples = rd32(p + 0x10);
        memcpy(s->inputs, p + 0x1c, AR_CHANNELS);
    }
    if (s->count > AR_CHANNELS) s->count = AR_CHANNELS;
}

static u32 request_update(ArRenderer *r, const u8 *in, u64 in_size, u8 *out, u64 out_size) {
    if (in_size < sizeof(UpdateHeader)) return 0x29a;  /* audio: invalid update data */
    UpdateHeader h;
    memcpy(&h, in, sizeof h);
    u32 npools = h.memory_pool_size / 0x20, nres = h.voice_resources_size / RESOURCE_IN;
    u32 nvoices = h.voices_size / VOICE_IN, neffects = h.effects_size / EFFECT_IN;
    u32 nmixes = h.mix_size / MIX_IN, nsinks = h.sinks_size / SINK_IN;
    u32 rev = (h.revision >> 24) - '0';                 /* 'REVn' */
    bool render_info = rev >= 5;                        /* elapsed frame count: REV5 and later */
    u64 need = sizeof(UpdateHeader) + npools * 0x10 + nvoices * 0x10 + neffects * 0x10 + nsinks * 0x20 + 0x10 +
               0xb0 + (render_info ? 0x10 : 0);
    if (need != out_size) {
        static bool said;
        if (!said) hlog(0, "audren: update output of %llu bytes, expected %llu", out_size, need);
        said = true;
        if (need > out_size) return 0x29a;
    }
    if (sizeof(UpdateHeader) + (u64)h.behaviour_size + h.memory_pool_size + h.voice_resources_size + h.voices_size +
        h.effects_size + h.splitter_size + h.mix_size + h.sinks_size > in_size) return 0x29a;
    memset(out, 0, out_size);
    const u8 *ip = in + sizeof(UpdateHeader) + h.behaviour_size;
    u8 *op = out + sizeof(UpdateHeader);
    UpdateHeader oh = {0};
    oh.revision = h.revision;
    AcquireSRWLockExclusive(&r->lock);
    for (u32 i = 0; i < npools; i++, ip += 0x20, op += 0x10) {        /* memory pools: guest memory is ours */
        u32 st = rd32(ip + 0x10);
        if (st == POOL_REQUEST_ATTACH) st = POOL_ATTACHED;
        else if (st == POOL_REQUEST_DETACH) st = POOL_DETACHED;
        memcpy(op, &st, 4);
    }
    oh.memory_pool_size = npools * 0x10;
    for (u32 i = 0; i < nres; i++, ip += RESOURCE_IN)                  /* voice channel resources */
        if (i < r->voice_count && ip[0x64]) memcpy(r->chans[i].vol, ip + 4, sizeof r->chans[i].vol);
    for (u32 i = 0; i < r->voice_count; i++) r->voices[i].in_use = false;
    for (u32 i = 0; i < nvoices; i++, ip += VOICE_IN, op += 0x10) {   /* voices */
        if (!ip[9]) continue;
        update_voice(r, ip, rev);
        u32 id = rd32(ip);
        if (id >= r->voice_count || ip[8]) continue;
        const ArVoice *v = &r->voices[id];
        if (v->res[0] >= r->voice_count) continue;
        const ArChannel *cs = &r->chans[v->res[0]];
        memcpy(op, &cs->played, 8);
        memcpy(op + 8, &cs->consumed, 4);
    }
    oh.voices_size = nvoices * 0x10;
    for (u32 i = 0; i < neffects; i++, ip += EFFECT_IN, op += 0x10) { /* effects: in use or removable */
        if (i < r->effect_count) update_effect(r, &r->effects[i], ip);
        op[0] = ip[2] ? EFFECT_USED : EFFECT_REMOVED;
    }
    oh.effects_size = neffects * 0x10;
    ip += h.splitter_size;                                             /* splitters: the game uses none */
    for (u32 i = 0; i < nmixes; i++, ip += MIX_IN)                     /* mixes */
        if (i < r->mix_count) update_mix(r, &r->mixes[i], ip);
    for (u32 i = 0; i < nsinks; i++, ip += SINK_IN, op += 0x20) {     /* sinks: a circular buffer's position */
        if (i >= r->sink_count) continue;
        update_sink(&r->sinks[i], ip);
        memcpy(op, &r->sinks[i].pos, 4);
    }
    oh.sinks_size = nsinks * 0x20;
    op += 0x10;                                                        /* performance: no history */
    oh.performance_size = 0x10;
    op += 0xb0;                                                        /* errors: none */
    oh.behaviour_size = 0xb0;
    if (render_info) {
        memcpy(op, &r->frames, 8);
        op += 0x10;
        oh.render_info_size = 0x10;
    }
    ReleaseSRWLockExclusive(&r->lock);
    oh.size = (u32)(op - out);
    memcpy(out, &oh, sizeof oh);
    return R_OK;
}

/* ---------------------------------------------------------------------------------------------- the clock */
/* a frame whenever the host device has fewer than four queued (one per tick without a device); a frame takes well
 * under a millisecond, so the thread runs above the guest's threads to keep the device fed */
static unsigned __stdcall renderer_thread(void *p) {
    ArRenderer *r = p;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -50000;                          /* 5 ms */
    SetWaitableTimer(timer, &due, 5, NULL, NULL, FALSE);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    while (!r->closing) {
        WaitForSingleObject(timer, INFINITE);
        if (!r->started) continue;
        u32 queued = r->sample_rate == 48000 ? audio_queued_frames(r->sample_count) : ~0u;
        u32 frames = queued == ~0u ? 1 : queued >= 4 ? 0 : 4 - queued;
        for (u32 i = 0; i < frames && r->started; i++) {
            AcquireSRWLockExclusive(&r->lock);
            ar_render_frame(r);
            ReleaseSRWLockExclusive(&r->lock);
            event_signal(r->event);
        }
    }
    CloseHandle(timer);
    return 0;
}

/* ---------------------------------------------------------------------------------------------- IAudioRenderer */
static void ar_sample_rate(SvcObj *o, IpcReq *r) { out_u32(r, ((ArRenderer *)o->data)->sample_rate); }
static void ar_sample_count(SvcObj *o, IpcReq *r) { out_u32(r, ((ArRenderer *)o->data)->sample_count); }
static void ar_mix_count(SvcObj *o, IpcReq *r) { out_u32(r, ((ArRenderer *)o->data)->mix_buffers); }
static void ar_state(SvcObj *o, IpcReq *r) { out_u32(r, ((ArRenderer *)o->data)->started ? 0 : 1); }
static void ar_request_update(SvcObj *o, IpcReq *r) {
    const IpcBuf *in = buf_in(r, 0), *out = buf_out(r, 0);
    if (!in || !out) { r->result = 0x29a; return; }
    r->result = request_update(o->data, (const u8 *)(uintptr_t)in->addr, in->size, (u8 *)(uintptr_t)out->addr, out->size);
}
static void ar_start(SvcObj *o, IpcReq *r) { ((ArRenderer *)o->data)->started = true; }
static void ar_stop(SvcObj *o, IpcReq *r) { ((ArRenderer *)o->data)->started = false; }
static void ar_event(SvcObj *o, IpcReq *r) { out_event(r, ((ArRenderer *)o->data)->event); }
static void ar_time_limit(SvcObj *o, IpcReq *r) { out_u32(r, 100); }

static void renderer_destroy(SvcObj *o) {
    ArRenderer *rr = o->data;
    rr->closing = true;                              /* the thread exits on its next tick and leaks rr: rare */
}

static const Iface iface_renderer = {"IAudioRenderer", (const IpcCmd[]){
    {0, ar_sample_rate, "GetSampleRate"},
    {1, ar_sample_count, "GetSampleCount"},
    {2, ar_mix_count, "GetMixBufferCount"},
    {3, ar_state, "GetState"},
    {4, ar_request_update, "RequestUpdate"},
    {5, ar_start, "Start"},
    {6, ar_stop, "Stop"},
    {7, ar_event, "QuerySystemEvent"},
    {8, ipc_noop, "SetRenderingTimeLimit"},
    {9, ar_time_limit, "GetRenderingTimeLimit"},
    {10, ar_request_update, "RequestUpdateAuto"},
    IPC_END}, renderer_destroy};

/* ---------------------------------------------------------------------------------------------- IAudioDevice */
static const char *const device_names[] = {"AudioTvOutput", "AudioStereoJackOutput", "AudioBuiltInSpeakerOutput"};
static KEvent *device_event;
static float device_volume = 1.0f;

static void ad_list(SvcObj *o, IpcReq *r) {
    const IpcBuf *b = buf_out(r, 0);
    u32 n = 0;
    if (b)
        for (; n < 3 && (n + 1) * 0x100 <= b->size; n++) {
            memset((void *)(uintptr_t)(b->addr + n * 0x100), 0, 0x100);
            strcpy((char *)(uintptr_t)(b->addr + n * 0x100), device_names[n]);
        }
    out_u32(r, n);
}
static void ad_set_volume(SvcObj *o, IpcReq *r) { memcpy(&device_volume, r->in, 4); }
static void ad_get_volume(SvcObj *o, IpcReq *r) { out_data(r, &device_volume, 4); }
static void ad_active_name(SvcObj *o, IpcReq *r) {
    const IpcBuf *b = buf_out(r, 0);
    if (b && b->size >= 0x100) { memset((void *)(uintptr_t)b->addr, 0, 0x100); strcpy((char *)(uintptr_t)b->addr, device_names[0]); }
}
static void ad_event(SvcObj *o, IpcReq *r) { out_event(r, device_event); }
static void ad_channels(SvcObj *o, IpcReq *r) { out_u32(r, 2); }

static const Iface iface_device = {"IAudioDevice", (const IpcCmd[]){
    {0, ad_list, "ListAudioDeviceName"},
    {1, ad_set_volume, "SetAudioDeviceOutputVolume"},
    {2, ad_get_volume, "GetAudioDeviceOutputVolume"},
    {3, ad_active_name, "GetActiveAudioDeviceName"},
    {4, ad_event, "QueryAudioDeviceSystemEvent"},
    {5, ad_channels, "GetActiveChannelCount"},
    {6, ad_list, "ListAudioDeviceNameAuto"},
    {7, ad_set_volume, "SetAudioDeviceOutputVolumeAuto"},
    {8, ad_get_volume, "GetAudioDeviceOutputVolumeAuto"},
    {10, ad_active_name, "GetActiveAudioDeviceNameAuto"},
    {11, ad_event, "QueryAudioDeviceInputEvent"},
    {12, ad_event, "QueryAudioDeviceOutputEvent"},
    IPC_END}, NULL};

/* ---------------------------------------------------------------------------------------------- audren:u */
/* AudioRendererParameter: sample rate, sample count, mix buffers, sub mixes, voices, sinks, effects, performance
 * frames, flags, splitter infos, splitter destinations, external context size, revision */
static void am_work_buffer_size(SvcObj *o, IpcReq *r) {
    u32 count = in_u32(r, 4), buffers = in_u32(r, 8), voices = in_u32(r, 16), effects = in_u32(r, 24);
    u64 size = 0x10000 + (u64)voices * 0x800 + (u64)effects * 0x2000 + (u64)buffers * count * 4 * 6;
    out_u64(r, (size + 0xfff) & ~0xfffull);
}

static void am_open(SvcObj *o, IpcReq *r) {
    ArRenderer *rr = calloc(1, sizeof *rr);
    rr->sample_rate = in_u32(r, 0);
    rr->sample_count = in_u32(r, 4);
    rr->mix_buffers = in_u32(r, 8);
    rr->mix_count = in_u32(r, 12) + 1;
    rr->voice_count = in_u32(r, 16);
    rr->sink_count = in_u32(r, 20);
    rr->effect_count = in_u32(r, 24);
    rr->revision = in_u32(r, 0x30);
    rr->voices = calloc(rr->voice_count + 1, sizeof(ArVoice));
    rr->chans = calloc(rr->voice_count + 1, sizeof(ArChannel));
    rr->mixes = calloc(rr->mix_count, sizeof(ArMix));
    rr->order = calloc(rr->mix_count, sizeof(int));
    rr->effects = calloc(rr->effect_count + 1, sizeof(ArEffect));
    rr->sinks = calloc(rr->sink_count + 1, sizeof(ArSink));
    rr->buf = calloc((size_t)rr->mix_buffers * (rr->sample_count ? rr->sample_count : 1) + 1, sizeof(float));
    rr->depop = calloc(rr->mix_buffers + 1, sizeof(float));
    for (u32 i = 0; i < rr->mix_count; i++) rr->mixes[i].dest = AR_UNUSED_MIX;
    rr->layout_dirty = true;
    rr->event = event_create();
    InitializeSRWLock(&rr->lock);
    hlog(1, "audren: renderer %u Hz, %u samples per frame, %u mix buffers, %u mixes, %u voices, %u effects, %u sinks, "
         "revision %.4s", rr->sample_rate, rr->sample_count, rr->mix_buffers, rr->mix_count, rr->voice_count,
         rr->effect_count, rr->sink_count, (char *)&rr->revision);
    _beginthreadex(NULL, 0, renderer_thread, rr, 0, NULL);
    out_object(r, svcobj_new(&iface_renderer, rr, NULL));
}

IPC_OPEN(am_device, &iface_device)

static const Iface iface_audren = {"audren:u", (const IpcCmd[]){
    {0, am_open, "OpenAudioRenderer"},
    {1, am_work_buffer_size, "GetWorkBufferSize"},
    {2, am_device, "GetAudioDeviceService"},
    {4, am_device, "GetAudioDeviceServiceWithRevisionInfo"},
    IPC_END}, NULL};

static SvcObj *open_audren(const char *name) { return svcobj_new(&iface_audren, NULL, name); }

void audio_register(void) {
    trace = getenv("MK8_AUDIO_TRACE") != NULL;
    device_event = event_create();
    service_register("audren:u", open_audren);
}
