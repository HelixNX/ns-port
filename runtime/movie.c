/* Movies (CLAUDE.md, decision 3): subsdk0's movie::Extractor and movie::Decoder start NVIDIA's multimedia stack,
 * which needs nvdrv. For now the constructors main calls (tools/lifter/overrides.txt) build objects of our own:
 * every file holds one 1280x720 H.264 track that is already at its end, so the player sets itself up (the game
 * expects that to work) and never shows a frame. Media Foundation comes later.
 * Status values (movie::Status, from subsdk0's AndroidErrorToNnError): 0 ok, 0x2000 end of stream,
 * 0x5003 try again later, 0x4004 output format changed. */
#include "hos.h"
#include <stdlib.h>

#define ST_OK 0u
#define ST_EOS 0x2000u
#define ST_AGAIN 0x5003u

static const char *const extractor_names[18] = {0, 0, "~Extractor", "~Extractor (delete)", "SetDataSource(path)",
    "SetDataSource(data, size)", "GetTrackCount", "GetTrackConfiguration", "SelectTrack", "UnselectTrack", "SeekTo",
    "GetTrackIndexForAvailableData", "Read", "Advance", "GetSampleTime", "GetSampleMetaData", "GetBufferedRange",
    "SetDataSource(reader, path)"};
static const char *const decoder_names[21] = {0, 0, "~Decoder", "~Decoder (delete)", "Configure", "RegisterClientEvents",
    "GetOutputFormat", "Start", "Stop", "Reset", "Flush", "AcquireInputBufferIndex", "GetInputBuffer",
    "SendInputBufferForDecode", "AcquireOutputBufferIndex", "GetOutputBuffer", "GetOutputBufferSize",
    "ReleaseOutputBufferIndex", "GetLastError", "GetOutputBuffer(format)", "GetOutputBufferFormat"};

/* vtables in the layout of subsdk0's: {offset to top, RTTI, D2, D0, methods...}; one host function per slot */
static u64 extractor_vt[18], decoder_vt[21];
static bool ready;

/* the track, written with the game's own movie::MediaData setters */
static void describe_track(Ctx *c, u64 md) {
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
    c->x[0] = md; c->x[1] = (u64)(uintptr_t)width; c->x[2] = 1280; guest_call(c, set_int32);
    c->x[0] = md; c->x[1] = (u64)(uintptr_t)height; c->x[2] = 720; guest_call(c, set_int32);
    c->x[0] = md; c->x[1] = (u64)(uintptr_t)stride; c->x[2] = 1280; guest_call(c, set_int32);
    if (set_rect) {
        c->x[0] = md; c->x[1] = (u64)(uintptr_t)crop; c->x[2] = 0; c->x[3] = 0; c->x[4] = 1279; c->x[5] = 719;
        guest_call(c, set_rect);
    }
    *c = saved;
}

/* the player's DecoderEvents (four nn::os events: input available, output available, format changed, error):
 * signalled so that its thread wakes up, asks for output and finds the end of the stream */
static u64 client_events;
static void signal_events(Ctx *c) {
    static u64 signal_event;
    if (!signal_event) signal_event = sym_lookup("_ZN2nn2os11SignalEventEPNS0_9EventTypeE");
    if (!client_events || !signal_event) return;
    Ctx saved = *c;
    for (int i = 0; i < 4; i++) {
        u64 ev = LD64(client_events + 8 * i);
        if (!ev) continue;
        c->x[0] = ev;
        guest_call(c, signal_event);
    }
    *c = saved;
}

/* one black 1280x720 frame (NV12: Y 0, UV 128), handed out once with the end-of-stream flag */
static u8 *black_frame;
#define FRAME_BYTES (1280 * 720 * 4)
static int outputs;                     /* output buffers handed out */
static void give_buffer(Ctx *c, u64 buffer) {
    static u64 set_data, set_range;
    if (!set_data) {
        set_data = sym_lookup("_ZN5movie6Buffer18SetDataAndCapacityEPvm");
        set_range = sym_lookup("_ZN5movie6Buffer8SetRangeEii");
    }
    if (!black_frame) {
        black_frame = calloc(1, FRAME_BYTES);
        memset(black_frame + 1280 * 720, 128, 1280 * 720 / 2);
    }
    if (!buffer || !set_data) return;
    Ctx saved = *c;
    c->x[0] = buffer; c->x[1] = (u64)(uintptr_t)black_frame; c->x[2] = FRAME_BYTES; guest_call(c, set_data);
    if (set_range) { c->x[0] = buffer; c->x[1] = 0; c->x[2] = 1280 * 720 * 3 / 2; guest_call(c, set_range); }
    *c = saved;
}

