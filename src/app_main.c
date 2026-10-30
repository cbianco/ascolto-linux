#include "ascolto.h"
#include "ascolto/filesystem.h"

#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <mach-o/dyld.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* Keep this descriptor until process exit. Bootstrap children must not inherit
 * the lock, and a second process must fail before schema adoption/migration. */
static int startup_lock = -1;

static int startup_socket_available(const char *path) {
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(address.sun_path)) {
        fprintf(stderr, "[ascolto] The control socket path is too long. Use --socket with a shorter path.\n");
        return -1;
    }
    struct stat info;
    if (lstat(path, &info)) {
        if (errno == ENOENT) return 0;
        fprintf(stderr, "[ascolto] Cannot inspect the control socket. Check its path and permissions.\n");
        return -1;
    }
    if (!S_ISSOCK(info.st_mode) || info.st_uid != getuid()) {
        fprintf(stderr, "[ascolto] The control socket path is occupied by another file or owner. Choose another --socket path.\n");
        return -1;
    }
    int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe < 0) return -1;
    if (fcntl(probe, F_SETFL, O_NONBLOCK) || fcntl(probe, F_SETFD, FD_CLOEXEC)) { close(probe); return -1; }
    snprintf(address.sun_path, sizeof(address.sun_path), "%s", path);
    int connected = connect(probe, (struct sockaddr *)&address, sizeof(address));
    int error = errno;
    close(probe);
    if (!connected || error != ECONNREFUSED) {
        fprintf(stderr, "[ascolto] An existing app owns this control socket. Keep it open or quit it before starting this version; the database was not migrated.\n");
        return -1;
    }
    return 0;
}

static int startup_guard(const char *config_path, const char *socket_override,
                         bool isolated, bool smoke, ascolto_config_t *defaults) {
    char parent[sizeof(defaults->config_path) + 3];
    int count = snprintf(parent, sizeof(parent), "%s%s", strchr(config_path, '/') ? "" : "./", config_path);
    if (count < 0 || (size_t)count >= sizeof(parent)) return -1;
    char *last = strrchr(parent, '/');
    if (!last) return -1;
    last[1] = 0;
    char lock_path[sizeof(parent) + 32], database[sizeof(parent) + 32];
    snprintf(lock_path, sizeof(lock_path), "%sascolto.instance.lock", parent);
    snprintf(database, sizeof(database), "%sascolto.sqlite3", parent);
    if (ascolto_mkdir_parent(lock_path)) {
        fprintf(stderr, "[ascolto] Cannot prepare the configuration directory for the startup lock.\n");
        return -1;
    }
    startup_lock = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    struct stat lock_info;
    if (startup_lock < 0 || fstat(startup_lock, &lock_info) || !S_ISREG(lock_info.st_mode) ||
        lock_info.st_uid != getuid() || flock(startup_lock, LOCK_EX | LOCK_NB)) {
        fprintf(stderr, "[ascolto] Another instance is using this configuration, or its startup lock is unavailable. Keep the existing app open or quit it before starting another.\n");
        return -1;
    }
    /* Do not use storage_open/config_load here: both may migrate the database
     * while an older application still owns its control socket. */
    bool saved_socket = false;
    sqlite3 *db = NULL;
    struct stat database_info;
    bool database_exists = lstat(database, &database_info) == 0;
    if (!database_exists && errno != ENOENT) return -1;
    if (database_exists) {
        const char *stage = "open";
        int rc = sqlite3_open_v2(database, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX, NULL);
        sqlite3_stmt *query = NULL;
        if (rc == SQLITE_OK) {
            stage = "busy_timeout";
            rc = sqlite3_busy_timeout(db, ASCOLTO_DATABASE_BUSY_TIMEOUT_MS);
        }
        if (rc == SQLITE_OK) {
            stage = "prepare";
            rc = sqlite3_prepare_v2(db, "SELECT control_socket FROM app_settings WHERE id=1", -1, &query, NULL);
        }
        if (rc == SQLITE_OK) { stage = "step"; rc = sqlite3_step(query); }
        if (rc == SQLITE_ROW) {
            const char *value = (const char *)sqlite3_column_text(query, 0);
            if (value && *value) {
                if (strlen(value) >= sizeof(defaults->control_socket)) {
                    sqlite3_finalize(query); sqlite3_close(db);
                    fprintf(stderr, "[ascolto] The saved control socket path is too long. Use --socket with a shorter path.\n");
                    return -1;
                }
                snprintf(defaults->control_socket, sizeof(defaults->control_socket), "%s", value);
                saved_socket = true;
            }
        }
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            int extended = db ? sqlite3_extended_errcode(db) : rc;
            char reason[192];
            snprintf(reason, sizeof(reason), "%s", db ? sqlite3_errmsg(db) : sqlite3_errstr(rc));
            for (char *p = reason; *p; ++p) if ((unsigned char)*p < 32) *p = ' ';
            sqlite3_finalize(query);
            if (db) sqlite3_close(db);
            fprintf(stderr, "[ascolto] Cannot read the existing configuration safely (SQLite %s, code %d/%d: %s). Quit the existing app and check database permissions before restarting.\n", stage, rc, extended, reason);
            return -1;
        }
        sqlite3_finalize(query);
    }
    if (db) sqlite3_close(db);
    /* An override cannot make migrating a database still owned by an older app
     * safe. Check its saved endpoint before choosing this instance's endpoint. */
    if ((saved_socket || !isolated) && startup_socket_available(defaults->control_socket)) return -1;
    if (isolated && !saved_socket)
        snprintf(defaults->control_socket, sizeof(defaults->control_socket), "%sascolto.sock", parent);
    if (smoke && !socket_override)
        snprintf(defaults->control_socket, sizeof(defaults->control_socket), "/tmp/ascolto-smoke-%u.sock", (unsigned)getpid());
    if (socket_override) {
        if (strlen(socket_override) >= sizeof(defaults->control_socket)) {
            fprintf(stderr, "[ascolto] The control socket path is too long. Use --socket with a shorter path.\n");
            return -1;
        }
        snprintf(defaults->control_socket, sizeof(defaults->control_socket), "%s", socket_override);
    }
    return startup_socket_available(defaults->control_socket);
}

