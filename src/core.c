#define _POSIX_C_SOURCE 200809L
#include "ascolto/storage.h"
#include "ascolto/ai_settings.h"
#include "ascolto/filesystem.h"
#include "ascolto/core.h"
#include "ascolto/json.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

uint64_t ascolto_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

static void add_recent_file(ascolto_core_t *core, const char *path) {
    if (!core || !path || !*path) return;
    for(size_t index=0;index<core->recent_files_count;index++){
        if(!strcmp(core->recent_files[index],path)){
            memmove(&core->recent_files[index],&core->recent_files[index+1],(core->recent_files_count-index-1)*sizeof(core->recent_files[0]));
            core->recent_files_count--;break;
        }
    }
    if (core->recent_files_count == ASCOLTO_MAX_RECENT_FILES) {
        for (size_t i = 1; i < ASCOLTO_MAX_RECENT_FILES; i++) {
            snprintf(core->recent_files[i - 1], sizeof(core->recent_files[i - 1]), "%s", core->recent_files[i]);
        }
        core->recent_files_count--;
    }
    snprintf(core->recent_files[core->recent_files_count++], sizeof(core->recent_files[0]), "%s", path);
}

static void scan_existing_transcripts(ascolto_core_t *core) {
    if (!core || !core->config.output_dir[0]) return;
    ascolto_mkdir_p(core->config.output_dir);
    DIR *dir = opendir(core->config.output_dir);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t len = strlen(ent->d_name);
        char full[512];
        if (len > 3 && strcmp(ent->d_name + len - 3, ".md") == 0) {
            snprintf(full, sizeof(full), "%.240s/%.240s", core->config.output_dir, ent->d_name);
            add_recent_file(core, full);
        } else if (strncmp(ent->d_name, "call_", 5) == 0 || (len > 11 && ent->d_name[4] == '-' && ent->d_name[10] == '_')) {
            snprintf(full, sizeof(full), "%.240s/%.240s/transcript.md", core->config.output_dir, ent->d_name);
            if (access(full, R_OK) == 0) add_recent_file(core, full);
        }
    }
    closedir(dir);
}

static void process_and_commit_utterance_unlocked(
    ascolto_core_t *core,
    ascolto_channel_t channel,
    const float *pcm,
    size_t num_samples,
    double start_sec,
    double end_sec,
    const char *hint_text
) {
    if (!core || !pcm || num_samples < (ASCOLTO_SAMPLE_RATE / 4) || core->transcription_error) return;

    char lang[16];
    char keywords[1024];
    pthread_mutex_lock(&core->mutex);
    snprintf(lang, sizeof(lang), "%s", core->config.language);
    snprintf(keywords, sizeof(keywords), "%s", core->config.keywords);
    pthread_mutex_unlock(&core->mutex);

    ascolto_whistle_result_t wres;
    if (core->local_stt.context) {
        ascolto_stt_text_t text;
        if (ascolto_local_stt_transcribe(core->local_models, &core->local_stt, pcm,
            (int)num_samples, lang, keywords, &text)) { core->transcription_error = true; return; }
        ascolto_whistle_embed_segment(&core->whistle, pcm, num_samples, &wres);
        snprintf(wres.text, sizeof(wres.text), "%s", text.text);
        snprintf(wres.language, sizeof(wres.language), "%s", text.language);
    } else {
        ascolto_whistle_process_segment(&core->whistle, pcm, num_samples, lang, keywords, hint_text, &wres);
    }

    /* Mock IPC speech is a formant fixture, not a recording of spoken words.
       Its explicit hint supplies text while the real encoder still supplies embeddings. */
    if (core->audio.use_mock && hint_text && *hint_text)
        snprintf(wres.text, sizeof(wres.text), "%s", hint_text);
    if (wres.text[0] == '\0') {
        ascolto_whistle_result_free(&wres);
        return;
    }

    float turn_db = ascolto_audio_rms_db(pcm, (int)num_samples);
    bool recent_sys = core->last_sys_voiced_sample >= 0 &&
        core->vad[ASCOLTO_CHANNEL_SYS].total_samples_processed - core->last_sys_voiced_sample < 35200;
    ascolto_mic_activity_t activity = core->echo.mic;
    if (channel == ASCOLTO_CHANNEL_MIC) activity = ascolto_mic_activity_take(&core->echo.mic);
    bool reject = ascolto_transcript_is_noise(wres.text, 0, 1.0f, channel, turn_db, recent_sys);
    if (channel == ASCOLTO_CHANNEL_MIC &&
        ascolto_echo_filter_matches(&core->echo, wres.text, start_sec, end_sec, turn_db, &activity)) reject = true;
    if (reject) { ascolto_whistle_result_free(&wres); return; }
    if (channel == ASCOLTO_CHANNEL_SYS) {
        ascolto_echo_filter_remember(&core->echo, wres.text, start_sec, end_sec, turn_db);
    }

    float emb[ASCOLTO_EMBED_DIM];
    float pitch_hz = 0.0f;
    float rms_db = -100.0f;
    ascolto_diarizer_compute_embedding(
        pcm,
        num_samples,
        wres.frame_embeddings,
        wres.frame_embedding_floats,
        wres.frame_embedding_dim,
        emb,
        &pitch_hz,
        &rms_db
    );

    pthread_mutex_lock(&core->mutex);
    char speaker_label[32] = {0};
    float sim = 1.0f;
    int spk_id = ascolto_diarizer_assign(
        &core->diarizer,
        channel,
        emb,
        pitch_hz,
        start_sec,
        speaker_label,
        &sim
    );

    core->total_segments++;
    ascolto_segment_t seg;
    memset(&seg, 0, sizeof(seg));
    seg.seq_id = core->total_segments;
    seg.channel = channel;
    seg.start_sec = start_sec;
    seg.end_sec = end_sec;
    snprintf(seg.speaker_label, sizeof(seg.speaker_label), "%s", speaker_label);
    seg.speaker_id = spk_id;
    seg.speaker_similarity = sim;
    snprintf(seg.text, sizeof(seg.text), "%s", wres.text);
    snprintf(seg.language, sizeof(seg.language), "%s", wres.language[0] ? wres.language : core->local_stt.context ? "" : core->config.language);
    seg.ttft_ms = wres.ttft_ms;
    seg.decode_tps = wres.decode_tps;
    seg.rms_db = rms_db;
    seg.pitch_hz = pitch_hz;

    if (core->segments_count < ASCOLTO_MAX_SEGMENTS_MEM) {
        core->segments[core->segments_count++] = seg;
    } else {
        memmove(&core->segments[0], &core->segments[1], (ASCOLTO_MAX_SEGMENTS_MEM - 1) * sizeof(ascolto_segment_t));
        core->segments[ASCOLTO_MAX_SEGMENTS_MEM - 1] = seg;
    }

    ascolto_db_utterance_t turn={.session_id=core->database_session,.start_ms=(int64_t)(seg.start_sec*1000+0.5),.end_ms=(int64_t)(seg.end_sec*1000+0.5),.channel=ascolto_channel_to_string(seg.channel),.speaker_label=seg.speaker_label,.text=seg.text,.language=seg.language,.source_app=core->active_mic_app};
    char timing[32];if(core->local_stt.context)snprintf(timing,sizeof(timing),"null");else snprintf(timing,sizeof(timing),"%.1f",seg.ttft_ms);
    char diagnostics[512];snprintf(diagnostics,sizeof(diagnostics),"{\"seq_id\":%llu,\"speaker_id\":%d,\"similarity\":%.3f,\"pitch_hz\":%.1f,\"rms_db\":%.1f,\"ttft_ms\":%s,\"local_model\":\"%s\"}",(unsigned long long)seg.seq_id,seg.speaker_id,seg.speaker_similarity,seg.pitch_hz,seg.rms_db,timing,core->local_stt.id);
    turn.metadata_json=diagnostics;
    int storage_result=ascolto_storage_write(core->config.config_path,&core->storage_writer,&turn);
    core->storage_error=storage_result;
    pthread_mutex_unlock(&core->mutex);

    char *event = NULL;
    size_t event_size = 0;
    FILE *event_file = open_memstream(&event, &event_size);
    if (event_file) {
        fprintf(event_file, "\"seq_id\":%llu,\"channel\":\"%s\",\"speaker\":\"%s\",\"speaker_id\":%d,"
                           "\"similarity\":%.3f,\"pitch_hz\":%.1f,\"text\":",
                (unsigned long long)seg.seq_id, ascolto_channel_to_string(seg.channel),
                seg.speaker_label, seg.speaker_id, seg.speaker_similarity, seg.pitch_hz);
        ascolto_write_json_string(event_file, seg.text);
        fclose(event_file);
        ascolto_obs_emit(&core->obs, "segment_committed", event);
        free(event);
    }

    ascolto_whistle_result_free(&wres);
}

