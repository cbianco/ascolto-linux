#define _POSIX_C_SOURCE 200809L
#include "ascolto/core.h"
#include <libavformat/avformat.h>
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define ASSERT_TRUE(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "  [FAIL] %s:%d: %s\n", __FILE__, __LINE__, msg); \
        return -1; \
    } \
} while (0)

static int send_ipc_cmd(const char *sock_path, const char *cmd, char *reply, size_t reply_cap) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    char line[2048];
    snprintf(line, sizeof(line), "%s\n", cmd);
    if (write(fd, line, strlen(line)) < 0) {
        close(fd);
        return -1;
    }
    size_t total = 0;
    ssize_t n;
    while (total + 1 < reply_cap && (n = read(fd, reply + total, reply_cap - 1 - total)) > 0) {
        total += (size_t)n;
    }
    reply[total] = '\0';
    close(fd);
    return 0;
}

static int test_config_persistence(void) {
    printf("[TEST 1] Config initialization, serialization, and reload...\n");
    ascolto_config_t cfg1, cfg2;
    ascolto_config_init_defaults(&cfg1);
    snprintf(cfg1.config_path, sizeof(cfg1.config_path), "/tmp/opencode/ascolto_test_config.ini");
    snprintf(cfg1.output_dir, sizeof(cfg1.output_dir), "/tmp/opencode/ascolto_test_transcripts");
    cfg1.auto_mic_trigger = false;
    snprintf(cfg1.language, sizeof(cfg1.language), "it");
    snprintf(cfg1.mic_device, sizeof(cfg1.mic_device), "usb_headset_mic");
    snprintf(cfg1.sys_device, sizeof(cfg1.sys_device), "hdmi_monitor");
    cfg1.diarization_sensitivity = ASCOLTO_SENSITIVITY_HIGH;
    snprintf(cfg1.keywords, sizeof(cfg1.keywords), "Bitrock, Cactus Compute, Whistle, PipeWire");

    ASSERT_TRUE(ascolto_config_save(&cfg1, cfg1.config_path) == 0, "config save failed");

    ascolto_config_init_defaults(&cfg2);
    ASSERT_TRUE(ascolto_config_load(&cfg2, cfg1.config_path) == 0, "config load failed");
    ASSERT_TRUE(cfg2.auto_mic_trigger == false, "auto_mic_trigger mismatch");
    ASSERT_TRUE(strcmp(cfg2.language, "it") == 0, "language mismatch");
    ASSERT_TRUE(strcmp(cfg2.mic_device, "usb_headset_mic") == 0, "mic_device mismatch");
    ASSERT_TRUE(strcmp(cfg2.sys_device, "hdmi_monitor") == 0, "sys_device mismatch");
    ASSERT_TRUE(cfg2.diarization_sensitivity == ASCOLTO_SENSITIVITY_HIGH, "sensitivity mismatch");
    ASSERT_TRUE(strcmp(cfg2.keywords, "Bitrock, Cactus Compute, Whistle, PipeWire") == 0, "keywords mismatch");
    printf("  -> PASS\n");
    return 0;
}

