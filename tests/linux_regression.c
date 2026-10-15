#define _POSIX_C_SOURCE 200809L
#include "ascolto/core.h"
#include <libavformat/avformat.h>
#include <json-glib/json-glib.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool entered, release;
    ascolto_audio_chunk_cb original;
    void *original_context;
    ascolto_core_t *core;
    _Atomic int stopped;
} drain_t;

static void slow_consumer(void *argument, ascolto_channel_t channel, const float *pcm,
                          size_t count, double start, const char *hint) {
    drain_t *drain = argument;
    pthread_mutex_lock(&drain->mutex);
    drain->entered = true;
    pthread_cond_broadcast(&drain->condition);
    while (!drain->release) pthread_cond_wait(&drain->condition, &drain->mutex);
    pthread_mutex_unlock(&drain->mutex);
    drain->original(drain->original_context, channel, pcm, count, start, hint);
}

static void *stop(void *argument) {
    drain_t *drain = argument;
    ascolto_core_stop(drain->core);
    atomic_fetch_add(&drain->stopped, 1);
    return NULL;
}

static bool wait_stopping(ascolto_core_t *core) {
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&core->mutex);
        bool stopping = core->state == ASCOLTO_STATE_STOPPING;
        pthread_mutex_unlock(&core->mutex);
        if (stopping) return true;
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    return false;
}

int main(void) {
    char directory[] = "/tmp/ascolto-linux-regression-XXXXXX";
    if (!mkdtemp(directory)) return 1;
    ascolto_config_t config;
    ascolto_config_init_defaults(&config);
    config.headless = true;
    config.auto_mic_trigger = false;
    config.save_audio = true;
    snprintf(config.output_dir, sizeof(config.output_dir), "%s", directory);
    snprintf(config.config_path, sizeof(config.config_path), "%s/config.ini", directory);
    snprintf(config.control_socket, sizeof(config.control_socket), "%s/control.sock", directory);
    snprintf(config.weights_path, sizeof(config.weights_path), "%s/unused-model", directory);
    ascolto_core_t *core = calloc(1, sizeof(*core));
    if (!core || ascolto_core_init(core, &config, true)) return 1;
    drain_t drain = {.core = core, .original = core->audio.on_chunk, .original_context = core->audio.cb_user_data};
    pthread_mutex_init(&drain.mutex, NULL);
    pthread_cond_init(&drain.condition, NULL);
    core->audio.on_chunk = slow_consumer;
    core->audio.cb_user_data = &drain;
    ascolto_core_start_manual(core);
    float pcm[16013] = {0};
    ascolto_audio_backend_queue_pcm(&core->audio, ASCOLTO_CHANNEL_SYS, pcm, 320, 0);
    ascolto_audio_backend_queue_pcm(&core->audio, ASCOLTO_CHANNEL_MIC, pcm, 320, 0);
    pthread_mutex_lock(&drain.mutex);
    while (!drain.entered) pthread_cond_wait(&drain.condition, &drain.mutex);
    pthread_mutex_unlock(&drain.mutex);
    ascolto_audio_backend_queue_pcm(&core->audio, ASCOLTO_CHANNEL_SYS, pcm, 16013, 320);
    ascolto_audio_backend_queue_pcm(&core->audio, ASCOLTO_CHANNEL_MIC, pcm, 16013, 320);
    pthread_t first, duplicate;
    pthread_create(&first, NULL, stop, &drain);
    int failures = !wait_stopping(core);
    pthread_create(&duplicate, NULL, stop, &drain);
    ascolto_core_set_auto_mic(core, false);
    ascolto_core_pause(core);
    if (ascolto_core_start_manual(core) != -2 || atomic_load(&drain.stopped)) failures++;
    pthread_mutex_lock(&drain.mutex);
    drain.release = true;
    pthread_cond_broadcast(&drain.condition);
    pthread_mutex_unlock(&drain.mutex);
    pthread_join(first, NULL);
    pthread_join(duplicate, NULL);
    if (core->state != ASCOLTO_STATE_IDLE || atomic_load(&drain.stopped) != 2 || core->session_id != 1) failures++;
    for (int ch = 0; ch < 2; ch++) {
        if (core->vad[ch].total_samples_processed != 16333) failures++;
        AVFormatContext *format = NULL;
        if (avformat_open_input(&format, core->current_audio_path[ch], NULL, NULL) ||
            avformat_find_stream_info(format, NULL) < 0 || format->streams[0]->duration != 16333) failures++;
        avformat_close_input(&format);
    }
    core->audio.on_chunk = drain.original;
    core->audio.cb_user_data = drain.original_context;
    /* Explicit timestamp gaps also survive direct IPC-style ingestion, and text
       containing JSON delimiters remains readable by postprocessing. */
    ascolto_core_start_manual(core);
    for (int i = 0; i < 8000; i++) pcm[i] = 0.1f;
    const char *text = "The \"release\" is ready.\nNext task.";
    ascolto_core_ingest_audio(core, ASCOLTO_CHANNEL_MIC, pcm, 8000, 0.5, text);
    ascolto_core_stop(core);
    if (core->vad[ASCOLTO_CHANNEL_MIC].total_samples_processed != 16000) failures++;
    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_file(parser, core->current_jsonl_path, NULL)) failures++;
    else {
        JsonObject *object = json_node_get_object(json_parser_get_root(parser));
        if (!object || strcmp(json_object_get_string_member(object, "text"), text) ||
            json_object_get_double_member(object, "start_sec") != 0.5) failures++;
    }
    g_object_unref(parser);
    ascolto_core_destroy(core);
    free(core);
    pthread_cond_destroy(&drain.condition);
    pthread_mutex_destroy(&drain.mutex);
    printf("Linux drain/concurrency/raw-audio regressions: %d failures\n", failures);
    return failures ? 1 : 0;
}
