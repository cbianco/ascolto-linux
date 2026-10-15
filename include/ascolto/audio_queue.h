#ifndef ASCOLTO_AUDIO_QUEUE_H
#define ASCOLTO_AUDIO_QUEUE_H
#include "ascolto/capture_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ascolto_audio_queue ascolto_audio_queue_t;
typedef void (*ascolto_audio_pair_cb)(void *context, const float *mic, const float *sys,
                                     int count, int64_t start_sample);
/* The callback runs on one consumer thread, without holding the producer lock. */
ascolto_audio_queue_t *ascolto_audio_queue_start(int capacity, ascolto_audio_pair_cb callback, void *context);
void ascolto_audio_queue_push(ascolto_audio_queue_t *queue, ascolto_channel_t channel,
                              const float *pcm, int count, int64_t start_sample);
/* Stop producers before calling finish. It drains every queued sample and joins. */
void ascolto_audio_queue_finish(ascolto_audio_queue_t *queue);
void ascolto_audio_queue_free(ascolto_audio_queue_t *queue);
int64_t ascolto_audio_queue_dropped(ascolto_audio_queue_t *queue);
#ifdef __cplusplus
}
#endif
#endif
