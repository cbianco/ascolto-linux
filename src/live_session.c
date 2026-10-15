#include "ascolto.h"
#include "macos/transcript.h"
#include "ascolto/storage.h"

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

static void on_live_vad_turn(
    ascolto_channel_t channel,
    const float* pcm,
    int num_samples,
    double start_s,
    double end_s,
    float rms_db,
    void* userdata
) {
    ascolto_live_app_t* app = (ascolto_live_app_t*)userdata;
    if (!app || !app->whistle || !app->whistle->loaded) return;

    ascolto_mic_activity_t activity = app->echo.mic;
    if (channel == ASCOLTO_CHANNEL_MIC) activity = ascolto_mic_activity_take(&app->echo.mic);

    char kw_buf[2048];
    ascolto_config_load_keywords(&app->config, kw_buf, sizeof(kw_buf));

    /* Enrich Whistle keyword biasing with Calendar attendee names if available */
    if (app->call_meta.calendar_attendees[0] != '\0') {
        size_t len = strlen(kw_buf);
        if (len + 2 < sizeof(kw_buf)) {
            if (len > 0) kw_buf[len++] = '\n';
            for (const char* p = app->call_meta.calendar_attendees; *p && len + 1 < sizeof(kw_buf); p++) {
                kw_buf[len++] = (*p == ',') ? '\n' : *p;
            }
            kw_buf[len] = '\0';
        }
    }

    ascolto_transcript_result_t tr;
    int tok = ascolto_whistle_transcribe(
        app->whistle,
        pcm,
        num_samples,
        app->config.language,
        kw_buf[0] ? kw_buf : NULL,
        true,
        &tr
    );
    if (tok < 0 || tr.text[0] == '\0') return;

    double sys_stream_s = (double)app->vad_sys.total_samples_processed / (double)ASCOLTO_SAMPLE_RATE;
    double dt_sys_stream = (app->last_sys_voiced_stream_s >= 0.0)
        ? (sys_stream_s - app->last_sys_voiced_stream_s)
        : 999.0;
    if (dt_sys_stream < 0.0) dt_sys_stream = 0.0;
    bool recent_sys = (dt_sys_stream < 2.2) || (activity.sys_frames > 0) || app->vad_sys.in_speech;

    if (ascolto_is_noise_hallucination(&tr, channel, rms_db, recent_sys)) {
        return;
    }

    float* emb = NULL;
    int n_frames = 0;
    ascolto_whistle_embed_frames(app->whistle, pcm, num_samples, &emb, &n_frames);

    ascolto_voiceprint_t vp;
    ascolto_extract_voiceprint(pcm, num_samples, emb, n_frames, &vp);
    free(emb);

    if (channel == ASCOLTO_CHANNEL_MIC) {
        if (ascolto_echo_filter_matches(&app->echo, tr.text, start_s, end_s, rms_db, &activity)) {
            app->mic_echo_turns_suppressed++;
            return;
        }
    } else {
        ascolto_echo_filter_remember(&app->echo, tr.text, start_s, end_s, rms_db);
    }

    float sim = 1.0f;
    bool is_new = false;
    const char* speaker = ascolto_diarizer_identify(&app->diarizer, channel, &vp, &sim, &is_new);

    app->session_utterance_count++;
    snprintf(app->last_speaker, sizeof(app->last_speaker), "%s", speaker);
    snprintf(app->last_utterance_text, sizeof(app->last_utterance_text), "%s", tr.text);

    ascolto_transcript_write_turn(app, channel, start_s, end_s, rms_db, &vp, &tr, speaker, sim, is_new);
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

    if (ascolto_transcript_open(app, is_manual)) return -1;

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
    ascolto_online_vad_flush(&app->vad_sys, on_live_vad_turn, app);
    ascolto_online_vad_flush(&app->vad_mic, on_live_vad_turn, app);

    double elapsed = (double)(app->vad_mic.total_samples_processed > app->vad_sys.total_samples_processed ? app->vad_mic.total_samples_processed : app->vad_sys.total_samples_processed) / ASCOLTO_SAMPLE_RATE;

    ascolto_transcript_close(app, elapsed);

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
    pthread_mutex_lock(&app->lock);
    ascolto_online_vad_free(&app->vad_mic);
    ascolto_online_vad_free(&app->vad_sys);
    pthread_mutex_unlock(&app->lock);
    pthread_mutex_destroy(&app->lock);
    pthread_cond_destroy(&app->session_cond);
    pthread_mutex_destroy(&app->snapshot_lock);
}
