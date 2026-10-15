#ifndef ASCOLTO_H
#define ASCOLTO_H

#include "ascolto/audio_types.h"
#include "ascolto/audio_processing.h"
#include "ascolto/capture_buffer.h"
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASCOLTO_WHISTLE_EMBED_DIM 512
#define ASCOLTO_VOICEPRINT_DIM 16
#define ASCOLTO_MAX_WORDS 128
#define ASCOLTO_MAX_SPEAKERS 16
#define ASCOLTO_MAX_SEGMENTS 256
#define ASCOLTO_SYS_ENV_HISTORY 128
#define ASCOLTO_RECENT_SYS_UTTS 8

/* --------------------------------------------------------------------------
 * Audio Stream & Channel Types
 * -------------------------------------------------------------------------- */


const char* ascolto_channel_name(ascolto_channel_t ch);

/* --------------------------------------------------------------------------
 * WAV 16kHz Mono PCM Utilities
 * -------------------------------------------------------------------------- */
typedef struct {
    float* samples;
    int num_samples;
    int sample_rate;
} ascolto_audio_buffer_t;

int ascolto_wav_load(const char* path, ascolto_audio_buffer_t* out_buf);
int ascolto_wav_save(const char* path, const float* samples, int num_samples, int sample_rate);
void ascolto_audio_free(ascolto_audio_buffer_t* buf);
float ascolto_audio_rms_db(const float* samples, int num_samples);

/* --------------------------------------------------------------------------
 * Persistent Configuration (~/.config/ascolto/config.json)
 * -------------------------------------------------------------------------- */
typedef struct {
    bool auto_trigger_mic;          /* Automatically start/stop when mic is active */
    bool paused;                    /* User manually paused auto-trigger */
    bool cloud_recap;              /* Opt-in cloud audio transcription and recap */
    bool recap_tts;                /* Optional cloud MP3 rendering */
    char control_socket[512];      /* Private local CLI socket */
    char recap_python[512];        /* Interpreter with scripts/requirements-recap.txt installed */
    bool save_audio;                /* Save compressed stereo .m4a (L=MIC, R=SYS) for reprocessing */
    char language[16];              /* "auto", "it", "en", "de", "fr", "es", "nl", "pl" */
    uint32_t mic_device_id;         /* 0 = system default input device */
    uint32_t sys_device_id;         /* 0 = auto loopback/virtual device */
    float diarizer_threshold;       /* Default 0.60 (0.72 high, 0.60 balanced, 0.48 conservative) */
    char output_dir[512];           /* Default ~/Documents/Ascolto */
    char config_path[512];          /* Default ~/.config/ascolto/config.json */
    char keywords_path[512];        /* Default ~/.config/ascolto/keywords.txt */
    char model_path[512];           /* Default models/whistle.cact */
} ascolto_config_t;

void ascolto_config_init_defaults(ascolto_config_t* cfg);
int ascolto_config_load(ascolto_config_t* cfg, const char* path_override);
int ascolto_config_save(const ascolto_config_t* cfg, const char* path_override);
/* Reads keywords file into a newline-separated buffer suitable for Whistle */
int ascolto_config_load_keywords(const ascolto_config_t* cfg, char* out_buf, size_t out_cap);

/* --------------------------------------------------------------------------
 * Microphone Activity Monitor (CoreAudio + Simulated Backend)
 * -------------------------------------------------------------------------- */
typedef struct {
    uint32_t device_id;
    char device_name[128];
    double sample_rate;
    bool is_input;
    bool is_default_input;
    bool is_default_output;
    bool is_running_somewhere;
} ascolto_macos_device_info_t;

int ascolto_macos_list_devices(ascolto_macos_device_info_t* out_devices, int max_devices);
int ascolto_macos_default_mic_status(ascolto_macos_device_info_t* out_info);
int ascolto_macos_device_status(uint32_t device_id, ascolto_macos_device_info_t* out_info);

/* Diagnostic records contain timings and states only, and are written off-main. */
void ascolto_diagnostics_init(const char* config_path);
void ascolto_diagnostic(const char* format, ...) __attribute__((format(printf, 1, 2)));

/* Checks if any OTHER process on macOS (excluding our own getpid()) is actively using audio input (microphone).
   If active, writes the bundle ID (e.g. "com.google.Chrome", "com.microsoft.teams2") into out_bundle_id. */
bool ascolto_macos_external_app_using_mic(char* out_bundle_id, size_t bundle_cap, int* out_pid);

/* Queries all external processes currently using Audio Input (MIC) and Audio Output (SYS).
   Formats human-readable lists into out_in_apps and out_out_apps.
   Returns true if at least one external app is using the microphone (input). */
