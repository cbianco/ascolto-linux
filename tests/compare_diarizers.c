#ifdef ASCOLTO_COMPARE_MACOS
#include "ascolto/offline.h"
#else
#include "ascolto/diarizer.h"
#include "ascolto/whistle_engine.h"
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "Usage: %s <model> <fixture-directory>\n", argv[0]); return 1; }
    const char *names[] = {"alice_turn1.wav", "rocko_turn1.wav", "alice_turn2.wav", "rocko_turn2.wav", "spk3_turn1.wav"};
    const char *expected[] = {"SPEAKER_1", "SPEAKER_2", "SPEAKER_1", "SPEAKER_2", "SPEAKER_3"};
#ifdef ASCOLTO_COMPARE_MACOS
    ascolto_whistle_t engine;
    ascolto_diarizer_t diarizer;
    ascolto_diarizer_init(&diarizer, 0.60f);
    const char *algorithm = "macos-16d";
#else
    ascolto_whistle_engine_t engine;
    ascolto_diarizer_t diarizer;
    ascolto_diarizer_init(&diarizer, ASCOLTO_SENSITIVITY_MEDIUM);
    const char *algorithm = "linux-128d";
#endif
    if (ascolto_whistle_init(&engine, argv[1])) return 1;
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", argv[2], names[i]);
        float similarity;
#ifdef ASCOLTO_COMPARE_MACOS
        ascolto_audio_buffer_t audio;
        if (ascolto_wav_load(path, &audio)) return 1;
        float *frames = NULL;
        int count = 0;
        if (ascolto_whistle_embed_frames(&engine, audio.samples, audio.num_samples, &frames, &count) < 0) return 1;
        ascolto_voiceprint_t voice;
        ascolto_extract_voiceprint(audio.samples, audio.num_samples, frames, count, &voice);
        bool is_new;
        const char *label = ascolto_diarizer_identify(&diarizer, ASCOLTO_CHANNEL_SYS, &voice, &similarity, &is_new);
        free(frames);
        ascolto_audio_free(&audio);
#else
        float *audio = NULL;
        size_t count;
        if (ascolto_wav_load_16k_mono(path, &audio, &count)) return 1;
        ascolto_whistle_result_t result;
        if (ascolto_whistle_process_segment(&engine, audio, count, "it", NULL, NULL, &result)) return 1;
        float embedding[ASCOLTO_EMBED_DIM], pitch, db;
        ascolto_diarizer_compute_embedding(audio, count, result.frame_embeddings, result.frame_embedding_floats,
                                           result.frame_embedding_dim, embedding, &pitch, &db);
        char label[32];
        ascolto_diarizer_assign(&diarizer, ASCOLTO_CHANNEL_SYS, embedding, pitch, i, label, &similarity);
        ascolto_whistle_result_free(&result);
        free(audio);
#endif
        printf("{\"algorithm\":\"%s\",\"fixture\":\"%s\",\"expected\":\"%s\",\"speaker\":\"%s\",\"match\":%s,\"similarity\":%.4f}\n",
               algorithm, names[i], expected[i], label, strcmp(expected[i], label) ? "false" : "true", similarity);
    }
#ifdef ASCOLTO_COMPARE_MACOS
    ascolto_whistle_free(&engine);
#else
    ascolto_whistle_destroy(&engine);
#endif
    return 0;
}
