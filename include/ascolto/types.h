#ifndef ASCOLTO_TYPES_H
#define ASCOLTO_TYPES_H

#include "ascolto/audio_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASCOLTO_MAX_SPEAKERS       32
#define ASCOLTO_EMBED_DIM          128
#define ASCOLTO_MAX_DEVICES        32
#define ASCOLTO_MAX_RECENT_FILES   10
#define ASCOLTO_MAX_SEGMENTS_MEM   512

typedef enum {
    ASCOLTO_STATE_IDLE = 0,
    ASCOLTO_STATE_RECORDING_AUTO = 1,
    ASCOLTO_STATE_RECORDING_MANUAL = 2,
    ASCOLTO_STATE_PAUSED = 3,
    ASCOLTO_STATE_STOPPING = 4
} ascolto_state_t;



typedef enum {
    ASCOLTO_SENSITIVITY_LOW = 0,
    ASCOLTO_SENSITIVITY_MEDIUM = 1,
    ASCOLTO_SENSITIVITY_HIGH = 2
} ascolto_sensitivity_t;

typedef struct {
    char id[128];
    char name[128];
    char description[256];
    bool is_monitor;
    bool is_default;
} ascolto_device_info_t;

typedef struct {
    bool auto_mic_trigger;
    bool save_audio;
    bool cloud_recap;
    bool recap_tts;
    char language[16];
    char recap_lang[8];
    char tts_lang[8];
    char mic_device[128];
    char sys_device[128];
    ascolto_sensitivity_t diarization_sensitivity;
    float custom_threshold;
    char keywords[1024];
    char output_dir[512];
    char config_path[512];
    char weights_path[512];
    char control_socket[512];
    char telemetry_log[512];
    int mic_debounce_ms;
    bool headless;
} ascolto_config_t;

typedef struct {
    uint64_t seq_id;
    ascolto_channel_t channel;
    double start_sec;
    double end_sec;
    char speaker_label[32];
    int speaker_id;
    float speaker_similarity;
    char text[2048];
    char language[16];
    double ttft_ms;
    double decode_tps;
    float rms_db;
    float pitch_hz;
} ascolto_segment_t;

const char *ascolto_state_to_string(ascolto_state_t state);
const char *ascolto_channel_to_string(ascolto_channel_t channel);
const char *ascolto_sensitivity_to_string(ascolto_sensitivity_t sens);
ascolto_sensitivity_t ascolto_sensitivity_from_string(const char *str);
float ascolto_sensitivity_threshold(ascolto_sensitivity_t sens, float custom_override);

void ascolto_config_init_defaults(ascolto_config_t *cfg);
int ascolto_config_load(ascolto_config_t *cfg, const char *path);
int ascolto_config_save(const ascolto_config_t *cfg, const char *path);

#ifdef __cplusplus
}
#endif

#endif /* ASCOLTO_TYPES_H */
