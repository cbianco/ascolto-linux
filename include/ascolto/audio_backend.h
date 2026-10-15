#ifndef ASCOLTO_AUDIO_BACKEND_H
#define ASCOLTO_AUDIO_BACKEND_H

#include "ascolto/types.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*ascolto_audio_chunk_cb)(
    void *user_data,
    ascolto_channel_t channel,
    const float *pcm_16k,
    size_t num_samples,
    double stream_time_sec,
    const char *hint_text
);

typedef void (*ascolto_mic_trigger_cb)(
    void *user_data,
    bool mic_active,
    const char *client_app_name
);

typedef struct ascolto_audio_backend ascolto_audio_backend_t;

struct ascolto_audio_backend {
    void *impl;
    bool use_mock;
    ascolto_audio_chunk_cb on_chunk;
    ascolto_mic_trigger_cb on_mic_trigger;
    void *cb_user_data;

    int (*start_watcher)(ascolto_audio_backend_t *b);
    void (*stop_watcher)(ascolto_audio_backend_t *b);
    int (*start_capture)(ascolto_audio_backend_t *b, const char *mic_dev, const char *sys_dev);
    void (*stop_capture)(ascolto_audio_backend_t *b);
    int (*list_devices)(ascolto_audio_backend_t *b, ascolto_device_info_t *out_devs, size_t max_devs, size_t *out_count);
    void (*destroy)(ascolto_audio_backend_t *b);
};

/* Creates either the live PipeWire backend or the deterministic test-harness mock backend. */
int ascolto_audio_backend_create(
    ascolto_audio_backend_t *b,
    bool use_mock,
    ascolto_audio_chunk_cb on_chunk,
    ascolto_mic_trigger_cb on_mic_trigger,
    void *user_data
);

/* Deterministic injection into the same paired queue used by live PipeWire readers. */
int ascolto_audio_backend_queue_pcm(ascolto_audio_backend_t *backend, ascolto_channel_t channel,
                                   const float *pcm, int count, int64_t start_sample);

/* Direct inspection helper to check if any third-party app currently has an active PipeWire mic capture stream. */
bool ascolto_pipewire_check_mic_active(char *out_app_name, size_t app_name_cap);

/* Enumerates PipeWire / WirePlumber audio sources and monitor sources. */
int ascolto_pipewire_enumerate_devices(ascolto_device_info_t *out_devs, size_t max_devs, size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* ASCOLTO_AUDIO_BACKEND_H */
