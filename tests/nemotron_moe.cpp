#include "expert_cache.h"
#include "ggml.h"
#include "ggml-backend.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <numeric>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

/* A synthetic single Nemotron-style MoE block. No NVIDIA weights, encoder,
 * token generation or claim about full-model latency is involved in this test. */
static constexpr int hidden = 64, intermediate = 32, experts = 128, selected = 6;
static constexpr size_t matrix_elements = hidden * intermediate;
static constexpr size_t matrix_bytes = matrix_elements * sizeof(ggml_bf16_t);
static constexpr size_t expert_bytes = 2 * matrix_bytes;
static int checks;
#define CHECK(x) do { ++checks; if (!(x)) throw std::runtime_error("check failed: " #x); } while (0)

static float value(int index) {
    return std::sin(float(index * 17 % 997)) * 0.08f;
}
static bool bounded_error(float actual, float expected, float &maximum) {
    if (!std::isfinite(actual) || !std::isfinite(expected)) return false;
    float difference = std::abs(actual - expected);
    if (!std::isfinite(difference)) return false;
    maximum = std::max(maximum, difference);
    return maximum < 0.0001f;
}
static std::vector<float> dense(const ggml_bf16_t *up, const ggml_bf16_t *down, const float *input) {
    std::array<float, intermediate> activation{};
    for (int i = 0; i < intermediate; ++i) {
        float sum = 0;
        for (int j = 0; j < hidden; ++j) sum += ggml_bf16_to_fp32(up[i * hidden + j]) * input[j];
        activation[i] = std::max(sum, 0.0f) * std::max(sum, 0.0f);
    }
    std::vector<float> result(hidden, 0);
    for (int i = 0; i < hidden; ++i)
        for (int j = 0; j < intermediate; ++j) result[i] += ggml_bf16_to_fp32(down[i * intermediate + j]) * activation[j];
    return result;
}
struct Block {
    ggml_backend_t backend;
    ggml_context *ctx = nullptr;
    ggml_backend_buffer_t buffer = nullptr;
    ggml_tensor *up, *down, *x, *ids, *output, *shared_output;
    ggml_cgraph *graph;
    explicit Block(ggml_backend_t b, const ggml_bf16_t *shared) : backend(b) {
        try {
        ctx = ggml_init({1024 * 1024, nullptr, true});
        CHECK(ctx);
        up = ggml_new_tensor_3d(ctx, GGML_TYPE_BF16, hidden, intermediate, selected);
        down = ggml_new_tensor_3d(ctx, GGML_TYPE_BF16, intermediate, hidden, selected);
        x = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, hidden, 1, 1);
        ids = ggml_new_tensor_2d(ctx, GGML_TYPE_I32, selected, 1);
        auto activation = ggml_mul_mat_id(ctx, up, x, ids);
        activation = ggml_sqr(ctx, ggml_relu(ctx, activation));
        output = ggml_mul_mat_id(ctx, down, activation, ids);
        auto shared_up = ggml_new_tensor_2d(ctx, GGML_TYPE_BF16, hidden, intermediate);
        auto shared_down = ggml_new_tensor_2d(ctx, GGML_TYPE_BF16, intermediate, hidden);
        auto shared_activation = ggml_mul_mat(ctx, shared_up, x);
        shared_output = ggml_mul_mat(ctx, shared_down, ggml_sqr(ctx, ggml_relu(ctx, shared_activation)));
        graph = ggml_new_graph_custom(ctx, 128, false);
        ggml_build_forward_expand(graph, output);
        ggml_build_forward_expand(graph, shared_output);
        buffer = ggml_backend_alloc_ctx_tensors(ctx, backend);
        CHECK(buffer);
        ggml_backend_tensor_set(shared_up, shared, 0, matrix_bytes);
        ggml_backend_tensor_set(shared_down, shared + matrix_elements, 0, matrix_bytes);
        } catch (...) { if (buffer) ggml_backend_buffer_free(buffer); if (ctx) ggml_free(ctx); throw; }
    }
    ~Block() { if (buffer) ggml_backend_buffer_free(buffer); if (ctx) ggml_free(ctx); }
    std::vector<float> run(const ascolto_expert_lease_t *leases, const float *input) {
        int32_t compact[selected];
        for (int i = 0; i < selected; ++i) {
            compact[i] = i;
            /* Keep the BF16 payload unchanged and remap the original 128 IDs
             * to six compact graph slots. Only these six experts reach GPU. */
            ggml_backend_tensor_set(up, leases[i].data, i * matrix_bytes, matrix_bytes);
            ggml_backend_tensor_set(down, leases[i].data + matrix_bytes, i * matrix_bytes, matrix_bytes);
        }
        ggml_backend_tensor_set(x, input, 0, hidden * sizeof(float));
        ggml_backend_tensor_set(ids, compact, 0, sizeof(compact));
        CHECK(ggml_backend_graph_compute(backend, graph) == GGML_STATUS_SUCCESS);
        /* graph_compute is synchronous; cache pins remain held through GPU completion. */
        std::vector<float> result(hidden * (selected + 1));
        ggml_backend_tensor_get(output, result.data(), 0, hidden * selected * sizeof(float));
        ggml_backend_tensor_get(shared_output, result.data() + hidden * selected, 0, hidden * sizeof(float));
        return result;
    }
};
static bool cancel(void *flag) { return static_cast<std::atomic<bool> *>(flag)->load(); }
static void packed_q8_cache() {
    const size_t up_bytes = ggml_row_size(GGML_TYPE_Q8_0, hidden) * intermediate;
    const size_t down_bytes = ggml_row_size(GGML_TYPE_Q8_0, intermediate) * hidden;
    const size_t packed_pair = up_bytes + down_bytes;
    const size_t stride = (packed_pair + 4095) & ~size_t(4095);
    CHECK(packed_pair < expert_bytes && stride >= packed_pair);
    std::vector<float> source(experts * matrix_elements);
    for (size_t i = 0; i < source.size(); ++i) source[i] = value(int(i));
    std::vector<unsigned char> packed(experts * packed_pair);
    CHECK(ggml_quantize_chunk(GGML_TYPE_Q8_0, source.data(), packed.data(), 0,
        experts * intermediate, hidden, nullptr) == experts * up_bytes);
    CHECK(ggml_quantize_chunk(GGML_TYPE_Q8_0, source.data(), packed.data() + experts * up_bytes, 0,
        experts * hidden, intermediate, nullptr) == experts * down_bytes);
    char path[] = "/tmp/ascolto-moe-q8-cache-XXXXXX";
    int fd = mkstemp(path); CHECK(fd >= 0);
    size_t written = 0;
    while (written < packed.size()) {
        const auto n = write(fd, packed.data() + written, packed.size() - written);
        CHECK(n > 0); written += size_t(n);
    }
    auto cache = ascolto_expert_cache_create(path, 8 * stride, packed_pair, 4); CHECK(cache);
    ascolto_expert_extent_t extents[selected];
    ascolto_expert_lease_t leases[selected]{};
    for (int i = 0; i < selected; ++i) {
        // Include the final expert and duplicate IDs; block headers must be
        // copied exactly with their quantized values and not treated as BF16.
        const int id = i == selected - 1 ? 127 : i / 2;
        extents[i] = {{uint64_t(id) * up_bytes, experts * up_bytes + uint64_t(id) * down_bytes},
                      {up_bytes, down_bytes}};
    }
    for (int pass = 0; pass < 2; ++pass) {
        CHECK(!ascolto_expert_cache_acquire(cache, extents, selected, nullptr, nullptr, leases));
        for (int i = 0; i < selected; ++i) {
            CHECK(!std::memcmp(leases[i].data, packed.data() + extents[i].offset[0], up_bytes));
            CHECK(!std::memcmp(leases[i].data + up_bytes, packed.data() + extents[i].offset[1], down_bytes));
            CHECK(!ascolto_expert_cache_release(cache, leases + i));
        }
        ascolto_expert_cache_stats_t stats{}; ascolto_expert_cache_stats(cache, &stats);
        CHECK(stats.capacity_bytes == 8 * stride && stats.slots == 8 && !stats.pinned_slots);
        CHECK(stats.bytes_read == 4 * packed_pair && stats.misses == 4);
        if (pass) CHECK(stats.hits == selected);
    }
    CHECK(!ascolto_expert_cache_destroy(cache));
    close(fd); unlink(path);
}
struct ReadGate {
    std::mutex mutex;
    std::condition_variable changed;
    int calls = 0;
    bool after_first_projection = false, resume_cancelled = false;
};
static bool cancel_during_read(void *argument) {
    auto &gate = *static_cast<ReadGate *>(argument);
    std::unique_lock<std::mutex> lock(gate.mutex);
    /* One reader and 4 KiB projections: the second cancellation checkpoint is
     * after the first successful pread, before the second projection's read. */
    if (++gate.calls == 2) {
        gate.after_first_projection = true; gate.changed.notify_all();
        gate.changed.wait(lock, [&] { return gate.resume_cancelled; });
    }
    return gate.resume_cancelled;
}