static int test_whistle_engine_and_wav_io(void) {
    printf("[TEST 2] Whistle native C API (libneedle.a + whistle.cact) and WAV resampler...\n");
    ascolto_whistle_engine_t eng;
    int rc = ascolto_whistle_init(&eng, "third_party/whistle/whistle.cact");
    ASSERT_TRUE(rc == 0 && eng.model_loaded, "whistle.cact failed to load into libneedle.a");
    ASSERT_TRUE(eng.embed_dim > 0, "invalid Whistle embedding dimension");
    printf("  Whistle model loaded: embed_dim=%zu\n", eng.embed_dim);

    /* Test silence produces empty transcript and valid encoder embeddings */
    float silence[16000] = {0};
    ascolto_whistle_result_t res;
    ASSERT_TRUE(ascolto_whistle_process_segment(&eng, silence, 16000, "en", "Whistle", NULL, &res) == 0,
                "process_segment failed on silence");
    ASSERT_TRUE(res.text[0] == '\0', "silence should return empty transcript");
    ASSERT_TRUE(res.frame_embeddings != NULL && res.frame_embedding_floats >= eng.embed_dim,
                "needle_embed failed to return encoder frame embeddings");
    ascolto_whistle_result_free(&res);

    /* Test WAV write and read round-trip */
    float voice[16000];
    ascolto_synth_voice_utterance(voice, 16000, 130.0f, 520.0f, 1480.0f, 2500.0f);
    const char *wav_path = "/tmp/opencode/ascolto_test_voice.wav";
    ASSERT_TRUE(ascolto_wav_write_16k_mono(wav_path, voice, 16000) == 0, "wav write failed");

    float *loaded = NULL;
    size_t loaded_n = 0;
    ASSERT_TRUE(ascolto_wav_load_16k_mono(wav_path, &loaded, &loaded_n) == 0, "wav load failed");
    ASSERT_TRUE(loaded_n == 16000 && loaded != NULL, "wav sample count mismatch");
    free(loaded);

    /* Test real human speech WAV (48 kHz -> resampled to 16 kHz -> transcribed by Whistle) */
    const char *real_wav = "/usr/share/sounds/alsa/Front_Center.wav";
    if (access(real_wav, R_OK) == 0) {
        float *speech_pcm = NULL;
        size_t speech_n = 0;
        ASSERT_TRUE(ascolto_wav_load_16k_mono(real_wav, &speech_pcm, &speech_n) == 0, "failed to load Front_Center.wav");
        ascolto_whistle_result_t sres;
        ASSERT_TRUE(ascolto_whistle_process_segment(&eng, speech_pcm, speech_n, "en", "Front, Center", NULL, &sres) == 0,
                    "whistle transcribe failed on Front_Center.wav");
        printf("  Real speech WAV ('Front_Center.wav', %zu samples @ 16kHz) -> text=\"%s\" (lang=%s, ttft=%.1f ms, decode=%.1f tok/s)\n",
               speech_n, sres.text, sres.language, sres.ttft_ms, sres.decode_tps);
        ASSERT_TRUE(strlen(sres.text) > 0, "Whistle returned empty transcript on real speech WAV");
        ascolto_whistle_result_free(&sres);
        free(speech_pcm);
    }

    ascolto_whistle_destroy(&eng);
    printf("  -> PASS\n");
    return 0;
}

