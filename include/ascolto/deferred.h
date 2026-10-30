#ifndef ASCOLTO_DEFERRED_H
#define ASCOLTO_DEFERRED_H
#include <stdbool.h>
#include <stdint.h>

typedef struct ascolto_live_app_s ascolto_live_app_t;
typedef struct ascolto_deferred_worker ascolto_deferred_worker_t;
typedef struct {
    bool available, processing, checking_model, paused_for_capture;
    /* Progress is the delivered PCM sample position per channel. */
    int64_t job_id, session_id, processed_frames, total_frames, completed_segments;
    int64_t cancel_job_id, retry_job_id;
    int queued_count, failed_count;
    char state[24], model_id[40], error_code[80];
} ascolto_deferred_snapshot_t;

/* Start/stop run on the native action/startup worker. The processing context and
 * SQLite connection belong exclusively to the new thread. Stop cooperates at
 * segment boundaries; an adapter call already in progress completes first. */
ascolto_deferred_worker_t *ascolto_deferred_start(ascolto_live_app_t *app);
/* Join on the background lifecycle queue while retaining copied snapshot state
 * for UI readers. Shutdown is idempotent; stop releases the retained object. */
void ascolto_deferred_shutdown(ascolto_deferred_worker_t *worker);
void ascolto_deferred_stop(ascolto_deferred_worker_t *worker);
void ascolto_deferred_wake(ascolto_deferred_worker_t *worker);
/* Snapshot copies and command enqueueing perform no filesystem/database I/O. */
void ascolto_deferred_snapshot(ascolto_deferred_worker_t *worker, ascolto_deferred_snapshot_t *out);
int ascolto_deferred_request_cancel(ascolto_deferred_worker_t *worker, int64_t job_id);
int ascolto_deferred_request_retry(ascolto_deferred_worker_t *worker, int64_t job_id);
#endif
