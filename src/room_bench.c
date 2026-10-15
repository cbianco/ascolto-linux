#include "ascolto/offline.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    double start_s;
    double end_s;
    ascolto_channel_t channel;
    int speaker;
    const char* keyword;
    int matches;
} expected_turn_t;

typedef struct {
    const char* name;
    float echo_gain;
    bool muted;
    bool quiet_reply;
    bool barge_in;
    bool irregular_chunks;
    bool long_turn;
    bool third_speaker;
} acoustic_room_t;

static void mix_clip(float* dst, int length, const ascolto_audio_buffer_t* clip,
                     double start_s, float target_db) {
    int start = (int)lround(start_s * ASCOLTO_SAMPLE_RATE);
    float gain = powf(10.0f, (target_db - ascolto_audio_rms_db(clip->samples, clip->num_samples)) / 20.0f);
    for (int i = 0; i < clip->num_samples && start + i < length; i++) {
        dst[start + i] += clip->samples[i] * gain;
    }
}

static void add_turn(expected_turn_t* turns, int* count, const ascolto_audio_buffer_t* clip,
                     double start, ascolto_channel_t channel, int speaker, const char* keyword) {
    turns[*count] = (expected_turn_t){start, start + (double)clip->num_samples / ASCOLTO_SAMPLE_RATE,
                                   channel, speaker, keyword, 0};
    (*count)++;
}

static double json_number(const char* line, const char* key) {
    char pattern[80];
    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    const char* p = strstr(line, pattern);
    return p ? strtod(p + strlen(pattern), NULL) : 0.0;
}

