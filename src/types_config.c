#define _POSIX_C_SOURCE 200809L
#include "ascolto/filesystem.h"
#include "ascolto/storage.h"
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
  case ASCOLTO_STATE_STOPPING:
    return "STOPPING";
  case ASCOLTO_STATE_IDLE:
    return "IDLE";
  case ASCOLTO_STATE_RECORDING_AUTO:
    return "RECORDING_AUTO";
  case ASCOLTO_STATE_RECORDING_MANUAL:
    return "RECORDING_MANUAL";
  case ASCOLTO_STATE_PAUSED:
    return "PAUSED";
  default:
    return "UNKNOWN";
  }
}

const char *ascolto_channel_to_string(ascolto_channel_t channel) {
  return ascolto_channel_name(channel);
}

const char *ascolto_sensitivity_to_string(ascolto_sensitivity_t sens) {
  switch (sens) {
  case ASCOLTO_SENSITIVITY_LOW:
    return "low";
  case ASCOLTO_SENSITIVITY_MEDIUM:
    return "medium";
  case ASCOLTO_SENSITIVITY_HIGH:
    return "high";
  default:
    return "medium";
  }
}

ascolto_sensitivity_t ascolto_sensitivity_from_string(const char *str) {
  if (!str)
    return ASCOLTO_SENSITIVITY_MEDIUM;
  if (strcasecmp(str, "low") == 0 || strcasecmp(str, "bassa") == 0) {
    return ASCOLTO_SENSITIVITY_LOW;
  }
  if (strcasecmp(str, "high") == 0 || strcasecmp(str, "alta") == 0) {
    return ASCOLTO_SENSITIVITY_HIGH;
  }
  return ASCOLTO_SENSITIVITY_MEDIUM;
}

float ascolto_sensitivity_threshold(ascolto_sensitivity_t sens,
                                    float custom_override) {
  if (custom_override > 0.05f && custom_override < 0.999f) {
    return custom_override;
  }
  switch (sens) {
  case ASCOLTO_SENSITIVITY_LOW:
    return 0.72f; /* Fewer speakers, merges more aggressively */
  case ASCOLTO_SENSITIVITY_MEDIUM:
    return 0.85f; /* Balanced speaker separation */
  case ASCOLTO_SENSITIVITY_HIGH:
    return 0.93f; /* Sensitive to subtle voice differences */
  default:
    return 0.85f;
  }
}

void ascolto_config_init_defaults(ascolto_config_t *cfg) {
  if (!cfg)
    return;
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
  if (!home || !*home)
    home = "/tmp";

  snprintf(cfg->output_dir, sizeof(cfg->output_dir), "%s/Documents/Ascolto",
           home);
  snprintf(cfg->config_path, sizeof(cfg->config_path),
           "%s/.config/ascolto/config.ini", home);
  snprintf(cfg->weights_path, sizeof(cfg->weights_path),
           "third_party/whistle/whistle.cact");

  const char *xdg_run = getenv("XDG_RUNTIME_DIR");
  if (xdg_run && *xdg_run) {
    snprintf(cfg->control_socket, sizeof(cfg->control_socket),
             "%s/ascolto.sock", xdg_run);
  } else {
    snprintf(cfg->control_socket, sizeof(cfg->control_socket),
             "/tmp/ascolto-%d.sock", (int)getuid());
  }
  cfg->telemetry_log[0] = '\0';
}

