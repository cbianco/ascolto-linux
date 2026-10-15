#include "ascolto.h"

#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t condition;
    bool entered;
    bool release;
    int calls;
    uint64_t generation;
    ascolto_live_app_t* app;
    _Atomic bool manual_done;
} controlled_drain_t;

/* Replace only the hardware drain in this binary. All session transitions and
   PCM/VAD processing are production code; no real microphone is opened. */
void ascolto_macos_capture_finish_session(ascolto_macos_capture_t* capture, uint64_t generation) {
    if (!capture) return;
    controlled_drain_t* drain = (controlled_drain_t*)capture;
    pthread_mutex_lock(&drain->lock);
    drain->calls++;
    drain->generation = generation;
    drain->entered = true;
    pthread_cond_broadcast(&drain->condition);
    while (!drain->release) pthread_cond_wait(&drain->condition, &drain->lock);
    pthread_mutex_unlock(&drain->lock);
}

static double now_s(void) {
    struct timespec time;
    clock_gettime(CLOCK_MONOTONIC, &time);
    return time.tv_sec + time.tv_nsec / 1e9;
}

static int init_app(ascolto_live_app_t* app, const char* directory, const char* name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.json", directory, name);
    ascolto_config_t config;
    ascolto_config_init_defaults(&config);
    config.auto_trigger_mic = false;
    config.save_audio = false;
    snprintf(config.output_dir, sizeof(config.output_dir), "%s", directory);
    if (ascolto_config_save(&config, path) != 0) return 1;
    return ascolto_live_app_init(app, NULL, path);
}

static void* hold_processing_lock(void* argument) {
    controlled_drain_t* context = argument;
    pthread_mutex_lock(&context->app->lock);
    pthread_mutex_lock(&context->lock);
    context->entered = true;
    pthread_cond_broadcast(&context->condition);
    pthread_mutex_unlock(&context->lock);
    usleep(1500000); /* Model a long ASR call holding the processing mutex. */
    pthread_mutex_unlock(&context->app->lock);
    return NULL;
}

static void wait_entered(controlled_drain_t* context) {
    pthread_mutex_lock(&context->lock);
    while (!context->entered) pthread_cond_wait(&context->condition, &context->lock);
    pthread_mutex_unlock(&context->lock);
}

static void init_context(controlled_drain_t* context, ascolto_live_app_t* app) {
    memset(context, 0, sizeof(*context));
    pthread_mutex_init(&context->lock, NULL);
    pthread_cond_init(&context->condition, NULL);
    context->app = app;
    atomic_init(&context->manual_done, false);
}

static void free_context(controlled_drain_t* context) {
    pthread_mutex_destroy(&context->lock);
    pthread_cond_destroy(&context->condition);
}

static int verify_ui_snapshot(const char* directory) {
    ascolto_live_app_t app;
    if (init_app(&app, directory, "ui-snapshot")) return 1;
    ascolto_live_app_start_session(&app, true);
    controlled_drain_t context;
    init_context(&context, &app);
    pthread_t holder;
    pthread_create(&holder, NULL, hold_processing_lock, &context);
    wait_entered(&context);
    double start = now_s();
    ascolto_live_snapshot_t snapshot;
    ascolto_live_app_read_snapshot(&app, &snapshot);
    char status[64];
    ascolto_live_app_format_statusbar(&app, status, sizeof(status));
    double elapsed_ms = (now_s() - start) * 1000.0;
    bool pass = snapshot.state == ASCOLTO_STATE_RECORDING && elapsed_ms < 100.0;
    pthread_join(holder, NULL);
    ascolto_live_app_free(&app);
    free_context(&context);
    printf("[%s] UI snapshot/status read during blocked ASR: %.2f ms\n", pass ? "PASS" : "FAIL", elapsed_ms);
    return pass ? 0 : 1;
}

static void* automatic_stop(void* argument) {
    controlled_drain_t* context = argument;
    ascolto_live_app_on_mic_hardware_change(context->app, false, "Test microphone");
    return NULL;
}

static void* manual_stop(void* argument) {
    controlled_drain_t* context = argument;
    ascolto_live_app_stop_session(context->app);
    context->manual_done = true;
    return NULL;
}

static int verify_committed_stop(const char* directory, bool pause) {
    ascolto_live_app_t app;
    if (init_app(&app, directory, pause ? "pause-drain" : "auto-drain")) return 1;
    controlled_drain_t context;
    init_context(&context, &app);
    pthread_mutex_lock(&app.lock);
    app.mic_hardware_active = false;
    app.capture = (ascolto_macos_capture_t*)&context;
    pthread_mutex_unlock(&app.lock);
    ascolto_live_app_set_auto_trigger(&app, true);
    ascolto_live_app_on_mic_hardware_change(&app, true, "Test microphone");
    pthread_t monitor, manual;
    pthread_create(&monitor, NULL, automatic_stop, &context);
    wait_entered(&context);
    ascolto_live_snapshot_t snapshot;
    ascolto_live_app_read_snapshot(&app, &snapshot);
    bool stopping = snapshot.state == ASCOLTO_STATE_STOPPING;
    int start_result = ascolto_live_app_start_session(&app, true);
    if (pause) ascolto_live_app_set_paused(&app, true);
    else ascolto_live_app_set_auto_trigger(&app, false);
    pthread_create(&manual, NULL, manual_stop, &context);
    usleep(20000);
    bool duplicate_waits = !context.manual_done;
    pthread_mutex_lock(&context.lock);
    context.release = true;
    pthread_cond_broadcast(&context.condition);
    pthread_mutex_unlock(&context.lock);
    pthread_join(monitor, NULL);
    pthread_join(manual, NULL);
    ascolto_live_app_read_snapshot(&app, &snapshot);
    bool pass = stopping && start_result == -2 && duplicate_waits && context.calls == 1 &&
                snapshot.state == (pause ? ASCOLTO_STATE_PAUSED : ASCOLTO_STATE_IDLE) &&
                app.session_generation == context.generation;
    pthread_mutex_lock(&app.lock);
    app.capture = NULL;
    pthread_mutex_unlock(&app.lock);
    ascolto_live_app_free(&app);
    free_context(&context);
    printf("[%s] Committed stop survives %s during drain; duplicate stop drains once\n",
           pass ? "PASS" : "FAIL", pause ? "pause" : "auto-trigger disabled");
    return pass ? 0 : 1;
}

