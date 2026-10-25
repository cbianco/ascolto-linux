/* Persistent C API benchmark; sherpa-onnx and ONNX Runtime have C++ cores. */
#include "../bench_common.h"
#include "c-api.h"

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        printf("{\"version\":\"%s\",\"git_commit\":\"%s\",\"git_date\":\"%s\"}\n",
               SherpaOnnxGetVersionStr(), SherpaOnnxGetGitSha1(), SherpaOnnxGetGitDate());
        return 0;
    }
    if (argc != 6) {
        fprintf(stderr, "Usage: %s MODEL_DIR MANIFEST REPEATS SIDECAR THREADS\n", argv[0]);
        return 2;
    }
    int repeats = atoi(argv[3]), threads = atoi(argv[5]);
    if (repeats < 1 || threads < 1) return 2;
    FILE *side = fopen(argv[4], "w");
    if (!side) return 2;
    bench_clip clips[128];
    int count = bench_clips(argv[2], clips, 128);
    if (count <= 0) return 2;
    char encoder[2048], decoder[2048], tokens[2048];
    snprintf(encoder, sizeof(encoder), "%s/encoder.int8.onnx", argv[1]);
    snprintf(decoder, sizeof(decoder), "%s/decoder.int8.onnx", argv[1]);
    snprintf(tokens, sizeof(tokens), "%s/tokens.txt", argv[1]);
    SherpaOnnxOfflineRecognizerConfig config = {0};
    config.feat_config.sample_rate = 16000;
    config.feat_config.feature_dim = 80;
    config.model_config.cohere_transcribe.encoder = encoder;
    config.model_config.cohere_transcribe.decoder = decoder;
    config.model_config.cohere_transcribe.language = "it";
    config.model_config.cohere_transcribe.use_punct = 1;
    config.model_config.cohere_transcribe.use_itn = 1;
    config.model_config.tokens = tokens;
    config.model_config.num_threads = threads;
    config.model_config.provider = "cpu";
    config.model_config.debug = 1;
    config.decoding_method = "greedy_search";
    fprintf(stderr, "sherpa-onnx %s commit %s\n", SherpaOnnxGetVersionStr(), SherpaOnnxGetGitSha1());
    double load_start = bench_now(), load_cpu_start = bench_cpu();
    const SherpaOnnxOfflineRecognizer *recognizer = SherpaOnnxCreateOfflineRecognizer(&config);
    double load_ms = bench_now() - load_start, load_cpu_ms = bench_cpu() - load_cpu_start;
    if (!recognizer) return 3;
    fprintf(side, "{\"event\":\"load\",\"load_ms\":%.3f,\"load_cpu_ms\":%.3f,"
            "\"gpu_allocated_bytes\":null,\"threads\":%d}\n", load_ms, load_cpu_ms, threads);
    fflush(side);

    int failures = 0;
    for (int repeat = -1; repeat < repeats; ++repeat) {
        int limit = repeat < 0 ? 1 : count;
        for (int i = 0; i < limit; ++i) {
            bench_clip *clip = &clips[i];
            double start = bench_now(), cpu_start = bench_cpu();
            double final_result_ms = -1;
            const SherpaOnnxOfflineStream *stream = SherpaOnnxCreateOfflineStream(recognizer);
            const SherpaOnnxOfflineRecognizerResult *result = NULL;
            if (stream) {
                SherpaOnnxAcceptWaveformOffline(stream, 16000, clip->pcm, clip->n);
                SherpaOnnxDecodeOfflineStream(recognizer, stream);
                result = SherpaOnnxGetOfflineStreamResult(stream);
                final_result_ms = bench_now() - start;
            }
            int error = !result || !result->text;
            int token_count = result ? result->count : 0;
            char *text = strdup(result && result->text ? result->text : "");
            if (!text) error = 1;
            if (result) SherpaOnnxDestroyOfflineRecognizerResult(result);
            if (stream) SherpaOnnxDestroyOfflineStream(stream);
            double total_ms = bench_now() - start, cpu_ms = bench_cpu() - cpu_start;
            bench_emit("cohere-transcribe-2b-int8", "onnxruntime-cpu", clip, repeat,
                       load_ms, total_ms, -1, cpu_ms, "final_result_only",
                       text ? text : "", token_count, error);
            fprintf(side, "{\"event\":\"inference\",\"clip\":\"%s\",\"repeat\":%d,"
                    "\"first_observed_result_ms\":%.3f,\"first_observed_kind\":\"final_result\","
                    "\"final_result_ms\":%.3f,\"gpu_allocated_bytes\":null}\n",
                    clip->id, repeat, final_result_ms, final_result_ms);
            fflush(side);
            failures += error;
            free(text);
        }
    }
    SherpaOnnxDestroyOfflineRecognizer(recognizer);
    for (int i = 0; i < count; ++i) free(clips[i].pcm);
    fclose(side);
    return failures ? 4 : 0;
}