static ascolto_expert_extent_t extent(int id) {
    return {{uint64_t(id) * matrix_bytes, uint64_t(experts + id) * matrix_bytes}, {matrix_bytes, matrix_bytes}};
}
static void batch_reservation(const char *path, const ggml_bf16_t *weights) {
    auto cache = ascolto_expert_cache_create(path, 2 * expert_bytes, expert_bytes, 4); CHECK(cache);
    auto contents = [&](const ascolto_expert_lease_t &lease, int id) {
        CHECK(!std::memcmp(lease.data, weights + id * matrix_elements, matrix_bytes));
        CHECK(!std::memcmp(lease.data + matrix_bytes, weights + (experts + id) * matrix_elements, matrix_bytes));
    };
    auto release = [&](ascolto_expert_lease_t *leases, size_t count) {
        for (size_t i = 0; i < count; ++i) CHECK(!ascolto_expert_cache_release(cache, leases + i));
    };
    ascolto_expert_extent_t seed[] = {extent(0), extent(1)};
    ascolto_expert_lease_t initial[2]{};
    CHECK(!ascolto_expert_cache_acquire(cache, seed, 2, nullptr, nullptr, initial));
    const size_t original_a_slot = initial[0].slot;
    const uint64_t original_a_generation = initial[0].generation;
    release(initial, 2);
    ascolto_expert_cache_stats_t before{}, after{};
    ascolto_expert_cache_stats(cache, &before);
    // A is older than B. Loading C before reserving the later A hit used to
    // evict A and read it again, even though both requested experts fit.
    ascolto_expert_extent_t late_hit[] = {extent(2), extent(0)};
    ascolto_expert_lease_t pair[2]{};
    CHECK(!ascolto_expert_cache_acquire(cache, late_hit, 2, nullptr, nullptr, pair));
    contents(pair[0], 2); contents(pair[1], 0);
    CHECK(pair[1].slot == original_a_slot && pair[1].generation == original_a_generation);
    ascolto_expert_cache_stats(cache, &after);
    CHECK(after.misses == before.misses + 1 && after.hits == before.hits + 1);
    CHECK(after.bytes_read == before.bytes_read + expert_bytes && after.pinned_slots == 2);
    release(pair, 2);

    before = after;
    ascolto_expert_extent_t mixed[] = {extent(3), extent(2), extent(3), extent(2)};
    ascolto_expert_lease_t duplicate[4]{};
    CHECK(!ascolto_expert_cache_acquire(cache, mixed, 4, nullptr, nullptr, duplicate));
    for (int i = 0; i < 4; ++i) contents(duplicate[i], i % 2 ? 2 : 3);
    CHECK(duplicate[0].slot == duplicate[2].slot && duplicate[0].generation == duplicate[2].generation);
    CHECK(duplicate[1].slot == duplicate[3].slot && duplicate[1].generation == duplicate[3].generation);
    ascolto_expert_cache_stats(cache, &after);
    CHECK(after.misses == before.misses + 1 && after.hits == before.hits + 2);
    CHECK(after.bytes_read == before.bytes_read + expert_bytes && after.pinned_slots == 2);
    CHECK(!ascolto_expert_cache_release(cache, duplicate));
    CHECK(ascolto_expert_cache_destroy(cache) == ASCOLTO_EXPERT_BUSY);
    contents(duplicate[2], 3);
    release(duplicate + 1, 3);

    // An over-capacity batch must not evict entries, count partial hits or
    // disturb a lease belonging to another consumer, even with duplicates.
    ascolto_expert_lease_t held{};
    auto c = extent(2);
    CHECK(!ascolto_expert_cache_acquire(cache, &c, 1, nullptr, nullptr, &held));
    ascolto_expert_cache_stats(cache, &before);
    ascolto_expert_extent_t busy[] = {extent(0), extent(3), extent(2), extent(0)};
    CHECK(ascolto_expert_cache_acquire(cache, busy, 4, nullptr, nullptr, duplicate) == ASCOLTO_EXPERT_BUSY);
    for (const auto &lease : duplicate) CHECK(!lease.data);
    contents(held, 2);
    ascolto_expert_cache_stats(cache, &after);
    CHECK(after.hits == before.hits && after.misses == before.misses && after.bytes_read == before.bytes_read);
    CHECK(after.pinned_slots == 1);
    ascolto_expert_extent_t resident[] = {extent(3), extent(2)};
    CHECK(!ascolto_expert_cache_acquire(cache, resident, 2, nullptr, nullptr, pair));
    ascolto_expert_cache_stats(cache, &after);
    CHECK(after.hits == before.hits + 2 && after.misses == before.misses && after.bytes_read == before.bytes_read);
    release(pair, 2); CHECK(!ascolto_expert_cache_release(cache, &held));

    before = after;
    std::atomic<bool> cancelled{true};
    ascolto_expert_extent_t cancel_mixed[] = {extent(0), extent(3), extent(0), extent(3)};
    CHECK(ascolto_expert_cache_acquire(cache, cancel_mixed, 4, cancel, &cancelled, duplicate) == ASCOLTO_EXPERT_CANCELLED);
    for (const auto &lease : duplicate) CHECK(!lease.data);
    ascolto_expert_cache_stats(cache, &after);
    CHECK(!after.pinned_slots && after.bytes_read == before.bytes_read);
    before = after;
    CHECK(!ascolto_expert_cache_acquire(cache, cancel_mixed, 4, nullptr, nullptr, duplicate));
    for (int i = 0; i < 4; ++i) contents(duplicate[i], i % 2 ? 3 : 0);
    ascolto_expert_cache_stats(cache, &after);
    CHECK(after.misses == before.misses + 1 && after.hits == before.hits + 2);
    CHECK(after.bytes_read == before.bytes_read + expert_bytes);
    release(duplicate, 4);
    CHECK(!ascolto_expert_cache_destroy(cache));
}

