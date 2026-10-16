/* Movies (CLAUDE.md, decision 3): subsdk0's movie::Extractor and movie::Decoder start NVIDIA's multimedia stack,
 * which needs nvdrv. The constructors main calls (tools/lifter/overrides.txt) build objects of our own instead,
 * with vtables in subsdk0's layout (offset to top, RTTI, D2, D0, methods). Our streams keep the file's timing:
 * the extractor walks the video track's samples (duration, frame rate and size from the MP4's headers) and seeks,
 * and the decoder turns each sample into a black NV12 frame with that sample's time. Media Foundation comes later.
 *
 * How main's player uses them (main+0x379f28..0x37ca88): a thread waits on the decoder's client events
 * (RegisterClientEvents: input available, output available, format changed, error) and collects input indices
 * (each one posts "feed a sample" to the player thread) and output indices (queued for the renderer, 32 at most,
 * unchecked: so outputs must wait for the renderer's releases). The player thread reads one sample per input
 * index, seeks and loops (SeekTo, then GetSampleTime is the time the renderer waits for). The renderer shows one
 * output per movie frame and its time is the player's current time, which the menus poll.
 * Status values (movie::Status, from subsdk0's AndroidErrorToNnError): 0 ok, 0x2000 end of stream,
 * 0x5003 try again later, 0x4004 output format changed. */
#include "hos.h"
#include <stdlib.h>
#include <windows.h>

#define ST_OK 0u
#define ST_EOS 0x2000u
#define ST_AGAIN 0x5003u
#define FLAG_EOS 4u

const char *fs_romfs_dir(void);

static const char *const extractor_names[18] = {0, 0, "~Extractor", "~Extractor (delete)", "SetDataSource(path)",
    "SetDataSource(data, size)", "GetTrackCount", "GetTrackConfiguration", "SelectTrack", "UnselectTrack", "SeekTo",
    "GetTrackIndexForAvailableData", "Read", "Advance", "GetSampleTime", "GetSampleMetaData", "GetBufferedRange",
    "SetDataSource(reader, path)"};
static const char *const decoder_names[21] = {0, 0, "~Decoder", "~Decoder (delete)", "Configure", "RegisterClientEvents",
    "GetOutputFormat", "Start", "Stop", "Reset", "Flush", "AcquireInputBufferIndex", "GetInputBuffer",
    "SendInputBufferForDecode", "AcquireOutputBufferIndex", "GetOutputBuffer", "GetOutputBufferSize",
    "ReleaseOutputBufferIndex", "GetLastError", "GetOutputBuffer(format)", "GetOutputBufferFormat"};

static u64 extractor_vt[18], decoder_vt[21];
static bool ready;

/* the objects main allocates are 16 bytes: our vtable, then our state */
typedef struct {
    s64 duration, frame;                /* microseconds */
    s64 cur;                            /* the next sample's time */
    u32 width, height;
} Stream;

enum { NIN = 4, NOUT = 4 };
typedef struct {
    CRITICAL_SECTION lock;
    u64 events;                         /* guest array of four nn::os::EventType pointers */
    u32 width, height;
    bool started;
    struct { bool used, queued; s64 time; u32 flags; } in[NIN];
    u32 in_order[NIN], nin_order;       /* inputs sent for decoding, oldest first */
    struct { bool used, ready; s64 time; u32 flags; } out[NOUT];
    u32 out_order[NOUT], nout_order;    /* decoded outputs not yet acquired, oldest first */
} Dec;

#define STATE(c) ((void *)(uintptr_t)LD64((c)->x[0] + 8))

/* ---------------------------------------------------------------------------------------------- MP4 headers */
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static u64 be64(const u8 *p) { return (u64)be32(p) << 32 | be32(p + 4); }

