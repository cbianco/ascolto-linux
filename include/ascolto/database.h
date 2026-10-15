#ifndef ASCOLTO_DATABASE_H
#define ASCOLTO_DATABASE_H
#include <stddef.h>
#include <stdint.h>
#include <sqlite3.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ASCOLTO_DATABASE_VERSION 1
#define ASCOLTO_DATABASE_APPLICATION_ID 0x4153434c
#define ASCOLTO_DATABASE_BUSY_TIMEOUT_MS 1500

typedef struct ascolto_db ascolto_db_t;
typedef struct {
    int64_t started_at_ms;
    const char *capture_backend, *call_title, *meeting_title, *source_app, *vocabulary_snapshot_json, *metadata_json;
} ascolto_db_session_t;
typedef struct {
    int64_t session_id, start_ms, end_ms;
    const char *channel, *speaker_label, *text, *language, *source_app, *metadata_json;
} ascolto_db_utterance_t;
typedef struct {
    int64_t session_id, created_at_ms;
    const char *kind, *path, *state;
} ascolto_db_artifact_t;

/* A handle belongs to one background worker. Use separate handles for concurrent workers.
 * Parent directories must exist. Returns SQLite result codes; never resets an existing DB.
 * No function stores credentials or deletes filesystem resources. NULL/zero IDs mean absent. */
int ascolto_db_open(const char *path, int busy_timeout_ms, ascolto_db_t **out, char *error, size_t error_cap);
void ascolto_db_close(ascolto_db_t *db);
sqlite3 *ascolto_db_sqlite(ascolto_db_t *db);
int ascolto_db_begin(ascolto_db_t *db);
int ascolto_db_commit(ascolto_db_t *db);
int ascolto_db_rollback(ascolto_db_t *db);
int ascolto_db_session_create(ascolto_db_t *db, const ascolto_db_session_t *session, int64_t *id);
int ascolto_db_session_state(ascolto_db_t *db, int64_t id, const char *state, int64_t ended_at_ms, const char *metadata_json);
/* Speaker insertion, utterance insertion and durable export request are one transaction. */
int ascolto_db_utterance_append(ascolto_db_t *db, const ascolto_db_utterance_t *utterance);
int ascolto_db_artifact_put(ascolto_db_t *db, const ascolto_db_artifact_t *artifact);
/* AI connection/catalog/preference/job mutations belong to the Python worker. */
/* Only known app_settings columns are accepted. Stable oauth_host_id is readable, never rewritten here. */
int ascolto_db_setting_get(ascolto_db_t *db, const char *key, char *out, size_t cap);
int ascolto_db_setting_set(ascolto_db_t *db, const char *key, const char *value);
/* A publisher may acknowledge only the exact revision it rendered, after atomic file publication. */
int ascolto_db_export_ack(ascolto_db_t *db, int64_t session_id, int64_t revision);
int64_t ascolto_db_now_ms(void);
#ifdef __cplusplus
}
#endif
#endif
