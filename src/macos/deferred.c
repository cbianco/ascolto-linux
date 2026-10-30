#include "ascolto.h"
#include "ascolto/deferred.h"
#include "ascolto/deferred_store.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* Replay lives beside the production VAD/filter/diarizer implementation. */
extern int ascolto_deferred_process_job(ascolto_live_app_t *, ascolto_deferred_store_t *,
    const ascolto_deferred_job_t *, bool (*)(void *), void *);
extern void ascolto_deferred_finalize_job(ascolto_live_app_t *, const ascolto_deferred_job_t *);

typedef struct deferred_command {
    int64_t id;
    bool retry;
    struct deferred_command *next;
} deferred_command_t;

struct ascolto_deferred_worker {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    bool stopping, ready, joining, joined;
    ascolto_live_app_t *source, *context;
    ascolto_deferred_store_t *store;
    ascolto_deferred_snapshot_t snapshot;
    deferred_command_t *commands, *last_command;
    int64_t active_id;
    double last_refresh;
    bool cancel_active;
};

static double monotonic_seconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static bool stopping(ascolto_deferred_worker_t *worker) {
    pthread_mutex_lock(&worker->lock);
    bool stop = worker->stopping;
    pthread_mutex_unlock(&worker->lock);
    return stop;
}

static void wait_for_work(ascolto_deferred_worker_t *worker) {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 250000000;
    if (deadline.tv_nsec >= 1000000000) { deadline.tv_sec++; deadline.tv_nsec -= 1000000000; }
    pthread_mutex_lock(&worker->lock);
    if (!worker->stopping && !worker->commands)
        pthread_cond_timedwait(&worker->condition, &worker->lock, &deadline);
    pthread_mutex_unlock(&worker->lock);
}

static void drain_commands(ascolto_deferred_worker_t *worker) {
    pthread_mutex_lock(&worker->lock);
    deferred_command_t *command = worker->commands;
    worker->commands = worker->last_command = NULL;
    pthread_mutex_unlock(&worker->lock);
    while (command) {
        deferred_command_t *next = command->next;
        int rc = command->retry ? ascolto_deferred_retry(worker->store, command->id) :
                                 ascolto_deferred_cancel(worker->store, command->id);
        if (rc == SQLITE_OK && !command->retry && command->id == worker->active_id)
            worker->cancel_active = true;
        if (rc != SQLITE_OK) {
            pthread_mutex_lock(&worker->lock);
            snprintf(worker->snapshot.error_code, sizeof(worker->snapshot.error_code), "queue_command_failed");
            pthread_mutex_unlock(&worker->lock);
        }
        free(command);
        command = next;
    }
}

static void refresh_snapshot(ascolto_deferred_worker_t *worker) {
    ascolto_deferred_job_t job = {0};
    int rc = worker->active_id ? ascolto_deferred_get(worker->store, worker->active_id, &job) :
                                ascolto_deferred_latest(worker->store, &job);
    int queued = 0, failed = 0;
    int64_t queued_id = 0, retry_id = 0;
    sqlite3_stmt *statement = NULL;
    sqlite3 *db = ascolto_db_sqlite(ascolto_deferred_store_database(worker->store));
    if (sqlite3_prepare_v2(db, "SELECT sum(state='queued'),sum(state='failed'),min(CASE WHEN state='queued' THEN id END),max(CASE WHEN state IN ('failed','cancelled') THEN id END) FROM native_transcription_jobs", -1, &statement, NULL) == SQLITE_OK &&
        sqlite3_step(statement) == SQLITE_ROW) {
        queued = sqlite3_column_int(statement, 0);
        failed = sqlite3_column_int(statement, 1);
        queued_id = sqlite3_column_int64(statement, 2);
        retry_id = sqlite3_column_int64(statement, 3);
    }
    sqlite3_finalize(statement);
    if (!worker->active_id && queued_id) {
        ascolto_deferred_job_clear(&job);
        rc = ascolto_deferred_get(worker->store, queued_id, &job);
    }
    pthread_mutex_lock(&worker->lock);
    worker->snapshot.queued_count = queued;
    worker->snapshot.failed_count = failed;
    worker->snapshot.processing = worker->active_id != 0;
    worker->snapshot.cancel_job_id = worker->active_id ? worker->active_id : queued_id;
    worker->snapshot.retry_job_id = retry_id;
    if (rc == SQLITE_OK) {
        worker->snapshot.job_id = job.id;
        worker->snapshot.session_id = job.session_id;
        worker->snapshot.processed_frames = job.processed_frames;
        worker->snapshot.total_frames = job.sample_count;
        worker->snapshot.completed_segments = job.completed_segments;
        snprintf(worker->snapshot.state, sizeof(worker->snapshot.state), "%s", job.state);
        snprintf(worker->snapshot.model_id, sizeof(worker->snapshot.model_id), "%s", job.model_id);
        snprintf(worker->snapshot.error_code, sizeof(worker->snapshot.error_code), "%s", job.error_code ? job.error_code : "");
        if (job.cancel_requested && worker->active_id) worker->cancel_active = true;
    }
    pthread_mutex_unlock(&worker->lock);
    ascolto_deferred_job_clear(&job);
    worker->last_refresh = monotonic_seconds();
}

