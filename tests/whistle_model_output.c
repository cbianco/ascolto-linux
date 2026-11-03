#define _POSIX_C_SOURCE 200809L
#ifdef ASCOLTO_TEST_MACOS
#include "ascolto.h"
#else
#include "ascolto/whistle_engine.h"
#include "ascolto/audio_processing.h"
#endif
#include "needle.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char response[4096];
static bool model_error;
static int calls;
int needle_load(const unsigned char *data, unsigned long long n) { (void)data; (void)n; return 0; }
int needle_models(void) { return NEEDLE_SPEECH; }
int needle_embed(const char *input, const float *pcm, int n, float *out, int cap) {
    (void)input; (void)pcm; (void)n; (void)out; (void)cap; return 0;
}
int needle_transcribe(const float *pcm, int n, const char *lang, const char *keywords,
    int times, char *out, int cap) {
    (void)pcm; (void)n; (void)lang; (void)keywords; (void)times;
    calls++;
    if (model_error) return -1;
    snprintf(out, (size_t)cap, "{\"text\":\"%s\",\"language\":\"it\"}", response);
    return 0;
}
int needle_stream_transcribe_process(const float *pcm, int n, const char *lang,
    const char *keywords, char *out, int cap) {
    (void)pcm; (void)n; (void)lang; (void)keywords; (void)out; (void)cap; return -1;
}
int needle_stream_transcribe_stop(char *out, int cap) { (void)out; (void)cap; return -1; }

int main(void) {
    float pcm[320] = {0};
#ifdef ASCOLTO_TEST_MACOS
    ascolto_whistle_t engine = {.loaded = true};
    ascolto_transcript_result_t result;
#define RECOGNIZE(n) ascolto_whistle_transcribe(&engine, pcm, (n), "it", NULL, true, &result)
#else
    ascolto_whistle_engine_t engine = {.model_loaded = true};
    pthread_mutex_init(&engine.mutex, NULL);
    ascolto_whistle_result_t result;
#define RECOGNIZE(n) ascolto_whistle_process_segment(&engine, pcm, (n), "it", NULL, NULL, &result)
#endif
    strcpy(response, "Buongiorno.");
    assert(RECOGNIZE(320) == 0 && !strcmp(result.text, response));
    memset(response, 'x', 2050); response[2050] = 0;
    assert(RECOGNIZE(320) < 0); /* No silently truncated success. */
    model_error = true;
    assert(RECOGNIZE(320) < 0);
    model_error = false;
    response[0] = 0;
    assert(RECOGNIZE(320) == 0 && !result.text[0]);
#ifndef ASCOLTO_TEST_MACOS
    int previous = calls;
    assert(RECOGNIZE(ASCOLTO_TRANSCRIPTION_CHUNK_SECONDS * ASCOLTO_SAMPLE_RATE + 1) < 0);
    assert(calls == previous); /* No prefix-only inference on oversized input. */
    pthread_mutex_destroy(&engine.mutex);
#endif
    puts("Whistle model output: complete text or explicit failure");
    return 0;
}
