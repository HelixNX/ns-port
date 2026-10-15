/* audren:u: the audio renderer. The renderer runs on its own clock: every 5 ms a host thread renders one frame
 * (sample_count samples), which plays each started voice's wave buffers at its rate and pitch and consumes them, and
 * signals the renderer's event. RequestUpdate exchanges state with the game in the update-data format (header, then
 * memory pools, voices, effects, sinks, performance, errors, render info). Mixing and output come in M6; until then
 * playback only keeps time.
 * Also IAudioDevice (device names, volume). */
#include "../ipc.h"
#include <stdlib.h>
#include <windows.h>
#include <process.h>

/* ---------------------------------------------------------------------------------------------- update data */
typedef struct UpdateHeader {
    u32 revision, behaviour_size, memory_pool_size, voices_size, voice_resources_size, effects_size, mix_size,
        sinks_size, performance_size, splitter_size, render_info_size, unk[4], size;
} UpdateHeader;

typedef struct WaveBuffer {                         /* VoiceInfo::WaveBufferInternal, 0x38 */
    u64 address, size;
    s32 start_offset, end_offset;
    u8 loop, stream_ended, sent_to_dsp, pad;
    s32 loop_count;
    u64 context_address, context_size;
    u32 loop_start, loop_end;
} WaveBuffer;

enum { PLAY_STARTED, PLAY_STOPPED, PLAY_PAUSED };
enum { POOL_REQUEST_DETACH = 2, POOL_DETACHED = 3, POOL_REQUEST_ATTACH = 4, POOL_ATTACHED = 5 };
enum { EFFECT_USED = 3, EFFECT_REMOVED = 4 };

#define MAX_WAVEBUFFERS 4

typedef struct Voice {
    bool in_use;
    u8 play_state;
    u32 sample_rate;
    float pitch;
    struct { bool valid, loop; s64 samples, left; } wb[MAX_WAVEBUFFERS];
    u32 index;                                      /* the wave buffer playing now */
    u64 played, consumed;
} Voice;

typedef struct Renderer {
    u32 sample_rate, sample_count, mixes, voices_count, revision;
    Voice *voices;
    bool started;
    u64 frames;
    KEvent *event;
    SRWLOCK lock;
    volatile bool closing;
} Renderer;

/* one rendering frame of every started voice */
static void render_frame(Renderer *r) {
    for (u32 v = 0; v < r->voices_count; v++) {
        Voice *vo = &r->voices[v];
        if (!vo->in_use || vo->play_state != PLAY_STARTED) continue;
        double rate = (double)(vo->sample_rate ? vo->sample_rate : r->sample_rate) * (vo->pitch > 0 ? vo->pitch : 1.0f);
        s64 need = (s64)(rate * r->sample_count / r->sample_rate + 0.5);
        for (int guard = 0; need > 0 && guard < 2 * MAX_WAVEBUFFERS; guard++) {
            typeof(vo->wb[0]) *w = &vo->wb[vo->index];
            if (!w->valid) break;
            s64 n = need < w->left ? need : w->left;
            w->left -= n;
            need -= n;
            vo->played += (u64)n;
            if (w->left <= 0) {
                if (w->loop && w->samples > 0) { w->left = w->samples; continue; }
                w->valid = false;
                vo->consumed++;
                vo->index = (vo->index + 1) % MAX_WAVEBUFFERS;
            }
        }
    }
    r->frames++;
}

static unsigned __stdcall renderer_thread(void *p) {
    Renderer *r = p;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    LARGE_INTEGER due;
    due.QuadPart = -50000;                          /* 5 ms */
    SetWaitableTimer(timer, &due, 5, NULL, NULL, FALSE);
    while (!r->closing) {
        WaitForSingleObject(timer, INFINITE);
        AcquireSRWLockExclusive(&r->lock);
        if (r->started) render_frame(r);
        ReleaseSRWLockExclusive(&r->lock);
        if (r->started) event_signal(r->event);
    }
    CloseHandle(timer);
    return 0;
}

