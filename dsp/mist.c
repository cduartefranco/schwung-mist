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
#include <time.h>

#include "audio_fx_api_v1.h"

#define SR          44100.0f          /* = host MOVE_SAMPLE_RATE (plugin_api_v1.h) */
#define TWO_PI      6.283185307179586f
#define RINGLEN     262144           /* 2^18 samples ~5.94s @44.1k — input+feedback ring */
#define RMASK       (RINGLEN - 1)
#define MAX_GRAINS  96                /* bounded voice pool — no malloc in process */
#define MIP_LEVELS  5                 /* ring + 4 band-limited copies (anti-alias for pitch-up) */
#define SCATTER_MS  40.0f             /* SCATTER=1 -> +-40 ms grain position spread */
#define WET_MAKEUP  2.37f             /* +7.5 dB */
#define WLEN        1024              /* grain window lookup table (rebuilt on SPIKE/AMOUNT change) */
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
    P_WIDTH, P_VOL, P_IN_GAIN, P_FB_TIME, P_FREEZE, P_REVERSE,
    P_TRG_THRESH, P_TRG_RETURN, P_TRG_SCALE,
    P_TRG1_DEST, P_TRG1_MIN, P_TRG1_MAX,
    P_TRG2_DEST, P_TRG2_MIN, P_TRG2_MAX,
    P_DENSITY, P_SPREAD, P_GMODE, P_EU_PATTERN, P_EU_RATE, P_EU_ROT, P_EU_BURST, P_EU_JITTER,
    P_SCATTER,
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
/* append-only: saved presets store the index */
#define DEST_OPTS "Off|Size|Amount|Spike|Blur|Pitch|FB Pre|FB Post|Mix|Width|Vol|Density|Spread|Scatter"
static const int g_dest_param[] = {
    -1, P_SIZE, P_AMOUNT, P_SPIKE, P_BLUR, P_PITCH, P_FB_PRE, P_FB_POST, P_MIX, P_WIDTH, P_VOL,
    P_DENSITY, P_SPREAD, P_SCATTER
};

/* Euclidean grain-trigger variants (pulses k over steps n; Toussaint's classics).
   append-only: saved presets store the index */
#define EU_OPTS "5/8 Cinquillo|3/8 Tresillo|2/5|3/4|3/7|4/7|4/9|5/9|5/11|5/12|5/16 Bossa|7/12|7/16|9/16|11/16|13/16|Every step"
static const int g_eu_k[] = {5, 3, 2, 3, 3, 4, 4, 5, 5, 5, 5,  7,  7,  9, 11, 13, 1};
static const int g_eu_n[] = {8, 8, 5, 4, 7, 7, 9, 9, 11, 12, 16, 12, 16, 16, 16, 16, 1};
#define N_EU ((int)(sizeof(g_eu_k) / sizeof(g_eu_k[0])))
/* step length as a fraction of a 4/4 bar */
#define EU_RATE_OPTS "1/32|1/16T|1/16|1/8T|1/8|1/4"
static const float g_eu_rate[] = {1.0f/32, 1.0f/24, 1.0f/16, 1.0f/12, 1.0f/8, 1.0f/4};
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
    [P_MIX]       = {"mix",     "Mix",     PT_FLOAT, 0, 1,    0.50f, 0.01f, NULL, NULL},
    [P_WIDTH]     = {"width",   "Width",   PT_FLOAT, 0, 1,    0.60f, 0.01f, NULL, NULL},
    [P_VOL]       = {"vol",     "Vol",     PT_FLOAT, 0, 1.5f, 0.90f, 0.01f, NULL, NULL},
    [P_IN_GAIN]   = {"in_gain", "In Gain", PT_FLOAT, -12, 24, 0.0f,  0.5f,  "dB", NULL},
    /* 0 = tight/auto (post tap = one grain-delay, like FB Pre); >0 = echo time for FB Post */
    [P_FB_TIME]   = {"fb_time", "FB Time", PT_INT,   0, 2000, 0,     1,    "ms", NULL},
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
    /* grain stream: density (= Simplos TRIGGER, ref default 22/s) + euclidean rhythm mode */
    [P_DENSITY]   = {"density",  "Density", PT_FLOAT, 1, 200,  22,    0.5f,  "/s", NULL},
    [P_SPREAD]    = {"spread",   "Spread",  PT_FLOAT, 0, 1,    0.70f, 0.01f, NULL, NULL},
    [P_GMODE]     = {"gmode",    "Mode",    PT_ENUM,  0, 1,    0, 1, NULL, "Free|Euclid"},
    [P_EU_PATTERN]= {"eu_pattern","Pattern",PT_ENUM,  0, 16,   0, 1, NULL, EU_OPTS},
    [P_EU_RATE]   = {"eu_rate",  "Rate",    PT_ENUM,  0, 5,    2, 1, NULL, EU_RATE_OPTS},
    [P_EU_ROT]    = {"eu_rot",   "Rotate",  PT_INT,   0, 15,   0, 1, NULL, NULL},
    [P_EU_BURST]  = {"eu_burst", "Burst",   PT_INT,   1, 8,    1, 1, NULL, NULL},
    [P_EU_JITTER] = {"eu_jitter","Jitter",  PT_FLOAT, 0, 1,    0.05f, 0.01f, NULL, NULL},
    /* creative position randomness (Stranular pos-rnd): +-SCATTER_MS grain position +
       looser free-mode timing. Default 0: measured (tests/quality.py) no comb filtering
       to fix, and on tonal input it adds level wobble (11.7 -> 34 % at pitch 0). */
    [P_SCATTER]   = {"scatter",  "Scatter", PT_FLOAT, 0, 1,    0.00f, 0.01f, NULL, NULL},
};

