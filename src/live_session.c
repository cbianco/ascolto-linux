#include "ascolto.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

static void ensure_dir_recursive(const char* dir_path) {
    if (!dir_path || !dir_path[0]) return;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", dir_path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

/* --------------------------------------------------------------------------
 * Online Streaming VAD
 * -------------------------------------------------------------------------- */
int ascolto_online_vad_init(ascolto_online_vad_t* v, ascolto_channel_t channel, const ascolto_vad_config_t* cfg) {
    if (!v) return -1;
    memset(v, 0, sizeof(*v));
    v->channel = channel;
    v->cfg = cfg ? *cfg : ascolto_vad_default_config();
    v->capacity = ASCOLTO_SAMPLE_RATE * 30; /* 30 seconds buffer capacity */
    v->buffer = (float*)calloc((size_t)v->capacity, sizeof(float));
    if (!v->buffer) return -2;
    return 0;
}

void ascolto_online_vad_reset(ascolto_online_vad_t* v) {
    if (!v) return;
    v->write_pos = 0;
    v->in_speech = false;
    v->speech_start_sample = 0;
    v->last_voiced_sample = 0;
    v->total_samples_processed = 0;
}

void ascolto_online_vad_free(ascolto_online_vad_t* v) {
    if (!v) return;
    free(v->buffer);
    v->buffer = NULL;
    v->capacity = 0;
}

void ascolto_online_vad_feed(
    ascolto_online_vad_t* v,
    const float* samples,
    int num_samples,
    ascolto_vad_turn_cb_t cb,
    void* userdata
) {
    if (!v || !v->buffer || !samples || num_samples <= 0) return;

    const int frame_len = ASCOLTO_SAMPLE_RATE / 50; /* 20 ms = 320 samples */
    const int min_speech = (int)(v->cfg.min_speech_s * ASCOLTO_SAMPLE_RATE);
    const int min_silence = (int)(v->cfg.min_silence_s * ASCOLTO_SAMPLE_RATE);
    const int max_seg = (int)(v->cfg.max_segment_s * ASCOLTO_SAMPLE_RATE);

    int idx = 0;
    while (idx < num_samples) {
        int step = (idx + frame_len <= num_samples) ? frame_len : (num_samples - idx);
        float db = ascolto_audio_rms_db(samples + idx, step);
        bool voiced = (db >= v->cfg.energy_threshold_db);

        if (voiced || v->in_speech) {
            if (!v->in_speech) {
                v->in_speech = true;
                v->write_pos = 0;
                v->speech_start_sample = 0;
                v->last_voiced_sample = 0;
            }
            int can_copy = (v->write_pos + step <= v->capacity) ? step : (v->capacity - v->write_pos);
            if (can_copy > 0) {
                memcpy(v->buffer + v->write_pos, samples + idx, (size_t)can_copy * sizeof(float));
                v->write_pos += can_copy;
            }
            if (voiced) {
                v->last_voiced_sample = v->write_pos;
            }

            int silence_tail = v->write_pos - v->last_voiced_sample;
            bool hit_silence_end = (!voiced && silence_tail >= min_silence);
            bool hit_max_len = (v->write_pos >= max_seg);

            if (hit_silence_end || hit_max_len) {
                int speech_len = v->last_voiced_sample;
                if (speech_len >= min_speech && cb) {
                    int out_len = v->write_pos;
                    double end_s = (double)(v->total_samples_processed + step) / ASCOLTO_SAMPLE_RATE;
                    double start_s = end_s - ((double)out_len / ASCOLTO_SAMPLE_RATE);
                    if (start_s < 0.0) start_s = 0.0;
                    float seg_rms = ascolto_audio_rms_db(v->buffer, out_len);
                    cb(v->channel, v->buffer, out_len, start_s, end_s, seg_rms, userdata);
                }
                v->in_speech = false;
                v->write_pos = 0;
                v->last_voiced_sample = 0;
            }
        }

        v->total_samples_processed += step;
        idx += step;
    }
}

void ascolto_online_vad_flush(
    ascolto_online_vad_t* v,
    ascolto_vad_turn_cb_t cb,
    void* userdata
) {
    if (!v || !v->buffer) return;
    const int min_speech = (int)(v->cfg.min_speech_s * ASCOLTO_SAMPLE_RATE);
    if (v->in_speech && v->last_voiced_sample >= min_speech && cb) {
        int out_len = v->write_pos;
        double end_s = (double)v->total_samples_processed / ASCOLTO_SAMPLE_RATE;
        double start_s = end_s - ((double)out_len / ASCOLTO_SAMPLE_RATE);
        if (start_s < 0.0) start_s = 0.0;
        float seg_rms = ascolto_audio_rms_db(v->buffer, out_len);
        cb(v->channel, v->buffer, out_len, start_s, end_s, seg_rms, userdata);
    }
    v->in_speech = false;
    v->write_pos = 0;
    v->last_voiced_sample = 0;
}

/* --------------------------------------------------------------------------
 * Live Application Controller
 * -------------------------------------------------------------------------- */
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
    memcpy(snapshot->current_md_path, app->current_md_path, sizeof(snapshot->current_md_path));
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

static int extract_normalized_words(const char* text, char words[][32], int max_words) {
    if (!text || max_words <= 0) return 0;
    int count = 0;
    const char* p = text;
    while (*p && count < max_words) {
        while (*p && !isalnum((unsigned char)*p) && (unsigned char)*p < 128) p++;
        if (!*p) break;
        int len = 0;
        char w[32];
        while (*p && (isalnum((unsigned char)*p) || (unsigned char)*p >= 128)) {
            if (len + 1 < (int)sizeof(w)) {
                w[len++] = (char)tolower((unsigned char)*p);
            }
            p++;
        }
        w[len] = '\0';
        if (len >= 2) {
            snprintf(words[count++], 32, "%s", w);
        }
    }
    return count;
}

bool ascolto_is_lexical_echo(const char* mic_text, const char* sys_text) {
    if (!mic_text || !sys_text || !mic_text[0] || !sys_text[0]) return false;

    char mw[64][32];
    char sw[64][32];
    int nm = extract_normalized_words(mic_text, mw, 64);
    int ns = extract_normalized_words(sys_text, sw, 64);
    if (nm == 0 || ns == 0) return false;

    /* Join normalized words to check direct substring containment */
    char m_join[512] = {0};
    char s_join[512] = {0};
    for (int i = 0; i < nm; i++) {
        if (i > 0) strncat(m_join, " ", sizeof(m_join) - strlen(m_join) - 1);
        strncat(m_join, mw[i], sizeof(m_join) - strlen(m_join) - 1);
    }
    for (int i = 0; i < ns; i++) {
        if (i > 0) strncat(s_join, " ", sizeof(s_join) - strlen(s_join) - 1);
        strncat(s_join, sw[i], sizeof(s_join) - strlen(s_join) - 1);
    }
    if (strlen(m_join) >= 3 && (strstr(s_join, m_join) != NULL || strstr(m_join, s_join) != NULL)) {
        return true;
    }

    /* Check word-level and phonetic-stem (>=4 char) overlap */
    int matched = 0;
    for (int i = 0; i < nm; i++) {
        size_t mlen = strlen(mw[i]);
        bool hit = false;
        for (int j = 0; j < ns; j++) {
            size_t slen = strlen(sw[j]);
            if (strcmp(mw[i], sw[j]) == 0) {
                hit = true;
                break;
            }
            if (mlen >= 4 && slen >= 4) {
                if (strstr(sw[j], mw[i]) != NULL || strstr(mw[i], sw[j]) != NULL ||
                    strncmp(mw[i], sw[j], 4) == 0) {
                    hit = true;
                    break;
                }
            }
        }
        if (hit) matched++;
    }

    if ((float)matched / (float)nm >= 0.60f) {
        return true;
    }
    return false;
}

static bool has_repetition_loop(const char* text) {
    char words[96][32];
    int n = extract_normalized_words(text, words, 96);
    if (n < 6) return false;

    for (int i = 0; i < n; i++) {
        int freq = 1;
        for (int j = i + 1; j < n; j++) {
            if (strcmp(words[i], words[j]) == 0) freq++;
        }
        if (freq >= 5 && (float)freq / (float)n >= 0.45f) {
            return true;
        }
    }
    return false;
}

bool ascolto_is_noise_hallucination(
    const ascolto_transcript_result_t* tr,
    ascolto_channel_t channel,
    float rms_db,
    bool recent_sys_activity
) {
    if (!tr || tr->text[0] == '\0') return true;

    /* Reject decoder repetition loops ("Poco demo, poco demo, poco demo...") */
    if (has_repetition_loop(tr->text)) {
        return true;
    }

    /* Calculate average word probability from Whistle decoder attention */
    float avg_prob = 1.0f;
    if (tr->num_words > 0) {
        float prob_sum = 0.0f;
        for (int i = 0; i < tr->num_words; i++) {
            prob_sum += tr->words[i].probability;
        }
        avg_prob = prob_sum / (float)tr->num_words;
        if (avg_prob < 0.50f) {
            return true;
        }
    }

    bool is_classic_phantom =
        (strcmp(tr->text, "Grazie.") == 0 ||
         strcmp(tr->text, "Grazie mille.") == 0 ||
         strcmp(tr->text, "Grazie a tutti.") == 0 ||
         strcmp(tr->text, "Grazie, grazie.") == 0 ||
         strcmp(tr->text, "Grazie!") == 0 ||
         strcmp(tr->text, "Buongiorno.") == 0 ||
         strcmp(tr->text, "Buongiorno a tutti.") == 0 ||
         strcmp(tr->text, "Buonasera.") == 0 ||
         strcmp(tr->text, "Ciao a tutti, ciao a tutti.") == 0 ||
         strstr(tr->text, "Sottotitoli") != NULL ||
         strstr(tr->text, "Amara.org") != NULL);

    bool is_short_filler =
        (strcmp(tr->text, "Ok.") == 0 ||
         strcmp(tr->text, "Eh...") == 0 ||
         strcmp(tr->text, "Ehm...") == 0 ||
         strcmp(tr->text, "Oh!") == 0 ||
         strcmp(tr->text, "Allora.") == 0 ||
         strcmp(tr->text, "Allora...") == 0 ||
         strcmp(tr->text, "Io...") == 0 ||
         strcmp(tr->text, "Non.") == 0);

    if (channel == ASCOLTO_CHANNEL_MIC) {
        if (is_classic_phantom && (rms_db < -22.0f || recent_sys_activity || avg_prob < 0.78f)) {
            return true;
        }
        if (is_short_filler && (rms_db < -24.0f || recent_sys_activity || avg_prob < 0.72f)) {
            return true;
        }
    } else {
        /* On SYS, filter quiet background noise hallucinations so they don't spawn fake speakers */
        if (is_classic_phantom && (rms_db < -26.0f || avg_prob < 0.68f)) {
            return true;
        }
    }
    return false;
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

    /* Copy & reset MIC turn co-activity stats before continuing */
    int mic_total_fr = app->mic_turn_total_frames;
    int mic_sys_fr = app->mic_turn_sys_active_frames;
    float mic_sys_peak = app->mic_turn_sys_peak_db;
    if (channel == ASCOLTO_CHANNEL_MIC) {
        app->mic_turn_total_frames = 0;
        app->mic_turn_sys_active_frames = 0;
        app->mic_turn_sys_peak_db = -100.0f;
    }

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
    bool recent_sys = (dt_sys_stream < 2.2) || (mic_sys_fr > 0) || app->vad_sys.in_speech;

    if (ascolto_is_noise_hallucination(&tr, channel, rms_db, recent_sys)) {
        return;
    }

    float* emb = NULL;
    int n_frames = 0;
    ascolto_whistle_embed_frames(app->whistle, pcm, num_samples, &emb, &n_frames);

    ascolto_voiceprint_t vp;
    ascolto_extract_voiceprint(pcm, num_samples, emb, n_frames, &vp);
    free(emb);

    /* Stage 2: Turn-Level Speaker Bleed / Echo Verification for MIC ("ME") turns */
    if (channel == ASCOLTO_CHANNEL_MIC) {
        float sys_overlap_ratio = (mic_total_fr > 0) ? ((float)mic_sys_fr / (float)mic_total_fr) : 0.0f;

        /* Accepted above-margin frames identify a local interruption; do not discard it
           based on its average RMS, similar pitch, or a shared word such as "OK". */
        bool local_interruption = mic_sys_fr >= 3 && rms_db >= mic_sys_peak + 1.0f;

        /* 2b. Check against recent SYS utterances for lexical echo or remote voiceprint match */
        int n_rec = (app->recent_sys_utt_count < ASCOLTO_RECENT_SYS_UTTS)
            ? app->recent_sys_utt_count
            : ASCOLTO_RECENT_SYS_UTTS;
        for (int i = 0; i < n_rec; i++) {
            const ascolto_recent_sys_utt_t* su = &app->recent_sys_utts[i];
            bool near_in_time = (start_s <= su->end_s + 3.5 && end_s >= su->start_s - 2.0);
            if (!near_in_time) continue;

            if (!local_interruption && sys_overlap_ratio >= 0.30f &&
                ascolto_is_lexical_echo(tr.text, su->text) && rms_db < su->rms_db + 3.5f) {
                app->mic_echo_turns_suppressed++;
                return;
            }

        }
    } else {
        /* Record SYS utterance into rolling history for Stage-2 echo comparison */
        ascolto_recent_sys_utt_t* slot = &app->recent_sys_utts[app->recent_sys_utt_count % ASCOLTO_RECENT_SYS_UTTS];
        slot->start_s = start_s;
        slot->end_s = end_s;
        slot->rms_db = rms_db;
        slot->f0_hz = vp.f0_mean_hz;
        slot->vp = vp;
        snprintf(slot->text, sizeof(slot->text), "%s", tr.text);
        app->recent_sys_utt_count++;
    }

    float sim = 1.0f;
    bool is_new = false;
    const char* speaker = ascolto_diarizer_identify(&app->diarizer, channel, &vp, &sim, &is_new);

    app->session_utterance_count++;
    snprintf(app->last_speaker, sizeof(app->last_speaker), "%s", speaker);
    snprintf(app->last_utterance_text, sizeof(app->last_utterance_text), "%s", tr.text);

    char t_start[16], t_end[16];
    format_mm_ss(start_s, t_start, sizeof(t_start));
    format_mm_ss(end_s, t_end, sizeof(t_end));

    const char* src_app = (channel == ASCOLTO_CHANNEL_MIC)
        ? (app->active_in_apps[0] ? app->active_in_apps : app->active_mic_name)
        : (app->active_out_apps[0] ? app->active_out_apps : "Sistema");

    if (!app->meta_written_to_md &&
        (app->call_meta.window_title[0] != '\0' || app->call_meta.calendar_title[0] != '\0')) {
        if (app->md_file) {
            if (app->call_meta.window_title[0]) {
                fprintf(app->md_file, "> 📋 **Titolo Finestra Call**: %s\n", app->call_meta.window_title);
            }
            if (app->call_meta.calendar_title[0]) {
                fprintf(app->md_file, "> 📅 **Riunione a Calendario**: %s\n", app->call_meta.calendar_title);
            }
            if (app->call_meta.calendar_attendees[0]) {
                fprintf(app->md_file, "> 👥 **Partecipanti**: %s\n", app->call_meta.calendar_attendees);
            }
            fprintf(app->md_file, "\n");
            fflush(app->md_file);
        }
        if (app->jsonl_file) {
            fprintf(app->jsonl_file,
                    "{\"event\":\"call_metadata\",\"window_title\":\"%s\",\"calendar_title\":\"%s\",\"attendees\":\"%s\"}\n",
                    app->call_meta.window_title,
                    app->call_meta.calendar_title,
                    app->call_meta.calendar_attendees);
            fflush(app->jsonl_file);
        }
        app->meta_written_to_md = true;
    }

    if (app->md_file) {
        fprintf(app->md_file, "- **[%s - %s] %s** *(canale %s · app: %s, lingua: %s)*: %s\n",
                t_start, t_end, speaker, ascolto_channel_name(channel), src_app, tr.language, tr.text);
        fflush(app->md_file);
    }

    if (app->jsonl_file) {
        fprintf(app->jsonl_file,
                "{\"event\":\"utterance\",\"channel\":\"%s\",\"source_app\":\"%s\",\"speaker\":\"%s\","
                "\"is_new_speaker\":%s,\"similarity\":%.3f,\"start_s\":%.2f,\"end_s\":%.2f,"
                "\"f0_hz\":%.1f,\"rms_db\":%.1f,\"language\":\"%s\",\"ttft_ms\":%.1f,\"text\":\"",
                ascolto_channel_name(channel),
                src_app,
                speaker,
                is_new ? "true" : "false",
                sim,
                start_s,
                end_s,
                vp.f0_mean_hz,
                rms_db,
                tr.language,
                tr.ttft_ms);
        for (const char* p = tr.text; *p; p++) {
            if (*p == '"' || *p == '\\') fputc('\\', app->jsonl_file);
            fputc(*p, app->jsonl_file);
        }
        fprintf(app->jsonl_file, "\"}\n");
        fflush(app->jsonl_file);
    }
}

int ascolto_live_app_init(ascolto_live_app_t* app, ascolto_whistle_t* whistle, const char* config_path) {
    if (!app) return -1;
    memset(app, 0, sizeof(*app));
    pthread_mutex_init(&app->lock, NULL);
    pthread_cond_init(&app->session_cond, NULL);
    pthread_mutex_init(&app->snapshot_lock, NULL);

    ascolto_config_init_defaults(&app->config);
    ascolto_config_load(&app->config, config_path);
    ascolto_config_save(&app->config, config_path);

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
    app->last_sys_voiced_time_s = -10.0;
    app->last_sys_voiced_stream_s = -10.0;
    for (int i = 0; i < ASCOLTO_SYS_ENV_HISTORY; i++) {
        app->sys_env_db[i] = -100.0f;
    }
    app->sys_env_frames = 0;
    memset(app->pending_audio_count, 0, sizeof(app->pending_audio_count));
    app->mic_barge_in_streak = 0;
    app->mic_turn_total_frames = 0;
    app->mic_turn_sys_active_frames = 0;
    app->mic_turn_sys_peak_db = -100.0f;
    app->mic_bleed_frames_suppressed = 0;
    app->mic_echo_turns_suppressed = 0;
    app->recent_sys_utt_count = 0;
    app->capture_dropped_samples = 0;
    memset(app->capture_padded_samples, 0, sizeof(app->capture_padded_samples));
    snprintf(app->active_mic_name, sizeof(app->active_mic_name), "Predefinito di sistema");
    snprintf(app->active_call_app, sizeof(app->active_call_app), "Nessuna call attiva");
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
    if (app->active_in_apps[0]) {
        snprintf(app->active_call_app, sizeof(app->active_call_app), "%s", app->active_in_apps);
    }

    publish_snapshot_unlocked(app);
    return 0;
}

static int start_session_unlocked(ascolto_live_app_t* app, bool is_manual) {
    if (app->state == ASCOLTO_STATE_RECORDING) return 0;
    if (app->state == ASCOLTO_STATE_STOPPING) return -2;

    ensure_dir_recursive(app->config.output_dir);

    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char stamp[64];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d_%H-%M-%S", &tm_now);

    snprintf(app->current_md_path, sizeof(app->current_md_path),
             "%s/%s_call.md", app->config.output_dir, stamp);
    snprintf(app->current_jsonl_path, sizeof(app->current_jsonl_path),
             "%s/%s_call.jsonl", app->config.output_dir, stamp);
    snprintf(app->current_mic_raw_path, sizeof(app->current_mic_raw_path),
             "%s/.%s_mic.f32", app->config.output_dir, stamp);
    snprintf(app->current_sys_raw_path, sizeof(app->current_sys_raw_path),
             "%s/.%s_sys.f32", app->config.output_dir, stamp);
    snprintf(app->current_m4a_path, sizeof(app->current_m4a_path),
             "%s/%s_call.m4a", app->config.output_dir, stamp);

    app->md_file = fopen(app->current_md_path, "w");
    app->jsonl_file = fopen(app->current_jsonl_path, "w");
    if (app->config.save_audio) {
        app->mic_raw_file = fopen(app->current_mic_raw_path, "wb");
        app->sys_raw_file = fopen(app->current_sys_raw_path, "wb");
    }

    /* Use the metadata cached by the monitor. AX/EventKit queries can block and must
       never run while holding the session lock used by the menu and audio worker. */

    if (app->md_file) {
        char human_date[64];
        strftime(human_date, sizeof(human_date), "%Y-%m-%d %H:%M:%S", &tm_now);
        fprintf(app->md_file, "# Trascrizione Call — %s\n\n", human_date);
        if (app->call_meta.window_title[0]) {
            fprintf(app->md_file, "- **Titolo Finestra Call**: %s\n", app->call_meta.window_title);
        }
        if (app->call_meta.calendar_title[0]) {
            fprintf(app->md_file, "- **Riunione a Calendario**: %s\n", app->call_meta.calendar_title);
        }
        if (app->call_meta.calendar_organizer[0]) {
            fprintf(app->md_file, "- **Organizzatore**: %s\n", app->call_meta.calendar_organizer);
        }
        if (app->call_meta.calendar_attendees[0]) {
            fprintf(app->md_file, "- **Partecipanti invitati**: %s\n", app->call_meta.calendar_attendees);
        }
        if (app->call_meta.calendar_notes[0]) {
            fprintf(app->md_file, "- **Descrizione / Agenda**: %s\n", app->call_meta.calendar_notes);
        }
        fprintf(app->md_file, "- **Microfono (`ME`)**: %s\n", app->active_mic_name);
        fprintf(app->md_file, "- **App Input (`MIC`)**: %s\n", app->active_in_apps[0] ? app->active_in_apps : "Manuale");
        fprintf(app->md_file, "- **App Output (`SYS`)**: %s\n", app->active_out_apps[0] ? app->active_out_apps : "Sistema");
        fprintf(app->md_file, "- **Lingua impostata**: `%s`\n\n---\n\n", app->config.language);
        fflush(app->md_file);
        app->meta_written_to_md = (app->call_meta.window_title[0] != '\0' || app->call_meta.calendar_title[0] != '\0');
    }
    if (app->jsonl_file) {
        fprintf(app->jsonl_file,
                "{\"event\":\"session_start\",\"manual\":%s,\"mic\":\"%s\",\"in_app\":\"%s\",\"out_app\":\"%s\","
                "\"window_title\":\"%s\",\"calendar_title\":\"%s\",\"attendees\":\"%s\"}\n",
                is_manual ? "true" : "false",
                app->active_mic_name,
                app->active_in_apps,
                app->active_out_apps,
                app->call_meta.window_title,
                app->call_meta.calendar_title,
                app->call_meta.calendar_attendees);
        fflush(app->jsonl_file);
    }

    ascolto_diarizer_init(&app->diarizer, app->config.diarizer_threshold);
    ascolto_online_vad_reset(&app->vad_mic);
    ascolto_online_vad_reset(&app->vad_sys);

    for (int i = 0; i < ASCOLTO_SYS_ENV_HISTORY; i++) {
        app->sys_env_db[i] = -100.0f;
    }
    app->sys_env_frames = 0;
    memset(app->pending_audio_count, 0, sizeof(app->pending_audio_count));
    app->last_sys_rms_db = -100.0f;
    app->last_sys_voiced_time_s = -10.0;
    app->last_sys_voiced_stream_s = -10.0;
    app->mic_barge_in_streak = 0;
    app->mic_turn_total_frames = 0;
    app->mic_turn_sys_active_frames = 0;
    app->mic_turn_sys_peak_db = -100.0f;
    app->mic_bleed_frames_suppressed = 0;
    app->mic_echo_turns_suppressed = 0;
    app->recent_sys_utt_count = 0;
    app->capture_dropped_samples = 0;
    memset(app->capture_padded_samples, 0, sizeof(app->capture_padded_samples));

    app->session_generation++;
    app->session_start_epoch_s = now_monotonic_s();
    app->session_duration_s = 0.0;
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

    double elapsed = now_monotonic_s() - app->session_start_epoch_s;
    double stream_dur_s = (double)app->vad_mic.total_samples_processed / (double)ASCOLTO_SAMPLE_RATE;
    if (stream_dur_s > elapsed) elapsed = stream_dur_s;
    app->session_duration_s = elapsed;

    if (app->md_file) {
        char dur[16];
        format_mm_ss(elapsed, dur, sizeof(dur));
        fprintf(app->md_file, "\n---\n*Fine chiamata (durata: %s, frasi: %d, interlocutori remoti: %d)*\n",
                dur, app->session_utterance_count, app->diarizer.num_speakers);
        fclose(app->md_file);
        app->md_file = NULL;
    }
    if (app->jsonl_file) {
        fprintf(app->jsonl_file,
                "{\"event\":\"session_end\",\"duration_s\":%.2f,\"utterances\":%d,\"remote_speakers\":%d,\"audio_m4a\":\"%s\",\"bleed_frames\":%d,\"echo_turns\":%d,\"capture_dropped_samples\":%lld,\"padded_mic_samples\":%lld,\"padded_sys_samples\":%lld}\n",
                elapsed, app->session_utterance_count, app->diarizer.num_speakers,
                app->config.save_audio ? app->current_m4a_path : "",
                app->mic_bleed_frames_suppressed, app->mic_echo_turns_suppressed,
                (long long)app->capture_dropped_samples,
                (long long)app->capture_padded_samples[0], (long long)app->capture_padded_samples[1]);
        fclose(app->jsonl_file);
        app->jsonl_file = NULL;
    }

    if (app->mic_raw_file) {
        fclose(app->mic_raw_file);
        app->mic_raw_file = NULL;
    }
    if (app->sys_raw_file) {
        fclose(app->sys_raw_file);
        app->sys_raw_file = NULL;
    }

    /* If a brief (< 3.0s) transient session produced 0 utterances, remove the empty stub files */
    if (app->session_utterance_count == 0 && elapsed < 3.0) {
        if (app->current_md_path[0]) unlink(app->current_md_path);
        if (app->current_jsonl_path[0]) unlink(app->current_jsonl_path);
        if (app->current_mic_raw_path[0]) unlink(app->current_mic_raw_path);
        if (app->current_sys_raw_path[0]) unlink(app->current_sys_raw_path);
        app->manual_override_active = false;
        app->state = app->config.paused ? ASCOLTO_STATE_PAUSED : ASCOLTO_STATE_IDLE;
        return 0;
    }

    /* Compress dual raw streams (Ch1=MIC, Ch2=SYS) into tiny stereo 48kbps AAC .m4a (~21 MB/hour).
       Explicitly include /opt/homebrew/bin and /usr/local/bin in PATH because macOS GUI .app bundles
       launched via LaunchServices only have /usr/bin:/bin in PATH. */
    if (app->config.save_audio && app->current_mic_raw_path[0] && app->current_sys_raw_path[0]) {
        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
                 "(export PATH=\"/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:$PATH\"; "
                 "ffmpeg -y -loglevel error "
                 "-f f32le -ar 16000 -ac 1 -i \"%s\" "
                 "-f f32le -ar 16000 -ac 1 -i \"%s\" "
                 "-filter_complex \"[0:a][1:a]amerge=inputs=2[aout]\" "
                 "-map \"[aout]\" -c:a aac -b:a 48k \"%s\" && "
                 "rm -f \"%s\" \"%s\") >/dev/null 2>&1 &",
                 app->current_mic_raw_path,
                 app->current_sys_raw_path,
                 app->current_m4a_path,
                 app->current_mic_raw_path,
                 app->current_sys_raw_path);
        system(cmd);
    }

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

void ascolto_live_app_set_paused(ascolto_live_app_t* app, bool paused) {
    if (!app) return;
    pthread_mutex_lock(&app->lock);
    app->config.paused = paused;
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
    ascolto_config_save(&app->config, NULL);
    publish_snapshot_unlocked(app);
    pthread_mutex_unlock(&app->lock);
}

void ascolto_live_app_set_auto_trigger(ascolto_live_app_t* app, bool enabled) {
    if (!app) return;
    pthread_mutex_lock(&app->lock);
    app->config.auto_trigger_mic = enabled;
    ascolto_config_save(&app->config, NULL);
    if (enabled && !app->config.paused && app->mic_hardware_active && app->state == ASCOLTO_STATE_IDLE) {
        start_session_unlocked(app, false);
    }
    publish_snapshot_unlocked(app);
    pthread_mutex_unlock(&app->lock);
}

void ascolto_live_app_set_language(ascolto_live_app_t* app, const char* lang) {
    if (!app || !lang) return;
    pthread_mutex_lock(&app->lock);
    snprintf(app->config.language, sizeof(app->config.language), "%s", lang);
    ascolto_config_save(&app->config, NULL);
    publish_snapshot_unlocked(app);
    pthread_mutex_unlock(&app->lock);
}

void ascolto_live_app_set_devices(ascolto_live_app_t* app, uint32_t mic_dev_id, uint32_t sys_dev_id) {
    if (!app) return;
    pthread_mutex_lock(&app->lock);
    app->config.mic_device_id = mic_dev_id;
    app->config.sys_device_id = sys_dev_id;
    ascolto_config_save(&app->config, NULL);
    publish_snapshot_unlocked(app);
    pthread_mutex_unlock(&app->lock);
}

void ascolto_live_app_set_diarizer_threshold(ascolto_live_app_t* app, float threshold) {
    if (!app) return;
    pthread_mutex_lock(&app->lock);
    app->config.diarizer_threshold = threshold;
    app->diarizer.similarity_threshold = threshold;
    ascolto_config_save(&app->config, NULL);
    publish_snapshot_unlocked(app);
    pthread_mutex_unlock(&app->lock);
}

void ascolto_live_app_set_save_audio(ascolto_live_app_t* app, bool enabled) {
    if (!app) return;
    pthread_mutex_lock(&app->lock);
    app->config.save_audio = enabled;
    ascolto_config_save(&app->config, NULL);
    publish_snapshot_unlocked(app);
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
        double now_s = now_monotonic_s();
        float chunk_db = ascolto_audio_rms_db(samples, num_samples);
        const int frame_len = ASCOLTO_SAMPLE_RATE / 50; /* 20 ms = 320 samples */

        if (channel == ASCOLTO_CHANNEL_SYS) {
            if (app->sys_raw_file) {
                fwrite(samples, sizeof(float), (size_t)num_samples, app->sys_raw_file);
            }
            for (int idx = 0; idx < num_samples; idx += frame_len) {
                int step = (idx + frame_len <= num_samples) ? frame_len : (num_samples - idx);
                float f_db = ascolto_audio_rms_db(samples + idx, step);
                app->sys_env_db[app->sys_env_frames % ASCOLTO_SYS_ENV_HISTORY] = f_db;
                app->sys_env_frames++;

                if (f_db >= -36.5f) {
                    double sys_t = (double)(app->vad_sys.total_samples_processed + idx + step) /
                                   (double)ASCOLTO_SAMPLE_RATE;
                    if (f_db > app->last_sys_rms_db || (sys_t - app->last_sys_voiced_stream_s) > 0.8) {
                        app->last_sys_rms_db = f_db;
                    } else {
                        app->last_sys_rms_db = 0.92f * app->last_sys_rms_db + 0.08f * f_db;
                    }
                    app->last_sys_voiced_stream_s = sys_t;
                    app->last_sys_voiced_time_s = now_s;
                }
            }
            ascolto_online_vad_feed(&app->vad_sys, samples, num_samples, on_live_vad_turn, app);
        } else {
            if (app->mic_raw_file) {
                fwrite(samples, sizeof(float), (size_t)num_samples, app->mic_raw_file);
            }
            app->last_mic_rms_db = chunk_db;

            /* Stage 1: timestamp-aligned 20ms energy gate with a bounded SYS lookback. */
            for (int idx = 0; idx < num_samples; idx += frame_len) {
                int step = (idx + frame_len <= num_samples) ? frame_len : (num_samples - idx);
                float f_db = ascolto_audio_rms_db(samples + idx, step);

                double mic_t = (double)(app->vad_mic.total_samples_processed + step) /
                               (double)ASCOLTO_SAMPLE_RATE;
                double sys_t = (double)app->vad_sys.total_samples_processed /
                               (double)ASCOLTO_SAMPLE_RATE;

                /* Address SYS history by stream sample position, independent of callback sizes.
                   Only suppress MIC within 240ms of SYS speech and below SYS + 3.5 dB. */
                int64_t frame_end = (app->vad_mic.total_samples_processed + step + frame_len - 1) / frame_len;
                if (frame_end > app->sys_env_frames) frame_end = app->sys_env_frames;
                float ref_sys_db = -100.0f;
                bool sys_active_window = false;
                for (int k = 0; k < 12 && frame_end - k > 0; k++) {
                    int64_t frame = frame_end - 1 - k;
                    if (app->sys_env_frames - frame > ASCOLTO_SYS_ENV_HISTORY) break;
                    float sdb = app->sys_env_db[frame % ASCOLTO_SYS_ENV_HISTORY];
                    if (sdb >= -36.5f) {
                        sys_active_window = true;
                        if (sdb > ref_sys_db) ref_sys_db = sdb;
                    }
                }
                /* A missing reference must never gate local-only audio. */
                if (fabs(mic_t - sys_t) > 0.25) sys_active_window = false;
                bool is_bleed = sys_active_window && f_db < ref_sys_db + 3.5f;
                app->mic_barge_in_streak = (!is_bleed && sys_active_window) ? app->mic_barge_in_streak + 1 : 0;

                if (is_bleed) {
                    float zeros[320] = {0};
                    ascolto_online_vad_feed(&app->vad_mic, zeros, step, on_live_vad_turn, app);
                    if (f_db >= app->vad_mic.cfg.energy_threshold_db) {
                        app->mic_bleed_frames_suppressed++;
                    }
                } else {
                    ascolto_online_vad_feed(&app->vad_mic, samples + idx, step, on_live_vad_turn, app);
                    if (app->vad_mic.in_speech) {
                        app->mic_turn_total_frames++;
                        if (sys_active_window) {
                            app->mic_turn_sys_active_frames++;
                            if (ref_sys_db > app->mic_turn_sys_peak_db) {
                                app->mic_turn_sys_peak_db = ref_sys_db;
                            }
                        }
                    } else {
                        app->mic_turn_total_frames = 0;
                        app->mic_turn_sys_active_frames = 0;
                        app->mic_turn_sys_peak_db = -100.0f;
                    }
                }
            }
        }
        app->session_duration_s = now_s - app->session_start_epoch_s;
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