typedef struct {
    int count;
    double starts[4];
    double ends[4];
} turns_t;

static void save_turn(ascolto_channel_t channel, const float* pcm, int count,
                      double start, double end, float rms, void* userdata) {
    (void)channel; (void)pcm; (void)count; (void)rms;
    turns_t* turns = userdata;
    if (turns->count < 4) {
        turns->starts[turns->count] = start;
        turns->ends[turns->count] = end;
    }
    turns->count++;
}

static void drain_pcm(ascolto_capture_buffer_t* buffer, ascolto_live_app_t* app,
                       ascolto_online_vad_t* vad, turns_t* turns) {
    float mic[320], sys[320];
    int count;
    while ((count = ascolto_capture_buffer_pop(buffer, mic, sys, 320, false)) > 0) {
        ascolto_live_app_feed_audio(app, ASCOLTO_CHANNEL_SYS, sys, count);
        ascolto_live_app_feed_audio(app, ASCOLTO_CHANNEL_MIC, mic, count);
        ascolto_online_vad_feed(vad, sys, count, save_turn, turns);
    }
}

static int verify_overflow_time(const char* directory) {
    ascolto_live_app_t app;
    if (init_app(&app, directory, "overflow")) return 1;
    ascolto_live_app_start_session(&app, true);
    app.mic_raw_file = tmpfile();
    app.sys_raw_file = tmpfile();
    ascolto_capture_buffer_t* buffer = ascolto_capture_buffer_create(6400);
    ascolto_vad_config_t config = ascolto_vad_default_config();
    config.min_speech_s = 0.30;
    config.min_silence_s = 0.32;
    ascolto_online_vad_t vad;
    ascolto_online_vad_init(&vad, ASCOLTO_CHANNEL_SYS, &config);
    turns_t turns = {0};
    float speech[3200], silence[3200] = {0};
    for (int i = 0; i < 3200; i++) speech[i] = 0.1f * sinf(2.0f * 3.14159265f * i / 80.0f);
    int64_t cursor = 0;
    for (int i = 0; i < 2; i++, cursor += 3200) {
        ascolto_capture_buffer_push(buffer, ASCOLTO_CHANNEL_SYS, speech, 3200, cursor);
        ascolto_capture_buffer_push(buffer, ASCOLTO_CHANNEL_MIC, silence, 3200, cursor);
        drain_pcm(buffer, &app, &vad, &turns);
    }
    /* Pause the consumer while 3s of speech enters a 0.4s queue: the 2.6s gap
       must close the first VAD turn and appear in both archived raw channels. */
    for (int i = 0; i < 15; i++, cursor += 3200) {
        ascolto_capture_buffer_push(buffer, ASCOLTO_CHANNEL_SYS, speech, 3200, cursor);
        ascolto_capture_buffer_push(buffer, ASCOLTO_CHANNEL_MIC, silence, 3200, cursor);
    }
    drain_pcm(buffer, &app, &vad, &turns);
    for (int i = 0; i < 2; i++, cursor += 3200) {
        ascolto_capture_buffer_push(buffer, ASCOLTO_CHANNEL_SYS, silence, 3200, cursor);
        ascolto_capture_buffer_push(buffer, ASCOLTO_CHANNEL_MIC, silence, 3200, cursor);
        drain_pcm(buffer, &app, &vad, &turns);
    }
    bool pass = ascolto_capture_buffer_dropped(buffer) == 41600 &&
                vad.total_samples_processed == cursor && app.vad_sys.total_samples_processed == cursor &&
                app.vad_mic.total_samples_processed == cursor && turns.count == 2 &&
                fabs(turns.starts[1] - 3.0) < 0.021 && turns.ends[0] < turns.starts[1] &&
                app.mic_raw_file && app.sys_raw_file &&
                ftell(app.mic_raw_file) == cursor * (int64_t)sizeof(float) &&
                ftell(app.sys_raw_file) == cursor * (int64_t)sizeof(float);
    float sample = 1;
    if (app.sys_raw_file) {
        fseek(app.sys_raw_file, 16000 * (long)sizeof(float), SEEK_SET);
        if (fread(&sample, sizeof(sample), 1, app.sys_raw_file) != 1 || sample != 0) pass = false;
        fseek(app.sys_raw_file, 0, SEEK_END);
    }
    printf("[%s] Overflow preserves %.2fs, raw stereo length and two separate VAD turns\n",
           pass ? "PASS" : "FAIL", (double)cursor / ASCOLTO_SAMPLE_RATE);
    ascolto_online_vad_free(&vad);
    ascolto_capture_buffer_free(buffer);
    ascolto_live_app_free(&app);
    return pass ? 0 : 1;
}

int main(void) {
    char directory[] = "/tmp/ascolto-regression.XXXXXX";
    if (!mkdtemp(directory)) return 1;
    int failures = verify_ui_snapshot(directory);
    failures += verify_committed_stop(directory, false);
    failures += verify_committed_stop(directory, true);
    failures += verify_overflow_time(directory);
    printf("Regression result: %d failed\n", failures);
    return failures ? 1 : 0;
}
