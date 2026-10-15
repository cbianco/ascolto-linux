#define _POSIX_C_SOURCE 200809L
#include "ascolto/types.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

const char *ascolto_state_to_string(ascolto_state_t state) {
    switch (state) {
        case ASCOLTO_STATE_STOPPING:         return "STOPPING";
        case ASCOLTO_STATE_IDLE:             return "IDLE";
        case ASCOLTO_STATE_RECORDING_AUTO:   return "RECORDING_AUTO";
        case ASCOLTO_STATE_RECORDING_MANUAL: return "RECORDING_MANUAL";
        case ASCOLTO_STATE_PAUSED:           return "PAUSED";
        default:                             return "UNKNOWN";
    }
}

const char *ascolto_channel_to_string(ascolto_channel_t channel) {
    return (channel == ASCOLTO_CHANNEL_MIC) ? "MIC" : "SYS";
}

const char *ascolto_sensitivity_to_string(ascolto_sensitivity_t sens) {
    switch (sens) {
        case ASCOLTO_SENSITIVITY_LOW:    return "low";
        case ASCOLTO_SENSITIVITY_MEDIUM: return "medium";
        case ASCOLTO_SENSITIVITY_HIGH:   return "high";
        default:                         return "medium";
    }
}

ascolto_sensitivity_t ascolto_sensitivity_from_string(const char *str) {
    if (!str) return ASCOLTO_SENSITIVITY_MEDIUM;
    if (strcasecmp(str, "low") == 0 || strcasecmp(str, "bassa") == 0) {
        return ASCOLTO_SENSITIVITY_LOW;
    }
    if (strcasecmp(str, "high") == 0 || strcasecmp(str, "alta") == 0) {
        return ASCOLTO_SENSITIVITY_HIGH;
    }
    return ASCOLTO_SENSITIVITY_MEDIUM;
}

float ascolto_sensitivity_threshold(ascolto_sensitivity_t sens, float custom_override) {
    if (custom_override > 0.05f && custom_override < 0.999f) {
        return custom_override;
    }
    switch (sens) {
        case ASCOLTO_SENSITIVITY_LOW:    return 0.72f; /* Fewer speakers, merges more aggressively */
        case ASCOLTO_SENSITIVITY_MEDIUM: return 0.85f; /* Balanced speaker separation */
        case ASCOLTO_SENSITIVITY_HIGH:   return 0.93f; /* Sensitive to subtle voice differences */
        default:                         return 0.85f;
    }
}

static int mkdir_p(const char *dir_path) {
    if (!dir_path || !*dir_path) return -1;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir_path);
    size_t len = strlen(tmp);
    if (len == 0) return -1;
    if (tmp[len - 1] == '/') tmp[len - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) return -1;
    return 0;
}

void ascolto_config_init_defaults(ascolto_config_t *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->auto_mic_trigger = true;
    cfg->save_audio = false;
    cfg->cloud_recap = false;
    cfg->recap_tts = false;
    snprintf(cfg->language, sizeof(cfg->language), "auto");
    snprintf(cfg->recap_lang, sizeof(cfg->recap_lang), "it");
    snprintf(cfg->tts_lang, sizeof(cfg->tts_lang), "it");
    snprintf(cfg->mic_device, sizeof(cfg->mic_device), "default");
    snprintf(cfg->sys_device, sizeof(cfg->sys_device), "default");
    cfg->diarization_sensitivity = ASCOLTO_SENSITIVITY_MEDIUM;
    cfg->custom_threshold = 0.0f;
    cfg->keywords[0] = '\0';
    cfg->mic_debounce_ms = 600;
    cfg->headless = false;

    const char *home = getenv("HOME");
    if (!home || !*home) home = "/tmp";

    snprintf(cfg->output_dir, sizeof(cfg->output_dir), "%s/Documents/Ascolto", home);
    snprintf(cfg->config_path, sizeof(cfg->config_path), "%s/.config/ascolto/config.ini", home);
    snprintf(cfg->weights_path, sizeof(cfg->weights_path), "third_party/whistle/whistle.cact");

    const char *xdg_run = getenv("XDG_RUNTIME_DIR");
    if (xdg_run && *xdg_run) {
        snprintf(cfg->control_socket, sizeof(cfg->control_socket), "%s/ascolto.sock", xdg_run);
    } else {
        snprintf(cfg->control_socket, sizeof(cfg->control_socket), "/tmp/ascolto-%d.sock", (int)getuid());
    }
    cfg->telemetry_log[0] = '\0';
}