/* knob-cell widgets (drawn by canvas.js). Emitted in chain_params because that is
   where the host looks for custom viz kinds; a group spans ADJACENT cells on one
   row, span:false roles lend their value without taking a cell. */
static const struct { int p; const char *viz; } g_viz[] = {
    {P_AMOUNT,     "{\"group\":\"win\",\"role\":\"amount\",\"kind\":\"custom:mist_window\"}"},
    {P_SPIKE,      "{\"group\":\"win\",\"role\":\"spike\",\"kind\":\"custom:mist_window\"}"},
    {P_TRG1_DEST,  "{\"group\":\"t1\",\"role\":\"dest\",\"span\":false,\"kind\":\"custom:mist_range\"}"},
    {P_TRG1_MIN,   "{\"group\":\"t1\",\"role\":\"min\",\"kind\":\"custom:mist_range\",\"extra_keys\":[\"trg_env\"]}"},
    {P_TRG1_MAX,   "{\"group\":\"t1\",\"role\":\"max\",\"kind\":\"custom:mist_range\"}"},
    {P_TRG2_DEST,  "{\"group\":\"t2\",\"role\":\"dest\",\"span\":false,\"kind\":\"custom:mist_range\"}"},
    {P_TRG2_MIN,   "{\"group\":\"t2\",\"role\":\"min\",\"kind\":\"custom:mist_range\",\"extra_keys\":[\"trg_env\"]}"},
    {P_TRG2_MAX,   "{\"group\":\"t2\",\"role\":\"max\",\"kind\":\"custom:mist_range\"}"},
    {P_GMODE,      "{\"group\":\"eu\",\"role\":\"mode\",\"span\":false,\"kind\":\"custom:mist_euclid\"}"},
    {P_EU_PATTERN, "{\"group\":\"eu\",\"role\":\"pattern\",\"kind\":\"custom:mist_euclid\",\"extra_keys\":[\"eu_pos\"]}"},
    {P_EU_RATE,    "{\"group\":\"eu\",\"role\":\"rate\",\"kind\":\"custom:mist_euclid\"}"},
    {P_EU_ROT,     "{\"group\":\"eu\",\"role\":\"rot\",\"span\":false,\"kind\":\"custom:mist_euclid\"}"},
};
static const char *viz_for(int p) {
    for (size_t i = 0; i < sizeof(g_viz) / sizeof(g_viz[0]); i++) if (g_viz[i].p == p) return g_viz[i].viz;
    return NULL;
}

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
/* same knee, but the curve flattens at ceiling C (< 1) instead of full scale */
static inline float soft_limit_c(float x, float T, float C) {
    float a = fabsf(x);
    if (a <= T) return x;
    float s = x < 0.0f ? -1.0f : 1.0f;
    return s * (T + (C - T) * tanhf((a - T) / (C - T)));
}