static void on_vad_turn(ascolto_channel_t channel, const float *pcm, int count,
    double start, double end, float rms_db, void *context) {
    (void)rms_db;
    ascolto_core_t *core = context;
    char hint[512];
    snprintf(hint, sizeof(hint), "%s", core->vad_hint[channel]);
    core->vad_hint[channel][0] = '\0';
    process_and_commit_utterance_unlocked(core, channel, pcm, (size_t)count, start, end,
                                        hint[0] ? hint : NULL);
}

void ascolto_core_flush_channel(ascolto_core_t *core, ascolto_channel_t channel) {
    if (!core || channel < 0 || channel > ASCOLTO_CHANNEL_SYS) return;
    pthread_mutex_lock(&core->processing_mutex);
    ascolto_online_vad_flush(&core->vad[channel], on_vad_turn, core);
    core->vad_hint[channel][0] = '\0';
    pthread_mutex_unlock(&core->processing_mutex);
}

static int start_session_unlocked(ascolto_core_t *core, ascolto_state_t target_state, const char *reason) {
    char mic_dev[128], sys_dev[128];

    pthread_mutex_lock(&core->mutex);
    if (core->state != ASCOLTO_STATE_IDLE || core->session_starting) {
        int result = core->state == ASCOLTO_STATE_STOPPING || core->session_starting ? -2 : 0;
        pthread_mutex_unlock(&core->mutex);
        return result;
    }

    core->session_starting = true;
    pthread_mutex_unlock(&core->mutex);
    int local_result = ascolto_local_stt_begin(core->local_models, &core->local_stt);
    pthread_mutex_lock(&core->mutex);
    if (local_result) goto start_failed;
    core->transcription_error = false;

    if(core->storage_writer.head && ascolto_storage_flush(core->config.config_path,&core->storage_writer)){goto start_failed;}
    memset(&core->storage_writer,0,sizeof(core->storage_writer));
    core->session_id++;
    if(ascolto_storage_call_directory(core->config.output_dir,core->active_mic_app,core->current_session_dir,sizeof(core->current_session_dir)-64)){goto start_failed;}
    snprintf(core->current_md_path, sizeof(core->current_md_path), "%.447s/transcript.md", core->current_session_dir);
    snprintf(core->current_jsonl_path, sizeof(core->current_jsonl_path), "%.447s/Data/transcript.jsonl", core->current_session_dir);

    for (int ch = 0; ch < 2; ch++) {
        core->current_audio_path[ch][0] = '\0';
        core->recorder[ch] = NULL;
        if (!core->config.save_audio && !core->config.cloud_recap) continue;
        snprintf(core->current_audio_path[ch], sizeof(core->current_audio_path[ch]),
                 "%.447s/Audio/%s.flac", core->current_session_dir, ch == ASCOLTO_CHANNEL_MIC ? "mic" : "sys");
        core->recorder[ch] = ascolto_audio_recorder_open(core->current_audio_path[ch]);
        if (!core->recorder[ch]) core->current_audio_path[ch][0] = '\0';
    }

    char *vocabulary=ascolto_storage_vocabulary(core->config.keywords);
    if(!vocabulary){goto start_failed;}
    char initial_metadata[128];snprintf(initial_metadata,sizeof(initial_metadata),"{\"local_model\":\"%s\"}",core->local_stt.id);
    ascolto_db_session_t stored={.metadata_json=initial_metadata,.started_at_ms=ascolto_db_now_ms(),.capture_backend="pipewire",.source_app=core->active_mic_app,.vocabulary_snapshot_json=vocabulary};
    core->storage_error=ascolto_storage_session(core->config.config_path,&stored,core->current_md_path,core->current_jsonl_path,&core->database_session);
    free(vocabulary);
    if(core->storage_error){for(int ch=0;ch<2;ch++){ascolto_audio_recorder_close(core->recorder[ch]);core->recorder[ch]=NULL;}goto start_failed;}
    for(int ch=0;ch<2;ch++)if(core->current_audio_path[ch][0])ascolto_storage_artifact(core->config.config_path,core->database_session,ch?"audio_sys_flac":"audio_mic_flac",core->current_audio_path[ch],"pending");

    add_recent_file(core, core->current_md_path);

    ascolto_diarizer_reset(&core->diarizer);
    ascolto_diarizer_set_sensitivity(&core->diarizer, core->config.diarization_sensitivity);

    for (int ch = 0; ch < 2; ch++) {
        ascolto_online_vad_reset(&core->vad[ch]);
        core->vad_hint[ch][0] = '\0';
        core->session_audio_sec[ch] = 0.0;
    }

    ascolto_echo_filter_reset(&core->echo);
    core->last_sys_voiced_sample = -1;
    core->session_start_ms = ascolto_monotonic_ms();
    core->session_paused_accum_ms = 0;
    core->pause_start_ms = 0;
    core->state = target_state;
    core->session_starting = true;

    snprintf(mic_dev, sizeof(mic_dev), "%s", core->config.mic_device);
    snprintf(sys_dev, sizeof(sys_dev), "%s", core->config.sys_device);
    uint64_t sid = core->session_id;
    pthread_mutex_unlock(&core->mutex);

    /* Immediately mark tray UI dirty before starting capture */
    ascolto_tray_update(&core->tray);

    int capture_result = core->audio.start_capture(&core->audio, mic_dev, sys_dev);
    pthread_mutex_lock(&core->mutex);
    core->session_starting = false;
    pthread_cond_broadcast(&core->session_condition);
    pthread_mutex_unlock(&core->mutex);
    if (capture_result) {
        ascolto_core_stop(core);
        return capture_result;
    }

    char ev[512];
    snprintf(ev, sizeof(ev), "\"session_id\":%llu,\"state\":\"%s\",\"reason\":\"%s\"",
             (unsigned long long)sid,
             ascolto_state_to_string(target_state),
             reason ? reason : "manual");
    ascolto_obs_emit(&core->obs, "session_started", ev);
    return 0;
start_failed:
    for (int ch = 0; ch < 2; ++ch) { ascolto_audio_recorder_close(core->recorder[ch]); core->recorder[ch] = NULL; }
    core->session_starting = false;
    pthread_cond_broadcast(&core->session_condition);
    pthread_mutex_unlock(&core->mutex);
    ascolto_local_stt_end(core->local_models, &core->local_stt);
    return -1;
}

