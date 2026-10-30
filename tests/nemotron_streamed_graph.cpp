#include "streamed_moe.h"
#include "ggml-alloc.h"
#include "ggml-cpu.h"
#include "gguf.h"
#include "ggml-backend-impl.h"
#include <cmath>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error("check failed: " #x); } while (0)
constexpr int h = 64, f = 32, e = 128, k = 6, rows = 67;
using Owner = std::unique_ptr<ascolto_streamed_moe, decltype(&ascolto_moe_close)>;
static bool equivalent(const std::vector<float> &a, const std::vector<float> &b, float &error) {
    double signal = 0, delta = 0;
    float peak = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) return false;
        signal += double(a[i]) * a[i];
        double d = double(a[i]) - b[i]; delta += d * d;
        peak = std::max(peak, std::abs(a[i])); error = std::max(error, std::abs(a[i] - b[i]));
    }
    return peak > .001f && signal > 1e-6 && error <= 1e-7f + .0001f * peak && delta <= 1e-8 * signal;
}
static void count_node(ggml_tensor *, int ith, int, void *arg) { if (!ith) ++*static_cast<int *>(arg); }
static ggml_tensor *resident_product(ggml_context *ctx, ggml_tensor *weight, ggml_tensor *input, ggml_tensor *ids) {
    // The streamed microbatches retain Metal's MV arithmetic. A resident graph
    // with 32 or more rows would instead select MM and round its activations.
    ggml_tensor *result = nullptr;
    for (int row = 0; row < rows; row += 31) {
        int count = std::min(31, rows - row);
        auto x = ggml_view_3d(ctx, input, input->ne[0], input->ne[1], count,
                             input->nb[1], input->nb[2], size_t(row) * input->nb[2]);
        auto selected = ggml_view_2d(ctx, ids, k, count, ids->nb[1], size_t(row) * ids->nb[1]);
        auto product = ggml_mul_mat_id(ctx, weight, x, selected);
        result = result ? ggml_concat(ctx, result, product, 2) : product;
    }
    return result;
}