struct ParallelReadGate {
    std::mutex mutex;
    std::condition_variable changed;
    size_t waiting = 0, peak = 0;
    bool release = false;
};
static bool cancel_parallel_reads(void *argument) {
    auto &gate = *static_cast<ParallelReadGate *>(argument);
    std::unique_lock<std::mutex> lock(gate.mutex);
    gate.peak = std::max(gate.peak, ++gate.waiting);
    gate.changed.notify_all();
    gate.changed.wait(lock, [&] { return gate.release; });
    --gate.waiting;
    return true;
}
static void bounded_parallel_readers(const char *path) {
    for (unsigned readers : {0u, 1u, 4u, unsigned(ASCOLTO_EXPERT_READERS_MAX), std::numeric_limits<unsigned>::max()}) {
        const size_t expected = std::max(1u, std::min(readers, unsigned(ASCOLTO_EXPERT_READERS_MAX)));
        auto cache = ascolto_expert_cache_create(path, ASCOLTO_EXPERT_BATCH_MAX * expert_bytes, expert_bytes, readers); CHECK(cache);
        std::array<ascolto_expert_extent_t, ASCOLTO_EXPERT_BATCH_MAX> extents;
        std::array<ascolto_expert_lease_t, ASCOLTO_EXPERT_BATCH_MAX> leases{};
        for (size_t i = 0; i < extents.size(); ++i) extents[i] = extent(int(i));
        ParallelReadGate gate;
        int result = ASCOLTO_EXPERT_INVALID;
        std::thread acquisition([&] {
            result = ascolto_expert_cache_acquire(cache, extents.data(), extents.size(), cancel_parallel_reads, &gate, leases.data());
        });
        bool reached;
        {
            std::unique_lock<std::mutex> lock(gate.mutex);
            reached = gate.changed.wait_for(lock, std::chrono::seconds(5), [&] { return gate.waiting == expected; });
            gate.release = true;
        }
        gate.changed.notify_all(); acquisition.join();
        CHECK(reached && gate.peak == expected);
        CHECK(result == ASCOLTO_EXPERT_CANCELLED);
        for (const auto &lease : leases) CHECK(!lease.data);
        ascolto_expert_cache_stats_t stats{}; ascolto_expert_cache_stats(cache, &stats);
        CHECK(!stats.pinned_slots && !stats.bytes_read);
        CHECK(!ascolto_expert_cache_destroy(cache));
    }
}