/* the video track: mdhd (timescale, duration), stts (the first sample delta), tkhd (size) */
static void mp4_trak(Stream *s, const u8 *p, u64 n) {
    u32 timescale = 0, delta = 0, w = 0, h = 0;
    u64 dur = 0;
    bool video = false;
    /* walk every box under trak; the ones we need have fixed layouts */
    const u8 *stack[8], *end[8];
    int depth = 0;
    stack[0] = p; end[0] = p + n;
    while (depth >= 0) {
        if (stack[depth] + 8 > end[depth]) { depth--; continue; }
        const u8 *b = stack[depth];
        u64 size = be32(b);
        if (size < 8 || b + size > end[depth]) { depth--; continue; }
        stack[depth] = b + size;
        const u8 *body = b + 8;
        if (!memcmp(b + 4, "mdia", 4) || !memcmp(b + 4, "minf", 4) || !memcmp(b + 4, "stbl", 4)) {
            if (depth < 7) { depth++; stack[depth] = body; end[depth] = b + size; }
        } else if (!memcmp(b + 4, "tkhd", 4)) {
            const u8 *q = body + (body[0] == 1 ? 88 : 76);
            if (q + 8 <= b + size) { w = be32(q) >> 16; h = be32(q + 4) >> 16; }
        } else if (!memcmp(b + 4, "mdhd", 4)) {
            if (body[0] == 1) { timescale = be32(body + 20); dur = be64(body + 24); }
            else { timescale = be32(body + 12); dur = be32(body + 16); }
        } else if (!memcmp(b + 4, "hdlr", 4)) {
            video = !memcmp(body + 8, "vide", 4);
        } else if (!memcmp(b + 4, "stts", 4)) {
            if (be32(body + 4) >= 1) delta = be32(body + 12);
        }
    }
    if (!video || !timescale) return;
    s->duration = (s64)(dur * 1000000 / timescale);
    if (delta) s->frame = (s64)((u64)delta * 1000000 / timescale);
    if (w && h) { s->width = w; s->height = h; }
}

static void mp4_probe(Stream *s, const char *path) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) { hlog(0, "movie: %s: not found", path); return; }
    LARGE_INTEGER sz;
    GetFileSizeEx(f, &sz);
    u64 off = 0;
    while (off + 16 <= (u64)sz.QuadPart) {                 /* top-level boxes: find moov (often at the end) */
        u8 hd[16];
        DWORD got;
        LARGE_INTEGER at = {.QuadPart = (LONGLONG)off};
        SetFilePointerEx(f, at, NULL, FILE_BEGIN);
        if (!ReadFile(f, hd, 16, &got, NULL) || got < 16) break;
        u64 size = be32(hd), hdr = 8;
        if (size == 1) { size = be64(hd + 8); hdr = 16; }
        if (size == 0) size = (u64)sz.QuadPart - off;
        if (size < hdr) break;
        if (!memcmp(hd + 4, "moov", 4) && size < (64u << 20)) {
            u8 *m = malloc(size);
            SetFilePointerEx(f, at, NULL, FILE_BEGIN);
            if (m && ReadFile(f, m, (DWORD)size, &got, NULL) && got == size) {
                for (u64 p = hdr; p + 8 <= size;) {            /* moov's children: each trak */
                    u64 bs = be32(m + p);
                    if (bs < 8 || p + bs > size) break;
                    if (!memcmp(m + p + 4, "trak", 4)) mp4_trak(s, m + p + 8, bs - 8);
                    p += bs;
                }
            }
            free(m);
            break;
        }
        off += size;
    }
    CloseHandle(f);
}

/* ---------------------------------------------------------------------------------------------- guest helpers */
static void describe_track(Ctx *c, u64 md, u32 w, u32 h) {
    static u64 set_string, set_int32, set_rect;
    if (!set_string) {
        set_string = sym_lookup("_ZN5movie9MediaData9SetStringEPKcS2_");
        set_int32 = sym_lookup("_ZN5movie9MediaData8SetInt32EPKci");
        set_rect = sym_lookup("_ZN5movie9MediaData7SetRectEPKciiii");
    }
    if (!md || !set_string || !set_int32) return;
    static const char mime[] = "mime", avc[] = "video/avc", width[] = "width", height[] = "height", stride[] = "stride",
                      crop[] = "crop";
    Ctx saved = *c;
    c->x[0] = md; c->x[1] = (u64)(uintptr_t)mime; c->x[2] = (u64)(uintptr_t)avc; guest_call(c, set_string);
    c->x[0] = md; c->x[1] = (u64)(uintptr_t)width; c->x[2] = w; guest_call(c, set_int32);
    c->x[0] = md; c->x[1] = (u64)(uintptr_t)height; c->x[2] = h; guest_call(c, set_int32);
    c->x[0] = md; c->x[1] = (u64)(uintptr_t)stride; c->x[2] = w; guest_call(c, set_int32);
    if (set_rect) {
        c->x[0] = md; c->x[1] = (u64)(uintptr_t)crop; c->x[2] = 0; c->x[3] = 0; c->x[4] = w - 1; c->x[5] = h - 1;
        guest_call(c, set_rect);
    }
    *c = saved;
}

