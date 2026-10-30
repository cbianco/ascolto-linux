#ifndef ASCOLTO_RETRANSCRIBE_H
#define ASCOLTO_RETRANSCRIBE_H
#include "ascolto/local_stt.h"

/* Synchronous preparation; call only on a background queue. All arguments,
 * including the copied model catalog and cancellation context, must remain
 * valid until return. No model is loaded and no preference is changed.
 * The source archive and transcript are never modified. Successful preparation
 * atomically creates a separate transcript version and a durable queued job.
 * Returns a SQLite result code and a bounded, content-free error code. */
int ascolto_retranscribe_enqueue(const char *config_path,
    const ascolto_local_models_snapshot_t *catalog, int64_t source_session_id,
    const char *model_id, const char *language, bool (*cancelled)(void *),
    void *context, int64_t *out_job_id, char *error_code, size_t error_cap);
#endif
