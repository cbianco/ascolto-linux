#include "ascolto.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    FILE* trace_file;
    int mic_events;
    bool mic_currently_active;
} session_trace_ctx_t;

static void json_escape_print(FILE* f, const char* s) {
    if (!f || !s) return;
    while (*s) {
        if (*s == '"' || *s == '\\') {
            fputc('\\', f);
            fputc(*s, f);
        } else if (*s == '\n') {
            fputs("\\n", f);
        } else if (*s == '\r') {
            fputs("\\r", f);
        } else if (*s == '\t') {
            fputs("\\t", f);
        } else {
            fputc(*s, f);
        }
        s++;
    }
}

static void on_simulated_mic_event(bool is_active, double timestamp_s, void* userdata) {
    session_trace_ctx_t* ctx = (session_trace_ctx_t*)userdata;
    if (!ctx) return;
    ctx->mic_events++;
    ctx->mic_currently_active = is_active;
    if (ctx->trace_file) {
        fprintf(
            ctx->trace_file,
            "{\"event\":\"mic_state_change\",\"timestamp_s\":%.3f,\"mic_active\":%s}\n",
            timestamp_s,
            is_active ? "true" : "false"
        );
    }
}

static int compare_utterances(const void* a, const void* b) {
    const ascolto_utterance_t* ua = (const ascolto_utterance_t*)a;
    const ascolto_utterance_t* ub = (const ascolto_utterance_t*)b;
    if (ua->start_s < ub->start_s) return -1;
    if (ua->start_s > ub->start_s) return 1;
    return 0;
}

