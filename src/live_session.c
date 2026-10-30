#include "ascolto.h"
#include "macos/transcript.h"
#include "ascolto/storage.h"
#include "ascolto/filesystem.h"
#include "ascolto/deferred_store.h"
#include "ascolto/ai_settings.h"
#include <fcntl.h>
#include <CommonCrypto/CommonDigest.h>

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

const char* ascolto_app_state_label(ascolto_app_state_t st) {
    switch (st) {
        case ASCOLTO_STATE_RECORDING: return "RECORDING";
        case ASCOLTO_STATE_PAUSED:    return "PAUSED";
        case ASCOLTO_STATE_STOPPING:  return "STOPPING";
        default:                      return "IDLE";
    }
}

static double now_monotonic_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void format_mm_ss(double seconds, char* out, size_t cap) {
    if (seconds < 0.0) seconds = 0.0;
    int total = (int)seconds;
    int mm = total / 60;
    int ss = total % 60;
    snprintf(out, cap, "%02d:%02d", mm, ss);
}

static void publish_snapshot_unlocked(ascolto_live_app_t* app) {
    pthread_mutex_lock(&app->snapshot_lock);
    ascolto_live_snapshot_t* snapshot = &app->snapshot;
    snapshot->config = app->config;
    snapshot->state = app->state;
    snapshot->sys_audio_permission_ok = app->sys_audio_permission_ok;
    snapshot->mic_hardware_active = app->mic_hardware_active;
    snapshot->transcription_error = app->transcription_error;
    snapshot->last_mic_rms_db = app->last_mic_rms_db;
    snapshot->last_sys_rms_db = app->last_sys_rms_db;
    memcpy(snapshot->active_mic_name, app->active_mic_name, sizeof(snapshot->active_mic_name));
    memcpy(snapshot->active_in_apps, app->active_in_apps, sizeof(snapshot->active_in_apps));
    memcpy(snapshot->active_out_apps, app->active_out_apps, sizeof(snapshot->active_out_apps));
    snapshot->call_meta = app->call_meta;
    memcpy(snapshot->last_speaker, app->last_speaker, sizeof(snapshot->last_speaker));
    memcpy(snapshot->last_utterance_text, app->last_utterance_text, sizeof(snapshot->last_utterance_text));
    memcpy(snapshot->current_md_path, app->files.md_path, sizeof(snapshot->current_md_path));
    snapshot->settings_error = app->settings_error;
    snapshot->storage_error = app->files.storage_error;
    snapshot->utterances = app->session_utterance_count;
    snapshot->remote_speakers = app->diarizer.num_speakers;
    snapshot->session_start_s = app->session_start_epoch_s;
    snapshot->mic_samples = app->vad_mic.total_samples_processed;
    snapshot->sys_samples = app->vad_sys.total_samples_processed;
    snapshot->capture_dropped_samples = app->capture_dropped_samples;
    snapshot->mic_padding = app->capture_padded_samples[ASCOLTO_CHANNEL_MIC];
    pthread_mutex_unlock(&app->snapshot_lock);
}

void ascolto_live_app_read_snapshot(ascolto_live_app_t* app, ascolto_live_snapshot_t* snapshot) {
    if (!app || !snapshot) return;
    pthread_mutex_lock(&app->snapshot_lock);
    *snapshot = app->snapshot;
    pthread_mutex_unlock(&app->snapshot_lock);
}

bool ascolto_is_noise_hallucination(const ascolto_transcript_result_t *tr,
    ascolto_channel_t channel, float rms_db, bool recent_sys_activity) {
    if (!tr) return true;
    float probability = 1.0f;
    if (tr->num_words > 0) {
        probability = 0;
        for (int i = 0; i < tr->num_words; i++) probability += tr->words[i].probability;
        probability /= tr->num_words;
    }
    return ascolto_transcript_is_noise(tr->text, tr->num_words, probability, channel, rms_db, recent_sys_activity);
}

/* Checkpoints contain no PCM. Their explicit version and source digest prevent
 * reuse after a pipeline/schema change or modification of a saved segment. */
typedef struct {
    unsigned char pcm_digest[32];
    ascolto_transcript_result_t transcript;
    ascolto_voiceprint_t voiceprint;
    bool voiceprint_ready;
} deferred_checkpoint_t;
struct ascolto_deferred_replay_state {
    ascolto_deferred_store_t *store;
    const ascolto_deferred_job_t *job;
    int64_t ordinal, position;
    bool (*should_stop)(void *);
    void *stop_context;
    bool interrupted;
};

static int deferred_cached(ascolto_live_app_t *app, ascolto_channel_t channel,
                           const float *pcm, int count, double start, double end,
                           deferred_checkpoint_t *checkpoint) {
    struct ascolto_deferred_replay_state *r = app->deferred_replay;
    if (!r) return 0;
    CC_SHA256(pcm, (CC_LONG)((size_t)count * sizeof(float)), checkpoint->pcm_digest);
    ascolto_deferred_segment_t cached = {0};
    int rc = ascolto_deferred_segment_get(r->store, r->job->id, r->ordinal, &cached);
    if (rc == SQLITE_NOTFOUND) return 0;
    if (rc) return -1;
    bool matches = cached.checkpoint_version == 1 && cached.checkpoint_size == sizeof(*checkpoint) &&
        !strcmp(cached.channel, ascolto_channel_name(channel)) &&
        cached.start_ms == (int64_t)(start * 1000 + 0.5) && cached.end_ms == (int64_t)(end * 1000 + 0.5);
    if (matches) matches = !memcmp(checkpoint->pcm_digest, cached.checkpoint, sizeof(checkpoint->pcm_digest));
    if (matches) memcpy(checkpoint, cached.checkpoint, sizeof(*checkpoint));
    ascolto_deferred_segment_clear(&cached);
    return matches ? 1 : -1;
}