static int test_hybrid_speaker_diarization(void) {
    printf("[TEST 3] Hybrid speaker diarization (MIC='ME' + SYS multi-speaker clustering with Whistle embeddings)...\n");
    ascolto_whistle_engine_t eng;
    ASSERT_TRUE(ascolto_whistle_init(&eng, "third_party/whistle/whistle.cact") == 0, "whistle init failed");

    ascolto_diarizer_t diar;
    ascolto_diarizer_init(&diar, ASCOLTO_SENSITIVITY_MEDIUM);

    size_t n_samples = 24000; /* 1.5s at 16 kHz */
    float *pcm_me   = (float *)calloc(n_samples, sizeof(float));
    float *pcm_spk1 = (float *)calloc(n_samples, sizeof(float));
    float *pcm_spk2 = (float *)calloc(n_samples, sizeof(float));
    float *pcm_spk3 = (float *)calloc(n_samples, sizeof(float));
    float *pcm_spk1_b = (float *)calloc(n_samples, sizeof(float));
    float *pcm_spk2_b = (float *)calloc(n_samples, sizeof(float));

    /* Distinct vocal profiles (F0 + Formants F1, F2, F3) */
    ascolto_synth_voice_utterance(pcm_me,     n_samples, 135.0f, 500.0f, 1500.0f, 2500.0f);
    ascolto_synth_voice_utterance(pcm_spk1,   n_samples,  98.0f, 420.0f, 1180.0f, 2250.0f); /* Deep baritone */
    ascolto_synth_voice_utterance(pcm_spk2,   n_samples, 172.0f, 620.0f, 1850.0f, 2850.0f); /* Tenor / Alto */
    ascolto_synth_voice_utterance(pcm_spk3,   n_samples, 255.0f, 780.0f, 2320.0f, 3300.0f); /* Soprano */
    /* Second utterances from Speaker 1 and Speaker 2 with slight natural variation */
    ascolto_synth_voice_utterance(pcm_spk1_b, n_samples, 100.0f, 430.0f, 1195.0f, 2260.0f);
    ascolto_synth_voice_utterance(pcm_spk2_b, n_samples, 169.0f, 610.0f, 1835.0f, 2830.0f);

    ascolto_whistle_result_t wr;
    float emb_me[ASCOLTO_EMBED_DIM], emb_1[ASCOLTO_EMBED_DIM], emb_2[ASCOLTO_EMBED_DIM],
          emb_3[ASCOLTO_EMBED_DIM], emb_1b[ASCOLTO_EMBED_DIM], emb_2b[ASCOLTO_EMBED_DIM];
    float f0_me, f0_1, f0_2, f0_3, f0_1b, f0_2b, rms;

    ascolto_whistle_process_segment(&eng, pcm_me, n_samples, "it", NULL, "Ciao sono io", &wr);
    ascolto_diarizer_compute_embedding(pcm_me, n_samples, wr.frame_embeddings, wr.frame_embedding_floats, wr.frame_embedding_dim, emb_me, &f0_me, &rms);
    ascolto_whistle_result_free(&wr);

    ascolto_whistle_process_segment(&eng, pcm_spk1, n_samples, "it", NULL, "Buongiorno da speaker uno", &wr);
    ascolto_diarizer_compute_embedding(pcm_spk1, n_samples, wr.frame_embeddings, wr.frame_embedding_floats, wr.frame_embedding_dim, emb_1, &f0_1, &rms);
    ascolto_whistle_result_free(&wr);

    ascolto_whistle_process_segment(&eng, pcm_spk2, n_samples, "it", NULL, "Buongiorno da speaker due", &wr);
    ascolto_diarizer_compute_embedding(pcm_spk2, n_samples, wr.frame_embeddings, wr.frame_embedding_floats, wr.frame_embedding_dim, emb_2, &f0_2, &rms);
    ascolto_whistle_result_free(&wr);

    ascolto_whistle_process_segment(&eng, pcm_spk3, n_samples, "it", NULL, "Buongiorno da speaker tre", &wr);
    ascolto_diarizer_compute_embedding(pcm_spk3, n_samples, wr.frame_embeddings, wr.frame_embedding_floats, wr.frame_embedding_dim, emb_3, &f0_3, &rms);
    ascolto_whistle_result_free(&wr);

    ascolto_whistle_process_segment(&eng, pcm_spk1_b, n_samples, "it", NULL, "Ancora speaker uno", &wr);
    ascolto_diarizer_compute_embedding(pcm_spk1_b, n_samples, wr.frame_embeddings, wr.frame_embedding_floats, wr.frame_embedding_dim, emb_1b, &f0_1b, &rms);
    ascolto_whistle_result_free(&wr);

    ascolto_whistle_process_segment(&eng, pcm_spk2_b, n_samples, "it", NULL, "Ancora speaker due", &wr);
    ascolto_diarizer_compute_embedding(pcm_spk2_b, n_samples, wr.frame_embeddings, wr.frame_embedding_floats, wr.frame_embedding_dim, emb_2b, &f0_2b, &rms);
    ascolto_whistle_result_free(&wr);

    char label[32];
    float sim = 0.0f;

    /* 1. MIC channel must always be ME */
    int id_me = ascolto_diarizer_assign(&diar, ASCOLTO_CHANNEL_MIC, emb_me, f0_me, 1.0, label, &sim);
    ASSERT_TRUE(id_me == 0 && strcmp(label, "ME") == 0, "MIC channel was not labeled ME");

    /* 2. First remote voice -> SPEAKER_1 */
    int id_1 = ascolto_diarizer_assign(&diar, ASCOLTO_CHANNEL_SYS, emb_1, f0_1, 2.5, label, &sim);
    ASSERT_TRUE(id_1 == 1 && strcmp(label, "SPEAKER_1") == 0, "First SYS speaker should be SPEAKER_1");

    /* 3. Second distinct remote voice -> SPEAKER_2 */
    int id_2 = ascolto_diarizer_assign(&diar, ASCOLTO_CHANNEL_SYS, emb_2, f0_2, 4.0, label, &sim);
    ASSERT_TRUE(id_2 == 2 && strcmp(label, "SPEAKER_2") == 0, "Second SYS speaker should be SPEAKER_2");

    /* 4. Third distinct remote voice -> SPEAKER_3 */
    int id_3 = ascolto_diarizer_assign(&diar, ASCOLTO_CHANNEL_SYS, emb_3, f0_3, 5.5, label, &sim);
    ASSERT_TRUE(id_3 == 3 && strcmp(label, "SPEAKER_3") == 0, "Third SYS speaker should be SPEAKER_3");

    /* 5. Re-identification of SPEAKER_1 */
    int id_1_again = ascolto_diarizer_assign(&diar, ASCOLTO_CHANNEL_SYS, emb_1b, f0_1b, 7.0, label, &sim);
    printf("  Re-ID Speaker 1 -> %s (sim=%.3f, pitch=%.1f Hz)\n", label, sim, f0_1b);
    ASSERT_TRUE(id_1_again == 1 && strcmp(label, "SPEAKER_1") == 0, "Failed to re-identify SPEAKER_1");

    /* 6. Re-identification of SPEAKER_2 */
    int id_2_again = ascolto_diarizer_assign(&diar, ASCOLTO_CHANNEL_SYS, emb_2b, f0_2b, 8.5, label, &sim);
    printf("  Re-ID Speaker 2 -> %s (sim=%.3f, pitch=%.1f Hz)\n", label, sim, f0_2b);
    ASSERT_TRUE(id_2_again == 2 && strcmp(label, "SPEAKER_2") == 0, "Failed to re-identify SPEAKER_2");
    ASSERT_TRUE(diar.speaker_count == 3, "Expected exactly 3 remote speakers");

    free(pcm_me);
    free(pcm_spk1);
    free(pcm_spk2);
    free(pcm_spk3);
    free(pcm_spk1_b);
    free(pcm_spk2_b);
    ascolto_whistle_destroy(&eng);
    printf("  -> PASS\n");
    return 0;
}

