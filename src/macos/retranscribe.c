#include "ascolto/retranscribe.h"
#include "ascolto/deferred_store.h"
#include "ascolto/storage.h"
#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    int64_t started, ended;
    char *backend, *title, *meeting, *app, *vocabulary, *metadata;
    char *audio, *transcript, *keywords;
    double threshold;
} source_t;

static void source_clear(source_t *s) {
    free(s->backend); free(s->title); free(s->meeting); free(s->app);
    free(s->vocabulary); free(s->metadata); free(s->audio);
    free(s->transcript); free(s->keywords);
}
static void bind_text(sqlite3_stmt *q, int n, const char *s) {
    sqlite3_bind_text(q, n, s, -1, SQLITE_TRANSIENT);
}
static bool is_cancelled(bool (*callback)(void *), void *context) {
    return callback && callback(context);
}
static int failure(char *error, size_t cap, const char *code, int rc) {
    if (error && cap) snprintf(error, cap, "%s", code);
    return rc;
}
static bool installed(const ascolto_local_models_snapshot_t *catalog, const char *id) {
    static const char *allowed[] = {"whistle", "whisper-turbo-q5", "parakeet-v3-q8",
        "qwen3-asr-1.7b", "nemotron-3.5-bf16-ssd", "nemotron-3.5-q8"};
    if (!catalog || !id) return false;
    bool known = false;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(*allowed); i++)
        if (!strcmp(id, allowed[i])) known = true;
    if (!known) return false;
    for (int i = 0; i < ASCOLTO_LOCAL_MODEL_COUNT; i++)
        if (!strcmp(id, catalog->models[i].id))
            return catalog->models[i].state == ASCOLTO_MODEL_READY ||
                (!strcmp(id, "whistle") && catalog->models[i].state == ASCOLTO_MODEL_INCLUDED);
    return false;
}
static bool valid_language(const char *language) {
    static const char *allowed[] = {"auto", "it", "en", "de", "fr", "es", "nl", "pl"};
    if (!language) return false;
    for (size_t i = 0; i < sizeof(allowed) / sizeof(*allowed); i++)
        if (!strcmp(language, allowed[i])) return true;
    return false;
}

/* This check is repeated inside the final write transaction. The per-source
 * flock also serializes simultaneous preparation before either job exists. */
static int check_available(sqlite3 *db, int64_t source, const char *model,
                           const char *language, const char **error) {
    sqlite3_stmt *q = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT s.state,EXISTS(SELECT 1 FROM native_transcription_jobs j WHERE j.session_id=s.id AND j.state IN ('recording','queued','running')),"
        "EXISTS(SELECT 1 FROM native_transcription_jobs j WHERE json_extract(j.metadata_json,'$.native_transcription_source_session')=s.id AND j.model_id=?2 AND j.language=?3 AND j.state IN ('recording','queued','running')),"
        "coalesce(json_extract(s.metadata_json,'$.native_transcription_source_session'),0),s.capture_backend "
        "FROM sessions s WHERE s.id=?1", -1, &q, NULL);
    if (!rc) {
        sqlite3_bind_int64(q, 1, source); bind_text(q, 2, model); bind_text(q, 3, language);
        rc = sqlite3_step(q);
        if (rc == SQLITE_ROW) {
            const char *state = (const char *)sqlite3_column_text(q, 0);
            const char *backend = (const char *)sqlite3_column_text(q, 4);
            if (sqlite3_column_int64(q, 3) > 0 || !backend || strcmp(backend, "coreaudio")) {
                *error = "source_unsupported"; rc = SQLITE_MISUSE;
            } else if (!state || (strcmp(state, "completed") && strcmp(state, "failed")) || sqlite3_column_int(q, 1)) {
                *error = "source_busy"; rc = SQLITE_BUSY;
            } else if (sqlite3_column_int(q, 2)) { *error = "already_queued"; rc = SQLITE_BUSY; }
            else rc = SQLITE_OK;
        } else if (rc == SQLITE_DONE) { *error = "source_unavailable"; rc = SQLITE_NOTFOUND; }
    }
    sqlite3_finalize(q);
    return rc;
}

