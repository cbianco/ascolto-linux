#include "streamed_moe.h"
#include "ggml-backend-impl.h"
#include "ggml-cpu.h"
#include "ggml-alloc.h"
#include "gguf.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
constexpr int max_selected = ASCOLTO_EXPERT_BATCH_MAX;
// Metal switches MUL_MAT_ID from MV to MM at 32 rows. Keep the original MV
// arithmetic, including F32 activations, while amortizing graph submission.
constexpr int max_batch_rows = 31;
constexpr uint64_t entry_magic = UINT64_C(0x4153434d4f455353);
void require(bool ok, const char *why) { if (!ok) throw std::runtime_error(why); }
struct Projection {
    uint64_t magic = entry_magic;
    ascolto_streamed_moe *owner = nullptr;
    int layer = -1, side = -1;
    ggml_type type = GGML_TYPE_COUNT;
    int64_t in = 0, out = 0, experts = 0;
    uint64_t offset = 0;
    size_t bytes = 0;
    ggml_tensor *metadata = nullptr;
};
struct SharedView {
    ggml_backend_buffer_t buffer = nullptr;
    const void *base = nullptr;
    size_t bytes = 0, stride = 0, slots = 0;
};
/* Each backend owns a separate CPU pool / GPU queue. It never executes on the
 * outer scheduler's pool while that pool waits for this custom operation. */
struct Product {
    ggml_backend_t backend = nullptr;
    ggml_context *ctx = nullptr;
    ggml_backend_buffer_t buffer = nullptr;
    ggml_tensor *weight = nullptr, *down = nullptr, *input = nullptr, *ids = nullptr, *output = nullptr;
    ggml_cgraph *graph = nullptr;
    int64_t in, out, count;
    ggml_type type;
    bool shared;
    Product(const Projection &p, int64_t k, int64_t rows, int threads, ggml_backend_dev_t device, bool fused,
            const SharedView &view, size_t up_bytes)
        : in(p.in), out(p.out), count(k), type(p.type), shared(view.buffer != nullptr) {
        try {
            ctx = ggml_init({1024 * 1024, nullptr, true}); require(ctx, "SSD graph metadata allocation failed");
            auto bind = [&](ggml_tensor *tensor, size_t offset) {
                tensor->nb[2] = view.stride; tensor->nb[3] = view.stride * view.slots;
                require(offset <= view.bytes && ggml_nbytes(tensor) <= view.bytes - offset, "Shared SSD tensor exceeds cache bounds");
                require(ggml_backend_tensor_alloc(view.buffer, tensor, const_cast<char *>(static_cast<const char *>(view.base)) + offset) == GGML_STATUS_SUCCESS,
                        "Cannot bind shared SSD cache tensor");
            };
            weight = ggml_new_tensor_3d(ctx, type, in, out, shared ? int64_t(view.slots) : k);
            if (shared) bind(weight, p.side ? up_bytes : 0);
            input = ggml_new_tensor_3d(ctx, GGML_TYPE_F32, in, k, rows);
            ids = ggml_new_tensor_2d(ctx, GGML_TYPE_I32, k, rows);
            output = ggml_mul_mat_id(ctx, weight, input, ids);
            if (fused) {
                down = ggml_new_tensor_3d(ctx, type, out, in, shared ? int64_t(view.slots) : k);
                if (shared) bind(down, up_bytes);
                output = ggml_mul_mat_id(ctx, down, ggml_sqr(ctx, ggml_relu(ctx, output)), ids);
            }
            graph = ggml_new_graph_custom(ctx, 32, false);
            ggml_build_forward_expand(graph, output);
            // The pinned Metal backend supports native BF16 and Q8_0 routed
            // products. Other expert formats are not supported.
            if (device && (p.type == GGML_TYPE_BF16 || p.type == GGML_TYPE_Q8_0) &&
                    ggml_backend_dev_supports_op(device, output))
                backend = ggml_backend_dev_init(device, nullptr);
            else {
                backend = ggml_backend_cpu_init();
                require(backend, "SSD CPU backend allocation failed");
                ggml_backend_cpu_set_n_threads(backend, threads);
            }
            require(backend, "SSD backend allocation failed");
            std::fprintf(stderr, "[ssd] compact backend=%s in=%lld out=%lld experts=%lld fused=%d shared_cache=%d\n", ggml_backend_name(backend),
                (long long)in, (long long)out, (long long)k, fused, shared);
            buffer = ggml_backend_alloc_ctx_tensors(ctx, backend);
            require(buffer, "SSD compact expert allocation failed");
            std::vector<int32_t> compact(size_t(k * rows));
            for (size_t i = 0; i < compact.size(); ++i) compact[i] = int32_t(i % k);
            ggml_backend_tensor_set(ids, compact.data(), 0, compact.size() * sizeof(int32_t));
        } catch (...) { clear(); throw; }
    }
    void clear() {
        if (backend) ggml_backend_synchronize(backend);
        if (buffer) ggml_backend_buffer_free(buffer);
        if (ctx) ggml_free(ctx);
        if (backend) ggml_backend_free(backend);
        backend = nullptr; buffer = nullptr; ctx = nullptr;
    }
    ~Product() { clear(); }
};
struct Leases {
    ascolto_expert_cache_t *cache;
    std::vector<ascolto_expert_lease_t> held;
    explicit Leases(ascolto_expert_cache_t *c, size_t count) : cache(c), held(count) {}
    ~Leases() { for (auto &lease : held) if (lease.data) ascolto_expert_cache_release(cache, &lease); }
};
struct RoutedBatch {
    int64_t row = 0, count = 0;
    std::vector<int32_t> selected, unique;
    std::vector<ascolto_expert_extent_t> extents;
    std::unique_ptr<Leases> leases;
    double acquire_ms = 0;
};
const char *tag_name(ggml_backend_buffer_type_t) { return "Ascolto_SSD_metadata"; }
size_t tag_alignment(ggml_backend_buffer_type_t) { return 32; }
}

