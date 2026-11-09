/*
 * (MIST) — granular feedback processor Audio FX for schwung (Ableton Move).
 *
 * Simplos-inspired (Kentaro Tools) granular PROCESSOR: incoming audio writes
 * into a ring buffer; grains re-play its recent past (windowed, pitch-shifted,
 * cubic-interpolated); a feedback loop whose read delay TRACKS the grain size
 * keeps the loop tight ("minimal feedback latency" — the Simplos signature).
 * A pre/post feedback blend morphs a dry grain cloud into a spatial-delay
 * instrument. Original DSP — recreates the *behavior*, not Simplos's code.
 * See modules/mist/MIST_ARCHITECTURE.md for the full design + the Python
 * reference this core must match: modules/mist/tools/mist_render.py.
 *
 * schwung Audio FX ABI v2 (same skeleton as ECO/schwung-tapescam): the host
 * calls move_audio_fx_init_v2(host) once; instances are created per Signal
 * Chain slot. Audio is processed IN-PLACE — stereo interleaved int16
 * [L0,R0,L1,R1,...], 44100 Hz (host MOVE_SAMPLE_RATE — NOT 48k, a v0.2.3 bug).
 *
 * v0.3 (P2a): every parameter lives in ONE table (g_params) that drives
 * set/get/state/chain_params. Adds IN GAIN and the first modulator, the
 * AUDIO TRIGGER (atrg): a transient detector on the dry input that fires a
 * 0..1 envelope on each hit (THRESH / RETURN / SCALE) and sweeps up to
 * TRG_SLOTS destinations, each across a min..max range (range-based routing,
 * Simplos/Stranular-style — not signed depth). Ranges are offsets from the
 * knob's own position, so the knob stays live while routed.
 * The menu LAYOUT lives only in module.json (we return -1 for ui_hierarchy,
 * which makes both the slot and Master-FX hosts fall back to it).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <math.h>
#include <stdint.h>

#include "audio_fx_api_v1.h"

#define SR          44100.0f          /* = host MOVE_SAMPLE_RATE (plugin_api_v1.h) */
#define TWO_PI      6.283185307179586f
#define RINGLEN     262144           /* 2^18 samples ~5.94s @44.1k — input+feedback ring */
#define RMASK       (RINGLEN - 1)
#define MAX_GRAINS  24                /* bounded voice pool — no malloc in process */
#define MAX_SIZE_MS 500.0f
#define MIN_SIZE_MS 8.0f
#define TRG_SLOTS   2

static const host_api_v1_t *g_host = NULL;

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ======================= parameter table ======================= */
enum { PT_FLOAT, PT_INT, PT_ENUM };

enum {
    P_SIZE, P_AMOUNT, P_SPIKE, P_BLUR, P_PITCH, P_FB_PRE, P_FB_POST, P_MIX,
    P_WIDTH, P_VOL, P_IN_GAIN, P_FREEZE, P_REVERSE,
    P_TRG_THRESH, P_TRG_RETURN, P_TRG_SCALE,
    P_TRG1_DEST, P_TRG1_MIN, P_TRG1_MAX,
    P_TRG2_DEST, P_TRG2_MIN, P_TRG2_MAX,
    P_COUNT
};

typedef struct {
    const char *key, *name;
    int   type;
    float min, max, def, step;
    const char *unit;      /* NULL = none */
    const char *opts;      /* enum options, '|'-separated */
} pdef_t;

/* Modulation destinations (index into this list = the DEST enum value).
   Entry 0 is "Off". Everything else is a continuous base param. */
#define DEST_OPTS "Off|Size|Amount|Spike|Blur|Pitch|FB Pre|FB Post|Mix|Width|Vol"
static const int g_dest_param[] = {
    -1, P_SIZE, P_AMOUNT, P_SPIKE, P_BLUR, P_PITCH, P_FB_PRE, P_FB_POST, P_MIX, P_WIDTH, P_VOL
};
#define N_DEST ((int)(sizeof(g_dest_param) / sizeof(g_dest_param[0])))

