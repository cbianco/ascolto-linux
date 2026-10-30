#include "salm_engine.h"
#include "ggml-backend.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <vector>
#include <sstream>
#include "json.hpp"
#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/clonefile.h>
#include <unistd.h>
#endif

static void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
static uint32_t integer(std::ifstream &file, int bytes) {
    uint32_t result = 0;
    for (int i = 0; i < bytes; ++i) { int c = file.get(); check(c != EOF, "Truncated WAV"); result |= uint32_t(c) << (8 * i); }
    return result;
}
static std::vector<float> wave(const char *path) {
    std::ifstream f(path, std::ios::binary); check(bool(f), "Cannot open WAV");
    char tag[4]; f.read(tag, 4); check(!std::memcmp(tag, "RIFF", 4), "Expected RIFF");
    integer(f, 4); f.read(tag, 4); check(!std::memcmp(tag, "WAVE", 4), "Expected WAVE");
    uint32_t format = 0, bits = 0;
    while (f.read(tag, 4)) {
        auto size = integer(f, 4), start = uint32_t(f.tellg());
        if (!std::memcmp(tag, "fmt ", 4)) {
            format = integer(f, 2); check(integer(f, 2) == 1 && integer(f, 4) == 16000, "Expected mono 16 kHz");
            integer(f, 4); integer(f, 2); bits = integer(f, 2);
        }
        if (!std::memcmp(tag, "data", 4)) {
            check((format == 1 && bits == 16) || (format == 3 && bits == 32), "Expected PCM16 or float WAV");
            std::vector<float> data(size / (bits / 8));
            for (auto &v : data) {
                if (format == 1) v = int16_t(integer(f, 2)) / 32768.0f;
                else { auto n = integer(f, 4); std::memcpy(&v, &n, 4); }
                check(std::isfinite(v), "Non-finite input");
            }
            return data;
        }
        f.seekg(start + size + (size & 1));
    }
    throw std::runtime_error("No WAV audio");
}
int main(int argc, char **argv) {
    try {
        check(argc == 4 || argc == 5, "Usage: nemotron-decoder PACKAGE WAV|@MANIFEST CACHE_MIB (0 = resident) [fault|en|it]");
        const bool corpus = argv[2][0] == '@';
        struct Case { std::string id, path, language, reference; };
        std::vector<Case> cases;
        if (corpus) {
            std::ifstream manifest(argv[2] + 1); check(bool(manifest), "Cannot open evaluation manifest");
            std::string line;
            while (std::getline(manifest, line)) {
                if (line.empty() || line[0] == '#') continue;
                std::istringstream fields(line); Case item;
                check(bool(std::getline(fields,item.id,'\t')) && bool(std::getline(fields,item.path,'\t')) &&
                      bool(std::getline(fields,item.language,'\t')), "Invalid evaluation manifest row");
                std::getline(fields,item.reference);
                if (item.id == "id" && item.path == "path") continue;
                check(item.language == "en" || item.language == "it", "Unsupported evaluation language");
                cases.push_back(std::move(item));
            }
            check(!cases.empty(), "Empty evaluation manifest");
        }
        auto pcm = corpus ? std::vector<float>{} : wave(argv[2]);
        nemo_speech::speechlm::SalmConfig cfg;
        cfg.llm_path = std::string(argv[1]) + "/decoder.gguf";
        cfg.encoder_path = std::string(argv[1]) + "/encoder.gguf";
        cfg.expert_cache_bytes = std::stoull(argv[3]) * 1024 * 1024;
        bool fault = argc == 5 && std::string(argv[4]) == "fault";
        const std::string language = argc == 5 && std::string(argv[4]) == "it" ? "Italian" : "English";
        check(argc != 5 || fault || std::string(argv[4]) == "it" || std::string(argv[4]) == "en", "Invalid test mode");
        std::string cloned;
        std::string temporary_directory;
        if (fault) {
            check(cfg.expert_cache_bytes > 0, "Fault test requires SSD mode");
#ifdef __APPLE__
            char directory[] = "/tmp/ascolto-decoder-fault-XXXXXX";
            check(mkdtemp(directory), "Cannot create fault directory");
            temporary_directory = directory;
            cloned = temporary_directory + "/decoder.gguf";
            check(!clonefile(cfg.llm_path.c_str(), cloned.c_str(), 0), "Cannot clone decoder for fault test");
            cfg.llm_path = cloned;
#else
            throw std::runtime_error("Full-model fault fixture currently requires APFS clonefile");
#endif
        }
        cfg.max_sequences = 1; cfg.n_ctx_per_sequence = 4096; cfg.max_context = 4096;
        cfg.n_batch = 512; cfg.encoder_max_batch = 1; cfg.mtp_draft = 0; cfg.threads = 8;
        ggml_backend_load_all();
        cfg.gpu = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU) ? 0 : -1;
        const char *cpu_reference = std::getenv("ASCOLTO_NEMOTRON_TEST_CPU_REFERENCE");
        cfg.cpu_reference = cpu_reference && std::strcmp(cpu_reference, "1") == 0;
        if (cfg.cpu_reference) {
            check(cfg.expert_cache_bytes == 0, "CPU reference requires cache zero");
            cfg.gpu = -1;
            cfg.max_new_tokens = 128;
        }
        const auto load_start = std::chrono::steady_clock::now();
        nemo_speech::speechlm::SalmEngine engine(cfg);
        std::ofstream report;
        if (const char *path = std::getenv("ASCOLTO_NEMOTRON_TEST_JSONL")) {
            report.open(path); check(bool(report), "Cannot open evaluation report");
            report << nlohmann::json({{"type","load"},{"elapsed_s",std::chrono::duration<double>(std::chrono::steady_clock::now()-load_start).count()}}).dump() << std::endl;
        }
        if (fault) {
#ifdef __APPLE__
            check(!truncate(cloned.c_str(), 0), "Cannot truncate fault clone");
            unlink(cloned.c_str()); rmdir(temporary_directory.c_str());
#endif
            for (int attempt = 0; attempt < 2; ++attempt) {
                bool submitted = false, failed = false, published = false;
                try {
                    engine.serve([&](auto &batch, size_t, bool) {
                        if (submitted) return false;
                        nemo_speech::speechlm::SalmRequest request;
                        request.samples = pcm.data(); request.count = pcm.size();
                        request.on_text = [&](auto text) { if (!text.empty()) published = true; };
                        batch.push_back(std::move(request)); submitted = true; return true;
                    }, [&](size_t, auto &&value) {
                        if (!value.text.empty()) published = true;
                        failed = !value.error.empty();
                    });
                } catch (const std::exception &) { failed = true; }
                check(failed && !published, "SSD fault must fail without publishing text, including a retry");
            }
            std::puts("PASS SALM short-read failure, zero text publication, poisoned-context retry and teardown");
            return 0;
        }
        std::string previous;
        int repeats = corpus ? 1 : 2;
        if (const char *value = std::getenv("ASCOLTO_NEMOTRON_TEST_REPEATS")) {
            repeats = std::stoi(value); check(repeats > 0 && repeats <= 100, "Invalid repetition count");
        }
        bool any_failed = false;
        const size_t total = corpus ? cases.size() * size_t(repeats) : size_t(repeats);
        for (size_t cycle = 0; cycle < total; ++cycle) {
            const Case *item = corpus ? &cases[cycle % cases.size()] : nullptr;
            if (item) pcm = wave(item->path.c_str());
            check(!pcm.empty() && pcm.size() <= 60 * 16000, "Evaluation audio must be at most 60 seconds");
            const std::string turn_language = item ? (item->language == "it" ? "Italian" : "English") : language;
            const char *second_wave = std::getenv("ASCOLTO_NEMOTRON_TEST_SECOND_WAV");
            if (!corpus && cycle && second_wave) pcm = wave(second_wave);
            bool submitted = false, finished = false;
            nemo_speech::speechlm::SalmResult result;
            auto start = std::chrono::steady_clock::now();
            try { engine.serve([&](auto &batch, size_t, bool) {
                if (submitted) return false;
                nemo_speech::speechlm::SalmRequest request;
                request.samples = pcm.data(); request.count = pcm.size();
                request.prompt = "Transcribe the audio verbatim, with punctuation and capitalization. Preserve repetitions and fillers. Output only the transcript. Do not translate. Audio language: " + turn_language + ". <|audio|>";
                batch.push_back(std::move(request)); submitted = true; return true;
            }, [&](size_t, auto &&value) { result = std::move(value); finished = true; });
            } catch (const std::exception &exception) { result.error = exception.what(); }
            const bool ok = finished && result.error.empty() && !result.cancelled && result.end_of_turn && !result.text.empty();
            any_failed |= !ok;
            if (!corpus) check(ok, "Incomplete transcription");
            if (!corpus && cycle && !second_wave) check(previous == result.text, "Non-deterministic repeated transcript");
            previous = result.text;
            std::printf("TRANSCRIPT %s\n", result.text.c_str());
            const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::printf("CYCLE %zu seconds=%.3f\n", cycle, elapsed);
            nlohmann::json record = {{"type","clip"},{"id",item ? item->id : "fixture"},
                {"turn",cycle},{"repeat",corpus ? cycle/cases.size() : cycle},
                {"text",result.text},{"ok",ok},{"error",result.error},
                {"audio_s",pcm.size()/16000.0},{"elapsed_s",elapsed}};
#ifdef __APPLE__
            task_vm_info_data_t memory{}; mach_msg_type_number_t memory_count = TASK_VM_INFO_COUNT;
            if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&memory, &memory_count) == KERN_SUCCESS) {
                record["footprint_bytes"] = memory.phys_footprint;
                record["rss_bytes"] = memory.resident_size;
                record["peak_footprint_bytes"] = memory.ledger_phys_footprint_peak;
            }
#endif
            if (report) report << record.dump() << std::endl;
            std::fflush(stdout);
        }
#ifdef __APPLE__
        task_vm_info_data_t info{}; mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
            std::printf("FOOTPRINT %llu PEAK %llu RSS %llu RSS_PEAK %llu\n", info.phys_footprint,
                info.ledger_phys_footprint_peak, info.resident_size, info.resident_size_peak);
#endif
        return any_failed ? 1 : 0;
    } catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
