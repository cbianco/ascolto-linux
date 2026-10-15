#ifndef ASCOLTO_AUDIO_TYPES_H
#define ASCOLTO_AUDIO_TYPES_H
#include <stdint.h>
#define ASCOLTO_SAMPLE_RATE 16000
#define ASCOLTO_FRAME_SAMPLES 320
/* Both native backends deliver mono float PCM on this common sample clock. */
typedef enum {
    ASCOLTO_CHANNEL_MIC = 0,
    ASCOLTO_CHANNEL_SYS = 1
} ascolto_channel_t;
#ifdef __cplusplus
extern "C" {
#endif
const char *ascolto_channel_name(ascolto_channel_t channel);
#ifdef __cplusplus
}
#endif
#endif
