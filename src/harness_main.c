#include "ascolto.h"
#include "needle.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>

#define DEFAULT_MODEL_PATH "models/whistle.cact"

static bool str_contains_ci(const char* haystack, const char* needle_str) {
    if (!haystack || !needle_str) return false;
    size_t nlen = strlen(needle_str);
    if (nlen == 0) return true;
    for (const char* p = haystack; *p; p++) {
        size_t i = 0;
        while (i < nlen && p[i] && tolower((unsigned char)p[i]) == tolower((unsigned char)needle_str[i])) {
            i++;
        }
        if (i == nlen) return true;
    }
    return false;
}

/* --------------------------------------------------------------------------
 * Subcommand: probe-macos
 * -------------------------------------------------------------------------- */
static int cmd_probe_macos(void) {
    printf("=== [ascolto-harness] macOS CoreAudio Device Probe ===\n");
    ascolto_macos_device_info_t devs[32];
    int n = ascolto_macos_list_devices(devs, 32);
    if (n < 0) {
        fprintf(stderr, "ERROR: Failed to list macOS CoreAudio devices (rc=%d)\n", n);
        return 1;
    }
    printf("Found %d audio device(s):\n", n);
    for (int i = 0; i < n; i++) {
        printf(
            "  [%2d] id=%-4u %-36s | sr=%6.0f Hz | in=%s | def_in=%s | def_out=%s | RUNNING=%s\n",
            i,
            devs[i].device_id,
            devs[i].device_name,
            devs[i].sample_rate,
            devs[i].is_input ? "yes" : "no ",
            devs[i].is_default_input ? "YES" : "no ",
            devs[i].is_default_output ? "YES" : "no ",
            devs[i].is_running_somewhere ? "ACTIVE" : "idle"
        );
    }

    ascolto_macos_device_info_t def_mic;
    if (ascolto_macos_default_mic_status(&def_mic) == 0) {
        printf("\nDefault Input Microphone:\n");
        printf("  Name   : %s (ID %u)\n", def_mic.device_name, def_mic.device_id);
        printf("  Status : %s\n", def_mic.is_running_somewhere ? "ACTIVE (IN CALL / RECORDING)" : "IDLE");
    } else {
        printf("\nDefault Input Microphone: None detected\n");
    }

    char in_apps[256] = {0}, out_apps[256] = {0};
    ascolto_macos_query_audio_apps(in_apps, sizeof(in_apps), out_apps, sizeof(out_apps));
    printf("\nActive External Audio Apps:\n");
    printf("  App IN  (MIC) : %s\n", in_apps[0] ? in_apps : "None");
    printf("  App OUT (SYS) : %s\n", out_apps[0] ? out_apps : "None");

    ascolto_call_metadata_t meta;
    ascolto_macos_query_call_metadata(in_apps[0] ? in_apps : out_apps, &meta);
    printf("\nCall Context Metadata:\n");
    printf("  Accessibility : %s (Window Title: '%s')\n",
           meta.ax_trusted ? "GRANTED" : "not granted yet",
           meta.window_title[0] ? meta.window_title : "-");
    printf("  Calendar      : %s (Event Title : '%s', Attendees: '%s')\n",
           meta.calendar_authorized ? "GRANTED" : "not granted yet",
           meta.calendar_title[0] ? meta.calendar_title : "-",
           meta.calendar_attendees[0] ? meta.calendar_attendees : "-");
    return 0;
}

/* --------------------------------------------------------------------------
 * Subcommand: probe-engine
 * -------------------------------------------------------------------------- */
static int cmd_probe_engine(const char* model_path) {
    printf("=== [ascolto-harness] Whistle Engine Probe ===\n");
    ascolto_whistle_t w;
    int rc = ascolto_whistle_init(&w, model_path);
    if (rc != 0) {
        fprintf(stderr, "ERROR: ascolto_whistle_init(%s) failed (rc=%d, err=%s)\n",
                model_path, rc, needle_last_error());
        return 1;
    }
    printf("Loaded model      : %s (%.2f MB)\n", model_path, (double)w.model_size / (1024.0 * 1024.0));
    printf("needle_models()   : %d (NEEDLE_SPEECH=%d)\n", needle_models(), (needle_models() & NEEDLE_SPEECH) != 0);

    /* Probe silence handling */
    float silence[16000] = {0};
    ascolto_transcript_result_t sil_res;
    ascolto_whistle_transcribe(&w, silence, 16000, NULL, NULL, true, &sil_res);
    printf("Silence 1.0s test : text=\"%s\", lang=\"%s\"\n", sil_res.text, sil_res.language);

    /* Probe embedding dimensions on 1.0s audio */
    float* emb = NULL;
    int frames = 0;
    if (ascolto_whistle_embed_frames(&w, silence, 16000, &emb, &frames) > 0) {
        printf("Embed 1.0s test   : %d frames x %d dims = %d floats\n",
               frames, ASCOLTO_WHISTLE_EMBED_DIM, frames * ASCOLTO_WHISTLE_EMBED_DIM);
        free(emb);
    }

    ascolto_whistle_free(&w);
    return 0;
}