/* Called between source frames/segments. A new call pauses replay without
 * relinquishing its deterministic state or loaded model. */
static bool should_stop(void *context) {
    ascolto_deferred_worker_t *worker = context;
    for (;;) {
        if (stopping(worker)) return true;
        drain_commands(worker);
        if (monotonic_seconds() - worker->last_refresh >= 0.25) refresh_snapshot(worker);
        if (worker->cancel_active) return true;
        ascolto_live_snapshot_t live;
        ascolto_live_app_read_snapshot(worker->source, &live);
        bool capture = live.state == ASCOLTO_STATE_RECORDING || live.state == ASCOLTO_STATE_STOPPING;
        pthread_mutex_lock(&worker->lock);
        worker->snapshot.paused_for_capture = capture;
        pthread_mutex_unlock(&worker->lock);
        if (!capture) return false;
        wait_for_work(worker);
    }
}

static int64_t raw_samples(const char *path) {
    struct stat info;
    if (!path || lstat(path, &info) || !S_ISREG(info.st_mode) || info.st_size < 0) return -1;
    return info.st_size / (int64_t)sizeof(float);
}

static int recover_capture(ascolto_deferred_worker_t *worker, const ascolto_deferred_job_t *job,
                           int64_t samples, bool missing) {
    ascolto_db_t *database = ascolto_deferred_store_database(worker->store);
    int rc = ascolto_db_begin(database);
    if (rc != SQLITE_OK) return rc;
    sqlite3_stmt *statement = NULL;
    rc = sqlite3_prepare_v2(ascolto_db_sqlite(database),
        "SELECT coalesce(ended_at_ms,started_at_ms+?2),CASE WHEN ended_at_ms IS NULL THEN json_set(metadata_json,'$.duration_s',?3,'$.capture_interrupted',json('true')) ELSE metadata_json END,coalesce(json_extract(metadata_json,'$.audio_write_failed'),0),ended_at_ms IS NULL,state FROM sessions WHERE id=?1", -1, &statement, NULL);
    char *metadata = NULL;
    int64_t ended = 0;
    bool audio_failed = false, interrupted = false;
    char state[24] = "failed";
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(statement, 1, job->session_id);
        sqlite3_bind_int64(statement, 2, samples / ASCOLTO_SAMPLE_RATE * 1000 + samples % ASCOLTO_SAMPLE_RATE * 1000 / ASCOLTO_SAMPLE_RATE);
        sqlite3_bind_double(statement, 3, (double)samples / ASCOLTO_SAMPLE_RATE);
        rc = sqlite3_step(statement);
        if (rc == SQLITE_ROW) {
            audio_failed = sqlite3_column_int(statement, 2) != 0;
            interrupted = sqlite3_column_int(statement, 3) != 0;
            if (interrupted && samples == 0) missing = true;
            ended = missing && interrupted ? 0 : sqlite3_column_int64(statement, 0);
            if (!interrupted && !missing && !audio_failed) {
                const char *saved_state = (const char *)sqlite3_column_text(statement, 4);
                snprintf(state, sizeof(state), "%s", saved_state ? saved_state : "failed");
            }
            const char *value = (const char *)sqlite3_column_text(statement, 1);
            metadata = value ? strdup(value) : NULL;
            rc = metadata ? SQLITE_OK : SQLITE_NOMEM;
        }
    }
    sqlite3_finalize(statement);
    if (rc == SQLITE_OK) rc = ascolto_db_session_state(database, job->session_id, state, ended, metadata);
    free(metadata);
    if (rc == SQLITE_OK) rc = missing || audio_failed ?
        ascolto_deferred_finish(worker->store, job->id, "failed", audio_failed ? "audio_write_failed" : "source_audio_missing") :
        ascolto_deferred_seal(worker->store, job->id, samples);
    if (rc == SQLITE_OK) rc = ascolto_db_commit(database);
    if (rc != SQLITE_OK) ascolto_db_rollback(database);
    return rc;
}

