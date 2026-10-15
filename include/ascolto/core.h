#ifndef ASCOLTO_CORE_H
#define ASCOLTO_CORE_H

#include "ascolto/types.h"
#include "ascolto/storage.h"
#include "ascolto/audio_processing.h"
#include "ascolto/diarizer.h"
#include "ascolto/whistle_engine.h"
#include "ascolto/audio_backend.h"
#include "ascolto/audio_recorder.h"
#include "ascolto/tray_ui.h"
#include "ascolto/observability.h"
#include <pthread.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ascolto_core {
    ascolto_config_t config;
    ascolto_state_t state;
    ascolto_state_t pre_pause_state;
    bool running;
    bool external_mic_active;
    char active_mic_app[128];
    uint64_t last_mic_change_ms;

    uint64_t session_id;
    int64_t database_session;
    ascolto_storage_writer_t storage_writer;
    int storage_error;
    uint64_t session_start_ms;
    uint64_t session_paused_accum_ms;
    uint64_t pause_start_ms;
    double session_audio_sec[2];

    char current_session_dir[512];
    char current_md_path[512];
    char current_jsonl_path[512];
    char current_audio_path[2][512];
    ascolto_audio_recorder_t *recorder[2];
    FILE *md_fp;
    FILE *jsonl_fp;

    size_t recent_files_count;
    char recent_files[ASCOLTO_MAX_RECENT_FILES][512];

    uint64_t total_segments;
    ascolto_segment_t segments[ASCOLTO_MAX_SEGMENTS_MEM];
    size_t segments_count;

    ascolto_online_vad_t vad[2];
    char vad_hint[2][512];
    ascolto_echo_filter_t echo;
    int64_t last_sys_voiced_sample;

    ascolto_whistle_engine_t whistle;
    ascolto_diarizer_t diarizer;
    ascolto_audio_backend_t audio;
    ascolto_tray_ui_t tray;
    ascolto_obs_t obs;

    pthread_mutex_t processing_mutex;
    pthread_mutex_t config_mutex;
    pthread_mutex_t mutex;
    pthread_cond_t session_condition;
    bool session_starting;
    bool drain_recording;
};

int ascolto_core_init(ascolto_core_t *core, const ascolto_config_t *cfg, bool use_mock_audio);
void ascolto_core_destroy(ascolto_core_t *core);

/* Session & state transitions */
int ascolto_core_start_manual(ascolto_core_t *core);
int ascolto_core_stop(ascolto_core_t *core);
int ascolto_core_pause(ascolto_core_t *core);
int ascolto_core_resume(ascolto_core_t *core);

/* Configuration mutations at runtime (from tray menu or control socket) */
int ascolto_core_set_auto_mic(ascolto_core_t *core, bool enabled);
int ascolto_core_set_save_audio(ascolto_core_t *core, bool enabled);
int ascolto_core_set_cloud_recap(ascolto_core_t *core, bool enabled);
int ascolto_core_set_recap_tts(ascolto_core_t *core, bool enabled);
/* Spawns scripts/recap.py detached for a session; md_path may be NULL for the last session. */
int ascolto_core_run_recap(ascolto_core_t *core, const char *md_path);
/* Renders recap.spoken.txt of a session to recap.mp3 (no STT, no Claude call). */
int ascolto_core_make_mp3(ascolto_core_t *core, const char *md_path);
int ascolto_core_set_recap_lang(ascolto_core_t *core, const char *lang);
int ascolto_core_set_tts_lang(ascolto_core_t *core, const char *lang);
int ascolto_core_set_language(ascolto_core_t *core, const char *lang);
int ascolto_core_set_output_directory(ascolto_core_t *core, const char *path);
int ascolto_core_set_mic_device(ascolto_core_t *core, const char *dev);
int ascolto_core_set_sys_device(ascolto_core_t *core, const char *dev);
int ascolto_core_set_sensitivity(ascolto_core_t *core, ascolto_sensitivity_t sens);
int ascolto_core_set_keywords(ascolto_core_t *core, const char *keywords);

/* Trigger & audio ingestion */
void ascolto_core_notify_mic_trigger(ascolto_core_t *core, bool active, const char *app_name);
void ascolto_core_ingest_audio(
    ascolto_core_t *core,
    ascolto_channel_t channel,
    const float *pcm_16k,
    size_t num_samples,
    double stream_time_sec,
    const char *hint_text
);
void ascolto_core_flush_channel(ascolto_core_t *core, ascolto_channel_t channel);

/* Periodic tick (updates timer, checks debounce, steps UI) */
void ascolto_core_tick(ascolto_core_t *core);
uint64_t ascolto_core_elapsed_sec(ascolto_core_t *core);
uint64_t ascolto_monotonic_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* ASCOLTO_CORE_H */
