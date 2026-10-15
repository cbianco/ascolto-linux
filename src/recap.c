#define _POSIX_C_SOURCE 200809L
#include "ascolto/core.h"
#include <fcntl.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *recap_language_name(const char *code) {
    static const struct { const char *code; const char *name; } map[] = {
        {"en", "English"}, {"es", "Spanish"}, {"fr", "French"}, {"de", "German"},
        {"nl", "Dutch"}, {"pl", "Polish"}
    };
    for (size_t i = 0; code && i < sizeof(map) / sizeof(map[0]); i++) {
        if (strcmp(code, map[i].code) == 0) return map[i].name;
    }
    return "Italian";
}

static int spawn_recap(ascolto_core_t *core, const char *md_path, bool mp3_only) {
    if (!core) return -1;
    char dir[512], lang[16], recap_lang[8], tts_lang[8], keywords[1024];
    bool tts;

    pthread_mutex_lock(&core->mutex);
    snprintf(dir, sizeof(dir), "%s", (md_path && *md_path) ? md_path : core->current_md_path);
    snprintf(lang, sizeof(lang), "%s", core->config.language);
    snprintf(recap_lang, sizeof(recap_lang), "%s", core->config.recap_lang);
    snprintf(tts_lang, sizeof(tts_lang), "%s", core->config.tts_lang);
    snprintf(keywords, sizeof(keywords), "%s", core->config.keywords);
    tts = core->config.recap_tts;
    pthread_mutex_unlock(&core->mutex);

    /* Only per-call folders are supported: <dir>/transcript.md */
    const char *suffix = "/transcript.md";
    size_t len = strlen(dir), slen = strlen(suffix);
    if (len <= slen || strcmp(dir + len - slen, suffix) != 0) return -1;
    dir[len - slen] = '\0';

    char exe[512];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return -1;
    exe[n] = '\0';
    char exe_copy[512];
    snprintf(exe_copy, sizeof(exe_copy), "%s", exe);
    char script[640], flac2wav[640], python[640];
    snprintf(script, sizeof(script), "%s/../scripts/recap.py", dirname(exe));
    snprintf(python, sizeof(python), "%s/../.venv/bin/python3", dirname(exe));
    if (access(python, X_OK)) snprintf(python, sizeof(python), "python3");
    snprintf(flac2wav, sizeof(flac2wav), "%s/flac2wav", dirname(exe_copy));

    const char *home = getenv("HOME");
    char log_path[640];
    snprintf(log_path, sizeof(log_path), "%s/.local/share/ascolto/recap.log", (home && *home) ? home : "/tmp");

    char recap_name[32], tts_name[32];
    snprintf(recap_name, sizeof(recap_name), "%s", recap_language_name(recap_lang));
    snprintf(tts_name, sizeof(tts_name), "%s", recap_language_name(tts_lang));

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (fork() != 0) _exit(0); /* double fork: the daemon never has to reap the pipeline */
        setsid();
        int devnull = open("/dev/null", O_RDONLY);
        int logfd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (devnull >= 0) dup2(devnull, STDIN_FILENO);
        if (logfd >= 0) { dup2(logfd, STDOUT_FILENO); dup2(logfd, STDERR_FILENO); }
        const char *argv[28];
        int a = 0;
        argv[a++] = python;
        argv[a++] = script;
        argv[a++] = "--dir";
        argv[a++] = dir;
        argv[a++] = "--language";
        argv[a++] = lang;
        argv[a++] = "--recap-lang";
        argv[a++] = recap_name;
        argv[a++] = "--tts-lang";
        argv[a++] = tts_name;
        argv[a++] = "--keywords";
        argv[a++] = keywords;
        argv[a++] = "--flac2wav";
        argv[a++] = flac2wav;
        if (mp3_only) argv[a++] = "--mp3-only";
        else if (tts) argv[a++] = "--tts";
        argv[a] = NULL;
        execvp(python, (char *const *)argv);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);

    char ev[640];
    snprintf(ev, sizeof(ev), "\"dir\":\"%.480s\",\"tts\":%s,\"mp3_only\":%s", dir,
             (tts || mp3_only) ? "true" : "false", mp3_only ? "true" : "false");
    ascolto_obs_emit(&core->obs, "recap_started", ev);
    return 0;
}

int ascolto_core_run_recap(ascolto_core_t *core, const char *md_path) {
    return spawn_recap(core, md_path, false);
}

int ascolto_core_make_mp3(ascolto_core_t *core, const char *md_path) {
    return spawn_recap(core, md_path, true);
}