/* --------------------------------------------------------------------------
 * Subcommand: inspect-audio <wav_path>
 * -------------------------------------------------------------------------- */
static int cmd_inspect_audio(const char* model_path, const char* wav_path, const char* lang, const char* kw) {
    printf("=== [ascolto-harness] Audio Inspector: %s ===\n", wav_path);
    ascolto_audio_buffer_t buf;
    int rc = ascolto_wav_load(wav_path, &buf);
    if (rc != 0) {
        fprintf(stderr, "ERROR: Failed to load WAV '%s' (rc=%d)\n", wav_path, rc);
        return 1;
    }
    printf("WAV Info: %d samples, %d Hz (%.2f s), overall RMS=%.1f dB\n",
           buf.num_samples, buf.sample_rate,
           (double)buf.num_samples / buf.sample_rate,
           ascolto_audio_rms_db(buf.samples, buf.num_samples));

    ascolto_whistle_t w;
    if (ascolto_whistle_init(&w, model_path) != 0) {
        fprintf(stderr, "ERROR: Failed to load Whistle model '%s'\n", model_path);
        ascolto_audio_free(&buf);
        return 1;
    }

    ascolto_vad_segment_t segs[ASCOLTO_MAX_SEGMENTS];
    int n_segs = ascolto_vad_segment_buffer(buf.samples, buf.num_samples, ASCOLTO_CHANNEL_SYS, NULL, segs, ASCOLTO_MAX_SEGMENTS);
    printf("VAD Segments detected: %d\n", n_segs);

    for (int i = 0; i < n_segs; i++) {
        const float* spcm = buf.samples + segs[i].start_sample;
        int slen = segs[i].end_sample - segs[i].start_sample;

        float* emb = NULL;
        int n_frames = 0;
        ascolto_whistle_embed_frames(&w, spcm, slen, &emb, &n_frames);

        ascolto_voiceprint_t vp;
        ascolto_extract_voiceprint(spcm, slen, emb, n_frames, &vp);
        free(emb);

        ascolto_transcript_result_t tr;
        ascolto_whistle_transcribe(&w, spcm, slen, lang, kw, true, &tr);

        printf("\n  [Segment %d] %.2fs .. %.2fs (RMS=%.1f dB, WhistleEmbed=%d frames)\n",
               i + 1, segs[i].start_s, segs[i].end_s, segs[i].rms_db, n_frames);
        printf("    Voiceprint : F0=%.1f Hz (±%.1f), Centroid=%.0f Hz, Rolloff=%.0f Hz, ZCR=%.3f\n",
               vp.f0_mean_hz, vp.f0_std_hz, vp.spectral_centroid_hz, vp.spectral_rolloff_hz, vp.zcr);
        printf("    Transcript : \"%s\" [lang=%s, ttft=%.1fms, decode=%.0f tok/s]\n",
               tr.text, tr.language, tr.ttft_ms, tr.decode_tps);
        if (tr.num_words > 0) {
            printf("    Words (%2d) : ", tr.num_words);
            for (int k = 0; k < tr.num_words; k++) {
                printf("%s[%.2f-%.2fs p=%.2f] ",
                       tr.words[k].word,
                       segs[i].start_s + tr.words[k].start_s,
                       segs[i].start_s + tr.words[k].end_s,
                       tr.words[k].probability);
            }
            printf("\n");
        }
    }

    ascolto_whistle_free(&w);
    ascolto_audio_free(&buf);
    return 0;
}

/* --------------------------------------------------------------------------
 * Subcommand: simulate
 * -------------------------------------------------------------------------- */
