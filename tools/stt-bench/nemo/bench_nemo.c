/* Persistent utterance benchmark through NeMo-Speech.cpp's public C ABI. */
#include "../bench_common.h"
#include "nemo_speech/asr.h"

static int drain(nemo_speech_asr_stream *stream, double start, double *first_ms,
                 const char **first_kind, char **last) {
    nemo_speech_asr_result *result = NULL;
    int error;
    while (!(error = nemo_speech_asr_stream_next(stream, &result)) && result) {
        const char *text = nemo_speech_asr_result_transcript(result, 0);
        if (text && *text && *first_ms < 0) {
            *first_ms = bench_now() - start;
            *first_kind = nemo_speech_asr_result_is_final(result) ? "final_result" : "incremental_text";
        }
        if (text) {
            size_t size = strlen(text) + 1;
            char *copy = malloc(size);
            if (!copy) { nemo_speech_asr_result_destroy(result); return 2; }
            memcpy(copy, text, size); free(*last); *last = copy;
        }
        nemo_speech_asr_result_destroy(result); result = NULL;
    }
    return error;
}

static int decode(nemo_speech_asr_recognizer *recognizer,
                  const nemo_speech_asr_recognition_options *options,
                  const char *model, const char *backend, const bench_clip *clip,
                  int repeat, double load_ms, int streaming) {
    nemo_speech_asr_result *result = NULL;
    nemo_speech_asr_stream *stream = NULL;
    char *stream_text = NULL;
    double first_ms = -1;
    const char *first_kind = "unavailable";
    double cpu = bench_cpu(), start = bench_now();
    int error;
    if (streaming) {
        error = nemo_speech_asr_streaming_recognize(recognizer, options, &stream);
        /* Feed 20 ms blocks without wall-clock pacing; this measures compute. */
        for (int offset = 0; !error && offset < clip->n; offset += 320) {
            int n = clip->n - offset < 320 ? clip->n - offset : 320;
            error = nemo_speech_asr_stream_push_f32(stream, clip->pcm + offset, n, 16000);
            if (!error) error = drain(stream, start, &first_ms, &first_kind, &stream_text);
        }
        if (!error) error = nemo_speech_asr_stream_finish(stream);
        if (!error) error = drain(stream, start, &first_ms, &first_kind, &stream_text);
        nemo_speech_asr_stream_close(stream);
    } else {
        error = nemo_speech_asr_recognize_f32(recognizer, options, clip->pcm,
                                             clip->n, 16000, &result);
    }
    double elapsed = bench_now() - start, used_cpu = bench_cpu() - cpu;
    if (error) fprintf(stderr, "decode %s: %s\n", clip->id, nemo_speech_asr_last_error());
    const char *transcript = streaming ? stream_text :
        (result ? nemo_speech_asr_result_transcript(result, 0) : "");
    /* recognize_f32 exposes no token callback; returned text is final-only. */
    bench_emit(model, backend, clip, repeat, load_ms, elapsed, first_ms, used_cpu,
               first_kind, transcript, -1, error);
    nemo_speech_asr_result_destroy(result);
    free(stream_text);
    return error != 0;
}

int main(int argc, char **argv) {
    if (argc != 5 && !(argc == 6 && !strcmp(argv[5], "--streaming"))) {
        fprintf(stderr, "usage: %s MODEL_ID BACKEND MODEL_GGUF MANIFEST_TSV [--streaming]\n", argv[0]);
        return 2;
    }
    int streaming = argc == 6;
    bench_clip clips[256];
    int count = bench_clips(argv[4], clips, 256);
    if (count <= 0) { fprintf(stderr, "cannot read corpus\n"); return 2; }
    nemo_speech_asr_backend_config backend = {0};
    backend.size = sizeof(backend); backend.gpu = !strcmp(argv[2], "cpu") ? -1 : 0;
    nemo_speech_asr_model_config model = {0};
    model.size = sizeof(model); model.path = argv[3];
    nemo_speech_asr_streaming_config stream_config = {0};
    stream_config.size = sizeof(stream_config);
    stream_config.chunk_size = 0.16f;
    stream_config.ctc_left_padding = stream_config.ctc_right_padding = 1.92f;
    stream_config.rnnt_right_context = 3;
    nemo_speech_asr_recognizer_config config = {0};
    config.size = sizeof(config); config.backend = &backend; config.model = &model;
    config.streaming = &stream_config;
    nemo_speech_asr_recognizer *recognizer = NULL;
    double cpu = bench_cpu(), start = bench_now();
    int error = nemo_speech_asr_create(&config, &recognizer);
    double load_ms = bench_now() - start, load_cpu_ms = bench_cpu() - cpu;
    fprintf(stderr, "BENCH_LOAD_JSON {\"wall_ms\":%.3f,\"cpu_ms\":%.3f,\"peak_rss_mb\":%.3f}\n",
            load_ms, load_cpu_ms, bench_rss());
    if (error) { fprintf(stderr, "load: %s\n", nemo_speech_asr_last_error()); return 1; }
    nemo_speech_asr_recognition_options options = nemo_speech_asr_recognition_options_default();
    options.language_code = "it-IT";
    options.enable_word_time_offsets = false;
    options.interim_results = streaming;
    int errors = decode(recognizer, &options, argv[1], argv[2], &clips[0], -1, load_ms, streaming);
    for (int repeat = 0; repeat < 3; repeat++)
        for (int i = 0; i < count; i++)
            errors += decode(recognizer, &options, argv[1], argv[2], &clips[i], repeat, load_ms, streaming);
    nemo_speech_asr_destroy(recognizer);
    for (int i = 0; i < count; i++) free(clips[i].pcm);
    return errors ? 1 : 0;
}
