#include "ascolto/local_stt.h"
#include "parakeet_capi.h"
#include <cstddef>
#include "ggml_graph.hpp"
#include <cstdio>
#include <cstring>
#include <cstdlib>
static void *load(const char *path, char *error, size_t cap) {
#ifdef __APPLE__
    setenv("PARAKEET_DEVICE", "MTL0", 1);
#else
    setenv("PARAKEET_DEVICE", "CPU", 1);
#endif
    parakeet_ctx *ctx = parakeet_capi_load(path);
    if (!ctx) { std::snprintf(error, cap, "Parakeet could not load the verified model. Check device support and available memory."); pk::shutdown_backend(); }
    return ctx;
}
static int transcribe(void *ctx, const float *pcm, int n, const char *, const char *,
    ascolto_stt_text_t *out, char *error, size_t cap) {
    char *text = parakeet_capi_transcribe_pcm(static_cast<parakeet_ctx *>(ctx), pcm, n, 16000, 0);
    if (!text) { std::snprintf(error, cap, "Parakeet transcription failed."); return -1; }
    std::memset(out, 0, sizeof(*out));
    bool fits = std::strlen(text) < sizeof(out->text);
    if (fits) std::snprintf(out->text, sizeof(out->text), "%s", text);
    else std::snprintf(error, cap, "Parakeet result exceeds the transcript limit.");
    parakeet_capi_free_string(text); return fits ? 0 : -1;
}
static void destroy(void *ctx) {
    parakeet_capi_free(static_cast<parakeet_ctx *>(ctx));
    pk::shutdown_backend();
}
extern "C" __attribute__((visibility("default"))) const ascolto_stt_api_t *ascolto_stt_get_api(void) {
    static const ascolto_stt_api_t api = {ASCOLTO_STT_ABI, load, transcribe, destroy,
#ifdef __APPLE__
    "MTL0 / CPU"
#else
    "CPU"
#endif
    }; return &api;
}
