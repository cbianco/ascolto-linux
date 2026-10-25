#include "ascolto/local_stt.h"
#include "qwen_asr.h"
#include "qwen_asr_kernels.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void *load(const char *path, char *error, size_t cap) {
    qwen_set_threads(8); qwen_verbose = 0;
    qwen_ctx_t *ctx = qwen_load(path);
    if (!ctx) snprintf(error, cap, "Qwen could not load the verified model. About 7 GB of memory is required.");
    else { ctx->segment_sec = 0; ctx->skip_silence = 0; ctx->past_text_conditioning = 0; }
    return ctx;
}
static int transcribe(void *ptr, const float *pcm, int n, const char *lang, const char *keywords,
    ascolto_stt_text_t *out, char *error, size_t cap) {
    qwen_ctx_t *ctx = ptr;
    const char *codes[] = {"it", "en", "de", "fr", "es", "nl", "pl"};
    const char *names[] = {"Italian", "English", "German", "French", "Spanish", "Dutch", "Polish"};
    const char *force = NULL;
    for (int i = 0; i < 7; ++i) if (lang && !strcmp(lang, codes[i])) force = names[i];
    if (lang && *lang && strcmp(lang, "auto") && !force) { snprintf(error, cap, "Qwen does not support this language hint."); return -1; }
    if (qwen_set_force_language(ctx, force) || qwen_set_prompt(ctx, keywords)) { snprintf(error, cap, "Qwen could not apply language or keywords."); return -1; }
    char *text = qwen_transcribe_audio(ctx, pcm, n);
    if (!text) { snprintf(error, cap, "Qwen transcription failed."); return -1; }
    memset(out, 0, sizeof(*out));
    int fits = strlen(text) < sizeof(out->text);
    if (fits) { snprintf(out->text, sizeof(out->text), "%s", text); if (force) snprintf(out->language, sizeof(out->language), "%s", lang); }
    else snprintf(error, cap, "Qwen result exceeds the transcript limit.");
    free(text); return fits ? 0 : -1;
}
static void destroy(void *ctx) { qwen_free(ctx); qwen_set_threads(1); }
__attribute__((visibility("default"))) const ascolto_stt_api_t *ascolto_stt_get_api(void) {
    static const ascolto_stt_api_t api = {ASCOLTO_STT_ABI, load, transcribe, destroy, "CPU BLAS"}; return &api;
}
