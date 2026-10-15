#include "ascolto.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Estimates fundamental frequency (F0 in Hz) on a 30ms voiced frame using
   first-peak normalized autocorrelation after the initial zero crossing
   (avoids formant/harmonic locking and octave errors).
   Human vocal pitch range: [75 Hz .. 350 Hz]. Returns 0.0f if unvoiced. */
static float estimate_frame_f0(const float* frame, int len, int sample_rate) {
    double energy = 0.0;
    for (int i = 0; i < len; i++) {
        energy += (double)frame[i] * (double)frame[i];
    }
    if (energy / len < 2e-4) return 0.0f;

    int min_lag = sample_rate / 350; /* ~45 samples at 16kHz */
    int max_lag = sample_rate / 75;  /* ~213 samples at 16kHz */
    if (max_lag >= len - 2) max_lag = len - 3;

    double corr[256] = {0};

    /* Wait until autocorrelation drops below 0.30 from lag=0 before searching for pitch period */
    int start_lag = min_lag;
    for (int lag = 4; lag <= max_lag; lag++) {
        double num = 0.0, den1 = 0.0, den2 = 0.0;
        int n = len - lag;
        for (int i = 0; i < n; i++) {
            double a = frame[i];
            double b = frame[i + lag];
            num += a * b;
            den1 += a * a;
            den2 += b * b;
        }
        double denom = sqrt(den1 * den2);
        corr[lag] = (denom > 1e-9) ? (num / denom) : 0.0;
    }

    while (start_lag < max_lag && corr[start_lag] > 0.35) {
        start_lag++;
    }

    double best_corr = 0.0;
    int best_lag = 0;
    for (int lag = start_lag; lag <= max_lag; lag++) {
        if (corr[lag] > best_corr) {
            best_corr = corr[lag];
            best_lag = lag;
        }
    }

    /* Prefer the first strong peak (fundamental period) if within 90% of global max
       to avoid locking onto 2*T0 subharmonics, and check lag/2 if harmonic locked */
    for (int lag = start_lag + 1; lag < max_lag; lag++) {
        if (corr[lag] >= corr[lag - 1] && corr[lag] >= corr[lag + 1]) {
            if (corr[lag] >= 0.60 && corr[lag] >= 0.88 * best_corr) {
                best_lag = lag;
                best_corr = corr[lag];
                break;
            }
        }
    }

    if (best_corr < 0.58 || best_lag <= 0) {
        return 0.0f;
    }
    return (float)sample_rate / (float)best_lag;
}

/* Goertzel power at target frequency Hz on a Hamming-windowed frame */
static double goertzel_power(const float* frame, int len, double freq_hz, int sample_rate) {
    double omega = 2.0 * M_PI * freq_hz / (double)sample_rate;
    double coeff = 2.0 * cos(omega);
    double s_prev = 0.0;
    double s_prev2 = 0.0;
    for (int i = 0; i < len; i++) {
        double w = 0.54 - 0.46 * cos(2.0 * M_PI * (double)i / (double)(len - 1));
        double s = (double)frame[i] * w + coeff * s_prev - s_prev2;
        s_prev2 = s_prev;
        s_prev = s;
    }
    return s_prev2 * s_prev2 + s_prev * s_prev - coeff * s_prev * s_prev2;
}