int main(int argc, char **argv) {
    try {
        ggml_backend_load_all();
        std::string mode = argc > 1 ? argv[1] : "cpu";
        bool metal = mode.find("metal") == 0, wide = mode.find("wide") != std::string::npos;
        bool fail_mapping = mode.find("fallback") != std::string::npos;
        const bool q8 = mode.find("q8") != std::string::npos;
        const ggml_type weight_type = q8 ? GGML_TYPE_Q8_0 : GGML_TYPE_BF16;
        const bool prefetch = mode.find("prefetch") != std::string::npos;
        CHECK(!setenv("ASCOLTO_NEMOTRON_SSD_PREFETCH", prefetch ? "1" : "0", 1));
        auto gpu = metal ? ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU) : nullptr;
        if (metal) CHECK(gpu);
        ggml_backend_device fake{};
        auto stream_device = gpu;
        if (fail_mapping) {
            fake = *gpu;
            fake.iface.buffer_from_host_ptr = [](ggml_backend_dev_t, void *, size_t, size_t) -> ggml_backend_buffer_t { return nullptr; };
            stream_device = &fake;
        }
        const size_t cache_slots = wide ? (q8 ? 405 : 200) : 8;
        const uint64_t budget = cache_slots * 8192;
        // Real Nemotron Q8 fits 405 pairs in 4 GiB. Four synthetic layers
        // force eviction and exercise remapped Metal IDs through slot 404.
        const int layer_count = wide && q8 ? 4 : 2;
        auto backend = ggml_backend_cpu_init(); CHECK(backend);
        ggml_backend_cpu_set_n_threads(backend, 4);
        auto ctx = ggml_init({8 * 1024 * 1024, nullptr, true}); CHECK(ctx);
        std::vector<std::array<ggml_tensor *, 2>> weights(layer_count);
        auto file = gguf_init_empty();
        for (int layer = 0; layer < layer_count; ++layer) for (int side = 0; side < 2; ++side) {
            auto t = ggml_new_tensor_3d(ctx, weight_type, side ? f : h, side ? h : f, e);
            char name[80]; std::snprintf(name, sizeof(name), "blk.%d.ffn_%s_exps.weight", layer, side ? "down" : "up");
            ggml_set_name(t, name); weights[layer][side] = t;
        }
        auto x_storage = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, h * 2, 1, rows);
        auto x = ggml_view_3d(ctx, x_storage, h, 1, rows, x_storage->nb[1], x_storage->nb[2], h * sizeof(float));
        auto ids = ggml_new_tensor_2d(ctx, GGML_TYPE_I32, k, rows);
        auto buffer = ggml_backend_alloc_ctx_tensors(ctx, backend); CHECK(buffer);
        for (int layer = 0; layer < layer_count; ++layer) for (int side = 0; side < 2; ++side) {
            auto t = weights[layer][side];
            std::vector<float> source(size_t(ggml_nelements(t)));
            for (size_t i = 0; i < source.size(); ++i)
                source[i] = std::sin(float((i * 17 + layer * 31 + side * 7) % 997)) * .2f;
            std::vector<unsigned char> data(ggml_nbytes(t));
            if (q8) {
                CHECK(ggml_quantize_chunk(weight_type, source.data(), data.data(), 0,
                    t->ne[1] * t->ne[2], t->ne[0], nullptr) == data.size());
                CHECK(data.size() == ggml_row_size(weight_type, t->ne[0]) * size_t(t->ne[1] * t->ne[2]));
                CHECK(data.size() < source.size() * sizeof(ggml_bf16_t));
            } else {
                for (size_t i = 0; i < source.size(); ++i) {
                    const auto value = ggml_fp32_to_bf16(source[i]);
                    std::memcpy(data.data() + i * sizeof(value), &value, sizeof(value));
                }
            }
            ggml_backend_tensor_set(t, data.data(), 0, data.size());
            gguf_add_tensor(file, t);
        }
        char path[] = "/tmp/ascolto-moe-gguf-XXXXXX";
        int fd = mkstemp(path); CHECK(fd >= 0); close(fd);
        CHECK(gguf_write_to_file(file, path, false)); gguf_free(file);
        // Reader overrides are private tuning inputs. Out-of-range/malformed
        // values must retain the bounded default instead of over-allocating.
        const char *readers_env = std::getenv("ASCOLTO_NEMOTRON_SSD_READERS");
        const bool had_readers = readers_env != nullptr;
        const std::string saved_readers = readers_env ? readers_env : "";
        const char *reader_values[] = {nullptr, "1", "4", "8", "16", "0", "17", "invalid"};
        for (const char *value : reader_values) {
            if (value) CHECK(!setenv("ASCOLTO_NEMOTRON_SSD_READERS", value, 1));
            else CHECK(!unsetenv("ASCOLTO_NEMOTRON_SSD_READERS"));
            Owner probe(ascolto_moe_open(path, budget, 2, stream_device, false), ascolto_moe_close);
            ascolto_moe_stream_stats_t tuning{}; ascolto_moe_stream_stats(probe.get(), &tuning);
            const unsigned expected = value && (!std::strcmp(value, "1") || !std::strcmp(value, "4") ||
                !std::strcmp(value, "8") || !std::strcmp(value, "16"))
                ? unsigned(std::strtoul(value, nullptr, 10)) : metal ? 8 : 4;
            if (tuning.readers != expected)
                std::fprintf(stderr, "Reader default mismatch: device=%s override=%s actual=%u expected=%u\n",
                    stream_device ? ggml_backend_dev_name(stream_device) : "CPU", value ? value : "unset", tuning.readers, expected);
            CHECK(tuning.readers == expected && !tuning.prefetch_started);
        }
        if (had_readers) CHECK(!setenv("ASCOLTO_NEMOTRON_SSD_READERS", saved_readers.c_str(), 1));
        else CHECK(!unsetenv("ASCOLTO_NEMOTRON_SSD_READERS"));
        Owner ssd(ascolto_moe_open(path, budget, 2, stream_device, false), ascolto_moe_close); CHECK(ssd);
        std::vector<std::array<ggml_tensor *, 2>> streamed(layer_count);
        for (int layer = 0; layer < layer_count; ++layer) for (int side = 0; side < 2; ++side) {
            streamed[layer][side] = ascolto_moe_import(ascolto_moe_buft(ssd.get()), weights[layer][side]);
            CHECK(streamed[layer][side] && !streamed[layer][side]->data && !streamed[layer][side]->buffer);
        }
        ascolto_expert_cache_stats_t stats{}; ascolto_moe_stats(ssd.get(), &stats);
        CHECK(stats.bytes_read == 0 && stats.pinned_slots == 0 && stats.capacity_bytes == budget);
        std::vector<float> inputs(h * 2 * rows);
        std::vector<int32_t> indices(k * rows);
        for (size_t i = 0; i < inputs.size(); ++i) inputs[i] = std::sin(float(i % 109)) * .75f;
        for (int row = 0; row < rows; ++row) for (int slot = 0; slot < k; ++slot) {
            int routed_row = row % 11 == 1 ? row - 1 : row;
            int routed_slot = row == rows - 1 && slot == k - 1 ? 0 : slot;
            indices[row * k + slot] = (routed_row * 7 + routed_slot * 11) % e;
        }
        ggml_backend_tensor_set(x_storage, inputs.data(), 0, inputs.size() * sizeof(float));
        ggml_backend_tensor_set(ids, indices.data(), 0, indices.size() * sizeof(int32_t));
        // The resident reference executes the ordinary GGML operations on all 128 experts.
        auto reference = x, actual = x;
        std::vector<std::pair<ggml_tensor *, ggml_tensor *>> stages;
        for (int layer = 0; layer < layer_count; ++layer) {
            // Start another two-layer branch before the deterministic small
            // fixture decays below the zero-output rejection threshold.
            if (layer == 2) { reference = x; actual = x; }
            auto up = resident_product(ctx, weights[layer][0], reference, ids);
            auto streamed_up = ascolto_moe_matmul(ctx, streamed[layer][0], actual, ids);
            auto fused = ascolto_moe_ffn(ctx, streamed[layer][0], streamed[layer][1], actual, ids);
            stages.emplace_back(up, streamed_up);
            reference = resident_product(ctx, weights[layer][1], ggml_sqr(ctx, ggml_relu(ctx, up)), ids);
            actual = ascolto_moe_matmul(ctx, streamed[layer][1], ggml_sqr(ctx, ggml_relu(ctx, streamed_up)), ids);
            stages.emplace_back(reference, actual);
            stages.emplace_back(reference, fused);
            // Collapse to one activation row between layers, preserving strided input on layer 0.
            reference = ggml_sum_rows(ctx, ggml_cont(ctx, ggml_transpose(ctx, reference)));
            actual = ggml_sum_rows(ctx, ggml_cont(ctx, ggml_transpose(ctx, actual)));
            reference = ggml_cont(ctx, ggml_transpose(ctx, reference));
            actual = ggml_cont(ctx, ggml_transpose(ctx, actual));
            stages.emplace_back(reference, actual);
        }
        auto graph = ggml_new_graph_custom(ctx, 256, false);
        for (auto stage : stages) {
            ggml_set_output(stage.first); ggml_set_output(stage.second);
            ggml_build_forward_expand(graph, stage.first); ggml_build_forward_expand(graph, stage.second);
        }
        auto resident_backend = gpu ? ggml_backend_dev_init(gpu, nullptr) : nullptr;
        ggml_backend_t backends[] = {resident_backend ? resident_backend : backend, backend};
        auto scheduler = ggml_backend_sched_new(backends, nullptr, resident_backend ? 2 : 1, 256, false, false);
        CHECK(scheduler);
        for (int i = 0; i < ggml_graph_n_nodes(graph); ++i) {
            auto node = ggml_graph_node(graph, i);
            ggml_backend_sched_set_tensor_backend(scheduler, node,
                resident_backend && node->op == GGML_OP_MUL_MAT_ID ? resident_backend : backend);
        }
        CHECK(ggml_backend_sched_alloc_graph(scheduler, graph));
        ggml_backend_cpu_set_abort_callback(backend, ascolto_moe_aborted, ssd.get());
        CHECK(ggml_backend_sched_graph_compute(scheduler, graph) == GGML_STATUS_SUCCESS);
        float maximum = 0;
        for (size_t stage = 0; stage < stages.size(); ++stage) {
            auto a = stages[stage].first, b = stages[stage].second;
            std::vector<float> expected(size_t(ggml_nelements(a))), received(expected.size()), zero(expected.size(), 0);
            ggml_backend_tensor_get(a, expected.data(), 0, ggml_nbytes(a));
            ggml_backend_tensor_get(b, received.data(), 0, ggml_nbytes(b));
            float error = 0, zero_error = 0;
            bool equal = equivalent(expected, received, error);
            std::printf("COMPARE stage=%zu backend=%s error=%.9g peak=%.9g\n", stage,
                ggml_backend_name(ggml_backend_sched_get_tensor_backend(scheduler, a)), error,
                *std::max_element(expected.begin(), expected.end()));
            CHECK(equal);
            if (metal) CHECK(expected == received);
            CHECK(!equivalent(expected, zero, zero_error));
            maximum = std::max(maximum, error);
            std::printf("STAGE %zu max_error=%.9g zero_error=%.9g\n", stage, error, zero_error);
        }
        if (prefetch && metal && !fail_mapping) for (int repeat = 0; repeat < 3; ++repeat) {
            CHECK(ggml_backend_sched_graph_compute(scheduler, graph) == GGML_STATUS_SUCCESS);
            for (auto stage : stages) {
                std::vector<float> expected(size_t(ggml_nelements(stage.first))), received(expected.size());
                ggml_backend_tensor_get(stage.first, expected.data(), 0, ggml_nbytes(stage.first));
                ggml_backend_tensor_get(stage.second, received.data(), 0, ggml_nbytes(stage.second));
                CHECK(expected == received);
            }
            ascolto_moe_stats(ssd.get(), &stats); CHECK(!stats.pinned_slots);
        }
        ascolto_moe_stats(ssd.get(), &stats);
        const size_t pair_bytes = (ggml_nbytes(weights[0][0]) + ggml_nbytes(weights[0][1])) / e;
        // Packed Q8 pairs leave page padding in each slot. Require eviction
        // against the usable payload capacity, rather than bytes of padding.
        CHECK(stats.bytes_read > stats.slots * pair_bytes && stats.misses > stats.slots && !stats.pinned_slots);
        if (wide) CHECK(stats.highest_slot == cache_slots - 1 && stats.slots == cache_slots);
        CHECK(ascolto_moe_uses_shared_cache(ssd.get()) == (metal && !fail_mapping));
        ascolto_moe_stream_stats_t execution{}; ascolto_moe_stream_stats(ssd.get(), &execution);
        if (prefetch && metal && !fail_mapping) {
            if (wide) CHECK(execution.prefetch_started > 0);
            CHECK(execution.prefetch_completed == execution.prefetch_started);
            CHECK(!execution.prefetch_failures);
            if (!wide) CHECK(execution.prefetch_fallbacks > 0);
        } else CHECK(!execution.prefetch_started && !execution.prefetch_completed);
        ggml_backend_cpu_set_abort_callback(backend, nullptr, nullptr);
        ggml_backend_sched_free(scheduler);
        if (resident_backend) ggml_backend_free(resident_backend);
        // Warm a fresh provider, then truncate before a miss on the next pass.
        // Both a final custom node and a custom with downstream work must propagate ABORTED.
        for (int fused = 0; fused < 2; ++fused) for (int downstream = 0; downstream < 2; ++downstream) {
            file = gguf_init_empty();
            for (auto &layer : weights) for (auto t : layer) gguf_add_tensor(file, t);
            CHECK(gguf_write_to_file(file, path, false)); gguf_free(file);
            Owner fault(ascolto_moe_open(path, wide ? 40 * 8192 : 65536, 2, stream_device, false), ascolto_moe_close);
            auto fc = ggml_init({1024 * 1024, nullptr, true}); CHECK(fc);
            auto w = ascolto_moe_import(ascolto_moe_buft(fault.get()), weights[0][0]);
            auto d = fused ? ascolto_moe_import(ascolto_moe_buft(fault.get()), weights[0][1]) : nullptr;
            auto fault_ids = wide ? ggml_new_tensor_2d(fc, GGML_TYPE_I32, k, 7) : ids;
            auto fault_input = wide ? ggml_view_3d(fc, x, h, 1, 7, x->nb[1], x->nb[2], 0) : x;
            auto result = fused ? ascolto_moe_ffn(fc, w, d, fault_input, fault_ids) : ascolto_moe_matmul(fc, w, fault_input, fault_ids);
            auto warm = ggml_new_graph_custom(fc, 32, false); ggml_build_forward_expand(warm, result);
            auto warm_buffer = ggml_backend_alloc_ctx_tensors(fc, backend); CHECK(warm_buffer);
            std::vector<int32_t> fault_indices(k * 7);
            if (wide) {
                // Keep 32 hits resident. The next atomic acquisition protects
                // them before loading its six misses, which fail after
                // truncation and must release every hit reservation.
                for (size_t i = 0; i < fault_indices.size(); ++i) fault_indices[i] = int32_t(i % 32);
                ggml_backend_tensor_set(fault_ids, fault_indices.data(), 0, fault_indices.size() * sizeof(int32_t));
            }
            ggml_backend_cpu_set_abort_callback(backend, ascolto_moe_aborted, fault.get());
            CHECK(ggml_backend_graph_compute(backend, warm) == GGML_STATUS_SUCCESS);
            CHECK(ascolto_moe_uses_shared_cache(fault.get()) == (metal && !fail_mapping));
            ascolto_moe_stats(fault.get(), &stats);
            const uint64_t warm_hits = stats.hits;
            if (wide) {
                for (size_t i = 0; i < fault_indices.size(); ++i) fault_indices[i] = int32_t(i % 38);
                ggml_backend_tensor_set(fault_ids, fault_indices.data(), 0, fault_indices.size() * sizeof(int32_t));
            }
            int calls = 0;
            if (downstream) {
                ggml_tensor *args[] = {result};
                result = ggml_custom_4d(fc, GGML_TYPE_F32, result->ne[0], k, result->ne[2], 1, args, 1, count_node, 1, &calls);
            }
            auto fg = ggml_new_graph_custom(fc, 32, false); ggml_build_forward_expand(fg, result);
            auto fb = downstream ? ggml_backend_alloc_ctx_tensors(fc, backend) : nullptr;
            if (downstream) CHECK(fb);
            CHECK(!truncate(path, 0));
            ggml_backend_cpu_set_abort_callback(backend, ascolto_moe_aborted, fault.get());
            CHECK(ggml_backend_graph_compute(backend, fg) == GGML_STATUS_ABORTED);
            CHECK(ascolto_moe_aborted(fault.get()) && calls == 0);
            bool rejected = false;
            try { ascolto_moe_check(fault.get()); } catch (...) { rejected = true; }
            CHECK(rejected);
            ascolto_moe_stats(fault.get(), &stats); CHECK(!stats.pinned_slots);
            if (wide) CHECK(stats.hits >= warm_hits + 32);
            ggml_backend_cpu_set_abort_callback(backend, nullptr, nullptr);
            if (fb) ggml_backend_buffer_free(fb);
            ggml_backend_buffer_free(warm_buffer); ggml_free(fc);
        }
        if (prefetch && metal && !fail_mapping) for (int fused = 0; fused < 2; ++fused) {
            file = gguf_init_empty();
            for (auto &layer : weights) for (auto t : layer) gguf_add_tensor(file, t);
            CHECK(gguf_write_to_file(file, path, false)); gguf_free(file);
            Owner fault(ascolto_moe_open(path, 40 * 8192, 2, stream_device, false), ascolto_moe_close);
            auto fc = ggml_init({1024 * 1024, nullptr, true}); CHECK(fc);
            auto w = ascolto_moe_import(ascolto_moe_buft(fault.get()), weights[0][0]);
            auto d = fused ? ascolto_moe_import(ascolto_moe_buft(fault.get()), weights[0][1]) : nullptr;
            auto fault_ids = ggml_new_tensor_2d(fc, GGML_TYPE_I32, k, rows);
            auto result = fused ? ascolto_moe_ffn(fc, w, d, x, fault_ids) : ascolto_moe_matmul(fc, w, x, fault_ids);
            int calls = 0;
            ggml_tensor *args[] = {result};
            auto downstream = ggml_custom_4d(fc, GGML_TYPE_F32, result->ne[0], k, rows, 1, args, 1, count_node, 1, &calls);
            auto fg = ggml_new_graph_custom(fc, 32, false); ggml_build_forward_expand(fg, downstream);
            auto fb = ggml_backend_alloc_ctx_tensors(fc, backend); CHECK(fb);
            std::vector<int32_t> routed(k * rows);
            for (size_t i = 0; i < routed.size(); ++i) routed[i] = int32_t(i % k);
            ggml_backend_tensor_set(fault_ids, routed.data(), 0, routed.size() * sizeof(int32_t));
            ggml_backend_cpu_set_abort_callback(backend, ascolto_moe_aborted, fault.get());
            CHECK(ggml_backend_graph_compute(backend, fg) == GGML_STATUS_SUCCESS);
            CHECK(calls == 1);
            ascolto_moe_stream_stats(fault.get(), &execution);
            const auto completed = execution.prefetch_completed;
            CHECK(completed > 0);
            // The current 31-row bank still hits its six experts, so Metal
            // computes while the known next bank reads six missing experts.
            // Truncation makes that asynchronous read fail after a valid bank.
            for (int row = 31; row < rows; ++row) for (int slot = 0; slot < k; ++slot)
                routed[size_t(row * k + slot)] = 6 + slot;
            ggml_backend_tensor_set(fault_ids, routed.data(), 0, routed.size() * sizeof(int32_t));
            CHECK(!truncate(path, 0));
            calls = 0;
            CHECK(ggml_backend_graph_compute(backend, fg) == GGML_STATUS_ABORTED);
            CHECK(ascolto_moe_aborted(fault.get()) && calls == 0);
            ascolto_moe_stream_stats(fault.get(), &execution);
            CHECK(execution.prefetch_failures == 1 && execution.prefetch_completed == completed);
            CHECK(execution.prefetch_started == completed + 1);
            ascolto_moe_stats(fault.get(), &stats); CHECK(!stats.pinned_slots);
            std::vector<float> discarded(size_t(ggml_nelements(result)), 1);
            ggml_backend_tensor_get(result, discarded.data(), 0, ggml_nbytes(result));
            CHECK(std::all_of(discarded.begin(), discarded.end(), [](float value) { return value == 0; }));
            bool rejected = false;
            try { ascolto_moe_check(fault.get()); } catch (...) { rejected = true; }
            CHECK(rejected);
            ggml_backend_cpu_set_abort_callback(backend, nullptr, nullptr);
            ggml_backend_buffer_free(fb); ggml_free(fc);
        }
        unlink(path);
        ggml_backend_buffer_free(buffer); ggml_free(ctx); ggml_backend_free(backend);
        std::printf("PASS %d checks; multilayer %s max error %.9g; backend %s\n", checks,
            q8 ? "Q8_0" : "BF16", maximum, gpu ? "Metal" : "CPU");
        return 0;
    } catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
