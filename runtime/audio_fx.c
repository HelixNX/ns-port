/* The audio renderer's effects with state: delay, reverb and I3DL2 reverb, in float, with the console's structure
 * and constants:
 *   delay    a delay line per channel; feedback through a channel-mixing matrix and a one-pole low pass.
 *   reverb   early reflections tapped from a pre-delay line, then a four-line feedback delay network whose lines
 *            each end in a one-pole HF damper and a Schroeder all-pass; mode tables give the tap and line times.
 *   I3DL2    the same network shape driven by I3DL2 parameters (millibels, seconds, percent): a low-passed input,
 *            twenty early taps, two all-passes and a shelf filter per line.
 * Gains in the parameters are Q14 (reverb, delay) or floats (I3DL2). Angles in the console's formulas count 256
 * to a full turn (1280 / kHz is 5 kHz). Six-channel effects arrive in the order FL FR C LFE BL BR (audio_dsp.c
 * remaps older revisions' FL FR BL BR C LFE). */
#include "audio_dsp.h"
#include <stdlib.h>
#include <math.h>

#define PI 3.14159265358979f

static float q14(const u8 *p) { s32 v; memcpy(&v, p, 4); return v / 16384.0f; }
static float f32at(const u8 *p) { float v; memcpy(&v, p, 4); return v; }
static u32 u32at(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static float pow10c(float v) { return v >= 0 ? 1.0f : v <= -5.3f ? 0.0f : powf(10.0f, v); }
static u32 channels(const ArEffect *e) {
    u16 n;
    memcpy(&n, e->param + 0xe, 2);
    return n == 1 || n == 2 || n == 4 || n == 6 ? n : 0;
}

/* a delay line of len samples (up to max): read() is the sample written len writes ago */
typedef struct Line { float *buf; u32 cap, len, w; float k; } Line;
static void line_init(Line *l, u32 max) {
    l->cap = max + 1;
    l->buf = calloc(l->cap, sizeof(float));
    l->len = max;
    l->w = 0;
}
static void line_free(Line *l) { free(l->buf); }
static void line_len(Line *l, s64 len) { l->len = len < 1 ? 1 : len >= l->cap ? l->cap - 1 : (u32)len; }
static inline float line_read(const Line *l) { return l->buf[(l->w + l->cap - l->len) % l->cap]; }
static inline float line_tap(const Line *l, u32 k) { return l->buf[(l->w + l->cap - 1 - k % l->cap) % l->cap]; }
static inline void line_write(Line *l, float x) { l->buf[l->w] = x; if (++l->w == l->cap) l->w = 0; }
static inline float line_tick(Line *l, float x) { float y = line_read(l); line_write(l, x); return y; }
/* Schroeder all-pass around a line with coefficient k */
static inline float allpass(Line *l, float x) {
    float v = line_read(l), m = x - v * l->k;
    line_write(l, m);
    return v + m * l->k;
}

/* ---------------------------------------------------------------------------------------------- delay */
typedef struct Delay {
    Line line[AR_CHANNELS];
    float lp[AR_CHANNELS];
    float fb, direct, cross, lp_fb, lp_gain;
} Delay;

static void delay_params(Delay *d, const ArEffect *e, u32 rate, u32 n) {
    float spread = q14(e->param + 0x2c);
    d->fb = q14(e->param + 0x20) * 0.98f;
    d->direct = d->fb * (1 - spread);
    d->cross = (n == 4 || n == 6 ? spread / 2 : spread) * d->fb;
    d->lp_fb = q14(e->param + 0x30) * 0.95f;
    d->lp_gain = 1 - d->lp_fb;
    for (u32 c = 0; c < n; c++) line_len(&d->line[c], (s64)u32at(e->param + 0x14) * rate / 1000);
}

void fx_delay(ArEffect *e, u32 rate, float **in, float **out, u32 frames) {
    u32 n = channels(e);
    if (!n) return;
    Delay *d = e->state;
    if (!d) {
        e->state = d = calloc(1, sizeof *d);
        u32 max = (u32)((u64)u32at(e->param + 0x10) * rate / 1000);
        for (u32 c = 0; c < AR_CHANNELS; c++) line_init(&d->line[c], max ? max : 1);
        e->changed = true;
    }
    if (e->changed) delay_params(d, e, rate, n);
    float in_gain = q14(e->param + 0x1c), wet = q14(e->param + 0x24), dry = q14(e->param + 0x28);
    /* feedback matrix m[from][to]: neighbours get the cross gain; 6 channels: the LFE feeds only itself */
    float m[AR_CHANNELS][AR_CHANNELS] = {0};
    float D = d->direct, X = d->cross;
    if (n == 1) m[0][0] = d->fb;
    else if (n == 2) { m[0][0] = m[1][1] = D; m[0][1] = m[1][0] = X; }
    else if (n == 4) {
        static const u8 near4[4][2] = {{1, 2}, {0, 3}, {0, 3}, {1, 2}};
        for (int c = 0; c < 4; c++) { m[c][c] = D; m[c][near4[c][0]] = m[c][near4[c][1]] = X; }
    } else {
        static const s8 near6[6][2] = {{2, 4}, {2, 5}, {0, 1}, {-1, -1}, {0, 5}, {1, 4}};
        for (int c = 0; c < 6; c++) {
            m[c][c] = D;
            for (int k = 0; k < 2; k++) if (near6[c][k] >= 0) m[c][near6[c][k]] = X;
        }
        m[3][3] = q14(e->param + 0x20);
    }
    for (u32 k = 0; k < frames; k++) {
        float x[AR_CHANNELS], y[AR_CHANNELS];
        for (u32 c = 0; c < n; c++) { x[c] = in[c][k]; y[c] = line_read(&d->line[c]); }
        for (u32 c = 0; c < n; c++) {
            float g = x[c] * in_gain;
            for (u32 j = 0; j < n; j++) g += y[j] * m[j][c];
            d->lp[c] = g * d->lp_gain + d->lp[c] * d->lp_fb;
            line_write(&d->line[c], d->lp[c]);
        }
        for (u32 c = 0; c < n; c++) out[c][k] = x[c] * dry + y[c] * wet;
    }
}

/* ---------------------------------------------------------------------------------------------- reverb */
static const float early_times[5][11] = {          /* ms, per early mode; [10] is where the late part taps */
    {0, 3.5f, 2.8f, 3.9f, 2.7f, 13.4f, 7.9f, 8.4f, 9.9f, 12.0f, 12.5f},
    {0, 11.8f, 5.5f, 11.2f, 10.4f, 38.1f, 22.2f, 29.6f, 21.2f, 24.8f, 40.0f},
    {0, 41.5f, 20.5f, 41.3f, 0, 29.5f, 33.8f, 45.2f, 46.8f, 0, 50.0f},
    {33.1f, 43.3f, 22.8f, 37.9f, 14.9f, 35.3f, 17.9f, 34.2f, 0, 43.3f, 50.0f},
    {0}};
static const float early_gains[5][10] = {
    {0.7f, 0.68f, 0.7f, 0.68f, 0.7f, 0.68f, 0.7f, 0.68f, 0.68f, 0.68f},
    {0.7f, 0.68f, 0.7f, 0.68f, 0.7f, 0.68f, 0.68f, 0.68f, 0.68f, 0.68f},
    {0.5f, 0.7f, 0.7f, 0.68f, 0.5f, 0.68f, 0.68f, 0.7f, 0.68f, 0},
    {0.93f, 0.92f, 0.87f, 0.86f, 0.94f, 0.81f, 0.8f, 0.77f, 0.76f, 0.65f},
    {0}};
static const float fdn_times[5][4] = {             /* ms, per late mode */
    {53.953247f, 79.192566f, 116.23877f, 130.615295f}, {53.953247f, 79.192566f, 116.23877f, 170.615295f},
    {5, 10, 5, 10}, {47.03f, 71, 103, 170}, {53.953247f, 79.192566f, 116.23877f, 170.615295f}};
static const float decay_times[5][4] = {{7, 9, 13, 17}, {7, 9, 13, 17}, {1, 1, 1, 1}, {7, 7, 13, 9}, {7, 9, 13, 17}};
static const float fdn_max_times[4] = {53.953247f, 79.192566f, 116.23877f, 170.615295f};
static const float decay_max_times[4] = {7, 9, 13, 17};

typedef struct Reverb {
    Line pre, center, fdn[4], ap[4];
    u32 tap[10], late_tap;
    float tap_gain[10], hf_keep[4], hf_gain[4], fb[4];
} Reverb;

static void reverb_params(Reverb *v, const ArEffect *e, u32 rate) {
    const u8 *p = e->param;
    float ms = rate / 1000.0f;
    u32 early = u32at(p + 0x14), late = u32at(p + 0x20);
    if (early > 4) early = 4;
    if (late > 4) late = 4;
    float pre = q14(p + 0x1c), egain = q14(p + 0x18), colour = q14(p + 0x30), decay = q14(p + 0x28);
    float ratio = q14(p + 0x2c);
    if (pre < 0) pre = 0;
    for (int t = 0; t < 10; t++) {
        float at = (pre + early_times[early][t]) * ms;
        v->tap[t] = at < 0 ? 0 : at > v->pre.cap - 1 ? v->pre.cap - 1 : (u32)at;
        float g = egain * early_gains[early][t];
        v->tap_gain[t] = g < 0 ? 0 : g > 1 ? 1 : g;
    }
    if (channels(e) == 2) { v->tap_gain[4] *= 0.5f; v->tap_gain[5] *= 0.5f; }
    float at = (pre + early_times[early][10]) * ms;
    v->late_tap = at < 0 ? 0 : at > v->pre.cap - 1 ? v->pre.cap - 1 : (u32)at;
    if (colour < 0) colour = 0;
    if (colour > 1) colour = 1;
    if (decay <= 0) decay = 0.0001f;
    if (ratio < 0.001f) ratio = 0.001f;
    if (ratio > 0.999f) ratio = 0.999f;
    float cosw = cosf(2 * PI * 5000 / rate);         /* HF reference */
    for (int i = 0; i < 4; i++) {
        line_len(&v->fdn[i], (s64)(fdn_times[late][i] * ms) - 1);
        line_len(&v->ap[i], (s64)(decay_times[late][i] * ms));
        v->ap[i].k = 0.6f * (1 - colour);
        /* dB per pass through the longest loop, scaled so the tail falls 60 dB in the decay time */
        float b = -3 * (fdn_max_times[i] + decay_max_times[i]) / decay;
        float keep = 0, pass = 1;
        if (ratio <= 0.9949f) {
            float x = pow10c((1 / ratio - 1) * 2 / 100 * (b / 10));
            if (x < 0.0001f) x = 0.0001f;
            float f = 1 - x;
            if (f < 0.0001f) f = 0.0001f;
            float g = 2 - cosw * x * 2, h = g * g - f * f * 4;
            keep = (g - sqrtf(h > 0 ? h : 0)) / (f * 2);
            pass = 1 - keep;
        }
        v->hf_keep[i] = keep;
        v->hf_gain[i] = pow10c(b / 1000) * pass * 0.70709f;
    }
}

void fx_reverb(ArEffect *e, u32 rate, float **in, float **out, u32 frames) {
    u32 n = channels(e);
    if (!n) return;
    Reverb *v = e->state;
    float ms = rate / 1000.0f;
    if (!v) {
        e->state = v = calloc(1, sizeof *v);
        for (int i = 0; i < 4; i++) {
            line_init(&v->fdn[i], (u32)(fdn_max_times[i] * ms));
            line_init(&v->ap[i], (u32)(decay_max_times[i] * ms));
        }
        line_init(&v->pre, (u32)(350 * ms));
        line_init(&v->center, (u32)(5 * ms));
        e->changed = true;
    }
    if (e->changed) reverb_params(v, e, rate);
    static const u8 taps[7][10] = {
        [1] = {0}, [2] = {0, 0, 1, 1, 0, 1, 0, 0, 1, 1}, [4] = {0, 0, 1, 1, 0, 1, 2, 2, 3, 3},
        [6] = {0, 0, 1, 1, 2, 2, 4, 4, 5, 5}};
    const u8 *p = e->param;
    float base = q14(p + 0x34), late = q14(p + 0x24), wet = q14(p + 0x38), dry = q14(p + 0x3c);
    for (u32 k = 0; k < frames; k++) {
        float o[AR_CHANNELS] = {0}, x = 0;
        for (int t = 0; t < 10; t++) {
            float s = line_tap(&v->pre, v->tap[t]) * v->tap_gain[t];
            o[taps[n][t]] += s;
            if (n == 6) o[3] += s;
        }
        if (n == 6) o[3] *= 0.2f;
        for (u32 c = 0; c < n; c++) x += in[c][k];
        line_write(&v->pre, x * base);
        for (int i = 0; i < 4; i++) v->fb[i] = v->fb[i] * v->hf_keep[i] + line_read(&v->fdn[i]) * v->hf_gain[i];
        float pd = line_tap(&v->pre, v->late_tap) * late, a[4];
        float mix[4] = {v->fb[2] + v->fb[1] + pd, -v->fb[0] - v->fb[3] + pd, v->fb[0] - v->fb[3] + pd,
                        v->fb[1] - v->fb[2] + pd};
        for (int i = 0; i < 4; i++) {
            a[i] = allpass(&v->ap[i], mix[i]);
            line_write(&v->fdn[i], a[i]);
        }
        if (n == 6) {
            float late6[6] = {a[0], a[1], line_tick(&v->center, (a[2] - a[3]) * 0.5f), a[3], a[2], a[3]};
            for (u32 c = 0; c < 6; c++) out[c][k] = in[c][k] * dry + (o[c] + late6[c]) * wet;
        } else
            for (u32 c = 0; c < n; c++) out[c][k] = in[c][k] * dry + (o[c] + a[c]) * wet;
    }
}

/* ---------------------------------------------------------------------------------------------- I3DL2 */
static const float i3_min_times[4] = {5, 6, 13, 14}, i3_max_times[4] = {45.7042f, 82.7817f, 149.938293f, 271.575806f};
static const float i3_ap0_times[4] = {17, 13, 9, 7}, i3_ap1_times[4] = {19, 11, 10, 6};
static const float i3_tap_times[20] = {
    0.017136f, 0.059154f, 0.161733f, 0.390186f, 0.425262f, 0.455411f, 0.689737f, 0.745910f, 0.833844f, 0.859502f,
    0.0f,      0.075024f, 0.168788f, 0.299901f, 0.337443f, 0.371903f, 0.599011f, 0.716741f, 0.817859f, 0.851664f};
static const float i3_tap_gains[20] = {
    0.67096f, 0.61027f, 1.0f,     0.3568f,  0.68361f, 0.65978f, 0.51939f, 0.24712f, 0.45945f, 0.45021f,
    0.64196f, 0.54879f, 0.92925f, 0.3827f,  0.72867f, 0.69794f, 0.5464f,  0.24563f, 0.45214f, 0.44042f};

typedef struct I3dl2 {
    Line early, center, fdn[4], ap0[4], ap1[4];
    u32 tap[20], late_tap;
    float early_gain, late_gain, dry, lp_keep, lp_pass, lp;
    float shelf[4][3], z[4];
} I3dl2;

static void i3dl2_params(I3dl2 *v, const ArEffect *e, u32 rate) {
    const u8 *p = e->param;
    float ms = rate / 1000.0f;
    float room_hf = f32at(p + 0x18), ref = f32at(p + 0x1c), decay = f32at(p + 0x20), hf_ratio = f32at(p + 0x24);
    float room = f32at(p + 0x28), refl = f32at(p + 0x2c), rev = f32at(p + 0x30), diffusion = f32at(p + 0x34);
    float refl_delay = f32at(p + 0x38), late_delay = f32at(p + 0x3c), density = f32at(p + 0x40);
    v->dry = f32at(p + 0x44);
    v->early_gain = pow10c(fminf(room + refl, 5000) / 2000);
    v->late_gain = pow10c(fminf(room + rev, 5000) / 2000);
    float hf = pow10c(room_hf / 2000);
    if (hf >= 1) { v->lp_keep = 0; v->lp_pass = 1; }
    else {                                           /* one-pole low pass with gain hf at the reference */
        float a = 1 - hf, b = 2 - cosf(2 * PI * ref / rate) * hf * 2, c = sqrtf(fmaxf(b * b - a * a * 4, 0));
        v->lp_keep = fminf((b - c) / (a * 2), 0.99723f);
        v->lp_pass = 1 - v->lp_keep;
    }
    v->late_tap = (u32)fmaxf((refl_delay + late_delay) * 1000 * ms, 0);
    float diff = diffusion * 0.6f * 0.01f;
    float half = PI * ref / 2 / rate, cot = cosf(half) / sinf(half);
    if (decay <= 0) decay = 0.0001f;
    if (hf_ratio <= 0) hf_ratio = 0.0001f;
    for (int i = 0; i < 4; i++) {
        line_len(&v->fdn[i], (s64)((i3_min_times[i] + density / 100 * (i3_max_times[i] - i3_min_times[i])) * ms));
        float a = (float)(v->fdn[i].len + v->ap0[i].len + v->ap1[i].len) * -60 / (decay * rate);
        float b = a / hf_ratio, d = pow10c((b - a) / 40), g = pow10c((b + a) / 40) * 0.7071f;
        v->shelf[i][0] = (cot * d + 1) * g / (cot + d);
        v->shelf[i][1] = (1 - cot * d) * g / (cot + d);
        v->shelf[i][2] = (cot - d) / (cot + d);
        v->ap0[i].k = diff;
        v->ap1[i].k = diff * -0.9f;
    }
    float span = (late_delay * 0.9998f + 0.02f) * 1000, at = refl_delay * 1000;
    for (int t = 0; t < 20; t++) {
        float len = (at + span * i3_tap_times[t]) * ms;
        v->tap[t] = len < 0 ? 0 : len >= v->early.cap - 1 ? v->early.cap - 1 : (u32)len;
    }
    if (v->late_tap >= v->early.cap) v->late_tap = v->early.cap - 1;
}

void fx_i3dl2(ArEffect *e, u32 rate, float **in, float **out, u32 frames) {
    u32 n = channels(e);
    if (!n) return;
    I3dl2 *v = e->state;
    float ms = rate / 1000.0f;
    if (!v) {
        e->state = v = calloc(1, sizeof *v);
        for (int i = 0; i < 4; i++) {
            line_init(&v->fdn[i], (u32)(i3_max_times[i] * ms));
            line_init(&v->ap0[i], (u32)(i3_ap0_times[i] * ms));
            line_init(&v->ap1[i], (u32)(i3_ap1_times[i] * ms));
        }
        line_init(&v->center, (u32)(5 * ms));
        line_init(&v->early, (u32)(400 * ms));
        e->changed = true;
    }
    if (e->changed) i3dl2_params(v, e, rate);
    static const u8 taps[7][20] = {
        [1] = {0}, [2] = {0, 0, 0, 1, 1, 1, 1, 0, 0, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1, 1},
        [4] = {0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 0, 0, 0, 0, 3, 3, 3},
        [6] = {2, 0, 0, 1, 1, 1, 1, 4, 4, 4, 1, 1, 1, 0, 0, 0, 0, 5, 5, 5}};
    for (u32 k = 0; k < frames; k++) {
        float o[AR_CHANNELS] = {0}, x = 0, a[4], f[4];
        float late = line_tap(&v->early, v->late_tap) * v->late_gain;
        for (int t = 0; t < 20; t++) {
            float s = line_tap(&v->early, v->tap[t]) * i3_tap_gains[t];
            o[taps[n][t]] += s;
            if (n == 6) o[3] += s;
        }
        for (u32 c = 0; c < n; c++) x += in[c][k];
        v->lp = x * v->lp_pass + v->lp * v->lp_keep;
        line_write(&v->early, v->lp);
        for (u32 c = 0; c < n; c++) o[c] *= v->early_gain;
        for (int i = 0; i < 4; i++) {             /* HF shelf on each line's output */
            float y = line_read(&v->fdn[i]);
            f[i] = y * v->shelf[i][0] + v->z[i];
            v->z[i] = f[i] * v->shelf[i][2] + y * v->shelf[i][1];
        }
        float mix[4] = {f[1] + f[2] + late, -f[0] - f[3] + late, f[0] - f[3] + late, f[1] - f[2] + late};
        for (int i = 0; i < 4; i++) {
            a[i] = allpass(&v->ap1[i], allpass(&v->ap0[i], mix[i]));
            line_write(&v->fdn[i], a[i]);
        }
        if (n == 6) {
            float late6[6] = {a[0], a[1], line_tick(&v->center, (a[2] - a[3]) * 0.5f), a[3], a[2], a[3]};
            for (u32 c = 0; c < 6; c++) out[c][k] = o[c] + late6[c] + v->dry * in[c][k];
        } else
            for (u32 c = 0; c < n; c++) out[c][k] = o[c] + a[c] + v->dry * in[c][k];
    }
}

void fx_free(ArEffect *e) {
    if (!e->state) return;
    switch (e->type) {
    case FX_DELAY: {
        Delay *d = e->state;
        for (int c = 0; c < AR_CHANNELS; c++) line_free(&d->line[c]);
        break;
    }
    case FX_REVERB: {
        Reverb *v = e->state;
        for (int i = 0; i < 4; i++) { line_free(&v->fdn[i]); line_free(&v->ap[i]); }
        line_free(&v->pre);
        line_free(&v->center);
        break;
    }
    case FX_I3DL2: {
        I3dl2 *v = e->state;
        for (int i = 0; i < 4; i++) { line_free(&v->fdn[i]); line_free(&v->ap0[i]); line_free(&v->ap1[i]); }
        line_free(&v->early);
        line_free(&v->center);
        break;
    }
    }
}
