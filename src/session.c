#include "ascolto/offline.h"

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

    /* Bound transport requests without selecting, masking or re-aligning speech. */
    ascolto_vad_segment_t raw_segs[ASCOLTO_MAX_SEGMENTS];
    int n_raw = 0;
    const int chunk_samples = ASCOLTO_TRANSCRIPTION_CHUNK_SECONDS * ASCOLTO_SAMPLE_RATE;
    for (int offset = 0; offset < max_samples; offset += chunk_samples) {
        for (int ch = ASCOLTO_CHANNEL_SYS; ch >= ASCOLTO_CHANNEL_MIC; ch--) {
            const float *pcm = ch == ASCOLTO_CHANNEL_MIC ? mic_pcm : sys_pcm;
            int count = ch == ASCOLTO_CHANNEL_MIC ? mic_samples : sys_samples;
            if (!pcm || offset >= count) continue;
            if (n_raw == ASCOLTO_MAX_SEGMENTS) { mon->destroy(mon); return -2; }
            int end = count - offset > chunk_samples ? offset + chunk_samples : count;
            raw_segs[n_raw++] = (ascolto_vad_segment_t){
                .channel = (ascolto_channel_t)ch, .start_sample = offset, .end_sample = end,
                .start_s = (double)offset / ASCOLTO_SAMPLE_RATE,
                .end_s = (double)end / ASCOLTO_SAMPLE_RATE,
                .rms_db = ascolto_audio_rms_db(pcm + offset, end - offset)};
        }
    }

    ascolto_diarizer_t diarizer;
    ascolto_diarizer_init(&diarizer, diarizer_threshold);

    /* Recognize original PCM and retain every nonempty model response. */
    for (int i = 0; i < n_raw && out_report->num_utterances < ASCOLTO_MAX_SEGMENTS; i++) {
        const ascolto_vad_segment_t* seg = &raw_segs[i];
        const float* src = (seg->channel == ASCOLTO_CHANNEL_MIC) ? mic_pcm : sys_pcm;
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

        if (tok < 0) { mon->destroy(mon); return -3; }
        if (!tr.text[0]) continue;

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


    /* 4. Simulate Mic deactivation at end of call */
    mon->step(mon, duration_s + 0.1);
    mon->destroy(mon);

    return 0;
}