static int load_source(ascolto_db_t *database, int64_t id, source_t *s) {
    sqlite3 *db = ascolto_db_sqlite(database);
    sqlite3_stmt *q = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT started_at_ms,coalesce(ended_at_ms,0),capture_backend,coalesce(call_title,''),coalesce(meeting_title,''),coalesce(source_app,''),vocabulary_snapshot_json,metadata_json,"
        "(SELECT path FROM artifacts WHERE session_id=s.id AND kind='audio_m4a' AND state='ready' ORDER BY id DESC LIMIT 1),"
        "(SELECT path FROM artifacts WHERE session_id=s.id AND kind='transcript_md' ORDER BY id DESC LIMIT 1),"
        "coalesce((SELECT diarizer_threshold FROM native_transcription_jobs WHERE session_id=s.id ORDER BY id DESC LIMIT 1),json_extract(metadata_json,'$.diarizer_threshold')),"
        "(SELECT keywords FROM native_transcription_jobs WHERE session_id=s.id ORDER BY id DESC LIMIT 1) "
        "FROM sessions s WHERE id=?", -1, &q, NULL);
    if (rc) return rc;
    sqlite3_bind_int64(q, 1, id);
    rc = sqlite3_step(q);
    if (rc == SQLITE_ROW) {
        s->started = sqlite3_column_int64(q, 0); s->ended = sqlite3_column_int64(q, 1);
        char **strings[] = {&s->backend, &s->title, &s->meeting, &s->app,
            &s->vocabulary, &s->metadata, &s->audio, &s->transcript};
        rc = SQLITE_OK;
        for (int i = 0; i < 8; i++) {
            const char *value = (const char *)sqlite3_column_text(q, i + 2);
            *strings[i] = value ? strdup(value) : NULL;
            if (value && !*strings[i]) rc = SQLITE_NOMEM;
        }
        s->threshold = sqlite3_column_type(q, 10) == SQLITE_NULL ? -1 : sqlite3_column_double(q, 10);
        if (sqlite3_column_type(q, 11) != SQLITE_NULL) {
            const char *keywords = (const char *)sqlite3_column_text(q, 11);
            s->keywords = strdup(keywords);
            if (!s->keywords) rc = SQLITE_NOMEM;
        }
    } else if (rc == SQLITE_DONE) rc = SQLITE_NOTFOUND;
    sqlite3_finalize(q);
    if (rc) return rc;
    if (!isfinite(s->threshold) || s->threshold < 0 || s->threshold > 1) {
        char value[64] = ""; char *end = NULL;
        s->threshold = .60;
        if (!ascolto_storage_get(database, "diarizer_threshold", value, sizeof(value))) {
            double current = strtod(value, &end);
            if (end != value && !*end && isfinite(current) && current > 0 && current <= 1) s->threshold = current;
        }
    }
    /* Recording-first sessions have the full frozen keywords, including call
     * attendees. Older captures retain their vocabulary snapshot instead. */
    if (s->keywords) return SQLITE_OK;
    rc = sqlite3_prepare_v2(db, "SELECT value FROM json_each(?1) WHERE type='text' ORDER BY key", -1, &q, NULL);
    if (rc) return rc;
    bind_text(q, 1, s->vocabulary);
    size_t used = 0;
    s->keywords = calloc(1, 65536);
    if (!s->keywords) { sqlite3_finalize(q); return SQLITE_NOMEM; }
    while ((rc = sqlite3_step(q)) == SQLITE_ROW) {
        const char *word = (const char *)sqlite3_column_text(q, 0);
        size_t n = strlen(word);
        if (used + n + 2 > 65536) { rc = SQLITE_TOOBIG; break; }
        if (used) s->keywords[used++] = '\n';
        memcpy(s->keywords + used, word, n + 1); used += n;
    }
    sqlite3_finalize(q);
    return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static bool same_file(const struct stat *a, const struct stat *b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
        a->st_mtimespec.tv_sec == b->st_mtimespec.tv_sec && a->st_mtimespec.tv_nsec == b->st_mtimespec.tv_nsec &&
        a->st_ctimespec.tv_sec == b->st_ctimespec.tv_sec && a->st_ctimespec.tv_nsec == b->st_ctimespec.tv_nsec;
}
static int sync_directory(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return -1;
    int rc = fsync(fd); close(fd); return rc;
}
static int directory(const char *path) {
    if (mkdir(path, 0700) && errno != EEXIST) return -1;
    struct stat st;
    return lstat(path, &st) || !S_ISDIR(st.st_mode) ? -1 : 0;
}
static int write_all(int fd, const float *samples, size_t count) {
    const char *p = (const char *)samples;
    size_t left = count * sizeof(float);
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; left -= (size_t)n;
    }
    return 0;
}