static int stop_session_unlocked(ascolto_core_t *core, const char *reason) {
    pthread_mutex_lock(&core->mutex);
    while (core->session_starting) pthread_cond_wait(&core->session_condition, &core->mutex);
    if (core->state == ASCOLTO_STATE_STOPPING) {
        uint64_t generation = core->session_id;
        while (core->state == ASCOLTO_STATE_STOPPING && core->session_id == generation)
            pthread_cond_wait(&core->session_condition, &core->mutex);
        pthread_mutex_unlock(&core->mutex);
        return 0;
    }
    if (core->state == ASCOLTO_STATE_IDLE) {
        pthread_mutex_unlock(&core->mutex);
        return 0;
    }
    core->drain_recording = core->state != ASCOLTO_STATE_PAUSED;
    core->state = ASCOLTO_STATE_STOPPING;
    ascolto_storage_state(core->config.config_path,core->database_session,"stopping",NULL);
    pthread_mutex_unlock(&core->mutex);
    ascolto_tray_update(&core->tray);

    /* Stop capture threads WITHOUT holding core->mutex */
    core->audio.stop_capture(&core->audio);

    /* Flush any remaining voiced utterances (>= 0.25s) */
    ascolto_core_flush_channel(core, ASCOLTO_CHANNEL_SYS);
    ascolto_core_flush_channel(core, ASCOLTO_CHANNEL_MIC);

    ascolto_audio_recorder_t *recorders[2];
    pthread_mutex_lock(&core->mutex);
    for (int ch = 0; ch < 2; ch++) {
        recorders[ch] = core->recorder[ch];
        core->recorder[ch] = NULL;
    }
    pthread_mutex_unlock(&core->mutex);
    bool had_audio = recorders[0] && recorders[1];
    for (int ch = 0; ch < 2; ch++) ascolto_audio_recorder_close(recorders[ch]);

    pthread_mutex_lock(&core->mutex);
    core->storage_error=ascolto_storage_flush(core->config.config_path,&core->storage_writer);
    char stored_metadata[256];snprintf(stored_metadata,sizeof(stored_metadata),"{\"duration_s\":%.3f,\"remote_speakers\":%d,\"local_model\":\"%s\",\"transcription_failed\":%s}",fmax(core->session_audio_sec[0],core->session_audio_sec[1]),core->diarizer.speaker_count,core->local_stt.id,core->transcription_error?"true":"false");
    if(ascolto_storage_state(core->config.config_path,core->database_session,core->storage_error||core->transcription_error?"failed":"completed",stored_metadata))core->storage_error=1;
    for(int ch=0;ch<2;ch++)if(core->current_audio_path[ch][0])ascolto_storage_artifact(core->config.config_path,core->database_session,ch?"audio_sys_flac":"audio_mic_flac",core->current_audio_path[ch],"ready");
    uint64_t sid = core->session_id;
    int spk_cnt = core->diarizer.speaker_count;
    uint64_t tot_seg = core->total_segments;
    bool use_ai = ascolto_ai_automatic_enabled(core->config.config_path);
    bool run_recap = core->config.cloud_recap && had_audio && !use_ai && !core->transcription_error;
    char recap_md[512];
    snprintf(recap_md, sizeof(recap_md), "%s", core->current_md_path);
    pthread_mutex_unlock(&core->mutex);
    if (!core->storage_error && !core->transcription_error && tot_seg > 0) ascolto_ai_recap_start(core->config.config_path, recap_md, true);
    if (!core->storage_error && !core->transcription_error && had_audio) ascolto_ai_task_start(core->config.config_path, recap_md, "automatic_audio");
    if (run_recap) ascolto_core_run_recap(core, recap_md);
    ascolto_local_stt_end(core->local_models, &core->local_stt);
    pthread_mutex_lock(&core->mutex);
    core->state = ASCOLTO_STATE_IDLE;
    core->drain_recording = false;
    pthread_cond_broadcast(&core->session_condition);
    pthread_mutex_unlock(&core->mutex);

    ascolto_tray_update(&core->tray);

    char ev[512];
    snprintf(ev, sizeof(ev), "\"session_id\":%llu,\"reason\":\"%s\",\"speakers\":%d,\"segments\":%llu",
             (unsigned long long)sid,
             reason ? reason : "stop",
             spk_cnt,
             (unsigned long long)tot_seg);
    ascolto_obs_emit(&core->obs, "session_stopped", ev);
    return 0;
}