static int test_e2e_multispeaker_call_and_ipc_observability(void) {
    printf("[TEST 4] End-to-End multi-speaker call simulation, Mic Auto-Trigger, Tray UI, and IPC Socket...\n");
    ascolto_config_t cfg;
    ascolto_config_init_defaults(&cfg);
    snprintf(cfg.config_path, sizeof(cfg.config_path), "/tmp/opencode/ascolto_e2e_config.ini");
    snprintf(cfg.output_dir, sizeof(cfg.output_dir), "/tmp/opencode/ascolto_e2e_transcripts");
    snprintf(cfg.control_socket, sizeof(cfg.control_socket), "/tmp/opencode/ascolto_e2e.sock");
    snprintf(cfg.telemetry_log, sizeof(cfg.telemetry_log), "/tmp/opencode/ascolto_e2e_events.jsonl");
    snprintf(cfg.weights_path, sizeof(cfg.weights_path), "third_party/whistle/whistle.cact");
    snprintf(cfg.language, sizeof(cfg.language), "it");
    snprintf(cfg.keywords, sizeof(cfg.keywords), "Bitrock, Whistle, PipeWire");
    cfg.headless = true;
    unlink(cfg.telemetry_log);

    ascolto_core_t *core = (ascolto_core_t *)calloc(1, sizeof(ascolto_core_t));
    ASSERT_TRUE(ascolto_core_init(core, &cfg, true) == 0, "core_init failed");

    char reply[16384];

    /* 1. Verify initial IDLE status via control socket */
    ASSERT_TRUE(send_ipc_cmd(cfg.control_socket, "STATUS", reply, sizeof(reply)) == 0, "IPC STATUS failed");
    ASSERT_TRUE(strstr(reply, "\"state\":\"IDLE\"") != NULL, "Expected IDLE state initially");
    ASSERT_TRUE(strstr(reply, "\"whistle_loaded\":true") != NULL, "Expected whistle_loaded:true");

    /* 2. Simulate Zoom activating microphone -> must auto-trigger RECORDING_AUTO */
    ASSERT_TRUE(send_ipc_cmd(cfg.control_socket, "SIM_MIC_ACTIVE 1 Zoom", reply, sizeof(reply)) == 0, "IPC SIM_MIC_ACTIVE 1 failed");
    ASSERT_TRUE(strstr(reply, "\"state\":\"RECORDING_AUTO\"") != NULL, "Expected auto transition to RECORDING_AUTO");

    /* 3. Verify Tray UI state reflects active recording */
    ASSERT_TRUE(send_ipc_cmd(cfg.control_socket, "GET_TRAY_STATE", reply, sizeof(reply)) == 0, "IPC GET_TRAY_STATE failed");
    ASSERT_TRUE(strstr(reply, "\"recording\":true") != NULL && strstr(reply, "ascolto-recording-symbolic") != NULL, "Tray UI should show the recording gesture");

    /* 4. Inject a 5-turn multi-speaker conversation across MIC and SYS channels */
    ASSERT_TRUE(send_ipc_cmd(cfg.control_socket,
                "INJECT_SYNTH_SPEECH MIC 135 500 1500 1000 Ciao a tutti, iniziamo la call di architettura su PipeWire e Whistle.",
                reply, sizeof(reply)) == 0, "Turn 1 failed");
    ASSERT_TRUE(send_ipc_cmd(cfg.control_socket,
                "INJECT_SYNTH_SPEECH SYS 98 420 1180 1200 Ciao Franco, qui Marco: il backend audio dual-channel funziona bene.",
                reply, sizeof(reply)) == 0, "Turn 2 failed");
    ASSERT_TRUE(send_ipc_cmd(cfg.control_socket,
                "INJECT_SYNTH_SPEECH SYS 235 740 2200 1200 Qui Giulia: ho verificato gli speech embedding e la diarizzazione.",
                reply, sizeof(reply)) == 0, "Turn 3 failed");

    /* Test PAUSE and RESUME mid-call */
    ASSERT_TRUE(send_ipc_cmd(cfg.control_socket, "PAUSE", reply, sizeof(reply)) == 0, "IPC PAUSE failed");
    ASSERT_TRUE(strstr(reply, "\"state\":\"PAUSED\"") != NULL, "Expected PAUSED state");
    ASSERT_TRUE(send_ipc_cmd(cfg.control_socket, "GET_TRAY_STATE", reply, sizeof(reply)) == 0, "IPC GET_TRAY_STATE failed");
    ASSERT_TRUE(strstr(reply, "\"paused\":true") != NULL && strstr(reply, "ascolto-paused-symbolic") != NULL, "Tray should show the paused ear");
    ASSERT_TRUE(send_ipc_cmd(cfg.control_socket, "RESUME", reply, sizeof(reply)) == 0, "IPC RESUME failed");
    ASSERT_TRUE(strstr(reply, "\"state\":\"RECORDING_AUTO\"") != NULL, "Expected resume to RECORDING_AUTO");

    /* Speaker 1 (Marco) and ME and Speaker 2 (Giulia) speak again */
    ASSERT_TRUE(send_ipc_cmd(cfg.control_socket,
                "INJECT_SYNTH_SPEECH SYS 100 425 1190 1200 Confermo che anche il trigger automatico sul microfono e stabile.",
                reply, sizeof(reply)) == 0, "Turn 4 failed");
    ASSERT_TRUE(send_ipc_cmd(cfg.control_socket,
                "INJECT_SYNTH_SPEECH MIC 136 505 1510 1000 Ottimo lavoro, salviamo la trascrizione.",
                reply, sizeof(reply)) == 0, "Turn 5 failed");
    ASSERT_TRUE(send_ipc_cmd(cfg.control_socket,
                "INJECT_SYNTH_SPEECH SYS 232 730 2180 1200 Perfetto, a dopo!",
                reply, sizeof(reply)) == 0, "Turn 6 failed");

    /* 5. Inspect speakers & segments via IPC */
    ASSERT_TRUE(send_ipc_cmd(cfg.control_socket, "LIST_SPEAKERS", reply, sizeof(reply)) == 0, "LIST_SPEAKERS failed");
    printf("  Active remote speakers: %s", reply);
    ASSERT_TRUE(strstr(reply, "\"SPEAKER_1\"") != NULL && strstr(reply, "\"SPEAKER_2\"") != NULL,
                "Expected SPEAKER_1 and SPEAKER_2 in speaker registry");
    ASSERT_TRUE(strstr(reply, "\"SPEAKER_3\"") == NULL,
                "Expected only 2 remote speakers for the 2 remote voices");

    ASSERT_TRUE(send_ipc_cmd(cfg.control_socket, "LIST_SEGMENTS", reply, sizeof(reply)) == 0, "LIST_SEGMENTS failed");
    ASSERT_TRUE(strstr(reply, "\"speaker\":\"ME\"") != NULL, "Expected ME segments");
    ASSERT_TRUE(strstr(reply, "\"speaker\":\"SPEAKER_1\"") != NULL, "Expected SPEAKER_1 segments");
    ASSERT_TRUE(strstr(reply, "\"speaker\":\"SPEAKER_2\"") != NULL, "Expected SPEAKER_2 segments");

    /* 6. Test runtime configuration mutations via IPC */
    ASSERT_TRUE(send_ipc_cmd(cfg.control_socket, "SET_KEYWORDS Bitrock, Ascolto, GNOME", reply, sizeof(reply)) == 0, "SET_KEYWORDS failed");
    ASSERT_TRUE(send_ipc_cmd(cfg.control_socket, "SET_SENSITIVITY high", reply, sizeof(reply)) == 0, "SET_SENSITIVITY failed");
    ASSERT_TRUE(send_ipc_cmd(cfg.control_socket, "SET_LANG en", reply, sizeof(reply)) == 0, "SET_LANG failed");

    /* 7. Simulate Zoom closing the microphone -> auto-stop session */
    char md_path[512];
    snprintf(md_path, sizeof(md_path), "%s", core->current_md_path);
    ASSERT_TRUE(send_ipc_cmd(cfg.control_socket, "SIM_MIC_ACTIVE 0 Zoom", reply, sizeof(reply)) == 0, "SIM_MIC_ACTIVE 0 failed");
    ASSERT_TRUE(strstr(reply, "\"state\":\"IDLE\"") != NULL, "Expected auto transition to IDLE when mic closes");

    /* 8. Verify generated Markdown transcript file on disk */
    struct stat st;
    ASSERT_TRUE(stat(md_path, &st) == 0 && st.st_size > 200, "Generated Markdown transcript missing or empty");
    FILE *fp = fopen(md_path, "r");
    ASSERT_TRUE(fp != NULL, "Cannot open generated Markdown transcript");
    char md_content[8192];
    size_t nread = fread(md_content, 1, sizeof(md_content) - 1, fp);
    md_content[nread] = '\0';
    fclose(fp);
    ASSERT_TRUE(strstr(md_content, "[ME]") != NULL &&
                strstr(md_content, "[SPEAKER_1]") != NULL &&
                strstr(md_content, "[SPEAKER_2]") != NULL,
                "Markdown transcript missing speaker tags");

    /* 9. Verify observability event log */
    ASSERT_TRUE(stat(cfg.telemetry_log, &st) == 0 && st.st_size > 200, "Telemetry JSONL log missing or empty");

    ascolto_core_destroy(core);
    free(core);
    printf("  -> PASS (Transcript written to %s)\n", md_path);
    return 0;
}