static void deferred_commit(ascolto_live_app_t *app, ascolto_channel_t channel,
                            double start, double end, float rms,
                            deferred_checkpoint_t *checkpoint, bool cached,
                            const char *speaker, float sim, bool is_new) {
    struct ascolto_deferred_replay_state *r = app->deferred_replay;
    if (!r || app->transcription_error) return;
    if (cached) { r->ordinal++; return; }
    ascolto_deferred_segment_t segment = {.ordinal = r->ordinal, .end_frame = r->position,
        .channel = (char *)ascolto_channel_name(channel), .checkpoint_version = 1,
        .checkpoint = checkpoint, .checkpoint_size = sizeof(*checkpoint),
        .start_ms = (int64_t)(start * 1000 + 0.5), .end_ms = (int64_t)(end * 1000 + 0.5)};
    char metadata[512];
    snprintf(metadata, sizeof(metadata),
        "{\"is_new_speaker\":%s,\"similarity\":%.3f,\"f0_hz\":%.1f,\"rms_db\":%.1f,\"ttft_ms\":null,\"local_model\":\"%s\",\"post_call\":true}",
        is_new ? "true" : "false", sim, checkpoint->voiceprint.f0_mean_hz, rms, app->local_stt.id);
    ascolto_db_utterance_t turn = {.session_id = r->job->session_id,
        .start_ms = segment.start_ms, .end_ms = segment.end_ms, .channel = segment.channel,
        .speaker_label = speaker, .text = checkpoint->transcript.text,
        .language = checkpoint->transcript.language,
        .source_app = channel == ASCOLTO_CHANNEL_MIC ? app->active_in_apps : app->active_out_apps,
        .metadata_json = metadata};
    int rc = ascolto_deferred_segment_commit(r->store, r->job->id, &segment, speaker ? &turn : NULL);
    if (rc) { app->transcription_error = true; app->files.storage_error = rc; }
    else {
        r->ordinal++;
        if (speaker && ascolto_storage_publish(ascolto_deferred_store_database(r->store), r->job->session_id)) {
            app->transcription_error = true;
            app->files.storage_error = SQLITE_IOERR;
        }
    }
}

static void on_live_vad_turn(ascolto_channel_t channel, const float *pcm,
    int num_samples, double start_s, double end_s, float rms_db, void *userdata) {
    ascolto_live_app_t *app = userdata;
    if (!app || !app->whistle || !app->whistle->loaded || app->transcription_error) return;
    struct ascolto_deferred_replay_state *r = app->deferred_replay;
    if (r && (r->interrupted || r->should_stop(r->stop_context))) { r->interrupted = true; return; }
    ascolto_mic_activity_t activity = app->echo.mic;
    if (channel == ASCOLTO_CHANNEL_MIC) activity = ascolto_mic_activity_take(&app->echo.mic);
    char kw_buf[4096];
    if (!r) {
        ascolto_config_load_keywords(&app->config, kw_buf, sizeof(kw_buf));
        if (app->call_meta.calendar_attendees[0]) {
            size_t len = strlen(kw_buf);
            if (len + 2 < sizeof(kw_buf)) {
                if (len > 0) kw_buf[len++] = '\n';
                for (const char *p = app->call_meta.calendar_attendees; *p && len + 1 < sizeof(kw_buf); p++)
                    kw_buf[len++] = *p == ',' ? '\n' : *p;
                kw_buf[len] = 0;
            }
        }
    }
    const char *keywords = r ? r->job->keywords : kw_buf;
    deferred_checkpoint_t checkpoint = {0};
    int cached = deferred_cached(app, channel, pcm, num_samples, start_s, end_s, &checkpoint);
    if (cached < 0) { app->transcription_error = true; return; }
    ascolto_transcript_result_t *tr = &checkpoint.transcript;
    if (!cached) {
        int tok;
        if (app->local_stt.context) {
            ascolto_stt_text_t result = {0};
            tok = ascolto_local_stt_transcribe(app->local_models, &app->local_stt, pcm, num_samples,
                app->config.language, keywords && keywords[0] ? keywords : NULL, &result);
            if (!tok) {
                snprintf(tr->text, sizeof(tr->text), "%s", result.text);
                snprintf(tr->language, sizeof(tr->language), "%s", result.language);
            }
        } else {
            tok = ascolto_whistle_transcribe(app->whistle, pcm, num_samples, app->config.language,
                keywords && keywords[0] ? keywords : NULL, true, tr);
        }
        if (tok < 0 || (app->local_stt.context && tok != 0)) { app->transcription_error = true; return; }
    }
    double sys_stream_s = (double)app->vad_sys.total_samples_processed / ASCOLTO_SAMPLE_RATE;
    double dt_sys_stream = app->last_sys_voiced_stream_s >= 0 ? sys_stream_s - app->last_sys_voiced_stream_s : 999;
    if (dt_sys_stream < 0) dt_sys_stream = 0;
    bool recent_sys = dt_sys_stream < 2.2 || activity.sys_frames > 0 || app->vad_sys.in_speech;
    if (!tr->text[0] || ascolto_is_noise_hallucination(tr, channel, rms_db, recent_sys)) {
        deferred_commit(app, channel, start_s, end_s, rms_db, &checkpoint, cached, NULL, 0, false);
        return;
    }
    if (!checkpoint.voiceprint_ready) {
        float *emb = NULL;
        int n_frames = 0;
        ascolto_whistle_embed_frames(app->whistle, pcm, num_samples, &emb, &n_frames);
        ascolto_extract_voiceprint(pcm, num_samples, emb, n_frames, &checkpoint.voiceprint);
        free(emb);
        checkpoint.voiceprint_ready = true;
    }
    if (channel == ASCOLTO_CHANNEL_MIC) {
        if (ascolto_echo_filter_matches(&app->echo, tr->text, start_s, end_s, rms_db, &activity)) {
            app->mic_echo_turns_suppressed++;
            deferred_commit(app, channel, start_s, end_s, rms_db, &checkpoint, cached, NULL, 0, false);
            return;
        }
    } else ascolto_echo_filter_remember(&app->echo, tr->text, start_s, end_s, rms_db);
    float sim = 1;
    bool is_new = false;
    const char *speaker = ascolto_diarizer_identify(&app->diarizer, channel, &checkpoint.voiceprint, &sim, &is_new);
    app->session_utterance_count++;
    snprintf(app->last_speaker, sizeof(app->last_speaker), "%s", speaker);
    snprintf(app->last_utterance_text, sizeof(app->last_utterance_text), "%s", tr->text);
    if (r) deferred_commit(app, channel, start_s, end_s, rms_db, &checkpoint, cached, speaker, sim, is_new);
    else ascolto_transcript_write_turn(app, channel, start_s, end_s, rms_db, &checkpoint.voiceprint, tr, speaker, sim, is_new);
}