static const pdef_t g_params[P_COUNT] = {
    /* base — defaults = the praised "05 pitch down -12" preset */
    [P_SIZE]      = {"size",    "Size",    PT_INT,   MIN_SIZE_MS, MAX_SIZE_MS, 220, 1,    "ms", NULL},
    [P_AMOUNT]    = {"amount",  "Amount",  PT_FLOAT, 0, 1,    0.60f, 0.01f, NULL, NULL},
    [P_SPIKE]     = {"spike",   "Spike",   PT_FLOAT, 0, 1,    0.50f, 0.01f, NULL, NULL},
    [P_BLUR]      = {"blur",    "Blur",    PT_FLOAT, 0, 1,    0.15f, 0.01f, NULL, NULL},
    [P_PITCH]     = {"pitch",   "Pitch",   PT_FLOAT, -36, 36, -12,   0.1f,  "st", NULL},
    [P_FB_PRE]    = {"fb_pre",  "FB Pre",  PT_FLOAT, 0, 1,    0.00f, 0.01f, NULL, NULL},
    [P_FB_POST]   = {"fb_post", "FB Post", PT_FLOAT, 0, 1,    0.45f, 0.01f, NULL, NULL},
    [P_MIX]       = {"mix",     "Mix",     PT_FLOAT, 0, 1,    1.00f, 0.01f, NULL, NULL},
    [P_WIDTH]     = {"width",   "Width",   PT_FLOAT, 0, 1,    0.60f, 0.01f, NULL, NULL},
    [P_VOL]       = {"vol",     "Vol",     PT_FLOAT, 0, 1.5f, 0.90f, 0.01f, NULL, NULL},
    [P_IN_GAIN]   = {"in_gain", "In Gain", PT_FLOAT, -12, 24, 6.0f,  0.5f,  "dB", NULL},
    [P_FREEZE]    = {"freeze",  "Freeze",  PT_ENUM,  0, 1,    0, 1, NULL, "off|on"},
    [P_REVERSE]   = {"reverse", "Reverse", PT_ENUM,  0, 1,    0, 1, NULL, "off|on"},
    /* audio trigger (mod-atrg) */
    [P_TRG_THRESH]= {"trg_thresh","Thresh", PT_FLOAT, -60, 0,  -24,  0.5f,  "dB", NULL},
    [P_TRG_RETURN]= {"trg_return","Return", PT_INT,   10, 2000, 300,  1,    "ms", NULL},
    [P_TRG_SCALE] = {"trg_scale", "Scale",  PT_FLOAT, 0, 1,    0.50f, 0.01f, NULL, NULL},
    [P_TRG1_DEST] = {"trg1_dest", "T1 Dest",PT_ENUM,  0, N_DEST - 1, 7 /*FB Post*/, 1, NULL, DEST_OPTS},
    [P_TRG1_MIN]  = {"trg1_min",  "T1 Min", PT_FLOAT, -1, 1,   0.00f, 0.01f, NULL, NULL},
    [P_TRG1_MAX]  = {"trg1_max",  "T1 Max", PT_FLOAT, -1, 1,   0.35f, 0.01f, NULL, NULL},
    [P_TRG2_DEST] = {"trg2_dest", "T2 Dest",PT_ENUM,  0, N_DEST - 1, 0 /*Off*/,     1, NULL, DEST_OPTS},
    [P_TRG2_MIN]  = {"trg2_min",  "T2 Min", PT_FLOAT, -1, 1,   0.00f, 0.01f, NULL, NULL},
    [P_TRG2_MAX]  = {"trg2_max",  "T2 Max", PT_FLOAT, -1, 1,  -0.30f, 0.01f, NULL, NULL},
};

static const int g_slot_params[TRG_SLOTS][3] = {
    {P_TRG1_DEST, P_TRG1_MIN, P_TRG1_MAX},
    {P_TRG2_DEST, P_TRG2_MIN, P_TRG2_MAX},
};

static inline float p_norm(int i, float v) {
    const pdef_t *d = &g_params[i]; return (v - d->min) / (d->max - d->min);
}
static inline float p_denorm(int i, float n) {
    const pdef_t *d = &g_params[i]; return d->min + n * (d->max - d->min);
}

