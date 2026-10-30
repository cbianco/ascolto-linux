#ifndef ASCOLTO_LOCAL_STT_H
#define ASCOLTO_LOCAL_STT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ASCOLTO_LOCAL_MODEL_COUNT 6
#define ASCOLTO_STT_ABI 1
/* UI grouping only: package identities, downloads and saved choices stay separate. */
static inline int ascolto_nemotron_bits(const char *id) {
    if (!id) return 0;
    if (!strcmp(id, "nemotron-3.5-q8")) return 8;
    if (!strcmp(id, "nemotron-3.5-bf16-ssd")) return 16;
    return 0;
}
static inline const char *ascolto_nemotron_id(int bits) {
    return bits == 8 ? "nemotron-3.5-q8" :
           bits == 16 ? "nemotron-3.5-bf16-ssd" : "";
}
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
    char selected[40], active[40], loading[40], error[256];
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
/* Session load reopens/checks every artifact. Unchanged process-local verified
 * identities reuse SHA-256 results; changed identities are hashed again. */
int ascolto_local_stt_begin(ascolto_local_models_t *, ascolto_local_stt_t *);
/* Load a frozen job choice without changing the saved next-session selection.
 * The caller owns one session until end; consecutive same-model jobs may reuse
 * it. Missing/checking packages fail explicitly without a model fallback. */
int ascolto_local_stt_begin_id(ascolto_local_models_t *, ascolto_local_stt_t *, const char *id);
#ifdef ASCOLTO_LOCAL_STT_TEST_MANIFEST
uint64_t ascolto_local_models_test_hash_count(ascolto_local_models_t *, const char *id);
int ascolto_local_models_test_first_hash(ascolto_local_models_t *);
#endif
void ascolto_local_stt_end(ascolto_local_models_t *, ascolto_local_stt_t *);
int ascolto_local_stt_transcribe(ascolto_local_models_t *, ascolto_local_stt_t *, const float *, int,
    const char *language, const char *keywords, ascolto_stt_text_t *);
#ifdef __cplusplus
}
#endif
#endif
