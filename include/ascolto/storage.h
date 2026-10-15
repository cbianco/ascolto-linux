#ifndef ASCOLTO_STORAGE_H
#define ASCOLTO_STORAGE_H
#include "ascolto/database.h"
#include <stdbool.h>
typedef struct ascolto_pending_turn ascolto_pending_turn_t;
typedef struct {
  ascolto_pending_turn_t *head, *tail;
  int64_t session;
} ascolto_storage_writer_t;
/* Failed DB commits stay in this in-memory queue; publication failures use the
 * durable outbox. */
int ascolto_storage_write(const char *config, ascolto_storage_writer_t *writer,
                          const ascolto_db_utterance_t *turn);
int ascolto_storage_flush(const char *config, ascolto_storage_writer_t *writer);
/* All functions run before UI startup or on existing background action/consumer
 * workers. */
int ascolto_storage_open(const char *config, ascolto_db_t **out);
/* Restore archive shortcuts without creating a live session or changing
 * counters. */
int ascolto_storage_latest_transcript(const char *config, char *markdown,
                                      size_t markdown_cap, char *jsonl,
                                      size_t jsonl_cap);
int ascolto_storage_bootstrap(const char *config, const char *platform);
int ascolto_storage_get(ascolto_db_t *db, const char *key, char *out,
                        size_t cap);
int ascolto_storage_set(ascolto_db_t *db, const char *key, const char *value);
/* Empty or missing text keeps the caller's native default. */
void ascolto_storage_read_text(ascolto_db_t *db, const char *key, char *out,
                               size_t cap);
typedef struct { const char *key, *value; } ascolto_storage_setting_t;
/* The caller owns the transaction, including any keyword update. */
int ascolto_storage_set_many(ascolto_db_t *db,
                             const ascolto_storage_setting_t *settings,
                             size_t count);
char *ascolto_storage_vocabulary(const char *keywords);
int ascolto_storage_keywords(ascolto_db_t *db, char *out, size_t cap);
int ascolto_storage_set_keywords(ascolto_db_t *db, const char *text);
int ascolto_storage_session(const char *config,
                            const ascolto_db_session_t *values, const char *md,
                            const char *jsonl, int64_t *session);
int ascolto_storage_state(const char *config, int64_t session,
                          const char *state, const char *metadata_json);
int ascolto_storage_artifact(const char *config, int64_t session,
                             const char *kind, const char *path,
                             const char *state);
int ascolto_storage_archive(const char *config, const char *transcript,
                            const char *archive, const char *state);
int ascolto_storage_publish(ascolto_db_t *db, int64_t session);
#endif
