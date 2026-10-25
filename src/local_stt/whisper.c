#include "ascolto/local_stt.h"
#include "whisper.h"
#include <stdio.h>
#include <string.h>
static void *load(const char *path, char *error, size_t cap) {
    struct whisper_context_params p = whisper_context_default_params();
#ifdef __APPLE__
    p.use_gpu = true; p.flash_attn = true;
#else
    p.use_gpu = false;
#endif
    void *ctx = whisper_init_from_file_with_params(path, p);
    if (!ctx) snprintf(error, cap, "Whisper could not load the verified model. Check available memory.");
    return ctx;
}
static int transcribe(void *ctx, const float *pcm, int n, const char *lang, const char *keywords,
    ascolto_stt_text_t *out, char *error, size_t cap) {
    struct whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    p.n_threads = 8; p.language = lang && strcmp(lang, "auto") ? lang : "auto";
    p.no_context = true; p.no_timestamps = true; p.print_realtime = false;
    p.print_progress = false; p.print_timestamps = false; p.print_special = false;
    p.greedy.best_of = 1; p.temperature = 0; p.temperature_inc = 0;
    p.initial_prompt = keywords && *keywords ? keywords : NULL;
    if (whisper_full(ctx, p, pcm, n)) { snprintf(error, cap, "Whisper transcription failed."); return -1; }
    memset(out, 0, sizeof(*out));
    for (int s = 0; s < whisper_full_n_segments(ctx); ++s) {
        const char *text = whisper_full_get_segment_text(ctx, s);
        if (strlen(out->text) + strlen(text) >= sizeof(out->text)) { snprintf(error, cap, "Whisper result exceeds the transcript limit."); return -1; }
        strcat(out->text, text);
    }
    const char *detected = whisper_lang_str(whisper_full_lang_id(ctx));
    if (detected) snprintf(out->language, sizeof(out->language), "%s", detected);
    return 0;
}
static void destroy(void *ctx) { whisper_free(ctx); }
__attribute__((visibility("default"))) const ascolto_stt_api_t *ascolto_stt_get_api(void) {
    static const ascolto_stt_api_t api = {ASCOLTO_STT_ABI, load, transcribe, destroy,
#ifdef __APPLE__
    "Metal / CPU"
#else
    "CPU"
#endif
    }; return &api;
}