/* events: bit 0 input available, bit 1 output available */
static void signal_events(Ctx *c, Dec *d, u32 which) {
    static u64 signal_event;
    if (!signal_event) signal_event = sym_lookup("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    if (!d->events || !signal_event || !which) return;
    Ctx saved = *c;
    for (int i = 0; i < 2; i++) {
        u64 ev = LD64(d->events + 8 * i);
        if (!(which >> i & 1) || !ev) continue;
        c->x[0] = ev;
        guest_call(c, signal_event);
    }
    *c = saved;
}

/* movie::Buffer: +0 s32 offset, +4 s32 size, +0x18 capacity, +0x28 data */
static void buffer_set(u64 b, u64 data, u64 capacity, u32 size) {
    ST64(b + 0x28, data);
    ST64(b + 0x18, capacity);
    ST32(b, 0);
    ST32(b + 4, size);
}

/* ---------------------------------------------------------------------------------------------- decoder */
/* decoded inputs move to free output slots, oldest first; returns the events to signal */
static u32 decode(Dec *d) {
    u32 ev = 0;
    while (d->nin_order) {
        int o = -1;
        for (int i = 0; i < NOUT && o < 0; i++) if (!d->out[i].used) o = i;
        if (o < 0) break;
        u32 k = d->in_order[0];
        memmove(d->in_order, d->in_order + 1, --d->nin_order * sizeof *d->in_order);
        d->out[o].used = d->out[o].ready = true;
        d->out[o].time = d->in[k].time;
        d->out[o].flags = d->in[k].flags & FLAG_EOS;
        d->out_order[d->nout_order++] = (u32)o;
        d->in[k].used = d->in[k].queued = false;
        ev |= 3;
    }
    return ev;
}

static void dec_reset(Dec *d) {
    memset(d->in, 0, sizeof d->in);
    memset(d->out, 0, sizeof d->out);
    d->nin_order = d->nout_order = 0;
}

static u8 *frame_data;                  /* a black NV12 frame of the largest size asked for */
static u64 frame_bytes;
static void black_frame(u32 w, u32 h) {
    u64 n = (u64)w * h * 3 / 2;
    if (n <= frame_bytes) return;
    u8 *p = malloc(n);
    memset(p, 16, (u64)w * h);
    memset(p + (u64)w * h, 128, n - (u64)w * h);
    frame_data = p;                     /* the old one may still be read by another thread: kept */
    frame_bytes = n;
}

static void decoder_call(Ctx *c, int slot, Dec *d) {
    u32 ev = 0;
    c->x[0] = ST_OK;
    switch (slot) {
    case 2: case 3: return;                                       /* destructors: the state stays (rare) */
    case 4: {                                                     /* Configure(MediaData): the track's size */
        static u64 find_int32;
        if (!find_int32) find_int32 = sym_lookup("_ZNK5movie9MediaData9FindInt32EPKcPi");
        if (!find_int32 || !c->x[1]) return;
        static const char width[] = "width", height[] = "height";
        s32 w = 0, h = 0;
        Ctx saved = *c;
        c->x[0] = saved.x[1]; c->x[1] = (u64)(uintptr_t)width; c->x[2] = (u64)(uintptr_t)&w; guest_call(c, find_int32);
        c->x[0] = saved.x[1]; c->x[1] = (u64)(uintptr_t)height; c->x[2] = (u64)(uintptr_t)&h; guest_call(c, find_int32);
        *c = saved;
        if (w > 0 && h > 0 && w <= 4096 && h <= 4096) { d->width = (u32)w; d->height = (u32)h; }
        c->x[0] = ST_OK;
        return;
    }
    case 5: d->events = c->x[1]; return;                          /* RegisterClientEvents */
    case 6: case 20: describe_track(c, c->x[1], d->width, d->height); return;   /* GetOutputFormat */
    case 7:                                                       /* Start */
        EnterCriticalSection(&d->lock);
        d->started = true;
        LeaveCriticalSection(&d->lock);
        ev = 1;
        break;
    case 8: case 9: case 10:                                      /* Stop, Reset, Flush */
        EnterCriticalSection(&d->lock);
        dec_reset(d);
        if (slot != 10) d->started = false;
        LeaveCriticalSection(&d->lock);
        ev = slot == 10 ? 1 : 0;
        break;
    case 11: {                                                    /* AcquireInputBufferIndex(&index, &more) */
        EnterCriticalSection(&d->lock);
        int k = -1, more = 0;
        for (int i = 0; i < NIN; i++) {
            if (d->in[i].used) continue;
            if (k < 0) k = i; else more++;
        }
        if (k >= 0 && d->started) {
            d->in[k].used = true;
            if (c->x[1]) ST32(c->x[1], (u32)k);
            if (c->x[2]) ST32(c->x[2], (u32)more);
        } else {
            c->x[0] = ST_AGAIN;
        }
        LeaveCriticalSection(&d->lock);
        return;
    }
    case 12: {                                                    /* GetInputBuffer(index, buffer) */
        static u8 *in_mem;
        if (!in_mem) in_mem = calloc(NIN, 0x1000);
        u32 k = (u32)c->x[1] % NIN;
        if (c->x[2]) buffer_set(c->x[2], (u64)(uintptr_t)(in_mem + k * 0x1000), 0x1000, 0);
        return;
    }
    case 13: {                                                    /* SendInputBufferForDecode(index, off, size, time, flags) */
        u32 k = (u32)c->x[1];
        EnterCriticalSection(&d->lock);
        if (k < NIN && d->in[k].used && !d->in[k].queued) {
            d->in[k].queued = true;
            d->in[k].time = (s64)c->x[4];
            d->in[k].flags = (u32)c->x[5];
            d->in_order[d->nin_order++] = k;
            ev = decode(d);
        }
        LeaveCriticalSection(&d->lock);
        break;
    }
    case 14: {                                                    /* AcquireOutputBufferIndex(&index, &time, &flags, &more) */
        EnterCriticalSection(&d->lock);
        if (d->nout_order) {
            u32 o = d->out_order[0];
            memmove(d->out_order, d->out_order + 1, --d->nout_order * sizeof *d->out_order);
            d->out[o].ready = false;
            if (c->x[1]) ST32(c->x[1], o);
            if (c->x[2]) ST64(c->x[2], (u64)d->out[o].time);
            if (c->x[3]) ST32(c->x[3], d->out[o].flags);
            if (c->x[4]) ST32(c->x[4], d->nout_order);
        } else {
            c->x[0] = ST_AGAIN;
        }
        LeaveCriticalSection(&d->lock);
        return;
    }
    case 15: case 19: {                                           /* GetOutputBuffer(index, buffer): copies a frame */
        u64 b = c->x[2];
        if (!b) return;
        black_frame(d->width, d->height);
        u64 n = (u64)d->width * d->height * 3 / 2, data = LD64(b + 0x28), cap = LD64(b + 0x18);
        if (data && cap) {
            memcpy((void *)(uintptr_t)data, frame_data, n < cap ? n : cap);
            ST32(b, 0);
            ST32(b + 4, (u32)(n < cap ? n : cap));
        } else {
            buffer_set(b, (u64)(uintptr_t)frame_data, frame_bytes, (u32)n);
        }
        return;
    }
    case 16:                                                      /* GetOutputBufferSize(&size, &count) */
        if (c->x[1]) ST32(c->x[1], d->width * d->height * 3 / 2);
        if (c->x[2]) ST32(c->x[2], NOUT);
        return;
    case 17: {                                                    /* ReleaseOutputBufferIndex(index) */
        u32 o = (u32)c->x[1];
        EnterCriticalSection(&d->lock);
        if (o < NOUT && d->out[o].used && !d->out[o].ready) d->out[o].used = false;
        ev = decode(d);
        LeaveCriticalSection(&d->lock);
        break;
    }
    case 18: return;                                              /* GetLastError */
    default: c->x[0] = ST_EOS; return;
    }
    signal_events(c, d, ev);
    c->x[0] = ST_OK;
}

/* ---------------------------------------------------------------------------------------------- extractor */
static void extractor_call(Ctx *c, int slot, Stream *s) {
    c->x[0] = ST_OK;
    switch (slot) {
    case 2: case 3: return;
    case 4: case 17: {                                            /* SetDataSource(path): "<mount>:/<path in RomFS>" */
        const char *p = (const char *)(uintptr_t)c->x[slot == 4 ? 1 : 2];
        if (!p) return;
        const char *rel = strstr(p, ":/");
        rel = rel ? rel + 2 : p;
        char host[1400];
        snprintf(host, sizeof host, "%s/%s", fs_romfs_dir(), rel);
        mp4_probe(s, host);
        hlog(1, "movie: %s: %lld.%03lld s, %lld us per frame, %ux%u (frames are black)", rel,
             s->duration / 1000000, s->duration / 1000 % 1000, s->frame, s->width, s->height);
        return;
    }
    case 5: return;                                               /* SetDataSource(data, size) */
    case 6: if (c->x[1]) ST32(c->x[1], 1); return;               /* GetTrackCount: one */
    case 7: describe_track(c, c->x[2], s->width, s->height); return;   /* GetTrackConfiguration */
    case 8: case 9: case 16: return;                              /* Select / Unselect / GetBufferedRange */
    case 10: {                                                    /* SeekTo(time, mode): to the sample at or before */
        s64 t = (s64)c->x[1];
        if (t < 0) t = 0;
        s->cur = t / s->frame * s->frame;
        hlog(2, "movie: seek to %lld us -> %lld", t, s->cur);
        return;
    }
    case 11:                                                      /* GetTrackIndexForAvailableData(&track) */
        if (s->cur >= s->duration) { c->x[0] = ST_EOS; return; }
        if (c->x[1]) ST32(c->x[1], 0);
        return;
    case 12:                                                      /* Read(buffer): a one-byte sample */
        if (s->cur >= s->duration) { c->x[0] = ST_EOS; return; }
        if (c->x[1] && LD64(c->x[1] + 0x28) && LD64(c->x[1] + 0x18)) { ST32(c->x[1], 0); ST32(c->x[1] + 4, 1); }
        return;
    case 13: s->cur += s->frame; return;                          /* Advance */
    case 14:                                                      /* GetSampleTime(&time) */
        if (c->x[1]) ST64(c->x[1], (u64)(s->cur < s->duration ? s->cur : s->duration));
        return;
    case 15: return;                                              /* GetSampleMetaData */
    default: c->x[0] = ST_EOS; return;
    }
}

static void m_call(Ctx *c) {
    int slot = -1;
    bool dec = false;
    for (int i = 2; i < 21 && slot < 0; i++) {
        if (i < 18 && extractor_vt[i] == c->pc) slot = i;
        else if (decoder_vt[i] == c->pc) { slot = i; dec = true; }
    }
    static volatile LONG logged;
    if (slot != 11 && slot != 12 && slot != 13 && slot != 14 && slot != 15 && slot != 17 &&
        InterlockedIncrement(&logged) <= 100)
        hlog(1, "movie: %s::%s(%llx, %llx, %llx) from %llx", dec ? "Decoder" : "Extractor",
             dec ? decoder_names[slot] : extractor_names[slot], c->x[1], c->x[2], c->x[3], c->x[30]);
    void *st = STATE(c);
    if (!st) { c->x[0] = ST_EOS; return; }
    if (dec) decoder_call(c, slot, st);
    else extractor_call(c, slot, st);
}

static void init_vtables(void) {
    for (int i = 2; i < 18; i++) extractor_vt[i] = hostcall_register(extractor_names[i], m_call);
    for (int i = 2; i < 21; i++) decoder_vt[i] = hostcall_register(decoder_names[i], m_call);
    ready = true;
}

static void construct(Ctx *c, const u64 *vt, void *state) {
    static SRWLOCK init = SRWLOCK_INIT;
    AcquireSRWLockExclusive(&init);
    if (!ready) init_vtables();
    ReleaseSRWLockExclusive(&init);
    ST64(c->x[0], (u64)(uintptr_t)(vt + 2));   /* the object's vtable pointer skips the two header words */
    ST64(c->x[0] + 8, (u64)(uintptr_t)state);
}

static void new_extractor(Ctx *c) {
    Stream *s = calloc(1, sizeof *s);
    s->duration = 10 * 1000000;         /* if the file can't be read: ten seconds of 30 fps 1280x720 */
    s->frame = 33333;
    s->width = 1280;
    s->height = 720;
    construct(c, extractor_vt, s);
}

static void new_decoder(Ctx *c) {
    Dec *d = calloc(1, sizeof *d);
    InitializeCriticalSection(&d->lock);
    d->width = 1280;
    d->height = 720;
    construct(c, decoder_vt, d);
}

void h__ZN5movie9ExtractorC1ENS_13ContainerTypeE(Ctx *c) { new_extractor(c); }
void h__ZN5movie9ExtractorC2ENS_13ContainerTypeE(Ctx *c) { new_extractor(c); }
void h__ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeE(Ctx *c) { new_decoder(c); }
void h__ZN5movie7DecoderC2ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeE(Ctx *c) { new_decoder(c); }
