/* Persistent, completed-clip Qwen3-ASR benchmark. Inference stays in native C. */
#include "bench_common.h"
#include "qwen_asr.h"
#include "qwen_asr_kernels.h"

typedef struct {
    double started_ms;
    double first_ms;
} token_clock;

static void on_token(const char *piece, void *userdata) {
    token_clock *clock = userdata;
    if (clock->first_ms < 0 && piece && piece[0])
        clock->first_ms = bench_now() - clock->started_ms;
}

static int transcribe(qwen_ctx_t *ctx, const char *model, const bench_clip *clip,
                      int repeat, double load_ms) {
    token_clock clock = {.first_ms = -1};
    qwen_set_token_callback(ctx, on_token, &clock);
    const double cpu_start = bench_cpu();
    clock.started_ms = bench_now();
    char *text = qwen_transcribe_audio(ctx, clip->pcm, clip->n);
    const double total_ms = bench_now() - clock.started_ms;
    const double cpu_ms = bench_cpu() - cpu_start;
    const int error = text ? 0 : 1;
    bench_emit(model, "CPU Accelerate/NEON", clip, repeat, load_ms, total_ms,
               clock.first_ms, cpu_ms, "first nonempty output token callback",
               text, ctx->perf_text_tokens, error);
    fprintf(stderr,
            "qwen-bench: model=%s clip=%s repeat=%d wall_ms=%.3f "
            "encode_ms=%.3f decode_ms=%.3f tokens=%d error=%d\n",
            model, clip->id, repeat, total_ms, ctx->perf_encode_ms,
            ctx->perf_decode_ms, ctx->perf_text_tokens, error);
    free(text);
    return error;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "Usage: %s MODEL_ID MODEL_DIRECTORY MANIFEST_TSV\n", argv[0]);
        return 2;
    }
    bench_clip clips[256];
    const int count = bench_clips(argv[3], clips, 256);
    if (count < 1) {
        fprintf(stderr, "qwen-bench: cannot load nonempty manifest %s\n", argv[3]);
        return 2;
    }
    qwen_verbose = 1;
    qwen_set_threads(8);
    fprintf(stderr, "qwen-bench: CPU Accelerate/NEON, threads=8, language=Italian, "
                    "segmentation=off, silence_removal=off, past_text=off\n");
    const double load_start = bench_now();
    qwen_ctx_t *ctx = qwen_load(argv[2]);
    const double load_ms = bench_now() - load_start;
    if (!ctx) {
        fprintf(stderr, "qwen-bench: model load failed\n");
        return 1;
    }
    ctx->segment_sec = 0;
    ctx->skip_silence = 0;
    ctx->past_text_conditioning = 0;
    if (qwen_set_force_language(ctx, "Italian") != 0) {
        fprintf(stderr, "qwen-bench: cannot force Italian\n");
        qwen_free(ctx);
        return 1;
    }
    int errors = transcribe(ctx, argv[1], &clips[0], -1, load_ms);
    for (int repeat = 0; repeat < 3; repeat++)
        for (int i = 0; i < count; i++)
            errors += transcribe(ctx, argv[1], &clips[i], repeat, load_ms);
    qwen_free(ctx);
    qwen_set_threads(1);
    for (int i = 0; i < count; i++) free(clips[i].pcm);
    return errors ? 1 : 0;
}