static void on_audio_chunk_cb(
    void *user_data,
    ascolto_channel_t channel,
    const float *pcm_16k,
    size_t num_samples,
    double stream_time_sec,
    const char *hint_text
) {
    ascolto_core_t *core = (ascolto_core_t *)user_data;
    ascolto_core_ingest_audio(core, channel, pcm_16k, num_samples, stream_time_sec, hint_text);
}

static void on_mic_trigger_cb(void *user_data, bool mic_active, const char *client_app_name) {
    ascolto_core_t *core = (ascolto_core_t *)user_data;
    ascolto_core_notify_mic_trigger(core, mic_active, client_app_name);
}

int ascolto_core_init(ascolto_core_t *core, const ascolto_config_t *cfg, bool use_mock_audio) {
    if (!core || !cfg) return -1;
    memset(core, 0, sizeof(*core));
    core->config = *cfg;
    if(ascolto_config_save(&core->config,NULL))return -1;
    core->state = ASCOLTO_STATE_IDLE;
    core->running = true;
    pthread_mutex_init(&core->mutex, NULL);
    pthread_mutex_init(&core->processing_mutex, NULL);
    pthread_mutex_init(&core->config_mutex, NULL);
    pthread_cond_init(&core->session_condition, NULL);

    for (int ch = 0; ch < 2; ch++) {
        ascolto_vad_config_t vad_config = ascolto_vad_default_config();
        vad_config.energy_threshold_db = ch == ASCOLTO_CHANNEL_MIC ? -35.5f : -35.0f;
        vad_config.max_segment_s = 10.0;
        if (ascolto_online_vad_init(&core->vad[ch], (ascolto_channel_t)ch, &vad_config)) {
            for (int previous = 0; previous < ch; previous++) ascolto_online_vad_free(&core->vad[previous]);
            pthread_cond_destroy(&core->session_condition);
            pthread_mutex_destroy(&core->processing_mutex);
            pthread_mutex_destroy(&core->config_mutex);
            pthread_mutex_destroy(&core->mutex);
            return -1;
        }
    }

    scan_existing_transcripts(core);
    /* Restore persisted archive paths before initializing GTK or the control server. */
    if(ascolto_storage_latest_transcript(core->config.config_path,core->current_md_path,sizeof(core->current_md_path),core->current_jsonl_path,sizeof(core->current_jsonl_path)))return -1;
    if(core->current_md_path[0]){
        add_recent_file(core,core->current_md_path);
        snprintf(core->current_session_dir,sizeof(core->current_session_dir),"%s",core->current_md_path);
        char *slash=strrchr(core->current_session_dir,'/');if(slash)*slash=0;
    }

    ascolto_diarizer_init(&core->diarizer, core->config.diarization_sensitivity);
    core->diarizer.custom_threshold = core->config.custom_threshold;

    core->local_models = ascolto_local_models_create(core->config.config_path);
    if (!core->local_models) return -1;
    ascolto_whistle_init(&core->whistle, core->config.weights_path);
    ascolto_audio_backend_create(&core->audio, use_mock_audio, on_audio_chunk_cb, on_mic_trigger_cb, core);
    ascolto_tray_init(&core->tray, core, core->config.headless);
    core->audio.start_watcher(&core->audio);
    ascolto_obs_init(&core->obs, core, core->config.control_socket, core->config.telemetry_log);

    char ev[256];
    snprintf(ev, sizeof(ev), "\"whistle_loaded\":%s,\"embed_dim\":%zu,\"mock_audio\":%s",
             core->whistle.model_loaded ? "true" : "false",
             core->whistle.embed_dim,
             use_mock_audio ? "true" : "false");
    ascolto_obs_emit(&core->obs, "core_initialized", ev);
    return 0;
}

