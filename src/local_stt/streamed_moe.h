#pragma once
#include "ggml.h"
#include "ggml-backend.h"
#include "expert_cache.h"
#include <cstdint>

/* Private pinned-runtime interface. The provider outlives every model/context
 * using its metadata. One provider serializes its mutable execution scratch. */
struct ascolto_streamed_moe;
ascolto_streamed_moe *ascolto_moe_open(const char *path, uint64_t budget,
                                    int threads, ggml_backend_dev_t device,
                                    bool require_nemotron = true);
void ascolto_moe_close(ascolto_streamed_moe *);
ggml_backend_buffer_type_t ascolto_moe_buft(ascolto_streamed_moe *);
ggml_context *ascolto_moe_metadata(ggml_backend_buffer_type_t);
ggml_tensor *ascolto_moe_import(ggml_backend_buffer_type_t, const ggml_tensor *);
ggml_tensor *ascolto_moe_matmul(ggml_context *, ggml_tensor *weight,
                              ggml_tensor *input, ggml_tensor *ids);
ggml_tensor *ascolto_moe_ffn(ggml_context *, ggml_tensor *up, ggml_tensor *down,
                           ggml_tensor *input, ggml_tensor *ids);
bool ascolto_moe_aborted(void *);
void ascolto_moe_check(ascolto_streamed_moe *);
void ascolto_moe_loaded(ascolto_streamed_moe *);
void ascolto_moe_stats(ascolto_streamed_moe *, ascolto_expert_cache_stats_t *);
bool ascolto_moe_uses_shared_cache(ascolto_streamed_moe *);

/* Content-free execution counters for native validation and SSD profiling. */
struct ascolto_moe_stream_stats_t {
    uint64_t prefetch_started = 0, prefetch_completed = 0;
    uint64_t prefetch_fallbacks = 0, prefetch_failures = 0;
    unsigned readers = 4;
};
void ascolto_moe_stream_stats(ascolto_streamed_moe *, ascolto_moe_stream_stats_t *);