static void run(ggml_backend_t backend) {
    float rejected = 0;
    CHECK(!bounded_error(std::numeric_limits<float>::quiet_NaN(), 0, rejected));
    CHECK(!bounded_error(0, std::numeric_limits<float>::infinity(), rejected));
    std::vector<ggml_bf16_t> weights(2 * experts * matrix_elements), shared(2 * matrix_elements);
    for (size_t i = 0; i < weights.size(); ++i) weights[i] = ggml_fp32_to_bf16(value(int(i)));
    for (size_t i = 0; i < shared.size(); ++i) shared[i] = ggml_fp32_to_bf16(value(int(i + 413)));
    char path[] = "/tmp/ascolto-moe-block-XXXXXX";
    int fd = mkstemp(path); CHECK(fd >= 0);
    size_t written = 0;
    while (written < weights.size() * sizeof(ggml_bf16_t)) {
        ssize_t n = write(fd, reinterpret_cast<const char *>(weights.data()) + written,
                          weights.size() * sizeof(ggml_bf16_t) - written);
        CHECK(n > 0); written += size_t(n);
    }
    CHECK(!fsync(fd));
    batch_reservation(path, weights.data());
    bounded_parallel_readers(path);
    auto cache = ascolto_expert_cache_create(path, 8 * expert_bytes, expert_bytes, 4); CHECK(cache);
    Block block(backend, shared.data());
    float max_error = 0;
    ascolto_expert_lease_t leases[selected]{};
    ascolto_expert_extent_t extents[selected]{};
    std::array<float, hidden> input;
    for (int row = 0; row < 24; ++row) {
        for (int i = 0; i < hidden; ++i) input[i] = value(row * hidden + i + 73);
        std::array<float, experts> scores, corrected;
        std::array<int, experts> order;
        for (int e = 0; e < experts; ++e) {
            float logit = 0;
            for (int i = 0; i < hidden; ++i) logit += value(e * hidden + i + 137) * input[i];
            scores[e] = 1 / (1 + std::exp(-logit));
            corrected[e] = scores[e] + value((row / 3) * experts + e) * 0.2f;
            order[e] = e;
        }
        std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return corrected[a] > corrected[b]; });
        float norm = 0;
        for (int i = 0; i < selected; ++i) {
            int e = order[i]; norm += scores[e];
            extents[i] = {{uint64_t(e) * matrix_bytes, uint64_t(experts + e) * matrix_bytes}, {matrix_bytes, matrix_bytes}};
        }
        CHECK(!ascolto_expert_cache_acquire(cache, extents, selected, nullptr, nullptr, leases));
        for (int i = 0; i < selected; ++i) {
            CHECK(!std::memcmp(leases[i].data, weights.data() + order[i] * matrix_elements, matrix_bytes));
            CHECK(!std::memcmp(leases[i].data + matrix_bytes, weights.data() + (experts + order[i]) * matrix_elements, matrix_bytes));
        }
        auto output = block.run(leases, input.data());
        auto reference = dense(shared.data(), shared.data() + matrix_elements, input.data());
        std::vector<float> actual(output.begin() + hidden * selected, output.end());
        for (int i = 0; i < selected; ++i) {
            auto expected = dense(weights.data() + order[i] * matrix_elements,
                                  weights.data() + (experts + order[i]) * matrix_elements, input.data());
            float weight = scores[order[i]] / norm * 2.5f;
            for (int j = 0; j < hidden; ++j) {
                reference[j] += expected[j] * weight;
                actual[j] += output[i * hidden + j] * weight;
            }
            CHECK(!ascolto_expert_cache_release(cache, &leases[i]));
        }
        for (int j = 0; j < hidden; ++j) CHECK(bounded_error(actual[j], reference[j], max_error));
    }
    ascolto_expert_cache_stats_t stats{};
    ascolto_expert_cache_stats(cache, &stats);
    CHECK(stats.capacity_bytes == 8 * expert_bytes && !stats.pinned_slots && stats.misses && stats.hits);
    auto before = stats.bytes_read;
    CHECK(!ascolto_expert_cache_acquire(cache, extents, selected, nullptr, nullptr, leases));
    ascolto_expert_cache_stats(cache, &stats); CHECK(stats.bytes_read == before);
    CHECK(ascolto_expert_cache_destroy(cache) == ASCOLTO_EXPERT_BUSY);
    ascolto_expert_extent_t other[selected];
    for (int e = 0, filled = 0; filled < selected; ++e) {
        /* Pick a disjoint set: six pins plus six new experts exceed eight slots. */
        bool in_use = false;
        for (auto &extent : extents) if (extent.offset[0] == uint64_t(e) * matrix_bytes) in_use = true;
        if (!in_use) other[filled++] = {{uint64_t(e) * matrix_bytes, uint64_t(experts + e) * matrix_bytes}, {matrix_bytes, matrix_bytes}};
    }
    ascolto_expert_lease_t blocked[selected]{};
    CHECK(ascolto_expert_cache_acquire(cache, other, selected, nullptr, nullptr, blocked) == ASCOLTO_EXPERT_BUSY);
    ascolto_expert_cache_stats(cache, &stats); CHECK(stats.pinned_slots == selected);
    for (int i = 0; i < selected; ++i) {
        CHECK(!std::memcmp(leases[i].data, reinterpret_cast<const char *>(weights.data()) + extents[i].offset[0], matrix_bytes));
        CHECK(!std::memcmp(leases[i].data + matrix_bytes, reinterpret_cast<const char *>(weights.data()) + extents[i].offset[1], matrix_bytes));
    }
    for (auto &lease : leases) CHECK(!ascolto_expert_cache_release(cache, &lease));
    CHECK(ascolto_expert_cache_release(cache, &leases[0]) == ASCOLTO_EXPERT_INVALID);
    std::atomic<bool> cancelled{true};
    CHECK(ascolto_expert_cache_acquire(cache, other, selected, cancel, &cancelled, leases) == ASCOLTO_EXPERT_CANCELLED);
    ascolto_expert_cache_stats(cache, &stats); CHECK(!stats.pinned_slots);
    auto invalid = other[0]; invalid.offset[0] = UINT64_MAX;
    CHECK(ascolto_expert_cache_acquire(cache, &invalid, 1, nullptr, nullptr, leases) == ASCOLTO_EXPERT_INVALID);
    const auto measured = stats;
    CHECK(!ascolto_expert_cache_destroy(cache));
    /* Coordinate cancellation after a real partial read while release, stats
     * and another acquisition run on other threads. Partial cache bytes must
     * never become a successful hit in the queued acquisition. */
    cache = ascolto_expert_cache_create(path, 8 * expert_bytes, expert_bytes, 1); CHECK(cache);
    ascolto_expert_extent_t held_extent = {{127 * matrix_bytes, 255 * matrix_bytes}, {matrix_bytes, matrix_bytes}};
    ascolto_expert_lease_t held{};
    CHECK(!ascolto_expert_cache_acquire(cache, &held_extent, 1, nullptr, nullptr, &held));
    ReadGate gate;
    int cancelled_result = 0, queued_result = ASCOLTO_EXPERT_INVALID;
    ascolto_expert_lease_t cancelled_leases[selected]{}, queued_leases[selected]{};
    std::thread reading([&] { cancelled_result = ascolto_expert_cache_acquire(cache, other, selected,
                                      cancel_during_read, &gate, cancelled_leases); });
    bool reached;
    {
        std::unique_lock<std::mutex> lock(gate.mutex);
        reached = gate.changed.wait_for(lock, std::chrono::seconds(5), [&] { return gate.after_first_projection; });
    }
    std::atomic<bool> stop_stats{false}, good_stats{true};
    std::thread observer([&] {
        while (!stop_stats.load()) {
            ascolto_expert_cache_stats_t observed{}; ascolto_expert_cache_stats(cache, &observed);
            if (observed.pinned_slots > 8 || observed.capacity_bytes != 8 * expert_bytes) good_stats.store(false);
            std::this_thread::yield();
        }
    });
    int released = ascolto_expert_cache_release(cache, &held);
    std::thread queued([&] { queued_result = ascolto_expert_cache_acquire(cache, other, selected,
                                                   nullptr, nullptr, queued_leases); });
    {
        std::lock_guard<std::mutex> lock(gate.mutex); gate.resume_cancelled = true;
    }
    gate.changed.notify_all(); reading.join(); queued.join();
    stop_stats.store(true); observer.join();
    CHECK(reached && !released && good_stats.load());
    CHECK(cancelled_result == ASCOLTO_EXPERT_CANCELLED && queued_result == ASCOLTO_EXPERT_OK);
    for (int i = 0; i < selected; ++i) {
        CHECK(!cancelled_leases[i].data);
        CHECK(!std::memcmp(queued_leases[i].data, reinterpret_cast<const char *>(weights.data()) + other[i].offset[0], matrix_bytes));
        CHECK(!std::memcmp(queued_leases[i].data + matrix_bytes, reinterpret_cast<const char *>(weights.data()) + other[i].offset[1], matrix_bytes));
        CHECK(!ascolto_expert_cache_release(cache, &queued_leases[i]));
    }
    ascolto_expert_cache_stats(cache, &stats); CHECK(!stats.pinned_slots && stats.bytes_read >= matrix_bytes);
    CHECK(!ascolto_expert_cache_destroy(cache));
    /* A file truncated after indexing must cause short-read failure, never reuse
     * stale or partially filled expert bytes as a successful model result. */
    cache = ascolto_expert_cache_create(path, 8 * expert_bytes, expert_bytes, 4); CHECK(cache);
    CHECK(!ftruncate(fd, 1));
    CHECK(ascolto_expert_cache_acquire(cache, other, selected, nullptr, nullptr, leases) == ASCOLTO_EXPERT_IO);
    ascolto_expert_cache_stats(cache, &stats); CHECK(!stats.pinned_slots);
    CHECK(!ascolto_expert_cache_destroy(cache)); close(fd); unlink(path);
    std::printf("BF16 MoE prototype %s: PASS (%d checks; max error %.9g; cache %llu B; reads %llu B; hits %llu; misses %llu)\n",
                ggml_backend_name(backend), checks, max_error,
                (unsigned long long)measured.capacity_bytes, (unsigned long long)before,
                (unsigned long long)measured.hits, (unsigned long long)measured.misses);
}
int main(int argc, char **argv) {
    ggml_backend_load_all();
    auto backend = ggml_backend_init_by_type(argc == 2 && !std::strcmp(argv[1], "metal") ?
        GGML_BACKEND_DEVICE_TYPE_GPU : GGML_BACKEND_DEVICE_TYPE_CPU, nullptr);
    if (!backend) { std::fputs("Requested native backend is unavailable\n", stderr); return 2; }
    int result = 0;
    try { packed_q8_cache(); run(backend); } catch (const std::exception &e) { std::fprintf(stderr, "%s\n", e.what()); result = 1; }
    ggml_backend_free(backend);
    while (ggml_backend_reg_count()) ggml_backend_unload(ggml_backend_reg_get(0));
    return result;
}