int ascolto_live_app_init(ascolto_live_app_t* app, ascolto_whistle_t* whistle, const char* config_path) {
    if (!app) return -1;
    memset(app, 0, sizeof(*app));
    pthread_mutex_init(&app->lock, NULL);
    pthread_cond_init(&app->session_cond, NULL);
    pthread_mutex_init(&app->snapshot_lock, NULL);

    ascolto_config_init_defaults(&app->config);
    if (ascolto_config_load(&app->config, config_path) == -1 || ascolto_config_save(&app->config, config_path)) return -1;

    app->whistle = whistle;
    app->local_models = ascolto_local_models_create(app->config.config_path);
    if (!app->local_models) return -1;
    ascolto_diarizer_init(&app->diarizer, app->config.diarizer_threshold);

    /* Separate VAD tuning for MIC (capture normal desk voice down to -35.5 dB)
       and SYS (shorter turns for responsive multi-speaker diarization) */
    ascolto_vad_config_t mic_vad_cfg = ascolto_vad_default_config();
    mic_vad_cfg.energy_threshold_db = -35.5f;
    mic_vad_cfg.min_speech_s = 0.30;
    mic_vad_cfg.min_silence_s = 0.32;
    mic_vad_cfg.max_segment_s = 12.0;

    ascolto_vad_config_t sys_vad_cfg = ascolto_vad_default_config();
    sys_vad_cfg.energy_threshold_db = -35.0f;
    sys_vad_cfg.min_speech_s = 0.35;
    sys_vad_cfg.min_silence_s = 0.38;
    sys_vad_cfg.max_segment_s = 10.0;

    ascolto_online_vad_init(&app->vad_mic, ASCOLTO_CHANNEL_MIC, &mic_vad_cfg);
    ascolto_online_vad_init(&app->vad_sys, ASCOLTO_CHANNEL_SYS, &sys_vad_cfg);

    app->state = app->config.paused ? ASCOLTO_STATE_PAUSED : ASCOLTO_STATE_IDLE;
    app->sys_audio_permission_ok = false;
    app->last_mic_rms_db = -100.0f;
    app->last_sys_rms_db = -100.0f;
    app->last_sys_voiced_stream_s = -10.0;
    ascolto_echo_filter_reset(&app->echo);
    memset(app->pending_audio_count, 0, sizeof(app->pending_audio_count));
    app->mic_bleed_frames_suppressed = 0;
    app->mic_echo_turns_suppressed = 0;
    app->capture_dropped_samples = 0;
    memset(app->capture_padded_samples, 0, sizeof(app->capture_padded_samples));
    snprintf(app->active_mic_name, sizeof(app->active_mic_name), "Predefinito di sistema");
    snprintf(app->active_in_apps, sizeof(app->active_in_apps), "Nessuna");
    snprintf(app->active_out_apps, sizeof(app->active_out_apps), "Nessuna");
    snprintf(app->last_utterance_text, sizeof(app->last_utterance_text), "Nessuna trascrizione recente");

    ascolto_macos_device_info_t def_mic;
    if (ascolto_macos_default_mic_status(&def_mic) == 0) {
        snprintf(app->active_mic_name, sizeof(app->active_mic_name), "%s", def_mic.device_name);
    }
    app->mic_hardware_active = ascolto_macos_query_audio_apps(
        app->active_in_apps, sizeof(app->active_in_apps),
        app->active_out_apps, sizeof(app->active_out_apps)
    );

    /* Startup runs before the UI. Only archive paths are restored, never live counters. */
    if (ascolto_storage_latest_transcript(app->config.config_path, app->files.md_path, sizeof(app->files.md_path), app->files.jsonl_path, sizeof(app->files.jsonl_path))) return -1;
    publish_snapshot_unlocked(app);
    return 0;
}

static int start_session_unlocked(ascolto_live_app_t* app, bool is_manual) {
    if (app->state == ASCOLTO_STATE_RECORDING) return 0;
    if (app->state == ASCOLTO_STATE_STOPPING) return -2;

    /* Publish detected input before a potentially long model load. UI/status
       readers use this snapshot and the model manager's short-held mutex. */
    publish_snapshot_unlocked(app);
    double preparation_started = now_monotonic_s();
    if (app->deferred_worker) {
        ascolto_local_models_snapshot_t models = {0};
        ascolto_local_models_snapshot(app->local_models, &models);
        memset(&app->local_stt, 0, sizeof(app->local_stt));
        snprintf(app->local_stt.id, sizeof(app->local_stt.id), "%s", models.selected);
    } else if (ascolto_local_stt_begin(app->local_models, &app->local_stt)) return -1;
    ascolto_diagnostic("Session model ready manual=%d duration_ms=%.0f", is_manual,
                      (now_monotonic_s() - preparation_started) * 1000.0);
    app->transcription_error = false;
    if (ascolto_transcript_open(app, is_manual)) {
        if (!app->deferred_worker) ascolto_local_stt_end(app->local_models, &app->local_stt);
        return -1;
    }

    ascolto_diarizer_init(&app->diarizer, app->config.diarizer_threshold);
    ascolto_online_vad_reset(&app->vad_mic);
    ascolto_online_vad_reset(&app->vad_sys);

    ascolto_echo_filter_reset(&app->echo);
    memset(app->pending_audio_count, 0, sizeof(app->pending_audio_count));
    app->last_sys_rms_db = -100.0f;
    app->last_sys_voiced_stream_s = -10.0;
    app->mic_bleed_frames_suppressed = 0;
    app->mic_echo_turns_suppressed = 0;
    app->capture_dropped_samples = 0;
    memset(app->capture_padded_samples, 0, sizeof(app->capture_padded_samples));

    app->session_generation++;
    app->session_start_epoch_s = now_monotonic_s();
    app->session_utterance_count = 0;
    app->manual_override_active = is_manual;
    app->state = ASCOLTO_STATE_RECORDING;
    return 0;
}

