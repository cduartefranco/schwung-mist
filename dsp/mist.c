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
 * [L0,R0,L1,R1,...], 48000 Hz.
 *
 * P1 scope (8 knobs, no mod matrix — that is P2/P3):
 *   SIZE  AMOUNT  SPIKE  BLUR  PITCH  FB-PRE  FB-POST  MIX
 * WIDTH/VOL + FREEZE/REVERSE toggles live on a second param level.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

#include "audio_fx_api_v1.h"

#define SR          48000.0f
#define TWO_PI      6.283185307179586f
#define RINGLEN     262144           /* 2^18 samples ~5.46s — input+feedback ring */
#define RMASK       (RINGLEN - 1)
#define MAX_GRAINS  24                /* bounded voice pool — no malloc in process */
#define MAX_SIZE_MS 500.0f
#define MIN_SIZE_MS 8.0f

static const host_api_v1_t *g_host = NULL;

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

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

typedef struct {
    char  module_dir[256];

    /* user params (normalized where noted) */
    float p_size;      /* ms, 8..500 */
    float p_amount;    /* 0..1 window decay roundness */
    float p_spike;     /* 0..1 window attack sharpness */
    float p_blur;      /* 0..1 position/pitch jitter */
    float p_pitch;     /* semitones, -36..+36 */
    float p_fb_pre;    /* 0..1 */
    float p_fb_post;   /* 0..1 */
    float p_width;     /* 0..1 */
    float p_mix;       /* 0..1 */
    float p_vol;       /* 0..1.5 (linear gain) */
    int   p_freeze;    /* 0/1 — stop writing input into the ring */
    int   p_reverse;   /* 0/1 — grains read backwards */

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

static inline float soft_limit(float x, float T) {
    float a = fabsf(x);
    if (a <= T) return x;
    float s = x < 0.0f ? -1.0f : 1.0f;
    return s * (T + (1.0f - T) * tanhf((a - T) / (1.0f - T)));
}

/* ---- v2 API ---- */
static void *v2_create(const char *module_dir, const char *config_json) {
    (void)config_json;
    mist_t *e = (mist_t *)calloc(1, sizeof(mist_t));
    if (!e) { v2_log("alloc failed"); return NULL; }
    if (module_dir) strncpy(e->module_dir, module_dir, sizeof(e->module_dir) - 1);

    e->ring = (float *)calloc(RINGLEN, sizeof(float));
    if (!e->ring) { v2_log("ring alloc failed"); free(e); return NULL; }

    /* defaults = the Python reference's confirmed-praised "05 pitch down -12"
       preset ("very successful, lots of character and weight") — the prior
       defaults (pitch=0, fb=0) were a near-transparent grain-copy of the
       input, so the first touch on hardware barely registered as an effect. */
    e->p_size = 220.0f; e->p_amount = 0.6f; e->p_spike = 0.5f; e->p_blur = 0.15f;
    e->p_pitch = -12.0f; e->p_fb_pre = 0.0f; e->p_fb_post = 0.45f;
    e->p_width = 0.6f; e->p_mix = 1.0f; e->p_vol = 0.9f;
    e->p_freeze = 0; e->p_reverse = 0;
    e->rng = 0xC0FFEEu;
    e->next_onset = 0.0f;

    v2_log("instance created");
    return e;
}

static void v2_destroy(void *inst) {
    mist_t *e = (mist_t *)inst; if (!e) return;
    free(e->ring); free(e);
    v2_log("instance destroyed");
}

static void v2_process(void *inst, int16_t *io, int frames) {
    mist_t *e = (mist_t *)inst;
    if (!e || !e->ring) return;

    const float size_n  = clampf(e->p_size, MIN_SIZE_MS, MAX_SIZE_MS) * 0.001f * SR;
    /* DENSITY is implicit from SIZE on the base page (P1 has no separate density
       knob yet — see MIST_ARCHITECTURE §5 page-2 note); target ~60% grain overlap. */
    const float density = clampf(SR / (size_n * 0.6f), 4.0f, 200.0f);
    const float grain_spacing = SR / density;

    /* adaptive feedback delay: TRACKS grain size (the Simplos signature) —
       pre = tight (grain_delay), post = looser (4x), both still adapt with SIZE */
    const float ratio_max = powf(2.0f, fabsf(e->p_pitch) / 12.0f) * (1.0f + e->p_blur);
    const unsigned grain_delay = (unsigned)(size_n * fmaxf(1.0f, ratio_max)) + 64;
    const unsigned post_delay  = grain_delay * 4u;

    const float damp_fc_pre  = 7000.0f;
    const float damp_fc_post = 5500.0f;
    const float dampB_pre  = 1.0f - expf(-TWO_PI * damp_fc_pre  / SR);
    const float dampB_post = 1.0f - expf(-TWO_PI * damp_fc_post / SR);
    const float dcB = 1.0f - expf(-TWO_PI * 20.0f / SR);
    const float MK = 1.7f;                     /* feedback make-up (re-granulation loss) */

    const float atk_n = fmaxf(1.0f, (0.02f + 0.33f * (1.0f - e->p_spike)) * size_n);
    const float dec_n = fmaxf(1.0f, size_n - atk_n);
    const float atk_k = 1.0f / (0.3f + 3.0f * e->p_spike);
    const float decAmt = e->p_amount;

    const float overlap = fmaxf(1.0f, grain_spacing > 0 ? (SR / grain_spacing) * (size_n / SR) : 1.0f);
    const float gnorm = 1.0f / sqrtf(overlap);

    for (int n = 0; n < frames; n++) {
        float xL = io[n * 2]     / 32768.0f;
        float xR = io[n * 2 + 1] / 32768.0f;
        float xin = 0.5f * (xL + xR);

        /* --- feedback taps: read pre/post delayed wet from the ring, process, write --- */
        float rd_pre  = read_cubic(e->ring, (float)e->wr - (float)grain_delay);
        float rd_post = read_cubic(e->ring, (float)e->wr - (float)post_delay);

        e->damp_pre  += dampB_pre  * (rd_pre  - e->damp_pre);
        e->damp_post += dampB_post * (rd_post - e->damp_post);
        float wpre = e->damp_pre, wpost = e->damp_post;

        /* DC blocker = subtract a slow one-pole lowpass estimate of the DC/sub */
        e->dcy_pre  += dcB * (wpre  - e->dcy_pre);  wpre  -= e->dcy_pre;
        e->dcy_post += dcB * (wpost - e->dcy_post); wpost -= e->dcy_post;

        wpre  = soft_limit(wpre,  0.9f);
        wpost = soft_limit(wpost, 0.9f);

        float fb_in = e->p_fb_pre * MK * wpre + e->p_fb_post * MK * wpost;

        float write_val = e->p_freeze ? 0.0f : xin;
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
                    float st = e->p_pitch + randf(&e->rng) * 12.0f * e->p_blur;
                    g->ratio = powf(2.0f, st / 12.0f) * (e->p_reverse ? -1.0f : 1.0f);
                    float posjit = randf(&e->rng) * e->p_blur * size_n;
                    g->read_pos = (float)e->wr - (float)grain_delay + posjit;
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
        float side = 0.5f * (gl - gr) * (0.3f + 1.7f * e->p_width);
        float wL = mid + side, wR = mid - side;

        float oL = (1.0f - e->p_mix) * xL + e->p_mix * wL;
        float oR = (1.0f - e->p_mix) * xR + e->p_mix * wR;
        oL *= e->p_vol; oR *= e->p_vol;
        oL = soft_limit(oL, 0.7f); oR = soft_limit(oR, 0.7f);

        io[n * 2]     = (int16_t)(clampf(oL, -1.0f, 1.0f) * 32767.0f);
        io[n * 2 + 1] = (int16_t)(clampf(oR, -1.0f, 1.0f) * 32767.0f);
    }
}

static void v2_set(void *inst, const char *key, const char *val) {
    mist_t *e = (mist_t *)inst; if (!e || !key || !val) return;

    if (strcmp(key, "size") == 0) {
        e->p_size = clampf((float)atof(val), MIN_SIZE_MS, MAX_SIZE_MS);
    } else if (strcmp(key, "amount") == 0) {
        e->p_amount = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "spike") == 0) {
        e->p_spike = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "blur") == 0) {
        e->p_blur = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "pitch") == 0) {
        e->p_pitch = clampf((float)atof(val), -36.0f, 36.0f);
    } else if (strcmp(key, "fb_pre") == 0) {
        e->p_fb_pre = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "fb_post") == 0) {
        e->p_fb_post = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "width") == 0) {
        e->p_width = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "mix") == 0) {
        e->p_mix = clampf((float)atof(val), 0.0f, 1.0f);
    } else if (strcmp(key, "vol") == 0) {
        e->p_vol = clampf((float)atof(val), 0.0f, 1.5f);
    } else if (strcmp(key, "freeze") == 0) {
        e->p_freeze = atoi(val) ? 1 : 0;
    } else if (strcmp(key, "reverse") == 0) {
        e->p_reverse = atoi(val) ? 1 : 0;
    } else if (strcmp(key, "state") == 0) {
        float f;
        if (json_num(val, "size", &f) == 0)     e->p_size = clampf(f, MIN_SIZE_MS, MAX_SIZE_MS);
        if (json_num(val, "amount", &f) == 0)   e->p_amount = clampf(f, 0.0f, 1.0f);
        if (json_num(val, "spike", &f) == 0)    e->p_spike = clampf(f, 0.0f, 1.0f);
        if (json_num(val, "blur", &f) == 0)     e->p_blur = clampf(f, 0.0f, 1.0f);
        if (json_num(val, "pitch", &f) == 0)    e->p_pitch = clampf(f, -36.0f, 36.0f);
        if (json_num(val, "fb_pre", &f) == 0)   e->p_fb_pre = clampf(f, 0.0f, 1.0f);
        if (json_num(val, "fb_post", &f) == 0)  e->p_fb_post = clampf(f, 0.0f, 1.0f);
        if (json_num(val, "width", &f) == 0)    e->p_width = clampf(f, 0.0f, 1.0f);
        if (json_num(val, "mix", &f) == 0)      e->p_mix = clampf(f, 0.0f, 1.0f);
        if (json_num(val, "vol", &f) == 0)      e->p_vol = clampf(f, 0.0f, 1.5f);
    }
}