static int next_closed_recording(ascolto_deferred_worker_t *worker, ascolto_deferred_job_t *job) {
    sqlite3_stmt *statement = NULL;
    sqlite3 *db = ascolto_db_sqlite(ascolto_deferred_store_database(worker->store));
    int rc = sqlite3_prepare_v2(db, "SELECT j.id FROM native_transcription_jobs j JOIN sessions s ON s.id=j.session_id WHERE j.state='recording' AND s.ended_at_ms IS NOT NULL ORDER BY j.id LIMIT 1", -1, &statement, NULL);
    int64_t id = 0;
    if (rc == SQLITE_OK) {
        rc = sqlite3_step(statement);
        if (rc == SQLITE_ROW) { id = sqlite3_column_int64(statement, 0); rc = SQLITE_OK; }
        else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    }
    sqlite3_finalize(statement);
    return rc == SQLITE_OK ? ascolto_deferred_get(worker->store, id, job) : rc;
}

static void recover_recordings(ascolto_deferred_worker_t *worker, bool closed_only) {
    if (closed_only) {
        ascolto_live_snapshot_t live;
        ascolto_live_app_read_snapshot(worker->source, &live);
        if (live.state == ASCOLTO_STATE_RECORDING || live.state == ASCOLTO_STATE_STOPPING) return;
    }
    ascolto_deferred_job_t job = {0};
    while (!stopping(worker)) {
        int rc = closed_only ? next_closed_recording(worker, &job) : ascolto_deferred_next_recording(worker->store, &job);
        if (rc != SQLITE_OK) break;
        int64_t mic = raw_samples(job.mic_path), sys = raw_samples(job.sys_path);
        /* Preserve all durable PCM. Replay pads a shorter channel with silence.
         * At runtime only a durably closed session may leave recording state. */
        bool missing = mic < 0 || sys < 0;
        rc = recover_capture(worker, &job, missing ? 0 : mic > sys ? mic : sys, missing);
        ascolto_deferred_job_clear(&job);
        if (rc != SQLITE_OK) break;
    }
}

static void recover_finalization(ascolto_deferred_worker_t *worker) {
    sqlite3 *db = ascolto_db_sqlite(ascolto_deferred_store_database(worker->store));
    int64_t cursor = 0;
    while (!stopping(worker)) {
        sqlite3_stmt *statement = NULL;
        if (sqlite3_prepare_v2(db, "SELECT id FROM native_transcription_jobs WHERE state='completed' AND id>? ORDER BY id LIMIT 1", -1, &statement, NULL) != SQLITE_OK) break;
        sqlite3_bind_int64(statement, 1, cursor);
        int rc = sqlite3_step(statement);
        if (rc == SQLITE_ROW) cursor = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        if (rc != SQLITE_ROW) break;
        ascolto_deferred_job_t job = {0};
        if (ascolto_deferred_get(worker->store, cursor, &job) != SQLITE_OK) break;
        if (raw_samples(job.mic_path) >= 0 || raw_samples(job.sys_path) >= 0)
            ascolto_deferred_finalize_job(worker->context, &job);
        ascolto_deferred_job_clear(&job);
    }
}

static bool model_checking(ascolto_deferred_worker_t *worker, const char *id) {
    ascolto_local_models_snapshot_t models;
    ascolto_local_models_snapshot(worker->context->local_models, &models);
    for (int i = 0; i < ASCOLTO_LOCAL_MODEL_COUNT; i++)
        if (!strcmp(models.models[i].id, id)) return models.models[i].state == ASCOLTO_MODEL_CHECKING ||
            models.models[i].state == ASCOLTO_MODEL_VERIFYING || models.models[i].state == ASCOLTO_MODEL_DOWNLOADING;
    return false;
}

static void release_model(ascolto_deferred_worker_t *worker) {
    if (worker->context->local_stt.id[0])
        ascolto_local_stt_end(worker->context->local_models, &worker->context->local_stt);
}

