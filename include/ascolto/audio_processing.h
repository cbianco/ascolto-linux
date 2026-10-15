#ifndef ASCOLTO_AUDIO_PROCESSING_H
#define ASCOLTO_AUDIO_PROCESSING_H
#include "ascolto/audio_types.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif
#define ASCOLTO_SYS_ENV_HISTORY 128
float ascolto_audio_rms_db(const float *samples, int count);
typedef struct {
    float energy_threshold_db;  /* Default: -42.0 dB */
    double min_speech_s;        /* Default: 0.25 s */
    double min_silence_s;       /* Default: 0.35 s */
    double max_segment_s;       /* Default: 25.0 s */
    double pad_s;               /* Default: 0.12 s */
} ascolto_vad_config_t;

ascolto_vad_config_t ascolto_vad_default_config(void);

/* Online streaming VAD per channel (accumulates live chunks and flushes completed turns) */
typedef struct {
    ascolto_channel_t channel;
    ascolto_vad_config_t cfg;
    float* buffer;
    int capacity;
    int write_pos;
    bool in_speech;
    int last_voiced_sample;
    int64_t total_samples_processed;
} ascolto_online_vad_t;

int ascolto_online_vad_init(ascolto_online_vad_t* v, ascolto_channel_t channel, const ascolto_vad_config_t* cfg);
void ascolto_online_vad_reset(ascolto_online_vad_t* v);
void ascolto_online_vad_free(ascolto_online_vad_t* v);

typedef void (*ascolto_vad_turn_cb_t)(
    ascolto_channel_t channel,
    const float* pcm,
    int num_samples,
    double start_s,
    double end_s,
    float rms_db,
    void* userdata
);

/* Feeds 16kHz mono float samples into online VAD; invokes cb whenever a speech turn completes */
void ascolto_online_vad_feed(
    ascolto_online_vad_t* v,
    const float* samples,
    int num_samples,
    ascolto_vad_turn_cb_t cb,
    void* userdata
);

/* Flushes any remaining speech turn at end of call */
void ascolto_online_vad_flush(
    ascolto_online_vad_t* v,
    ascolto_vad_turn_cb_t cb,
    void* userdata
);


/* Shared per-session echo state; each caller serializes access with audio processing. */
typedef struct {
    int frames;
    int sys_frames;
    float sys_peak_db;
} ascolto_mic_activity_t;

typedef struct {
    double start_s, end_s;
    float rms_db;
    char text[512];
} ascolto_echo_turn_t;

#define ASCOLTO_RECENT_SYS_TURNS 8
typedef struct {
    float sys_db[ASCOLTO_SYS_ENV_HISTORY];
    int64_t sys_frames;
    ascolto_mic_activity_t mic;
    ascolto_echo_turn_t turns[ASCOLTO_RECENT_SYS_TURNS];
    unsigned turn_count;
} ascolto_echo_filter_t;

void ascolto_echo_filter_reset(ascolto_echo_filter_t *filter);
void ascolto_mic_activity_track(ascolto_mic_activity_t *activity, bool in_speech,
                                bool sys_active, float sys_db);
ascolto_mic_activity_t ascolto_mic_activity_take(ascolto_mic_activity_t *activity);
void ascolto_echo_filter_remember(ascolto_echo_filter_t *filter, const char *text,
                                  double start, double end, float rms_db);
bool ascolto_echo_filter_matches(const ascolto_echo_filter_t *filter, const char *text,
    double start, double end, float rms_db, const ascolto_mic_activity_t *activity);

/* Reference only recent SYS speech on the same sample timeline. */
bool ascolto_bleed_reference(const float *history, int64_t frames, int64_t mic_end,
                             int64_t sys_end, float *reference_db);
bool ascolto_is_lexical_echo(const char *mic_text, const char *sys_text);
bool ascolto_transcript_is_noise(const char *text, int word_count, float average_probability,
                                 ascolto_channel_t channel, float rms_db, bool recent_sys);
bool ascolto_echo_turn_matches(const char *mic_text, double start, double end, float rms_db,
                               float overlap_ratio, bool local_interruption, const char *sys_text,
                               double sys_start, double sys_end, float sys_db);
#ifdef __cplusplus
}
#endif
#endif
