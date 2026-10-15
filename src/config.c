#include "ascolto.h"
#include "ascolto/filesystem.h"
#include "ascolto/json.h"
#include "ascolto/storage.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void ascolto_config_init_defaults(ascolto_config_t *cfg) {
  if (!cfg)
    return;
  memset(cfg, 0, sizeof(*cfg));
  const char *runtime_dir = getenv("TMPDIR");
  if (runtime_dir && runtime_dir[0])
    snprintf(cfg->control_socket, sizeof(cfg->control_socket),
             "%s/ascolto.sock", runtime_dir);
  else
    snprintf(cfg->control_socket, sizeof(cfg->control_socket),
             "/tmp/ascolto-%u.sock", (unsigned)getuid());
  cfg->auto_trigger_mic = true;
  cfg->paused = false;
  cfg->save_audio = true;
  snprintf(cfg->recap_python, sizeof(cfg->recap_python), "python3");
  snprintf(cfg->language, sizeof(cfg->language), "auto");
  cfg->mic_device_id = 0;
  cfg->sys_device_id = 0;
  cfg->diarizer_threshold = 0.60f;
  snprintf(cfg->model_path, sizeof(cfg->model_path), "models/whistle.cact");

  const char *home = getenv("HOME");
  if (!home || !home[0])
    home = ".";
  snprintf(cfg->output_dir, sizeof(cfg->output_dir), "%s/Documents/Ascolto",
           home);
  snprintf(cfg->config_path, sizeof(cfg->config_path),
           "%s/.config/ascolto/config.json", home);
  snprintf(cfg->keywords_path, sizeof(cfg->keywords_path),
           "%s/.config/ascolto/keywords.txt", home);
}

int ascolto_config_load(ascolto_config_t *cfg, const char *override) {
  if (!cfg)
    return -1;
  if (override && *override && override != cfg->config_path)
    snprintf(cfg->config_path, sizeof(cfg->config_path), "%s", override);
  if (ascolto_storage_bootstrap(cfg->config_path, "macos"))
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
    cfg->auto_trigger_mic = atoi(value) != 0;
  }
  if (!ascolto_storage_get(db, "paused", value, sizeof(value))) {
    cfg->paused = atoi(value) != 0;
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
  ascolto_storage_read_text(db, "transcription_language", cfg->language, sizeof(cfg->language));
  if (!ascolto_storage_get(db, "mic_device_ref", value, sizeof(value))) {
    cfg->mic_device_id = (uint32_t)strtoul(value, NULL, 10);
  }
  if (!ascolto_storage_get(db, "sys_device_ref", value, sizeof(value))) {
    cfg->sys_device_id = (uint32_t)strtoul(value, NULL, 10);
  }
  if (!ascolto_storage_get(db, "diarizer_threshold", value, sizeof(value))) {
    cfg->diarizer_threshold = strtof(value, NULL);
  }
  ascolto_storage_read_text(db, "output_dir", cfg->output_dir, sizeof(cfg->output_dir));
  ascolto_storage_read_text(db, "keywords_path", cfg->keywords_path, sizeof(cfg->keywords_path));
  ascolto_storage_read_text(db, "model_path", cfg->model_path, sizeof(cfg->model_path));
  ascolto_storage_read_text(db, "control_socket", cfg->control_socket, sizeof(cfg->control_socket));
  ascolto_storage_read_text(db, "recap_python", cfg->recap_python, sizeof(cfg->recap_python));
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
  char mic[32], sys[32], threshold[32];
  snprintf(mic, sizeof(mic), "%u", cfg->mic_device_id);
  snprintf(sys, sizeof(sys), "%u", cfg->sys_device_id);
  snprintf(threshold, sizeof(threshold), "%g", cfg->diarizer_threshold);
  const ascolto_storage_setting_t settings[] = {
      {"platform", "macos"}, {"capture_backend", "coreaudio"},
      {"auto_monitoring", cfg->auto_trigger_mic ? "1" : "0"},
      {"paused", cfg->paused ? "1" : "0"},
      {"save_audio", cfg->save_audio ? "1" : "0"},
      {"cloud_recap", cfg->cloud_recap ? "1" : "0"},
      {"recap_tts", cfg->recap_tts ? "1" : "0"},
      {"transcription_language", cfg->language},
      {"mic_device_ref", mic}, {"sys_device_ref", sys},
      {"diarizer_threshold", threshold}, {"output_dir", cfg->output_dir},
      {"keywords_path", cfg->keywords_path}, {"model_path", cfg->model_path},
      {"control_socket", cfg->control_socket}, {"recap_python", cfg->recap_python}};
  if (!rc)
    rc = ascolto_storage_set_many(db, settings, sizeof(settings) / sizeof(settings[0]));
  if (!rc)
    rc = ascolto_db_commit(db);
  if (rc)
    ascolto_db_rollback(db);
  ascolto_db_close(db);
  return rc ? -1 : 0;
}

int ascolto_config_load_keywords(const ascolto_config_t *cfg, char *out,
                                 size_t cap) {
  if (!cfg || !out || !cap)
    return -1;
  out[0] = 0;
  ascolto_db_t *db = NULL;
  if (ascolto_storage_open(cfg->config_path, &db))
    return -1;
  int rc = ascolto_storage_keywords(db, out, cap);
  ascolto_db_close(db);
  return rc ? -1 : (int)strlen(out);
}
