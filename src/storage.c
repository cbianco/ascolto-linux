#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#include "ascolto/storage.h"
#include "ascolto/ai_settings.h"
#include "ascolto/filesystem.h"
#include "ascolto/json.h"
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;

int ascolto_storage_open(const char *config, ascolto_db_t **out) {
  if (!config || !*config)
    return SQLITE_MISUSE;
  char path[2300], error[256];
  snprintf(path, sizeof(path), "%s%s", strchr(config, '/') ? "" : "./", config);
  snprintf(strrchr(path, '/') + 1,
           sizeof(path) - (size_t)(strrchr(path, '/') + 1 - path),
           "ascolto.sqlite3");
  ascolto_mkdir_parent(path);
  int result = ascolto_db_open(path, ASCOLTO_DATABASE_BUSY_TIMEOUT_MS, out,
                               error, sizeof(error));
  if (result)
    fprintf(stderr, "[storage] Cannot open authoritative database: %s\n",
            error);
  return result;
}
int ascolto_storage_bootstrap(const char *config, const char *platform) {
  char script[2300], python[2300];
  if (ascolto_ai_worker_paths("ascolto_runtime.py", script, sizeof(script),
                              python, sizeof(python)))
    return -1;
  char *args[] = {python,           "-B",           script,
                  "--config",       (char *)config, "--platform",
                  (char *)platform, "bootstrap",    NULL};
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                   O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                   O_WRONLY, 0);
  pid_t pid;
  int result = posix_spawnp(&pid, python, &actions, NULL, args, environ);
  posix_spawn_file_actions_destroy(&actions);
  if (result)
    return -1;
  int status;
  while (waitpid(pid, &status, 0) < 0)
    if (errno != EINTR)
      return -1;
  if (!WIFEXITED(status) || WEXITSTATUS(status)) {
    fprintf(stderr, "[storage] Legacy adoption/export recovery failed; "
                    "existing files retained.\n");
    return -1;
  }
  return 0;
}
int ascolto_storage_get(ascolto_db_t *db, const char *key, char *out,
                        size_t cap) {
  int rc = ascolto_db_setting_get(db, key, out, cap);
  if (rc != SQLITE_MISUSE)
    return rc;
  sqlite3_stmt *s = NULL;
  rc = sqlite3_prepare_v2(ascolto_db_sqlite(db),
                          "SELECT value FROM native_settings WHERE key=?", -1,
                          &s, NULL);
  if (!rc) {
    sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
      snprintf(out, cap, "%s", sqlite3_column_text(s, 0));
      rc = SQLITE_OK;
    } else if (rc == SQLITE_DONE)
      rc = SQLITE_NOTFOUND;
  }
  sqlite3_finalize(s);
  return rc;
}
int ascolto_storage_set(ascolto_db_t *db, const char *key, const char *value) {
  int rc = ascolto_db_setting_set(db, key, value);
  if (rc != SQLITE_MISUSE)
    return rc;
  sqlite3_stmt *s = NULL;
  rc = sqlite3_prepare_v2(ascolto_db_sqlite(db),
                          "INSERT INTO native_settings VALUES(?,?) ON "
                          "CONFLICT(key) DO UPDATE SET value=excluded.value",
                          -1, &s, NULL);
  if (!rc) {
    sqlite3_bind_text(s, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(s, 2, value, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(s) == SQLITE_DONE
             ? SQLITE_OK
             : sqlite3_errcode(ascolto_db_sqlite(db));
  }
  sqlite3_finalize(s);
  return rc;
}
void ascolto_storage_read_text(ascolto_db_t *db, const char *key, char *out,
                               size_t cap) {
  char value[2048];
  if (!ascolto_storage_get(db, key, value, sizeof(value)) && *value)
    snprintf(out, cap, "%s", value);
}
int ascolto_storage_set_many(ascolto_db_t *db,
                             const ascolto_storage_setting_t *settings,
                             size_t count) {
  for (size_t i = 0; i < count; i++) {
    int rc = ascolto_storage_set(db, settings[i].key, settings[i].value);
    if (rc)
      return rc;
  }
  return SQLITE_OK;
}
int ascolto_storage_keywords(ascolto_db_t *db, char *out, size_t cap) {
  sqlite3_stmt *s = NULL;
  out[0] = 0;
  int rc = sqlite3_prepare_v2(ascolto_db_sqlite(db),
                              "SELECT group_concat(text, ', ') FROM (SELECT "
                              "text FROM keywords WHERE enabled=1 ORDER BY id)",
                              -1, &s, NULL);
  if (!rc && sqlite3_step(s) == SQLITE_ROW) {
    const unsigned char *text = sqlite3_column_text(s, 0);
    snprintf(out, cap, "%s", text ? (const char *)text : "");
  }
  sqlite3_finalize(s);
  return rc;
}
int ascolto_storage_set_keywords(ascolto_db_t *db, const char *text) {
  int rc = sqlite3_exec(ascolto_db_sqlite(db), "DELETE FROM keywords", NULL,
                        NULL, NULL);
  if (rc)
    return rc;
  char *copy = strdup(text);
  if (!copy)
    return SQLITE_NOMEM;
  char *save = NULL;
  for (char *p = strtok_r(copy, ",\n\r", &save); p && !rc;
       p = strtok_r(NULL, ",\n\r", &save)) {
    while (*p == ' ' || *p == '\t')
      p++;
    size_t n = strlen(p);
    while (n && (p[n - 1] == ' ' || p[n - 1] == '\t'))
      p[--n] = 0;
    if (!n)
      continue;
    sqlite3_stmt *s = NULL;
    rc = sqlite3_prepare_v2(ascolto_db_sqlite(db),
                            "INSERT OR IGNORE INTO keywords(text) VALUES(?)",
                            -1, &s, NULL);
    if (!rc) {
      sqlite3_bind_text(s, 1, p, -1, SQLITE_TRANSIENT);
      rc = sqlite3_step(s) == SQLITE_DONE
               ? SQLITE_OK
               : sqlite3_errcode(ascolto_db_sqlite(db));
    }
    sqlite3_finalize(s);
  }
  free(copy);
  return rc;
}
static int absolute_path(const char *path, char *out, size_t cap) {
  char full[4096], parent[4096], resolved[4096];
  if (path[0] == '/')
    snprintf(full, sizeof(full), "%s", path);
  else {
    if (!getcwd(full, sizeof(full)))
      return -1;
    size_t n = strlen(full);
    if (snprintf(full + n, sizeof(full) - n, "/%s", path) >=
        (int)(sizeof(full) - n))
      return -1;
  }
  snprintf(parent, sizeof(parent), "%s", full);
  char *slash = strrchr(parent, '/');
  if (!slash)
    return -1;
  *slash = 0;
  if (!realpath(*parent ? parent : "/", resolved))
    return -1;
  return snprintf(out, cap, "%s/%s", resolved, strrchr(full, '/') + 1) >=
                 (int)cap
             ? -1
             : 0;
}
static int artifact(ascolto_db_t *db, int64_t session, const char *kind,
                    const char *path, const char *state) {
  char absolute[4096];
  if (absolute_path(path, absolute, sizeof(absolute)))
    return SQLITE_CANTOPEN;
  ascolto_db_artifact_t value = {.session_id = session,
                                 .kind = kind,
                                 .path = absolute,
                                 .state = state,
                                 .created_at_ms = ascolto_db_now_ms()};
  return ascolto_db_artifact_put(db, &value);
}
static int close_export(FILE *file, const char *temporary, const char *target) {
  int result = fflush(file);
  if (!result)
    result = fsync(fileno(file));
  if (fclose(file))
    result = -1;
  if (!result)
    result = rename(temporary, target);
  if (!result) {
    char parent[4096];
    snprintf(parent, sizeof(parent), "%s", target);
    char *slash = strrchr(parent, '/');
    if (slash) {
      *slash = 0;
      int d = open(parent, O_RDONLY);
      if (d < 0)
        result = -1;
      else {
        result = fsync(d);
        close(d);
      }
    }
  }
  if (result)
    unlink(temporary);
  return result;
}
static int query_step(sqlite3_stmt *query, bool allow_done) {
  int step = sqlite3_step(query);
  return step == SQLITE_ROW || (allow_done && step == SQLITE_DONE) ? step
                                                                   : -step;
}

int ascolto_storage_publish(ascolto_db_t *db, int64_t session) {
  sqlite3 *sql = ascolto_db_sqlite(db);
  int rc = ascolto_db_begin(db);
  if (rc)
    return rc;
  sqlite3_stmt *query = NULL;
  int64_t revision = 0;
  bool linux_format = false;
  char md[4096] = "", jsonl[4096] = "";
  rc = sqlite3_prepare_v2(
      sql, "SELECT revision FROM export_outbox WHERE session_id=?", -1, &query,
      NULL);
  if (!rc) {
    sqlite3_bind_int64(query, 1, session);
    int step = query_step(query, true);
    if (step == SQLITE_ROW)
      revision = sqlite3_column_int64(query, 0);
    else if (step < 0)
      rc = -step;
  }
  sqlite3_finalize(query);
  query = NULL;
  if (rc || !revision) {
    ascolto_db_rollback(db);
    return rc;
  }
  rc = sqlite3_prepare_v2(sql,
                          "SELECT kind,path FROM artifacts WHERE session_id=? "
                          "AND kind IN ('transcript_md','transcript_jsonl')",
                          -1, &query, NULL);
  if (!rc) {
    sqlite3_bind_int64(query, 1, session);
    int step;
    while ((step = query_step(query, true)) == SQLITE_ROW) {
      char *destination =
          !strcmp((const char *)sqlite3_column_text(query, 0), "transcript_md")
              ? md
              : jsonl;
      const char *path = (const char *)sqlite3_column_text(query, 1);
      if (!path || strlen(path) >= sizeof(md)) {
        rc = SQLITE_TOOBIG;
        break;
      }
      snprintf(destination, sizeof(md), "%s", path);
    }
    if (!rc && step < 0)
      rc = -step;
  }
  sqlite3_finalize(query);
  query = NULL;
  char mt[4200], jt[4200];
  snprintf(mt, sizeof(mt), "%s.XXXXXX", md);
  snprintf(jt, sizeof(jt), "%s.XXXXXX", jsonl);
  int mf = -1, jf = -1;
  if (!rc && *md && *jsonl) {
    mf = mkstemp(mt);
    jf = mkstemp(jt);
  }
  if (mf < 0 || jf < 0) {
    if (mf >= 0)
      close(mf);
    if (jf >= 0)
      close(jf);
    unlink(mt);
    unlink(jt);
    ascolto_db_rollback(db);
    return rc ? rc : SQLITE_IOERR;
  }
  FILE *markdown = fdopen(mf, "w"), *events = fdopen(jf, "w");
  if (!markdown || !events) {
    if (markdown)
      fclose(markdown);
    else
      close(mf);
    if (events)
      fclose(events);
    else
      close(jf);
    unlink(mt);
    unlink(jt);
    ascolto_db_rollback(db);
    return SQLITE_IOERR;
  }
  fprintf(markdown, "# Call transcript\n\n");
  rc = sqlite3_prepare_v2(
      sql,
      "SELECT "
      "json_patch(metadata_json,json_object('event','session_start','started_"
      "at_ms',started_at_ms,'window_title',coalesce(call_title,''),'calendar_"
      "title',coalesce(meeting_title,''))),capture_backend FROM sessions WHERE "
      "id=?",
      -1, &query, NULL);
  if (!rc) {
    sqlite3_bind_int64(query, 1, session);
    int step = query_step(query, false);
    if (step == SQLITE_ROW) {
      fprintf(events, "%s\n", sqlite3_column_text(query, 0));
      linux_format =
          !strcmp((const char *)sqlite3_column_text(query, 1), "pipewire");
    } else
      rc = step < 0 ? -step : SQLITE_NOTFOUND;
  }
  sqlite3_finalize(query);
  query = NULL;
  if (!rc)
    rc = sqlite3_prepare_v2(
        sql,
        "SELECT "
        "json_extract(metadata_json,'$.calendar_organizer'),json_extract("
        "metadata_json,'$.attendees'),json_extract(metadata_json,'$.calendar_"
        "notes'),call_title,meeting_title,source_app FROM sessions WHERE id=?",
        -1, &query, NULL);
  if (!rc) {
    sqlite3_bind_int64(query, 1, session);
    int step = query_step(query, false);
    if (step == SQLITE_ROW) {
      const char *labels[] = {"Organizer",        "Invited attendees",
                              "Agenda",           "Call title",
                              "Calendar meeting", "Input application"};
      for (int index = 0; index < 6; index++) {
        const char *value = (const char *)sqlite3_column_text(query, index);
        if (value && *value)
          fprintf(markdown, "- **%s**: %s\n", labels[index], value);
      }
      fprintf(markdown, "\n");
    } else
      rc = step < 0 ? -step : SQLITE_NOTFOUND;
  }
  sqlite3_finalize(query);
  query = NULL;
  if (!rc)
    rc = sqlite3_prepare_v2(
        sql,
        "SELECT "
        "u.start_ms,u.end_ms,coalesce(sp.label,u.channel),u.channel,u.text,"
        "json_patch(u.metadata_json,json_object('event','utterance','channel',"
        "u.channel,'speaker',coalesce(sp.label,u.channel),'start_s',u.start_ms/"
        "1000.0,'end_s',u.end_ms/1000.0,'start_sec',u.start_ms/"
        "1000.0,'end_sec',u.end_ms/"
        "1000.0,'text',u.text,'language',coalesce(u.language,''),'source_app',"
        "coalesce(u.source_app,''),'source_id','db:'||u.session_id||':'||u.id))"
        " FROM utterances u LEFT JOIN speakers sp "
        "ON sp.id=u.speaker_id WHERE u.session_id=? ORDER BY u.start_ms,u.id",
        -1, &query, NULL);
  if (!rc) {
    sqlite3_bind_int64(query, 1, session);
    int step;
    while ((step = query_step(query, true)) == SQLITE_ROW) {
      int start = sqlite3_column_int(query, 0) / 1000,
          end = sqlite3_column_int(query, 1) / 1000;
      if (linux_format)
        fprintf(markdown, "**[%02d:%02d - %02d:%02d] [%s]**: %s\n\n",
                start / 60, start % 60, end / 60, end % 60,
                sqlite3_column_text(query, 2), sqlite3_column_text(query, 4));
      else
        fprintf(markdown,
                "- **[%02d:%02d - %02d:%02d] %s** *(channel %s)*: %s\n",
                start / 60, start % 60, end / 60, end % 60,
                sqlite3_column_text(query, 2), sqlite3_column_text(query, 3),
                sqlite3_column_text(query, 4));
      fprintf(events, "%s\n", sqlite3_column_text(query, 5));
    }
    if (step < 0)
      rc = -step;
  }
  sqlite3_finalize(query);
  query = NULL;
  if (!rc)
    rc = sqlite3_prepare_v2(
        sql,
        "SELECT json_patch(metadata_json,json_object('event','session_end')) "
        "FROM sessions WHERE id=? AND state IN ('completed','failed')",
        -1, &query, NULL);
  if (!rc) {
    sqlite3_bind_int64(query, 1, session);
    int step = query_step(query, true);
    if (step == SQLITE_ROW) {
      fprintf(events, "%s\n", sqlite3_column_text(query, 0));
    } else if (step < 0)
      rc = -step;
  }
  sqlite3_finalize(query);
  if (ferror(markdown) || ferror(events))
    rc = SQLITE_IOERR;
  if (rc) {
    fclose(markdown);
    fclose(events);
    unlink(mt);
    unlink(jt);
  } else {
    if (close_export(markdown, mt, md))
      rc = SQLITE_IOERR;
    if (close_export(events, jt, jsonl))
      rc = SQLITE_IOERR;
  }
  if (!rc)
    rc = artifact(db, session, "transcript_md", md, "ready");
  if (!rc)
    rc = artifact(db, session, "transcript_jsonl", jsonl, "ready");
  if (!rc)
    rc = ascolto_db_export_ack(db, session, revision);
  if (!rc)
    rc = ascolto_db_commit(db);
  if (rc)
    ascolto_db_rollback(db);
  return rc;
}
int ascolto_storage_session(const char *config,
                            const ascolto_db_session_t *values, const char *md,
                            const char *jsonl, int64_t *session) {
  ascolto_db_t *db = NULL;
  int rc = ascolto_storage_open(config, &db);
  if (rc)
    return rc;
  rc = ascolto_db_begin(db);
  if (!rc)
    rc = ascolto_db_session_create(db, values, session);
  if (!rc)
    rc = artifact(db, *session, "transcript_md", md, "pending");
  if (!rc)
    rc = artifact(db, *session, "transcript_jsonl", jsonl, "pending");
  if (!rc)
    rc = ascolto_db_commit(db);
  if (rc)
    ascolto_db_rollback(db);
  if (!rc)
    rc = ascolto_storage_publish(db, *session);
  ascolto_db_close(db);
  return rc;
}
int ascolto_storage_state(const char *config, int64_t session,
                          const char *state, const char *metadata) {
  if (!session)
    return SQLITE_MISUSE;
  ascolto_db_t *db = NULL;
  int rc = ascolto_storage_open(config, &db);
  if (rc)
    return rc;
  rc = ascolto_db_session_state(
      db, session, state,
      (!strcmp(state, "completed") || !strcmp(state, "failed"))
          ? ascolto_db_now_ms()
          : 0, metadata);
  if (!rc)
    rc = ascolto_storage_publish(db, session);
  ascolto_db_close(db);
  return rc;
}
int ascolto_storage_artifact(const char *config, int64_t session,
                             const char *kind, const char *path,
                             const char *state) {
  ascolto_db_t *db = NULL;
  int rc = ascolto_storage_open(config, &db);
  if (!rc)
    rc = artifact(db, session, kind, path, state);
  ascolto_db_close(db);
  return rc;
}

int ascolto_storage_archive(const char *config, const char *transcript,
                            const char *archive, const char *state) {
  ascolto_db_t *db = NULL;
  int rc = ascolto_storage_open(config, &db);
  if (rc)
    return rc;
  sqlite3_stmt *q = NULL;
  rc = sqlite3_prepare_v2(
      ascolto_db_sqlite(db),
      "SELECT session_id FROM artifacts WHERE path=? AND kind='transcript_md'",
      -1, &q, NULL);
  int64_t session = 0;
  if (!rc) {
    char normalized[4096];
    if (absolute_path(transcript, normalized, sizeof(normalized))) {
      ascolto_db_close(db);
      sqlite3_finalize(q);
      return SQLITE_NOTFOUND;
    }
    sqlite3_bind_text(q, 1, normalized, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(q);
    if (step == SQLITE_ROW)
      session = sqlite3_column_int64(q, 0);
    else
      rc = step == SQLITE_DONE ? SQLITE_NOTFOUND : step;
  }
  sqlite3_finalize(q);
  if (!rc && session)
    rc = artifact(db, session, "audio_m4a", archive, state);
  ascolto_db_close(db);
  return rc;
}

struct ascolto_pending_turn {
  ascolto_db_utterance_t value;
  struct ascolto_pending_turn *next;
  char strings[];
};
int ascolto_storage_flush(const char *config,
                          ascolto_storage_writer_t *writer) {
  if (!writer->head && !writer->session)
    return 0;
  ascolto_db_t *db = NULL;
  int rc = ascolto_storage_open(config, &db);
  if (rc)
    return -rc;
  while (writer->head) {
    ascolto_pending_turn_t *next = writer->head;
    rc = ascolto_db_utterance_append(db, &next->value);
    if (rc) {
      ascolto_db_close(db);
      return -rc;
    }
    writer->head = next->next;
    if (!writer->head)
      writer->tail = NULL;
    free(next);
  }
  rc = ascolto_storage_publish(db, writer->session);
  ascolto_db_close(db);
  return rc;
}
int ascolto_storage_write(const char *config, ascolto_storage_writer_t *writer,
                          const ascolto_db_utterance_t *turn) {
  const char *values[] = {turn->channel, turn->speaker_label, turn->text,
                         turn->language, turn->source_app, turn->metadata_json};
  size_t size = sizeof(ascolto_pending_turn_t);
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++)
    if (values[i])
      size += strlen(values[i]) + 1;
  ascolto_pending_turn_t *next = calloc(1, size);
  if (!next)
    return -SQLITE_NOMEM;
  next->value = *turn;
  const char **fields[] = {&next->value.channel, &next->value.speaker_label,
                          &next->value.text, &next->value.language,
                          &next->value.source_app, &next->value.metadata_json};
  char *destination = next->strings;
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    if (!values[i])
      continue;
    size_t length = strlen(values[i]) + 1;
    memcpy(destination, values[i], length);
    *fields[i] = destination;
    destination += length;
  }
  if (writer->tail)
    writer->tail->next = next;
  else
    writer->head = next;
  writer->tail = next;
  writer->session = turn->session_id;
  return ascolto_storage_flush(config, writer);
}

char *ascolto_storage_vocabulary(const char *keywords) {
  char *copy = strdup(keywords ? keywords : ""), *json = NULL;
  size_t length = 0;
  if (!copy)
    return NULL;
  FILE *out = open_memstream(&json, &length);
  if (!out) {
    free(copy);
    return NULL;
  }
  fputc('[', out);
  bool first = true;
  char *state = NULL;
  for (char *word = strtok_r(copy, ",\n\r", &state); word;
       word = strtok_r(NULL, ",\n\r", &state)) {
    while (*word == ' ' || *word == '\t')
      word++;
    size_t size = strlen(word);
    while (size && (word[size - 1] == ' ' || word[size - 1] == '\t'))
      word[--size] = 0;
    if (!size)
      continue;
    if (!first)
      fputc(',', out);
    first = false;
    ascolto_write_json_string(out, word);
  }
  fputc(']', out);
  fclose(out);
  free(copy);
  return json;
}

static bool readable_regular_file(const char *path) {
  struct stat info;
  return path && !stat(path, &info) && S_ISREG(info.st_mode) &&
         !access(path, R_OK);
}

int ascolto_storage_latest_transcript(const char *config, char *markdown,
                                      size_t markdown_cap, char *jsonl,
                                      size_t jsonl_cap) {
  if (!markdown || !markdown_cap || !jsonl || !jsonl_cap)
    return SQLITE_MISUSE;
  markdown[0] = jsonl[0] = 0;
  ascolto_db_t *db = NULL;
  int rc = ascolto_storage_open(config, &db);
  if (rc)
    return rc;
  sqlite3_stmt *query = NULL;
  rc = sqlite3_prepare_v2(
      ascolto_db_sqlite(db),
      "SELECT a.path,(SELECT j.path FROM artifacts j WHERE "
      "j.session_id=a.session_id AND j.kind='transcript_jsonl' AND "
      "j.state='ready' ORDER BY j.created_at_ms DESC,j.id DESC LIMIT 1) "
      "FROM artifacts a JOIN sessions s ON s.id=a.session_id "
      "WHERE a.kind='transcript_md' AND a.state='ready' "
      "ORDER BY s.started_at_ms DESC,s.id DESC,a.created_at_ms DESC,a.id DESC",
      -1, &query, NULL);
  if (!rc) {
    int step;
    while ((step = sqlite3_step(query)) == SQLITE_ROW) {
      const char *md = (const char *)sqlite3_column_text(query, 0);
      const char *trace = (const char *)sqlite3_column_text(query, 1);
      if (!readable_regular_file(md))
        continue;
      if (strlen(md) >= markdown_cap ||
          (readable_regular_file(trace) && strlen(trace) >= jsonl_cap)) {
        rc = SQLITE_TOOBIG;
        break;
      }
      snprintf(markdown, markdown_cap, "%s", md);
      if (readable_regular_file(trace))
        snprintf(jsonl, jsonl_cap, "%s", trace);
      break;
    }
    if (step != SQLITE_ROW && step != SQLITE_DONE)
      rc = step;
  }
  sqlite3_finalize(query);
  ascolto_db_close(db);
  return rc;
}
