#define _POSIX_C_SOURCE 200809L
#include "ascolto/ai_settings.h"
#include "ascolto/storage.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char **environ;

int ascolto_ai_worker_paths(const char *name, char *script, size_t sc,
                            char *python, size_t pc) {
  char executable[2048];
#ifdef __APPLE__
  uint32_t capacity = sizeof(executable);
  if (_NSGetExecutablePath(executable, &capacity))
    return -1;
#else
  ssize_t length =
      readlink("/proc/self/exe", executable, sizeof(executable) - 1);
  if (length < 0)
    return -1;
  executable[length] = 0;
#endif
  char *slash = strrchr(executable, '/');
  if (!slash)
    return -1;
  *slash = 0;
  const char *locations[] = {"/../Resources", "/../scripts"};
  bool found = false;
  for (unsigned i = 0; i < 2; i++) {
    snprintf(script, sc, "%s%s/%s", executable, locations[i], name);
    if (!access(script, R_OK)) {
      found = true;
      break;
    }
  }
  if (!found)
    return -1;
  const char *runtimes[] = {"/../Resources/python/bin/python3",
                            "/../.venv/bin/python3"};
  snprintf(python, pc, "python3");
  for (unsigned i = 0; i < sizeof(runtimes) / sizeof(runtimes[0]); i++) {
    char candidate[2300];
    snprintf(candidate, sizeof(candidate), "%s%s", executable, runtimes[i]);
    if (!access(candidate, X_OK)) {
      snprintf(python, pc, "%s", candidate);
      break;
    }
  }
  return 0;
}

static bool task_enabled(const char *config, bool audio) {
  ascolto_db_t *db = NULL;
  if (ascolto_storage_open(config, &db))
    return false;
  sqlite3_stmt *query = NULL;
  bool enabled = false;
  const char *sql =
      audio ? "SELECT enabled AND cloud_audio_opt_in FROM "
              "transcription_preferences WHERE id=1"
            : "SELECT auto_generate FROM recap_preferences WHERE id=1";
  if (!sqlite3_prepare_v2(ascolto_db_sqlite(db), sql, -1, &query, NULL) &&
      sqlite3_step(query) == SQLITE_ROW)
    enabled = sqlite3_column_int(query, 0) != 0;
  sqlite3_finalize(query);
  ascolto_db_close(db);
  return enabled;
}
bool ascolto_ai_automatic_enabled(const char *config) {
  return task_enabled(config, false) || task_enabled(config, true);
}

static void *reap_ai_worker(void *argument) {
  pid_t child = *(pid_t *)argument;
  free(argument);
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
  }
  return NULL;
}

int ascolto_ai_task_start(const char *config, const char *transcript,
                          const char *task) {
  if (!config || !transcript || !*transcript)
    return -1;
  if (!strcmp(task, "automatic") && !task_enabled(config, false))
    return 0;
  if (!strcmp(task, "automatic_audio") && !task_enabled(config, true))
    return 0;
  char script[2300], python[2300], log_path[2300];
  if (ascolto_ai_worker_paths("ascolto_runtime.py", script, sizeof(script),
                              python, sizeof(python)))
    return -1;
  snprintf(log_path, sizeof(log_path), "%s%s", strchr(config, '/') ? "" : "./",
           config);
  char *slash = strrchr(log_path, '/');
  if (!slash)
    return -1;
  snprintf(slash + 1, sizeof(log_path) - (size_t)(slash + 1 - log_path),
           "ai-recap.log");
  char *arguments[] = {"env",
                       "PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin",
                       "PYTHONDONTWRITEBYTECODE=1",
                       python,
                       "-B",
                       script,
                       "--transcript",
                       (char *)transcript,
                       "--config",
                       (char *)config,
                       (char *)task,
                       NULL};
  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                   O_RDONLY, 0);
  posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_path,
                                   O_WRONLY | O_CREAT | O_APPEND, 0600);
  posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
  pid_t *child = malloc(sizeof(*child));
  if (!child) {
    posix_spawn_file_actions_destroy(&actions);
    return -1;
  }
  int result =
      posix_spawn(child, "/usr/bin/env", &actions, NULL, arguments, environ);
  posix_spawn_file_actions_destroy(&actions);
  if (result) {
    free(child);
    return -1;
  }
  pthread_t thread;
  if (pthread_create(&thread, NULL, reap_ai_worker, child)) {
    /* An unreaped short-lived worker is preferable to blocking session
     * shutdown. */
    free(child);
    return -1;
  }
  pthread_detach(thread);
  return 0;
}

int ascolto_ai_recap_start(const char *config, const char *transcript,
                           bool automatic) {
  return ascolto_ai_task_start(config, transcript,
                               automatic ? "automatic" : "recap");
}