static void process_audio_unlocked(ascolto_live_app_t* app, ascolto_channel_t channel,
                                   const float* samples, int num_samples);

static int stop_session_unlocked(ascolto_live_app_t* app) {
    if (app->state != ASCOLTO_STATE_RECORDING && app->state != ASCOLTO_STATE_STOPPING) return 0;

    for (int ch = ASCOLTO_CHANNEL_SYS; ch >= ASCOLTO_CHANNEL_MIC; ch--) {
        if (app->pending_audio_count[ch] > 0) {
            process_audio_unlocked(app, (ascolto_channel_t)ch, app->pending_audio[ch], app->pending_audio_count[ch]);
            app->pending_audio_count[ch] = 0;
        }
    }
    if (!app->deferred_worker) {
        ascolto_online_vad_flush(&app->vad_sys, on_live_vad_turn, app);
        ascolto_online_vad_flush(&app->vad_mic, on_live_vad_turn, app);
    }

    double elapsed = (double)(app->vad_mic.total_samples_processed > app->vad_sys.total_samples_processed ? app->vad_mic.total_samples_processed : app->vad_sys.total_samples_processed) / ASCOLTO_SAMPLE_RATE;

    ascolto_transcript_close(app, elapsed);
    if (!app->deferred_worker) ascolto_local_stt_end(app->local_models, &app->local_stt);

    app->manual_override_active = false;
    app->state = app->config.paused ? ASCOLTO_STATE_PAUSED : ASCOLTO_STATE_IDLE;
    return 0;
}

int ascolto_live_app_start_session(ascolto_live_app_t* app, bool is_manual) {
    if (!app) return -1;
    ascolto_diagnostic("Session start requested manual=%d; waiting for processing lock", is_manual);
    pthread_mutex_lock(&app->lock);
    if (app->state == ASCOLTO_STATE_STOPPING) {
        pthread_mutex_unlock(&app->lock);
        return -2;
    }
    if (is_manual) {
        app->config.paused = false;
        app->manual_stop_for_current_call = false;
    }
    int rc = start_session_unlocked(app, is_manual);
    ascolto_diagnostic("Session start completed result=%d generation=%llu state=%d", rc,
                      (unsigned long long)app->session_generation, app->state);
    publish_snapshot_unlocked(app);
    pthread_mutex_unlock(&app->lock);
    return rc;
}

/* Enters and returns with app->lock held. A committed stop must finish even if
   preferences change during capture drain. Only its owner drains the generation. */
static int stop_session_locked(ascolto_live_app_t* app) {
    if (app->state == ASCOLTO_STATE_STOPPING) {
        while (app->state == ASCOLTO_STATE_STOPPING) {
            pthread_cond_wait(&app->session_cond, &app->lock);
        }
        return 0;
    }
    if (app->state != ASCOLTO_STATE_RECORDING) return 0;
    uint64_t generation = app->session_generation;
    ascolto_macos_capture_t* capture = app->capture;
    app->state = ASCOLTO_STATE_STOPPING;
    ascolto_storage_state(app->config.config_path, app->files.database_session, "stopping", NULL);
    ascolto_diagnostic("Session stop committed generation=%llu", (unsigned long long)generation);
    publish_snapshot_unlocked(app);
    pthread_mutex_unlock(&app->lock);
    ascolto_macos_capture_finish_session(capture, generation);
    pthread_mutex_lock(&app->lock);
    int rc = 0;
    if (app->session_generation == generation && app->state == ASCOLTO_STATE_STOPPING) {
        rc = stop_session_unlocked(app);
    }
    publish_snapshot_unlocked(app);
    pthread_cond_broadcast(&app->session_cond);
    ascolto_diagnostic("Session stop completed generation=%llu state=%d", (unsigned long long)generation, app->state);
    return rc;
}

int ascolto_live_app_stop_session(ascolto_live_app_t* app) {
    if (!app) return -1;
    ascolto_diagnostic("Session stop requested; waiting for processing lock");
    pthread_mutex_lock(&app->lock);
    if (app->mic_hardware_active) app->manual_stop_for_current_call = true;
    int rc = stop_session_locked(app);
    publish_snapshot_unlocked(app);
    pthread_mutex_unlock(&app->lock);
    return rc;
}

void ascolto_live_app_on_mic_hardware_change(ascolto_live_app_t* app, bool mic_active, const char* mic_name) {
    if (!app) return;
    pthread_mutex_lock(&app->lock);
    if (!mic_active) app->manual_stop_for_current_call = false;
    app->mic_hardware_active = mic_active;
    if (mic_name && mic_name[0]) snprintf(app->active_mic_name, sizeof(app->active_mic_name), "%s", mic_name);
    if (app->config.auto_trigger_mic && !app->config.paused) {
        if (mic_active && app->state == ASCOLTO_STATE_IDLE && !app->manual_stop_for_current_call) {
            start_session_unlocked(app, false);
        } else if (!mic_active && app->state == ASCOLTO_STATE_RECORDING && !app->manual_override_active) {
            stop_session_locked(app);
        }
    }
    publish_snapshot_unlocked(app);
    pthread_mutex_unlock(&app->lock);
}

/* Called with app->lock held after a preference mutation. */
static bool save_preferences_unlocked(ascolto_live_app_t *app, const ascolto_config_t *previous) {
    app->settings_error = ascolto_config_save(&app->config, NULL);
    if (app->settings_error) app->config = *previous;
    publish_snapshot_unlocked(app);
    return !app->settings_error;
}

