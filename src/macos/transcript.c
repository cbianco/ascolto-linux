#include "transcript.h"
#include "ascolto/ai_settings.h"
#include "ascolto/filesystem.h"
#include "ascolto/json.h"
#include "ascolto/storage.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static char *session_metadata(ascolto_live_app_t *app, bool manual,
                              double duration) {
  char *buffer = NULL;
  size_t size = 0;
  FILE *out = open_memstream(&buffer, &size);
  if (!out)
    return NULL;
  if (duration < 0)
    fprintf(out, "{\"manual\":%s", manual ? "true" : "false");
  else
    fprintf(
        out,
        "{\"manual\":%s,\"duration_s\":%.3f,\"utterances\":%d,\"remote_"
        "speakers\":%d,\"bleed_frames\":%d,\"echo_turns\":%d,\"capture_dropped_"
        "samples\":%lld,\"padded_mic_samples\":%lld,\"padded_sys_samples\":%"
        "lld",
        manual ? "true" : "false", duration, app->session_utterance_count,
        app->diarizer.num_speakers, app->mic_bleed_frames_suppressed,
        app->mic_echo_turns_suppressed, (long long)app->capture_dropped_samples,
        (long long)app->capture_padded_samples[0],
        (long long)app->capture_padded_samples[1]);
  const char *keys[] = {"mic",          "in_app",         "out_app",
                        "window_title", "calendar_title", "calendar_organizer",
                        "attendees",    "calendar_notes", "audio_m4a"};
  const char *values[] = {app->active_mic_name,
                          app->active_in_apps,
                          app->active_out_apps,
                          app->call_meta.window_title,
                          app->call_meta.calendar_title,
                          app->call_meta.calendar_organizer,
                          app->call_meta.calendar_attendees,
                          app->call_meta.calendar_notes,
                          app->files.save_audio ? app->files.m4a_path : ""};
  for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
    fprintf(out, ",\"%s\":", keys[i]);
    ascolto_write_json_string(out, values[i]);
  }
  fprintf(out, "}");
  fclose(out);
  return buffer;
}

int ascolto_transcript_open(ascolto_live_app_t *app, bool manual) {
  if (app->files.storage_writer.head &&
      ascolto_storage_flush(app->config.config_path,
                            &app->files.storage_writer))
    return -1;
  memset(&app->files, 0, sizeof(app->files));
  char directory[900];
  const char *title = app->call_meta.calendar_title[0] ? app->call_meta.calendar_title : app->call_meta.window_title;
  if (ascolto_storage_call_directory(app->config.output_dir, title, directory, sizeof(directory))) return -1;
  snprintf(app->files.md_path, sizeof(app->files.md_path), "%s/transcript.md", directory);
  snprintf(app->files.jsonl_path, sizeof(app->files.jsonl_path), "%s/Data/transcript.jsonl", directory);
  snprintf(app->files.mic_raw_path, sizeof(app->files.mic_raw_path), "%s/Audio/.mic.f32", directory);
  snprintf(app->files.sys_raw_path, sizeof(app->files.sys_raw_path), "%s/Audio/.sys.f32", directory);
  snprintf(app->files.m4a_path, sizeof(app->files.m4a_path), "%s/Audio/recording.m4a", directory);

  app->files.save_audio = app->config.save_audio || app->config.cloud_recap;
  char keywords[4096];
  ascolto_config_load_keywords(&app->config, keywords, sizeof(keywords));
  char *vocabulary = ascolto_storage_vocabulary(keywords);
  if (!vocabulary)
    return -1;
  char *metadata = session_metadata(app, manual, -1);
  if (!metadata) {
    free(vocabulary);
    return -1;
  }
  ascolto_db_session_t values = {.started_at_ms = ascolto_db_now_ms(),
                                 .capture_backend = "coreaudio",
                                 .call_title = app->call_meta.window_title,
                                 .meeting_title = app->call_meta.calendar_title,
                                 .source_app = app->active_in_apps,
                                 .vocabulary_snapshot_json = vocabulary,
                                 .metadata_json = metadata};
  int rc = ascolto_storage_session(app->config.config_path, &values,
                                   app->files.md_path, app->files.jsonl_path,
                                   &app->files.database_session);
  free(vocabulary);
  free(metadata);
  if (rc) {
    app->files.storage_error = rc;
    return -1;
  }
  if (app->files.save_audio) {
    app->files.mic_raw = fopen(app->files.mic_raw_path, "wb");
    app->files.sys_raw = fopen(app->files.sys_raw_path, "wb");
    if (app->files.mic_raw)
      chmod(app->files.mic_raw_path, 0600);
    if (app->files.sys_raw)
      chmod(app->files.sys_raw_path, 0600);
    ascolto_storage_artifact(app->config.config_path,
                             app->files.database_session, "audio_m4a",
                             app->files.m4a_path, "pending");
  }
  return 0;
}

