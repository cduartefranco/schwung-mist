/*
 * mist_wav — run the REAL dsp/mist.c (dlopen'd .so) over raw audio, exactly as
 * the Move host does: 44.1 kHz stereo interleaved int16, 128-frame blocks, in place.
 *
 *   stdin : raw s16le stereo @44.1k     stdout : raw s16le stereo (same length + tail)
 *   argv  : mist_wav <mist.so> <bpm> <tail_seconds> [key value]...
 *
 * Normally driven by tools/mist_wav.py (WAV in/out, resampling, presets).
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "../dsp/audio_fx_api_v1.h"

static float g_bpm = 120.0f;
static float get_bpm(void) { return g_bpm; }
static int   clock_status(void) { return 2; /* running */ }
static void  logf_(const char *m) { fprintf(stderr, "%s\n", m); }

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: mist_wav <mist.so> <bpm> <tail_s> [key val]...\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    audio_fx_init_v2_fn init = (audio_fx_init_v2_fn)dlsym(h, "move_audio_fx_init_v2");
    if (!init) { fprintf(stderr, "no move_audio_fx_init_v2\n"); return 1; }
    g_bpm = (float)atof(argv[2]);
    long tail = (long)(atof(argv[3]) * 44100.0);

    host_api_v1_t host; memset(&host, 0, sizeof(host));
    host.api_version = 1; host.sample_rate = 44100; host.frames_per_block = 128;
    host.log = logf_; host.get_bpm = get_bpm; host.get_clock_status = clock_status;
    audio_fx_api_v2_t *api = init(&host);
    void *inst = api->create_instance(".", NULL);
    for (int a = 4; a + 1 < argc; a += 2) api->set_param(inst, argv[a], argv[a + 1]);

    int16_t buf[128 * 2];
    size_t got;
    while ((got = fread(buf, sizeof(int16_t) * 2, 128, stdin)) > 0) {
        if (got < 128) memset(buf + got * 2, 0, (128 - got) * 2 * sizeof(int16_t));
        api->process_block(inst, buf, 128);
        fwrite(buf, sizeof(int16_t) * 2, 128, stdout);
    }
    for (long t = 0; t < tail; t += 128) {               /* let the feedback ring out */
        memset(buf, 0, sizeof(buf));
        api->process_block(inst, buf, 128);
        fwrite(buf, sizeof(int16_t) * 2, 128, stdout);
    }
    api->destroy_instance(inst);
    return 0;
}
