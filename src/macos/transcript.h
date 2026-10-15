#ifndef ASCOLTO_MACOS_TRANSCRIPT_H
#define ASCOLTO_MACOS_TRANSCRIPT_H
#include "ascolto.h"

/* The caller owns app->lock; these functions own transcript and raw-audio I/O. */
int ascolto_transcript_open(ascolto_live_app_t *app, bool is_manual);
void ascolto_transcript_write_turn(ascolto_live_app_t *app, ascolto_channel_t channel,
    double start_s, double end_s, float rms_db, const ascolto_voiceprint_t *vp,
    const ascolto_transcript_result_t *tr, const char *speaker, float sim, bool is_new);
void ascolto_transcript_close(ascolto_live_app_t *app, double elapsed);
#endif