struct ascolto_streamed_moe {
    ggml_backend_buffer_type tag{};
    ggml_context *metadata = nullptr;
    ascolto_expert_cache_t *cache = nullptr;
    std::map<std::string, Projection> projections;
    std::map<int, std::array<Projection *, 2>> layers;
    std::array<std::map<int64_t, std::unique_ptr<Product>>, 2> products;
    std::map<int64_t, std::unique_ptr<Product>> fused_products;
    SharedView shared;
    bool shared_checked = false;
    std::atomic<bool> failed{false};
    std::mutex mutex;
    ggml_backend_dev_t device = nullptr;
    int threads = 1;
    int batch_rows = max_batch_rows;
    bool profile = false;
    bool prefetch = false;
    ascolto_moe_stream_stats_t stream_stats;
    ~ascolto_streamed_moe() {
        products = {};
        fused_products.clear();
        if (shared.buffer) ggml_backend_buffer_free(shared.buffer);
        if (cache) ascolto_expert_cache_destroy(cache);
        if (metadata) ggml_free(metadata);
    }
};

ascolto_streamed_moe *ascolto_moe_open(const char *path, uint64_t budget,
                                    int threads, ggml_backend_dev_t device, bool strict) {
    auto result = std::make_unique<ascolto_streamed_moe>();
    std::unique_ptr<gguf_context, decltype(&gguf_free)> file(
        gguf_init_from_file(path, {true, nullptr}), gguf_free);
    require(bool(file), "Cannot index SSD GGUF metadata");
    auto split = gguf_find_key(file.get(), "split.count");
    require(split < 0 || gguf_get_val_u16(file.get(), split) == 1, "Split SSD GGUF files are unsupported");
    if (strict) {
        auto arch = gguf_find_key(file.get(), "general.architecture");
        require(arch >= 0 && std::string(gguf_get_val_str(file.get(), arch)) == "nemotron_h_moe",
                "SSD backend requires Nemotron-H MoE");
    }
    size_t maximum = 0;
    int64_t backbone_layers = std::numeric_limits<int64_t>::max();
    if (strict) {
        auto count = gguf_find_key(file.get(), "nemotron_h_moe.block_count");
        auto mtp = gguf_find_key(file.get(), "nemotron_h_moe.nextn_predict_layers");
        require(count >= 0, "SSD decoder has no block count");
        backbone_layers = gguf_get_val_u32(file.get(), count);
        if (mtp >= 0) backbone_layers -= gguf_get_val_u32(file.get(), mtp);
        require(backbone_layers == 52, "Unexpected Nemotron backbone depth");
    }
    for (int64_t i = 0; i < gguf_get_n_tensors(file.get()); ++i) {
        std::string name = gguf_get_tensor_name(file.get(), i);
        int layer = -1, consumed = 0, side = -1;
        if (std::sscanf(name.c_str(), "blk.%d.ffn_up_exps.weight%n", &layer, &consumed) == 1 &&
            consumed == int(name.size())) side = 0;
        consumed = 0;
        if (std::sscanf(name.c_str(), "blk.%d.ffn_down_exps.weight%n", &layer, &consumed) == 1 &&
            consumed == int(name.size())) side = 1;
        if (side < 0) continue;
        if (layer >= backbone_layers) continue; // MTP is disabled and never indexed or loaded.
        auto ne = gguf_get_tensor_ne(file.get(), i);
        auto type = gguf_get_tensor_type(file.get(), i);
        require(layer >= 0 && ne[0] > 0 && ne[1] > 0 && ne[2] > 0 && ne[3] == 1,
                "Invalid SSD expert dimensions");
        require(type == GGML_TYPE_BF16 || type == GGML_TYPE_Q8_0,
                "Unsupported SSD expert dtype");
        // Quantized projections contain packed blocks, not one scalar per
        // logical element. Derive each expert extent from its complete rows.
        require(ne[0] % ggml_blck_size(type) == 0 &&
                uint64_t(ne[0]) <= SIZE_MAX / ggml_type_size(type), "Invalid SSD expert row");
        const size_t row_bytes = ggml_row_size(type, ne[0]);
        require(uint64_t(ne[1]) <= SIZE_MAX / row_bytes, "SSD expert size overflow");
        const size_t expert_bytes = row_bytes * size_t(ne[1]);
        auto bytes = gguf_get_tensor_size(file.get(), i);
        require(uint64_t(ne[2]) <= SIZE_MAX / expert_bytes &&
                bytes == expert_bytes * size_t(ne[2]), "Invalid SSD expert stride");
        auto &p = result->projections[name];
        p.owner = result.get(); p.layer = layer; p.side = side; p.type = type;
        p.in = ne[0]; p.out = ne[1]; p.experts = ne[2]; p.bytes = expert_bytes;
        const uint64_t data = gguf_get_data_offset(file.get()), offset = gguf_get_tensor_offset(file.get(), i);
        require(offset <= UINT64_MAX - data && bytes <= UINT64_MAX - data - offset, "SSD extent overflow");
        p.offset = data + offset;
        result->layers[layer][side] = &p;
    }
    require(!result->layers.empty(), "GGUF contains no SSD expert pairs");
    if (strict) require(result->layers.size() == 23, "SSD decoder requires exactly 23 backbone MoE layers without MTP");
    for (auto &layer : result->layers) {
        auto up = layer.second[0], down = layer.second[1];
        require(up && down && up->in == down->out && up->out == down->in && up->experts == down->experts &&
                up->type == down->type, "Inconsistent SSD expert pair");
        if (strict) require(up->in == 2688 && up->out == 1856 && up->experts == 128, "Unexpected Nemotron expert layout");
        require(up->bytes <= SIZE_MAX - down->bytes, "SSD pair size overflow");
        maximum = std::max(maximum, up->bytes + down->bytes);
    }
    require(threads > 0 && threads <= 256, "Invalid SSD compute thread count");
    result->threads = threads; result->device = device;
    // The measured Apple Silicon configuration overlaps SSD reads with Metal.
    // Keep the established CPU defaults on platforms not benchmarked here.
    // The device display name can be empty before lazy Metal initialization.
    // Registry identity is available immediately and does not initialize a GPU.
    auto registry = device ? ggml_backend_dev_backend_reg(device) : nullptr;
    if (registry && !std::strcmp(ggml_backend_reg_name(registry), "MTL")) {
        result->stream_stats.readers = 8;
        result->prefetch = true;
    }
    if (const char *value = std::getenv("ASCOLTO_NEMOTRON_SSD_BATCH_ROWS")) {
        char *end = nullptr;
        long rows = std::strtol(value, &end, 10);
        if (end != value && !*end && rows >= 1 && rows <= max_batch_rows) result->batch_rows = int(rows);
    }
    const char *profile = std::getenv("ASCOLTO_NEMOTRON_SSD_PROFILE");
    result->profile = profile && *profile && std::strcmp(profile, "0");
    const char *prefetch = std::getenv("ASCOLTO_NEMOTRON_SSD_PREFETCH");
    if (prefetch) result->prefetch = !std::strcmp(prefetch, "1");
    if (const char *value = std::getenv("ASCOLTO_NEMOTRON_SSD_READERS")) {
        char *end = nullptr;
        long readers = std::strtol(value, &end, 10);
        if (end != value && !*end && readers >= 1 && readers <= 16)
            result->stream_stats.readers = unsigned(readers);
    }
    result->metadata = ggml_init({ggml_tensor_overhead() * (result->projections.size() + 8), nullptr, true});
    require(result->metadata, "SSD metadata allocation failed");
    result->cache = ascolto_expert_cache_create(path, budget, maximum, result->stream_stats.readers);
    require(result->cache, "SSD cache allocation or file validation failed");
    ascolto_expert_cache_stats_t stats{}; ascolto_expert_cache_stats(result->cache, &stats);
    require(stats.slots >= 6, "SSD cache must hold at least six expert pairs");
    result->tag.iface.get_name = tag_name;
    result->tag.iface.get_alignment = tag_alignment;
    result->tag.device = ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU);
    result->tag.context = result.get();
    return result.release();
}