int ascolto_config_load(ascolto_config_t *cfg, const char *override) {
  if (!cfg)
    return -1;
  if (override && *override && override != cfg->config_path)
    snprintf(cfg->config_path, sizeof(cfg->config_path), "%s", override);
  if (ascolto_storage_bootstrap(cfg->config_path, "linux"))
    return -1;
  ascolto_db_t *db = NULL;
  if (ascolto_storage_open(cfg->config_path, &db))
    return -1;
  char value[2048];
  if (ascolto_storage_get(db, "platform", value, sizeof(value)) ||
      !strcmp(value, "unknown")) {
    ascolto_db_close(db);
    return -2;
  }
  if (!ascolto_storage_get(db, "auto_monitoring", value, sizeof(value))) {
    cfg->auto_mic_trigger = atoi(value) != 0;
  }
  if (!ascolto_storage_get(db, "save_audio", value, sizeof(value))) {
    cfg->save_audio = atoi(value) != 0;
  }
  if (!ascolto_storage_get(db, "cloud_recap", value, sizeof(value))) {
    cfg->cloud_recap = atoi(value) != 0;
  }
  if (!ascolto_storage_get(db, "recap_tts", value, sizeof(value))) {
    cfg->recap_tts = atoi(value) != 0;
  }
  ascolto_storage_read_text(db, "recap_lang", cfg->recap_lang, sizeof(cfg->recap_lang));
  ascolto_storage_read_text(db, "tts_language", cfg->tts_lang, sizeof(cfg->tts_lang));
  ascolto_storage_read_text(db, "transcription_language", cfg->language, sizeof(cfg->language));
  ascolto_storage_read_text(db, "mic_device_ref", cfg->mic_device, sizeof(cfg->mic_device));
  ascolto_storage_read_text(db, "sys_device_ref", cfg->sys_device, sizeof(cfg->sys_device));
  if (!ascolto_storage_get(db, "diarization_sensitivity", value,
                           sizeof(value))) {
    cfg->diarization_sensitivity = ascolto_sensitivity_from_string(value);
  }
  if (!ascolto_storage_get(db, "diarizer_threshold", value, sizeof(value))) {
    cfg->custom_threshold = strtof(value, NULL);
  }
  ascolto_storage_read_text(db, "output_dir", cfg->output_dir, sizeof(cfg->output_dir));
  ascolto_storage_read_text(db, "model_path", cfg->weights_path, sizeof(cfg->weights_path));
  ascolto_storage_read_text(db, "control_socket", cfg->control_socket, sizeof(cfg->control_socket));
  if (!ascolto_storage_get(db, "mic_debounce_ms", value, sizeof(value))) {
    cfg->mic_debounce_ms = atoi(value);
  }
  ascolto_storage_keywords(db, cfg->keywords, sizeof(cfg->keywords));
  ascolto_db_close(db);
  return 0;
}

int ascolto_config_save(const ascolto_config_t *cfg, const char *override) {
  if (!cfg)
    return -1;
  const char *path = override && *override ? override : cfg->config_path;
  ascolto_mkdir_p(cfg->output_dir);
  ascolto_db_t *db = NULL;
  if (ascolto_storage_open(path, &db))
    return -1;
  int rc = ascolto_db_begin(db);
  char threshold[32], debounce[32];
  snprintf(threshold, sizeof(threshold), "%g", cfg->custom_threshold);
  snprintf(debounce, sizeof(debounce), "%d", cfg->mic_debounce_ms);
  const ascolto_storage_setting_t settings[] = {
      {"platform", "linux"}, {"capture_backend", "pipewire"},
      {"auto_monitoring", cfg->auto_mic_trigger ? "1" : "0"},
      {"save_audio", cfg->save_audio ? "1" : "0"},
      {"cloud_recap", cfg->cloud_recap ? "1" : "0"},
      {"recap_tts", cfg->recap_tts ? "1" : "0"},
      {"recap_lang", cfg->recap_lang}, {"tts_language", cfg->tts_lang},
      {"transcription_language", cfg->language},
      {"mic_device_ref", cfg->mic_device}, {"sys_device_ref", cfg->sys_device},
      {"diarization_sensitivity", ascolto_sensitivity_to_string(cfg->diarization_sensitivity)},
      {"diarizer_threshold", threshold}, {"output_dir", cfg->output_dir},
      {"model_path", cfg->weights_path}, {"control_socket", cfg->control_socket},
      {"mic_debounce_ms", debounce}};
  if (!rc)
    rc = ascolto_storage_set_many(db, settings, sizeof(settings) / sizeof(settings[0]));
  if (!rc)
    rc = ascolto_storage_set_keywords(db, cfg->keywords);
  if (!rc)
    rc = ascolto_db_commit(db);
  if (rc)
    ascolto_db_rollback(db);
  ascolto_db_close(db);
  return rc ? -1 : 0;
}