static int decode(const char *archive, const char *mic_path, const char *sys_path,
                  bool (*cancelled)(void *), void *context, int64_t *samples,
                  const char **error) {
    int source = -1, output[2] = {-1, -1}, rc = SQLITE_IOERR;
    struct stat before, after, named;
    ExtAudioFileRef audio = NULL;
    CFURLRef url = NULL;
    source = open(archive, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (source < 0 || fstat(source, &before) || !S_ISREG(before.st_mode)) {
        *error = "source_audio_missing"; goto done;
    }
    url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)archive, strlen(archive), false);
    if (!url || ExtAudioFileOpenURL(url, &audio)) goto done;
    AudioStreamBasicDescription format = {0}; UInt32 size = sizeof(format);
    if (ExtAudioFileGetProperty(audio, kExtAudioFileProperty_FileDataFormat, &size, &format)) goto done;
    if (format.mChannelsPerFrame != 2) { *error = "source_audio_not_stereo"; rc = SQLITE_FORMAT; goto done; }
    format = (AudioStreamBasicDescription){.mSampleRate = 16000, .mFormatID = kAudioFormatLinearPCM,
        .mFormatFlags = kAudioFormatFlagsNativeFloatPacked, .mBytesPerPacket = 2 * sizeof(float),
        .mFramesPerPacket = 1, .mBytesPerFrame = 2 * sizeof(float), .mChannelsPerFrame = 2, .mBitsPerChannel = 32};
    if (ExtAudioFileSetProperty(audio, kExtAudioFileProperty_ClientDataFormat, sizeof(format), &format)) goto done;
    output[0] = open(mic_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    output[1] = open(sys_path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (output[0] < 0 || output[1] < 0) { *error = "storage_error"; goto done; }
    *samples = 0;
    for (;;) {
        if (is_cancelled(cancelled, context)) { *error = "cancelled"; rc = SQLITE_INTERRUPT; goto done; }
        float interleaved[8192], channels[2][4096];
        AudioBufferList buffers = {.mNumberBuffers = 1,
            .mBuffers = {{.mNumberChannels = 2, .mDataByteSize = sizeof(interleaved), .mData = interleaved}}};
        UInt32 count = 4096;
        if (ExtAudioFileRead(audio, &count, &buffers)) goto done;
        if (!count) break;
        for (UInt32 i = 0; i < count; i++) for (int ch = 0; ch < 2; ch++) {
            float value = interleaved[2 * i + ch];
            if (!isfinite(value)) goto done;
            channels[ch][i] = value;
        }
        if (write_all(output[0], channels[0], count) || write_all(output[1], channels[1], count)) {
            *error = "storage_error"; goto done;
        }
        if (*samples > INT64_MAX - count) goto done;
        *samples += count;
    }
    if (!*samples || fstat(source, &after) || lstat(archive, &named) ||
        !same_file(&before, &after) || !same_file(&before, &named)) goto done;
    for (int ch = 0; ch < 2; ch++) {
        if (fsync(output[ch])) { *error = "storage_error"; goto done; }
        int fd = output[ch]; output[ch] = -1;
        if (close(fd)) { *error = "storage_error"; goto done; }
    }
    rc = SQLITE_OK;
done:
    if (audio) ExtAudioFileDispose(audio);
    if (url) CFRelease(url);
    if (source >= 0) close(source);
    for (int ch = 0; ch < 2; ch++) if (output[ch] >= 0) close(output[ch]);
    return rc;
}

static char *version_metadata(sqlite3 *db, const source_t *source, int64_t id,
                              const char *model, const char *language, const char *markdown) {
    sqlite3_stmt *q = NULL;
    char *metadata = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT json_set(?1,'$.native_transcription_source_session',?2,'$.retranscription',json('true'),"
        "'$.local_model',?3,'$.language',?4,'$.transcript_md',?5,'$.audio_m4a','','$.post_call',json('true'),"
        "'$.retain_audio',json('false'),'$.cloud_recap',json('false'),'$.recap_tts',json('false'),"
        "'$.transcription_failed',json('false'),'$.audio_write_failed',json('false'),'$.utterances',0,'$.remote_speakers',0)",
        -1, &q, NULL) == SQLITE_OK) {
        bind_text(q, 1, source->metadata); sqlite3_bind_int64(q, 2, id);
        bind_text(q, 3, model); bind_text(q, 4, language); bind_text(q, 5, markdown);
        if (sqlite3_step(q) == SQLITE_ROW) metadata = strdup((const char *)sqlite3_column_text(q, 0));
    }
    sqlite3_finalize(q);
    return metadata;
}