/* parse an enum value given either as an index ("3") or an option label ("FB Post") */
static float parse_enum(const pdef_t *d, const char *val) {
    if ((val[0] >= '0' && val[0] <= '9') || val[0] == '-') return (float)atoi(val);
    const char *o = d->opts; int idx = 0;
    while (o && *o) {
        const char *bar = strchr(o, '|');
        size_t n = bar ? (size_t)(bar - o) : strlen(o);
        if (strlen(val) == n && strncasecmp(o, val, n) == 0) return (float)idx;
        idx++; o = bar ? bar + 1 : NULL;
    }
    return d->def;
}

/* ======================= DSP helpers ======================= */
/* ---- simple fast PRNG (xorshift32) — no libc rand() in the audio thread ---- */
static inline uint32_t xorshift32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}
static inline float randf(uint32_t *s) {                 /* -1..1 */
    return ((float)(xorshift32(s) & 0xFFFFFF) / (float)0xFFFFFF) * 2.0f - 1.0f;
}

/* 4-point cubic (Catmull-Rom) read from the ring at a fractional sample index */
static inline float read_cubic(const float *buf, float idx) {
    int i1 = (int)floorf(idx);
    float f = idx - (float)i1;
    unsigned m0 = (unsigned)(i1 - 1) & RMASK;
    unsigned m1 = (unsigned)(i1    ) & RMASK;
    unsigned m2 = (unsigned)(i1 + 1) & RMASK;
    unsigned m3 = (unsigned)(i1 + 2) & RMASK;
    float y0 = buf[m0], y1 = buf[m1], y2 = buf[m2], y3 = buf[m3];
    float c0 = y1;
    float c1 = 0.5f * (y2 - y0);
    float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
    float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
    return ((c3 * f + c2) * f + c1) * f + c0;
}

static inline float soft_limit(float x, float T) {
    float a = fabsf(x);
    if (a <= T) return x;
    float s = x < 0.0f ? -1.0f : 1.0f;
    return s * (T + (1.0f - T) * tanhf((a - T) / (1.0f - T)));
}

typedef struct {
    int    active;
    float  age;        /* samples since spawn */
    float  len;         /* grain length in samples (captured at spawn) */
    float  read_pos;    /* fractional read index into the ring, advances by ratio */
    float  ratio;        /* pitch playback rate */
    float  pan;           /* -1..1 */
    /* window shape captured at spawn (SPIKE/AMOUNT can move mid-grain otherwise) */
    float  atk_n, dec_n;  /* attack/decay length in samples */
    float  atk_k;         /* attack curve exponent */
} grain_t;

/* audio trigger (mod-atrg) state */
typedef struct {
    float det;          /* peak follower on the dry input (linear) */
    float env;          /* output envelope 0..1 */
    float target;       /* envelope target during the attack window */
    int   armed;        /* re-arms once the input drops 6 dB under threshold */
    int   atk_left;     /* samples left in the attack (peak-capture) window */
    int   holdoff;      /* min samples between triggers */
} atrg_t;

typedef struct {
    char  module_dir[256];

    float pv[P_COUNT];      /* base (knob) values, in each param's own units */

    /* ring buffer: holds input + feedback (what grains read from) */
    float *ring;
    unsigned wr;

    /* grain voice pool */
    grain_t grains[MAX_GRAINS];
    float   next_onset;     /* samples until the next grain spawn (control-rate accum) */
    uint32_t rng;

    /* feedback path state (pre + post taps, each: DC blocker + damp LP) */
    float dcy_pre, damp_pre;
    float dcy_post, damp_post;

    /* smoothed feedback delay — SIZE/PITCH/BLUR can now move per block under
       modulation, so the tap position glides instead of jumping (no clicks) */
    float gd_s;
    int   gd_init;

    atrg_t trg;
} mist_t;

