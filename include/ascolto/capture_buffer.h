#ifndef ASCOLTO_CAPTURE_BUFFER_H
#define ASCOLTO_CAPTURE_BUFFER_H
#include "ascolto/audio_types.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Timestamped dual-channel queue. Callers provide synchronization around these operations. */
typedef struct ascolto_capture_buffer_s ascolto_capture_buffer_t;
ascolto_capture_buffer_t* ascolto_capture_buffer_create(int capacity);
void ascolto_capture_buffer_free(ascolto_capture_buffer_t* buffer);
void ascolto_capture_buffer_reset(ascolto_capture_buffer_t* buffer);
void ascolto_capture_buffer_push(ascolto_capture_buffer_t* buffer, ascolto_channel_t channel,
                                 const float* samples, int count, int64_t start_sample);
int ascolto_capture_buffer_pop(ascolto_capture_buffer_t* buffer, float* mic, float* sys,
                               int count, bool pad_missing);
int ascolto_capture_buffer_available(const ascolto_capture_buffer_t* buffer, ascolto_channel_t channel);
int64_t ascolto_capture_buffer_dropped(const ascolto_capture_buffer_t* buffer);
int64_t ascolto_capture_buffer_padded(const ascolto_capture_buffer_t* buffer, ascolto_channel_t channel);
/* Deterministic verifier linked by test targets only. */
int ascolto_capture_buffer_verify(void);

#ifdef __cplusplus
}
#endif
#endif
