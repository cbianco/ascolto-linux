#define _POSIX_C_SOURCE 200809L
#include "ascolto/audio_queue.h"
#include "ascolto/audio_processing.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool entered;
    bool release;
    int failures;
    int64_t delivered;
    int64_t silence;
} context_t;

static void receive(void *argument, const float *mic, const float *sys, int count, int64_t start) {
    context_t *context = argument;
    pthread_mutex_lock(&context->mutex);
    context->entered = true;
    pthread_cond_broadcast(&context->condition);
    while (!context->release) pthread_cond_wait(&context->condition, &context->mutex);
    pthread_mutex_unlock(&context->mutex);
    if (start != context->delivered) context->failures++;
    for (int i = 0; i < count; i++) {
        if (mic[i] != sys[i] || (mic[i] && mic[i] != start + i + 1)) context->failures++;
        if (!mic[i]) context->silence++;
    }
    context->delivered += count;
}

static int verify_consumer(void) {
    context_t context = {0};
    pthread_mutex_init(&context.mutex, NULL);
    pthread_cond_init(&context.condition, NULL);
    ascolto_audio_queue_t *queue = ascolto_audio_queue_start(6400, receive, &context);
    if (!queue) return 1;
    float pcm[3200];
    for (int i = 0; i < 320; i++) pcm[i] = i + 1;
    ascolto_audio_queue_push(queue, ASCOLTO_CHANNEL_MIC, pcm, 320, 0);
    ascolto_audio_queue_push(queue, ASCOLTO_CHANNEL_SYS, pcm, 320, 0);
    pthread_mutex_lock(&context.mutex);
    while (!context.entered) pthread_cond_wait(&context.condition, &context.mutex);
    pthread_mutex_unlock(&context.mutex);
    /* Block ASR while producers overrun the queue, then include a partial tail. */
    int64_t start = 320;
    for (int chunk = 0; chunk < 20; chunk++) {
        for (int i = 0; i < 3200; i++) pcm[i] = (float)(start + i + 1);
        ascolto_audio_queue_push(queue, ASCOLTO_CHANNEL_SYS, pcm, 3200, start);
        ascolto_audio_queue_push(queue, ASCOLTO_CHANNEL_MIC, pcm, 3200, start);
        start += 3200;
    }
    for (int i = 0; i < 13; i++) pcm[i] = (float)(start + i + 1);
    ascolto_audio_queue_push(queue, ASCOLTO_CHANNEL_SYS, pcm, 13, start);
    ascolto_audio_queue_push(queue, ASCOLTO_CHANNEL_MIC, pcm, 13, start);
    pthread_mutex_lock(&context.mutex);
    context.release = true;
    pthread_cond_broadcast(&context.condition);
    pthread_mutex_unlock(&context.mutex);
    ascolto_audio_queue_finish(queue);
    if (context.delivered != start + 13 || context.silence != ascolto_audio_queue_dropped(queue) || !context.silence)
        context.failures++;
    ascolto_audio_queue_free(queue);
    pthread_cond_destroy(&context.condition);
    pthread_mutex_destroy(&context.mutex);
    return context.failures;
}

typedef struct {
    const float *source;
    int delivered, chunks, failures;
} chunks_t;

static void receive_chunk(ascolto_channel_t channel, const float *pcm, int count,
    double start, double end, float db, void *context) {
    (void)channel; (void)db;
    chunks_t *state = context;
    if (llround(start * ASCOLTO_SAMPLE_RATE) != state->delivered ||
        llround((end - start) * ASCOLTO_SAMPLE_RATE) != count ||
        count > ASCOLTO_TRANSCRIPTION_CHUNK_SECONDS * ASCOLTO_SAMPLE_RATE ||
        memcmp(pcm, state->source + state->delivered, (size_t)count * sizeof(float))) state->failures++;
    state->delivered += count;
    state->chunks++;
}

static int verify_complete_model_input(void) {
    int count = ASCOLTO_SAMPLE_RATE * 60 + 13;
    float *pcm = calloc((size_t)count, sizeof(float));
    if (!pcm) return 1;
    /* Quiet attacks, brief replies, internal silence and the 13-sample tail
     * must survive byte-for-byte; input callback boundaries must not matter. */
    for (int i = 0; i < count; i++)
        pcm[i] = i % 8000 < 3000 ? .001f * sinf((float)i / 10) : 0;
    int failures = 0;
    for (int ch = ASCOLTO_CHANNEL_MIC; ch <= ASCOLTO_CHANNEL_SYS; ch++) {
        ascolto_audio_chunker_t chunker;
        if (ascolto_audio_chunker_init(&chunker, (ascolto_channel_t)ch)) { failures++; continue; }
        chunks_t state = {.source = pcm};
        int sizes[] = {1, 13, 319, 641, 48001};
        for (int pos = 0, iteration = 0; pos < count; iteration++) {
            int n = sizes[iteration % 5];
            if (n > count - pos) n = count - pos;
            ascolto_audio_chunker_feed(&chunker, pcm + pos, n, receive_chunk, &state);
            pos += n;
        }
        ascolto_audio_chunker_flush(&chunker, receive_chunk, &state);
        ascolto_audio_chunker_flush(&chunker, receive_chunk, &state);
        if (state.delivered != count || state.chunks != 3 ||
            chunker.total_samples_processed != count) failures++;
        failures += state.failures;
        ascolto_audio_chunker_reset(&chunker);
        state = (chunks_t){.source = pcm};
        ascolto_audio_chunker_feed(&chunker, pcm, 13, receive_chunk, &state);
        ascolto_audio_chunker_flush(&chunker, receive_chunk, &state);
        if (state.delivered != 13 || state.chunks != 1 || state.failures) failures++;
        ascolto_audio_chunker_free(&chunker);
    }
    free(pcm);
    return failures;
}

int main(void) {
    int failures = ascolto_capture_buffer_verify() + verify_consumer() + verify_complete_model_input();
    printf("Portable queue and complete model input: %d failures\n", failures);
    return failures ? 1 : 0;
}
