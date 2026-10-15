#include "ascolto.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static void ensure_parent_dir(const char* file_path) {
    if (!file_path) return;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", file_path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

void ascolto_config_init_defaults(ascolto_config_t* cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->auto_trigger_mic = true;
    cfg->paused = false;
    cfg->save_audio = true;
    snprintf(cfg->language, sizeof(cfg->language), "auto");
    cfg->mic_device_id = 0;
    cfg->sys_device_id = 0;
    cfg->diarizer_threshold = 0.60f;
    snprintf(cfg->model_path, sizeof(cfg->model_path), "models/whistle.cact");

    const char* home = getenv("HOME");
    if (!home || !home[0]) home = ".";
    snprintf(cfg->output_dir, sizeof(cfg->output_dir), "%s/Documents/Ascolto", home);
    snprintf(cfg->config_path, sizeof(cfg->config_path), "%s/.config/ascolto/config.json", home);
    snprintf(cfg->keywords_path, sizeof(cfg->keywords_path), "%s/.config/ascolto/keywords.txt", home);
}

static void json_parse_str_field(const char* buf, const char* key, char* dst, size_t cap) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(buf, pat);
    if (!p) return;
    p = strchr(p + strlen(pat), ':');
    if (!p) return;
    p = strchr(p, '"');
    if (!p) return;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && *(p + 1)) p++;
        dst[i++] = *p++;
    }
    dst[i] = '\0';
}

static bool json_parse_bool_field(const char* buf, const char* key, bool fallback) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(buf, pat);
    if (!p) return fallback;
    p = strchr(p + strlen(pat), ':');
    if (!p) return fallback;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true", 4) == 0) return true;
    if (strncmp(p, "false", 5) == 0) return false;
    return fallback;
}

static double json_parse_num_field(const char* buf, const char* key, double fallback) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(buf, pat);
    if (!p) return fallback;
    p = strchr(p + strlen(pat), ':');
    if (!p) return fallback;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return strtod(p, NULL);
}

int ascolto_config_load(ascolto_config_t* cfg, const char* path_override) {
    if (!cfg) return -1;
    if (cfg->output_dir[0] == '\0') {
        ascolto_config_init_defaults(cfg);
    }
    if (path_override && path_override[0]) {
        snprintf(cfg->config_path, sizeof(cfg->config_path), "%s", path_override);
    }

    FILE* f = fopen(cfg->config_path, "rb");
    if (!f) return -2;

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    cfg->auto_trigger_mic = json_parse_bool_field(buf, "auto_trigger_mic", cfg->auto_trigger_mic);
    cfg->paused = json_parse_bool_field(buf, "paused", cfg->paused);
    cfg->save_audio = json_parse_bool_field(buf, "save_audio", cfg->save_audio);
    json_parse_str_field(buf, "language", cfg->language, sizeof(cfg->language));
    cfg->mic_device_id = (uint32_t)json_parse_num_field(buf, "mic_device_id", cfg->mic_device_id);
    cfg->sys_device_id = (uint32_t)json_parse_num_field(buf, "sys_device_id", cfg->sys_device_id);
    cfg->diarizer_threshold = (float)json_parse_num_field(buf, "diarizer_threshold", cfg->diarizer_threshold);
    json_parse_str_field(buf, "output_dir", cfg->output_dir, sizeof(cfg->output_dir));
    json_parse_str_field(buf, "keywords_path", cfg->keywords_path, sizeof(cfg->keywords_path));
    json_parse_str_field(buf, "model_path", cfg->model_path, sizeof(cfg->model_path));

    return 0;
}

int ascolto_config_save(const ascolto_config_t* cfg, const char* path_override) {
    if (!cfg) return -1;
    const char* path = (path_override && path_override[0]) ? path_override : cfg->config_path;
    ensure_parent_dir(path);

    /* Also ensure output_dir and keywords file exist */
    char dummy_out[540];
    snprintf(dummy_out, sizeof(dummy_out), "%s/.keep", cfg->output_dir);
    ensure_parent_dir(dummy_out);

    ensure_parent_dir(cfg->keywords_path);
    FILE* kf = fopen(cfg->keywords_path, "ab");
    if (kf) fclose(kf);

    FILE* f = fopen(path, "wb");
    if (!f) return -2;

    fprintf(f, "{\n");
    fprintf(f, "  \"auto_trigger_mic\": %s,\n", cfg->auto_trigger_mic ? "true" : "false");
    fprintf(f, "  \"paused\": %s,\n", cfg->paused ? "true" : "false");
    fprintf(f, "  \"save_audio\": %s,\n", cfg->save_audio ? "true" : "false");
    fprintf(f, "  \"language\": \"%s\",\n", cfg->language);
    fprintf(f, "  \"mic_device_id\": %u,\n", cfg->mic_device_id);
    fprintf(f, "  \"sys_device_id\": %u,\n", cfg->sys_device_id);
    fprintf(f, "  \"diarizer_threshold\": %.2f,\n", cfg->diarizer_threshold);
    fprintf(f, "  \"output_dir\": \"%s\",\n", cfg->output_dir);
    fprintf(f, "  \"keywords_path\": \"%s\",\n", cfg->keywords_path);
    fprintf(f, "  \"model_path\": \"%s\"\n", cfg->model_path);
    fprintf(f, "}\n");

    fclose(f);
    return 0;
}

int ascolto_config_load_keywords(const ascolto_config_t* cfg, char* out_buf, size_t out_cap) {
    if (!cfg || !out_buf || out_cap == 0) return -1;
    out_buf[0] = '\0';
    FILE* f = fopen(cfg->keywords_path, "rb");
    if (!f) return 0;
    size_t n = fread(out_buf, 1, out_cap - 1, f);
    out_buf[n] = '\0';
    fclose(f);
    /* Trim trailing whitespace */
    while (n > 0 && (out_buf[n - 1] == '\n' || out_buf[n - 1] == '\r' || out_buf[n - 1] == ' ')) {
        out_buf[--n] = '\0';
    }
    return (int)n;
}
