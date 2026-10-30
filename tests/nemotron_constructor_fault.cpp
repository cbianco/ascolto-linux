#include "salm_engine.h"
#include "ggml-backend.h"
#include "llama.h"
#include <cstdio>
#include <stdexcept>
#include <string>
#include <chrono>
#include <thread>
#include <algorithm>
#ifdef __APPLE__
#include <mach/mach.h>
static uint64_t footprint() {
    task_vm_info_data_t info{};
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) == KERN_SUCCESS ? info.phys_footprint : 0;
}
#else
static uint64_t footprint() { return 0; }
#endif

/* Opt-in fault injection using actual installed weights. An invalid user prompt
 * is rejected at the end of Impl's constructor, after model/context/samplers have
 * been acquired, without adding a failure hook to the shipping runtime. */
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    ggml_backend_load_all();
    uint64_t measured[8]{}, metal[8]{};
    auto device = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU);
    auto allocated = [&] {
        size_t available = 0, total = 0;
        if (device) ggml_backend_dev_memory(device, &available, &total);
        return total > available ? total - available : 0;
    };
    for (int attempt = 0; attempt < 8; ++attempt) {
#ifdef __APPLE__
        @autoreleasepool {
#endif
        nemo_speech::speechlm::SalmConfig config;
        config.llm_path = std::string(argv[1]) + "/decoder.gguf";
        config.encoder_path = std::string(argv[1]) + "/encoder.gguf";
        config.max_sequences = 1; config.max_context = 4096;
        config.n_ctx_per_sequence = 4096; config.n_batch = 512;
        config.encoder_max_batch = 1; config.mtp_draft = 0;
        config.prompt = "Deliberately invalid prompt without an audio locator.";
        config.gpu = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU) ? 0 : -1;
        try {
            nemo_speech::speechlm::SalmEngine engine(config);
            std::fputs("Late constructor fault was not triggered\n", stderr); return 1;
        } catch (const std::exception &exception) {
            if (std::string(exception.what()) != "prompt must contain exactly one <|audio|>") {
                std::fprintf(stderr, "Failed before the intended late constructor fault: %s\n", exception.what()); return 1;
            }
        }
        llama_backend_free();
#ifdef __APPLE__
        }
#endif
        /* Let the driver's asynchronous residency bookkeeping settle after
         * freeing buffers. Report every cycle, including first-use warmup. */
        uint64_t immediate = footprint(), immediate_metal = allocated();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        auto used = footprint();
        measured[attempt] = used; metal[attempt] = allocated();
        std::printf("Late constructor fault %d reached (footprint immediate/settled %llu/%llu; Metal allocated immediate/settled %llu/%llu bytes)\n",
            attempt + 1, (unsigned long long)immediate, (unsigned long long)used,
            (unsigned long long)immediate_metal, (unsigned long long)metal[attempt]);
        std::fflush(stdout);
    }
    auto bounds = std::minmax_element(measured + 4, measured + 8);
    if (*bounds.first && *bounds.second > *bounds.first + 128ULL * 1024 * 1024) {
        std::fprintf(stderr, "Footprint did not plateau over the last four cycles: %llu -> %llu\n",
                     (unsigned long long)*bounds.first, (unsigned long long)*bounds.second); return 1;
    }
    for (int i = 0; i < 8; ++i) if (metal[i] > 32ULL * 1024 * 1024) {
        std::fprintf(stderr, "Metal allocations remain after cleanup: %llu\n", (unsigned long long)metal[i]); return 1;
    }
    std::puts("Late constructor cleanup: PASS (eight faults; final four footprint plateau; Metal allocations released)");
    return 0;
}