void ascolto_live_app_set_paused(ascolto_live_app_t* app, bool paused) {
    if (!app) return;
    pthread_mutex_lock(&app->lock);
    ascolto_config_t previous = app->config;
    app->config.paused = paused;
    if (!save_preferences_unlocked(app, &previous)) { pthread_mutex_unlock(&app->lock); return; }
    if (paused && app->state == ASCOLTO_STATE_RECORDING && !app->manual_override_active) {
        stop_session_locked(app);
    } else if (!paused && app->state == ASCOLTO_STATE_PAUSED) {
        app->state = ASCOLTO_STATE_IDLE;
        if (app->config.auto_trigger_mic && app->mic_hardware_active && !app->manual_stop_for_current_call) {
            start_session_unlocked(app, false);
        }
    } else if (paused && app->state == ASCOLTO_STATE_IDLE) {
        app->state = ASCOLTO_STATE_PAUSED;
    }
    publish_snapshot_unlocked(app);
    pthread_mutex_unlock(&app->lock);
}

void ascolto_live_app_set_auto_trigger(ascolto_live_app_t* app, bool enabled) {
    if (!app) return;
    pthread_mutex_lock(&app->lock);
    ascolto_config_t previous = app->config;
    app->config.auto_trigger_mic = enabled;
    if (!save_preferences_unlocked(app, &previous)) { pthread_mutex_unlock(&app->lock); return; }
    if (enabled && !app->config.paused && app->mic_hardware_active && app->state == ASCOLTO_STATE_IDLE) {
        start_session_unlocked(app, false);
    }
    publish_snapshot_unlocked(app);
    pthread_mutex_unlock(&app->lock);
}

void ascolto_live_app_set_language(ascolto_live_app_t* app, const char* lang) {
    if (!app || !lang) return;
    pthread_mutex_lock(&app->lock);
    ascolto_config_t previous = app->config;
    snprintf(app->config.language, sizeof(app->config.language), "%s", lang);
    save_preferences_unlocked(app, &previous);
    pthread_mutex_unlock(&app->lock);
}

int ascolto_live_app_set_output_directory(ascolto_live_app_t* app, const char* path) {
    if (!app || !path || path[0] != '/' || strlen(path) >= sizeof(app->config.output_dir) || ascolto_mkdir_p(path)) return -1;
    pthread_mutex_lock(&app->lock);
    ascolto_config_t previous = app->config;
    snprintf(app->config.output_dir, sizeof(app->config.output_dir), "%s", path);
    bool saved = save_preferences_unlocked(app, &previous);
    pthread_mutex_unlock(&app->lock);
    return saved ? 0 : -1;
}

void ascolto_live_app_set_devices(ascolto_live_app_t* app, uint32_t mic_dev_id, uint32_t sys_dev_id) {
    if (!app) return;
    pthread_mutex_lock(&app->lock);
    ascolto_config_t previous = app->config;
    app->config.mic_device_id = mic_dev_id;
    app->config.sys_device_id = sys_dev_id;
    save_preferences_unlocked(app, &previous);
    pthread_mutex_unlock(&app->lock);
}

void ascolto_live_app_set_diarizer_threshold(ascolto_live_app_t* app, float threshold) {
    if (!app) return;
    pthread_mutex_lock(&app->lock);
    ascolto_config_t previous = app->config;
    app->config.diarizer_threshold = threshold;
    save_preferences_unlocked(app, &previous);
    app->diarizer.similarity_threshold = app->config.diarizer_threshold;
    pthread_mutex_unlock(&app->lock);
}

void ascolto_live_app_set_save_audio(ascolto_live_app_t* app, bool enabled) {
    if (!app) return;
    pthread_mutex_lock(&app->lock);
    ascolto_config_t previous = app->config;
    app->config.save_audio = enabled;
    save_preferences_unlocked(app, &previous);
    pthread_mutex_unlock(&app->lock);
}

void ascolto_live_app_set_cloud_recap(ascolto_live_app_t* app, bool enabled) {
    if (!app) return;
    pthread_mutex_lock(&app->lock);
    ascolto_config_t previous = app->config;
    app->config.cloud_recap = enabled;
    save_preferences_unlocked(app, &previous);
    pthread_mutex_unlock(&app->lock);
}

void ascolto_live_app_set_recap_tts(ascolto_live_app_t* app, bool enabled) {
    if (!app) return;
    pthread_mutex_lock(&app->lock);
    ascolto_config_t previous = app->config;
    app->config.recap_tts = enabled;
    save_preferences_unlocked(app, &previous);
    pthread_mutex_unlock(&app->lock);
}