void ascolto_core_destroy(ascolto_core_t *core) {
    if (!core) return;
    ascolto_tray_wait_actions(&core->tray);
    stop_session_unlocked(core, "shutdown");

    ascolto_tray_destroy(&core->tray);
    ascolto_obs_destroy(&core->obs);
    core->audio.destroy(&core->audio);
    ascolto_whistle_destroy(&core->whistle);
    ascolto_local_models_destroy(core->local_models);

    for (int ch = 0; ch < 2; ch++) {
        ascolto_online_vad_free(&core->vad[ch]);
    }
    pthread_cond_destroy(&core->session_condition);
    pthread_mutex_destroy(&core->mutex);
    pthread_mutex_destroy(&core->processing_mutex);
    pthread_mutex_destroy(&core->config_mutex);
}

int ascolto_core_start_manual(ascolto_core_t *core) {
    if (!core) return -1;
    return start_session_unlocked(core, ASCOLTO_STATE_RECORDING_MANUAL, "manual_start");
}

int ascolto_core_stop(ascolto_core_t *core) {
    if (!core) return -1;
    return stop_session_unlocked(core, "manual_stop");
}

int ascolto_core_pause(ascolto_core_t *core) {
    if (!core) return -1;
    pthread_mutex_lock(&core->mutex);
    if (core->state == ASCOLTO_STATE_RECORDING_AUTO || core->state == ASCOLTO_STATE_RECORDING_MANUAL) {
        core->pre_pause_state = core->state;
        core->state = ASCOLTO_STATE_PAUSED;
        ascolto_storage_state(core->config.config_path,core->database_session,"paused",NULL);
        core->pause_start_ms = ascolto_monotonic_ms();
        ascolto_obs_emit(&core->obs, "session_paused", NULL);
    }
    pthread_mutex_unlock(&core->mutex);
    ascolto_tray_update(&core->tray);
    return 0;
}

int ascolto_core_resume(ascolto_core_t *core) {
    if (!core) return -1;
    pthread_mutex_lock(&core->mutex);
    if (core->state == ASCOLTO_STATE_PAUSED) {
        uint64_t now = ascolto_monotonic_ms();
        if (now > core->pause_start_ms) {
            core->session_paused_accum_ms += (now - core->pause_start_ms);
        }
        core->state = (core->pre_pause_state != ASCOLTO_STATE_IDLE)
                          ? core->pre_pause_state
                          : ASCOLTO_STATE_RECORDING_MANUAL;
        ascolto_storage_state(core->config.config_path,core->database_session,"recording",NULL);
        ascolto_obs_emit(&core->obs, "session_resumed", NULL);
    }
    pthread_mutex_unlock(&core->mutex);
    ascolto_tray_update(&core->tray);
    return 0;
}

int ascolto_core_set_auto_mic(ascolto_core_t *core, bool enabled) {
    if (!core) return -1;
    bool should_start = false;
    bool should_stop = false;

    pthread_mutex_lock(&core->config_mutex);
    pthread_mutex_lock(&core->mutex);
    ascolto_config_t previous_config = core->config;
    core->config.auto_mic_trigger = enabled;
    ascolto_config_t candidate_config = core->config;
    pthread_mutex_unlock(&core->mutex);
    int persist_result = ascolto_config_save(&candidate_config, NULL);
    pthread_mutex_lock(&core->mutex);
    if (persist_result) {
        core->config = previous_config;
        pthread_mutex_unlock(&core->mutex); pthread_mutex_unlock(&core->config_mutex);
        ascolto_tray_update(&core->tray); return -1;
    }
    if (enabled && core->external_mic_active && core->state == ASCOLTO_STATE_IDLE) {
        should_start = true;
    } else if (!enabled && core->state == ASCOLTO_STATE_RECORDING_AUTO) {
        should_stop = true;
    }
    pthread_mutex_unlock(&core->mutex);
    pthread_mutex_unlock(&core->config_mutex);

    if (should_start) {
        start_session_unlocked(core, ASCOLTO_STATE_RECORDING_AUTO, "auto_mic_enabled_while_active");
    } else if (should_stop) {
        stop_session_unlocked(core, "auto_mic_disabled");
    }
    ascolto_tray_update(&core->tray);
    return 0;
}

int ascolto_core_set_save_audio(ascolto_core_t *core, bool enabled) {
    if (!core) return -1;
    pthread_mutex_lock(&core->config_mutex);
    pthread_mutex_lock(&core->mutex);
    ascolto_config_t previous_config = core->config;
    core->config.save_audio = enabled;
    ascolto_config_t candidate_config = core->config;
    pthread_mutex_unlock(&core->mutex);
    int persist_result = ascolto_config_save(&candidate_config, NULL);
    pthread_mutex_lock(&core->mutex);
    if (persist_result) {
        core->config = previous_config;
        pthread_mutex_unlock(&core->mutex); pthread_mutex_unlock(&core->config_mutex);
        ascolto_tray_update(&core->tray); return -1;
    }
    char ev[64];
    snprintf(ev, sizeof(ev), "\"save_audio\":%s", enabled ? "true" : "false");
    ascolto_obs_emit(&core->obs, "config_save_audio_changed", ev);
    pthread_mutex_unlock(&core->mutex);
    pthread_mutex_unlock(&core->config_mutex);
    ascolto_tray_update(&core->tray);
    return 0;
}