void ascolto_transcript_write_turn(ascolto_live_app_t *app,
                                   ascolto_channel_t channel, double start_s,
                                   double end_s, float rms_db,
                                   const ascolto_voiceprint_t *vp,
                                   const ascolto_transcript_result_t *tr,
                                   const char *speaker, float sim,
                                   bool is_new) {
  const char *source = channel == ASCOLTO_CHANNEL_MIC
                           ? (app->active_in_apps[0] ? app->active_in_apps
                                                     : app->active_mic_name)
                           : app->active_out_apps;
  ascolto_db_utterance_t turn = {.session_id = app->files.database_session,
                                 .start_ms = (int64_t)(start_s * 1000 + 0.5),
                                 .end_ms = (int64_t)(end_s * 1000 + 0.5),
                                 .channel = ascolto_channel_name(channel),
                                 .speaker_label = speaker,
                                 .text = tr->text,
                                 .language = tr->language,
                                 .source_app = source};
  char metadata[512];
  snprintf(metadata, sizeof(metadata),
           "{\"is_new_speaker\":%s,\"similarity\":%.3f,\"f0_hz\":%.1f,\"rms_"
           "db\":%.1f,\"ttft_ms\":%.1f}",
           is_new ? "true" : "false", sim, vp->f0_mean_hz, rms_db, tr->ttft_ms);
  turn.metadata_json = metadata;
  int rc = ascolto_storage_write(app->config.config_path,
                                 &app->files.storage_writer, &turn);
  app->files.storage_error = rc;
  if (rc) {
    ascolto_diagnostic("Transcript storage failed code=%d", rc);
  }
}

void ascolto_transcript_close(ascolto_live_app_t *app, double elapsed) {
  if (app->files.mic_raw) {
    fclose(app->files.mic_raw);
    app->files.mic_raw = NULL;
  }
  if (app->files.sys_raw) {
    fclose(app->files.sys_raw);
    app->files.sys_raw = NULL;
  }
  app->files.storage_error = ascolto_storage_flush(app->config.config_path,
                                                   &app->files.storage_writer);
  char *metadata = session_metadata(app, app->manual_override_active, elapsed);
  int rc = ascolto_storage_state(
      app->config.config_path, app->files.database_session,
      app->files.storage_error ? "failed" : "completed", metadata);
  free(metadata);
  if (rc)
    app->files.storage_error = rc;
  bool use_ai = ascolto_ai_automatic_enabled(app->config.config_path);
  if (!app->files.storage_error && app->session_utterance_count > 0)
    ascolto_ai_recap_start(app->config.config_path, app->files.md_path, true);
  if (app->files.save_audio && app->files.mic_raw_path[0] &&
      app->files.sys_raw_path[0]) {
    ascolto_config_t config = app->config;
    if (use_ai || app->files.storage_error)
      config.cloud_recap = false;
    ascolto_archive_finalize(&config, app->files.mic_raw_path,
                             app->files.sys_raw_path, app->files.m4a_path,
                             app->files.md_path);
  }
}
