/* Persistent PCM benchmark through parakeet.cpp's public C ABI. */
#include "../bench_common.h"
#include "parakeet_capi.h"

/* The upstream C ABI omits the global backend shutdown used by its CLI. */
void bench_parakeet_shutdown(void);

static int decode(parakeet_ctx *ctx, const char *model, const char *backend,
                  const bench_clip *clip, int repeat, double load_ms) {
    double cpu = bench_cpu(), start = bench_now();
    char *result = parakeet_capi_transcribe_pcm(ctx, clip->pcm, clip->n, 16000, 0);
    double elapsed = bench_now() - start, used_cpu = bench_cpu() - cpu;
    int error = result == NULL;
    if (error) fprintf(stderr, "decode %s: %s\n", clip->id,
                       parakeet_capi_last_error(ctx));
    /* The PCM API returns one final string and exposes no token callback. */
    bench_emit(model, backend, clip, repeat, load_ms, elapsed, -1, used_cpu,
               "unavailable", result ? result : "", -1, error);
    parakeet_capi_free_string(result);
    return error;
}

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s MODEL_ID BACKEND MODEL_GGUF MANIFEST_TSV\n", argv[0]);
        return 2;
    }
    bench_clip clips[256];
    int count = bench_clips(argv[4], clips, 256);
    if (count <= 0) { fprintf(stderr, "cannot read corpus\n"); return 2; }
    double cpu = bench_cpu(), start = bench_now();
    parakeet_ctx *ctx = parakeet_capi_load(argv[3]);
    double load_ms = bench_now() - start, load_cpu_ms = bench_cpu() - cpu;
    fprintf(stderr, "BENCH_LOAD_JSON {\"wall_ms\":%.3f,\"cpu_ms\":%.3f,\"peak_rss_mb\":%.3f}\n",
            load_ms, load_cpu_ms, bench_rss());
    if (!ctx) {
        fprintf(stderr, "load: %s\n", parakeet_capi_load_error());
        bench_parakeet_shutdown();
        return 1;
    }
    int errors = decode(ctx, argv[1], argv[2], &clips[0], -1, load_ms);
    for (int repeat = 0; repeat < 3; repeat++)
        for (int i = 0; i < count; i++)
            errors += decode(ctx, argv[1], argv[2], &clips[i], repeat, load_ms);
    parakeet_capi_free(ctx);
    bench_parakeet_shutdown();
    for (int i = 0; i < count; i++) free(clips[i].pcm);
    return errors ? 1 : 0;
}