static void finish_job(ascolto_deferred_worker_t *worker, const ascolto_deferred_job_t *job, int result) {
    while (!stopping(worker)) {
        drain_commands(worker);
        int rc = ascolto_deferred_finish(worker->store, job->id,
            worker->cancel_active ? "cancelled" : result == 0 ? "completed" : "failed",
            worker->cancel_active || result == 0 ? NULL : "transcription_failed");
        if (rc == SQLITE_OK) {
            /* A cancellation from another connection can win inside finish().
             * Read the durable result before any source cleanup/finalization. */
            ascolto_deferred_job_t saved = {0};
            if (ascolto_deferred_get(worker->store, job->id, &saved) == SQLITE_OK &&
                !strcmp(saved.state, "completed"))
                ascolto_deferred_finalize_job(worker->context, &saved);
            ascolto_deferred_job_clear(&saved);
            return;
        }
        pthread_mutex_lock(&worker->lock);
        snprintf(worker->snapshot.error_code, sizeof(worker->snapshot.error_code), "queue_commit_failed");
        pthread_mutex_unlock(&worker->lock);
        wait_for_work(worker);
    }
}

static void *worker_main(void *context) {
    ascolto_deferred_worker_t *worker = context;
    int rc = ascolto_deferred_store_open(worker->context->config.config_path, &worker->store);
    if (rc == SQLITE_OK) rc = ascolto_deferred_worker_start(worker->store);
    pthread_mutex_lock(&worker->lock);
    worker->snapshot.available = rc == SQLITE_OK;
    snprintf(worker->snapshot.state, sizeof(worker->snapshot.state), "%s", rc == SQLITE_OK ? "idle" : "unavailable");
    if (rc != SQLITE_OK) snprintf(worker->snapshot.error_code, sizeof(worker->snapshot.error_code), "%s", rc == SQLITE_BUSY ? "queue_worker_busy" : "queue_storage_error");
    pthread_mutex_unlock(&worker->lock);
    if (rc == SQLITE_OK) { recover_recordings(worker, false); recover_finalization(worker); }
    pthread_mutex_lock(&worker->lock);
    worker->ready = true;
    pthread_cond_broadcast(&worker->condition);
    pthread_mutex_unlock(&worker->lock);
    if (rc != SQLITE_OK) goto cleanup;
    while (!stopping(worker)) {
        drain_commands(worker);
        refresh_snapshot(worker);
        if (should_stop(worker)) break;
        recover_recordings(worker, true);
        ascolto_deferred_job_t job = {0};
        rc = ascolto_deferred_claim_next(worker->store, &job);
        if (rc == SQLITE_NOTFOUND) {
            release_model(worker);
            wait_for_work(worker);
            continue;
        }
        if (rc != SQLITE_OK) { wait_for_work(worker); continue; }
        worker->active_id = job.id;
        worker->cancel_active = false;
        refresh_snapshot(worker);
        if (strcmp(worker->context->local_stt.id, job.model_id)) release_model(worker);
        rc = worker->context->local_stt.id[0] ? 0 :
            ascolto_local_stt_begin_id(worker->context->local_models, &worker->context->local_stt, job.model_id);
        if (rc != 0 && model_checking(worker, job.model_id)) {
            ascolto_deferred_defer(worker->store, job.id);
            pthread_mutex_lock(&worker->lock);
            worker->snapshot.checking_model = true;
            pthread_mutex_unlock(&worker->lock);
            worker->active_id = 0;
            ascolto_deferred_job_clear(&job);
            wait_for_work(worker);
            continue;
        }
        pthread_mutex_lock(&worker->lock);
        worker->snapshot.checking_model = false;
        pthread_mutex_unlock(&worker->lock);
        if (rc != 0) {
            release_model(worker);
            /* This failure is actionable; only checking is retried automatically. */
            while (!stopping(worker) && ascolto_deferred_finish(worker->store, job.id, "failed", "model_unavailable") != SQLITE_OK)
                wait_for_work(worker);
        } else {
            rc = ascolto_deferred_process_job(worker->context, worker->store, &job, should_stop, worker);
            finish_job(worker, &job, rc);
            if (rc < 0) release_model(worker);
        }
        worker->active_id = 0;
        worker->cancel_active = false;
        ascolto_deferred_job_clear(&job);
    }
    release_model(worker);
cleanup:
    if (worker->store) ascolto_deferred_store_close(worker->store);
    worker->store = NULL;
    return NULL;
}