/* takes in a voice's parameters: state, new wave buffers (not yet sent to the DSP), flushes */
static void update_voice(Renderer *r, const u8 *in) {
    u32 id;
    memcpy(&id, in, 4);
    if (id >= r->voices_count) return;
    Voice *vo = &r->voices[id];
    bool is_new = in[8], in_use = in[9];
    if (is_new) memset(vo, 0, sizeof *vo);
    vo->in_use = in_use;
    vo->play_state = in[0xa];
    memcpy(&vo->sample_rate, in + 0xc, 4);
    memcpy(&vo->pitch, in + 0x1c, 4);
    u8 flush = in[0x159];
    const WaveBuffer *wbs = (const WaveBuffer *)(in + 0x60);
    for (int i = 0; i < MAX_WAVEBUFFERS; i++) {
        if (wbs[i].sent_to_dsp) continue;
        s64 n = (s64)wbs[i].end_offset - wbs[i].start_offset;
        vo->wb[i].valid = true;
        vo->wb[i].loop = wbs[i].loop && wbs[i].loop_count != 0;
        vo->wb[i].samples = vo->wb[i].left = n > 0 ? n : 0;
    }
    for (; flush; flush--) {                         /* dropped by the game: count them as consumed */
        if (vo->wb[vo->index].valid) {
            vo->wb[vo->index].valid = false;
            vo->consumed++;
        }
        vo->index = (vo->index + 1) % MAX_WAVEBUFFERS;
    }
    if (vo->play_state == PLAY_STOPPED)
        for (int i = 0; i < MAX_WAVEBUFFERS; i++)
            if (vo->wb[vo->index].valid) {
                vo->wb[vo->index].valid = false;
                vo->consumed++;
                vo->index = (vo->index + 1) % MAX_WAVEBUFFERS;
            }
}

