/* audout: the simple audio output (a queue of PCM buffers; newer games' sound engines, e.g. Wwise, use it instead
 * of the renderer). Each IAudioOut is an SDL stream on the default device (SDL mixes it with the renderer's); a
 * buffer is copied to the stream when appended and released, with the buffer event, once the device has played
 * past its end. Without a device, buffers are released at the rate they would play.
 * Command ids are from the SDK proxies (IAudioOutManager, IAudioOut). */
#include "../ipc.h"
#include <SDL3/SDL.h>
#include <stdlib.h>
#include <windows.h>
#include <process.h>

#define MAX_PENDING 64

typedef struct AudioOut {
    SRWLOCK lock;
    SDL_AudioStream *stream;
    KEvent *event;
    u32 rate, channels, format, state;  /* state: 0 started, 1 stopped */
    float volume;
    u64 pushed, played;                 /* bytes */
    u64 start_tick;                     /* without a device: when playing started (QPC) */
    struct { u64 tag, end; } pending[MAX_PENDING];
    int npending;
    u64 released[MAX_PENDING];
    int nreleased;
    struct AudioOut *next;
} AudioOut;

static AudioOut *outs;
static SRWLOCK outs_lock = SRWLOCK_INIT;

static u64 qpc(void) { LARGE_INTEGER n; QueryPerformanceCounter(&n); return (u64)n.QuadPart; }
static u64 qpf(void) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return (u64)f.QuadPart; }

static u32 frame_bytes(AudioOut *a) { return a->channels * (a->format == 5 ? 4 : 2); }

/* with the lock held: how far the device has played; release what it has finished */
static void update(AudioOut *a) {
    if (a->state != 0) return;
    if (a->stream) {
        int q = SDL_GetAudioStreamQueued(a->stream);
        if (q >= 0) a->played = a->pushed - (u64)q;
    } else {
        u64 t = (qpc() - a->start_tick) * a->rate / qpf() * frame_bytes(a);
        a->played = t < a->pushed ? t : a->pushed;
    }
    int done = 0;
    while (done < a->npending && a->pending[done].end <= a->played) done++;
    if (!done) return;
    for (int i = 0; i < done && a->nreleased < MAX_PENDING; i++) a->released[a->nreleased++] = a->pending[i].tag;
    memmove(a->pending, a->pending + done, (size_t)(a->npending - done) * sizeof a->pending[0]);
    a->npending -= done;
    event_signal(a->event);
}

static unsigned __stdcall release_thread(void *p) {
    (void)p;
    for (;;) {
        Sleep(2);
        AcquireSRWLockShared(&outs_lock);
        for (AudioOut *a = outs; a; a = a->next) {
            AcquireSRWLockExclusive(&a->lock);
            update(a);
            ReleaseSRWLockExclusive(&a->lock);
        }
        ReleaseSRWLockShared(&outs_lock);
    }
}

/* ---------------------------------------------------------------------------------------------- IAudioOut */
#define AO(o) ((AudioOut *)(o)->data)

static void ao_state(SvcObj *o, IpcReq *r) { out_u32(r, AO(o)->state); }
static void ao_start(SvcObj *o, IpcReq *r) {
    AudioOut *a = AO(o);
    AcquireSRWLockExclusive(&a->lock);
    if (a->state != 0) {
        a->state = 0;
        a->start_tick = qpc() - a->played / frame_bytes(a) * qpf() / a->rate;
        if (a->stream) SDL_ResumeAudioStreamDevice(a->stream);
    }
    ReleaseSRWLockExclusive(&a->lock);
}
static void ao_stop(SvcObj *o, IpcReq *r) {
    AudioOut *a = AO(o);
    AcquireSRWLockExclusive(&a->lock);
    if (a->state == 0) {
        update(a);
        a->state = 1;
        if (a->stream) SDL_PauseAudioStreamDevice(a->stream);
    }
    ReleaseSRWLockExclusive(&a->lock);
}

