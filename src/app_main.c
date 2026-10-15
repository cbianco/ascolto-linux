#include "ascolto.h"

#include <libgen.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

    if (!config_path && (getenv("ASCOLTO_MENUBAR_SMOKE_MS") || getenv("ASCOLTO_CAPTURE_SMOKE_TEST")))
        config_path = "/tmp/ascolto-test/smoke-config/config.json";
    ascolto_config_t tmp_cfg;
    ascolto_config_init_defaults(&tmp_cfg);
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

    if (socket_override) snprintf(app.config.control_socket, sizeof(app.config.control_socket), "%s", socket_override);
    const char* smoke_env = getenv("ASCOLTO_MENUBAR_SMOKE_MS");
    if ((smoke_env && atoi(smoke_env) > 0) || getenv("ASCOLTO_CAPTURE_SMOKE_TEST")) {
        snprintf(app.config.control_socket, sizeof(app.config.control_socket), "/tmp/ascolto-smoke-%u.sock", (unsigned)getpid());
        app.config.auto_trigger_mic = false;
        app.config.save_audio = false;
        if (!config_path || smoke_env) {
            snprintf(app.config.output_dir, sizeof(app.config.output_dir),
                     "/tmp/ascolto-test/smoke-out");
        }
    }

    ascolto_diagnostics_init(app.config.config_path);
    ascolto_macos_capture_t* capture = ascolto_macos_capture_start(&app);

    printf("[ascolto] Menu Bar active. Output dir: %s | Config: %s\n",
           app.config.output_dir, app.config.config_path);

    int rc = ascolto_run_menubar_app(&app, &capture);

    ascolto_macos_capture_stop(capture);
    ascolto_live_app_free(&app);
    ascolto_whistle_free(&whistle);
    return rc;
}