static u32 request_update(Renderer *r, const u8 *in, u64 in_size, u8 *out, u64 out_size) {
    if (in_size < sizeof(UpdateHeader)) return 0x29a;  /* audio: invalid update data */
    UpdateHeader h;
    memcpy(&h, in, sizeof h);
    u32 voice_stride = 0x170, effect_in = 0xc0, effect_out = 0x10;
    u32 npools = h.memory_pool_size / 0x20, nvoices = h.voices_size / voice_stride;
    u32 neffects = h.effects_size / effect_in, nsinks = h.sinks_size / 0x140;
    u32 rev = (h.revision >> 24) - '0';                 /* 'REVn' */
    bool render_info = rev >= 5;                        /* elapsed frame count: REV5 and later */
    u64 need = sizeof(UpdateHeader) + npools * 0x10 + nvoices * 0x10 + neffects * effect_out + nsinks * 0x20 + 0x10 +
               0xb0 + (render_info ? 0x10 : 0);
    if (need != out_size) {
        static bool said;
        if (!said) hlog(0, "audren: update output of %llu bytes, expected %llu", out_size, need);
        said = true;
        if (need > out_size) return 0x29a;
    }
    memset(out, 0, out_size);
    const u8 *ip = in + sizeof(UpdateHeader) + h.behaviour_size;
    u8 *op = out + sizeof(UpdateHeader);
    UpdateHeader oh = {0};
    oh.revision = h.revision;
    AcquireSRWLockExclusive(&r->lock);
    for (u32 i = 0; i < npools; i++, ip += 0x20, op += 0x10) {        /* memory pools */
        u32 st;
        memcpy(&st, ip + 0x10, 4);
        if (st == POOL_REQUEST_ATTACH) st = POOL_ATTACHED;
        else if (st == POOL_REQUEST_DETACH) st = POOL_DETACHED;
        memcpy(op, &st, 4);
    }
    oh.memory_pool_size = npools * 0x10;
    ip += h.voice_resources_size;
    for (u32 i = 0; i < nvoices; i++, ip += voice_stride, op += 0x10) {   /* voices */
        update_voice(r, ip);
        u32 id;
        memcpy(&id, ip, 4);
        if (id < r->voices_count) {
            Voice *vo = &r->voices[id];
            u32 consumed = (u32)vo->consumed;
            memcpy(op, &vo->played, 8);
            memcpy(op + 8, &consumed, 4);
        }
    }
    oh.voices_size = nvoices * 0x10;
    for (u32 i = 0; i < neffects; i++, ip += effect_in, op += effect_out)   /* effects: in use or removed */
        op[0] = ip[2] ? EFFECT_USED : EFFECT_REMOVED;
    oh.effects_size = neffects * effect_out;
    ip += h.splitter_size + h.mix_size;
    ip += h.sinks_size;                                                    /* sinks: nothing to report */
    op += nsinks * 0x20;
    oh.sinks_size = nsinks * 0x20;
    op += 0x10;                                                            /* performance: no history */
    oh.performance_size = 0x10;
    op += 0xb0;                                                            /* errors: none */
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

/* ---------------------------------------------------------------------------------------------- IAudioRenderer */
static void ar_sample_rate(SvcObj *o, IpcReq *r) { out_u32(r, ((Renderer *)o->data)->sample_rate); }
static void ar_sample_count(SvcObj *o, IpcReq *r) { out_u32(r, ((Renderer *)o->data)->sample_count); }
static void ar_mix_count(SvcObj *o, IpcReq *r) { out_u32(r, ((Renderer *)o->data)->mixes); }
static void ar_state(SvcObj *o, IpcReq *r) { out_u32(r, ((Renderer *)o->data)->started ? 0 : 1); }
static void ar_request_update(SvcObj *o, IpcReq *r) {
    const IpcBuf *in = buf_in(r, 0), *out = buf_out(r, 0);
    if (!in || !out) { r->result = 0x29a; return; }
    r->result = request_update(o->data, (const u8 *)(uintptr_t)in->addr, in->size, (u8 *)(uintptr_t)out->addr, out->size);
}
static void ar_start(SvcObj *o, IpcReq *r) { ((Renderer *)o->data)->started = true; }
static void ar_stop(SvcObj *o, IpcReq *r) { ((Renderer *)o->data)->started = false; }
static void ar_event(SvcObj *o, IpcReq *r) { out_event(r, ((Renderer *)o->data)->event); }
static void ar_time_limit(SvcObj *o, IpcReq *r) { out_u32(r, 100); }

static void renderer_destroy(SvcObj *o) {
    Renderer *rr = o->data;
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
/* AudioRendererParameter: sample rate, sample count, mixes, sub mixes, voices, sinks, effects, performance frames,
 * flags, splitter infos, splitter destinations, external context size, revision */
static void am_work_buffer_size(SvcObj *o, IpcReq *r) {
    u32 count = in_u32(r, 4), mixes = in_u32(r, 8) + in_u32(r, 12), voices = in_u32(r, 16), effects = in_u32(r, 24);
    u64 size = 0x10000 + (u64)voices * 0x800 + (u64)effects * 0x2000 + (u64)mixes * count * 4 * 6;
    out_u64(r, (size + 0xfff) & ~0xfffull);
}

static void am_open(SvcObj *o, IpcReq *r) {
    Renderer *rr = calloc(1, sizeof *rr);
    rr->sample_rate = in_u32(r, 0);
    rr->sample_count = in_u32(r, 4);
    rr->mixes = in_u32(r, 8);
    rr->voices_count = in_u32(r, 16);
    rr->revision = in_u32(r, 0x30);
    rr->voices = calloc(rr->voices_count ? rr->voices_count : 1, sizeof(Voice));
    rr->event = event_create();
    InitializeSRWLock(&rr->lock);
    hlog(1, "audren: renderer %u Hz, %u samples per frame, %u voices, revision %.4s", rr->sample_rate,
         rr->sample_count, rr->voices_count, (char *)&rr->revision);
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
    device_event = event_create();
    service_register("audren:u", open_audren);
}