/* AudioOutBuffer: u64 next, u64 sample buffer, u64 capacity, u64 data size, u64 data offset */
static void append(SvcObj *o, IpcReq *r, const IpcBuf *b) {
    AudioOut *a = AO(o);
    u64 tag = in_u64(r, 0);
    if (!b || b->size < 0x28) { r->result = 0x299; return; }
    u64 data = LD64(b->addr + 8), size = LD64(b->addr + 0x18), off = LD64(b->addr + 0x20);
    AcquireSRWLockExclusive(&a->lock);
    if (a->npending == MAX_PENDING) {
        ReleaseSRWLockExclusive(&a->lock);
        r->result = (8u << 9) | 153u;                    /* audio: buffer count reached */
        return;
    }
    if (a->stream && size) {
        const void *src = (const void *)(uintptr_t)(data + off);
        if (a->volume != 1.0f) SDL_SetAudioStreamGain(a->stream, a->volume);
        SDL_PutAudioStreamData(a->stream, src, (int)size);
    }
    a->pushed += size;
    a->pending[a->npending].tag = tag;
    a->pending[a->npending].end = a->pushed;
    a->npending++;
    ReleaseSRWLockExclusive(&a->lock);
}
static void ao_append(SvcObj *o, IpcReq *r) { append(o, r, r->na ? &r->a[0] : NULL); }
static void ao_append_auto(SvcObj *o, IpcReq *r) { append(o, r, buf_in(r, 0)); }
static void ao_event(SvcObj *o, IpcReq *r) { out_event(r, AO(o)->event); }
static void released(SvcObj *o, IpcReq *r, const IpcBuf *b) {
    AudioOut *a = AO(o);
    AcquireSRWLockExclusive(&a->lock);
    update(a);
    u32 n = 0;
    while (b && n < (u32)a->nreleased && (n + 1) * 8 <= b->size) {
        ST64(b->addr + 8 * n, a->released[n]);
        n++;
    }
    memmove(a->released, a->released + n, (size_t)(a->nreleased - (int)n) * 8);
    a->nreleased -= (int)n;
    if (!a->nreleased) event_clear(a->event);
    ReleaseSRWLockExclusive(&a->lock);
    out_u32(r, n);
}
static void ao_released(SvcObj *o, IpcReq *r) { released(o, r, r->nb ? &r->b[0] : NULL); }
static void ao_released_auto(SvcObj *o, IpcReq *r) { released(o, r, buf_out(r, 0)); }
static void ao_contains(SvcObj *o, IpcReq *r) {
    AudioOut *a = AO(o);
    u64 tag = in_u64(r, 0);
    bool found = false;
    AcquireSRWLockShared(&a->lock);
    for (int i = 0; i < a->npending; i++) found |= a->pending[i].tag == tag;
    ReleaseSRWLockShared(&a->lock);
    out_u8(r, found);
}
static void ao_count(SvcObj *o, IpcReq *r) { out_u32(r, (u32)AO(o)->npending); }
static void ao_played(SvcObj *o, IpcReq *r) { AudioOut *a = AO(o); out_u64(r, a->played / frame_bytes(a)); }
static void ao_flush(SvcObj *o, IpcReq *r) { out_u8(r, 1); }
static void ao_set_volume(SvcObj *o, IpcReq *r) {
    u32 v = in_u32(r, 0);
    memcpy(&AO(o)->volume, &v, 4);
}
static void ao_get_volume(SvcObj *o, IpcReq *r) { out_data(r, &AO(o)->volume, 4); }

static void ao_destroy(SvcObj *o) {
    AudioOut *a = AO(o);
    AcquireSRWLockExclusive(&outs_lock);
    for (AudioOut **p = &outs; *p; p = &(*p)->next)
        if (*p == a) { *p = a->next; break; }
    ReleaseSRWLockExclusive(&outs_lock);
    if (a->stream) SDL_DestroyAudioStream(a->stream);
    kobj_unref(&a->event->obj);
    free(a);
}

