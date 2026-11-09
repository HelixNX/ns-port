/* The audio renderer's state as the DSP sees it (audren:u fills it from the game's update data, services/audio.c)
 * and the frame renderer (audio_dsp.c): voices are decoded, resampled, filtered and mixed into their mix's buffers,
 * sub mixes run their effects and go to their destination, the final mix goes to the sinks: the host's audio
 * device (SDL3) or a circular buffer in guest memory. Mix buffers are float in 16-bit sample units. */
#ifndef AUDIO_DSP_H
#define AUDIO_DSP_H

#include "hos.h"
#include <windows.h>

#define AR_MIX_BUFFERS 24                   /* per mix */
#define AR_CHANNELS 6                       /* per voice, effect, sink */
#define AR_WAVEBUFFERS 4
#define AR_UNUSED_MIX 0x7fffffff
#define AR_FRAME_MAX 480                    /* samples per frame: 240 at 48 kHz, 160 at 32 kHz */

enum { AR_PCM16 = 2, AR_PCMFLOAT = 5, AR_ADPCM = 6 };
enum { AR_STARTED, AR_STOPPED, AR_REQUEST_STOP, AR_PAUSED };       /* server play states */
enum { FX_NONE, FX_MIX, FX_AUX, FX_DELAY, FX_REVERB, FX_I3DL2, FX_BIQUAD };
enum { SINK_NONE, SINK_DEVICE, SINK_CIRCULAR };

typedef struct ArWaveBuf {
    u64 addr, size, ctx_addr, ctx_size;
    s32 start, end;
    bool loop, stream_ended, sent;          /* sent: handed to the DSP (the game sees it as queued) */
} ArWaveBuf;

/* a voice channel resource: one decoded channel of a voice, with its mix volumes and playback position */
typedef struct ArChannel {
    float vol[AR_MIX_BUFFERS], prev_vol[AR_MIX_BUFFERS], last[AR_MIX_BUFFERS];  /* last: depop source */
    bool valid[AR_WAVEBUFFERS];
    u32 index, consumed, offset;            /* wave buffer playing, buffers consumed, samples into it */
    u64 played;
    double frac;                            /* resampler position, and its last four input samples */
    float hist[4];
    s16 yn1, yn2;                           /* ADPCM predictor history */
    double bq[2][4];                        /* biquad x1 x2 y1 y2 */
} ArChannel;

typedef struct ArVoice {
    bool in_use, was_playing;
    u8 format, state, last_state;
    s8 channels;
    u32 rate, wb_count;
    float pitch, volume, prev_volume;
    struct { bool enabled; s16 b[3], a[2]; } bq[2];
    bool bq_on[2];                          /* filter state is live (reset when a filter is switched on) */
    u64 data_addr, data_size;               /* ADPCM coefficients */
    s32 mix_id;
    ArWaveBuf wb[AR_WAVEBUFFERS];
    u32 res[AR_CHANNELS];
} ArVoice;

typedef struct ArMix {
    bool in_use;
    float volume;
    u32 buffers, offset;                    /* offset: first buffer in the renderer's mix buffers */
    s32 dest;
    int dist;                               /* steps to the final mix */
    float vol[AR_MIX_BUFFERS][AR_MIX_BUFFERS];
} ArMix;

typedef struct ArEffect {
    u8 type;
    bool enabled, is_new;
    s32 mix_id, order;
    u64 wb_addr, wb_size;
    u8 param[0xa0];
    bool changed;                           /* parameters differ from the last frame's */
    void *state;                            /* the effect's own state (audio_fx.c) */
    u64 aux_send, aux_return;               /* Aux: buffer infos (+0x40 the DSP's half) */
} ArEffect;

typedef struct ArSink {
    u8 type;
    bool in_use;
    u32 count;
    s8 inputs[AR_CHANNELS];
    bool downmix;
    float coeff[4];
    u64 addr;                               /* circular buffer */
    u32 size, pos, samples;
} ArSink;

typedef struct ArRenderer {
    u32 sample_rate, sample_count, mix_buffers, mix_count, voice_count, sink_count, effect_count, revision;
    ArVoice *voices;
    ArChannel *chans;
    ArMix *mixes;                           /* [0] is the final mix */
    ArEffect *effects;
    ArSink *sinks;
    int *order;                             /* mixes, farthest from the final mix first */
    float *buf, *depop;                     /* mix_buffers * sample_count; one depop value per buffer */
    bool started, layout_dirty;
    u64 frames;
    KEvent *event;
    SRWLOCK lock;
    volatile bool closing;
} ArRenderer;

void ar_render_frame(ArRenderer *r);        /* under r->lock */
void ar_effect_reset(ArEffect *e);          /* frees an effect's state */

/* effects with state (audio_fx.c): in and out are AR_CHANNELS buffer pointers (NULL past the channel count) */
void fx_reverb(ArEffect *e, u32 rate, float **in, float **out, u32 n);
void fx_i3dl2(ArEffect *e, u32 rate, float **in, float **out, u32 n);
void fx_delay(ArEffect *e, u32 rate, float **in, float **out, u32 n);
void fx_free(ArEffect *e);                  /* frees what the state points to */

/* the host's audio device: stereo 16-bit at 48 kHz */
void audio_init(void);
u32 audio_queued_frames(u32 sample_count);  /* frames queued but not played; ~0u if there is no device */
void audio_output(const s16 *lr, u32 n);

#endif