typedef struct {
    int    active;
    float  age;        /* samples since spawn */
    float  len;         /* grain length in samples (captured at spawn) */
    float  read_pos;    /* fractional read index into the ring, advances by ratio */
    float  ratio;        /* pitch playback rate */
    float  gl, gr;        /* equal-power pan gains (computed once at spawn) */
    float  wstep;         /* window-table increment per sample = WLEN / len */
    int    lvl;           /* which band-limited ring copy to read (by |ratio|) */
    float  pan;           /* -1..1 (kept for the Scope page) */
    int    id;            /* spawn counter (stable identity for the Scope page) */
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

    /* STEREO input+feedback rings; mip[c][0] is the full-band ring, mip[c][1..4]
       are 4th-order-Butterworth band-limited copies (fc = 14.3 kHz / 2^L) written
       in parallel. A grain pitched up by r reads the copy whose band x r stays
       under ~14 kHz -> no aliasing (v0.4: a 6 kHz tone at +24 st folded to 20 kHz). */
    float *mip[2][MIP_LEVELS];
    float  bq[2][MIP_LEVELS][2][4];   /* [ch][level][stage] x1 x2 y1 y2 */
    unsigned wr;

    /* grain voice pool */
    grain_t grains[MAX_GRAINS];
    float   next_onset;     /* samples until the next grain spawn (control-rate accum) */
    uint32_t rng;

    /* grain window LUT (ref grain_window(), normalized 0..1 over the grain) */
    float win[WLEN + 1];
    float win_spike, win_amount;

    /* euclidean scheduler */
    float eu_phase;         /* samples into the current step */
    int   eu_step;
    int   eu_pending;       /* a pulse is waiting out its jitter delay */
    float eu_wait;
    int   burst_left;
    float burst_next;
    int   clock_prev;

    /* feedback path (= the reference engine, tools/mist_render.py): the WET grain
       output -> DC block -> damp LP -> soft limit is stored in its own ring, and
       the pre/post taps read THAT back into the input ring. Every pass goes back
       through the grains (re-granulation loss ~0.5), which MK=1.7 makes up for.
       (v0.2.x-0.3.0 tapped the raw input ring instead — no loss, so with MK the
       loop barely decayed: 18 s+ tails = the "out of control" bug.) */
    float *wet[2];                   /* stereo feedback tap rings */
    float dc_x[2], dc_y[2], damp[2];
    float lp_x1[2], lp_x2[2], lp_y1[2], lp_y2[2];   /* 14 kHz 2-pole output LP */

    /* smoothed feedback delays — SIZE/PITCH/BLUR/FB TIME can move per block under
       modulation, so tap positions glide instead of jumping (no clicks) */
    float gd_s, pd_s;
    int   gd_init;

    atrg_t trg;

    /* Scope page telemetry (read by get_param "viz"; benign cross-thread races only) */
    int   grain_seq;
    float in_pk, out_pk;          /* peak since the last viz read */
    float pe_snap[P_COUNT];       /* effective (modulated) params of the last block */
    int   eu_k_snap, eu_n_snap;

    /* CPU stats (process_block wall time), logged on destroy (not in the RT path) */
    double cpu_sum_us; float cpu_max_us; long cpu_n;
} mist_t;