int ascolto_core_set_cloud_recap(ascolto_core_t *core, bool enabled) {
    if (!core) return -1;
    pthread_mutex_lock(&core->config_mutex);
    pthread_mutex_lock(&core->mutex);
    ascolto_config_t previous_config = core->config;
    core->config.cloud_recap = enabled;
    ascolto_config_t candidate_config = core->config;
    pthread_mutex_unlock(&core->mutex);
    int persist_result = ascolto_config_save(&candidate_config, NULL);
    pthread_mutex_lock(&core->mutex);
    if (persist_result) {
        core->config = previous_config;
        pthread_mutex_unlock(&core->mutex); pthread_mutex_unlock(&core->config_mutex);
        ascolto_tray_update(&core->tray); return -1;
    }
    char ev[64];
    snprintf(ev, sizeof(ev), "\"cloud_recap\":%s", enabled ? "true" : "false");
    ascolto_obs_emit(&core->obs, "config_cloud_recap_changed", ev);
    pthread_mutex_unlock(&core->mutex);
    pthread_mutex_unlock(&core->config_mutex);
    ascolto_tray_update(&core->tray);
    return 0;
}

int ascolto_core_set_recap_tts(ascolto_core_t *core, bool enabled) {
    if (!core) return -1;
    pthread_mutex_lock(&core->config_mutex);
    pthread_mutex_lock(&core->mutex);
    ascolto_config_t previous_config = core->config;
    core->config.recap_tts = enabled;
    ascolto_config_t candidate_config = core->config;
    pthread_mutex_unlock(&core->mutex);
    int persist_result = ascolto_config_save(&candidate_config, NULL);
    pthread_mutex_lock(&core->mutex);
    if (persist_result) {
        core->config = previous_config;
        pthread_mutex_unlock(&core->mutex); pthread_mutex_unlock(&core->config_mutex);
        ascolto_tray_update(&core->tray); return -1;
    }
    char ev[64];
    snprintf(ev, sizeof(ev), "\"recap_tts\":%s", enabled ? "true" : "false");
    ascolto_obs_emit(&core->obs, "config_recap_tts_changed", ev);
    pthread_mutex_unlock(&core->mutex);
    pthread_mutex_unlock(&core->config_mutex);
    ascolto_tray_update(&core->tray);
    return 0;
}

int ascolto_core_set_recap_lang(ascolto_core_t *core, const char *lang) {
    if (!core || !lang || !*lang) return -1;
    pthread_mutex_lock(&core->config_mutex);
    pthread_mutex_lock(&core->mutex);
    ascolto_config_t previous_config = core->config;
    snprintf(core->config.recap_lang, sizeof(core->config.recap_lang), "%s", lang);
    ascolto_config_t candidate_config = core->config;
    pthread_mutex_unlock(&core->mutex);
    int persist_result = ascolto_config_save(&candidate_config, NULL);
    pthread_mutex_lock(&core->mutex);
    if (persist_result) {
        core->config = previous_config;
        pthread_mutex_unlock(&core->mutex); pthread_mutex_unlock(&core->config_mutex);
        ascolto_tray_update(&core->tray); return -1;
    }
    char ev[64];
    snprintf(ev, sizeof(ev), "\"recap_lang\":\"%s\"", core->config.recap_lang);
    ascolto_obs_emit(&core->obs, "config_recap_lang_changed", ev);
    pthread_mutex_unlock(&core->mutex);
    pthread_mutex_unlock(&core->config_mutex);
    ascolto_tray_update(&core->tray);
    return 0;
}

int ascolto_core_set_tts_lang(ascolto_core_t *core, const char *lang) {
    if (!core || !lang || !*lang) return -1;
    pthread_mutex_lock(&core->config_mutex);
    pthread_mutex_lock(&core->mutex);
    ascolto_config_t previous_config = core->config;
    snprintf(core->config.tts_lang, sizeof(core->config.tts_lang), "%s", lang);
    ascolto_config_t candidate_config = core->config;
    pthread_mutex_unlock(&core->mutex);
    int persist_result = ascolto_config_save(&candidate_config, NULL);
    pthread_mutex_lock(&core->mutex);
    if (persist_result) {
        core->config = previous_config;
        pthread_mutex_unlock(&core->mutex); pthread_mutex_unlock(&core->config_mutex);
        ascolto_tray_update(&core->tray); return -1;
    }
    char ev[64];
    snprintf(ev, sizeof(ev), "\"tts_lang\":\"%s\"", core->config.tts_lang);
    ascolto_obs_emit(&core->obs, "config_tts_lang_changed", ev);
    pthread_mutex_unlock(&core->mutex);
    pthread_mutex_unlock(&core->config_mutex);
    ascolto_tray_update(&core->tray);
    return 0;
}

int ascolto_core_set_language(ascolto_core_t *core, const char *lang) {
    if (!core || !lang) return -1;
    pthread_mutex_lock(&core->config_mutex);
    pthread_mutex_lock(&core->mutex);
    ascolto_config_t previous_config = core->config;
    snprintf(core->config.language, sizeof(core->config.language), "%s", lang);
    ascolto_config_t candidate_config = core->config;
    pthread_mutex_unlock(&core->mutex);
    int persist_result = ascolto_config_save(&candidate_config, NULL);
    pthread_mutex_lock(&core->mutex);
    if (persist_result) {
        core->config = previous_config;
        pthread_mutex_unlock(&core->mutex); pthread_mutex_unlock(&core->config_mutex);
        ascolto_tray_update(&core->tray); return -1;
    }
    char ev[128];
    snprintf(ev, sizeof(ev), "\"language\":\"%s\"", core->config.language);
    ascolto_obs_emit(&core->obs, "config_language_changed", ev);
    pthread_mutex_unlock(&core->mutex);
    pthread_mutex_unlock(&core->config_mutex);
    ascolto_tray_update(&core->tray);
    return 0;
}

