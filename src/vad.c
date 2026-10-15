#include "ascolto.h"

#include <math.h>
#include <stddef.h>

ascolto_vad_config_t ascolto_vad_default_config(void) {
    ascolto_vad_config_t cfg;
    cfg.energy_threshold_db = -42.0f;
    cfg.min_speech_s = 0.25;
    cfg.min_silence_s = 0.35;
    cfg.max_segment_s = 25.0;
    cfg.pad_s = 0.12;
    return cfg;
}

int ascolto_vad_segment_buffer(
    const float* samples,
    int num_samples,
    ascolto_channel_t channel,
    const ascolto_vad_config_t* cfg_in,
    ascolto_vad_segment_t* out_segments,
    int max_segments
) {
    if (!samples || num_samples <= 0 || !out_segments || max_segments <= 0) return 0;

    ascolto_vad_config_t cfg = cfg_in ? *cfg_in : ascolto_vad_default_config();
    const int frame_len = ASCOLTO_SAMPLE_RATE / 50; /* 20 ms = 320 samples */
    const int min_speech_samples = (int)(cfg.min_speech_s * ASCOLTO_SAMPLE_RATE);
    const int min_silence_samples = (int)(cfg.min_silence_s * ASCOLTO_SAMPLE_RATE);
    const int max_segment_samples = (int)(cfg.max_segment_s * ASCOLTO_SAMPLE_RATE);
    const int pad_samples = (int)(cfg.pad_s * ASCOLTO_SAMPLE_RATE);

    int seg_count = 0;
    bool in_speech = false;
    int speech_start = 0;
    int last_voiced = 0;

    for (int pos = 0; pos < num_samples; pos += frame_len) {
        int n = (pos + frame_len <= num_samples) ? frame_len : (num_samples - pos);
        float db = ascolto_audio_rms_db(samples + pos, n);
        bool voiced = (db >= cfg.energy_threshold_db);

        if (voiced) {
            if (!in_speech) {
                in_speech = true;
                speech_start = pos;
            }
            last_voiced = pos + n;

            if ((last_voiced - speech_start) >= max_segment_samples && seg_count < max_segments) {
                int s0 = speech_start - pad_samples;
                if (s0 < 0) s0 = 0;
                int s1 = last_voiced;
                out_segments[seg_count].channel = channel;
                out_segments[seg_count].start_sample = s0;
                out_segments[seg_count].end_sample = s1;
                out_segments[seg_count].start_s = (double)s0 / ASCOLTO_SAMPLE_RATE;
                out_segments[seg_count].end_s = (double)s1 / ASCOLTO_SAMPLE_RATE;
                out_segments[seg_count].rms_db = ascolto_audio_rms_db(samples + s0, s1 - s0);
                seg_count++;
                in_speech = false;
            }
        } else if (in_speech) {
            int silence_len = (pos + n) - last_voiced;
            if (silence_len >= min_silence_samples) {
                int speech_len = last_voiced - speech_start;
                if (speech_len >= min_speech_samples && seg_count < max_segments) {
                    int s0 = speech_start - pad_samples;
                    if (s0 < 0) s0 = 0;
                    int s1 = last_voiced + pad_samples;
                    if (s1 > num_samples) s1 = num_samples;
                    /* Merge if overlapping with previous segment */
                    if (seg_count > 0 && s0 <= out_segments[seg_count - 1].end_sample) {
                        out_segments[seg_count - 1].end_sample = s1;
                        out_segments[seg_count - 1].end_s = (double)s1 / ASCOLTO_SAMPLE_RATE;
                        int m0 = out_segments[seg_count - 1].start_sample;
                        out_segments[seg_count - 1].rms_db = ascolto_audio_rms_db(samples + m0, s1 - m0);
                    } else {
                        out_segments[seg_count].channel = channel;
                        out_segments[seg_count].start_sample = s0;
                        out_segments[seg_count].end_sample = s1;
                        out_segments[seg_count].start_s = (double)s0 / ASCOLTO_SAMPLE_RATE;
                        out_segments[seg_count].end_s = (double)s1 / ASCOLTO_SAMPLE_RATE;
                        out_segments[seg_count].rms_db = ascolto_audio_rms_db(samples + s0, s1 - s0);
                        seg_count++;
                    }
                }
                in_speech = false;
            }
        }
    }

    if (in_speech) {
        int speech_len = last_voiced - speech_start;
        if (speech_len >= min_speech_samples && seg_count < max_segments) {
            int s0 = speech_start - pad_samples;
            if (s0 < 0) s0 = 0;
            int s1 = last_voiced + pad_samples;
            if (s1 > num_samples) s1 = num_samples;
            if (seg_count > 0 && s0 <= out_segments[seg_count - 1].end_sample) {
                out_segments[seg_count - 1].end_sample = s1;
                out_segments[seg_count - 1].end_s = (double)s1 / ASCOLTO_SAMPLE_RATE;
                int m0 = out_segments[seg_count - 1].start_sample;
                out_segments[seg_count - 1].rms_db = ascolto_audio_rms_db(samples + m0, s1 - m0);
            } else {
                out_segments[seg_count].channel = channel;
                out_segments[seg_count].start_sample = s0;
                out_segments[seg_count].end_sample = s1;
                out_segments[seg_count].start_s = (double)s0 / ASCOLTO_SAMPLE_RATE;
                out_segments[seg_count].end_s = (double)s1 / ASCOLTO_SAMPLE_RATE;
                out_segments[seg_count].rms_db = ascolto_audio_rms_db(samples + s0, s1 - s0);
                seg_count++;
            }
        }
    }

    return seg_count;
}