int ascolto_run_simulated_session(
    ascolto_whistle_t* whistle,
    const float* mic_pcm,
    int mic_samples,
    const float* sys_pcm,
    int sys_samples,
    const char* language,
    const char* keywords,
    float diarizer_threshold,
    FILE* trace_jsonl_out,
    ascolto_session_report_t* out_report
) {
    if (!whistle || !whistle->loaded || !out_report) return -1;
    memset(out_report, 0, sizeof(*out_report));

    int max_samples = (mic_samples > sys_samples) ? mic_samples : sys_samples;
    double duration_s = (double)max_samples / (double)ASCOLTO_SAMPLE_RATE;
    out_report->total_duration_s = duration_s;

    session_trace_ctx_t trace_ctx = {
        .trace_file = trace_jsonl_out,
        .mic_events = 0,
        .mic_currently_active = false
    };

    /* 1. Simulate Mic Monitor activation -> session capture -> deactivation */
    ascolto_mic_monitor_t* mon = ascolto_mic_monitor_create_simulated(
        0.0,
        duration_s,
        on_simulated_mic_event,
        &trace_ctx
    );
    if (!mon) return -2;

    mon->step(mon, 0.0); /* Triggers mic_active = true at t=0.0 */

    /* 2. Apply 20ms Frame-Level Speaker Bleed Gate (with automatic drift alignment for legacy archives)
          and segment both channels using VAD */
    const int frame_len = ASCOLTO_SAMPLE_RATE / 50; /* 20 ms = 320 samples */
    float* gated_mic = NULL;
    int* lag_per_frame = NULL;
    int n_m_frames = (mic_samples > 0) ? ((mic_samples + frame_len - 1) / frame_len) : 0;
    int n_s_frames = (sys_samples > 0) ? ((sys_samples + frame_len - 1) / frame_len) : 0;
    float* m_env = NULL;
    float* s_env = NULL;

    if (mic_pcm && mic_samples > 0) {
        gated_mic = (float*)malloc((size_t)mic_samples * sizeof(float));
        if (gated_mic) {
            memcpy(gated_mic, mic_pcm, (size_t)mic_samples * sizeof(float));
        }
    }

    if (gated_mic && mic_samples > 0 && sys_pcm && sys_samples > 0 && n_m_frames > 0 && n_s_frames > 0) {
        m_env = (float*)malloc((size_t)n_m_frames * sizeof(float));
        s_env = (float*)malloc((size_t)n_s_frames * sizeof(float));
        lag_per_frame = (int*)calloc((size_t)n_m_frames, sizeof(int));

        if (m_env && s_env && lag_per_frame) {
            for (int i = 0; i < n_m_frames; i++) {
                int s0 = i * frame_len;
                int step = (s0 + frame_len <= mic_samples) ? frame_len : (mic_samples - s0);
                m_env[i] = ascolto_audio_rms_db(mic_pcm + s0, step);
            }
            for (int i = 0; i < n_s_frames; i++) {
                int s0 = i * frame_len;
                int step = (s0 + frame_len <= sys_samples) ? frame_len : (sys_samples - s0);
                s_env[i] = ascolto_audio_rms_db(sys_pcm + s0, step);
            }

            /* Check if this is a synchronized stream (lag ~ 0) or a legacy archive with startup/callback drift */
            int probe_len = (n_m_frames < 350) ? n_m_frames : 350;
            double err_sync = 1e9;
            for (int l = 0; l <= 8 && l < probe_len; l++) {
                double e = 0.0;
                int cnt = 0;
                for (int tm = l; tm < probe_len && (tm - l) < n_s_frames; tm++) {
                    float mv = (m_env[tm] > -45.0f) ? m_env[tm] : -45.0f;
                    float sv = (s_env[tm - l] > -45.0f) ? s_env[tm - l] : -45.0f;
                    e += fabs((double)mv - (double)sv);
                    cnt++;
                }
                if (cnt > 20 && (e / cnt) < err_sync) err_sync = e / cnt;
            }

            int best_init = 0;
            double best_init_err = err_sync;
            for (int l = 45; l <= 100 && l + 100 < probe_len; l++) {
                double e = 0.0;
                int cnt = 0;
                int voiced_s = 0;
                for (int tm = l; tm < probe_len && (tm - l) < n_s_frames; tm++) {
                    float mv = (m_env[tm] > -45.0f) ? m_env[tm] : -45.0f;
                    float sv = (s_env[tm - l] > -45.0f) ? s_env[tm - l] : -45.0f;
                    if (sv > -35.0f) voiced_s++;
                    e += fabs((double)mv - (double)sv);
                    cnt++;
                }
                if (cnt > 40 && voiced_s >= 15 && (e / cnt) < best_init_err) {
                    best_init_err = e / cnt;
                    best_init = l;
                }
            }

            bool has_legacy_drift = (best_init >= 45 && best_init_err < err_sync - 2.5 && best_init_err < 6.5);
            int cur_lag = has_legacy_drift ? best_init : 0;
            int total_diff = (n_m_frames > n_s_frames) ? (n_m_frames - n_s_frames) : 0;

            for (int t0 = 0; t0 < n_m_frames; t0 += 50) {
                int t1 = (t0 + 150 < n_m_frames) ? (t0 + 150) : n_m_frames;
                if (has_legacy_drift && t0 >= best_init) {
                    int m_voiced = 0;
                    for (int tm = t0; tm < t1; tm++) {
                        if (m_env[tm] >= -34.0f) m_voiced++;
                    }
                    if (m_voiced >= 15) {
                        int best_l = cur_lag;
                        double best_err = 1e9;
                        int max_l = cur_lag + 120;
                        if (max_l > total_diff + 120 && total_diff > best_init) max_l = total_diff + 120;
                        if (max_l > 2000) max_l = 2000;
                        for (int l = (cur_lag > 2 ? cur_lag - 2 : 0); l <= max_l; l++) {
                            int ts0 = t0 - l;
                            int ts1 = t1 - l;
                            if (ts0 < 0 || ts1 > n_s_frames) continue;
                            int s_voiced = 0;
                            for (int ts = ts0; ts < ts1; ts++) {
                                if (s_env[ts] >= -34.0f) s_voiced++;
                            }
                            if (s_voiced < 15) continue;
                            double e = 0.0;
                            for (int tm = t0; tm < t1; tm++) {
                                float mv = (m_env[tm] > -45.0f) ? m_env[tm] : -45.0f;
                                float sv = (s_env[tm - l] > -45.0f) ? s_env[tm - l] : -45.0f;
                                e += fabs((double)mv - (double)sv);
                            }
                            e /= (double)(t1 - t0);
                            if (e < best_err) {
                                best_err = e;
                                best_l = l;
                            }
                        }
                        if (best_err < 5.8) {
                            cur_lag = best_l;
                        }
                    }
                }
                for (int tm = t0; tm < t0 + 50 && tm < n_m_frames; tm++) {
                    lag_per_frame[tm] = cur_lag;
                }
            }

            /* Apply 20ms frame-level Speaker Bleed Gate */
            for (int tm = 0; tm < n_m_frames; tm++) {
                int s0 = tm * frame_len;
                int step = (s0 + frame_len <= mic_samples) ? frame_len : (mic_samples - s0);
                if (has_legacy_drift && tm < best_init) {
                    memset(gated_mic + s0, 0, (size_t)step * sizeof(float));
                    continue;
                }
                float mdb = m_env[tm];
                if (mdb < -35.5f) {
                    continue;
                }
                int ts = tm - lag_per_frame[tm];
                int w0 = (ts - 11 > 0) ? (ts - 11) : 0;
                int w1 = (ts + 1 < n_s_frames) ? (ts + 1) : n_s_frames;
                float s_peak = -100.0f;
                int s_voiced = 0;
                for (int k = w0; k < w1; k++) {
                    if (s_env[k] > s_peak) s_peak = s_env[k];
                    if (s_env[k] >= -36.5f) s_voiced++;
                }

                bool is_bleed = s_voiced > 0 && mdb < s_peak + 3.5f;

                if (is_bleed) {
                    memset(gated_mic + s0, 0, (size_t)step * sizeof(float));
                }
            }
        }
    }

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

    ascolto_vad_segment_t raw_segs[ASCOLTO_MAX_SEGMENTS];
    int n_raw = 0;

    if (gated_mic && mic_samples > 0) {
        int n_mic_segs = ascolto_vad_segment_buffer(
            gated_mic,
            mic_samples,
            ASCOLTO_CHANNEL_MIC,
            &mic_vad_cfg,
            raw_segs + n_raw,
            ASCOLTO_MAX_SEGMENTS - n_raw
        );
        /* Adjust MIC segment timestamps when legacy drift was detected so MIC & SYS align */
        if (lag_per_frame) {
            for (int k = 0; k < n_mic_segs; k++) {
                int mid_fr = ((raw_segs[n_raw + k].start_sample + raw_segs[n_raw + k].end_sample) / 2) / frame_len;
                if (mid_fr >= 0 && mid_fr < n_m_frames && lag_per_frame[mid_fr] >= 45) {
                    double shift_s = (double)lag_per_frame[mid_fr] * 0.02;
                    raw_segs[n_raw + k].start_s -= shift_s;
                    raw_segs[n_raw + k].end_s -= shift_s;
                    if (raw_segs[n_raw + k].start_s < 0.0) raw_segs[n_raw + k].start_s = 0.0;
                    if (raw_segs[n_raw + k].end_s < raw_segs[n_raw + k].start_s + 0.1) {
                        raw_segs[n_raw + k].end_s = raw_segs[n_raw + k].start_s + 0.1;
                    }
                }
            }
        }
        n_raw += n_mic_segs;
    }
    if (sys_pcm && sys_samples > 0 && n_raw < ASCOLTO_MAX_SEGMENTS) {
        n_raw += ascolto_vad_segment_buffer(
            sys_pcm,
            sys_samples,
            ASCOLTO_CHANNEL_SYS,
            &sys_vad_cfg,
            raw_segs + n_raw,
            ASCOLTO_MAX_SEGMENTS - n_raw
        );
    }

    /* Sort raw segments by start_s (preferring SYS before MIC on ties so echo reference is ready) */
    for (int i = 0; i < n_raw - 1; i++) {
        for (int j = i + 1; j < n_raw; j++) {
            bool swap = false;
            if (raw_segs[j].start_s < raw_segs[i].start_s - 0.15) {
                swap = true;
            } else if (fabs(raw_segs[j].start_s - raw_segs[i].start_s) <= 0.15 &&
                       raw_segs[j].channel == ASCOLTO_CHANNEL_SYS &&
                       raw_segs[i].channel == ASCOLTO_CHANNEL_MIC) {
                swap = true;
            }
            if (swap) {
                ascolto_vad_segment_t tmp = raw_segs[i];
                raw_segs[i] = raw_segs[j];
                raw_segs[j] = tmp;
            }
        }
    }

    ascolto_diarizer_t diarizer;
    ascolto_diarizer_init(&diarizer, diarizer_threshold);

    /* 3. Process each segment through Whistle ASR + Whistle Embed + Diarizer */
    for (int i = 0; i < n_raw && out_report->num_utterances < ASCOLTO_MAX_SEGMENTS; i++) {
        const ascolto_vad_segment_t* seg = &raw_segs[i];
        const float* src = (seg->channel == ASCOLTO_CHANNEL_MIC) ? gated_mic : sys_pcm;
        const float* seg_pcm = src + seg->start_sample;
        int seg_len = seg->end_sample - seg->start_sample;

        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);

        ascolto_transcript_result_t tr;
        int tok = ascolto_whistle_transcribe(
            whistle,
            seg_pcm,
            seg_len,
            language,
            keywords,
            true,
            &tr
        );

        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed_ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                            (double)(t1.tv_nsec - t0.tv_nsec) / 1e6;
        out_report->total_transcribe_ms += elapsed_ms;

        bool recent_sys = false;
        for (int k = out_report->num_utterances - 1; k >= 0; k--) {
            if (out_report->utterances[k].channel == ASCOLTO_CHANNEL_SYS) {
                if (fabs(seg->start_s - out_report->utterances[k].end_s) < 2.2 ||
                    (seg->start_s <= out_report->utterances[k].end_s && seg->end_s >= out_report->utterances[k].start_s)) {
                    recent_sys = true;
                }
                break;
            }
        }

        if (tok < 0 || tr.text[0] == '\0' || ascolto_is_noise_hallucination(&tr, seg->channel, seg->rms_db, recent_sys)) {
            if (trace_jsonl_out) {
                fprintf(
                    trace_jsonl_out,
                    "{\"event\":\"vad_silence_skipped\",\"channel\":\"%s\",\"start_s\":%.2f,\"end_s\":%.2f,\"rms_db\":%.1f}\n",
                    ascolto_channel_name(seg->channel),
                    seg->start_s,
                    seg->end_s,
                    seg->rms_db
                );
            }
            continue;
        }

        /* Shift word timestamps by segment offset */
        for (int w = 0; w < tr.num_words; w++) {
            tr.words[w].start_s += seg->start_s;
            tr.words[w].end_s += seg->start_s;
        }

        /* Extract Whistle encoder embeddings + acoustic voiceprint */
        float* emb_frames = NULL;
        int n_emb_frames = 0;
        ascolto_whistle_embed_frames(whistle, seg_pcm, seg_len, &emb_frames, &n_emb_frames);

        ascolto_voiceprint_t vp;
        ascolto_extract_voiceprint(seg_pcm, seg_len, emb_frames, n_emb_frames, &vp);
        free(emb_frames);

        /* Stage 2: Check MIC utterance against overlapping/recent SYS utterances for lexical or voiceprint echo */
        if (seg->channel == ASCOLTO_CHANNEL_MIC) {
            bool is_echo = false;
            for (int k = out_report->num_utterances - 1; k >= 0 && k >= out_report->num_utterances - 8; k--) {
                const ascolto_utterance_t* su = &out_report->utterances[k];
                if (su->channel != ASCOLTO_CHANNEL_SYS) continue;
                if (seg->start_s < su->end_s && seg->end_s > su->start_s) {
                    if (ascolto_is_lexical_echo(tr.text, su->transcript.text) &&
                        seg->rms_db < (su->rms_db + 3.5f)) {
                        is_echo = true;
                        break;
                    }
                }
            }
            if (is_echo) continue;
        }

        float sim = 1.0f;
        bool is_new = false;
        const char* spk_label = ascolto_diarizer_identify(&diarizer, seg->channel, &vp, &sim, &is_new);

        ascolto_utterance_t* u = &out_report->utterances[out_report->num_utterances++];
        memset(u, 0, sizeof(*u));
        u->start_s = (tr.num_words > 0) ? tr.words[0].start_s : seg->start_s;
        u->end_s = (tr.num_words > 0) ? tr.words[tr.num_words - 1].end_s : seg->end_s;
        u->channel = seg->channel;
        snprintf(u->speaker, sizeof(u->speaker), "%s", spk_label);
        u->speaker_similarity = sim;
        u->is_new_speaker = is_new;
        u->f0_hz = vp.f0_mean_hz;
        u->centroid_hz = vp.spectral_centroid_hz;
        u->rms_db = seg->rms_db;
        u->transcript = tr;

        if (trace_jsonl_out) {
            fprintf(
                trace_jsonl_out,
                "{\"event\":\"utterance\",\"channel\":\"%s\",\"speaker\":\"%s\","
                "\"is_new_speaker\":%s,\"similarity\":%.3f,\"start_s\":%.2f,\"end_s\":%.2f,"
                "\"f0_hz\":%.1f,\"centroid_hz\":%.1f,\"rms_db\":%.1f,"
                "\"language\":\"%s\",\"ttft_ms\":%.1f,\"decode_tps\":%.1f,\"text\":\"",
                ascolto_channel_name(u->channel),
                u->speaker,
                u->is_new_speaker ? "true" : "false",
                u->speaker_similarity,
                u->start_s,
                u->end_s,
                u->f0_hz,
                u->centroid_hz,
                u->rms_db,
                u->transcript.language,
                u->transcript.ttft_ms,
                u->transcript.decode_tps
            );
            json_escape_print(trace_jsonl_out, u->transcript.text);
            fprintf(trace_jsonl_out, "\"}\n");
        }
    }

    qsort(
        out_report->utterances,
        (size_t)out_report->num_utterances,
        sizeof(ascolto_utterance_t),
        compare_utterances
    );

    out_report->detected_sys_speakers = diarizer.num_speakers;

    free(gated_mic);
    free(lag_per_frame);
    free(m_env);
    free(s_env);

    /* 4. Simulate Mic deactivation at end of call */
    mon->step(mon, duration_s + 0.1);
    mon->destroy(mon);

    return 0;
}