ascolto_deferred_worker_t *ascolto_deferred_start(ascolto_live_app_t *app) {
    if (!app) return NULL;
    ascolto_deferred_worker_t *worker = calloc(1, sizeof(*worker));
    if (!worker) return NULL;
    worker->context = calloc(1, sizeof(*worker->context));
    if (!worker->context) { free(worker); return NULL; }
    worker->source = app;
    worker->context->config = app->config;
    worker->context->local_models = app->local_models;
    worker->context->whistle = app->whistle;
    pthread_mutex_init(&worker->lock, NULL);
    pthread_cond_init(&worker->condition, NULL);
    pthread_mutex_init(&worker->context->lock, NULL);
    pthread_cond_init(&worker->context->session_cond, NULL);
    pthread_mutex_init(&worker->context->snapshot_lock, NULL);
    snprintf(worker->snapshot.state, sizeof(worker->snapshot.state), "starting");
    if (pthread_create(&worker->thread, NULL, worker_main, worker)) {
        pthread_mutex_destroy(&worker->context->snapshot_lock);
        pthread_cond_destroy(&worker->context->session_cond);
        pthread_mutex_destroy(&worker->context->lock);
        pthread_cond_destroy(&worker->condition);
        pthread_mutex_destroy(&worker->lock);
        free(worker->context);
        free(worker);
        return NULL;
    }
    pthread_mutex_lock(&worker->lock);
    while (!worker->ready) pthread_cond_wait(&worker->condition, &worker->lock);
    bool available = worker->snapshot.available;
    pthread_mutex_unlock(&worker->lock);
    if (!available) { ascolto_deferred_stop(worker); return NULL; }
    return worker;
}

void ascolto_deferred_shutdown(ascolto_deferred_worker_t *worker) {
    if (!worker) return;
    pthread_mutex_lock(&worker->lock);
    worker->stopping = true;
    pthread_cond_broadcast(&worker->condition);
    while (worker->joining && !worker->joined)
        pthread_cond_wait(&worker->condition, &worker->lock);
    if (worker->joined) { pthread_mutex_unlock(&worker->lock); return; }
    worker->joining = true;
    pthread_mutex_unlock(&worker->lock);
    pthread_join(worker->thread, NULL);
    pthread_mutex_lock(&worker->lock);
    worker->joining = false;
    worker->joined = true;
    worker->snapshot.available = false;
    worker->snapshot.processing = false;
    worker->snapshot.paused_for_capture = false;
    worker->snapshot.checking_model = false;
    snprintf(worker->snapshot.state, sizeof(worker->snapshot.state), "stopped");
    pthread_cond_broadcast(&worker->condition);
    pthread_mutex_unlock(&worker->lock);
}

void ascolto_deferred_stop(ascolto_deferred_worker_t *worker) {
    if (!worker) return;
    ascolto_deferred_shutdown(worker);
    while (worker->commands) { deferred_command_t *next = worker->commands->next; free(worker->commands); worker->commands = next; }
    ascolto_online_vad_free(&worker->context->vad_mic);
    ascolto_online_vad_free(&worker->context->vad_sys);
    pthread_mutex_destroy(&worker->context->snapshot_lock);
    pthread_cond_destroy(&worker->context->session_cond);
    pthread_mutex_destroy(&worker->context->lock);
    pthread_cond_destroy(&worker->condition);
    pthread_mutex_destroy(&worker->lock);
    free(worker->context);
    free(worker);
}

void ascolto_deferred_wake(ascolto_deferred_worker_t *worker) {
    if (!worker) return;
    pthread_mutex_lock(&worker->lock);
    pthread_cond_signal(&worker->condition);
    pthread_mutex_unlock(&worker->lock);
}

void ascolto_deferred_snapshot(ascolto_deferred_worker_t *worker, ascolto_deferred_snapshot_t *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    if (!worker) return;
    pthread_mutex_lock(&worker->lock);
    *out = worker->snapshot;
    pthread_mutex_unlock(&worker->lock);
}

static int request(ascolto_deferred_worker_t *worker, int64_t id, bool retry) {
    if (!worker || id <= 0) return -1;
    deferred_command_t *command = calloc(1, sizeof(*command));
    if (!command) return -1;
    command->id = id;
    command->retry = retry;
    pthread_mutex_lock(&worker->lock);
    if (worker->stopping || !worker->snapshot.available) {
        pthread_mutex_unlock(&worker->lock);
        free(command);
        return -1;
    }
    if (worker->last_command) worker->last_command->next = command;
    else worker->commands = command;
    worker->last_command = command;
    pthread_cond_signal(&worker->condition);
    pthread_mutex_unlock(&worker->lock);
    return 0;
}

int ascolto_deferred_request_cancel(ascolto_deferred_worker_t *worker, int64_t id) { return request(worker, id, false); }
int ascolto_deferred_request_retry(ascolto_deferred_worker_t *worker, int64_t id) { return request(worker, id, true); }
