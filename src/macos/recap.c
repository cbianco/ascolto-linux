#include "ascolto/ai_settings.h"
#include "ascolto/storage.h"
#include "ascolto.h"
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;
static pthread_mutex_t archive_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t archive_condition = PTHREAD_COND_INITIALIZER;
static unsigned archive_jobs;

void ascolto_archive_wait(void) {
    pthread_mutex_lock(&archive_mutex);
    while (archive_jobs) pthread_cond_wait(&archive_condition, &archive_mutex);
    pthread_mutex_unlock(&archive_mutex);
}

/* No shell expansion: artifact paths and vocabulary are passed as argv values. */
static int spawn_command(char *const arguments[], const char *log_path, pid_t *pid) {
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log_path ? log_path : "/dev/null",
                                   O_WRONLY | O_CREAT | O_APPEND, 0600);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    int result = posix_spawn(pid, "/usr/bin/env", &actions, NULL, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    return result;
}

static int wait_command(pid_t pid) {
    int status;
    while (waitpid(pid, &status, 0) < 0) { if (errno != EINTR) return -1; }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void *reap(void *argument) {
    pid_t pid = *(pid_t *)argument;
    free(argument);
    int result = wait_command(pid);
    ascolto_diagnostic("Recap process completed result=%d", result);
    return NULL;
}

int ascolto_recap_start(const ascolto_config_t *config, const char *md_path, bool mp3_only, bool prepare_only) {
    char archive[1024], script[2300], interpreter[2300], log_path[1024], keywords[2048];
    if (!config || !md_path || !md_path[0] || strlen(md_path) >= sizeof(archive) - 4) return -1;
    snprintf(archive, sizeof(archive), "%s", md_path);
    char *suffix = strrchr(archive, '.');
    if (!suffix || strcmp(suffix, ".md")) return -1;
    snprintf(suffix, sizeof(archive) - (size_t)(suffix - archive), ".m4a");
    if ((!mp3_only && access(archive, R_OK)) || ascolto_ai_worker_paths("recap.py", script, sizeof(script), interpreter, sizeof(interpreter))) return -1;
    snprintf(log_path, sizeof(log_path), "%s", config->config_path);
    char *slash = strrchr(log_path, '/');
    if (slash) snprintf(slash + 1, sizeof(log_path) - (size_t)(slash + 1 - log_path), "recap.log");
    else snprintf(log_path, sizeof(log_path), "recap.log");
    ascolto_config_load_keywords(config, keywords, sizeof(keywords));
    if (strcmp(config->recap_python, "python3"))
        snprintf(interpreter, sizeof(interpreter), "%s", config->recap_python);
    char environment_path[4096];
    snprintf(environment_path, sizeof(environment_path), "PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:%s", getenv("PATH") ?: "");
    char *arguments[24] = {"env", environment_path, "PYTHONDONTWRITEBYTECODE=1", interpreter, "-B",
        script, "--archive", archive, "--language", (char *)config->language, "--keywords", keywords, NULL};
    int next = 12;
    if (mp3_only) arguments[next++] = "--mp3-only";
    if (prepare_only) arguments[next++] = "--prepare-only";
    if (config->recap_tts) arguments[next++] = "--tts";
    arguments[next] = NULL;
    pid_t *pid = malloc(sizeof(*pid));
    if (!pid) return -1;
    int result = spawn_command(arguments, log_path, pid);
    if (result) { free(pid); return -1; }
    pthread_t thread;
    if (pthread_create(&thread, NULL, reap, pid)) { wait_command(*pid); free(pid); return -1; }
    pthread_detach(thread);
    return 0;
}

typedef struct {
    ascolto_config_t config;
    char mic[512], sys[512], archive[512], md[512];
} archive_job_t;

static void *finalize_archive(void *argument) {
    archive_job_t *job = argument;
    /* Publish the archive only after ffmpeg exits, so manual recap cannot read a partial file. */
    char temporary[600];
    snprintf(temporary, sizeof(temporary), "%s.partial.m4a", job->archive);
    char *arguments[] = {"env", "PATH=/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin", "ffmpeg",
        "-nostdin", "-y", "-v", "error", "-f", "f32le", "-ar", "16000", "-ac", "1", "-i", job->mic,
        "-f", "f32le", "-ar", "16000", "-ac", "1", "-i", job->sys,
        "-filter_complex", "[0:a][1:a]amerge=inputs=2[aout]", "-map", "[aout]",
        "-c:a", "aac", "-b:a", "48k", temporary, NULL};
    pid_t pid;
    int result = spawn_command(arguments, NULL, &pid);
    if (!result) result = wait_command(pid);
    if (!result) result = rename(temporary, job->archive);
    if (!result) {
        chmod(job->archive, 0600);
        result=ascolto_storage_archive(job->config.config_path,job->md,job->archive,"ready");
        if(!result){
            unlink(job->mic);unlink(job->sys);
            ascolto_ai_task_start(job->config.config_path,job->md,"automatic_audio");
        }
        if (!result && job->config.cloud_recap && !ascolto_ai_automatic_enabled(job->config.config_path)) result = ascolto_recap_start(&job->config, job->md, false, false);
    } else { unlink(temporary); ascolto_storage_archive(job->config.config_path,job->md,job->archive,"failed"); }
    ascolto_diagnostic("Archive finalization completed result=%d", result);
    free(job);
    pthread_mutex_lock(&archive_mutex);
    archive_jobs--;
    pthread_cond_broadcast(&archive_condition);
    pthread_mutex_unlock(&archive_mutex);
    return NULL;
}

int ascolto_archive_finalize(const ascolto_config_t *config, const char *mic, const char *sys,
                            const char *archive, const char *md) {
    archive_job_t *job = calloc(1, sizeof(*job));
    if (!job) return -1;
    job->config = *config;
    snprintf(job->mic, sizeof(job->mic), "%s", mic);
    snprintf(job->sys, sizeof(job->sys), "%s", sys);
    snprintf(job->archive, sizeof(job->archive), "%s", archive);
    snprintf(job->md, sizeof(job->md), "%s", md);
    pthread_t thread;
    pthread_mutex_lock(&archive_mutex);
    archive_jobs++;
    pthread_mutex_unlock(&archive_mutex);
    if (pthread_create(&thread, NULL, finalize_archive, job)) {
        free(job);
        pthread_mutex_lock(&archive_mutex);
        archive_jobs--;
        pthread_cond_broadcast(&archive_condition);
        pthread_mutex_unlock(&archive_mutex);
        return -1;
    }
    pthread_detach(thread);
    return 0;
}