void ascolto_extract_voiceprint(
    const float* pcm,
    int num_samples,
    const float* whistle_frames,
    int num_whistle_frames,
    ascolto_voiceprint_t* out_vp
) {
    if (!out_vp) return;
    memset(out_vp, 0, sizeof(*out_vp));
    if (!pcm || num_samples <= 0) return;

    const int frame_len = 480; /* 30 ms at 16 kHz */
    const int hop_len = 240;   /* 15 ms hop */

    /* Collect voiced F0 values and compute median + IQR-filtered mean for robustness */
    float f0_vals[2048];
    int f0_count = 0;

    double zcr_sum = 0.0;
    int frame_count = 0;

    static const double probe_freqs[18] = {
        110.0, 150.0, 200.0,
        260.0, 340.0, 440.0,
        580.0, 750.0, 950.0,
        1200.0, 1550.0, 1900.0,
        2300.0, 2800.0, 3400.0,
        4000.0, 5000.0, 6200.0
    };
    double band_power[6] = {0, 0, 0, 0, 0, 0};
    double freq_power[18] = {0};

    for (int pos = 0; pos + frame_len <= num_samples; pos += hop_len) {
        const float* fr = pcm + pos;
        float rms_db = ascolto_audio_rms_db(fr, frame_len);
        if (rms_db < -38.0f) continue;

        frame_count++;
        float f0 = estimate_frame_f0(fr, frame_len, ASCOLTO_SAMPLE_RATE);
        if (f0 > 0.0f && f0_count < 2048) {
            f0_vals[f0_count++] = f0;
        }

        int zc = 0;
        for (int i = 1; i < frame_len; i++) {
            if ((fr[i - 1] >= 0.0f && fr[i] < 0.0f) || (fr[i - 1] < 0.0f && fr[i] >= 0.0f)) {
                zc++;
            }
        }
        zcr_sum += (double)zc / (double)frame_len;

        for (int k = 0; k < 18; k++) {
            double p = goertzel_power(fr, frame_len, probe_freqs[k], ASCOLTO_SAMPLE_RATE);
            freq_power[k] += p;
            band_power[k / 3] += p;
        }
    }

    if (f0_count > 0) {
        /* Sort f0_vals to take robust central 60% (trimmed mean) */
        for (int i = 0; i < f0_count - 1; i++) {
            for (int j = i + 1; j < f0_count; j++) {
                if (f0_vals[j] < f0_vals[i]) {
                    float tmp = f0_vals[i];
                    f0_vals[i] = f0_vals[j];
                    f0_vals[j] = tmp;
                }
            }
        }
        int i0 = f0_count / 5;
        int i1 = f0_count - (f0_count / 5);
        if (i1 <= i0) { i0 = 0; i1 = f0_count; }
        double sum = 0.0, sq = 0.0;
        int cnt = i1 - i0;
        for (int i = i0; i < i1; i++) {
            sum += f0_vals[i];
            sq += (double)f0_vals[i] * (double)f0_vals[i];
        }
        double mean = sum / cnt;
        double var = (sq / cnt) - (mean * mean);
        out_vp->f0_mean_hz = (float)mean;
        out_vp->f0_std_hz = (var > 0.0) ? (float)sqrt(var) : 0.0f;
    } else {
        out_vp->f0_mean_hz = 160.0f;
        out_vp->f0_std_hz = 15.0f;
    }

    if (frame_count > 0) {
        out_vp->zcr = (float)(zcr_sum / frame_count);
    }
    out_vp->voiced_frames = frame_count;

    double total_power = 1e-9;
    double weighted_freq = 0.0;
    for (int k = 0; k < 18; k++) {
        total_power += freq_power[k];
        weighted_freq += probe_freqs[k] * freq_power[k];
    }
    out_vp->spectral_centroid_hz = (float)(weighted_freq / total_power);

    double cum_power = 0.0;
    out_vp->spectral_rolloff_hz = (float)probe_freqs[17];
    for (int k = 0; k < 18; k++) {
        cum_power += freq_power[k];
        if (cum_power >= 0.85 * total_power) {
            out_vp->spectral_rolloff_hz = (float)probe_freqs[k];
            break;
        }
    }

    /* Compute sub-band log-energies and subtract the average human vocal tilt
       so the residual represents speaker-specific timbre rather than common tilt */
    static const float baseline_tilt[6] = { -0.45f, -0.40f, -0.75f, -1.35f, -1.85f, -2.50f };
    for (int b = 0; b < 6; b++) {
        double ratio = (band_power[b] + 1e-9) / total_power;
        out_vp->band_energy[b] = (float)log10(ratio) - baseline_tilt[b];
    }

    if (whistle_frames && num_whistle_frames > 0) {
        for (int p = 0; p < 5; p++) {
            double acc = 0.0;
            int c0 = p * 100;
            int c1 = c0 + 100;
            for (int t = 0; t < num_whistle_frames; t++) {
                const float* row = whistle_frames + t * ASCOLTO_WHISTLE_EMBED_DIM;
                for (int c = c0; c < c1; c++) {
                    acc += (c & 1) ? -row[c] : row[c];
                }
            }
            out_vp->whistle_proj[p] = (float)(acc / (double)(num_whistle_frames * 100));
        }
    }

    /* Assemble 16-D speaker embedding centered around typical midpoint (F0 ~165 Hz, Centroid ~420 Hz) */
    float* fv = out_vp->feature_vec;
    fv[0] = (out_vp->f0_mean_hz - 165.0f) / 30.0f * 3.0f;
    fv[1] = (out_vp->f0_std_hz - 20.0f) / 15.0f * 0.6f;
    fv[2] = (out_vp->spectral_centroid_hz - 420.0f) / 150.0f * 2.2f;
    fv[3] = (out_vp->spectral_rolloff_hz - 650.0f) / 300.0f * 1.5f;
    fv[4] = (out_vp->zcr - 0.085f) / 0.03f * 1.5f;
    for (int b = 0; b < 6; b++) {
        fv[5 + b] = out_vp->band_energy[b] * 1.2f;
    }
    for (int p = 0; p < 5; p++) {
        fv[11 + p] = out_vp->whistle_proj[p] * 0.4f;
    }

    double norm = 0.0;
    for (int i = 0; i < ASCOLTO_VOICEPRINT_DIM; i++) {
        norm += (double)fv[i] * (double)fv[i];
    }
    norm = sqrt(norm);
    if (norm > 1e-7) {
        for (int i = 0; i < ASCOLTO_VOICEPRINT_DIM; i++) {
            fv[i] = (float)(fv[i] / norm);
        }
    }
}