bool ascolto_macos_query_audio_apps(
    char* out_in_apps,
    size_t in_cap,
    char* out_out_apps,
    size_t out_cap
);

typedef struct {
    bool ax_trusted;                /* Accessibility permission granted */
    bool calendar_authorized;       /* EventKit Calendar permission granted */
    char window_title[256];         /* Active call window title from AXUIElement */
    char calendar_title[256];       /* Active meeting title from macOS Calendar */
    char calendar_organizer[128];   /* Organizer name */
    char calendar_attendees[512];   /* Comma-separated participant names */
    char calendar_notes[512];       /* Short description / agenda from Calendar event */
} ascolto_call_metadata_t;

void ascolto_macos_query_call_metadata(
    const char* active_app_name,
    ascolto_call_metadata_t* out_meta
);
void ascolto_macos_request_accessibility_permission(void);
void ascolto_macos_request_calendar_permission(void);

typedef void (*ascolto_mic_callback_t)(bool is_active, double timestamp_s, void* userdata);

typedef struct ascolto_mic_monitor_s ascolto_mic_monitor_t;

struct ascolto_mic_monitor_s {
    const char* backend_name;
    bool (*is_active)(ascolto_mic_monitor_t* self);
    void (*step)(ascolto_mic_monitor_t* self, double current_time_s);
    void (*destroy)(ascolto_mic_monitor_t* self);
    ascolto_mic_callback_t callback;
    void* userdata;
    void* impl;
};

ascolto_mic_monitor_t* ascolto_mic_monitor_create_simulated(
    double active_start_s,
    double active_end_s,
    ascolto_mic_callback_t cb,
    void* userdata
);

ascolto_mic_monitor_t* ascolto_mic_monitor_create_macos(
    ascolto_mic_callback_t cb,
    void* userdata
);

/* --------------------------------------------------------------------------
 * Voice Activity Detection (VAD) & Turn Segmentation
 * -------------------------------------------------------------------------- */
typedef struct {
    ascolto_channel_t channel;
    int start_sample;
    int end_sample;
    double start_s;
    double end_s;
    float rms_db;
} ascolto_vad_segment_t;

int ascolto_vad_segment_buffer(
    const float* samples,
    int num_samples,
    ascolto_channel_t channel,
    const ascolto_vad_config_t* cfg,
    ascolto_vad_segment_t* out_segments,
    int max_segments
);

/* --------------------------------------------------------------------------
 * Whistle (Cactus Compute Needle C Engine) Wrapper
 * -------------------------------------------------------------------------- */
typedef struct {
    char word[64];
    double start_s;
    double end_s;
    float probability;
} ascolto_word_t;

typedef struct {
    char text[2048];
    char language[16];
    float ttft_ms;
    float decode_tps;
    int num_words;
    ascolto_word_t words[ASCOLTO_MAX_WORDS];
    char raw_json[8192];
} ascolto_transcript_result_t;

typedef struct {
    unsigned char* model_bytes;
    size_t model_size;
    bool loaded;
} ascolto_whistle_t;

int ascolto_whistle_init(ascolto_whistle_t* w, const char* model_path);
void ascolto_whistle_free(ascolto_whistle_t* w);

int ascolto_whistle_transcribe(
    ascolto_whistle_t* w,
    const float* pcm,
    int num_samples,
    const char* language,
    const char* keywords,
    bool word_timestamps,
    ascolto_transcript_result_t* out_result
);

int ascolto_whistle_embed_frames(
    ascolto_whistle_t* w,
    const float* pcm,
    int num_samples,
    float** out_embeddings,
    int* out_frames
);

int ascolto_whistle_stream_push(
    ascolto_whistle_t* w,
    const float* pcm,
    int num_samples,
    const char* language,
    const char* keywords,
    char* out_json,
    int out_capacity
);

int ascolto_whistle_stream_finalize(
    ascolto_whistle_t* w,
    char* out_json,
    int out_capacity
);

/* --------------------------------------------------------------------------
 * Speaker Diarization (Me vs Others + Multi-Speaker Clustering on SYS)
 * -------------------------------------------------------------------------- */
typedef struct {
    int voiced_frames;
    float f0_mean_hz;
    float f0_std_hz;
    float spectral_centroid_hz;
    float spectral_rolloff_hz;
    float zcr;
    float band_energy[6];
    float whistle_proj[5];
    float feature_vec[ASCOLTO_VOICEPRINT_DIM];
} ascolto_voiceprint_t;

typedef struct {
    int speaker_id;
    char label[32];
    int segment_count;
    float centroid[ASCOLTO_VOICEPRINT_DIM];
    float mean_f0_hz;
    float mean_centroid_hz;
} ascolto_speaker_cluster_t;

