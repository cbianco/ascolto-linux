#include "ascolto/local_stt.h"
#include "salm_engine.h"
#include "ggml-backend.h"
#include "llama.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>
#include <stdexcept>
#include <thread>
#include <utility>
#ifdef ASCOLTO_NEMOTRON_BF16
#include "prepare_bf16.h"
#include <dlfcn.h>
#endif

using nemo_speech::speechlm::SalmConfig;
using nemo_speech::speechlm::SalmEngine;
using nemo_speech::speechlm::SalmRequest;
using nemo_speech::speechlm::SalmResult;

static constexpr const char *decoder = "decoder.gguf";
static constexpr const char *encoder = "encoder.gguf";

/* The registry belongs to the private library. Static registrations must remain
 * available if dyld retains the Objective-C/Metal image after dlclose. Model and
 * backend instances are freed by SalmEngine; true unload destroys the registry. */
static void release_backends() {
    llama_backend_free();
    ggml_set_abort_callback(nullptr);
}

static void *load(const char *path, char *error, size_t cap) {
    try {
        SalmConfig config;
        config.llm_path = std::string(path) + "/" + decoder;
        config.encoder_path = std::string(path) + "/" + encoder;
        /* One serialized VAD turn: avoid allocating a server's 16 recurrent/KV slots. */
        config.max_sequences = 1;
        config.n_ctx_per_sequence = 4096;
        config.max_context = 4096;
        config.n_batch = 512;
        config.encoder_max_batch = 1;
        config.encoder_ahead_seconds = 60;
        config.max_new_tokens = 1024;
        config.threads = std::min(8u, std::max(1u, std::thread::hardware_concurrency()));
        config.gpu = -1;
        ggml_backend_load_all();
        for (size_t i = 0; i < ggml_backend_dev_count(); ++i) {
            auto type = ggml_backend_dev_type(ggml_backend_dev_get(i));
            if (type == GGML_BACKEND_DEVICE_TYPE_GPU || type == GGML_BACKEND_DEVICE_TYPE_IGPU) {
                config.gpu = 0;
                break;
            }
        }
        /* The embedded build excludes the upstream CUDA path's fatal OOM handling. */
        config.mtp_draft = 0;
#if defined(ASCOLTO_NEMOTRON_BF16) || defined(ASCOLTO_NEMOTRON_Q8)
        config.expert_cache_bytes = 4ULL * 1024 * 1024 * 1024;
#endif
        return new SalmEngine(std::move(config));
    } catch (const std::exception& exception) {
        std::fprintf(stderr, "[nemotron] Model load failed: %.200s\n", exception.what());
        release_backends();
        std::snprintf(error, cap, "Nemotron could not load the verified GGUF package. Check available memory and the model package.");
        return nullptr;
    }
}

static int transcribe(void *ptr, const float *pcm, int n, const char *lang, const char *keywords,
                      ascolto_stt_text_t *out, char *error, size_t cap) {
    if (!ptr || !pcm || !out || n <= 0 || n > 60 * 16000) {
        std::snprintf(error, cap, "Nemotron expects a completed mono turn of at most 60 seconds at 16 kHz.");
        return -1;
    }
    std::memset(out, 0, sizeof(*out));
    for (int i = 0; i < n; ++i) if (!std::isfinite(pcm[i])) {
        std::snprintf(error, cap, "Nemotron received non-finite PCM samples.");
        return -1;
    }
    try {
        auto *engine = static_cast<SalmEngine *>(ptr);
        std::string prompt = "Transcribe the audio verbatim, with punctuation and capitalization. "
                             "Preserve repetitions and fillers. Output only the transcript. Do not translate.";
        const char *codes[] = {"it", "en", "de", "fr", "es", "nl", "pl"};
        const char *names[] = {"Italian", "English", "German", "French", "Spanish", "Dutch", "Polish"};
        const char *language = nullptr;
        if (lang && *lang && std::strcmp(lang, "auto")) {
            for (size_t i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
                if (!std::strcmp(lang, codes[i])) language = names[i];
            if (!language) {
                std::snprintf(error, cap, "Nemotron does not support this language hint in Ascolto.");
                return -1;
            }
            prompt += std::string(" Audio language: ") + language + ".";
        }
        if (keywords && *keywords) {
            if (std::strlen(keywords) > 4096 || !engine->special_token_in(keywords).empty() ||
                std::strstr(keywords, "<|audio|>")) {
                std::snprintf(error, cap, "Nemotron keywords exceed the limit or contain model control tokens.");
                return -1;
            }
            prompt += std::string(" Context terms: ") + keywords + ".";
        }
        prompt += " <|audio|>";
        SalmRequest request;
        request.samples = pcm;
        request.count = static_cast<size_t>(n);
        request.prompt = std::move(prompt);
        request.temperature = 0;
        bool submitted = false, completed = false;
        SalmResult result;
        engine->serve([&](std::vector<SalmRequest>& batch, size_t, bool) {
            if (submitted) return false;
            batch.push_back(std::move(request)); submitted = true; return true;
        }, [&](size_t, SalmResult&& value) { result = std::move(value); completed = true; });
        if (!completed || !result.error.empty() || result.cancelled ||
            (!result.text.empty() && !result.end_of_turn)) {
            std::snprintf(error, cap, "Nemotron inference failed or reached its generation/context limit. The turn was not committed.");
            return -1;
        }
        if (result.text.size() >= sizeof(out->text)) {
            std::snprintf(error, cap, "Nemotron result exceeds the transcript limit.");
            return -1;
        }
        std::memcpy(out->text, result.text.c_str(), result.text.size() + 1);
        if (language) std::snprintf(out->language, sizeof(out->language), "%s", lang);
        return 0;
    } catch (const std::exception&) {
        std::snprintf(error, cap, "Nemotron inference failed. Finish this session and check available memory.");
        return -1;
    }
}

static void destroy(void *ptr) {
    delete static_cast<SalmEngine *>(ptr);
    release_backends();
}

extern "C" __attribute__((visibility("default"))) const ascolto_stt_api_t *ascolto_stt_get_api(void) {
    static const ascolto_stt_api_t api = {ASCOLTO_STT_ABI, load, transcribe, destroy, "GGML CPU / Metal"};
    return &api;
}

#ifdef ASCOLTO_NEMOTRON_BF16
extern "C" __attribute__((visibility("default"))) int ascolto_stt_prepare_package_v1(
    int staging_fd, int verified_input_fd, bool (*cancel)(void *), void *userdata, char *error, size_t capacity) noexcept {
    try {
        Dl_info info{};
        if (!dladdr(reinterpret_cast<void *>(&ascolto_stt_prepare_package_v1), &info) || !info.dli_fname)
            throw std::runtime_error("Cannot locate native BF16 preparation resources");
        std::string resources = info.dli_fname;
        auto slash = resources.find_last_of('/');
        resources = (slash == std::string::npos ? "." : resources.substr(0, slash)) + "/nemotron-bf16-preparation";
        return ascolto_prepare_bf16(staging_fd, verified_input_fd, resources.c_str(), ASCOLTO_NEMOTRON_BF16_RECIPE_SHA256,
                                    cancel, userdata, error, capacity);
    } catch (...) {
        if (error && capacity) std::snprintf(error, capacity, "Cannot locate native BF16 preparation resources");
        return -1;
    }
}
#endif