#ifndef ASCOLTO_STARTUP_GUARD_TEST
static void resolve_model_path(const char* configured, char* out_path, size_t cap) {
    if (access(configured, R_OK) == 0) {
        snprintf(out_path, cap, "%s", configured);
        return;
    }
    char exe_path[1024];
    uint32_t sz = sizeof(exe_path);
    if (_NSGetExecutablePath(exe_path, &sz) == 0) {
        char dir_buf[1024];
        snprintf(dir_buf, sizeof(dir_buf), "%s", exe_path);
        char* d = dirname(dir_buf);
        /* Try ../Resources/whistle.cact (inside .app bundle) or ../../../models/whistle.cact */
        char candidate[1024];
        snprintf(candidate, sizeof(candidate), "%s/../Resources/whistle.cact", d);
        if (access(candidate, R_OK) == 0) {
            snprintf(out_path, cap, "%s", candidate);
            return;
        }
        snprintf(candidate, sizeof(candidate), "%s/../../../../models/whistle.cact", d);
        if (access(candidate, R_OK) == 0) {
            snprintf(out_path, cap, "%s", candidate);
            return;
        }
        snprintf(candidate, sizeof(candidate), "%s/../models/whistle.cact", d);
        if (access(candidate, R_OK) == 0) {
            snprintf(out_path, cap, "%s", candidate);
            return;
        }
    }
    snprintf(out_path, cap, "%s", configured);
}