typedef struct {
    float similarity_threshold;
    int num_speakers;
    ascolto_speaker_cluster_t speakers[ASCOLTO_MAX_SPEAKERS];
} ascolto_diarizer_t;

void ascolto_diarizer_init(ascolto_diarizer_t* d, float similarity_threshold);

void ascolto_extract_voiceprint(
    const float* pcm,
    int num_samples,
    const float* whistle_frames,
    int num_whistle_frames,
    ascolto_voiceprint_t* out_vp
);

float ascolto_voiceprint_similarity(
    const ascolto_voiceprint_t* a,
    const float* cluster_centroid
);

const char* ascolto_diarizer_identify(
    ascolto_diarizer_t* d,
    ascolto_channel_t channel,
    const ascolto_voiceprint_t* vp,
    float* out_best_similarity,
    bool* out_is_new_speaker
);

/* --------------------------------------------------------------------------
 * Structured Trace & Full Call Session Simulator
 * -------------------------------------------------------------------------- */
typedef struct {
    double start_s;
    double end_s;
    ascolto_channel_t channel;
    char speaker[32];
    float speaker_similarity;
    bool is_new_speaker;
    float f0_hz;
    float centroid_hz;
    float rms_db;
    ascolto_transcript_result_t transcript;
} ascolto_utterance_t;

typedef struct {
    int num_utterances;
    ascolto_utterance_t utterances[ASCOLTO_MAX_SEGMENTS];
    int detected_sys_speakers;
    double total_duration_s;
    double total_transcribe_ms;
} ascolto_session_report_t;

int ascolto_run_simulated_session(
    ascolto_whistle_t* whistle,
    const float* mic_pcm,
    int mic_samples,
    const float* sys_pcm,
    int sys_samples,
    const char* language,
    const char* keywords,
    float diarizer_threshold,
    FILE* trace_jsonl_out,
    ascolto_session_report_t* out_report
);

/* --------------------------------------------------------------------------
 * Online Streaming VAD & Live App Controller (Menu Bar + Background Worker)
 * -------------------------------------------------------------------------- */
typedef enum {
    ASCOLTO_STATE_IDLE = 0,      /* Waiting for microphone activation */
    ASCOLTO_STATE_RECORDING = 1, /* Actively capturing & transcribing call */
    ASCOLTO_STATE_PAUSED = 2,    /* Auto-trigger paused by user */
    ASCOLTO_STATE_STOPPING = 3   /* Stop committed; queued audio is still draining */
} ascolto_app_state_t;

const char* ascolto_app_state_label(ascolto_app_state_t st);

typedef struct {
    double start_s;
    double end_s;
    float rms_db;
    float f0_hz;
    ascolto_voiceprint_t vp;
    char text[512];
} ascolto_recent_sys_utt_t;

typedef struct ascolto_macos_capture_s ascolto_macos_capture_t;

/* UI-readable state. Its mutex is held only for copying, never for ASR or I/O. */
typedef struct {
    ascolto_config_t config;
    ascolto_app_state_t state;
    bool sys_audio_permission_ok;
    float last_mic_rms_db;
    float last_sys_rms_db;
    char active_mic_name[128];
    char active_in_apps[256];
    char active_out_apps[256];
    ascolto_call_metadata_t call_meta;
    char last_speaker[32];
    char last_utterance_text[256];
    char current_md_path[512];
    int utterances;
    int remote_speakers;
    double session_start_s;
    int64_t mic_samples;
    int64_t mic_padding;
} ascolto_live_snapshot_t;

/* Live Application Controller state shared between CoreAudio, Worker, and Menu Bar UI */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t session_cond;
    pthread_mutex_t snapshot_lock;
    ascolto_live_snapshot_t snapshot;
    ascolto_macos_capture_t* capture;
    uint64_t session_generation;
    ascolto_config_t config;
    ascolto_whistle_t* whistle;
    ascolto_diarizer_t diarizer;
    ascolto_online_vad_t vad_mic;
    ascolto_online_vad_t vad_sys;

    ascolto_app_state_t state;
    bool session_save_audio;
    bool manual_override_active;
    bool manual_stop_for_current_call;
    bool mic_hardware_active;
    bool sys_audio_permission_ok;
    float last_mic_rms_db;
    float last_sys_rms_db;
    double last_sys_voiced_time_s;
    double last_sys_voiced_stream_s;

    /* 20ms frame-level SYS envelope ring buffer & MIC bleed gate state */
    float sys_env_db[ASCOLTO_SYS_ENV_HISTORY];
    int64_t sys_env_frames;
    float pending_audio[2][320];
    int pending_audio_count[2];
    int mic_barge_in_streak;
    int mic_turn_total_frames;
    int mic_turn_sys_active_frames;
    float mic_turn_sys_peak_db;
    int mic_bleed_frames_suppressed;
    int mic_echo_turns_suppressed;
    int64_t capture_dropped_samples;
    int64_t capture_padded_samples[2];

    /* Recent SYS utterances for Stage-2 lexical & voiceprint echo verification */
    ascolto_recent_sys_utt_t recent_sys_utts[ASCOLTO_RECENT_SYS_UTTS];
    int recent_sys_utt_count;

    char active_mic_name[128];
    char active_call_app[128];
    char active_in_apps[256];
    char active_out_apps[256];
    ascolto_call_metadata_t call_meta;
    bool meta_written_to_md;

    double session_start_epoch_s;
    double session_duration_s;
    int session_utterance_count;
    char current_md_path[512];
    char current_jsonl_path[512];
    char current_mic_raw_path[512];
    char current_sys_raw_path[512];
    char current_m4a_path[512];
    char last_speaker[32];
    char last_utterance_text[256];

    FILE* md_file;
    FILE* jsonl_file;
    FILE* mic_raw_file;
    FILE* sys_raw_file;
} ascolto_live_app_t;