static void process_audio_unlocked(
    ascolto_live_app_t* app,
    ascolto_channel_t channel,
    const float* samples,
    int num_samples
) {
    if (!app || !samples || num_samples <= 0) return;
    if (app->state == ASCOLTO_STATE_RECORDING || app->state == ASCOLTO_STATE_STOPPING) {
        if (app->deferred_worker) {
            /* The capture consumer does bounded disk work only. No ASR/VAD callbacks. */
            FILE *raw = channel == ASCOLTO_CHANNEL_MIC ? app->files.mic_raw : app->files.sys_raw;
            ascolto_online_vad_t *timeline = channel == ASCOLTO_CHANNEL_MIC ? &app->vad_mic : &app->vad_sys;
            if (!raw || fwrite(samples, sizeof(float), (size_t)num_samples, raw) != (size_t)num_samples)
                app->files.storage_error = SQLITE_IOERR_WRITE;
            timeline->total_samples_processed += num_samples;
            if (raw && timeline->total_samples_processed - app->files.synced_samples[channel] >= ASCOLTO_SAMPLE_RATE) {
                if (fflush(raw) || fsync(fileno(raw))) app->files.storage_error = SQLITE_IOERR_FSYNC;
                app->files.synced_samples[channel] = timeline->total_samples_processed;
            }
            if (channel == ASCOLTO_CHANNEL_MIC) app->last_mic_rms_db = ascolto_audio_rms_db(samples, num_samples);
            else app->last_sys_rms_db = ascolto_audio_rms_db(samples, num_samples);
            return;
        }
        float chunk_db = ascolto_audio_rms_db(samples, num_samples);
        const int frame_len = ASCOLTO_SAMPLE_RATE / 50; /* 20 ms = 320 samples */

        if (channel == ASCOLTO_CHANNEL_SYS) {
            if (app->files.sys_raw) {
                fwrite(samples, sizeof(float), (size_t)num_samples, app->files.sys_raw);
            }
            for (int idx = 0; idx < num_samples; idx += frame_len) {
                int step = (idx + frame_len <= num_samples) ? frame_len : (num_samples - idx);
                float f_db = ascolto_audio_rms_db(samples + idx, step);
                app->echo.sys_db[app->echo.sys_frames % ASCOLTO_SYS_ENV_HISTORY] = f_db;
                app->echo.sys_frames++;

                if (f_db >= -36.5f) {
                    double sys_t = (double)(app->vad_sys.total_samples_processed + idx + step) /
                                   (double)ASCOLTO_SAMPLE_RATE;
                    if (f_db > app->last_sys_rms_db || (sys_t - app->last_sys_voiced_stream_s) > 0.8) {
                        app->last_sys_rms_db = f_db;
                    } else {
                        app->last_sys_rms_db = 0.92f * app->last_sys_rms_db + 0.08f * f_db;
                    }
                    app->last_sys_voiced_stream_s = sys_t;
                }
            }
            ascolto_online_vad_feed(&app->vad_sys, samples, num_samples, on_live_vad_turn, app);
        } else {
            if (app->files.mic_raw) {
                fwrite(samples, sizeof(float), (size_t)num_samples, app->files.mic_raw);
            }
            app->last_mic_rms_db = chunk_db;

            /* Stage 1: timestamp-aligned 20ms energy gate with a bounded SYS lookback. */
            for (int idx = 0; idx < num_samples; idx += frame_len) {
                int step = (idx + frame_len <= num_samples) ? frame_len : (num_samples - idx);
                float f_db = ascolto_audio_rms_db(samples + idx, step);

                /* Address SYS history by stream sample position, independent of callback sizes.
                   Only suppress MIC within 240ms of SYS speech and below SYS + 3.5 dB. */
                float ref_sys_db;
                bool sys_active_window = ascolto_bleed_reference(app->echo.sys_db, app->echo.sys_frames,
                    app->vad_mic.total_samples_processed + step, app->vad_sys.total_samples_processed, &ref_sys_db);
                bool is_bleed = sys_active_window && f_db < ref_sys_db + 3.5f;

                if (is_bleed) {
                    float zeros[320] = {0};
                    ascolto_online_vad_feed(&app->vad_mic, zeros, step, on_live_vad_turn, app);
                    if (f_db >= app->vad_mic.cfg.energy_threshold_db) {
                        app->mic_bleed_frames_suppressed++;
                    }
                } else {
                    ascolto_online_vad_feed(&app->vad_mic, samples + idx, step, on_live_vad_turn, app);
                    ascolto_mic_activity_track(&app->echo.mic, app->vad_mic.in_speech,
                                               sys_active_window, ref_sys_db);
                }
            }
        }
    }
}

void ascolto_live_app_feed_audio(ascolto_live_app_t* app, ascolto_channel_t channel,
                                 const float* samples, int num_samples) {
    if (!app || !samples || num_samples <= 0 || channel < 0 || channel > ASCOLTO_CHANNEL_SYS) return;
    pthread_mutex_lock(&app->lock);
    if (app->state == ASCOLTO_STATE_RECORDING || app->state == ASCOLTO_STATE_STOPPING) {
        int ch = (int)channel;
        while (num_samples > 0) {
            int n = 320 - app->pending_audio_count[ch];
            if (n > num_samples) n = num_samples;
            memcpy(app->pending_audio[ch] + app->pending_audio_count[ch], samples, (size_t)n * sizeof(float));
            app->pending_audio_count[ch] += n;
            samples += n;
            num_samples -= n;
            if (app->pending_audio_count[ch] == 320) {
                process_audio_unlocked(app, channel, app->pending_audio[ch], 320);
                app->pending_audio_count[ch] = 0;
            }
        }
    }
    publish_snapshot_unlocked(app);
    pthread_mutex_unlock(&app->lock);
}

void ascolto_live_app_format_statusbar(ascolto_live_app_t* app, char* out_buf, size_t out_cap) {
    if (!app || !out_buf || out_cap == 0) return;
    ascolto_live_snapshot_t snapshot;
    ascolto_live_app_read_snapshot(app, &snapshot);
    if (snapshot.state == ASCOLTO_STATE_RECORDING) {
        char tbuf[16];
        format_mm_ss(now_monotonic_s() - snapshot.session_start_s, tbuf, sizeof(tbuf));
        snprintf(out_buf, out_cap, "🔴 %s", tbuf);
    } else if (snapshot.state == ASCOLTO_STATE_STOPPING) {
        snprintf(out_buf, out_cap, "Ascolto…");
    } else if (snapshot.state == ASCOLTO_STATE_PAUSED) {
        snprintf(out_buf, out_cap, "⏸️ Ascolto");
    } else {
        snprintf(out_buf, out_cap, "🎙️ Ascolto");
    }
}

void ascolto_live_app_free(ascolto_live_app_t* app) {
    if (!app) return;
    ascolto_live_app_stop_session(app);
    ascolto_deferred_stop(app->deferred_worker);
    app->deferred_worker = NULL;
    ascolto_archive_wait();
    pthread_mutex_lock(&app->lock);
    ascolto_online_vad_free(&app->vad_mic);
    ascolto_online_vad_free(&app->vad_sys);
    pthread_mutex_unlock(&app->lock);
    pthread_mutex_destroy(&app->lock);
    pthread_cond_destroy(&app->session_cond);
    pthread_mutex_destroy(&app->snapshot_lock);
    ascolto_local_models_destroy(app->local_models);
}