static int verify_gate_boundaries(const char* output_dir) {
    char path[640];
    snprintf(path, sizeof(path), "%s/gate-config.json", output_dir);
    ascolto_config_t cfg;
    ascolto_config_init_defaults(&cfg);
    cfg.auto_trigger_mic = false;
    cfg.save_audio = false;
    snprintf(cfg.output_dir, sizeof(cfg.output_dir), "%s", output_dir);
    ascolto_config_save(&cfg, path);
    ascolto_live_app_t app;
    if (ascolto_live_app_init(&app, NULL, path) != 0) return 1;
    ascolto_live_app_start_session(&app, true);
    float quiet_sys[320], local[320], loud_sys[320], silence[320] = {0};
    for (int i = 0; i < 320; i++) {
        float wave = sqrtf(2.0f) * sinf(2.0f * 3.14159265f * i / 80.0f);
        quiet_sys[i] = wave * powf(10.0f, -34.0f / 20.0f);
        local[i] = wave * powf(10.0f, -30.0f / 20.0f);
        loud_sys[i] = wave * powf(10.0f, -20.0f / 20.0f);
    }
    for (int i = 0; i < 3; i++) {
        ascolto_live_app_feed_audio(&app, ASCOLTO_CHANNEL_SYS, quiet_sys, 320);
        ascolto_live_app_feed_audio(&app, ASCOLTO_CHANNEL_MIC, local, 320);
    }
    bool above_margin = app.vad_mic.in_speech && app.mic_bleed_frames_suppressed == 0;
    ascolto_live_app_stop_session(&app);
    ascolto_live_app_start_session(&app, true);
    ascolto_live_app_feed_audio(&app, ASCOLTO_CHANNEL_SYS, loud_sys, 320);
    ascolto_live_app_feed_audio(&app, ASCOLTO_CHANNEL_MIC, loud_sys, 320);
    bool echo_blocked = !app.vad_mic.in_speech && app.mic_bleed_frames_suppressed == 1;
    for (int i = 0; i < 12; i++) {
        ascolto_live_app_feed_audio(&app, ASCOLTO_CHANNEL_SYS, silence, 320);
        ascolto_live_app_feed_audio(&app, ASCOLTO_CHANNEL_MIC, silence, 320);
    }
    ascolto_live_app_feed_audio(&app, ASCOLTO_CHANNEL_SYS, silence, 320);
    ascolto_live_app_feed_audio(&app, ASCOLTO_CHANNEL_MIC, quiet_sys, 320);
    bool quiet_after_sys = app.vad_mic.in_speech && app.mic_bleed_frames_suppressed == 1;
    ascolto_live_app_stop_session(&app);
    ascolto_live_app_free(&app);
    bool pass = above_margin && echo_blocked && quiet_after_sys;
    printf("  [%s] Gate boundaries: quiet local interruption above +3.5dB, active echo suppression, release within 250ms\n",
           pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

int ascolto_run_room_bench(ascolto_whistle_t* whistle, const char* output_dir) {
    const acoustic_room_t rooms[] = {
        {"headphones", 0.02f, false, false, false, false, false, false},
        {"speakers-muted", 1.05f, true, false, false, false, false, false},
        {"speakers-dialogue", 0.85f, false, false, false, false, false, false},
        {"quiet-reply", 0.85f, false, true, false, false, false, false},
        {"barge-in", 0.85f, false, false, true, false, false, false},
        {"irregular-callbacks", 1.05f, true, false, false, true, false, false},
        {"long-remote-turn", 1.05f, true, false, false, true, true, false},
        {"three-remote-speakers", 1.05f, true, false, false, false, false, true},
    };
    const char* files[] = {"alice_turn1.wav", "alice_turn2.wav", "rocko_turn1.wav",
                           "rocko_turn2.wav", "me_turn1.wav", "me_turn2.wav", "spk3_turn1.wav"};
    ascolto_audio_buffer_t clips[7] = {{0}};
    int rc = 1;
    for (int i = 0; i < 7; i++) {
        char path[512];
        snprintf(path, sizeof(path), "tests/fixtures/%s", files[i]);
        if (ascolto_wav_load(path, &clips[i]) != 0) {
            fprintf(stderr, "Room fixture missing: %s (run make fixtures)\n", path);
            goto cleanup;
        }
    }
    mkdir(output_dir, 0755);
    char summary_path[640];
    snprintf(summary_path, sizeof(summary_path), "%s/metrics.jsonl", output_dir);
    FILE* summary = fopen(summary_path, "w");
    if (!summary) goto cleanup;
    int failures = verify_gate_boundaries(output_dir);
    for (size_t r = 0; r < sizeof(rooms) / sizeof(rooms[0]); r++) {
        const acoustic_room_t* room = &rooms[r];
        int length = ASCOLTO_SAMPLE_RATE * (room->long_turn ? 48 : 30);
        float* mic = calloc((size_t)length, sizeof(float));
        float* sys = calloc((size_t)length, sizeof(float));
        if (!mic || !sys) { free(mic); free(sys); failures++; break; }
        expected_turn_t turns[20];
        int n_turns = 0;
        double remote_starts[] = {0.5, 6.0, 16.0, 21.0};
        int remote_clips[] = {0, 2, 1, 3};
        if (room->long_turn) {
            for (int i = 0; i < 10; i++) {
                /* Dense turns exceed the 10s VAD limit without changing speaker. */
                mix_clip(sys, length, &clips[0], 0.5 + i * 3.4, -21.0f);
            }
            turns[n_turns++] = (expected_turn_t){0.5, 35.0, ASCOLTO_CHANNEL_SYS, 1, "partire", 0};
        } else {
            for (int i = 0; i < 4; i++) {
                float db = (room->barge_in && i == 1) ? -31.0f : -21.0f;
                mix_clip(sys, length, &clips[remote_clips[i]], remote_starts[i], db);
                add_turn(turns, &n_turns, &clips[remote_clips[i]], remote_starts[i],
                         ASCOLTO_CHANNEL_SYS, (i % 2) + 1, i % 2 ? "server" : "partire");
                if (i == 2) turns[n_turns - 1].keyword = "documento";
                if (i == 3) turns[n_turns - 1].keyword = "controlli";
            }
        }
        if (room->third_speaker) {
            mix_clip(sys, length, &clips[6], 11.0, -21.0f);
            add_turn(turns, &n_turns, &clips[6], 11.0, ASCOLTO_CHANNEL_SYS, 3, "migrazione");
            mix_clip(sys, length, &clips[6], 26.0, -21.0f);
            add_turn(turns, &n_turns, &clips[6], 26.0, ASCOLTO_CHANNEL_SYS, 3, "migrazione");
        }
        if (!room->muted) {
            double reply_start = room->quiet_reply
                ? 6.0 + (double)clips[2].num_samples / ASCOLTO_SAMPLE_RATE + 0.26 : 11.0;
            float reply_db = room->quiet_reply ? -34.0f : -27.0f;
            mix_clip(mic, length, &clips[4], reply_start, reply_db);
            add_turn(turns, &n_turns, &clips[4], reply_start, ASCOLTO_CHANNEL_MIC, 0, "progetto");
            mix_clip(mic, length, &clips[5], 26.0, -28.0f);
            add_turn(turns, &n_turns, &clips[5], 26.0, ASCOLTO_CHANNEL_MIC, 0, "sviluppo");
            if (room->barge_in) {
                mix_clip(mic, length, &clips[5], 6.4, -21.0f);
                add_turn(turns, &n_turns, &clips[5], 6.4, ASCOLTO_CHANNEL_MIC, 0, "sviluppo");
            }
        }
        /* Deterministic noise plus delayed direct sound and desk reflections. */
        unsigned int random_state = 173;
        for (int i = 0; i < length; i++) {
            random_state = random_state * 1664525u + 1013904223u;
            mic[i] += ((float)(random_state >> 8) / 16777216.0f - 0.5f) * 0.002f;
            int delays[] = {480, 1120, 2400}; /* 30, 70 and 150ms */
            float gains[] = {0.82f, 0.14f, 0.04f};
            for (int j = 0; j < 3; j++) {
                if (i >= delays[j]) mic[i] += sys[i - delays[j]] * room->echo_gain * gains[j];
            }
        }

        char room_dir[640], cfg_path[700], mic_path[700], sys_path[700];
        snprintf(room_dir, sizeof(room_dir), "%s/%s", output_dir, room->name);
        mkdir(room_dir, 0755);
        snprintf(cfg_path, sizeof(cfg_path), "%s/config.json", room_dir);
        snprintf(mic_path, sizeof(mic_path), "%s/input-mic.wav", room_dir);
        snprintf(sys_path, sizeof(sys_path), "%s/input-sys.wav", room_dir);
        ascolto_wav_save(mic_path, mic, length, ASCOLTO_SAMPLE_RATE);
        ascolto_wav_save(sys_path, sys, length, ASCOLTO_SAMPLE_RATE);
        ascolto_config_t cfg;
        ascolto_config_init_defaults(&cfg);
        cfg.auto_trigger_mic = false;
        cfg.save_audio = false;
        snprintf(cfg.language, sizeof(cfg.language), "it");
        snprintf(cfg.output_dir, sizeof(cfg.output_dir), "%s", room_dir);
        snprintf(cfg.keywords_path, sizeof(cfg.keywords_path), "%s/keywords.txt", room_dir);
        ascolto_config_save(&cfg, cfg_path);
        ascolto_live_app_t app;
        ascolto_live_app_init(&app, whistle, cfg_path);
        ascolto_live_app_start_session(&app, true);
        int chunk_sizes[] = {171, 640, 319, 3200, 513, 97};
        for (int pos = 0, iteration = 0; pos < length; iteration++) {
            int n = room->irregular_chunks ? chunk_sizes[iteration % 6] : 640;
            if (n > length - pos) n = length - pos;
            ascolto_live_app_feed_audio(&app, ASCOLTO_CHANNEL_SYS, sys + pos, n);
            ascolto_live_app_feed_audio(&app, ASCOLTO_CHANNEL_MIC, mic + pos, n);
            pos += n;
        }
        ascolto_live_app_stop_session(&app);
        FILE* trace = fopen(app.files.jsonl_path, "r");
        int false_me = 0, mic_turns = 0, sys_turns = 0, wrong_speaker = 0;
        int labels[4] = {0};
        char line[8192];
        while (trace && fgets(line, sizeof(line), trace)) {
            if (!strstr(line, "\"event\":\"utterance\"")) continue;
            bool is_mic = strstr(line, "\"channel\":\"MIC\"") != NULL;
            if (is_mic) mic_turns++; else sys_turns++;
            double start = json_number(line, "start_s"), end = json_number(line, "end_s");
            int match = -1;
            double best_overlap = 0;
            for (int i = 0; i < n_turns; i++) {
                if (is_mic != (turns[i].channel == ASCOLTO_CHANNEL_MIC)) continue;
                double overlap = fmin(end, turns[i].end_s) - fmax(start, turns[i].start_s);
                if (overlap > best_overlap) { best_overlap = overlap; match = i; }
            }
            if (match < 0) { if (is_mic) false_me++; else wrong_speaker++; continue; }
            /* Check speech content as well as timing: a silent VAD turn is insufficient. */
            if (strstr(line, turns[match].keyword)) turns[match].matches++;
            if (!is_mic) {
                const char* label = strstr(line, "\"speaker\":\"SPEAKER_");
                int id = label ? atoi(label + strlen("\"speaker\":\"SPEAKER_")) : 0;
                int expected = turns[match].speaker;
                if (labels[expected] == 0) labels[expected] = id;
                if (id == 0 || labels[expected] != id) wrong_speaker++;
            }
        }
        if (trace) fclose(trace);
        int missed_mic = 0, missed_sys = 0;
        for (int i = 0; i < n_turns; i++) {
            if (turns[i].matches == 0) {
                if (turns[i].channel == ASCOLTO_CHANNEL_MIC) missed_mic++; else missed_sys++;
            }
        }
        int expected_speakers = room->long_turn ? 1 : (room->third_speaker ? 3 : 2);
        bool pass = trace && false_me == 0 && missed_mic == 0 && missed_sys == 0 &&
                    wrong_speaker == 0 && app.diarizer.num_speakers == expected_speakers &&
                    (expected_speakers == 1 || labels[1] != labels[2]) &&
                    (expected_speakers < 3 || (labels[3] != labels[1] && labels[3] != labels[2]));
        printf("  [%s] %s: MIC=%d SYS=%d false_ME=%d missed_MIC=%d missed_SYS=%d speakers=%d/%d gated_frames=%d\n",
               pass ? "PASS" : "FAIL", room->name, mic_turns, sys_turns, false_me,
               missed_mic, missed_sys, app.diarizer.num_speakers, expected_speakers,
               app.mic_bleed_frames_suppressed);
        fprintf(summary, "{\"room\":\"%s\",\"passed\":%s,\"mic_turns\":%d,\"sys_turns\":%d,"
                "\"false_me\":%d,\"missed_mic\":%d,\"missed_sys\":%d,\"wrong_speaker\":%d,"
                "\"speakers\":%d,\"expected_speakers\":%d,\"gated_frames\":%d,\"echo_turns\":%d}\n",
                room->name, pass ? "true" : "false", mic_turns, sys_turns, false_me,
                missed_mic, missed_sys, wrong_speaker, app.diarizer.num_speakers, expected_speakers,
                app.mic_bleed_frames_suppressed, app.mic_echo_turns_suppressed);
        if (!pass) failures++;
        ascolto_live_app_free(&app);
        free(mic); free(sys);
    }
    fclose(summary);
    printf("  Room feedback: %s\n", summary_path);
    rc = failures ? 1 : 0;
cleanup:
    for (int i = 0; i < 7; i++) ascolto_audio_free(&clips[i]);
    return rc;
}
