#include "ascolto/audio_processing.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char* ascolto_channel_name(ascolto_channel_t ch) {
    return (ch == ASCOLTO_CHANNEL_MIC) ? "MIC" : "SYS";
}

float ascolto_audio_rms_db(const float* samples, int num_samples) {
    if (!samples || num_samples <= 0) return -100.0f;
    double sum_sq = 0.0;
    for (int i = 0; i < num_samples; i++) {
        sum_sq += (double)samples[i] * (double)samples[i];
    }
    double rms = sqrt(sum_sq / (double)num_samples);
    if (rms < 1e-5) return -100.0f;
    return (float)(20.0 * log10(rms));
}

ascolto_vad_config_t ascolto_vad_default_config(void) {
    ascolto_vad_config_t cfg;
    cfg.energy_threshold_db = -42.0f;
    cfg.min_speech_s = 0.25;
    cfg.min_silence_s = 0.35;
    cfg.max_segment_s = 25.0;
    cfg.pad_s = 0.12;
    return cfg;
}

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

bool ascolto_transcript_is_noise(
    const char* text, int word_count, float avg_prob,
    ascolto_channel_t channel,
    float rms_db,
    bool recent_sys_activity
) {
    if (!text || text[0] == '\0') return true;

    /* Reject decoder repetition loops ("Poco demo, poco demo, poco demo...") */
    if (has_repetition_loop(text)) {
        return true;
    }

    if (word_count > 0 && avg_prob < 0.50f) return true;

    bool is_classic_phantom =
        (strcmp(text, "Grazie.") == 0 ||
         strcmp(text, "Grazie mille.") == 0 ||
         strcmp(text, "Grazie a tutti.") == 0 ||
         strcmp(text, "Grazie, grazie.") == 0 ||
         strcmp(text, "Grazie!") == 0 ||
         strcmp(text, "Buongiorno.") == 0 ||
         strcmp(text, "Buongiorno a tutti.") == 0 ||
         strcmp(text, "Buonasera.") == 0 ||
         strcmp(text, "Ciao a tutti, ciao a tutti.") == 0 ||
         strstr(text, "Sottotitoli") != NULL ||
         strstr(text, "Amara.org") != NULL);

    bool is_short_filler =
        (strcmp(text, "Ok.") == 0 ||
         strcmp(text, "Eh...") == 0 ||
         strcmp(text, "Ehm...") == 0 ||
         strcmp(text, "Oh!") == 0 ||
         strcmp(text, "Allora.") == 0 ||
         strcmp(text, "Allora...") == 0 ||
         strcmp(text, "Io...") == 0 ||
         strcmp(text, "Non.") == 0);

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


bool ascolto_bleed_reference(const float *history, int64_t frames, int64_t mic_end,
                             int64_t sys_end, float *reference_db) {
    *reference_db = -100.0f;
    if (llabs(mic_end - sys_end) > ASCOLTO_SAMPLE_RATE / 4) return false;
    int64_t end = (mic_end + ASCOLTO_FRAME_SAMPLES - 1) / ASCOLTO_FRAME_SAMPLES;
    if (end > frames) end = frames;
    bool active = false;
    for (int k = 0; k < 12 && end - k > 0; k++) {
        int64_t frame = end - 1 - k;
        if (frames - frame > ASCOLTO_SYS_ENV_HISTORY) break;
        float db = history[frame % ASCOLTO_SYS_ENV_HISTORY];
        if (db >= -36.5f) {
            active = true;
            if (db > *reference_db) *reference_db = db;
        }
    }
    return active;
}

bool ascolto_echo_turn_matches(const char *mic_text, double start, double end, float rms_db,
                               float overlap_ratio, bool local_interruption, const char *sys_text,
                               double sys_start, double sys_end, float sys_db) {
    return !local_interruption && overlap_ratio >= 0.30f && start <= sys_end + 3.5 && end >= sys_start - 2.0 &&
           rms_db < sys_db + 3.5f && ascolto_is_lexical_echo(mic_text, sys_text);
}

static void mic_activity_reset(ascolto_mic_activity_t *activity) {
    *activity = (ascolto_mic_activity_t){.sys_peak_db = -100.0f};
}

void ascolto_echo_filter_reset(ascolto_echo_filter_t *filter) {
    filter->sys_frames = 0;
    filter->turn_count = 0;
    mic_activity_reset(&filter->mic);
    for (int i = 0; i < ASCOLTO_SYS_ENV_HISTORY; i++) filter->sys_db[i] = -100.0f;
}

void ascolto_mic_activity_track(ascolto_mic_activity_t *activity, bool in_speech,
                                bool sys_active, float sys_db) {
    if (!in_speech) { mic_activity_reset(activity); return; }
    activity->frames++;
    if (sys_active) {
        activity->sys_frames++;
        if (sys_db > activity->sys_peak_db) activity->sys_peak_db = sys_db;
    }
}

ascolto_mic_activity_t ascolto_mic_activity_take(ascolto_mic_activity_t *activity) {
    ascolto_mic_activity_t previous = *activity;
    mic_activity_reset(activity);
    return previous;
}

void ascolto_echo_filter_remember(ascolto_echo_filter_t *filter, const char *text,
                                  double start, double end, float rms_db) {
    ascolto_echo_turn_t *turn = &filter->turns[filter->turn_count++ % ASCOLTO_RECENT_SYS_TURNS];
    turn->start_s = start;
    turn->end_s = end;
    turn->rms_db = rms_db;
    snprintf(turn->text, sizeof(turn->text), "%s", text);
}

bool ascolto_echo_filter_matches(const ascolto_echo_filter_t *filter, const char *text,
    double start, double end, float rms_db, const ascolto_mic_activity_t *activity) {
    float overlap = activity->frames ? (float)activity->sys_frames / activity->frames : 0;
    bool interruption = activity->sys_frames >= 3 && rms_db >= activity->sys_peak_db + 1.0f;
    for (unsigned i = 0; i < filter->turn_count && i < ASCOLTO_RECENT_SYS_TURNS; i++) {
        const ascolto_echo_turn_t *turn = &filter->turns[i];
        if (ascolto_echo_turn_matches(text, start, end, rms_db, overlap, interruption,
                                     turn->text, turn->start_s, turn->end_s, turn->rms_db)) return true;
    }
    return false;
}