void ascolto_moe_close(ascolto_streamed_moe *p) { delete p; }
ggml_backend_buffer_type_t ascolto_moe_buft(ascolto_streamed_moe *p) { return &p->tag; }
ggml_context *ascolto_moe_metadata(ggml_backend_buffer_type_t b) {
    return b && b->iface.get_name == tag_name ? static_cast<ascolto_streamed_moe *>(b->context)->metadata : nullptr;
}
ggml_tensor *ascolto_moe_import(ggml_backend_buffer_type_t b, const ggml_tensor *t) {
    auto ctx = ascolto_moe_metadata(b); if (!ctx) return nullptr;
    auto p = static_cast<ascolto_streamed_moe *>(b->context);
    auto found = p->projections.find(t->name);
    require(found != p->projections.end(), "Unindexed SSD tensor");
    auto &entry = found->second;
    require(!entry.metadata && t->type == entry.type && t->ne[0] == entry.in && t->ne[1] == entry.out &&
            t->ne[2] == entry.experts && t->ne[3] == 1 && ggml_is_contiguous(t), "SSD tensor metadata mismatch");
    auto result = ggml_dup_tensor(ctx, t); ggml_set_name(result, t->name);
    result->extra = &entry; entry.metadata = result;
    return result;
}

namespace {
void prepare_shared(ascolto_streamed_moe &owner, const Projection &p) {
    if (owner.shared_checked) return;
    owner.shared_checked = true;
#ifdef __APPLE__
    if (!owner.device || (p.type != GGML_TYPE_BF16 && p.type != GGML_TYPE_Q8_0) ||
            std::strncmp(ggml_backend_dev_name(owner.device), "MTL", 3)) return;
    ggml_backend_dev_props props{}; ggml_backend_dev_get_props(owner.device, &props);
    if (!props.caps.buffer_from_host_ptr) return;
    // Check both packed projection kernels before exposing shared cache pages.
    std::unique_ptr<ggml_context, decltype(&ggml_free)> probe(ggml_init({ggml_tensor_overhead() * 12, nullptr, true}), ggml_free);
    require(bool(probe), "Cannot validate shared SSD backend");
    auto w = ggml_new_tensor_3d(probe.get(), p.type, p.in, p.out, 6);
    auto x = ggml_new_tensor_3d(probe.get(), GGML_TYPE_F32, p.in, 6, 1);
    auto ids = ggml_new_tensor_2d(probe.get(), GGML_TYPE_I32, 6, 1);
    auto output = ggml_mul_mat_id(probe.get(), w, x, ids);
    if (!ggml_backend_dev_supports_op(owner.device, output)) return;
    auto down = ggml_new_tensor_3d(probe.get(), p.type, p.out, p.in, 6);
    output = ggml_mul_mat_id(probe.get(), down, output, ids);
    if (!ggml_backend_dev_supports_op(owner.device, output)) return;
    auto &view = owner.shared;
    view.base = ascolto_expert_cache_storage(owner.cache, &view.bytes, &view.stride, &view.slots);
    size_t maximum = ggml_backend_buft_get_max_size(ggml_backend_dev_buffer_type(owner.device));
    if (view.slots > INT32_MAX || view.bytes > maximum) return; // Avoid Metal's overlapping multi-view path.
    view.buffer = ggml_backend_dev_buffer_from_host_ptr(owner.device, const_cast<void *>(view.base), view.bytes, view.bytes);
    if (!view.buffer) std::fprintf(stderr, "[ssd] shared mapping unavailable; using bounded compact copies\n");
#else
    (void)p;
#endif
}
void compute_impl(ggml_tensor *dst, int ith, void *userdata, bool fused) noexcept {
    if (ith != 0) return;
    auto &p = *static_cast<Projection *>(userdata);
    auto &owner = *p.owner;
    if (owner.failed.load()) return;
    try {
        std::lock_guard<std::mutex> lock(owner.mutex);
        if (owner.failed.load()) return;
        using Clock = std::chrono::steady_clock;
        const auto started = owner.profile ? Clock::now() : Clock::time_point{};
        double acquire_ms = 0, compute_ms = 0;
        uint64_t batches = 0;
        ascolto_expert_cache_stats_t before{};
        if (owner.profile) ascolto_expert_cache_stats(owner.cache, &before);
        auto x = dst->src[0], ids = dst->src[1];
        const int64_t k = ids->ne[0], rows = ids->ne[1];
        require(x->type == GGML_TYPE_F32 && ids->type == GGML_TYPE_I32 && x->ne[0] == p.in &&
                x->ne[2] == rows && x->ne[3] == 1 && (x->ne[1] == 1 || x->ne[1] == k) &&
                ids->ne[2] == 1 && ids->ne[3] == 1 && k > 0 && k <= max_selected &&
                ggml_is_contiguous(dst), "Invalid SSD matmul inputs");
        prepare_shared(owner, p);
        auto pair = owner.layers.at(p.layer);
        const int limit = owner.shared.buffer ? owner.batch_rows : 1;
        std::vector<float> input(size_t(p.in * k * limit)), output(size_t((fused ? p.in : p.out) * k * limit));
        auto plan_batch = [&](int64_t row) -> std::unique_ptr<RoutedBatch> {
            if (row >= rows) return nullptr;
            auto batch = std::make_unique<RoutedBatch>();
            batch->row = row;
            batch->selected.resize(size_t(k * limit));
            batch->unique.reserve(size_t(k * limit));
            for (; batch->count < limit && row + batch->count < rows; ++batch->count) {
                const size_t previous = batch->unique.size();
                for (int64_t slot = 0; slot < k; ++slot) {
                    int32_t id;
                    std::memcpy(&id, static_cast<char *>(ids->data) +
                        (row + batch->count) * ids->nb[1] + slot * ids->nb[0], sizeof(id));
                    require(id >= 0 && id < p.experts, "SSD router expert ID is out of bounds");
                    batch->selected[size_t(batch->count * k + slot)] = id;
                    if (std::find(batch->unique.begin(), batch->unique.end(), id) == batch->unique.end())
                        batch->unique.push_back(id);
                }
                if (owner.shared.buffer && batch->unique.size() >
                    std::min(owner.shared.slots, size_t(max_selected))) {
                    batch->unique.resize(previous);
                    break;
                }
            }
            require(batch->count > 0, "SSD cache cannot hold a routed row");
            batch->selected.resize(size_t(batch->count * k));
            const auto &experts = owner.shared.buffer ? batch->unique : batch->selected;
            batch->extents.resize(experts.size());
            for (size_t i = 0; i < experts.size(); ++i) for (int side = 0; side < 2; ++side) {
                batch->extents[i].offset[side] = pair[side]->offset + uint64_t(experts[i]) * pair[side]->bytes;
                batch->extents[i].bytes[side] = pair[side]->bytes;
            }
            return batch;
        };
        auto acquire_shared = [&](RoutedBatch &batch) {
            const auto read_start = owner.profile ? Clock::now() : Clock::time_point{};
            batch.leases = std::make_unique<Leases>(owner.cache, batch.unique.size());
            // Resolve and protect the complete known union before choosing any
            // victims. Nemotron has at most 128 experts in one layer.
            require(ascolto_expert_cache_acquire(owner.cache, batch.extents.data(), batch.unique.size(), nullptr, nullptr,
                                                 batch.leases->held.data()) == 0, "SSD expert read failed");
            if (owner.profile)
                batch.acquire_ms = std::chrono::duration<double, std::milli>(Clock::now() - read_start).count();
        };
        double prefetch_wait_ms = 0;
        auto current = plan_batch(0);
        while (current) {
            const int64_t row = current->row, batch = current->count;
            auto &product = (fused ? owner.fused_products : owner.products[p.side])[batch];
            if (!product || product->in != p.in || product->out != p.out || product->count != k || product->type != p.type)
                product = std::make_unique<Product>(p, k, batch, owner.threads, owner.device, fused, owner.shared, pair[0]->bytes);
            std::vector<int32_t> mapped(size_t(k * batch));
            if (!current->leases) {
                if (product->shared) acquire_shared(*current);
                else current->leases = std::make_unique<Leases>(owner.cache, size_t(k * batch));
            }
            acquire_ms += current->acquire_ms;
            auto &leases = *current->leases;
            for (int64_t local = 0; local < batch; ++local) {
                for (int64_t slot = 0; slot < k; ++slot) {
                    const size_t index = size_t(local * k + slot);
                    const char *source = static_cast<char *>(x->data) +
                        (x->ne[1] == 1 ? 0 : slot * x->nb[1]) + (row + local) * x->nb[2];
                    if (x->nb[0] == sizeof(float)) std::memcpy(input.data() + index * size_t(p.in), source, size_t(p.in) * sizeof(float));
                    else for (int64_t col = 0; col < p.in; ++col)
                        std::memcpy(&input[index * size_t(p.in) + size_t(col)], source + col * x->nb[0], sizeof(float));
                }
                if (product->shared) continue;
                const auto read_start = owner.profile ? Clock::now() : Clock::time_point{};
                require(ascolto_expert_cache_acquire(owner.cache, current->extents.data() + local * k, size_t(k), nullptr, nullptr,
                                                     leases.held.data() + local * k) == 0, "SSD expert read failed");
                if (owner.profile) acquire_ms += std::chrono::duration<double, std::milli>(Clock::now() - read_start).count();
                for (int64_t slot = 0; slot < k; ++slot) {
                    const size_t index = size_t(local * k + slot);
                    ggml_backend_tensor_set(product->weight, leases.held[index].data + (p.side ? pair[0]->bytes : 0),
                                            size_t(slot) * p.bytes, p.bytes);
                    if (fused) ggml_backend_tensor_set(product->down, leases.held[index].data + pair[0]->bytes,
                                                       size_t(slot) * pair[1]->bytes, pair[1]->bytes);
                }
            }
            if (product->shared) {
                for (size_t i = 0; i < mapped.size(); ++i) {
                    const auto found = std::find(current->unique.begin(), current->unique.end(), current->selected[i]);
                    require(found != current->unique.end(), "SSD routed expert is missing its lease");
                    mapped[i] = int32_t(leases.held[size_t(found - current->unique.begin())].slot);
                }
                ggml_backend_tensor_set(product->ids, mapped.data(), 0, mapped.size() * sizeof(int32_t));
            }
            ggml_backend_tensor_set(product->input, input.data(), 0, size_t(p.in * k * batch) * sizeof(float));
            auto next = plan_batch(row + batch);
            // Declared after next: the async future's destructor joins before
            // next/current leases unwind, even if compute/download throws.
            // Only cache reads run on this thread; no backend is shared with it.
            std::future<void> prefetch;
            if (owner.prefetch && product->shared && next) {
                size_t combined = current->unique.size();
                for (int32_t id : next->unique)
                    if (std::find(current->unique.begin(), current->unique.end(), id) == current->unique.end()) ++combined;
                if (combined <= owner.shared.slots) {
                    try {
                        prefetch = std::async(std::launch::async, [&acquire_shared, batch_next = next.get()] {
                            acquire_shared(*batch_next);
                        });
                        ++owner.stream_stats.prefetch_started;
                    } catch (...) {
                        // Native thread resource pressure keeps the serial path.
                        ++owner.stream_stats.prefetch_fallbacks;
                    }
                } else ++owner.stream_stats.prefetch_fallbacks;
            }
            const auto compute_start = owner.profile ? Clock::now() : Clock::time_point{};
            require(ggml_backend_graph_compute(product->backend, product->graph) == GGML_STATUS_SUCCESS,
                    "SSD compact graph failed");
            const size_t output_count = size_t((fused ? p.in : p.out) * k * batch);
            ggml_backend_tensor_get(product->output, output.data(), 0, output_count * sizeof(float));
            if (owner.profile) compute_ms += std::chrono::duration<double, std::milli>(Clock::now() - compute_start).count();
            for (size_t i = 0; i < output_count; ++i) require(std::isfinite(output[i]), "Non-finite SSD expert output");
            if (prefetch.valid()) {
                const auto wait_start = owner.profile ? Clock::now() : Clock::time_point{};
                try {
                    prefetch.get();
                    ++owner.stream_stats.prefetch_completed;
                } catch (...) {
                    ++owner.stream_stats.prefetch_failures;
                    throw;
                }
                if (owner.profile) prefetch_wait_ms += std::chrono::duration<double, std::milli>(Clock::now() - wait_start).count();
            }
            std::memcpy(static_cast<char *>(dst->data) + row * dst->nb[2], output.data(), output_count * sizeof(float));
            ++batches;
            // graph_compute is synchronous. Both banks' leases survive its
            // completion/download and the prefetch join; the next bank stays
            // pinned through its own following compute. No weight bank is added.
            current = std::move(next);
        }
        if (owner.profile) {
            ascolto_expert_cache_stats_t after{}; ascolto_expert_cache_stats(owner.cache, &after);
            std::fprintf(stderr, "[ssd-profile] layer=%d side=%d fused=%d rows=%lld batches=%llu shared=%d read_bytes=%llu hits=%llu misses=%llu acquire_ms=%.3f compute_ms=%.3f prefetch_wait_ms=%.3f prefetch_started=%llu prefetch_completed=%llu prefetch_fallbacks=%llu total_ms=%.3f\n",
                p.layer, p.side, fused, (long long)rows, (unsigned long long)batches, owner.shared.buffer != nullptr,
                (unsigned long long)(after.bytes_read - before.bytes_read), (unsigned long long)(after.hits - before.hits),
                (unsigned long long)(after.misses - before.misses), acquire_ms, compute_ms, prefetch_wait_ms,
                (unsigned long long)owner.stream_stats.prefetch_started, (unsigned long long)owner.stream_stats.prefetch_completed,
                (unsigned long long)owner.stream_stats.prefetch_fallbacks,
                std::chrono::duration<double, std::milli>(Clock::now() - started).count());
        }
    } catch (...) {
        owner.failed.store(true);
        if (dst->data) std::memset(dst->data, 0, ggml_nbytes(dst));
    }
}
void compute(ggml_tensor *dst, int ith, int, void *userdata) noexcept { compute_impl(dst, ith, userdata, false); }
void compute_ffn(ggml_tensor *dst, int ith, int, void *userdata) noexcept { compute_impl(dst, ith, userdata, true); }
}
ggml_tensor *ascolto_moe_matmul(ggml_context *ctx, ggml_tensor *w, ggml_tensor *x, ggml_tensor *ids) {
    if (w->buffer || w->data || !w->extra) return nullptr;
    auto p = static_cast<Projection *>(w->extra);
    require(p->magic == entry_magic && p->metadata == w, "Invalid SSD graph metadata");
    ggml_tensor *args[] = {x, ids};
    return ggml_custom_4d(ctx, GGML_TYPE_F32, p->out, ids->ne[0], ids->ne[1], 1, args, 2, compute, 1, p);
}
ggml_tensor *ascolto_moe_ffn(ggml_context *ctx, ggml_tensor *up, ggml_tensor *down, ggml_tensor *x, ggml_tensor *ids) {
    if (!up || !down || up->buffer || up->data || !up->extra || down->buffer || down->data || !down->extra) return nullptr;
    auto a = static_cast<Projection *>(up->extra), b = static_cast<Projection *>(down->extra);
    require(a->magic == entry_magic && b->magic == entry_magic && a->metadata == up && b->metadata == down &&
            a->owner == b->owner && a->layer == b->layer && a->side == 0 && b->side == 1, "Invalid fused SSD pair");
    ggml_tensor *args[] = {x, ids};
    return ggml_custom_4d(ctx, GGML_TYPE_F32, a->in, ids->ne[0], ids->ne[1], 1, args, 2, compute_ffn, 1, a);
}
bool ascolto_moe_aborted(void *p) { return p && static_cast<ascolto_streamed_moe *>(p)->failed.load(); }
void ascolto_moe_check(ascolto_streamed_moe *p) {
    if (p) require(!p->failed.load(), "SSD decoder failed; its context must be destroyed before retrying");
}
void ascolto_moe_stats(ascolto_streamed_moe *p, ascolto_expert_cache_stats_t *stats) {
    ascolto_expert_cache_stats(p->cache, stats);
}
bool ascolto_moe_uses_shared_cache(ascolto_streamed_moe *p) { return p->shared.buffer != nullptr; }
void ascolto_moe_loaded(ascolto_streamed_moe *p) {
    for (const auto &item : p->projections)
        require(item.second.metadata && !item.second.metadata->buffer && !item.second.metadata->data,
                "SSD loader did not import every expert as metadata only");
    ascolto_expert_cache_stats_t stats{}; ascolto_moe_stats(p, &stats);
    require(!stats.bytes_read && !stats.pinned_slots, "SSD loader read expert payload before decoding");
    std::fprintf(stderr, "[ssd] indexed=%zu loaded_expert_bytes=0 cache=%llu\n", p->projections.size(),
                 (unsigned long long)stats.capacity_bytes);
}

void ascolto_moe_stream_stats(ascolto_streamed_moe *p, ascolto_moe_stream_stats_t *stats) {
    if (!p || !stats) return;
    std::lock_guard<std::mutex> lock(p->mutex);
    *stats = p->stream_stats;
}
