/* Persistent native Voxtral benchmark. All input is supplied as float32 PCM. */
#include "../bench_common.h"
#include "voxtral.h"
#include "voxtral_metal.h"

extern int vox_verbose;
extern size_t bench_metal_allocated_bytes(void);

static int collect(vox_stream_t *s, char **text, size_t *used, size_t *capacity,
                   double start, double *first_observed_ms) {
    const char *pieces[64];
    int n, count = 0;
    while ((n = vox_stream_get(s, pieces, 64)) > 0) {
        if (*first_observed_ms < 0) *first_observed_ms = bench_now() - start;
        for (int i = 0; i < n; ++i) {
            size_t bytes = strlen(pieces[i]);
            if (*used + bytes + 1 > *capacity) {
                size_t next = (*used + bytes + 1) * 2;
                char *grown = realloc(*text, next);
                if (!grown) return -1;
                *text = grown;
                *capacity = next;
            }
            memcpy(*text + *used, pieces[i], bytes);
            *used += bytes;
            (*text)[*used] = 0;
        }
        count += n;
    }
    return count;
}

int main(int argc, char **argv) {
    if (argc < 5 || argc > 6) {
        fprintf(stderr, "Usage: %s MODEL_DIR MANIFEST REPEATS SIDECAR [CHUNK_SAMPLES]\n", argv[0]);
        return 2;
    }
    int repeats = atoi(argv[3]);
    int chunk = argc == 6 ? atoi(argv[5]) : 0;
    if (repeats < 1 || chunk < 0) return 2;
    FILE *side = fopen(argv[4], "w");
    if (!side) return 2;
    bench_clip clips[128];
    int count = bench_clips(argv[2], clips, 128);
    if (count <= 0) return 2;

    double load_start = bench_now(), load_cpu_start = bench_cpu();
    vox_verbose = 1;
    if (!vox_metal_init() || !vox_metal_available()) {
        fprintf(stderr, "Metal initialization failed; CPU fallback is not benchmarked.\n");
        return 3;
    }
    vox_ctx_t *ctx = vox_load(argv[1]);
    double load_ms = bench_now() - load_start;
    double load_cpu_ms = bench_cpu() - load_cpu_start;
    if (!ctx) return 3;
    fprintf(side, "{\"event\":\"load\",\"load_ms\":%.3f,\"load_cpu_ms\":%.3f,"
            "\"gpu_allocated_bytes\":%zu,\"gpu_weight_cache_bytes\":%zu,\"chunk_samples\":%d}\n",
            load_ms, load_cpu_ms, bench_metal_allocated_bytes(), vox_metal_memory_used(), chunk);
    fflush(side);

    int failures = 0;
    for (int repeat = -1; repeat < repeats; ++repeat) {
        int limit = repeat < 0 ? 1 : count;
        for (int i = 0; i < limit; ++i) {
            bench_clip *clip = &clips[i];
            double start = bench_now(), cpu_start = bench_cpu();
            double first_observed_ms = -1, final_result_ms = -1;
            size_t capacity = 1024, used = 0;
            char *text = calloc(capacity, 1);
            int error = text ? 0 : 1, tokens = 0;
            vox_stream_t *stream = error ? NULL : vox_stream_init(ctx);
            if (!stream) error = 1;
            if (stream) {
                if (chunk) vox_set_processing_interval(stream, chunk / 16000.0f);
                for (int offset = 0; !error && offset < clip->n;) {
                    int n = chunk ? chunk : clip->n;
                    if (n > clip->n - offset) n = clip->n - offset;
                    if (vox_stream_feed(stream, clip->pcm + offset, n) != 0) error = 1;
                    offset += n;
                    int got = collect(stream, &text, &used, &capacity, start, &first_observed_ms);
                    if (got < 0) error = 1;
                    else tokens += got;
                }
                if (!error && vox_stream_finish(stream) != 0) error = 1;
                int got = collect(stream, &text, &used, &capacity, start, &first_observed_ms);
                if (got < 0) error = 1;
                else tokens += got;
                final_result_ms = bench_now() - start;
            }
            size_t gpu_bytes = bench_metal_allocated_bytes();
            size_t gpu_weight_bytes = vox_metal_memory_used();
            if (stream) vox_stream_free(stream);
            double total_ms = bench_now() - start, cpu_ms = bench_cpu() - cpu_start;
            bench_emit("voxtral-mini-4b-realtime", "metal", clip, repeat, load_ms,
                       total_ms, -1, cpu_ms, "unavailable_no_token_callback",
                       text ? text : "", tokens, error);
            fprintf(side, "{\"event\":\"inference\",\"clip\":\"%s\",\"repeat\":%d,"
                    "\"first_observed_result_ms\":", clip->id, repeat);
            if (first_observed_ms >= 0) fprintf(side, "%.3f", first_observed_ms);
            else fputs("null", side);
            fprintf(side, ",\"first_observed_kind\":\"%s\",\"final_result_ms\":%.3f,"
                    "\"gpu_allocated_bytes\":%zu,\"gpu_weight_cache_bytes\":%zu,\"chunk_samples\":%d}\n",
                    chunk ? "incremental_text_poll" : "batch_token_queue_poll",
                    final_result_ms, gpu_bytes, gpu_weight_bytes, chunk);
            fflush(side);
            failures += error;
            free(text);
        }
    }
    vox_free(ctx);
    vox_metal_shutdown();
    for (int i = 0; i < count; ++i) free(clips[i].pcm);
    fclose(side);
    return failures ? 4 : 0;
}
