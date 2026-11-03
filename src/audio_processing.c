#include "ascolto/audio_processing.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

const char *ascolto_channel_name(ascolto_channel_t ch) {
    return ch == ASCOLTO_CHANNEL_MIC ? "MIC" : "SYS";
}

float ascolto_audio_rms_db(const float *samples, int count) {
    if (!samples || count <= 0) return -100.0f;
    double energy = 0;
    for (int i = 0; i < count; i++) energy += (double)samples[i] * samples[i];
    double rms = sqrt(energy / count);
    return rms < 1e-5 ? -100.0f : (float)(20 * log10(rms));
}

ascolto_vad_config_t ascolto_vad_default_config(void) {
    return (ascolto_vad_config_t){-42.0f, .25, .35, 25., .12};
}

int ascolto_audio_chunker_init(ascolto_audio_chunker_t *chunker, ascolto_channel_t channel) {
    if (!chunker) return -1;
    memset(chunker, 0, sizeof(*chunker));
    chunker->channel = channel;
    chunker->capacity = ASCOLTO_SAMPLE_RATE * ASCOLTO_TRANSCRIPTION_CHUNK_SECONDS;
    chunker->buffer = calloc((size_t)chunker->capacity, sizeof(float));
    return chunker->buffer ? 0 : -1;
}

void ascolto_audio_chunker_reset(ascolto_audio_chunker_t *chunker) {
    if (!chunker) return;
    chunker->write_pos = 0;
    chunker->total_samples_processed = 0;
}

void ascolto_audio_chunker_free(ascolto_audio_chunker_t *chunker) {
    if (!chunker) return;
    free(chunker->buffer);
    chunker->buffer = NULL;
    chunker->capacity = chunker->write_pos = 0;
}

void ascolto_audio_chunker_flush(ascolto_audio_chunker_t *chunker,
    ascolto_audio_chunk_cb_t callback, void *context) {
    if (!chunker || !chunker->buffer || !chunker->write_pos) return;
    int count = chunker->write_pos;
    double end = (double)chunker->total_samples_processed / ASCOLTO_SAMPLE_RATE;
    if (callback) callback(chunker->channel, chunker->buffer, count,
        end - (double)count / ASCOLTO_SAMPLE_RATE, end,
        ascolto_audio_rms_db(chunker->buffer, count), context);
    chunker->write_pos = 0;
}

void ascolto_audio_chunker_feed(ascolto_audio_chunker_t *chunker,
    const float *pcm, int count, ascolto_audio_chunk_cb_t callback, void *context) {
    if (!chunker || !chunker->buffer || !pcm || count <= 0) return;
    while (count > 0) {
        int n = chunker->capacity - chunker->write_pos;
        if (n > count) n = count;
        memcpy(chunker->buffer + chunker->write_pos, pcm, (size_t)n * sizeof(float));
        chunker->write_pos += n;
        chunker->total_samples_processed += n;
        pcm += n;
        count -= n;
        if (chunker->write_pos == chunker->capacity)
            ascolto_audio_chunker_flush(chunker, callback, context);
    }
}