int ascolto_core_set_output_directory(ascolto_core_t *core, const char *path) {
    if (!core || !path || path[0] != '/' || strlen(path) >= sizeof(core->config.output_dir) || ascolto_mkdir_p(path)) return -1;
    pthread_mutex_lock(&core->config_mutex);
    pthread_mutex_lock(&core->mutex);
    ascolto_config_t candidate = core->config;
    pthread_mutex_unlock(&core->mutex);
    snprintf(candidate.output_dir, sizeof(candidate.output_dir), "%s", path);
    int result = ascolto_config_save(&candidate, NULL);
    pthread_mutex_lock(&core->mutex);
    if (!result) snprintf(core->config.output_dir, sizeof(core->config.output_dir), "%s", path);
    pthread_mutex_unlock(&core->mutex);
    pthread_mutex_unlock(&core->config_mutex);
    ascolto_tray_update(&core->tray);
    return result;
}

static int set_capture_device(ascolto_core_t *core, ascolto_channel_t channel, const char *device) {
    if (!core || !device) return -1;
    char mic[128], sys[128], event[256];
    pthread_mutex_lock(&core->config_mutex);
    pthread_mutex_lock(&core->mutex);
    ascolto_config_t previous_config = core->config;
    bool is_mic = channel == ASCOLTO_CHANNEL_MIC;
    char *selected = is_mic ? core->config.mic_device : core->config.sys_device;
    snprintf(selected, sizeof(core->config.mic_device), "%s", device);
    snprintf(mic, sizeof(mic), "%s", core->config.mic_device);
    snprintf(sys, sizeof(sys), "%s", core->config.sys_device);
    ascolto_config_t candidate_config = core->config;
    pthread_mutex_unlock(&core->mutex);
    int persist_result = ascolto_config_save(&candidate_config, NULL);
    pthread_mutex_lock(&core->mutex);
    if (persist_result) {
        core->config = previous_config;
        pthread_mutex_unlock(&core->mutex); pthread_mutex_unlock(&core->config_mutex);
        ascolto_tray_update(&core->tray); return -1;
    }
    bool restart = core->state != ASCOLTO_STATE_IDLE;
    snprintf(event, sizeof(event), "\"%s_device\":\"%s\"", is_mic ? "mic" : "sys", selected);
    ascolto_obs_emit(&core->obs, is_mic ? "config_mic_changed" : "config_sys_changed", event);
    pthread_mutex_unlock(&core->mutex);
    pthread_mutex_unlock(&core->config_mutex);

    if (restart) {
        core->audio.stop_capture(&core->audio);
        int result = core->audio.start_capture(&core->audio, mic, sys);
        pthread_mutex_lock(&core->mutex);
        core->session_starting = false;
        pthread_cond_broadcast(&core->session_condition);
        pthread_mutex_unlock(&core->mutex);
        if (result) { ascolto_core_stop(core); return result; }
    }
    ascolto_tray_update(&core->tray);
    return 0;
}

int ascolto_core_set_mic_device(ascolto_core_t *core, const char *device) {
    return set_capture_device(core, ASCOLTO_CHANNEL_MIC, device);
}

int ascolto_core_set_sys_device(ascolto_core_t *core, const char *device) {
    return set_capture_device(core, ASCOLTO_CHANNEL_SYS, device);
}

int ascolto_core_set_sensitivity(ascolto_core_t *core, ascolto_sensitivity_t sens) {
    if (!core) return -1;
    pthread_mutex_lock(&core->config_mutex);
    pthread_mutex_lock(&core->mutex);
    ascolto_config_t previous_config = core->config;
    core->config.diarization_sensitivity = sens;
    ascolto_diarizer_set_sensitivity(&core->diarizer, sens);
    ascolto_config_t candidate_config = core->config;
    pthread_mutex_unlock(&core->mutex);
    int persist_result = ascolto_config_save(&candidate_config, NULL);
    pthread_mutex_lock(&core->mutex);
    if (persist_result) {
        core->config = previous_config;
        pthread_mutex_unlock(&core->mutex); pthread_mutex_unlock(&core->config_mutex);
        ascolto_tray_update(&core->tray); return -1;
    }
    char ev[128];
    snprintf(ev, sizeof(ev), "\"sensitivity\":\"%s\",\"threshold\":%.3f",
             ascolto_sensitivity_to_string(sens),
             ascolto_sensitivity_threshold(sens, core->config.custom_threshold));
    ascolto_obs_emit(&core->obs, "config_sensitivity_changed", ev);
    pthread_mutex_unlock(&core->mutex);
    pthread_mutex_unlock(&core->config_mutex);
    ascolto_tray_update(&core->tray);
    return 0;
}

int ascolto_core_set_keywords(ascolto_core_t *core, const char *keywords) {
    if (!core || !keywords) return -1;
    pthread_mutex_lock(&core->config_mutex);
    pthread_mutex_lock(&core->mutex);
    ascolto_config_t previous_config = core->config;
    snprintf(core->config.keywords, sizeof(core->config.keywords), "%s", keywords);
    ascolto_config_t candidate_config = core->config;
    pthread_mutex_unlock(&core->mutex);
    int persist_result = ascolto_config_save(&candidate_config, NULL);
    pthread_mutex_lock(&core->mutex);
    if (persist_result) {
        core->config = previous_config;
        pthread_mutex_unlock(&core->mutex); pthread_mutex_unlock(&core->config_mutex);
        ascolto_tray_update(&core->tray); return -1;
    }
    char ev[512];
    snprintf(ev, sizeof(ev), "\"keywords\":\"%.450s\"", core->config.keywords);
    ascolto_obs_emit(&core->obs, "config_keywords_changed", ev);
    pthread_mutex_unlock(&core->mutex);
    pthread_mutex_unlock(&core->config_mutex);
    ascolto_tray_update(&core->tray);
    return 0;
}

