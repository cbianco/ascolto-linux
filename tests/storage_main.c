#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "ascolto/storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static int failed, checks;
#define CHECK(value)                                                           \
  do {                                                                         \
    checks++;                                                                  \
    if (!(value)) {                                                            \
      fprintf(stderr, "Storage check failed at line %d: %s\n", __LINE__,       \
              #value);                                                         \
      failed++;                                                                \
    }                                                                          \
  } while (0)
static int count(ascolto_db_t *db, const char *query) {
  sqlite3_stmt *s = NULL;
  int n = -1;
  if (!sqlite3_prepare_v2(ascolto_db_sqlite(db), query, -1, &s, NULL) &&
      sqlite3_step(s) == SQLITE_ROW)
    n = sqlite3_column_int(s, 0);
  sqlite3_finalize(s);
  return n;
}
static char *contents(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long size = ftell(f);
  rewind(f);
  char *data = calloc(1, (size_t)size + 1);
  if (data)
    fread(data, 1, (size_t)size, f);
  fclose(f);
  return data;
}
int main(void) {
  char *vocabulary =
      ascolto_storage_vocabulary(" Alpha, Beta\nName with spaces ");
  CHECK(vocabulary &&
        !strcmp(vocabulary, "[\"Alpha\",\"Beta\",\"Name with spaces\"]"));
  free(vocabulary);
  char directory[] = "/tmp/ascolto-storage-XXXXXX";
  if (!mkdtemp(directory))
    return 1;
  char call[1024], child[1100];
  CHECK(!ascolto_storage_call_directory(directory, "../Team / Sync!", call, sizeof(call)));
  CHECK(strstr(call, "_team-sync") != NULL);
  snprintf(child,sizeof(child),"%s/Data",call);
  struct stat folder_mode;
  CHECK(!stat(child,&folder_mode) && S_ISDIR(folder_mode.st_mode) && (folder_mode.st_mode&0777)==0700);
  char short_path[4];
  CHECK(ascolto_storage_call_directory(directory,"Call",short_path,sizeof(short_path)) != 0);
  char config[1024], md[1024], jsonl[1024], audio[1024];
  snprintf(config, sizeof(config), "%s/config.json", directory);
  snprintf(md, sizeof(md), "%s/Ω's call.md", directory);
  snprintf(jsonl, sizeof(jsonl), "%s/Ω's call.jsonl", directory);
  snprintf(audio, sizeof(audio), "%s/Ω's call.m4a", directory);
  ascolto_db_session_t values = {.started_at_ms = ascolto_db_now_ms(),
                                 .capture_backend = "coreaudio",
                                 .call_title = "Client's Ω",
                                 .source_app = "Fixture",
                                 .vocabulary_snapshot_json = "[\"Whistle\"]"};
  int64_t session = 0;
  CHECK(!ascolto_storage_session(config, &values, md, jsonl, &session));
  CHECK(session > 0);
  ascolto_db_t *db = NULL;
  CHECK(!ascolto_storage_open(config, &db));
  CHECK(count(db, "SELECT count(*) FROM export_outbox") == 0);
  ascolto_storage_writer_t writer = {0};
  ascolto_db_utterance_t turn = {.session_id = session,
                                 .start_ms = 100,
                                 .end_ms = 900,
                                 .channel = "MIC",
                                 .speaker_label = "ME",
                                 .text = "Unicode Ω and apostrophe's text",
                                 .language = "en",
                                 .source_app = "App's name"};
  turn.metadata_json = "{\"ttft_ms\":123,\"rms_db\":-27}";
  CHECK(!ascolto_storage_write(config, &writer, &turn));
  char *text = contents(jsonl);
  CHECK(text && strstr(text, "Unicode Ω") && strstr(text, "\"ttft_ms\":123"));
  free(text);
  CHECK(!sqlite3_exec(
      ascolto_db_sqlite(db),
      "CREATE TRIGGER fail_transcript BEFORE INSERT ON utterances BEGIN SELECT "
      "RAISE(ABORT,'fixture_failure'); END",
      NULL, NULL, NULL));
  char queued_text[] = "Retained queued phrase";
  char queued_metadata[] = "{\"ttft_ms\":456}";
  turn.text = queued_text;
  turn.metadata_json = queued_metadata;
  turn.start_ms = 1000;
  turn.end_ms = 1800;
  CHECK(ascolto_storage_write(config, &writer, &turn) < 0);
  CHECK(writer.head != NULL);
  memset(queued_text, 'x', strlen(queued_text));
  memset(queued_metadata, 'x', strlen(queued_metadata));
  CHECK(count(db, "SELECT count(*) FROM utterances") == 1);
  text = contents(jsonl);
  CHECK(text && !strstr(text, "Retained queued phrase"));
  free(text);
  CHECK(!sqlite3_exec(ascolto_db_sqlite(db), "DROP TRIGGER fail_transcript",
                      NULL, NULL, NULL));
  CHECK(!ascolto_storage_flush(config, &writer));
  CHECK(writer.head == NULL);
  CHECK(count(db, "SELECT count(*) FROM utterances") == 2);
  text = contents(jsonl);
  CHECK(text && strstr(text, "Retained queued phrase") &&
        strstr(text, "\"ttft_ms\":456"));
  free(text);
  /* Fail the second atomic publication after the first path was replaced. */
  CHECK(!unlink(jsonl));
  CHECK(!mkdir(jsonl, 0700));
  turn.text = "Durable outbox phrase";
  turn.metadata_json = NULL;
  turn.start_ms = 2000;
  turn.end_ms = 2800;
  CHECK(ascolto_storage_write(config, &writer, &turn) > 0);
  CHECK(writer.head == NULL);
  CHECK(count(db, "SELECT count(*) FROM utterances") == 3);
  CHECK(count(db, "SELECT count(*) FROM export_outbox") == 1);
  CHECK(!rmdir(jsonl));
  CHECK(!ascolto_storage_flush(config, &writer));
  CHECK(count(db, "SELECT count(*) FROM export_outbox") == 0);
  CHECK(
      !ascolto_storage_state(config, session, "completed",
                             "{\"duration_s\":3.8,\"calendar_notes\":\"Agenda "
                             "Ω\",\"capture_dropped_samples\":60800}"));
  text = contents(jsonl);
  CHECK(text && strstr(text, "\"duration_s\":3.8") &&
        strstr(text, "Agenda Ω") && strstr(text, "Durable outbox phrase"));
  free(text);
  CHECK(!ascolto_storage_archive(config, md, audio, "ready"));
  CHECK(count(db, "SELECT count(*) FROM artifacts WHERE kind='audio_m4a' AND "
                  "state='ready'") == 1);
  char restored_md[1024], restored_jsonl[1024];
  CHECK(!ascolto_storage_latest_transcript(config, restored_md,
                                           sizeof(restored_md), restored_jsonl,
                                           sizeof(restored_jsonl)));
  char canonical_md[4096], canonical_jsonl[4096];
  CHECK(realpath(md, canonical_md) && realpath(jsonl, canonical_jsonl));
  CHECK(!strcmp(restored_md, canonical_md) &&
        !strcmp(restored_jsonl, canonical_jsonl));
  CHECK(count(db, "SELECT count(*) FROM sessions") == 1);
  CHECK(!sqlite3_exec(
      ascolto_db_sqlite(db),
      "UPDATE artifacts SET state='pending' WHERE kind='transcript_md'", NULL,
      NULL, NULL));
  CHECK(!ascolto_storage_latest_transcript(config, restored_md,
                                           sizeof(restored_md), restored_jsonl,
                                           sizeof(restored_jsonl)));
  CHECK(!restored_md[0] && !restored_jsonl[0]);
  CHECK(!sqlite3_exec(
      ascolto_db_sqlite(db),
      "UPDATE artifacts SET state='ready' WHERE kind='transcript_md'", NULL,
      NULL, NULL));
  ascolto_db_close(db);
  char cwd[4096];
  CHECK(getcwd(cwd, sizeof(cwd)) != NULL);
  CHECK(!chdir(directory));
  CHECK(!ascolto_storage_open("local.json", &db));
  ascolto_db_close(db);
  CHECK(!chdir(cwd));
  printf("Native storage lifecycle/commit queue/export recovery: %d checks, %d "
         "failures\n",
         checks, failed);
  return failed ? 1 : 0;
}