float ascolto_voiceprint_similarity(
    const ascolto_voiceprint_t* a,
    const float* cluster_centroid
) {
    if (!a || !cluster_centroid) return 0.0f;
    double dot = 0.0;
    double na = 0.0;
    double nb = 0.0;
    for (int i = 0; i < ASCOLTO_VOICEPRINT_DIM; i++) {
        dot += (double)a->feature_vec[i] * (double)cluster_centroid[i];
        na += (double)a->feature_vec[i] * (double)a->feature_vec[i];
        nb += (double)cluster_centroid[i] * (double)cluster_centroid[i];
    }
    double denom = sqrt(na * nb);
    if (denom < 1e-9) return 0.0f;
    return (float)(dot / denom);
}

void ascolto_diarizer_init(ascolto_diarizer_t* d, float similarity_threshold) {
    if (!d) return;
    memset(d, 0, sizeof(*d));
    d->similarity_threshold = (similarity_threshold > 0.0f) ? similarity_threshold : 0.60f;
}

const char* ascolto_diarizer_identify(
    ascolto_diarizer_t* d,
    ascolto_channel_t channel,
    const ascolto_voiceprint_t* vp,
    float* out_best_similarity,
    bool* out_is_new_speaker
) {
    if (out_best_similarity) *out_best_similarity = 1.0f;
    if (out_is_new_speaker) *out_is_new_speaker = false;

    if (channel == ASCOLTO_CHANNEL_MIC) {
        return "ME";
    }
    if (!d || !vp) return "SPEAKER_1";

    int best_idx = -1;
    float best_sim = -2.0f;
    for (int i = 0; i < d->num_speakers; i++) {
        float sim = ascolto_voiceprint_similarity(vp, d->speakers[i].centroid);
        /* Cosine normalization loses spectral scale: voices with similar pitch can point
           in the same direction while their spectral centroids differ substantially.
           Retain that timbre distance for sustained turns; short backchannels are noisy. */
        if (vp->voiced_frames >= 90 && vp->spectral_centroid_hz > 0.0f &&
            d->speakers[i].mean_centroid_hz > 0.0f) {
            float timbre_distance = fabsf(logf(vp->spectral_centroid_hz / d->speakers[i].mean_centroid_hz));
            sim -= 1.2f * fminf(timbre_distance, 0.5f);
        }
        if (sim > best_sim) {
            best_sim = sim;
            best_idx = i;
        }
    }

    float eff_thresh = d->similarity_threshold;
    /* Short backchannels (< 45 voiced frames ~ 0.7s, e.g. "Ok.", "Sì.") have higher acoustic variance;
       attach them to the closest existing speaker unless clearly distinct, and avoid drifting the centroid. */
    if (d->num_speakers > 0 && vp->voiced_frames > 0 && vp->voiced_frames < 45) {
        eff_thresh -= 0.16f;
    }

    if (best_idx >= 0 && best_sim >= eff_thresh) {
        ascolto_speaker_cluster_t* cl = &d->speakers[best_idx];
        if (vp->voiced_frames >= 35) {
            float w_old = (float)cl->segment_count;
            float w_new = 1.0f;
            double norm = 0.0;
            for (int i = 0; i < ASCOLTO_VOICEPRINT_DIM; i++) {
                cl->centroid[i] = (cl->centroid[i] * w_old + vp->feature_vec[i] * w_new) / (w_old + w_new);
                norm += (double)cl->centroid[i] * (double)cl->centroid[i];
            }
            norm = sqrt(norm);
            if (norm > 1e-7) {
                for (int i = 0; i < ASCOLTO_VOICEPRINT_DIM; i++) {
                    cl->centroid[i] = (float)(cl->centroid[i] / norm);
                }
            }
            cl->mean_f0_hz = (cl->mean_f0_hz * w_old + vp->f0_mean_hz) / (w_old + w_new);
            cl->mean_centroid_hz = (cl->mean_centroid_hz * w_old + vp->spectral_centroid_hz) / (w_old + w_new);
        }
        cl->segment_count++;
        if (out_best_similarity) *out_best_similarity = best_sim;
        if (out_is_new_speaker) *out_is_new_speaker = false;
        return cl->label;
    }

    int idx = d->num_speakers;
    if (idx >= ASCOLTO_MAX_SPEAKERS) {
        idx = (best_idx >= 0) ? best_idx : (ASCOLTO_MAX_SPEAKERS - 1);
    } else {
        d->num_speakers++;
    }

    ascolto_speaker_cluster_t* cl = &d->speakers[idx];
    cl->speaker_id = idx + 1;
    snprintf(cl->label, sizeof(cl->label), "SPEAKER_%d", cl->speaker_id);
    cl->segment_count = 1;
    memcpy(cl->centroid, vp->feature_vec, sizeof(cl->centroid));
    cl->mean_f0_hz = vp->f0_mean_hz;
    cl->mean_centroid_hz = vp->spectral_centroid_hz;

    if (out_best_similarity) *out_best_similarity = (best_idx >= 0) ? best_sim : 0.0f;
    if (out_is_new_speaker) *out_is_new_speaker = true;
    return cl->label;
}
