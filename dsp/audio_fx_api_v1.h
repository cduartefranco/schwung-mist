/*
 * Audio FX Plugin API v1 + v2 (local copy for wrapper builds)
 *
 * Interface for audio effect plugins that process stereo audio IN-PLACE.
 * The host calls move_audio_fx_init_v2(host) once; it returns a struct of
 * function pointers. Instances are created per Signal Chain slot.
 * Audio is stereo interleaved int16 [L0,R0,L1,R1,...] at 48000 Hz.
 */

#ifndef AUDIO_FX_API_V1_H
#define AUDIO_FX_API_V1_H

#include <stdint.h>
#include "plugin_api_v1.h"  /* for host_api_v1_t */

#define AUDIO_FX_API_VERSION 1
#define AUDIO_FX_INIT_SYMBOL "move_audio_fx_init_v1"

/* v1 (global, single-instance) — kept for reference; ECO uses v2 */
typedef struct audio_fx_api_v1 {
    uint32_t api_version;
    int  (*on_load)(const char *module_dir, const char *config_json);
    void (*on_unload)(void);
    void (*process_block)(int16_t *audio_inout, int frames);
    void (*set_param)(const char *key, const char *val);
    int  (*get_param)(const char *key, char *buf, int buf_len);
} audio_fx_api_v1_t;

typedef audio_fx_api_v1_t* (*audio_fx_init_v1_fn)(const host_api_v1_t *host);

/* v2 (instance-based) — this is what ECO implements */
#define AUDIO_FX_API_VERSION_2 2
#define AUDIO_FX_INIT_V2_SYMBOL "move_audio_fx_init_v2"

typedef struct audio_fx_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *config_json);
    void  (*destroy_instance)(void *instance);
    void  (*process_block)(void *instance, int16_t *audio_inout, int frames);
    void  (*set_param)(void *instance, const char *key, const char *val);
    int   (*get_param)(void *instance, const char *key, char *buf, int buf_len);
} audio_fx_api_v2_t;

typedef audio_fx_api_v2_t* (*audio_fx_init_v2_fn)(const host_api_v1_t *host);

#endif /* AUDIO_FX_API_V1_H */