void ascolto_core_notify_mic_trigger(ascolto_core_t *core, bool active, const char *app_name) {
    if (!core) return;
    bool should_start = false;
    bool should_stop = false;
    char reason_app[128] = {0};

    pthread_mutex_lock(&core->mutex);
    core->external_mic_active = active;
    core->last_mic_change_ms = ascolto_monotonic_ms();
    snprintf(core->active_mic_app, sizeof(core->active_mic_app), "%s", (app_name && *app_name) ? app_name : "");
    snprintf(reason_app, sizeof(reason_app), "%s", core->active_mic_app[0] ? core->active_mic_app : "auto_mic");

    char ev[256];
    snprintf(ev, sizeof(ev), "\"mic_active\":%s,\"app\":\"%s\"",
             active ? "true" : "false", core->active_mic_app);
    ascolto_obs_emit(&core->obs, "mic_trigger_event", ev);

    if (core->config.auto_mic_trigger) {
        if (active && core->state == ASCOLTO_STATE_IDLE) {
            should_start = true;
        } else if (!active && core->state == ASCOLTO_STATE_RECORDING_AUTO) {
            should_stop = true;
        }
    }
    pthread_mutex_unlock(&core->mutex);

    if (should_start) {
        start_session_unlocked(core, ASCOLTO_STATE_RECORDING_AUTO, reason_app);
    } else if (should_stop) {
        stop_session_unlocked(core, "auto_mic_released");
    }
    ascolto_tray_update(&core->tray);
}

void ascolto_core_ingest_audio(ascolto_core_t *core, ascolto_channel_t channel,
    const float *pcm, size_t count, double stream_time_sec, const char *hint) {
    if (!core || !pcm || !count || channel < 0 || channel > ASCOLTO_CHANNEL_SYS) return;
    pthread_mutex_lock(&core->processing_mutex);
    pthread_mutex_lock(&core->mutex);
    bool recording = core->state == ASCOLTO_STATE_RECORDING_AUTO || core->state == ASCOLTO_STATE_RECORDING_MANUAL ||
                     (core->state == ASCOLTO_STATE_STOPPING && core->drain_recording);
    ascolto_audio_recorder_t *recorder = core->recorder[channel];
    pthread_mutex_unlock(&core->mutex);
    if (!recording) { pthread_mutex_unlock(&core->processing_mutex); return; }
    int64_t cursor = core->vad[channel].total_samples_processed;
    int64_t start = stream_time_sec >= 0 ? llround(stream_time_sec * ASCOLTO_SAMPLE_RATE) : cursor;
    if (start < cursor) {
        int64_t skipped = cursor - start;
        if ((uint64_t)skipped >= count) { pthread_mutex_unlock(&core->processing_mutex); return; }
        pcm += skipped;
        count -= (size_t)skipped;
        start = cursor;
    }
    int64_t gap = start - cursor;
    for (size_t offset = 0; gap || offset < count;) {
        float zeros[ASCOLTO_FRAME_SAMPLES] = {0};
        bool padding = gap > 0;
        int64_t remaining = padding ? gap : (int64_t)(count - offset);
        int step = remaining > ASCOLTO_FRAME_SAMPLES ? ASCOLTO_FRAME_SAMPLES : (int)remaining;
        const float *frame = padding ? zeros : pcm + offset;
        if (recorder) ascolto_audio_recorder_write(recorder, frame, (size_t)step);
        float db = ascolto_audio_rms_db(frame, step);
        float reference = -100.0f;
        bool active = false;
        if (channel == ASCOLTO_CHANNEL_SYS) {
            core->echo.sys_db[core->echo.sys_frames++ % ASCOLTO_SYS_ENV_HISTORY] = db;
            if (db >= -36.5f) core->last_sys_voiced_sample = core->vad[channel].total_samples_processed + step;
        } else {
            active = ascolto_bleed_reference(core->echo.sys_db, core->echo.sys_frames,
                core->vad[channel].total_samples_processed + step, core->vad[ASCOLTO_CHANNEL_SYS].total_samples_processed,
                &reference);
        }
        if (!padding && hint && *hint && !core->vad_hint[channel][0])
            snprintf(core->vad_hint[channel], sizeof(core->vad_hint[channel]), "%s", hint);
        bool bleed = channel == ASCOLTO_CHANNEL_MIC && active && db < reference + 3.5f;
        ascolto_online_vad_feed(&core->vad[channel], bleed ? zeros : frame, step, on_vad_turn, core);
        if (channel == ASCOLTO_CHANNEL_MIC && !bleed) {
            ascolto_mic_activity_track(&core->echo.mic, core->vad[channel].in_speech, active, reference);
        }
        if (padding) gap -= step;
        else offset += (size_t)step;
    }
    pthread_mutex_lock(&core->mutex);
    core->session_audio_sec[channel] = (double)core->vad[channel].total_samples_processed / ASCOLTO_SAMPLE_RATE;
    pthread_mutex_unlock(&core->mutex);
    pthread_mutex_unlock(&core->processing_mutex);
}

uint64_t ascolto_core_elapsed_sec(ascolto_core_t *core) {
    if (!core || core->state == ASCOLTO_STATE_IDLE) return 0;
    uint64_t now = ascolto_monotonic_ms();
    uint64_t paused = core->session_paused_accum_ms;
    if (core->state == ASCOLTO_STATE_PAUSED && now > core->pause_start_ms) {
        paused += (now - core->pause_start_ms);
    }
    if (now <= core->session_start_ms + paused) return 0;
    return (now - core->session_start_ms - paused) / 1000ULL;
}

void ascolto_core_tick(ascolto_core_t *core) {
    if (!core) return;
    ascolto_tray_step_iteration(&core->tray);
    ascolto_tray_update(&core->tray);
    ascolto_tray_step_iteration(&core->tray);
}
