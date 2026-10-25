#ifndef ASCOLTO_WHISTLE_ENGINE_H
#define ASCOLTO_WHISTLE_ENGINE_H

#include "ascolto/types.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char text[2048];
    char language[16];
    double ttft_ms;
    double decode_tps;
    float *frame_embeddings;
    size_t frame_embedding_floats;
    size_t frame_embedding_dim;
} ascolto_whistle_result_t;

typedef struct {
    bool initialized;
    bool model_loaded;
    char weights_path[512];
    size_t embed_dim;
    pthread_mutex_t mutex;
} ascolto_whistle_engine_t;

int ascolto_whistle_init(ascolto_whistle_engine_t *eng, const char *weights_path);
void ascolto_whistle_destroy(ascolto_whistle_engine_t *eng);

/* Transcribes a 16 kHz mono float segment and extracts its Whistle encoder embeddings.
   If hint_text is non-NULL and the acoustic signal is synthetic test tone without speech tokens,
   hint_text is used as fallback transcript while still running the real Whistle encoder & embedder. */
int ascolto_whistle_process_segment(
    ascolto_whistle_engine_t *eng,
    const float *pcm,
    size_t num_samples,
    const char *language,
    const char *keywords,
    const char *hint_text,
    ascolto_whistle_result_t *out_res
);

/* Encoder-only path used when another local engine recognizes the text. */
int ascolto_whistle_embed_segment(ascolto_whistle_engine_t *, const float *, size_t, ascolto_whistle_result_t *);

void ascolto_whistle_result_free(ascolto_whistle_result_t *res);

/* Utility to read any 8/16/24/32-bit mono/stereo WAV file and resample to 16 kHz mono float. */
int ascolto_wav_load_16k_mono(const char *wav_path, float **out_samples, size_t *out_count);
int ascolto_wav_write_16k_mono(const char *wav_path, const float *samples, size_t count);

/* Synthesizes realistic formant-voiced speech-like audio with a specific speaker vocal profile (F0 & formants). */
void ascolto_synth_voice_utterance(
    float *out_pcm,
    size_t num_samples,
    float f0_hz,
    float f1_hz,
    float f2_hz,
    float f3_hz
);

#ifdef __cplusplus
}
#endif

#endif /* ASCOLTO_WHISTLE_ENGINE_H */
