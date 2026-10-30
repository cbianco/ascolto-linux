#ifndef ASCOLTO_DEFERRED_STORE_H
#define ASCOLTO_DEFERRED_STORE_H
#include "ascolto/database.h"

typedef struct ascolto_deferred_store ascolto_deferred_store_t;
typedef struct {
    int64_t session_id, sample_count;
    const char *model_id, *language, *keywords, *mic_path, *sys_path, *metadata_json;
    double diarizer_threshold;
} ascolto_deferred_enqueue_t;
typedef struct {
    /* processed_frames and segment end_frame are PCM sample positions per channel. */
    int64_t id, session_id, sample_count, completed_segments, processed_frames;
    int64_t queued_at_ms, started_at_ms, finished_at_ms;
    int cancel_requested;
    double diarizer_threshold;
    char *state, *model_id, *language, *keywords, *mic_path, *sys_path, *metadata_json, *error_code;
} ascolto_deferred_job_t;
typedef struct {
    int64_t ordinal, end_frame;
    char *channel;
    unsigned checkpoint_version;
    void *checkpoint;
    size_t checkpoint_size;
    int64_t start_ms, end_ms;
} ascolto_deferred_segment_t;

/* Each handle belongs to one background worker. All strings in snapshots are
 * copied and released with the corresponding clear function. SQLite result codes
 * are returned; SQLITE_NOTFOUND means no matching job/segment. No file is deleted. */
int ascolto_deferred_store_open(const char *config_path, ascolto_deferred_store_t **out);
void ascolto_deferred_store_close(ascolto_deferred_store_t *store);
ascolto_db_t *ascolto_deferred_store_database(ascolto_deferred_store_t *store);
int ascolto_deferred_record(ascolto_deferred_store_t *store, const ascolto_deferred_enqueue_t *values, int64_t *job_id);
int ascolto_deferred_seal(ascolto_deferred_store_t *store, int64_t job_id, int64_t sample_count);
int ascolto_deferred_next_recording(ascolto_deferred_store_t *store, ascolto_deferred_job_t *out);
int ascolto_deferred_enqueue(ascolto_deferred_store_t *store, const ascolto_deferred_enqueue_t *values, int64_t *job_id);
int ascolto_deferred_get(ascolto_deferred_store_t *store, int64_t job_id, ascolto_deferred_job_t *out);
int ascolto_deferred_latest(ascolto_deferred_store_t *store, ascolto_deferred_job_t *out);
void ascolto_deferred_job_clear(ascolto_deferred_job_t *job);
/* Holds a nonblocking process lock until close. Only after proving exclusive
 * ownership does it requeue interrupted jobs, preserving committed segments. */
int ascolto_deferred_worker_start(ascolto_deferred_store_t *store);
int ascolto_deferred_claim_next(ascolto_deferred_store_t *store, ascolto_deferred_job_t *out);
int ascolto_deferred_defer(ascolto_deferred_store_t *store, int64_t job_id);
int ascolto_deferred_cancel(ascolto_deferred_store_t *store, int64_t job_id);
int ascolto_deferred_retry(ascolto_deferred_store_t *store, int64_t job_id);
int ascolto_deferred_segment_get(ascolto_deferred_store_t *store, int64_t job_id, int64_t ordinal, ascolto_deferred_segment_t *out);
void ascolto_deferred_segment_clear(ascolto_deferred_segment_t *segment);
/* Replay source frames from zero to reconstruct VAD/echo/diarization state.
 * Every deterministic segment, including rejected/silent segments, receives an
 * ordinal. Cached versioned checkpoint blobs avoid repeated ASR. A new result, optional accepted
 * utterance and progress checkpoint commit atomically. Repeating an identical
 * segment is a no-op; conflicting/noncontiguous results fail. */
int ascolto_deferred_segment_commit(ascolto_deferred_store_t *store, int64_t job_id,
    const ascolto_deferred_segment_t *segment, const ascolto_db_utterance_t *utterance);
int ascolto_deferred_progress(ascolto_deferred_store_t *store, int64_t job_id, int64_t processed_frames);
/* state: completed, failed or cancelled. Error codes contain only lowercase ASCII
 * letters, digits and underscores; no transcripts or provider responses.
 * Capture may fail a recording after an audio write/fsync error without a worker
 * lock. A locked worker may also fail/cancel an interrupted recording. */
int ascolto_deferred_finish(ascolto_deferred_store_t *store, int64_t job_id, const char *state, const char *error_code);
#endif