int ascolto_live_app_init(ascolto_live_app_t* app, ascolto_whistle_t* whistle, const char* config_path);
void ascolto_live_app_free(ascolto_live_app_t* app);

/* State & Menu Bar action handlers (thread-safe) */
int ascolto_live_app_start_session(ascolto_live_app_t* app, bool is_manual);
int ascolto_live_app_stop_session(ascolto_live_app_t* app);
void ascolto_live_app_on_mic_hardware_change(ascolto_live_app_t* app, bool mic_active, const char* mic_name);
void ascolto_live_app_set_paused(ascolto_live_app_t* app, bool paused);
void ascolto_live_app_set_auto_trigger(ascolto_live_app_t* app, bool enabled);
void ascolto_live_app_set_save_audio(ascolto_live_app_t* app, bool enabled);
void ascolto_live_app_set_cloud_recap(ascolto_live_app_t* app, bool enabled);
void ascolto_live_app_set_recap_tts(ascolto_live_app_t* app, bool enabled);
int ascolto_recap_start(const ascolto_config_t* config, const char* md_path, bool mp3_only, bool prepare_only);
void ascolto_archive_wait(void);
int ascolto_archive_finalize(const ascolto_config_t* config, const char* mic_raw, const char* sys_raw,
                            const char* archive, const char* md_path);

void ascolto_live_app_set_language(ascolto_live_app_t* app, const char* lang);
void ascolto_live_app_set_devices(ascolto_live_app_t* app, uint32_t mic_dev_id, uint32_t sys_dev_id);
void ascolto_live_app_set_diarizer_threshold(ascolto_live_app_t* app, float threshold);

/* Feed live 16kHz mono float audio chunks into the active session */
void ascolto_live_app_feed_audio(
    ascolto_live_app_t* app,
    ascolto_channel_t channel,
    const float* samples,
    int num_samples
);

/* Formats the current status bar title string (e.g. "🎙️ Ascolto" or "🔴 01:23" or "⏸️ Ascolto") */
void ascolto_live_app_format_statusbar(ascolto_live_app_t* app, char* out_buf, size_t out_cap);
void ascolto_live_app_read_snapshot(ascolto_live_app_t* app, ascolto_live_snapshot_t* snapshot);

/* Shared ASR hallucination / repetition filter and cross-channel lexical echo detector */
bool ascolto_is_noise_hallucination(
    const ascolto_transcript_result_t* tr,
    ascolto_channel_t channel,
    float rms_db,
    bool recent_sys_activity
);

bool ascolto_is_lexical_echo(const char* mic_text, const char* sys_text);

/* Deterministic acoustic rooms exercising the production live pipeline. */
int ascolto_run_room_bench(ascolto_whistle_t* whistle, const char* output_dir);

/* --------------------------------------------------------------------------
 * Real-Time macOS Audio Capture Engine (AudioQueue / CoreAudio)
 * -------------------------------------------------------------------------- */

ascolto_macos_capture_t* ascolto_macos_capture_start(ascolto_live_app_t* app);
void ascolto_macos_capture_stop(ascolto_macos_capture_t* cap);
void ascolto_macos_capture_finish_session(ascolto_macos_capture_t* cap, uint64_t generation);

/* --------------------------------------------------------------------------
 * macOS Menu Bar UI Entrypoint (AppKit NSStatusBar)
 * -------------------------------------------------------------------------- */
int ascolto_run_menubar_app(ascolto_live_app_t* app, ascolto_macos_capture_t** capture);

#ifdef __cplusplus
}
#endif

#endif /* ASCOLTO_H */