static void trim_whitespace(char *s) {
    if (!s) return;
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' || s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

int ascolto_config_load(ascolto_config_t *cfg, const char *path) {
    if (!cfg) return -1;
    const char *target = (path && *path) ? path : cfg->config_path;
    FILE *fp = fopen(target, "r");
    if (!fp) return -1;

    char line[1536];
    while (fgets(line, sizeof(line), fp)) {
        trim_whitespace(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim_whitespace(key);
        trim_whitespace(val);

        if (strcmp(key, "auto_mic_trigger") == 0) {
            cfg->auto_mic_trigger = (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0);
        } else if (strcmp(key, "save_audio") == 0) {
            cfg->save_audio = (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0);
        } else if (strcmp(key, "cloud_recap") == 0) {
            cfg->cloud_recap = (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0);
        } else if (strcmp(key, "recap_tts") == 0) {
            cfg->recap_tts = (strcmp(val, "1") == 0 || strcasecmp(val, "true") == 0);
        } else if (strcmp(key, "recap_lang") == 0) {
            snprintf(cfg->recap_lang, sizeof(cfg->recap_lang), "%s", val);
        } else if (strcmp(key, "tts_lang") == 0) {
            snprintf(cfg->tts_lang, sizeof(cfg->tts_lang), "%s", val);
        } else if (strcmp(key, "language") == 0) {
            snprintf(cfg->language, sizeof(cfg->language), "%s", val);
        } else if (strcmp(key, "mic_device") == 0) {
            snprintf(cfg->mic_device, sizeof(cfg->mic_device), "%s", val);
        } else if (strcmp(key, "sys_device") == 0) {
            snprintf(cfg->sys_device, sizeof(cfg->sys_device), "%s", val);
        } else if (strcmp(key, "diarization_sensitivity") == 0) {
            cfg->diarization_sensitivity = ascolto_sensitivity_from_string(val);
        } else if (strcmp(key, "custom_threshold") == 0) {
            cfg->custom_threshold = strtof(val, NULL);
        } else if (strcmp(key, "keywords") == 0) {
            snprintf(cfg->keywords, sizeof(cfg->keywords), "%s", val);
        } else if (strcmp(key, "output_dir") == 0) {
            snprintf(cfg->output_dir, sizeof(cfg->output_dir), "%s", val);
        } else if (strcmp(key, "weights_path") == 0) {
            snprintf(cfg->weights_path, sizeof(cfg->weights_path), "%s", val);
        } else if (strcmp(key, "mic_debounce_ms") == 0) {
            cfg->mic_debounce_ms = atoi(val);
        }
    }
    fclose(fp);
    return 0;
}

int ascolto_config_save(const ascolto_config_t *cfg, const char *path) {
    if (!cfg) return -1;
    const char *target = (path && *path) ? path : cfg->config_path;
    char dir_buf[512];
    snprintf(dir_buf, sizeof(dir_buf), "%s", target);
    char *slash = strrchr(dir_buf, '/');
    if (slash) {
        *slash = '\0';
        mkdir_p(dir_buf);
    }
    mkdir_p(cfg->output_dir);

    FILE *fp = fopen(target, "w");
    if (!fp) return -1;
    fprintf(fp, "# ascolto configuration\n");
    fprintf(fp, "auto_mic_trigger=%s\n", cfg->auto_mic_trigger ? "true" : "false");
    fprintf(fp, "save_audio=%s\n", cfg->save_audio ? "true" : "false");
    fprintf(fp, "cloud_recap=%s\n", cfg->cloud_recap ? "true" : "false");
    fprintf(fp, "recap_tts=%s\n", cfg->recap_tts ? "true" : "false");
    fprintf(fp, "recap_lang=%s\n", cfg->recap_lang);
    fprintf(fp, "tts_lang=%s\n", cfg->tts_lang);
    fprintf(fp, "language=%s\n", cfg->language);
    fprintf(fp, "mic_device=%s\n", cfg->mic_device);
    fprintf(fp, "sys_device=%s\n", cfg->sys_device);
    fprintf(fp, "diarization_sensitivity=%s\n", ascolto_sensitivity_to_string(cfg->diarization_sensitivity));
    fprintf(fp, "custom_threshold=%.3f\n", cfg->custom_threshold);
    fprintf(fp, "keywords=%s\n", cfg->keywords);
    fprintf(fp, "output_dir=%s\n", cfg->output_dir);
    fprintf(fp, "weights_path=%s\n", cfg->weights_path);
    fprintf(fp, "mic_debounce_ms=%d\n", cfg->mic_debounce_ms);
    fclose(fp);
    return 0;
}
