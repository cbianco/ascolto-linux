#define _POSIX_C_SOURCE 200809L
#include "ascolto/audio_queue.h"
#include "ascolto/audio_processing.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
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

typedef struct { int turns; double start[4]; } turns_t;

static void turn(ascolto_channel_t channel, const float *pcm, int count, double start,
                 double end, float db, void *argument) {
    (void)channel; (void)pcm; (void)count; (void)end; (void)db;
    turns_t *turns = argument;
    if (turns->turns < 4) turns->start[turns->turns] = start;
    turns->turns++;
}

static int verify_processing(void) {
    ascolto_vad_config_t config = ascolto_vad_default_config();
    config.energy_threshold_db = -35.5f;
    ascolto_online_vad_t vad;
    if (ascolto_online_vad_init(&vad, ASCOLTO_CHANNEL_MIC, &config)) return 1;
    float history[ASCOLTO_SYS_ENV_HISTORY] = {0}, mic[320], zeros[320] = {0};
    turns_t turns = {0};
    int failures = 0;
    for (int frame = 0; frame < 150; frame++) {
        history[frame % ASCOLTO_SYS_ENV_HISTORY] = frame < 25 ? -30 : -100;
        float db = frame < 10 ? -29 : frame < 25 ? -26 : frame >= 50 && frame < 75 ? -35 : -100;
        for (int i = 0; i < 320; i++) mic[i] = powf(10, db / 20);
        float reference;
        bool active = ascolto_bleed_reference(history, frame + 1, (frame + 1) * 320,
                                              (frame + 1) * 320, &reference);
        if (frame >= 37 && active) failures++;
        ascolto_online_vad_feed(&vad, active && db < reference + 3.5f ? zeros : mic, 320, turn, &turns);
    }
    ascolto_online_vad_flush(&vad, turn, &turns);
    if (turns.turns != 2 || fabs(turns.start[0] - 0.2) > 0.001 || fabs(turns.start[1] - 1.0) > 0.001)
        failures++;
    if (vad.total_samples_processed != 48000) failures++;
    if (!ascolto_transcript_is_noise("echo echo echo echo echo echo", 0, 1, ASCOLTO_CHANNEL_SYS, -20, false)) failures++;
    if (ascolto_transcript_is_noise("We will publish the release tomorrow.", 0, 1, ASCOLTO_CHANNEL_MIC, -35, false)) failures++;
    if (!ascolto_echo_turn_matches("Please publish the release", 0, 1, -28, 1, false,
                                  "Please publish the release", 0, 1, -30)) failures++;
    if (ascolto_echo_turn_matches("Please publish the release", 0, 1, -25, 1, true,
                                 "Please publish the release", 0, 1, -30)) failures++;
    ascolto_online_vad_free(&vad);
    return failures;
}

int main(void) {
    int failures = ascolto_capture_buffer_verify() + verify_consumer() + verify_processing();
    printf("Portable queue/VAD/echo regressions: %d failures\n", failures);
    return failures ? 1 : 0;
}