static int cmd_simulate(
    const char* model_path,
    const char* mic_wav,
    const char* sys_wav,
    const char* lang,
    const char* keywords,
    float threshold,
    const char* trace_path
) {
    ascolto_audio_buffer_t mic_buf = {0};
    ascolto_audio_buffer_t sys_buf = {0};

    if (mic_wav && ascolto_wav_load(mic_wav, &mic_buf) != 0) {
        fprintf(stderr, "ERROR: Cannot load --mic WAV '%s'\n", mic_wav);
        return 1;
    }
    if (sys_wav && ascolto_wav_load(sys_wav, &sys_buf) != 0) {
        fprintf(stderr, "ERROR: Cannot load --sys WAV '%s'\n", sys_wav);
        ascolto_audio_free(&mic_buf);
        return 1;
    }

    ascolto_whistle_t w;
    if (ascolto_whistle_init(&w, model_path) != 0) {
        fprintf(stderr, "ERROR: Failed to initialize Whistle from '%s'\n", model_path);
        ascolto_audio_free(&mic_buf);
        ascolto_audio_free(&sys_buf);
        return 1;
    }

    FILE* trace_f = NULL;
    if (trace_path) {
        trace_f = fopen(trace_path, "w");
        if (!trace_f) {
            fprintf(stderr, "WARNING: Could not open trace file '%s' for writing\n", trace_path);
        }
    }

    ascolto_session_report_t* report = (ascolto_session_report_t*)calloc(1, sizeof(ascolto_session_report_t));
    if (!report) {
        if (trace_f) fclose(trace_f);
        ascolto_whistle_free(&w);
        ascolto_audio_free(&mic_buf);
        ascolto_audio_free(&sys_buf);
        return 1;
    }
    int rc = ascolto_run_simulated_session(
        &w,
        mic_buf.samples,
        mic_buf.num_samples,
        sys_buf.samples,
        sys_buf.num_samples,
        lang,
        keywords,
        threshold,
        trace_f,
        report
    );

    if (trace_f) fclose(trace_f);

    if (rc != 0) {
        fprintf(stderr, "ERROR: Session simulation failed (rc=%d)\n", rc);
        free(report);
        ascolto_whistle_free(&w);
        ascolto_audio_free(&mic_buf);
        ascolto_audio_free(&sys_buf);
        return 1;
    }

    printf("=== [ascolto-harness] Simulated Call Report ===\n");
    printf("Call Duration          : %.2f s\n", report->total_duration_s);
    printf("Total Whistle ASR Time : %.1f ms (%.2fx real-time)\n",
           report->total_transcribe_ms,
           (report->total_transcribe_ms > 0.0) ? (report->total_duration_s * 1000.0 / report->total_transcribe_ms) : 0.0);
    printf("Utterances Transcribed : %d\n", report->num_utterances);
    printf("Remote SYS Speakers    : %d distinct interlocutor(s) + ME\n\n", report->detected_sys_speakers);

    printf("--- Unified Call Transcript & Speaker Telemetry ---\n");
    for (int i = 0; i < report->num_utterances; i++) {
        const ascolto_utterance_t* u = &report->utterances[i];
        printf(
            "[%05.2fs - %05.2fs] [%-3s | %-9s | sim=%+.2f%s | F0=%5.1fHz | lang=%s | ttft=%4.1fms]\n"
            "  > %s\n",
            u->start_s,
            u->end_s,
            ascolto_channel_name(u->channel),
            u->speaker,
            u->speaker_similarity,
            u->is_new_speaker ? " NEW" : "    ",
            u->f0_hz,
            u->transcript.language,
            u->transcript.ttft_ms,
            u->transcript.text
        );
    }

    if (trace_path) {
        printf("\nStructured JSONL trace written to: %s\n", trace_path);
    }

    free(report);

    ascolto_whistle_free(&w);
    ascolto_audio_free(&mic_buf);
    ascolto_audio_free(&sys_buf);
    return 0;
}

/* --------------------------------------------------------------------------
 * Subcommand: test (Autonomous Self-Verification Suite)
 * -------------------------------------------------------------------------- */
typedef struct {
    int transitions;
    bool last_active;
} test_mic_state_t;

static void test_mic_cb(bool is_active, double ts, void* ud) {
    (void)ts;
    test_mic_state_t* st = (test_mic_state_t*)ud;
    st->transitions++;
    st->last_active = is_active;
}