static int v2_get(void *inst, const char *key, char *buf, int len) {
    mist_t *e = (mist_t *)inst; if (!e) return -1;

    if (strcmp(key, "size") == 0)     return snprintf(buf, len, "%d", (int)e->p_size);
    if (strcmp(key, "amount") == 0)   return snprintf(buf, len, "%.2f", e->p_amount);
    if (strcmp(key, "spike") == 0)    return snprintf(buf, len, "%.2f", e->p_spike);
    if (strcmp(key, "blur") == 0)     return snprintf(buf, len, "%.2f", e->p_blur);
    if (strcmp(key, "pitch") == 0)    return snprintf(buf, len, "%.1f", e->p_pitch);
    if (strcmp(key, "fb_pre") == 0)   return snprintf(buf, len, "%.2f", e->p_fb_pre);
    if (strcmp(key, "fb_post") == 0)  return snprintf(buf, len, "%.2f", e->p_fb_post);
    if (strcmp(key, "width") == 0)    return snprintf(buf, len, "%.2f", e->p_width);
    if (strcmp(key, "mix") == 0)      return snprintf(buf, len, "%.2f", e->p_mix);
    if (strcmp(key, "vol") == 0)      return snprintf(buf, len, "%.2f", e->p_vol);
    if (strcmp(key, "freeze") == 0)   return snprintf(buf, len, "%d", e->p_freeze);
    if (strcmp(key, "reverse") == 0)  return snprintf(buf, len, "%d", e->p_reverse);
    if (strcmp(key, "name") == 0)     return snprintf(buf, len, "MIST");
    if (strcmp(key, "state") == 0)
        return snprintf(buf, len,
            "{\"size\":%.1f,\"amount\":%.3f,\"spike\":%.3f,\"blur\":%.3f,\"pitch\":%.1f,"
            "\"fb_pre\":%.3f,\"fb_post\":%.3f,\"width\":%.3f,\"mix\":%.3f,\"vol\":%.3f}",
            e->p_size, e->p_amount, e->p_spike, e->p_blur, e->p_pitch,
            e->p_fb_pre, e->p_fb_post, e->p_width, e->p_mix, e->p_vol);

    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *h = "{\"modes\":null,\"levels\":{\"root\":{\"children\":null,"
            "\"knobs\":[\"size\",\"amount\",\"spike\",\"blur\",\"pitch\",\"fb_pre\",\"fb_post\",\"mix\"],"
            "\"params\":[\"size\",\"amount\",\"spike\",\"blur\",\"pitch\",\"fb_pre\",\"fb_post\",\"mix\"]}}}";
        int l = strlen(h); if (l < len) { strcpy(buf, h); return l; } return -1;
    }
    if (strcmp(key, "chain_params") == 0) {
        const char *p = "["
            "{\"key\":\"size\",\"name\":\"Size\",\"type\":\"int\",\"min\":8,\"max\":500,\"default\":220,\"step\":1,\"unit\":\"ms\"},"
            "{\"key\":\"amount\",\"name\":\"Amount\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.6,\"step\":0.01},"
            "{\"key\":\"spike\",\"name\":\"Spike\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.5,\"step\":0.01},"
            "{\"key\":\"blur\",\"name\":\"Blur\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.15,\"step\":0.01},"
            "{\"key\":\"pitch\",\"name\":\"Pitch\",\"type\":\"float\",\"min\":-36,\"max\":36,\"default\":-12,\"step\":0.1,\"unit\":\"st\"},"
            "{\"key\":\"fb_pre\",\"name\":\"FB Pre\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0,\"step\":0.01},"
            "{\"key\":\"fb_post\",\"name\":\"FB Post\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.45,\"step\":0.01},"
            "{\"key\":\"mix\",\"name\":\"Mix\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":1.0,\"step\":0.01},"
            "{\"key\":\"width\",\"name\":\"Width\",\"type\":\"float\",\"min\":0,\"max\":1,\"default\":0.6,\"step\":0.01},"
            "{\"key\":\"vol\",\"name\":\"Vol\",\"type\":\"float\",\"min\":0,\"max\":1.5,\"default\":0.9,\"step\":0.01},"
            "{\"key\":\"freeze\",\"name\":\"Freeze\",\"type\":\"enum\",\"options\":[\"off\",\"on\"],\"default\":0},"
            "{\"key\":\"reverse\",\"name\":\"Reverse\",\"type\":\"enum\",\"options\":[\"off\",\"on\"],\"default\":0}"
        "]";
        int l = strlen(p); if (l < len) { strcpy(buf, p); return l; } return -1;
    }
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
