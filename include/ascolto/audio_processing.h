#ifndef ASCOLTO_AUDIO_PROCESSING_H
#define ASCOLTO_AUDIO_PROCESSING_H
#include "ascolto/audio_types.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
float ascolto_audio_rms_db(const float *samples, int count);

/* Transport bounds, not speech decisions. Every sample reaches the model. */
#define ASCOLTO_TRANSCRIPTION_CHUNK_SECONDS 30
typedef struct {
    ascolto_channel_t channel;
    float *buffer;
    int capacity;
    int write_pos;
    int64_t total_samples_processed;
} ascolto_audio_chunker_t;
typedef void (*ascolto_audio_chunk_cb_t)(ascolto_channel_t channel,
    const float *pcm, int count, double start_s, double end_s, float rms_db, void *userdata);
int ascolto_audio_chunker_init(ascolto_audio_chunker_t *, ascolto_channel_t);
void ascolto_audio_chunker_reset(ascolto_audio_chunker_t *);
void ascolto_audio_chunker_free(ascolto_audio_chunker_t *);
void ascolto_audio_chunker_feed(ascolto_audio_chunker_t *, const float *, int,
    ascolto_audio_chunk_cb_t, void *);
void ascolto_audio_chunker_flush(ascolto_audio_chunker_t *, ascolto_audio_chunk_cb_t, void *);

/* Offline acoustic inspection only; never used to select transcription input. */
typedef struct {
    float energy_threshold_db;
    double min_speech_s, min_silence_s, max_segment_s, pad_s;
} ascolto_vad_config_t;
ascolto_vad_config_t ascolto_vad_default_config(void);
#ifdef __cplusplus
}
#endif
#endif