static const Iface iface_audio_out = {"IAudioOut", (const IpcCmd[]){
    {0, ao_state, "GetAudioOutState"},
    {1, ao_start, "Start"},
    {2, ao_stop, "Stop"},
    {3, ao_append, "AppendAudioOutBuffer"},
    {4, ao_event, "RegisterBufferEvent"},
    {5, ao_released, "GetReleasedAudioOutBuffers"},
    {6, ao_contains, "ContainsAudioOutBuffer"},
    {7, ao_append_auto, "AppendAudioOutBufferAuto"},
    {8, ao_released_auto, "GetReleasedAudioOutBuffersAuto"},
    {9, ao_count, "GetAudioOutBufferCount"},
    {10, ao_played, "GetAudioOutPlayedSampleCount"},
    {11, ao_flush, "FlushAudioOutBuffers"},
    {12, ao_set_volume, "SetAudioOutVolume"},
    {13, ao_get_volume, "GetAudioOutVolume"},
    IPC_END}, ao_destroy};

/* ---------------------------------------------------------------------------------------------- IAudioOutManager */
static void list_outs(IpcReq *r, const IpcBuf *b) {
    u32 n = 0;
    if (b && b->size >= 0x100) {
        char name[0x100] = "DeviceOut";
        memcpy((void *)(uintptr_t)b->addr, name, sizeof name);
        n = 1;
    }
    out_u32(r, n);
}
static void am_list(SvcObj *o, IpcReq *r) { list_outs(r, r->nb ? &r->b[0] : NULL); }
static void am_list_auto(SvcObj *o, IpcReq *r) { list_outs(r, buf_out(r, 0)); }

/* in: u32 sample rate, u16 channels, u16 reserved, u64 applet resource user id; out: rate, channels, format, state */
static void open_out(IpcReq *r, const IpcBuf *name_out) {
    AudioOut *a = calloc(1, sizeof *a);
    InitializeSRWLock(&a->lock);
    a->rate = in_u32(r, 0) ? in_u32(r, 0) : 48000;
    a->channels = (in_u32(r, 4) & 0xffff) ? (in_u32(r, 4) & 0xffff) : 2;
    if (a->channels != 2 && a->channels != 6) a->channels = 2;
    a->format = 2;                                       /* PCM int16 */
    a->state = 1;
    a->volume = 1.0f;
    a->event = event_create();
    if (!getenv("NX_NOAUDIO") && SDL_WasInit(SDL_INIT_AUDIO)) {
        SDL_AudioSpec spec = {SDL_AUDIO_S16, (int)a->channels, (int)a->rate};
        a->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (!a->stream) hlog(0, "audout: no output stream: %s", SDL_GetError());
        else SDL_PauseAudioStreamDevice(a->stream);
    }
    if (name_out && name_out->size >= 10) memcpy((void *)(uintptr_t)name_out->addr, "DeviceOut", 10);
    AcquireSRWLockExclusive(&outs_lock);
    a->next = outs;
    outs = a;
    ReleaseSRWLockExclusive(&outs_lock);
    hlog(1, "audout: opened %u Hz, %u channels", a->rate, a->channels);
    out_u32(r, a->rate);
    out_u32(r, a->channels);
    out_u32(r, a->format);
    out_u32(r, a->state);
    out_object(r, svcobj_new(&iface_audio_out, a, "IAudioOut"));
}
static void am_open(SvcObj *o, IpcReq *r) { open_out(r, r->nb ? &r->b[0] : NULL); }
static void am_open_auto(SvcObj *o, IpcReq *r) { open_out(r, buf_out(r, 0)); }

static const Iface iface_audout = {"audout:u", (const IpcCmd[]){
    {0, am_list, "ListAudioOuts"},
    {1, am_open, "OpenAudioOut"},
    {2, am_list_auto, "ListAudioOutsAuto"},
    {3, am_open_auto, "OpenAudioOutAuto"},
    IPC_END}, NULL};

static SvcObj *open_audout(const char *name) { return svcobj_new(&iface_audout, NULL, name); }

void audout_register(void) {
    service_register("audout:u", open_audout);
    _beginthreadex(NULL, 0, release_thread, NULL, 0, NULL);
}