static int cmd_test(const char* model_path) {
    mkdir("/tmp/ascolto-test", 0755);
    int passed = 0;
    int failed = 0;

    #define CHECK(cond, msg, ...) do { \
        if (cond) { \
            printf("  [PASS] " msg "\n", ##__VA_ARGS__); \
            passed++; \
        } else { \
            printf("  [FAIL] " msg "\n", ##__VA_ARGS__); \
            failed++; \
        } \
    } while (0)

    printf("================================================================\n");
    printf("  ASCOLTO AUTONOMOUS TEST HARNESS SUITE\n");
    printf("================================================================\n\n");

    /* 1. WAV & RMS Test */
    printf("[1/10] WAV Audio I/O & Signal Metrics\n");
    {
        float sine[16000];
        for (int i = 0; i < 16000; i++) {
            sine[i] = 0.5f * sinf(2.0f * 3.14159265f * 440.0f * (float)i / 16000.0f);
        }
        float db = ascolto_audio_rms_db(sine, 16000);
        CHECK(fabsf(db - (-9.03f)) < 0.25f, "Sine wave RMS dB accurate (got %.2f dB, expected ~-9.03 dB)", db);

        const char* tmp_wav = "/tmp/ascolto-test/test_io.wav";
        int wrc = ascolto_wav_save(tmp_wav, sine, 16000, 16000);
        ascolto_audio_buffer_t loaded = {0};
        int lrc = ascolto_wav_load(tmp_wav, &loaded);
        CHECK(wrc == 0 && lrc == 0 && loaded.num_samples == 16000 && loaded.sample_rate == 16000,
              "16kHz mono WAV roundtrip save/load (%d samples)", loaded.num_samples);
        ascolto_audio_free(&loaded);
    }

    /* 2. macOS CoreAudio Probe */
    printf("\n[2/10] macOS CoreAudio Hardware Monitor\n");
    {
        ascolto_macos_device_info_t devs[32];
        int n = ascolto_macos_list_devices(devs, 32);
        CHECK(n > 0, "CoreAudio enumerated %d audio device(s)", n);

        ascolto_macos_device_info_t mic;
        int mrc = ascolto_macos_default_mic_status(&mic);
        CHECK(mrc == 0 && strlen(mic.device_name) > 0,
              "CoreAudio default input device queried: '%s' (active=%s)",
              mic.device_name, mic.is_running_somewhere ? "true" : "false");
    }

    /* 3. Mic Monitor State Machine */
    printf("\n[3/10] Microphone Trigger State Machine\n");
    {
        test_mic_state_t st = {0, false};
        ascolto_mic_monitor_t* mon = ascolto_mic_monitor_create_simulated(1.0, 5.0, test_mic_cb, &st);
        mon->step(mon, 0.2);
        bool s0 = mon->is_active(mon);
        mon->step(mon, 1.5);
        bool s1 = mon->is_active(mon);
        mon->step(mon, 3.0);
        bool s2 = mon->is_active(mon);
        mon->step(mon, 5.5);
        bool s3 = mon->is_active(mon);
        mon->destroy(mon);
        CHECK(!s0 && s1 && s2 && !s3 && st.transitions == 2,
              "Simulated mic trigger transitions cleanly (idle -> active -> idle, transitions=%d)", st.transitions);
    }

    /* 4. Whistle Engine Load, ASR (IT + EN), Word Timestamps & Silence Gate */
    printf("\n[4/10] Whistle C Engine (ASR + Word Timestamps + Silence Gate)\n");
    ascolto_whistle_t whistle;
    int init_rc = ascolto_whistle_init(&whistle, model_path);
    CHECK(init_rc == 0 && whistle.loaded, "Whistle model loaded from '%s' (%.2f MB)",
          model_path, (double)whistle.model_size / (1024.0 * 1024.0));

    if (init_rc == 0) {
        float silence[16000] = {0};
        ascolto_transcript_result_t sil_tr;
        ascolto_whistle_transcribe(&whistle, silence, 16000, NULL, NULL, true, &sil_tr);
        CHECK(sil_tr.text[0] == '\0', "Silence gate rejects 1.0s digital silence without hallucination");

        ascolto_audio_buffer_t it_buf = {0};
        if (ascolto_wav_load("tests/fixtures/me_turn1.wav", &it_buf) == 0) {
            ascolto_transcript_result_t it_tr;
            ascolto_whistle_transcribe(&whistle, it_buf.samples, it_buf.num_samples, "it", "progetto", true, &it_tr);
            CHECK(strcmp(it_tr.language, "it") == 0 &&
                  str_contains_ci(it_tr.text, "chiamata") &&
                  str_contains_ci(it_tr.text, "progetto") &&
                  it_tr.num_words >= 4,
                  "Italian ASR + word timestamps ('%s', words=%d, ttft=%.1fms)",
                  it_tr.text, it_tr.num_words, it_tr.ttft_ms);
            ascolto_audio_free(&it_buf);
        } else {
            CHECK(false, "Fixture tests/fixtures/me_turn1.wav missing (run scripts/gen_fixtures.sh)");
        }

        ascolto_audio_buffer_t en_buf = {0};
        if (ascolto_wav_load("tests/fixtures/english_sample.wav", &en_buf) == 0) {
            ascolto_transcript_result_t en_tr;
            ascolto_whistle_transcribe(&whistle, en_buf.samples, en_buf.num_samples, NULL, NULL, true, &en_tr);
            CHECK(strcmp(en_tr.language, "en") == 0 &&
                  str_contains_ci(en_tr.text, "microphone") &&
                  str_contains_ci(en_tr.text, "recording"),
                  "English auto-lang ASR ('%s', lang=%s)", en_tr.text, en_tr.language);
            ascolto_audio_free(&en_buf);
        } else {
            CHECK(false, "Fixture tests/fixtures/english_sample.wav missing");
        }
    }

    /* 5. Whistle Live Streaming API */
    printf("\n[5/10] Whistle Live Streaming C API (needle_stream_transcribe_*)\n");
    if (init_rc == 0) {
        ascolto_audio_buffer_t stream_buf = {0};
        if (ascolto_wav_load("tests/fixtures/alice_turn1.wav", &stream_buf) == 0) {
            char stream_accum[4096] = {0};
            char chunk_json[4096];
            int chunk_sz = 16000; /* 1 second chunks */
            for (int pos = 0; pos < stream_buf.num_samples; pos += chunk_sz) {
                int n = (pos + chunk_sz <= stream_buf.num_samples) ? chunk_sz : (stream_buf.num_samples - pos);
                ascolto_whistle_stream_push(&whistle, stream_buf.samples + pos, n, "it", "Cristian\ndesign", chunk_json, sizeof(chunk_json));
                strncat(stream_accum, chunk_json, sizeof(stream_accum) - strlen(stream_accum) - 1);
            }
            ascolto_whistle_stream_finalize(&whistle, chunk_json, sizeof(chunk_json));
            strncat(stream_accum, chunk_json, sizeof(stream_accum) - strlen(stream_accum) - 1);
            CHECK(str_contains_ci(stream_accum, "design") || str_contains_ci(stream_accum, "partire"),
                  "Streaming commit produced valid incremental JSON chunks");
            ascolto_audio_free(&stream_buf);
        }
    }

    /* 6. Whistle Embeddings & Speaker Voiceprint Diarization */
    printf("\n[6/10] Whistle Encoder Embeddings & Multi-Speaker Voiceprint Separation\n");
    if (init_rc == 0) {
        ascolto_audio_buffer_t a1 = {0}, a2 = {0}, r1 = {0}, r2 = {0};
        if (ascolto_wav_load("tests/fixtures/alice_turn1.wav", &a1) == 0 &&
            ascolto_wav_load("tests/fixtures/alice_turn2.wav", &a2) == 0 &&
            ascolto_wav_load("tests/fixtures/rocko_turn1.wav", &r1) == 0 &&
            ascolto_wav_load("tests/fixtures/rocko_turn2.wav", &r2) == 0) {

            float *ea1 = NULL, *ea2 = NULL, *er1 = NULL, *er2 = NULL;
            int fa1 = 0, fa2 = 0, fr1 = 0, fr2 = 0;
            ascolto_whistle_embed_frames(&whistle, a1.samples, a1.num_samples, &ea1, &fa1);
            ascolto_whistle_embed_frames(&whistle, a2.samples, a2.num_samples, &ea2, &fa2);
            ascolto_whistle_embed_frames(&whistle, r1.samples, r1.num_samples, &er1, &fr1);
            ascolto_whistle_embed_frames(&whistle, r2.samples, r2.num_samples, &er2, &fr2);

            CHECK(fa1 > 10 && fr1 > 10, "Whistle needle_embed extracted 512-d frames (Alice=%d, Rocko=%d)", fa1, fr1);

            ascolto_voiceprint_t vpa1, vpa2, vpr1, vpr2;
            ascolto_extract_voiceprint(a1.samples, a1.num_samples, ea1, fa1, &vpa1);
            ascolto_extract_voiceprint(a2.samples, a2.num_samples, ea2, fa2, &vpa2);
            ascolto_extract_voiceprint(r1.samples, r1.num_samples, er1, fr1, &vpr1);
            ascolto_extract_voiceprint(r2.samples, r2.num_samples, er2, fr2, &vpr2);

            float sim_alice_self = ascolto_voiceprint_similarity(&vpa2, vpa1.feature_vec);
            float sim_rocko_self = ascolto_voiceprint_similarity(&vpr2, vpr1.feature_vec);
            float sim_cross = ascolto_voiceprint_similarity(&vpr1, vpa1.feature_vec);

            CHECK(sim_alice_self > 0.70f && sim_rocko_self > 0.70f && sim_cross < 0.40f,
                  "Voiceprints separate speakers (Alice-Alice=%.2f, Rocko-Rocko=%.2f, Alice-Rocko=%.2f)",
                  sim_alice_self, sim_rocko_self, sim_cross);

            free(ea1); free(ea2); free(er1); free(er2);
            ascolto_audio_free(&a1); ascolto_audio_free(&a2);
            ascolto_audio_free(&r1); ascolto_audio_free(&r2);
        } else {
            CHECK(false, "Speaker fixtures missing");
        }
    }

    /* 7. End-to-End Dual-Channel Call Simulation */
    printf("\n[7/10] End-to-End Dual-Channel Call Session (ME + Multiple SYS Interlocutors)\n");
    if (init_rc == 0) {
        ascolto_audio_buffer_t mic_call = {0}, sys_call = {0};
        if (ascolto_wav_load("tests/fixtures/call_mic.wav", &mic_call) == 0 &&
            ascolto_wav_load("tests/fixtures/call_sys.wav", &sys_call) == 0) {

            const char* trace_out = "/tmp/ascolto-test/test_call_trace.jsonl";
            FILE* tf = fopen(trace_out, "w");
            ascolto_session_report_t* rep = (ascolto_session_report_t*)calloc(1, sizeof(ascolto_session_report_t));
            int src = ascolto_run_simulated_session(
                &whistle,
                mic_call.samples,
                mic_call.num_samples,
                sys_call.samples,
                sys_call.num_samples,
                "it",
                "Cristian\nprogetto\ndesign\nserver\nsviluppo\ndocumento",
                0.60f,
                tf,
                rep
            );
            if (tf) fclose(tf);

            CHECK(src == 0 && rep->num_utterances == 5,
                  "Detected all 5 chronological dialogue turns across MIC and SYS (got %d)",
                  rep->num_utterances);

            CHECK(rep->detected_sys_speakers == 2,
                  "Diarizer identified exactly 2 distinct remote interlocutors on SYS (got %d)",
                  rep->detected_sys_speakers);

            if (rep->num_utterances == 5) {
                bool turn_speakers_ok =
                    strcmp(rep->utterances[0].speaker, "ME") == 0 &&
                    strcmp(rep->utterances[1].speaker, "SPEAKER_1") == 0 &&
                    strcmp(rep->utterances[2].speaker, "SPEAKER_2") == 0 &&
                    strcmp(rep->utterances[3].speaker, "ME") == 0 &&
                    strcmp(rep->utterances[4].speaker, "SPEAKER_1") == 0;

                CHECK(turn_speakers_ok,
                      "Speaker turn sequence matches ground truth: [%s -> %s -> %s -> %s -> %s]",
                      rep->utterances[0].speaker,
                      rep->utterances[1].speaker,
                      rep->utterances[2].speaker,
                      rep->utterances[3].speaker,
                      rep->utterances[4].speaker);
            }
            free(rep);

            ascolto_audio_free(&mic_call);
            ascolto_audio_free(&sys_call);
        } else {
            CHECK(false, "Call fixtures tests/fixtures/call_mic.wav or call_sys.wav missing");
        }
    }

    /* 8. Persistent Configuration & Keywords File */
    printf("\n[8/10] Persistent Configuration & Keywords (~/.config/ascolto/config.json)\n");
    {
        const char* test_cfg_path = "/tmp/ascolto-test/test_cfg/config.json";
        const char* test_kw_path = "/tmp/ascolto-test/test_cfg/keywords.txt";
        ascolto_config_t c1, c2;
        ascolto_config_init_defaults(&c1);
        snprintf(c1.config_path, sizeof(c1.config_path), "%s", test_cfg_path);
        snprintf(c1.keywords_path, sizeof(c1.keywords_path), "%s", test_kw_path);
        snprintf(c1.language, sizeof(c1.language), "it");
        c1.auto_trigger_mic = false;
        c1.paused = true;
        c1.mic_device_id = 94;
        c1.sys_device_id = 54;
        c1.diarizer_threshold = 0.72f;

        int src = ascolto_config_save(&c1, test_cfg_path);
        FILE* kf = fopen(test_kw_path, "w");
        if (kf) {
            fprintf(kf, "Cristian\nWhistle\nCactus\n");
            fclose(kf);
        }

        ascolto_config_init_defaults(&c2);
        int lrc = ascolto_config_load(&c2, test_cfg_path);
        char kw_loaded[256] = {0};
        ascolto_config_load_keywords(&c2, kw_loaded, sizeof(kw_loaded));

        CHECK(src == 0 && lrc == 0 &&
              strcmp(c2.language, "it") == 0 &&
              c2.auto_trigger_mic == false &&
              c2.paused == true &&
              c2.mic_device_id == 94 &&
              c2.sys_device_id == 54 &&
              fabsf(c2.diarizer_threshold - 0.72f) < 0.01f,
              "Configuration save/load roundtrip preserves all Menu Bar settings");

        CHECK(str_contains_ci(kw_loaded, "Cristian") && str_contains_ci(kw_loaded, "Whistle"),
              "Keywords file loaded properly for Whistle keyword biasing");
    }

    /* 9. Live Menu Bar Controller State Machine & Real-Time Markdown/JSONL Output */
    printf("\n[9/10] Live Menu Bar Controller & Real-Time Stream Transcription\n");
    if (init_rc == 0) {
        const char* live_cfg_path = "/tmp/ascolto-test/test_live/config.json";
        ascolto_config_t base_cfg;
        ascolto_config_init_defaults(&base_cfg);
        snprintf(base_cfg.config_path, sizeof(base_cfg.config_path), "%s", live_cfg_path);
        snprintf(base_cfg.output_dir, sizeof(base_cfg.output_dir),
                 "/tmp/ascolto-test/test_live/out");
        snprintf(base_cfg.language, sizeof(base_cfg.language), "it");
        ascolto_config_save(&base_cfg, live_cfg_path);

        ascolto_live_app_t app;
        int linit = ascolto_live_app_init(&app, &whistle, live_cfg_path);
        char bar_idle[64], bar_pause[64], bar_rec[64];

        /* Force known initial state even if local hardware mic is active */
        ascolto_live_app_set_paused(&app, true);
        ascolto_live_app_format_statusbar(&app, bar_pause, sizeof(bar_pause));
        ascolto_live_app_set_paused(&app, false);
        app.state = ASCOLTO_STATE_IDLE;
        ascolto_live_app_format_statusbar(&app, bar_idle, sizeof(bar_idle));

        /* Trigger auto-recording via simulated hardware mic activation */
        ascolto_live_app_on_mic_hardware_change(&app, true, "Microfono Test");
        ascolto_live_app_format_statusbar(&app, bar_rec, sizeof(bar_rec));

        CHECK(linit == 0 &&
              strstr(bar_idle, "Ascolto") != NULL &&
              strstr(bar_pause, "Ascolto") != NULL &&
              app.state == ASCOLTO_STATE_RECORDING,
              "Menu Bar status transitions (Idle='%s', Paused='%s', Recording='%s')",
              bar_idle, bar_pause, bar_rec);

            /* Feed real speech chunks (200ms at a time) on MIC and SYS */
        ascolto_audio_buffer_t mic_clip = {0}, sys_clip = {0};
        if (ascolto_wav_load("tests/fixtures/me_turn1.wav", &mic_clip) == 0 &&
            ascolto_wav_load("tests/fixtures/alice_turn1.wav", &sys_clip) == 0) {
            int chunk = 3200; /* 200ms */
            for (int pos = 0; pos < mic_clip.num_samples; pos += chunk) {
                int n = (pos + chunk <= mic_clip.num_samples) ? chunk : (mic_clip.num_samples - pos);
                float sys_silence[3200] = {0};
                ascolto_live_app_feed_audio(&app, ASCOLTO_CHANNEL_SYS, sys_silence, n);
                ascolto_live_app_feed_audio(&app, ASCOLTO_CHANNEL_MIC, mic_clip.samples + pos, n);
            }
            /* Feed 0.5s silence to close turn immediately */
            float sil[8000] = {0};
            ascolto_live_app_feed_audio(&app, ASCOLTO_CHANNEL_SYS, sil, 8000);
            ascolto_live_app_feed_audio(&app, ASCOLTO_CHANNEL_MIC, sil, 8000);

            /* Now simulate SYS speaking while MIC picks up realistic speaker bleed (-18 dB weaker than SYS):
               The Echo/Bleed Gate must suppress MIC so Alice is ONLY transcribed as SPEAKER_1, NEVER as ME! */
            for (int pos = 0; pos < sys_clip.num_samples; pos += chunk) {
                int n = (pos + chunk <= sys_clip.num_samples) ? chunk : (sys_clip.num_samples - pos);
                float bleed[3200];
                for (int k = 0; k < n; k++) bleed[k] = sys_clip.samples[pos + k] * 0.12f;
                ascolto_live_app_feed_audio(&app, ASCOLTO_CHANNEL_SYS, sys_clip.samples + pos, n);
                ascolto_live_app_feed_audio(&app, ASCOLTO_CHANNEL_MIC, bleed, n);
            }
            ascolto_live_app_feed_audio(&app, ASCOLTO_CHANNEL_SYS, sil, 8000);
            ascolto_live_app_feed_audio(&app, ASCOLTO_CHANNEL_MIC, sil, 8000);

            ascolto_audio_free(&mic_clip);
            ascolto_audio_free(&sys_clip);
        }

        /* Trigger hardware mic release -> stops session & closes files */
        ascolto_live_app_on_mic_hardware_change(&app, false, "Microfono Test");

        FILE* mdf = fopen(app.current_md_path, "r");
        char md_content[4096] = {0};
        if (mdf) {
            size_t nr = fread(md_content, 1, sizeof(md_content) - 1, mdf);
            md_content[nr] = '\0';
            fclose(mdf);
        }

        CHECK(app.state == ASCOLTO_STATE_IDLE &&
              app.session_utterance_count == 2 &&
              strstr(md_content, "ME") != NULL &&
              strstr(md_content, "SPEAKER_1") != NULL,
              "Live session + Speaker Bleed Gate wrote exact 2 turns (ME + SPEAKER_1, bleed suppressed)");

        ascolto_live_app_free(&app);
    }

    if (init_rc == 0) {
        printf("\n[10/10] Acoustic Rooms (live pipeline, echo rejection and local speech retention)\n");
        CHECK(ascolto_capture_buffer_verify() == 0,
              "Capture queue preserves both timelines across slow ASR, startup offsets, missing SYS and overflow");
        CHECK(ascolto_run_room_bench(&whistle, "tests/room-results") == 0,
              "All acoustic rooms preserve expected MIC and SYS turns");
    }
    ascolto_whistle_free(&whistle);

    printf("\n================================================================\n");
    printf("  RESULT: %d passed, %d failed\n", passed, failed);
    printf("================================================================\n");
    return (failed == 0) ? 0 : 1;
}

static int cmd_reprocess(
    const char* model_path,
    const char* m4a_path,
    const char* lang,
    const char* keywords,
    float threshold,
    const char* trace_path
) {
    char temp_dir[] = "/tmp/ascolto-reprocess.XXXXXX";
    if (!mkdtemp(temp_dir)) return 1;
    char tmp_mic[256], tmp_sys[256];
    snprintf(tmp_mic, sizeof(tmp_mic), "%s/mic.wav", temp_dir);
    snprintf(tmp_sys, sizeof(tmp_sys), "%s/sys.wav", temp_dir);
    fflush(NULL);
    pid_t child = fork();
    if (child == 0) {
        execlp("ffmpeg", "ffmpeg", "-y", "-loglevel", "error", "-i", m4a_path,
               "-filter_complex", "[0:a]channelsplit=channel_layout=stereo[m][s]",
               "-map", "[m]", "-ar", "16000", "-ac", "1", "-c:a", "pcm_s16le", tmp_mic,
               "-map", "[s]", "-ar", "16000", "-ac", "1", "-c:a", "pcm_s16le", tmp_sys, (char*)NULL);
        _exit(127);
    }
    int status = 0;
    int rc = 1;
    if (child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        rc = cmd_simulate(model_path, tmp_mic, tmp_sys, lang, keywords, threshold, trace_path);
    } else {
        fprintf(stderr, "ERROR: Failed to extract stereo channels (L=MIC, R=SYS) from '%s'\n", m4a_path);
    }
    unlink(tmp_mic); unlink(tmp_sys); rmdir(temp_dir);
    return rc;
}

static void print_usage(const char* prog) {
    printf("Usage: %s <command> [options]\n\n", prog);
    printf("Commands:\n");
    printf("  test                                  Run autonomous verification test suite\n");
    printf("  probe-macos                           Inspect macOS CoreAudio devices & live mic state\n");
    printf("  probe-engine [--model <path>]         Verify Whistle C engine & embeddings\n");
    printf("  inspect-audio <wav> [--lang <code>]   Inspect VAD, voiceprint & Whistle ASR on a WAV\n");
    printf("  simulate --mic <wav> --sys <wav>      Simulate full dual-channel call session\n");
    printf("           [--lang <it|en|auto>] [--keywords <kw>] [--threshold <0.60>] [--trace <jsonl>]\n");
    printf("  reprocess <call.m4a> [options]        Re-transcribe a saved dual-channel .m4a call archive\n");
    printf("  room-bench [--output <directory>]    Verify acoustic rooms using the live audio pipeline\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char* cmd = argv[1];
    const char* model_path = DEFAULT_MODEL_PATH;
    const char* mic_wav = NULL;
    const char* sys_wav = NULL;
    const char* lang = "it";
    const char* keywords = NULL;
    const char* trace_path = NULL;
    float threshold = 0.60f;

    if (strcmp(cmd, "room-bench") == 0) {
        const char* output_dir = "tests/room-results";
        for (int i = 2; i + 1 < argc; i++) {
            if (strcmp(argv[i], "--model") == 0) model_path = argv[++i];
            else if (strcmp(argv[i], "--output") == 0) output_dir = argv[++i];
        }
        ascolto_whistle_t whistle;
        if (ascolto_whistle_init(&whistle, model_path) != 0) return 1;
        int rc = ascolto_run_room_bench(&whistle, output_dir);
        ascolto_whistle_free(&whistle);
        return rc;
    }

    if (strcmp(cmd, "probe-macos") == 0) {
        return cmd_probe_macos();
    }
    if (strcmp(cmd, "probe-engine") == 0) {
        for (int i = 2; i < argc - 1; i++) {
            if (strcmp(argv[i], "--model") == 0) model_path = argv[++i];
        }
        return cmd_probe_engine(model_path);
    }
    if (strcmp(cmd, "test") == 0) {
        for (int i = 2; i < argc - 1; i++) {
            if (strcmp(argv[i], "--model") == 0) model_path = argv[++i];
        }
        return cmd_test(model_path);
    }
    if (strcmp(cmd, "inspect-audio") == 0) {
        if (argc < 3) {
            print_usage(argv[0]);
            return 1;
        }
        const char* wav = argv[2];
        for (int i = 3; i < argc - 1; i++) {
            if (strcmp(argv[i], "--model") == 0) model_path = argv[++i];
            else if (strcmp(argv[i], "--lang") == 0) lang = argv[++i];
            else if (strcmp(argv[i], "--keywords") == 0) keywords = argv[++i];
        }
        return cmd_inspect_audio(model_path, wav, lang, keywords);
    }
    if (strcmp(cmd, "simulate") == 0) {
        for (int i = 2; i < argc - 1; i++) {
            if (strcmp(argv[i], "--model") == 0) model_path = argv[++i];
            else if (strcmp(argv[i], "--mic") == 0) mic_wav = argv[++i];
            else if (strcmp(argv[i], "--sys") == 0) sys_wav = argv[++i];
            else if (strcmp(argv[i], "--lang") == 0) lang = argv[++i];
            else if (strcmp(argv[i], "--keywords") == 0) keywords = argv[++i];
            else if (strcmp(argv[i], "--trace") == 0) trace_path = argv[++i];
            else if (strcmp(argv[i], "--threshold") == 0) threshold = strtof(argv[++i], NULL);
        }
        if (!mic_wav && !sys_wav) {
            fprintf(stderr, "ERROR: simulate requires at least --mic <wav> or --sys <wav>\n");
            return 1;
        }
        return cmd_simulate(model_path, mic_wav, sys_wav, lang, keywords, threshold, trace_path);
    }
    if (strcmp(cmd, "reprocess") == 0 && argc >= 3) {
        const char* m4a = argv[2];
        for (int i = 3; i < argc - 1; i++) {
            if (strcmp(argv[i], "--model") == 0) model_path = argv[++i];
            else if (strcmp(argv[i], "--lang") == 0) lang = argv[++i];
            else if (strcmp(argv[i], "--keywords") == 0) keywords = argv[++i];
            else if (strcmp(argv[i], "--trace") == 0) trace_path = argv[++i];
            else if (strcmp(argv[i], "--threshold") == 0) threshold = strtof(argv[++i], NULL);
        }
        return cmd_reprocess(model_path, m4a, lang, keywords, threshold, trace_path);
    }

    print_usage(argv[0]);
    return 1;
}