/* ---- helpers ---- */
static void v2_log(const char *m) {
    if (g_host && g_host->log) { char b[256]; snprintf(b, sizeof(b), "[mist] %s", m); g_host->log(b); }
}
static int json_num(const char *json, const char *key, float *out) {
    char s[64]; snprintf(s, sizeof(s), "\"%s\":", key);
    const char *p = strstr(json, s); if (!p) return -1;
    p += strlen(s); while (*p == ' ') p++; *out = (float)atof(p); return 0;
}

static void set_pv(mist_t *e, int i, float v) {
    const pdef_t *d = &g_params[i];
    v = clampf(v, d->min, d->max);
    if (d->type != PT_FLOAT) v = floorf(v + 0.5f);
    e->pv[i] = v;
}

/* ---- v2 API ---- */
static void *v2_create(const char *module_dir, const char *config_json) {
    (void)config_json;
    mist_t *e = (mist_t *)calloc(1, sizeof(mist_t));
    if (!e) { v2_log("alloc failed"); return NULL; }
    if (module_dir) strncpy(e->module_dir, module_dir, sizeof(e->module_dir) - 1);

    e->ring = (float *)calloc(RINGLEN, sizeof(float));
    if (!e->ring) { v2_log("ring alloc failed"); free(e); return NULL; }

    for (int i = 0; i < P_COUNT; i++) e->pv[i] = g_params[i].def;
    e->rng = 0xC0FFEEu;
    e->next_onset = 0.0f;
    e->trg.armed = 1;

    v2_log("instance created (v0.3 atrg)");
    return e;
}

static void v2_destroy(void *inst) {
    mist_t *e = (mist_t *)inst; if (!e) return;
    free(e->ring); free(e);
    v2_log("instance destroyed");
}

/* audio trigger: per-sample detector + envelope. x = dry input peak (linear). */
static inline void atrg_tick(atrg_t *t, float x, float thr, float rel_det,
                             float dec, float scale, int atk_len, int holdoff_len) {
    if (x > t->det) t->det = x; else t->det *= rel_det;

    if (t->holdoff > 0) t->holdoff--;
    if (t->armed && t->holdoff == 0 && t->det > thr) {
        t->armed = 0; t->atk_left = atk_len; t->holdoff = holdoff_len;
        t->target = t->env;                 /* retrigger never drops the envelope */
    } else if (!t->armed && t->det < thr * 0.5f) {
        t->armed = 1;                       /* -6 dB hysteresis */
    }

    if (t->atk_left > 0) {
        /* height: SCALE=0 -> every hit full; SCALE=1 -> follows dB over threshold (24 dB span) */
        float over = (t->det > thr) ? 20.0f * log10f(t->det / thr) / 24.0f : 0.0f;
        float h = (1.0f - scale) + scale * clampf(over, 0.0f, 1.0f);
        if (h > t->target) t->target = h;
        t->env += 0.35f * (t->target - t->env);     /* ~1 ms rise — no click */
        t->atk_left--;
    } else {
        t->env *= dec;
    }
}

