#include "runtime.h"
#include "ggml-cpu.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

static int checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error("check failed: " #x); } while (0)
int main(int argc, char **argv) {
    try {
        ggml_backend_load_all();
        auto gpu = argc > 1 && std::string(argv[1]) == "metal" ? ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_GPU) : nullptr;
        if (argc > 1) CHECK(gpu);
        auto backend = gpu ? ggml_backend_dev_init(gpu, nullptr) : ggml_backend_cpu_init(); CHECK(backend);
        if (!gpu) ggml_backend_cpu_set_n_threads(backend, 4);
        constexpr int time = 7, inputs = 2, outputs = 3, batch = 2, kernel = 3;
        for (int small = 0; small < 2; ++small) {
            auto ctx = ggml_init({1024 * 1024, nullptr, true}); CHECK(ctx);
            auto weights = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, kernel, inputs, outputs);
            auto audio = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, time, inputs, batch);
            auto result = ggml_runtime::conv_1d(ctx, weights, audio, 1, 1, 1);
            CHECK(result->ne[0] == time && result->ne[1] == outputs && result->ne[2] == batch);
            auto graph = ggml_new_graph_custom(ctx, 64, false); ggml_build_forward_expand(graph, result);
            auto buffer = ggml_backend_alloc_ctx_tensors(ctx, backend); CHECK(buffer);
            std::vector<float> w(kernel * inputs * outputs), x(time * inputs * batch), y(time * outputs * batch);
            for (size_t i = 0; i < w.size(); ++i)
                w[i] = ggml_bf16_to_fp32(ggml_fp32_to_bf16((small ? 1e-25f : .125f) * (1 + int(i % 5))));
            for (size_t i = 0; i < x.size(); ++i) x[i] = .25f * (1 + int(i % 7));
            ggml_backend_tensor_set(weights, w.data(), 0, w.size() * sizeof(float));
            ggml_backend_tensor_set(audio, x.data(), 0, x.size() * sizeof(float));
            CHECK(ggml_backend_graph_compute(backend, graph) == GGML_STATUS_SUCCESS);
            ggml_backend_tensor_get(result, y.data(), 0, y.size() * sizeof(float));
            for (int b = 0; b < batch; ++b) for (int o = 0; o < outputs; ++o) for (int t = 0; t < time; ++t) {
                double reference = 0;
                for (int in = 0; in < inputs; ++in) for (int j = 0; j < kernel; ++j) {
                    int position = t + j - 1;
                    if (position >= 0 && position < time)
                        reference += double(w[(o * inputs + in) * kernel + j]) * x[(b * inputs + in) * time + position];
                }
                float actual = y[(b * outputs + o) * time + t];
                CHECK(std::isfinite(actual) && actual > 0 && std::abs(double(actual) - reference) <= std::abs(reference) * 1e-5);
            }
            ggml_backend_buffer_free(buffer); ggml_free(ctx);
        }
        ggml_backend_free(backend);
        std::printf("PASS %d checks; F32 Conv1D preserves tiny BF16 values; backend %s\n", checks, gpu ? "Metal" : "CPU");
        return 0;
    } catch (const std::exception &error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