int main(int argc, char** argv) {
    const char* config_path = NULL;
    const char* model_override = NULL;
    const char* socket_override = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            socket_override = argv[++i];
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_path = argv[++i];
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model_override = argv[++i];
        } else if (strcmp(argv[i], "--smoke-test") == 0) {
            setenv("ASCOLTO_MENUBAR_SMOKE_MS", "350", 1);
        } else if (strcmp(argv[i], "--capture-smoke-test") == 0) {
            setenv("ASCOLTO_CAPTURE_SMOKE_TEST", "1", 1);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--config <path>] [--model <whistle.cact>] [--socket <path>] [--smoke-test] [--capture-smoke-test]\n", argv[0]);
            return 0;
        }
    }

    if ((config_path && !*config_path) || (socket_override && !*socket_override)) {
        fprintf(stderr, "[ascolto] --config and --socket require nonempty paths.\n");
        return 1;
    }
    if (!config_path && (getenv("ASCOLTO_MENUBAR_SMOKE_MS") || getenv("ASCOLTO_CAPTURE_SMOKE_TEST")))
        config_path = "/tmp/ascolto-test/smoke-config/config.json";
    ascolto_config_t tmp_cfg;
    ascolto_config_init_defaults(&tmp_cfg);
    bool isolated = config_path && strcmp(config_path, tmp_cfg.config_path);
    const char *effective_config = config_path ? config_path : tmp_cfg.config_path;
    bool smoke = getenv("ASCOLTO_MENUBAR_SMOKE_MS") || getenv("ASCOLTO_CAPTURE_SMOKE_TEST");
    if (startup_guard(effective_config, socket_override, isolated, smoke, &tmp_cfg)) return 1;
    char startup_socket[sizeof(tmp_cfg.control_socket)];
    snprintf(startup_socket, sizeof(startup_socket), "%s", tmp_cfg.control_socket);
    if (ascolto_config_load(&tmp_cfg, config_path) == -1) return 1;

    char resolved_model[1024];
    resolve_model_path(model_override ? model_override : tmp_cfg.model_path,
                       resolved_model, sizeof(resolved_model));

    ascolto_whistle_t whistle;
    if (ascolto_whistle_init(&whistle, resolved_model) != 0) {
        fprintf(stderr, "[ascolto] ERROR: Failed to load Whistle model at '%s'\n", resolved_model);
        return 1;
    }

    ascolto_live_app_t app;
    if (ascolto_live_app_init(&app, &whistle, config_path) != 0) {
        fprintf(stderr, "[ascolto] ERROR: Failed to initialize live application state\n");
        ascolto_whistle_free(&whistle);
        return 1;
    }

    snprintf(app.config.control_socket, sizeof(app.config.control_socket), "%s", startup_socket);
    const char* smoke_env = getenv("ASCOLTO_MENUBAR_SMOKE_MS");
    if ((smoke_env && atoi(smoke_env) > 0) || getenv("ASCOLTO_CAPTURE_SMOKE_TEST")) {
        app.config.auto_trigger_mic = false;
        app.config.save_audio = false;
        if (!config_path || smoke_env) {
            snprintf(app.config.output_dir, sizeof(app.config.output_dir),
                     "/tmp/ascolto-test/smoke-out");
        }
    }

    ascolto_diagnostics_init(app.config.config_path);
    /* Production capture never waits for local speech-model preparation. */
    if (!smoke_env && !getenv("ASCOLTO_CAPTURE_SMOKE_TEST")) {
        app.deferred_worker = ascolto_deferred_start(&app);
        if (!app.deferred_worker) {
            fprintf(stderr, "[ascolto] Cannot start the persistent transcription queue.\n");
            ascolto_live_app_free(&app);
            ascolto_whistle_free(&whistle);
            return 1;
        }
    }
    ascolto_macos_capture_t* capture = ascolto_macos_capture_start(&app);

    printf("[ascolto] Menu Bar active. Output dir: %s | Config: %s\n",
           app.config.output_dir, app.config.config_path);

    int rc = ascolto_run_menubar_app(&app, &capture);

    ascolto_macos_capture_stop(capture);
    ascolto_live_app_free(&app);
    ascolto_whistle_free(&whistle);
    return rc;
}
#endif
