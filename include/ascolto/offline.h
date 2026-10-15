#ifndef ASCOLTO_OFFLINE_H
#define ASCOLTO_OFFLINE_H
#include "ascolto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ASCOLTO_MAX_SEGMENTS 256

typedef struct {
    float* samples;
    int num_samples;
    int sample_rate;
} ascolto_audio_buffer_t;

int ascolto_wav_load(const char* path, ascolto_audio_buffer_t* out_buf);
int ascolto_wav_save(const char* path, const float* samples, int num_samples, int sample_rate);
void ascolto_audio_free(ascolto_audio_buffer_t* buf);

/* Offline simulation, archive reprocessing and deterministic acoustic fixtures. */
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

/* Deterministic acoustic rooms exercising the production live pipeline. */
int ascolto_run_room_bench(ascolto_whistle_t* whistle, const char* output_dir);

#ifdef __cplusplus
}
#endif
#endif