static int deferred_job_metadata(ascolto_live_app_t *app, ascolto_deferred_store_t *store,
                                 const ascolto_deferred_job_t *job) {
    sqlite3 *db = ascolto_db_sqlite(ascolto_deferred_store_database(store));
    sqlite3_stmt *q = NULL;
    const char *query = "SELECT json_extract(?1,'$.in_app'),json_extract(?1,'$.out_app'),"
        "json_extract(?1,'$.mic'),json_extract(?1,'$.transcript_md'),json_extract(?1,'$.audio_m4a'),"
        "json_extract(?1,'$.retain_audio'),json_extract(?1,'$.cloud_recap'),json_extract(?1,'$.recap_tts'),"
        "json_extract(?1,'$.recap_python')";
    if (sqlite3_prepare_v2(db, query, -1, &q, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(q, 1, job->metadata_json ? job->metadata_json : "{}", -1, SQLITE_STATIC);
    int rc = sqlite3_step(q);
    if (rc == SQLITE_ROW) {
        char *dest[] = {app->active_in_apps, app->active_out_apps, app->active_mic_name,
            app->files.md_path, app->files.m4a_path};
        size_t cap[] = {sizeof(app->active_in_apps), sizeof(app->active_out_apps), sizeof(app->active_mic_name),
            sizeof(app->files.md_path), sizeof(app->files.m4a_path)};
        for (int i = 0; i < 5; i++) {
            const char *value = (const char *)sqlite3_column_text(q, i);
            if (value && strlen(value) >= cap[i]) { rc = SQLITE_TOOBIG; break; }
            snprintf(dest[i], cap[i], "%s", value ? value : "");
        }
        app->files.retain_audio = sqlite3_column_int(q, 5) != 0;
        app->config.cloud_recap = sqlite3_column_int(q, 6) != 0;
        app->config.recap_tts = sqlite3_column_int(q, 7) != 0;
        const char *python = (const char *)sqlite3_column_text(q, 8);
        if (python && strlen(python) < sizeof(app->config.recap_python))
            snprintf(app->config.recap_python, sizeof(app->config.recap_python), "%s", python);
    }
    sqlite3_finalize(q);
    return rc == SQLITE_ROW ? 0 : -1;
}

int ascolto_deferred_process_job(ascolto_live_app_t *app, ascolto_deferred_store_t *store,
    const ascolto_deferred_job_t *job, bool (*should_stop)(void *), void *stop_context) {
    if (!app || !store || !job || !should_stop || job->sample_count < 0 ||
        !app->whistle || !app->whistle->loaded) return -1;
    FILE *source[2] = {NULL, NULL};
    const char *paths[2] = {job->mic_path, job->sys_path};
    struct stat original[2];
    int result = -1;
    for (int ch = 0; ch < 2; ch++) {
        int fd = open(paths[ch], O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) goto done;
        if (fstat(fd, &original[ch]) || !S_ISREG(original[ch].st_mode) ||
            original[ch].st_size % sizeof(float) != 0 ||
            original[ch].st_size / (int64_t)sizeof(float) > job->sample_count) { close(fd); goto done; }
        source[ch] = fdopen(fd, "rb");
        if (!source[ch]) { close(fd); goto done; }
    }
    memset(&app->files, 0, sizeof(app->files));
    app->files.database_session = job->session_id;
    if (deferred_job_metadata(app, store, job)) goto done;
    snprintf(app->config.language, sizeof(app->config.language), "%s", job->language);
    app->config.diarizer_threshold = (float)job->diarizer_threshold;
    app->transcription_error = false;
    app->session_utterance_count = 0;
    app->mic_bleed_frames_suppressed = app->mic_echo_turns_suppressed = 0;
    app->last_sys_rms_db = app->last_mic_rms_db = -100;
    app->last_sys_voiced_stream_s = -10;
    ascolto_echo_filter_reset(&app->echo);
    ascolto_diarizer_init(&app->diarizer, app->config.diarizer_threshold);
    ascolto_online_vad_free(&app->vad_mic);
    ascolto_online_vad_free(&app->vad_sys);
    ascolto_vad_config_t mic = ascolto_vad_default_config(), sys = ascolto_vad_default_config();
    mic.energy_threshold_db = -35.5f; mic.min_speech_s = .30; mic.min_silence_s = .32; mic.max_segment_s = 12;
    sys.energy_threshold_db = -35; sys.min_speech_s = .35; sys.min_silence_s = .38; sys.max_segment_s = 10;
    if (ascolto_online_vad_init(&app->vad_mic, ASCOLTO_CHANNEL_MIC, &mic) ||
        ascolto_online_vad_init(&app->vad_sys, ASCOLTO_CHANNEL_SYS, &sys)) goto done;
    struct ascolto_deferred_replay_state replay = {.store = store, .job = job,
        .should_stop = should_stop, .stop_context = stop_context};
    app->deferred_replay = &replay;
    app->state = ASCOLTO_STATE_RECORDING;
    for (int64_t position = 0; position < job->sample_count; position += ASCOLTO_FRAME_SAMPLES) {
        if (should_stop(stop_context)) { result = 1; goto done; }
        int n = (int)(job->sample_count - position > ASCOLTO_FRAME_SAMPLES ? ASCOLTO_FRAME_SAMPLES : job->sample_count - position);
        replay.position = position + n;
        for (int ch = ASCOLTO_CHANNEL_SYS; ch >= ASCOLTO_CHANNEL_MIC; ch--) {
            float samples[ASCOLTO_FRAME_SAMPLES] = {0};
            size_t count = fread(samples, sizeof(float), (size_t)n, source[ch]);
            if (count < (size_t)n && ferror(source[ch])) goto done;
            for (size_t i = 0; i < count; i++) if (!isfinite(samples[i])) goto done;
            process_audio_unlocked(app, (ascolto_channel_t)ch, samples, n);
            if (replay.interrupted) { result = 1; goto done; }
            if (app->transcription_error) goto done;
        }
        if (replay.position % ASCOLTO_SAMPLE_RATE == 0 &&
            ascolto_deferred_progress(store, job->id, replay.position) != SQLITE_OK) goto done;
    }
    ascolto_online_vad_flush(&app->vad_sys, on_live_vad_turn, app);
    ascolto_online_vad_flush(&app->vad_mic, on_live_vad_turn, app);
    if (replay.interrupted || should_stop(stop_context)) { result = 1; goto done; }
    if (app->transcription_error || replay.ordinal < job->completed_segments) goto done;
    for (int ch = 0; ch < 2; ch++) {
        struct stat final;
        if (fstat(fileno(source[ch]), &final) || final.st_size != original[ch].st_size ||
            final.st_mtimespec.tv_sec != original[ch].st_mtimespec.tv_sec ||
            final.st_mtimespec.tv_nsec != original[ch].st_mtimespec.tv_nsec ||
            final.st_ctimespec.tv_sec != original[ch].st_ctimespec.tv_sec ||
            final.st_ctimespec.tv_nsec != original[ch].st_ctimespec.tv_nsec) goto done;
    }
    if (ascolto_deferred_progress(store, job->id, job->sample_count) != SQLITE_OK) goto done;
    /* Capture owns its end timestamp. Recovery supplies a sample-based timestamp
     * only for a process interrupted before capture could close its session. */
    sqlite3 *db = ascolto_db_sqlite(ascolto_deferred_store_database(store));
    sqlite3_stmt *q = NULL;
    if (sqlite3_prepare_v2(db, "SELECT coalesce(ended_at_ms,started_at_ms+?2),json_set(metadata_json,'$.duration_s',?3,'$.utterances',?4,'$.remote_speakers',?5,'$.transcription_failed',json('false')) FROM sessions WHERE id=?1", -1, &q, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_int64(q, 1, job->session_id);
    sqlite3_bind_int64(q, 2, job->sample_count * 1000 / ASCOLTO_SAMPLE_RATE);
    sqlite3_bind_double(q, 3, (double)job->sample_count / ASCOLTO_SAMPLE_RATE);
    sqlite3_bind_int(q, 4, app->session_utterance_count);
    sqlite3_bind_int(q, 5, app->diarizer.num_speakers);
    int rc = sqlite3_step(q);
    char *metadata = rc == SQLITE_ROW ? strdup((const char *)sqlite3_column_text(q, 1)) : NULL;
    int64_t ended = rc == SQLITE_ROW ? sqlite3_column_int64(q, 0) : 0;
    sqlite3_finalize(q);
    if (!metadata) goto done;
    rc = ascolto_db_session_state(ascolto_deferred_store_database(store), job->session_id, "completed", ended, metadata);
    free(metadata);
    if (rc || ascolto_storage_publish(ascolto_deferred_store_database(store), job->session_id)) goto done;
    result = 0;
done:
    app->deferred_replay = NULL;
    app->state = ASCOLTO_STATE_IDLE;
    for (int ch = 0; ch < 2; ch++) if (source[ch]) fclose(source[ch]);
    return result;
}

void ascolto_deferred_finalize_job(ascolto_live_app_t *app, const ascolto_deferred_job_t *job) {
    ascolto_deferred_store_t *store = NULL;
    if (ascolto_deferred_store_open(app->config.config_path, &store)) return;
    int rc = deferred_job_metadata(app, store, job);
    bool archived = false, has_turns = false, retranscription = false;
    sqlite3_stmt *q = NULL;
    sqlite3 *db = ascolto_db_sqlite(ascolto_deferred_store_database(store));
    if (!rc) rc = sqlite3_prepare_v2(db, "SELECT EXISTS(SELECT 1 FROM artifacts WHERE session_id=?1 AND kind='audio_m4a' AND state='ready' AND path=?2),EXISTS(SELECT 1 FROM utterances WHERE session_id=?1),coalesce(json_extract(?3,'$.native_transcription_source_session'),0)>0", -1, &q, NULL);
    if (rc == SQLITE_OK) {
        sqlite3_bind_int64(q, 1, job->session_id);
        sqlite3_bind_text(q, 2, app->files.m4a_path, -1, SQLITE_STATIC);
        sqlite3_bind_text(q, 3, job->metadata_json ? job->metadata_json : "{}", -1, SQLITE_STATIC);
        if (sqlite3_step(q) == SQLITE_ROW) { archived = sqlite3_column_int(q, 0); has_turns = sqlite3_column_int(q, 1); retranscription = sqlite3_column_int(q, 2); }
        else rc = -1;
    }
    sqlite3_finalize(q);
    ascolto_deferred_store_close(store);
    if (rc || !app->files.md_path[0]) return;
    snprintf(app->config.language, sizeof(app->config.language), "%s", job->language);
    if (retranscription) {
        /* A requested new local version does not regenerate notes or upload audio.
         * These paths belong only to its private decoded working copy. */
        unlink(job->mic_path);
        unlink(job->sys_path);
        return;
    }
    /* Recap preferences remain independently opt-in. The worker is launched only
     * after all native transcript checkpoints and exports have succeeded. */
    if (has_turns) ascolto_ai_recap_start(app->config.config_path, app->files.md_path, true);
    /* A prior successful compression may have crashed between the two unlinks.
     * A committed readable archive is authoritative; never overwrite it by
     * attempting compression with just one remaining channel. */
    if (archived && access(app->files.m4a_path, R_OK) == 0) {
        unlink(job->mic_path); unlink(job->sys_path);
        ascolto_ai_task_start(app->config.config_path, app->files.md_path, "automatic_audio");
        return;
    }
    if (app->files.retain_audio) {
        if (ascolto_ai_automatic_enabled(app->config.config_path)) app->config.cloud_recap = false;
        ascolto_archive_finalize(&app->config, job->mic_path, job->sys_path, app->files.m4a_path, app->files.md_path);
    } else {
        unlink(job->mic_path);
        unlink(job->sys_path);
    }
}