int ascolto_retranscribe_enqueue(const char *config,
    const ascolto_local_models_snapshot_t *catalog, int64_t source_id,
    const char *model, const char *language, bool (*cancelled)(void *),
    void *context, int64_t *out_job_id, char *error, size_t error_cap) {
    if (out_job_id) *out_job_id = 0;
    if (error && error_cap) *error = 0;
    if (!config || source_id <= 0 || !out_job_id) return failure(error, error_cap, "invalid_request", SQLITE_MISUSE);
    if (!installed(catalog, model)) return failure(error, error_cap, "model_unavailable", SQLITE_NOTFOUND);
    if (!valid_language(language)) return failure(error, error_cap, "invalid_language", SQLITE_MISUSE);
    if (is_cancelled(cancelled, context)) return failure(error, error_cap, "cancelled", SQLITE_INTERRUPT);
    ascolto_deferred_store_t *store = NULL;
    source_t source = {0};
    int lock_fd = -1, rc = ascolto_deferred_store_open(config, &store);
    const char *code = "storage_error";
    char data[PATH_MAX] = "", history[PATH_MAX] = "", version[PATH_MAX] = "";
    char audio_dir[PATH_MAX] = "", version_data[PATH_MAX] = "", mic[PATH_MAX] = "", sys[PATH_MAX] = "";
    char markdown[PATH_MAX], jsonl[PATH_MAX], *metadata = NULL;
    bool committed = false, transaction = false;
    if (rc) goto done;
    ascolto_db_t *database = ascolto_deferred_store_database(store);
    sqlite3 *db = ascolto_db_sqlite(database);
    const char *db_path = sqlite3_db_filename(db, "main");
    char lock_path[PATH_MAX];
    if (!db_path || snprintf(lock_path, sizeof(lock_path), "%s.retranscribe-%lld.lock", db_path, (long long)source_id) >= (int)sizeof(lock_path)) { rc = SQLITE_TOOBIG; goto done; }
    lock_fd = open(lock_path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat lock_stat;
    if (lock_fd < 0 || fstat(lock_fd, &lock_stat) || !S_ISREG(lock_stat.st_mode)) { rc = SQLITE_CANTOPEN; goto done; }
    if (flock(lock_fd, LOCK_EX | LOCK_NB)) { code = "already_queued"; rc = SQLITE_BUSY; goto done; }
    rc = check_available(db, source_id, model, language, &code);
    if (rc) goto done;
    rc = load_source(database, source_id, &source);
    if (rc) goto done;
    if (!source.audio || source.audio[0] != '/' || !source.transcript || source.transcript[0] != '/') {
        code = "source_audio_missing"; rc = SQLITE_NOTFOUND; goto done;
    }
    char call[PATH_MAX];
    if (strlen(source.transcript) >= sizeof(call)) { rc = SQLITE_TOOBIG; goto done; }
    strcpy(call, source.transcript);
    char *slash = strrchr(call, '/');
    if (!slash || slash == call) { rc = SQLITE_CANTOPEN; goto done; }
    char parent[PATH_MAX];
    size_t parent_length = (size_t)(slash - call);
    memcpy(parent, call, parent_length); parent[parent_length] = 0;
    if (!strcmp(slash + 1, "transcript.md")) *slash = 0;
    else {
        /* Legacy flat transcripts share an archive root. Match the call folder
         * used by native/Python recap history without moving the originals. */
        char *suffix = strrchr(slash + 1, '.');
        if (suffix) *suffix = 0;
        if (directory(call)) { code = "source_unavailable"; rc = SQLITE_CANTOPEN; goto done; }
    }
    char canonical[PATH_MAX];
    if (!realpath(call, canonical)) { code = "source_unavailable"; rc = SQLITE_CANTOPEN; goto done; }
    if (snprintf(data, sizeof(data), "%s/Data", canonical) >= (int)sizeof(data) ||
        snprintf(history, sizeof(history), "%s/Local-transcription-history", data) >= (int)sizeof(history) ||
        snprintf(version, sizeof(version), "%s/version-XXXXXX", history) >= (int)sizeof(version)) { version[0] = 0; rc = SQLITE_TOOBIG; goto done; }
    if (directory(data) || directory(history)) { version[0] = 0; rc = SQLITE_CANTOPEN; goto done; }
    if (!mkdtemp(version)) { version[0] = 0; rc = SQLITE_CANTOPEN; goto done; }
    if (snprintf(audio_dir, sizeof(audio_dir), "%s/Audio", version) >= (int)sizeof(audio_dir) ||
        snprintf(version_data, sizeof(version_data), "%s/Data", version) >= (int)sizeof(version_data) ||
        snprintf(mic, sizeof(mic), "%s/.mic.f32", audio_dir) >= (int)sizeof(mic) ||
        snprintf(sys, sizeof(sys), "%s/.sys.f32", audio_dir) >= (int)sizeof(sys) ||
        snprintf(markdown, sizeof(markdown), "%s/transcript.md", version) >= 512 ||
        snprintf(jsonl, sizeof(jsonl), "%s/transcript.jsonl", version_data) >= 1024) { rc = SQLITE_TOOBIG; goto done; }
    if (directory(audio_dir) || directory(version_data)) { rc = SQLITE_CANTOPEN; goto done; }
    int64_t samples = 0;
    code = "decode_failed";
    rc = decode(source.audio, mic, sys, cancelled, context, &samples, &code);
    if (rc) goto done;
    code = "storage_error";
    if (sync_directory(audio_dir) || sync_directory(version_data) || sync_directory(version) ||
        sync_directory(history) || sync_directory(data) || sync_directory(canonical) ||
        sync_directory(parent)) { rc = SQLITE_IOERR_FSYNC; goto done; }
    if (is_cancelled(cancelled, context)) { code = "cancelled"; rc = SQLITE_INTERRUPT; goto done; }
    metadata = version_metadata(db, &source, source_id, model, language, markdown);
    if (!metadata) { rc = SQLITE_ERROR; goto done; }
    rc = ascolto_db_begin(database);
    if (rc) goto done;
    transaction = true;
    rc = check_available(db, source_id, model, language, &code);
    if (rc) goto done;
    /* Recheck the authoritative archive reference after the long decode. */
    sqlite3_stmt *q = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT 1 FROM artifacts WHERE session_id=?1 AND kind='audio_m4a' AND state='ready' AND path=?2", -1, &q, NULL);
    if (!rc) {
        sqlite3_bind_int64(q, 1, source_id); bind_text(q, 2, source.audio);
        rc = sqlite3_step(q) == SQLITE_ROW ? SQLITE_OK : SQLITE_NOTFOUND;
    }
    sqlite3_finalize(q);
    if (rc) { code = "source_unavailable"; goto done; }
    int64_t session = 0, job = 0;
    ascolto_db_session_t values = {.started_at_ms = source.started, .capture_backend = source.backend,
        .call_title = source.title, .meeting_title = source.meeting, .source_app = source.app,
        .vocabulary_snapshot_json = source.vocabulary, .metadata_json = metadata};
    rc = ascolto_db_session_create(database, &values, &session);
    if (!rc) rc = ascolto_db_session_state(database, session, "completed", source.ended, NULL);
    ascolto_db_artifact_t artifact = {.session_id = session, .created_at_ms = ascolto_db_now_ms(), .state = "pending"};
    artifact.kind = "transcript_md"; artifact.path = markdown;
    if (!rc) rc = ascolto_db_artifact_put(database, &artifact);
    artifact.kind = "transcript_jsonl"; artifact.path = jsonl;
    if (!rc) rc = ascolto_db_artifact_put(database, &artifact);
    ascolto_deferred_enqueue_t queued = {.session_id = session, .sample_count = samples,
        .model_id = model, .language = language, .keywords = source.keywords,
        .mic_path = mic, .sys_path = sys, .metadata_json = metadata, .diarizer_threshold = source.threshold};
    if (!rc) rc = ascolto_deferred_enqueue(store, &queued, &job);
    if (!rc && is_cancelled(cancelled, context)) { code = "cancelled"; rc = SQLITE_INTERRUPT; }
    if (!rc) rc = ascolto_db_commit(database);
    if (!rc) { transaction = false; committed = true; *out_job_id = job; }
done:
    if (transaction) ascolto_db_rollback(ascolto_deferred_store_database(store));
    if (!committed && version[0]) {
        if (mic[0]) unlink(mic);
        if (sys[0]) unlink(sys);
        if (audio_dir[0]) rmdir(audio_dir);
        if (version_data[0]) rmdir(version_data);
        rmdir(version);
    }
    free(metadata); source_clear(&source);
    ascolto_deferred_store_close(store);
    if (lock_fd >= 0) close(lock_fd);
    return rc ? failure(error, error_cap, code, rc) : SQLITE_OK;
}