/* mip filter coefficients: [level][stage] b0 b1 b2 a1 a2 (shared, built once) */
static float g_mip_c[MIP_LEVELS][2][5];
static int   g_mip_ready = 0;
static void build_mip_coefs(void) {
    const float Qs[2] = {0.54119610f, 1.30656296f};          /* 4th-order Butterworth */
    for (int L = 1; L < MIP_LEVELS; L++) {
        float fc = 14300.0f / (float)(1 << L);
        float K = tanf((float)M_PI * fc / SR);
        for (int st = 0; st < 2; st++) {
            float Q = Qs[st], N = 1.0f / (1.0f + K / Q + K * K);
            g_mip_c[L][st][0] = K * K * N; g_mip_c[L][st][1] = 2.0f * K * K * N; g_mip_c[L][st][2] = K * K * N;
            g_mip_c[L][st][3] = 2.0f * (K * K - 1.0f) * N; g_mip_c[L][st][4] = (1.0f - K / Q + K * K) * N;
        }
    }
    g_mip_ready = 1;
}

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

    int ok = 1;
    for (int c = 0; c < 2; c++) {
        for (int L = 0; L < MIP_LEVELS; L++) ok &= (e->mip[c][L] = (float *)calloc(RINGLEN, sizeof(float))) != NULL;
        ok &= (e->wet[c] = (float *)calloc(RINGLEN, sizeof(float))) != NULL;
    }
    if (!ok) {
        v2_log("ring alloc failed");
        for (int c = 0; c < 2; c++) { for (int L = 0; L < MIP_LEVELS; L++) free(e->mip[c][L]); free(e->wet[c]); }
        free(e); return NULL;
    }
    if (!g_mip_ready) build_mip_coefs();

    for (int i = 0; i < P_COUNT; i++) e->pv[i] = g_params[i].def;
    e->rng = 0xC0FFEEu;
    e->next_onset = 0.0f;
    e->trg.armed = 1;
    e->win_spike = e->win_amount = -1.0f;    /* force first window build */
    e->eu_step = -1;

    v2_log("instance created (v0.5 stereo+antialias)");
    return e;
}