static void m_call(Ctx *c) {
    const char *cls = "?", *name = "?";
    int slot = -1;
    for (int i = 2; i < 21; i++) {
        if (i < 18 && extractor_vt[i] == c->pc) { cls = "Extractor"; name = extractor_names[i]; slot = i; }
        if (decoder_vt[i] == c->pc) { cls = "Decoder"; name = decoder_names[i]; slot = 100 + i; }
    }
    static int logged;
    if (logged++ < 200)
        hlog(1, "movie: %s::%s(%llx, %llx, %llx) from %llx", cls, name, c->x[1], c->x[2], c->x[3], c->x[30]);
    switch (slot) {
    case 2: case 3: case 102: case 103: return;                 /* destructors */
    case 4: case 5: case 17: c->x[0] = ST_OK; return;          /* SetDataSource */
    case 6: if (c->x[1]) ST32(c->x[1], 1); c->x[0] = ST_OK; return;        /* GetTrackCount: one */
    case 7: describe_track(c, c->x[2]); c->x[0] = ST_OK; return;          /* GetTrackConfiguration */
    case 8: case 9: case 10: case 16: c->x[0] = ST_OK; return;  /* Select / Unselect / SeekTo / GetBufferedRange */
    case 105: client_events = c->x[1]; c->x[0] = ST_OK; return; /* RegisterClientEvents */
    case 107: signal_events(c); c->x[0] = ST_OK; return;        /* Start */
    case 104: case 108: case 109: case 110: c->x[0] = ST_OK; return;
    case 117: signal_events(c); c->x[0] = ST_OK; return;        /* ReleaseOutputBufferIndex: the next one is due */
    case 106: case 120: describe_track(c, c->x[1]); c->x[0] = ST_OK; return;  /* GetOutputFormat */
    case 116: if (c->x[1]) ST32(c->x[1], 1280 * 720 * 3 / 2); if (c->x[2]) ST32(c->x[2], 1); c->x[0] = ST_OK; return;
    case 118: c->x[0] = ST_OK; return;                          /* GetLastError */
    case 111: c->x[0] = ST_AGAIN; return;                       /* AcquireInputBufferIndex: nothing to feed */
    case 114: {                                                 /* AcquireOutputBufferIndex(index, timeUs, flags) */
        /* one black frame, then the end of the stream */
        if (outputs >= 2) { c->x[0] = ST_AGAIN; return; }
        if (c->x[1]) ST32(c->x[1], 0);
        if (c->x[2]) ST64(c->x[2], outputs * 33333);
        if (c->x[3]) ST32(c->x[3], outputs ? 4 : 0);            /* BUFFER_FLAG_END_OF_STREAM */
        outputs++;
        c->x[0] = ST_OK;
        return;
    }
    case 115: case 119: give_buffer(c, c->x[2]); c->x[0] = ST_OK; return;   /* GetOutputBuffer(index, buffer) */
    default: c->x[0] = ST_EOS; return;                          /* reads, samples, outputs: the end */
    }
}

static void init_vtables(void) {
    for (int i = 2; i < 18; i++) extractor_vt[i] = hostcall_register(extractor_names[i], m_call);
    for (int i = 2; i < 21; i++) decoder_vt[i] = hostcall_register(decoder_names[i], m_call);
    ready = true;
}

static void construct(Ctx *c, const u64 *vt, const char *what) {
    if (!ready) init_vtables();
    ST64(c->x[0], (u64)(uintptr_t)(vt + 2));   /* the object's vtable pointer skips the two header words */
    static int logged;
    if (logged++ < 4) hlog(1, "movie: %s created (an empty stream: movies are not played yet)", what);
}

void h__ZN5movie9ExtractorC1ENS_13ContainerTypeE(Ctx *c) { construct(c, extractor_vt, "Extractor"); }
void h__ZN5movie9ExtractorC2ENS_13ContainerTypeE(Ctx *c) { construct(c, extractor_vt, "Extractor"); }
void h__ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeE(Ctx *c) {
    construct(c, decoder_vt, "Decoder");
}
void h__ZN5movie7DecoderC2ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeE(Ctx *c) {
    construct(c, decoder_vt, "Decoder");
}