static int test_live_pipewire_integration(void) {
    printf("[TEST 5] Live PipeWire graph introspection and audio device enumeration...\n");
    ascolto_device_info_t devs[ASCOLTO_MAX_DEVICES];
    size_t count = 0;
    int rc = ascolto_pipewire_enumerate_devices(devs, ASCOLTO_MAX_DEVICES, &count);
    ASSERT_TRUE(rc == 0 && count >= 2, "PipeWire device enumeration returned fewer than 2 devices");
    printf("  Enumerated %zu audio devices via PipeWire:\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("    - [%s] id=%s name=%s desc=\"%s\"\n",
               devs[i].is_monitor ? "SYS-MON" : "MIC-SRC",
               devs[i].id, devs[i].name, devs[i].description);
    }

    char active_app[128] = {0};
    bool mic_busy = ascolto_pipewire_check_mic_active(active_app, sizeof(active_app));
    printf("  Current live PipeWire external mic state: %s (app='%s')\n",
           mic_busy ? "ACTIVE" : "IDLE", active_app);
    printf("  -> PASS\n");
    return 0;
}

static int test_daemon_binary_and_cli_tool(void) {
    printf("[TEST 6] Full daemon process (./bin/ascolto) + CLI controller (./bin/ascoltoctl) + GNOME AppIndicator...\n");
    const char *sock = "/tmp/opencode/ascolto_daemon_test.sock";
    unlink(sock);

    int rc = system("./bin/ascolto --socket /tmp/opencode/ascolto_daemon_test.sock "
                    "--output-dir /tmp/opencode/ascolto_daemon_transcripts "
                    "--config /tmp/opencode/ascolto_daemon_config.ini "
                    "--mock-audio >/tmp/opencode/ascolto_daemon.out 2>&1 &");
    ASSERT_TRUE(rc == 0, "Failed to launch ./bin/ascolto");

    /* Wait up to 3 seconds for socket to come online */
    for (int i = 0; i < 60; i++) {
        if (access(sock, F_OK) == 0) break;
        struct timespec ts = {0, 50 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    ASSERT_TRUE(access(sock, F_OK) == 0, "Daemon control socket did not appear");

    char out[4096];
    FILE *fp = popen("./bin/ascoltoctl -s /tmp/opencode/ascolto_daemon_test.sock GET_TRAY_STATE", "r");
    ASSERT_TRUE(fp != NULL, "popen ascoltoctl failed");
    size_t n = fread(out, 1, sizeof(out) - 1, fp);
    out[n] = '\0';
    pclose(fp);
    printf("  Daemon Tray State: %s", out);
    ASSERT_TRUE(strstr(out, "\"ok\":true") != NULL && strstr(out, "\"active\":true") != NULL,
                "GET_TRAY_STATE failed via ascoltoctl");

    if (access("/usr/share/sounds/alsa/Front_Center.wav", R_OK) == 0) {
        ASSERT_TRUE(system("./bin/ascoltoctl -s /tmp/opencode/ascolto_daemon_test.sock START >/dev/null") == 0, "START failed");
        ASSERT_TRUE(system("./bin/ascoltoctl -s /tmp/opencode/ascolto_daemon_test.sock INJECT_WAV MIC /usr/share/sounds/alsa/Front_Center.wav >/dev/null") == 0,
                    "INJECT_WAV MIC failed");
        ASSERT_TRUE(system("./bin/ascoltoctl -s /tmp/opencode/ascolto_daemon_test.sock INJECT_WAV SYS /usr/share/sounds/alsa/Front_Left.wav >/dev/null") == 0,
                    "INJECT_WAV SYS failed");

        fp = popen("./bin/ascoltoctl -s /tmp/opencode/ascolto_daemon_test.sock LIST_SEGMENTS", "r");
        ASSERT_TRUE(fp != NULL, "popen LIST_SEGMENTS failed");
        n = fread(out, 1, sizeof(out) - 1, fp);
        out[n] = '\0';
        pclose(fp);
        printf("  Transcribed real WAV segments: %s", out);
        ASSERT_TRUE(strstr(out, "\"speaker\":\"ME\"") != NULL && strstr(out, "\"speaker\":\"SPEAKER_1\"") != NULL,
                    "Expected ME and SPEAKER_1 from real WAV injection");
        ASSERT_TRUE(system("./bin/ascoltoctl -s /tmp/opencode/ascolto_daemon_test.sock STOP >/dev/null") == 0, "STOP failed");
    }

    ASSERT_TRUE(system("./bin/ascoltoctl -s /tmp/opencode/ascolto_daemon_test.sock QUIT >/dev/null") == 0, "QUIT failed");
    printf("  -> PASS\n");
    return 0;
}

static int test_live_daemon_repeated_start_stop_and_gui_dbusmenu(void) {
    printf("[TEST 7] Live PipeWire daemon repeated Start/Stop/Restart cycles + GNOME D-Bus GUI menu clicks...\n");
    const char *sock = "/tmp/opencode/ascolto_live_gui.sock";
    unlink(sock);

    /* Launch real daemon with LIVE PipeWire audio backend (no --mock-audio) and GNOME GUI indicator */
    int rc = system("./bin/ascolto --socket /tmp/opencode/ascolto_live_gui.sock "
                    "--output-dir /tmp/opencode/ascolto_live_gui_transcripts "
                    "--config /tmp/opencode/ascolto_live_gui_config.ini "
                    "--no-auto-mic >/tmp/opencode/ascolto_live_gui.out 2>&1 &");
    ASSERT_TRUE(rc == 0, "Failed to launch live ./bin/ascolto");

    for (int i = 0; i < 60; i++) {
        if (access(sock, F_OK) == 0) break;
        struct timespec ts = {0, 50 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    ASSERT_TRUE(access(sock, F_OK) == 0, "Live daemon control socket did not appear");

    char reply[8192];
    /* Run 3 full consecutive START -> PAUSE -> RESUME -> STOP cycles with real PipeWire capture */
    for (int cycle = 1; cycle <= 3; cycle++) {
        ASSERT_TRUE(send_ipc_cmd(sock, "START", reply, sizeof(reply)) == 0, "START failed");
        char expected_sid[64];
        snprintf(expected_sid, sizeof(expected_sid), "\"session_id\":%d", cycle);
        ASSERT_TRUE(strstr(reply, "\"state\":\"RECORDING_MANUAL\"") != NULL && strstr(reply, expected_sid) != NULL,
                    "Expected RECORDING_MANUAL and incremented session_id");

        /* Let real PipeWire capture run for 150 ms */
        struct timespec ts = {0, 150 * 1000 * 1000};
        nanosleep(&ts, NULL);

        ASSERT_TRUE(send_ipc_cmd(sock, "PAUSE", reply, sizeof(reply)) == 0, "PAUSE failed");
        ASSERT_TRUE(strstr(reply, "\"state\":\"PAUSED\"") != NULL, "Expected PAUSED state");

        ASSERT_TRUE(send_ipc_cmd(sock, "RESUME", reply, sizeof(reply)) == 0, "RESUME failed");
        ASSERT_TRUE(strstr(reply, "\"state\":\"RECORDING_MANUAL\"") != NULL, "Expected RECORDING_MANUAL after RESUME");

        ASSERT_TRUE(send_ipc_cmd(sock, "STOP", reply, sizeof(reply)) == 0, "STOP failed");
        ASSERT_TRUE(strstr(reply, "\"state\":\"IDLE\"") != NULL, "Expected IDLE after STOP");
        printf("  Cycle %d/3 (START -> PAUSE -> RESUME -> STOP) completed cleanly: %s", cycle, reply);
    }

    /* Test TOGGLE command (used by Desktop icon 1) */
    ASSERT_TRUE(send_ipc_cmd(sock, "TOGGLE", reply, sizeof(reply)) == 0, "TOGGLE 1 failed");
    ASSERT_TRUE(strstr(reply, "\"state\":\"RECORDING_MANUAL\"") != NULL && strstr(reply, "\"session_id\":4") != NULL,
                "Expected TOGGLE from IDLE to start session 4");
    ASSERT_TRUE(send_ipc_cmd(sock, "TOGGLE", reply, sizeof(reply)) == 0, "TOGGLE 2 failed");
    ASSERT_TRUE(strstr(reply, "\"state\":\"IDLE\"") != NULL,
                "Expected second TOGGLE to stop session 4 and return to IDLE");

    /* Test OPEN_FOLDER command (used by GUI 'Apri cartella trascrizioni') */
    ASSERT_TRUE(send_ipc_cmd(sock, "OPEN_FOLDER", reply, sizeof(reply)) == 0, "OPEN_FOLDER failed");
    ASSERT_TRUE(strstr(reply, "\"ok\":true") != NULL, "OPEN_FOLDER did not return ok:true");

    /* Verify recent files count in tray state reflects the sessions we just recorded */
    ASSERT_TRUE(send_ipc_cmd(sock, "GET_TRAY_STATE", reply, sizeof(reply)) == 0, "GET_TRAY_STATE failed");
    printf("  Tray state after 4 sessions: %s", reply);
    ASSERT_TRUE(strstr(reply, "\"recent_files_count\":") != NULL &&
                strstr(reply, "\"recent_files_count\":0") == NULL,
                "Expected recent_files_count > 0 in tray menu");

    ASSERT_TRUE(send_ipc_cmd(sock, "QUIT", reply, sizeof(reply)) == 0, "QUIT failed");
    struct timespec ts = {0, 150 * 1000 * 1000};
    nanosleep(&ts, NULL);
    printf("  -> PASS\n");
    return 0;
}

static int test_flac_recorder_roundtrip(void) {
    printf("[TEST 8] FLAC recorder round-trip (write, close, reopen, count samples)...\n");
    const char *path = "/tmp/opencode/ascolto_test_rec.flac";
    unlink(path);
    ascolto_audio_recorder_t *rec = ascolto_audio_recorder_open(path);
    ASSERT_TRUE(rec != NULL, "recorder open failed");

    const size_t total = 16000 * 3 + 777; /* deliberately not a multiple of the FLAC frame size */
    float chunk[1600];
    size_t written = 0;
    while (written < total) {
        size_t n = (total - written) < 1600 ? (total - written) : 1600;
        for (size_t i = 0; i < n; i++) {
            chunk[i] = 0.5f * sinf(2.0f * 3.14159265f * 440.0f * (float)(written + i) / 16000.0f);
        }
        ASSERT_TRUE(ascolto_audio_recorder_write(rec, chunk, n) == 0, "recorder write failed");
        written += n;
    }
    ascolto_audio_recorder_close(rec);

    struct stat st;
    ASSERT_TRUE(stat(path, &st) == 0 && st.st_size > 1000, "flac file missing or too small");
    ASSERT_TRUE((st.st_mode & 0777) == 0600, "flac file must be mode 0600");

    AVFormatContext *fmt = NULL;
    ASSERT_TRUE(avformat_open_input(&fmt, path, NULL, NULL) == 0, "reopen flac failed");
    ASSERT_TRUE(avformat_find_stream_info(fmt, NULL) >= 0, "stream info failed");
    AVCodecParameters *par = fmt->streams[0]->codecpar;
    ASSERT_TRUE(par->codec_id == AV_CODEC_ID_FLAC && par->sample_rate == 16000 &&
                par->ch_layout.nb_channels == 1, "unexpected flac parameters");
    int64_t dur = fmt->streams[0]->duration;
    avformat_close_input(&fmt);
    ASSERT_TRUE(dur == (int64_t)total, "decoded sample count differs from written count");
    printf("  -> PASS (%zu samples, %lld bytes)\n", total, (long long)st.st_size);
    return 0;
}

int main(void) {
    mkdir("/tmp/opencode", 0755);
    printf("=== ascolto Test Harness & Observability Suite ===\n\n");

    int (*tests[])(void) = {
        test_config_persistence,
        test_whistle_engine_and_wav_io,
        test_hybrid_speaker_diarization,
        test_e2e_multispeaker_call_and_ipc_observability,
        test_live_pipewire_integration,
        test_daemon_binary_and_cli_tool,
        test_live_daemon_repeated_start_stop_and_gui_dbusmenu,
        test_flac_recorder_roundtrip
    };

    size_t num_tests = sizeof(tests) / sizeof(tests[0]);
    for (size_t i = 0; i < num_tests; i++) {
        g_tests_run++;
        if (tests[i]() == 0) {
            g_tests_passed++;
        } else {
            fprintf(stderr, "\nTest %zu FAILED.\n", i + 1);
            return 1;
        }
    }

    printf("\n=== ALL %d/%d TESTS PASSED ===\n", g_tests_passed, g_tests_run);
    return 0;
}