static void v2_process(void *inst, int16_t *io, int frames) {
    mist_t *e = (mist_t *)inst;
    if (!e || !e->ring) return;

    /* ---- modulation (control rate: once per block) ---- */
    float pe[P_COUNT];                       /* effective values this block */
    memcpy(pe, e->pv, sizeof(pe));
    {
        float off[P_COUNT] = {0};
        int   hit[P_COUNT] = {0};
        float m = e->trg.env;
        for (int s = 0; s < TRG_SLOTS; s++) {
            int d = (int)e->pv[g_slot_params[s][0]];
            if (d <= 0 || d >= N_DEST) continue;
            int pi = g_dest_param[d];
            float mn = e->pv[g_slot_params[s][1]], mx = e->pv[g_slot_params[s][2]];
            off[pi] += mn + (mx - mn) * m;   /* range-based: sweep min..max */
            hit[pi] = 1;
        }
        for (int i = 0; i < P_COUNT; i++)
            if (hit[i]) pe[i] = p_denorm(i, clampf(p_norm(i, e->pv[i]) + off[i], 0.0f, 1.0f));
    }

    const float p_size = pe[P_SIZE], p_amount = pe[P_AMOUNT], p_spike = pe[P_SPIKE];
    const float p_blur = pe[P_BLUR], p_pitch = pe[P_PITCH];
    const float p_fb_pre = pe[P_FB_PRE], p_fb_post = pe[P_FB_POST];
    const float p_mix = pe[P_MIX], p_width = pe[P_WIDTH], p_vol = pe[P_VOL];
    const int   p_freeze = (int)e->pv[P_FREEZE], p_reverse = (int)e->pv[P_REVERSE];
    const float in_gain = powf(10.0f, e->pv[P_IN_GAIN] / 20.0f);

    const float size_n  = clampf(p_size, MIN_SIZE_MS, MAX_SIZE_MS) * 0.001f * SR;
    /* DENSITY is implicit from SIZE on the base page (P1 has no separate density
       knob yet — see MIST_ARCHITECTURE §5 page-2 note); target ~60% grain overlap. */
    const float density = clampf(SR / (size_n * 0.6f), 4.0f, 200.0f);
    const float grain_spacing = SR / density;

    /* adaptive feedback delay: TRACKS grain size (the Simplos signature) —
       pre = tight (grain_delay), post = looser (4x), both still adapt with SIZE */
    const float ratio_max = powf(2.0f, fabsf(p_pitch) / 12.0f) * (1.0f + p_blur);
    const float gd_target = (float)((unsigned)(size_n * fmaxf(1.0f, ratio_max)) + 64);
    if (!e->gd_init) { e->gd_s = gd_target; e->gd_init = 1; }
    const float gd_k = 1.0f - expf(-1.0f / (0.030f * SR));     /* ~30 ms glide */

    const float damp_fc_pre  = 7000.0f;
    const float damp_fc_post = 5500.0f;
    const float dampB_pre  = 1.0f - expf(-TWO_PI * damp_fc_pre  / SR);
    const float dampB_post = 1.0f - expf(-TWO_PI * damp_fc_post / SR);
    const float dcB = 1.0f - expf(-TWO_PI * 20.0f / SR);
    const float MK = 1.7f;                     /* feedback make-up (re-granulation loss) */

    const float atk_n = fmaxf(1.0f, (0.02f + 0.33f * (1.0f - p_spike)) * size_n);
    const float dec_n = fmaxf(1.0f, size_n - atk_n);
    const float atk_k = 1.0f / (0.3f + 3.0f * p_spike);
    const float decAmt = p_amount;

    const float overlap = fmaxf(1.0f, grain_spacing > 0 ? (SR / grain_spacing) * (size_n / SR) : 1.0f);
    const float gnorm = 1.0f / sqrtf(overlap);

    /* trigger constants */
    const float trg_thr  = powf(10.0f, e->pv[P_TRG_THRESH] / 20.0f);
    const float trg_rel  = expf(-1.0f / (0.050f * SR));                    /* 50 ms detector release */
    const float trg_dec  = expf(-3.0f / (e->pv[P_TRG_RETURN] * 0.001f * SR)); /* ~5% after RETURN */
    const float trg_scl  = e->pv[P_TRG_SCALE];
    const int   trg_atk  = (int)(0.005f * SR);                             /* 5 ms peak capture */
    const int   trg_hold = (int)(0.030f * SR);                             /* 30 ms min re-trigger */

    for (int n = 0; n < frames; n++) {
        float xL = io[n * 2]     / 32768.0f;
        float xR = io[n * 2 + 1] / 32768.0f;
        float xin = 0.5f * (xL + xR) * in_gain;

        atrg_tick(&e->trg, fmaxf(fabsf(xL), fabsf(xR)), trg_thr, trg_rel,
                  trg_dec, trg_scl, trg_atk, trg_hold);

        e->gd_s += gd_k * (gd_target - e->gd_s);
        const float grain_delay = e->gd_s;
        const float post_delay  = grain_delay * 4.0f;

        /* --- feedback taps: read pre/post delayed wet from the ring, process, write --- */
        float rd_pre  = read_cubic(e->ring, (float)e->wr - grain_delay);
        float rd_post = read_cubic(e->ring, (float)e->wr - post_delay);

        e->damp_pre  += dampB_pre  * (rd_pre  - e->damp_pre);
        e->damp_post += dampB_post * (rd_post - e->damp_post);
        float wpre = e->damp_pre, wpost = e->damp_post;

        /* DC blocker = subtract a slow one-pole lowpass estimate of the DC/sub */
        e->dcy_pre  += dcB * (wpre  - e->dcy_pre);  wpre  -= e->dcy_pre;
        e->dcy_post += dcB * (wpost - e->dcy_post); wpost -= e->dcy_post;

        wpre  = soft_limit(wpre,  0.9f);
        wpost = soft_limit(wpost, 0.9f);

        float fb_in = p_fb_pre * MK * wpre + p_fb_post * MK * wpost;

        float write_val = p_freeze ? 0.0f : xin;
        e->ring[e->wr] = write_val + fb_in;
        e->wr = (e->wr + 1) & RMASK;

        /* --- control-rate grain spawn (per-sample accumulator) --- */
        e->next_onset -= 1.0f;
        if (e->next_onset <= 0.0f) {
            e->next_onset = grain_spacing * (0.85f + 0.3f * ((randf(&e->rng) + 1.0f) * 0.5f));
            for (int v = 0; v < MAX_GRAINS; v++) {
                if (!e->grains[v].active) {
                    grain_t *g = &e->grains[v];
                    g->active = 1; g->age = 0.0f; g->len = size_n;
                    float st = p_pitch + randf(&e->rng) * 12.0f * p_blur;
                    g->ratio = powf(2.0f, st / 12.0f) * (p_reverse ? -1.0f : 1.0f);
                    float posjit = randf(&e->rng) * p_blur * size_n;
                    g->read_pos = (float)e->wr - grain_delay + posjit;
                    g->pan = randf(&e->rng) * 0.7f;
                    g->atk_n = atk_n; g->dec_n = dec_n; g->atk_k = atk_k;
                    break;                                /* voice pool full -> drop (bounded) */
                }
            }
        }

        /* --- sum active grains --- */
        float gl = 0.0f, gr = 0.0f;
        for (int v = 0; v < MAX_GRAINS; v++) {
            grain_t *g = &e->grains[v];
            if (!g->active) continue;
            float k = g->age;
            float w;
            if (k < g->atk_n) {
                w = powf(k / g->atk_n, g->atk_k);
            } else {
                float t = (k - g->atk_n) / g->dec_n;
                float lin = 1.0f - t;
                float cosShape = 0.5f + 0.5f * cosf((float)M_PI * t);
                w = (1.0f - decAmt) * lin + decAmt * cosShape;
            }
            float s = read_cubic(e->ring, g->read_pos) * w * gnorm;
            gl += s * sqrtf(0.5f * (1.0f - g->pan));
            gr += s * sqrtf(0.5f * (1.0f + g->pan));

            g->read_pos += g->ratio;
            g->age += 1.0f;
            if (g->age >= g->len) g->active = 0;
        }

        /* width (mid/side) on the wet grain sum */
        float mid = 0.5f * (gl + gr);
        float side = 0.5f * (gl - gr) * (0.3f + 1.7f * p_width);
        float wL = mid + side, wR = mid - side;

        float oL = (1.0f - p_mix) * xL + p_mix * wL;
        float oR = (1.0f - p_mix) * xR + p_mix * wR;
        oL *= p_vol; oR *= p_vol;
        oL = soft_limit(oL, 0.7f); oR = soft_limit(oR, 0.7f);

        io[n * 2]     = (int16_t)(clampf(oL, -1.0f, 1.0f) * 32767.0f);
        io[n * 2 + 1] = (int16_t)(clampf(oR, -1.0f, 1.0f) * 32767.0f);
    }
}