static void v2_destroy(void *inst) {
    mist_t *e = (mist_t *)inst; if (!e) return;
    char m[160];
    snprintf(m, sizeof(m), "instance destroyed — cpu per 128-frame block: avg %.0f us, max %.0f us (budget ~2900 us) over %ld blocks",
             e->cpu_n ? e->cpu_sum_us / (double)e->cpu_n : 0.0, e->cpu_max_us, e->cpu_n);
    for (int c = 0; c < 2; c++) { for (int L = 0; L < MIP_LEVELS; L++) free(e->mip[c][L]); free(e->wet[c]); }
    free(e);
    v2_log(m);
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

/* ref grain_window(): SPIKE sharpens the attack (power curve), AMOUNT rounds the decay */
static void build_window(mist_t *e, float spike, float amount) {
    const float atk = 0.02f + 0.33f * (1.0f - spike);
    const float ka  = 0.3f + 3.0f * spike;
    for (int i = 0; i <= WLEN; i++) {
        float x = (float)i / (float)WLEN, w;
        if (x < atk) w = powf(x / atk, 1.0f / ka);
        else {
            float t = (x - atk) / (1.0f - atk);
            w = (1.0f - amount) * (1.0f - t) + amount * (0.5f + 0.5f * cosf((float)M_PI * t));
        }
        e->win[i] = w;
    }
    e->win[WLEN] = 0.0f;
    e->win_spike = spike; e->win_amount = amount;
}

static void spawn_grain(mist_t *e, float size_n, float grain_delay, float pitch,
                        float blur, int reverse, float spread, float scatter_n) {
    for (int v = 0; v < MAX_GRAINS; v++) {
        grain_t *g = &e->grains[v];
        if (g->active) continue;
        g->active = 1; g->age = 0.0f; g->len = size_n;
        g->wstep = (float)WLEN / size_n;
        float st = pitch + randf(&e->rng) * 12.0f * blur;
        float a = powf(2.0f, st / 12.0f);
        g->ratio = a * (reverse ? -1.0f : 1.0f);
        /* band-limited copy: level L keeps fc_L x |ratio| under ~14-20 kHz */
        g->lvl = a <= 1.4f ? 0 : (int)ceilf(log2f(a / 1.4f));
        if (g->lvl >= MIP_LEVELS) g->lvl = MIP_LEVELS - 1;
        float posjit = randf(&e->rng) * (blur * size_n + scatter_n);
        g->read_pos = (float)e->wr - grain_delay + posjit;
        float pan = randf(&e->rng) * spread;
        g->pan = pan; g->id = ++e->grain_seq;
        g->gl = sqrtf(0.5f * (1.0f - pan)); g->gr = sqrtf(0.5f * (1.0f + pan));
        return;
    }
    /* voice pool full -> drop (bounded) */
}

static void v2_process(void *inst, int16_t *io, int frames) {
    mist_t *e = (mist_t *)inst;
    if (!e || !e->mip[0][0]) return;
    struct timespec t_0; clock_gettime(CLOCK_MONOTONIC, &t_0);

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
    const float density = clampf(pe[P_DENSITY], 1.0f, 200.0f);      /* grains / s (ref) */
    const float scatter_n = pe[P_SCATTER] * SCATTER_MS * 0.001f * SR;
    const float free_jit  = 0.15f + 0.35f * pe[P_SCATTER];          /* onset humanize +- */
    const float grain_spacing = SR / density;
    const float spread  = pe[P_SPREAD];

    /* euclidean mode */
    const int   euclid  = (int)e->pv[P_GMODE] == 1;
    const int   eu_i    = (int)clampf(e->pv[P_EU_PATTERN], 0, N_EU - 1);
    const int   eu_k    = g_eu_k[eu_i], eu_n = g_eu_n[eu_i];
    const int   eu_rot  = (int)e->pv[P_EU_ROT];
    const int   burst   = (int)e->pv[P_EU_BURST];
    float bpm = (g_host && g_host->get_bpm) ? g_host->get_bpm() : 120.0f;
    if (!(bpm >= 40.0f && bpm <= 300.0f)) bpm = 120.0f;
    const float step_len = (240.0f / bpm) * g_eu_rate[(int)clampf(e->pv[P_EU_RATE], 0, 5)] * SR;
    const float burst_gap = fminf(grain_spacing, step_len / (float)burst);
    const float jit = e->pv[P_EU_JITTER];
    /* restart the pattern on transport start (if the host exposes the clock) */
    if (g_host && g_host->get_clock_status) {
        int cs = g_host->get_clock_status();
        if (cs == 2 && e->clock_prev != 2) { e->eu_step = -1; e->eu_phase = step_len; }
        e->clock_prev = cs;
    }

    if (fabsf(p_spike - e->win_spike) > 0.004f || fabsf(p_amount - e->win_amount) > 0.004f)
        build_window(e, p_spike, p_amount);

    /* adaptive feedback delay: TRACKS grain size (the Simplos signature) —
       pre = tight (grain_delay), post = looser (4x), both still adapt with SIZE */
    const float ratio_max = powf(2.0f, fabsf(p_pitch) / 12.0f) * (1.0f + p_blur);
    /* max reach back into a ring: grains read up to grain_delay + size + jitter */
    const float max_d = (float)RINGLEN - 3.0f * MAX_SIZE_MS * 0.001f * SR - 1024.0f;
    /* + scatter reach so a forward-jittered grain never reads past the write head */
    const float gd_target = fminf((float)((unsigned)(size_n * fmaxf(1.0f, ratio_max) + scatter_n) + 256), max_d);
    const float pd_target = e->pv[P_FB_TIME] > 0.0f
                          ? fminf(fmaxf(e->pv[P_FB_TIME] * 0.001f * SR, 257.0f), max_d) : gd_target;
    if (!e->gd_init) { e->gd_s = gd_target; e->pd_s = pd_target; e->gd_init = 1; }
    const float gd_k = 1.0f - expf(-1.0f / (0.030f * SR));     /* ~30 ms glide */

    const float dampA = expf(-TWO_PI * 7000.0f / SR);          /* ref preset damp_hz 7000 */
    const float MK = 1.7f;                     /* feedback make-up (re-granulation loss) */

    /* 2-pole Butterworth LP @ 14 kHz (ref out_lp), bilinear */
    const float lpK = tanf((float)M_PI * 14000.0f / SR), lpQ = 0.70710678f;
    const float lpN = 1.0f / (1.0f + lpK / lpQ + lpK * lpK);
    const float lb0 = lpK * lpK * lpN, lb1 = 2.0f * lb0, lb2 = lb0;
    const float la1 = 2.0f * (lpK * lpK - 1.0f) * lpN, la2 = (1.0f - lpK / lpQ + lpK * lpK) * lpN;

    /* overlap normalization (ref): grains alive at once -> 1/sqrt so denser != louder */
    const float eff_rate = euclid ? ((float)eu_k / (float)eu_n) * (SR / step_len) * (float)burst : density;
    const float overlap = fmaxf(1.0f, eff_rate * (size_n / SR));
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
        { float a = fmaxf(fabsf(xL), fabsf(xR)); if (a > e->in_pk) e->in_pk = a; }
        float xin[2] = {xL * in_gain, xR * in_gain};

        atrg_tick(&e->trg, fmaxf(fabsf(xL), fabsf(xR)), trg_thr, trg_rel,
                  trg_dec, trg_scl, trg_atk, trg_hold);

        e->gd_s += gd_k * (gd_target - e->gd_s);
        e->pd_s += gd_k * (pd_target - e->pd_s);
        const float grain_delay = e->gd_s;
        const unsigned idx = e->wr;

        /* --- per channel: feedback (delayed, already-processed WET grain output, see
               struct note) + input -> full-band ring + band-limited mip copies --- */
        for (int c = 0; c < 2; c++) {
            float fb_in = 0.0f;
            if (p_fb_pre  > 0.0f) fb_in += p_fb_pre  * MK * read_cubic(e->wet[c], (float)idx - grain_delay);
            if (p_fb_post > 0.0f) fb_in += p_fb_post * MK * read_cubic(e->wet[c], (float)idx - e->pd_s);
            float v = (p_freeze ? 0.0f : xin[c]) + fb_in;
            e->mip[c][0][idx] = v;
            for (int L = 1; L < MIP_LEVELS; L++) {
                float x = v;
                for (int st = 0; st < 2; st++) {
                    const float *k = g_mip_c[L][st]; float *z = e->bq[c][L][st];
                    float y = k[0] * x + k[1] * z[0] + k[2] * z[1] - k[3] * z[2] - k[4] * z[3];
                    z[1] = z[0]; z[0] = x; z[3] = z[2]; z[2] = y; x = y;
                }
                e->mip[c][L][idx] = x;
            }
        }
        e->wr = (idx + 1) & RMASK;

        /* --- grain scheduling --- */
        if (!euclid) {
            /* free: uniform stream at DENSITY, +-15 % humanize (ref) */
            e->next_onset -= 1.0f;
            if (e->next_onset <= 0.0f) {
                e->next_onset = grain_spacing * (1.0f + free_jit * randf(&e->rng));
                spawn_grain(e, size_n, grain_delay, p_pitch, p_blur, p_reverse, spread, scatter_n);
            }
        } else {
            /* euclid: tempo-synced steps; a pulse step fires BURST grains spaced by
               the density spacing (clamped to fit the step), after a JITTER delay */
            e->eu_phase += 1.0f;
            if (e->eu_phase >= step_len) {
                e->eu_phase -= step_len;
                if (e->eu_phase >= step_len) e->eu_phase = 0.0f;
                e->eu_step = (e->eu_step + 1) % eu_n;
                int i = (e->eu_step + eu_rot) % eu_n;
                if (((i * eu_k) % eu_n) < eu_k) {               /* Bresenham euclid */
                    e->eu_pending = 1;
                    e->eu_wait = ((randf(&e->rng) + 1.0f) * 0.5f) * jit * 0.5f * step_len;
                }
            }
            if (e->eu_pending) {
                e->eu_wait -= 1.0f;
                if (e->eu_wait <= 0.0f) { e->eu_pending = 0; e->burst_left = burst; e->burst_next = 0.0f; }
            }
            if (e->burst_left > 0) {
                e->burst_next -= 1.0f;
                if (e->burst_next <= 0.0f) {
                    spawn_grain(e, size_n, grain_delay, p_pitch, p_blur, p_reverse, spread, scatter_n);
                    e->burst_left--; e->burst_next = burst_gap;
                }
            }
        }

        /* --- sum active grains --- */
        float gl = 0.0f, gr = 0.0f;
        for (int v = 0; v < MAX_GRAINS; v++) {
            grain_t *g = &e->grains[v];
            if (!g->active) continue;
            float wp = g->age * g->wstep;                   /* window LUT, linear interp */
            int   wi = (int)wp; if (wi >= WLEN) wi = WLEN - 1;
            float w  = e->win[wi] + (e->win[wi + 1] - e->win[wi]) * (wp - (float)wi);
            const float wg = w * gnorm;
            gl += read_cubic(e->mip[0][g->lvl], g->read_pos) * wg * g->gl;    /* true stereo */
            gr += read_cubic(e->mip[1][g->lvl], g->read_pos) * wg * g->gr;

            g->read_pos += g->ratio;
            g->age += 1.0f;
            if (g->age >= g->len) g->active = 0;
        }

        /* wet (per channel) -> DC block (R=.995) -> damp LP -> soft limit = feedback tap.
           A centred grain puts 0.707*s on each channel = exactly the v0.4 mono tap
           0.5*(gl+gr), so loop gain (tail length) is unchanged. */
        {
            float wv[2] = {gl, gr};
            for (int c = 0; c < 2; c++) {
                float y = wv[c] - e->dc_x[c] + 0.995f * e->dc_y[c]; e->dc_x[c] = wv[c]; e->dc_y[c] = y;
                e->damp[c] = dampA * e->damp[c] + (1.0f - dampA) * y;
                e->wet[c][idx] = soft_limit(e->damp[c], 0.9f);
            }
        }

        /* width (mid/side) on the wet grain sum */
        float mid = 0.5f * (gl + gr);
        float side = 0.5f * (gl - gr) * (0.3f + 1.7f * p_width);
        /* wet make-up: the overlap-normalized grain sum sits ~7.8 dB under the input
           (measured, tools/mist_wav.py); the ref renders were peak-normalized after
           the fact. Applied AFTER the feedback tap, so loop gain / tails are unchanged. */
        float wL = (mid + side) * WET_MAKEUP, wR = (mid - side) * WET_MAKEUP;

        {   /* 14 kHz LP on the WET only (ref out_lp; dry stays untouched on a master bus) */
            float in2[2] = {wL, wR};
            for (int c = 0; c < 2; c++) {
                float y = lb0 * in2[c] + lb1 * e->lp_x1[c] + lb2 * e->lp_x2[c]
                        - la1 * e->lp_y1[c] - la2 * e->lp_y2[c];
                e->lp_x2[c] = e->lp_x1[c]; e->lp_x1[c] = in2[c];
                e->lp_y2[c] = e->lp_y1[c]; e->lp_y1[c] = y; in2[c] = y;
            }
            wL = in2[0]; wR = in2[1];
        }
        /* dry/wet: both at unity up to 50 %, then crossfade (0 % and 100 % unchanged) —
           inserting MIST on a master bus doesn't drop the level */
        const float dry_g = fminf(1.0f, 2.0f * (1.0f - p_mix));
        const float wet_g = fminf(1.0f, 2.0f * p_mix);
        float oL = dry_g * xL + wet_g * wL;
        float oR = dry_g * xR + wet_g * wR;
        oL *= p_vol; oR *= p_vol;
        oL = soft_limit_c(oL, 0.7f, 0.944f); oR = soft_limit_c(oR, 0.7f, 0.944f);   /* -0.5 dBFS ceiling */

        io[n * 2]     = (int16_t)(clampf(oL, -1.0f, 1.0f) * 32767.0f);
        io[n * 2 + 1] = (int16_t)(clampf(oR, -1.0f, 1.0f) * 32767.0f);
        { float a = fmaxf(fabsf(oL), fabsf(oR)); if (a > e->out_pk) e->out_pk = a; }
    }
    memcpy(e->pe_snap, pe, sizeof(pe));
    e->eu_k_snap = eu_k; e->eu_n_snap = eu_n;

    struct timespec t_1; clock_gettime(CLOCK_MONOTONIC, &t_1);
    float us = (float)((t_1.tv_sec - t_0.tv_sec) * 1e6 + (t_1.tv_nsec - t_0.tv_nsec) * 1e-3);
    e->cpu_sum_us += us; e->cpu_n++; if (us > e->cpu_max_us) e->cpu_max_us = us;
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
    if (strcmp(key, "eu_pos") == 0)    /* step,n,k,rot,mode — euclid strip playhead */
        return snprintf(buf, len, "%d,%d,%d,%d,%d", e->eu_step < 0 ? 0 : e->eu_step,
                        e->eu_n_snap, e->eu_k_snap, (int)e->pv[P_EU_ROT], (int)e->pv[P_GMODE]);
    if (strcmp(key, "viz") == 0) {     /* Scope page: one compact snapshot per read */
        const float *q = e->pe_snap;
        int pos = app(buf, len, 0, "1,%.0f,%.2f,%.2f,%.2f,%.1f,%.2f,%.2f,%.2f,%.1f,%.2f,%.2f,%.2f,%d,%d,%d,%d,%d",
            q[P_SIZE], q[P_BLUR], q[P_SPIKE], q[P_AMOUNT], q[P_PITCH], q[P_FB_PRE], q[P_FB_POST],
            q[P_MIX], q[P_DENSITY], e->trg.env, e->in_pk, e->out_pk, (int)e->pv[P_GMODE],
            e->eu_step < 0 ? 0 : e->eu_step, e->eu_n_snap, e->eu_k_snap, (int)e->pv[P_EU_ROT]);
        e->in_pk = 0.0f; e->out_pk = 0.0f;
        int ng = 0;
        for (int v = 0; v < MAX_GRAINS && pos >= 0 && ng < 24; v++) {
            const grain_t *g = &e->grains[v];
            if (!g->active || g->len <= 0.0f) continue;
            float ph = g->age / g->len;
            int wi = (int)(g->age * g->wstep); if (wi < 0) wi = 0; if (wi > WLEN) wi = WLEN;
            float st = 12.0f * log2f(fabsf(g->ratio) + 1e-6f);
            pos = app(buf, len, pos, ";%d,%d,%d,%d,%d", g->id % 1000, (int)(ph * 99.0f),
                      (int)lroundf(st), (int)(e->win[wi] * 9.0f + 0.5f), (int)lroundf(g->pan * 9.0f));
            ng++;
        }
        return pos;
    }

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
                pos = app(buf, len, pos, "],\"default\":%d", (int)d->def);
                if (viz_for(j)) pos = app(buf, len, pos, ",\"viz\":%s", viz_for(j));
                pos = app(buf, len, pos, "}");
            } else {
                pos = app(buf, len, pos, ",\"type\":\"%s\",\"min\":%g,\"max\":%g,\"default\":%g,\"step\":%g",
                          d->type == PT_INT ? "int" : "float", d->min, d->max, d->def, d->step);
                if (d->unit) pos = app(buf, len, pos, ",\"unit\":\"%s\"", d->unit);
                if (viz_for(j)) pos = app(buf, len, pos, ",\"viz\":%s", viz_for(j));
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
