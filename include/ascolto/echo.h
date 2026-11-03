#ifndef ASCOLTO_ECHO_H
#define ASCOLTO_ECHO_H
#include "ascolto/audio_types.h"

typedef struct ascolto_echo ascolto_echo_t;

/* Post-capture processing only. Inputs and archives remain untouched. Feed
 * paired 20 ms frames, with a partial frame allowed only at the end. Output is
 * delayed by one frame internally; finish drains it without shifting timestamps
 * or losing the partial tail. A negative result reports misuse/failure. */
ascolto_echo_t *ascolto_echo_create(void);
void ascolto_echo_destroy(ascolto_echo_t *echo);
int ascolto_echo_process(ascolto_echo_t *echo, const float *mic, const float *sys,
                        int count, float output[ASCOLTO_FRAME_SAMPLES]);
int ascolto_echo_finish(ascolto_echo_t *echo, float output[ASCOLTO_FRAME_SAMPLES]);
#endif