static int find_param(const char *key) {
    for (int i = 0; i < P_COUNT; i++) if (strcmp(g_params[i].key, key) == 0) return i;
    return -1;
}

static void v2_set(void *inst, const char *key, const char *val) {
    mist_t *e = (mist_t *)inst; if (!e || !key || !val) return;

    int i = find_param(key);
    if (i >= 0) {
        const pdef_t *d = &g_params[i];
        set_pv(e, i, d->type == PT_ENUM ? parse_enum(d, val) : (float)atof(val));
        return;
    }
    if (strcmp(key, "state") == 0) {
        float f;
        for (int j = 0; j < P_COUNT; j++)
            if (json_num(val, g_params[j].key, &f) == 0) set_pv(e, j, f);
    }
}

static int fmt_value(const mist_t *e, int i, char *buf, int len) {
    const pdef_t *d = &g_params[i];
    if (d->type != PT_FLOAT) return snprintf(buf, len, "%d", (int)e->pv[i]);
    return snprintf(buf, len, d->step < 0.1f ? "%.2f" : "%.1f", e->pv[i]);
}

/* append-with-bounds helper for building JSON into a caller buffer */
static int app(char *buf, int len, int pos, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
#include <stdarg.h>
static int app(char *buf, int len, int pos, const char *fmt, ...) {
    if (pos < 0 || pos >= len) return -1;
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf + pos, (size_t)(len - pos), fmt, ap);
    va_end(ap);
    if (n < 0 || pos + n >= len) return -1;
    return pos + n;
}

