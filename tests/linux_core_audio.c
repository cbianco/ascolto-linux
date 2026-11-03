#define _POSIX_C_SOURCE 200809L
#include "ascolto/core.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* Exercise the Linux consumer on either host without capture, model weights or
 * platform services. Recognition and output sinks are instrumented boundaries;
 * the core, request assembler and speaker identification are production code. */
static const float *expected;
static int delivered, requests, writes, embeddings;
static bool fail_model, empty_model;

int ascolto_local_stt_transcribe(ascolto_local_models_t *models, ascolto_local_stt_t *session,
    const float *pcm, int count, const char *language, const char *keywords, ascolto_stt_text_t *out) {
    (void)models; (void)session; (void)language; (void)keywords;
    assert(count > 0 && count <= ASCOLTO_TRANSCRIPTION_CHUNK_SECONDS * ASCOLTO_SAMPLE_RATE);
    assert(!memcmp(pcm, expected + delivered, (size_t)count * sizeof(float)));
    delivered += count;
    requests++;
    if (fail_model) return -1;
    memset(out, 0, sizeof(*out));
    if (!empty_model) strcpy(out->text, "Thanks. Thanks. Thanks. Thanks. Thanks.");
    return 0;
}
int ascolto_whistle_embed_segment(ascolto_whistle_engine_t *engine, const float *pcm,
    size_t count, ascolto_whistle_result_t *out) {
    (void)engine; (void)pcm; (void)count;
    memset(out, 0, sizeof(*out)); embeddings++; return 0;
}
int ascolto_whistle_process_segment(ascolto_whistle_engine_t *engine, const float *pcm,
    size_t count, const char *language, const char *keywords, const char *hint, ascolto_whistle_result_t *out) {
    (void)engine; (void)hint;
    ascolto_stt_text_t text = {0};
    memset(out, 0, sizeof(*out));
    int rc = ascolto_local_stt_transcribe(NULL, NULL, pcm, (int)count, language, keywords, &text);
    strcpy(out->text, text.text);
    return rc;
}
void ascolto_whistle_result_free(ascolto_whistle_result_t *out) { free(out->frame_embeddings); }
int ascolto_storage_write(const char *config, ascolto_storage_writer_t *writer, const ascolto_db_utterance_t *turn) {
    (void)config; (void)writer;
    assert(!strcmp(turn->text, "Thanks. Thanks. Thanks. Thanks. Thanks."));
    writes++; return 0;
}
void ascolto_obs_emit(ascolto_obs_t *obs, const char *event, const char *fields) {
    (void)obs; (void)event; (void)fields;
}
int ascolto_audio_recorder_write(ascolto_audio_recorder_t *recorder, const float *pcm, size_t count) {
    (void)recorder; (void)pcm; (void)count; abort();
}

int main(void) {
    ascolto_core_t *core = calloc(1, sizeof(*core));
    assert(core);
    pthread_mutex_init(&core->mutex, NULL);
    pthread_mutex_init(&core->processing_mutex, NULL);
    core->state = ASCOLTO_STATE_RECORDING_MANUAL;
    int count = ASCOLTO_TRANSCRIPTION_CHUNK_SECONDS * ASCOLTO_SAMPLE_RATE + 13;
    float *pcm = calloc((size_t)count, sizeof(float));
    assert(pcm);
    for (int i = 0; i < count; i++) pcm[i] = (i % 321) * .000001f;
    expected = pcm;
    for (int ch = 0; ch < 2; ch++) {
        assert(!ascolto_audio_chunker_init(&core->chunks[ch], (ascolto_channel_t)ch));
        for (int optional = 0; optional < 2; optional++) {
            core->local_stt.context = optional ? (void *)1 : NULL;
            for (int scenario = 0; scenario < 3; scenario++) {
                fail_model = scenario == 2; empty_model = scenario == 1;
                core->transcription_error = false;
                requests = delivered = writes = embeddings = 0;
                ascolto_audio_chunker_reset(&core->chunks[ch]);
                /* A full request plus a sub-millisecond final tail. */
                ascolto_core_ingest_audio(core, (ascolto_channel_t)ch, pcm, (size_t)count, 0, NULL);
                ascolto_core_flush_channel(core, (ascolto_channel_t)ch);
                assert(requests == (fail_model ? 1 : 2));
                assert(delivered == (fail_model ? count - 13 : count));
                assert(writes == (fail_model || empty_model ? 0 : 2));
                assert(core->transcription_error == fail_model);
                if (optional) assert(embeddings == (fail_model || empty_model ? 0 : 2));
            }
        }
        ascolto_audio_chunker_free(&core->chunks[ch]);
    }
    free(pcm);
    pthread_mutex_destroy(&core->processing_mutex);
    pthread_mutex_destroy(&core->mutex);
    free(core);
    puts("Linux core: exact PCM, short tails, model text, empty responses and errors passed");
    return 0;
}
