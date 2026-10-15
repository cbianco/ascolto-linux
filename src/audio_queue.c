#define _POSIX_C_SOURCE 200809L
#include "ascolto/audio_queue.h"
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

struct ascolto_audio_queue {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    pthread_t thread;
    ascolto_capture_buffer_t *buffer;
    ascolto_audio_pair_cb callback;
    void *context;
    bool finishing;
    bool joined;
};

static double monotonic_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void *consume(void *argument) {
    ascolto_audio_queue_t *queue = argument;
    float mic[ASCOLTO_FRAME_SAMPLES], sys[ASCOLTO_FRAME_SAMPLES];
    int64_t delivered = 0;
    double missing_since = 0;
    for (;;) {
        pthread_mutex_lock(&queue->mutex);
        int count;
        for (;;) {
            int m = ascolto_capture_buffer_available(queue->buffer, ASCOLTO_CHANNEL_MIC);
            int s = ascolto_capture_buffer_available(queue->buffer, ASCOLTO_CHANNEL_SYS);
            if (queue->finishing && m == 0 && s == 0) {
                pthread_mutex_unlock(&queue->mutex);
                return NULL;
            }
            double now = monotonic_seconds();
            if ((m >= ASCOLTO_FRAME_SAMPLES && s >= ASCOLTO_FRAME_SAMPLES) || (!m && !s)) missing_since = 0;
            else if (!missing_since) missing_since = now;
            bool pad = queue->finishing || (missing_since && now - missing_since >= 0.30);
            count = ascolto_capture_buffer_pop(queue->buffer, mic, sys, ASCOLTO_FRAME_SAMPLES, pad);
            if (count) break;
            struct timespec until;
            clock_gettime(CLOCK_REALTIME, &until);
            until.tv_nsec += 50000000;
            if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
            pthread_cond_timedwait(&queue->condition, &queue->mutex, &until);
        }
        pthread_mutex_unlock(&queue->mutex);
        queue->callback(queue->context, mic, sys, count, delivered);
        delivered += count;
    }
}

ascolto_audio_queue_t *ascolto_audio_queue_start(int capacity, ascolto_audio_pair_cb callback, void *context) {
    if (!callback) return NULL;
    ascolto_audio_queue_t *queue = calloc(1, sizeof(*queue));
    if (!queue) return NULL;
    queue->buffer = ascolto_capture_buffer_create(capacity);
    if (!queue->buffer) { free(queue); return NULL; }
    pthread_mutex_init(&queue->mutex, NULL);
    pthread_cond_init(&queue->condition, NULL);
    queue->callback = callback;
    queue->context = context;
    if (pthread_create(&queue->thread, NULL, consume, queue)) {
        pthread_cond_destroy(&queue->condition);
        pthread_mutex_destroy(&queue->mutex);
        ascolto_capture_buffer_free(queue->buffer);
        free(queue);
        return NULL;
    }
    return queue;
}

void ascolto_audio_queue_push(ascolto_audio_queue_t *queue, ascolto_channel_t channel,
                              const float *pcm, int count, int64_t start_sample) {
    if (!queue) return;
    pthread_mutex_lock(&queue->mutex);
    if (!queue->finishing) {
        ascolto_capture_buffer_push(queue->buffer, channel, pcm, count, start_sample);
        pthread_cond_signal(&queue->condition);
    }
    pthread_mutex_unlock(&queue->mutex);
}

void ascolto_audio_queue_finish(ascolto_audio_queue_t *queue) {
    if (!queue || queue->joined) return;
    pthread_mutex_lock(&queue->mutex);
    queue->finishing = true;
    pthread_cond_signal(&queue->condition);
    pthread_mutex_unlock(&queue->mutex);
    pthread_join(queue->thread, NULL);
    queue->joined = true;
}

int64_t ascolto_audio_queue_dropped(ascolto_audio_queue_t *queue) {
    if (!queue) return 0;
    pthread_mutex_lock(&queue->mutex);
    int64_t dropped = ascolto_capture_buffer_dropped(queue->buffer);
    pthread_mutex_unlock(&queue->mutex);
    return dropped;
}

void ascolto_audio_queue_free(ascolto_audio_queue_t *queue) {
    if (!queue) return;
    ascolto_audio_queue_finish(queue);
    ascolto_capture_buffer_free(queue->buffer);
    pthread_cond_destroy(&queue->condition);
    pthread_mutex_destroy(&queue->mutex);
    free(queue);
}