static int v2_get(void *inst, const char *key, char *buf, int len) {
    mist_t *e = (mist_t *)inst; if (!e || !key || !buf || len <= 0) return -1;

    int i = find_param(key);
    if (i >= 0) return fmt_value(e, i, buf, len);

    if (strcmp(key, "name") == 0) return snprintf(buf, len, "MIST");
    if (strcmp(key, "trg_env") == 0) return snprintf(buf, len, "%.3f", e->trg.env);  /* live meter */

    if (strcmp(key, "state") == 0) {
        int pos = app(buf, len, 0, "{");
        for (int j = 0; j < P_COUNT && pos >= 0; j++)
            pos = app(buf, len, pos, "%s\"%s\":%.4g", j ? "," : "", g_params[j].key, e->pv[j]);
        pos = app(buf, len, pos, "}");
        return pos;
    }

    if (strcmp(key, "chain_params") == 0) {
        int pos = app(buf, len, 0, "[");
        for (int j = 0; j < P_COUNT && pos >= 0; j++) {
            const pdef_t *d = &g_params[j];
            pos = app(buf, len, pos, "%s{\"key\":\"%s\",\"name\":\"%s\"", j ? "," : "", d->key, d->name);
            if (d->type == PT_ENUM) {
                pos = app(buf, len, pos, ",\"type\":\"enum\",\"options\":[");
                const char *o = d->opts; int first = 1;
                while (o && *o && pos >= 0) {
                    const char *bar = strchr(o, '|');
                    int n = bar ? (int)(bar - o) : (int)strlen(o);
                    pos = app(buf, len, pos, "%s\"%.*s\"", first ? "" : ",", n, o);
                    first = 0; o = bar ? bar + 1 : NULL;
                }
                pos = app(buf, len, pos, "],\"default\":%d}", (int)d->def);
            } else {
                pos = app(buf, len, pos, ",\"type\":\"%s\",\"min\":%g,\"max\":%g,\"default\":%g,\"step\":%g",
                          d->type == PT_INT ? "int" : "float", d->min, d->max, d->def, d->step);
                if (d->unit) pos = app(buf, len, pos, ",\"unit\":\"%s\"", d->unit);
                pos = app(buf, len, pos, "}");
            }
        }
        pos = app(buf, len, pos, "]");
        return pos;
    }

    /* ui_hierarchy: deliberately NOT answered — module.json is the single
       source of the menu layout (both host paths fall back to it). */
    return -1;
}

static audio_fx_api_v2_t g_api;

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    memset(&g_api, 0, sizeof(g_api));
    g_api.api_version     = AUDIO_FX_API_VERSION_2;
    g_api.create_instance = v2_create;
    g_api.destroy_instance= v2_destroy;
    g_api.process_block   = v2_process;
    g_api.set_param       = v2_set;
    g_api.get_param       = v2_get;
    v2_log("(MIST) v2 initialized");
    return &g_api;
}
