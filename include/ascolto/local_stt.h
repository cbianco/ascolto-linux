#ifndef ASCOLTO_LOCAL_STT_H
#define ASCOLTO_LOCAL_STT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ASCOLTO_LOCAL_MODEL_COUNT 4
#define ASCOLTO_STT_ABI 1
/* Each adapter exports only this versioned C entry point. No ggml symbol crosses it. */
typedef struct { char text[2048]; char language[16]; } ascolto_stt_text_t;
typedef struct {
    unsigned abi;
    void *(*load)(const char *path, char *error, size_t cap);
    int (*transcribe)(void *, const float *, int, const char *, const char *, ascolto_stt_text_t *, char *, size_t);
    void (*destroy)(void *);
    const char *backend;
} ascolto_stt_api_t;
typedef const ascolto_stt_api_t *(*ascolto_stt_entry_t)(void);
typedef enum { ASCOLTO_MODEL_INCLUDED, ASCOLTO_MODEL_MISSING, ASCOLTO_MODEL_CHECKING,
    ASCOLTO_MODEL_DOWNLOADING, ASCOLTO_MODEL_VERIFYING, ASCOLTO_MODEL_READY,
    ASCOLTO_MODEL_ERROR, ASCOLTO_MODEL_UNSUPPORTED } ascolto_model_state_t;
typedef struct {
    char id[40], name[80], description[160], hint[64];
    ascolto_model_state_t state;
    uint64_t bytes, transferred;
    bool selected, active;
    char message[256];
} ascolto_model_snapshot_t;
typedef struct {
    ascolto_model_snapshot_t models[ASCOLTO_LOCAL_MODEL_COUNT];
    uint64_t available_bytes, revision;
    char selected[40], active[40], error[256];
} ascolto_local_models_snapshot_t;
typedef struct ascolto_local_models ascolto_local_models_t;
typedef struct { void *library, *context; const ascolto_stt_api_t *api; char id[40]; } ascolto_local_stt_t;
ascolto_local_models_t *ascolto_local_models_create(const char *config_path);
void ascolto_local_models_destroy(ascolto_local_models_t *);
void ascolto_local_models_snapshot(ascolto_local_models_t *, ascolto_local_models_snapshot_t *);
/* Call mutations on a background action queue. Download owns its own worker. */
int ascolto_local_models_download(ascolto_local_models_t *, const char *id);
void ascolto_local_models_cancel(ascolto_local_models_t *, const char *id);
int ascolto_local_models_select(ascolto_local_models_t *, const char *id);
int ascolto_local_models_remove(ascolto_local_models_t *, const char *id);
/* Session load verifies every artifact and never falls back to a different engine. */
int ascolto_local_stt_begin(ascolto_local_models_t *, ascolto_local_stt_t *);
void ascolto_local_stt_end(ascolto_local_models_t *, ascolto_local_stt_t *);
int ascolto_local_stt_transcribe(ascolto_local_models_t *, ascolto_local_stt_t *, const float *, int,
    const char *language, const char *keywords, ascolto_stt_text_t *);
#ifdef __cplusplus
}
#endif
#endif
