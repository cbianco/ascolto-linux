"""Compile and exercise the actual generated host-packed SALM prefill method."""
import importlib.util
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "patch_nemotron_prefill", ROOT / "scripts/patch_nemotron_prefill.py")
PATCH = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCH)

# Keep the pinned insertion boundaries verbatim, without requiring downloaded
# model sources. The ordinary body is a sentinel for the unchanged fallback.
SOURCE = '''    // Prefill freshly admitted slots: prefix tokens for all, then all audio
    // frames, then suffix tokens with logits on each slot's last token.
    void prefill(
        std::vector<Slot*>& slots_in, const std::vector<const Encoded*>& encs_in,
        std::vector<Slot*>& active) {
        ggml_nvtx::range nvtx_r("salm.prefill");
#if SALM_FAST_PATH
        if (fast) {
            prefill_packed(slots_in, encs_in, active);
            return;
        }
#endif
        ++baseline_calls;
        (void)slots_in;
        (void)encs_in;
        (void)active;
    }
    void serve() {
        ggml_nvtx::range run_range("salm.run");
        llama_memory_clear(ctx, true);
        ++serve_calls;
    }
'''


FIXTURE = r'''
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#define SALM_FAST_PATH 0
#define REQUIRE(x) do { if (!(x)) throw std::runtime_error("check failed: " #x); } while (0)
using llama_token = int32_t;
using llama_pos = int32_t;
constexpr llama_token LLAMA_TOKEN_NULL = -1;
namespace ggml_nvtx { struct range { explicit range(const char*) {} }; }
struct Context {
    uint32_t ubatch;
    int copied_from = -1, copied_to = -1;
    size_t size_override = 0;
    bool zero_size = false;
    int capture_mode = 0, restore_mode = 0;
    int capture_calls = 0, restore_calls = 0, clear_calls = 0;
    uint64_t recurrent = 7;
    std::vector<llama_token> history, captured, restored;
    std::vector<llama_pos> history_pos;
};
uint32_t llama_n_ubatch(Context* ctx) { return ctx->ubatch; }
Context* llama_get_memory(Context* ctx) { return ctx; }
void llama_memory_seq_cp(Context* ctx, int from, int to, int, int) {
    ctx->copied_from = from;
    ctx->copied_to = to;
}
void llama_synchronize(Context*) {}
void llama_memory_clear(Context* ctx, bool) {
    ++ctx->clear_calls;
    ctx->history.clear();
    ctx->history_pos.clear();
    ctx->recurrent = 7;
}
size_t llama_state_seq_get_size(Context* ctx, int) {
    if (ctx->zero_size) return 0;
    return ctx->size_override ? ctx->size_override : 24 + ctx->history.size() * 8;
}
size_t llama_state_seq_get_data(Context* ctx, uint8_t* dst, size_t size, int) {
    ++ctx->capture_calls;
    if (ctx->capture_mode == 2) throw std::runtime_error("injected capture failure");
    if (ctx->capture_mode == 1) return size - 1;
    REQUIRE(size == 24 + ctx->history.size() * 8);
    const uint64_t header[] = {0x5354415445, ctx->recurrent, ctx->history.size()};
    std::memcpy(dst, header, sizeof(header));
    for (size_t i = 0; i < ctx->history.size(); ++i) {
        std::memcpy(dst + 24 + i * 8, &ctx->history[i], 4);
        std::memcpy(dst + 28 + i * 8, &ctx->history_pos[i], 4);
    }
    ctx->captured = ctx->history;
    return size;
}
size_t llama_state_seq_set_data(Context* ctx, const uint8_t* src, size_t size, int) {
    ++ctx->restore_calls;
    if (ctx->restore_mode) {
        ctx->history.assign(1, LLAMA_TOKEN_NULL);  // model a partial native mutation
        if (ctx->restore_mode == 2) throw std::runtime_error("injected restore failure");
        return size - 1;
    }
    REQUIRE(size >= 24);
    uint64_t header[3];
    std::memcpy(header, src, sizeof(header));
    REQUIRE(header[0] == 0x5354415445 && size == 24 + header[2] * 8);
    ctx->recurrent = header[1];
    ctx->history.resize(header[2]);
    ctx->history_pos.resize(header[2]);
    for (size_t i = 0; i < ctx->history.size(); ++i) {
        std::memcpy(&ctx->history[i], src + 24 + i * 8, 4);
        std::memcpy(&ctx->history_pos[i], src + 28 + i * 8, 4);
    }
    ctx->restored = ctx->history;
    return size;
}
struct llama_batch {
    int32_t n_tokens = -123;
    llama_token* token = nullptr;
    float* embd = nullptr;
    llama_pos* pos = nullptr;
    int32_t* n_seq_id = nullptr;
    int32_t** seq_id = nullptr;
    int8_t* logits = nullptr;
    float* embd_input = nullptr;
    void* embd_input_device = nullptr;
};
struct Prompt { std::vector<llama_token> prefix, suffix; };
struct CacheEntry { int seq = 23; std::vector<llama_token> tokens; };
struct Slot {
    Prompt* prompt = nullptr;
    std::shared_ptr<CacheEntry> cached;
    int seq_id = 5;
    llama_pos pos = 11, dft_offset = 17;
    llama_token last = -999;
    std::vector<llama_token> resume, pending{-9};
};
struct Encoded { int frames = 0; std::vector<float> proj; };
struct Flagged { Slot* slot; int32_t batch_idx; };
struct Row {
    llama_token token;
    llama_pos pos;
    int seq, n_seq;
    bool logits;
    std::vector<float> embedding;
};
struct Engine {
    struct Config { int n_batch, max_sequences = 1; } cfg;
    Context context;
    Context* ctx = &context;
    void* ssd = this;
    int n_embd = 3, baseline_calls = 0, sample_calls = 0, serve_calls = 0;
    int throw_decode = 0;
    bool throw_sample = false;
    std::vector<llama_token> tokens;
    std::vector<llama_pos> positions;
    std::vector<int32_t> n_seq, seq_storage;
    std::vector<int32_t*> seq_ptrs;
    std::vector<int8_t> logits;
    std::vector<float> embeddings;
    llama_batch tok_batch, emb_batch;
    std::vector<std::vector<Row>> calls;
    explicit Engine(int batch, uint32_t ubatch)
        : cfg{batch, 1}, context{}, tokens(std::max(1, batch)),
          positions(tokens.size()), n_seq(tokens.size()), seq_storage(tokens.size()),
          seq_ptrs(tokens.size()), logits(tokens.size()), embeddings(tokens.size() * 3, -77) {
        context.ubatch = ubatch;
        for (size_t i = 0; i < seq_ptrs.size(); ++i) seq_ptrs[i] = &seq_storage[i];
        tok_batch.token = tokens.data();
        tok_batch.pos = positions.data();
        tok_batch.n_seq_id = n_seq.data();
        tok_batch.seq_id = seq_ptrs.data();
        tok_batch.logits = logits.data();
        emb_batch.embd = embeddings.data();
    }
    void decode(llama_batch& b) {
        REQUIRE(b.n_tokens > 0 && b.n_tokens <= cfg.n_batch);
        REQUIRE(static_cast<uint32_t>(b.n_tokens) <= context.ubatch);
        REQUIRE(b.token == tok_batch.token && b.embd == nullptr);
        REQUIRE(b.embd_input == emb_batch.embd && b.embd_input_device == nullptr);
        std::vector<Row> rows;
        for (int i = 0; i < b.n_tokens; ++i) {
            const float* v = b.embd_input + static_cast<size_t>(i) * n_embd;
            rows.push_back({b.token[i], b.pos[i], b.seq_id[i][0], b.n_seq_id[i],
                            b.logits[i] != 0, std::vector<float>(v, v + n_embd)});
        }
        calls.push_back(std::move(rows));
        if (throw_decode == static_cast<int>(calls.size()))
            throw std::runtime_error("injected decode failure");
        for (const auto& row : calls.back()) {
            context.history.push_back(row.token);
            context.history_pos.push_back(row.pos);
            context.recurrent = context.recurrent * 1099511628211ull ^ uint32_t(row.token);
            for (float value : row.embedding)
                context.recurrent = context.recurrent * 1099511628211ull ^ uint32_t(value * 4);
        }
    }
    void sample(const std::vector<Flagged>& flagged) {
        ++sample_calls;
        REQUIRE(flagged.size() == 1);
        const auto& f = flagged.front();
        REQUIRE(f.batch_idx >= 0 && static_cast<size_t>(f.batch_idx) < calls.back().size());
        REQUIRE(calls.back()[f.batch_idx].logits);
        if (throw_sample) throw std::runtime_error("injected sample failure");
        f.slot->last = 10000 + calls.back()[f.batch_idx].pos;
    }
    void check_unmixed() const {
        REQUIRE(tok_batch.embd_input == nullptr && tok_batch.embd_input_device == nullptr);
        REQUIRE(tok_batch.embd == nullptr && tok_batch.n_tokens == -123);
        REQUIRE(emb_batch.embd_input == nullptr && emb_batch.embd_input_device == nullptr);
    }
    // The production patch is inserted here, including its actual entry gate.
@METHODS@
};

struct Request {
    Prompt prompt;
    Slot slot;
    Encoded encoded;
    Request(int prefix, int audio, int suffix, int resume, int cached = 0) {
        slot.prompt = &prompt;
        for (int i = 0; i < prefix; ++i) prompt.prefix.push_back(100 + i);
        for (int i = 0; i < suffix; ++i) prompt.suffix.push_back(200 + i);
        for (int i = 0; i < resume; ++i) slot.resume.push_back(300 + i);
        encoded.frames = audio;
        for (int i = 0; i < audio * 3; ++i) encoded.proj.push_back(i + 0.25f);
        if (cached) {
            slot.cached = std::make_shared<CacheEntry>();
            slot.cached->tokens.assign(prompt.prefix.begin(), prompt.prefix.begin() + cached);
        }
    }
};
void run_case(int batch, int ubatch, int prefix, int audio, int suffix, int resume, int cached = 0) {
    Engine engine(batch, ubatch);
    Request r(prefix, audio, suffix, resume, cached);
    const int start = cached ? cached : r.slot.pos;
    std::vector<llama_token> expected(r.prompt.prefix.begin() + cached, r.prompt.prefix.end());
    expected.insert(expected.end(), audio, LLAMA_TOKEN_NULL);
    expected.insert(expected.end(), r.prompt.suffix.begin(), r.prompt.suffix.end());
    expected.insert(expected.end(), r.slot.resume.begin(), r.slot.resume.end());
    engine.prefill_ssd_packed_host(&r.slot, r.encoded);
    size_t seen = 0, output_rows = 0;
    for (const auto& call : engine.calls) for (const auto& row : call) {
        REQUIRE(row.token == expected.at(seen));
        REQUIRE(row.pos == start + static_cast<int>(seen));
        REQUIRE(row.seq == r.slot.seq_id && row.n_seq == 1);
        REQUIRE(row.logits == (seen + 1 == expected.size()));
        output_rows += row.logits;
        for (int j = 0; j < 3; ++j) {
            if (row.token == LLAMA_TOKEN_NULL) {
                const size_t frame = seen - static_cast<size_t>(prefix - cached);
                REQUIRE(row.embedding[j] == r.encoded.proj.at(frame * 3 + j));
            } else {
                REQUIRE(row.embedding[j] == 0.0f);
            }
        }
        ++seen;
    }
    REQUIRE(seen == expected.size() && output_rows == 1 && engine.sample_calls == 1);
    const size_t cap = static_cast<size_t>(std::min(batch, ubatch));
    REQUIRE(engine.calls.size() == (expected.size() + cap - 1) / cap);
    REQUIRE(r.slot.pos == start + static_cast<int>(expected.size()));
    REQUIRE(r.slot.last == 10000 + r.slot.pos - 1);
    REQUIRE(r.slot.pending == std::vector<llama_token>{r.slot.last});
    REQUIRE(r.slot.resume.empty() && !r.slot.cached && r.slot.dft_offset == 0);
    REQUIRE(engine.context.copied_from == (cached ? 23 : -1));
    REQUIRE(engine.context.copied_to == (cached ? r.slot.seq_id : -1));
    engine.check_unmixed();
}
void boundaries() {
    int cases = 0;
    for (int cap : {1, 2, 3, 7, 31, 512}) {
        for (int prefix : {0, 1, 2, cap - 1, cap, cap + 1})
            for (int audio : {0, 1, 2, cap - 1, cap, cap + 1})
                for (int suffix : {1, 2, cap + 1})
                    for (int resume : {0, 1, cap + 1}) {
                        run_case(cap, cap, prefix, audio, suffix, resume);
                        ++cases;
                    }
    }
    // The physical ubatch and caller allocation impose independent limits.
    run_case(8, 3, 2, 7, 3, 4);
    run_case(3, 8, 2, 7, 3, 4);
    run_case(7, 7, 5, 9, 2, 3, 2);
    run_case(7, 7, 5, 9, 2, 0, 5);
    run_case(7, 7, 0, 2, 0, 2);
    std::cout << "packing cases=" << cases + 5 << '\n';
}
void exceptions() {
    for (int fail : {1, 2, 3, 4}) {
        Engine engine(3, 3);
        Request r(2, 4, 2, 1);
        if (fail <= 3) engine.throw_decode = fail;
        else engine.throw_sample = true;
        bool threw = false;
        try { engine.prefill_ssd_packed_host(&r.slot, r.encoded); }
        catch (const std::runtime_error&) { threw = true; }
        REQUIRE(threw);
        engine.check_unmixed();
        REQUIRE(r.slot.pending == std::vector<llama_token>{-9});
        REQUIRE(!r.slot.resume.empty());
        // A later fallback call must still see the normal token batch fields.
        setenv("ASCOLTO_NEMOTRON_SSD_PACKED_PREFILL", "0", 1);
        std::vector<Slot*> slots{&r.slot}, active;
        std::vector<const Encoded*> encs{&r.encoded};
        engine.prefill(slots, encs, active);
        REQUIRE(engine.baseline_calls == 1);
        engine.check_unmixed();
    }
}
void invalid() {
    for (int mode = 0; mode < 7; ++mode) {
        Engine engine(mode == 5 ? 0 : 3, mode == 6 ? 0 : 3);
        Request r(2, 3, 1, 0);
        if (mode == 0) r.encoded.frames = -1;
        if (mode == 1) r.encoded.proj.pop_back();
        if (mode == 2) engine.n_embd = 0;
        if (mode == 3) {
            r.slot.cached = std::make_shared<CacheEntry>();
            r.slot.cached->tokens.resize(3);
        }
        if (mode == 4) r.prompt.suffix.clear();
        bool threw = false;
        try { engine.prefill_ssd_packed_host(&r.slot, r.encoded); }
        catch (const std::runtime_error&) { threw = true; }
        REQUIRE(threw && engine.calls.empty() && r.slot.pos == 11);
        engine.check_unmixed();
    }
}
void gate() {
    const char* values[] = {nullptr, "", "0", "false", "true", "01", "1", "2"};
    for (const char* value : values) {
        if (value) setenv("ASCOLTO_NEMOTRON_SSD_PACKED_PREFILL", value, 1);
        else unsetenv("ASCOLTO_NEMOTRON_SSD_PACKED_PREFILL");
        Engine engine(8, 8);
        Request r(1, 1, 1, 0);
        std::vector<Slot*> slots{&r.slot}, active;
        std::vector<const Encoded*> encs{&r.encoded};
        engine.prefill(slots, encs, active);
        const bool enabled = !value || std::strcmp(value, "1") == 0;
        REQUIRE(engine.baseline_calls == (enabled ? 0 : 1));
        REQUIRE(engine.calls.size() == (enabled ? 1u : 0u));
        engine.check_unmixed();
    }
    setenv("ASCOLTO_NEMOTRON_SSD_PACKED_PREFILL", "1", 1);
    for (int mode = 0; mode < 5; ++mode) {
        Engine engine(8, 8);
        Request r(1, 1, 1, 0);
        std::vector<Slot*> slots{&r.slot}, active;
        std::vector<const Encoded*> encs{&r.encoded};
        if (mode == 0) engine.ssd = nullptr;
        if (mode == 1) engine.cfg.max_sequences = 2;
        if (mode == 2) slots.push_back(&r.slot);
        if (mode == 3) encs.clear();
        if (mode == 4) { slots.clear(); encs.clear(); }
        engine.prefill(slots, encs, active);
        REQUIRE(engine.baseline_calls == 1 && engine.calls.empty());
        engine.check_unmixed();
    }
}
void invoke(Engine& engine, Request& request) {
    request.slot.pos = 0;
    std::vector<Slot*> slots{&request.slot}, active;
    std::vector<const Encoded*> encs{&request.encoded};
    engine.prefill(slots, encs, active);
}
void cache_roundtrip() {
    setenv("ASCOLTO_NEMOTRON_SSD_PACKED_PREFILL", "1", 1);
    setenv("ASCOLTO_NEMOTRON_SSD_PREFIX_CACHE", "1", 1);
    setenv("ASCOLTO_NEMOTRON_SSD_PROFILE", "1", 1);
    Engine engine(8, 8);
    Request first(3, 2, 1, 0);
    engine.serve();
    invoke(engine, first);
    REQUIRE(engine.calls.size() == 2 && engine.calls.front().size() == 3);
    REQUIRE(engine.context.capture_calls == 1 && engine.context.restore_calls == 0);
    REQUIRE(engine.context.captured == first.prompt.prefix);
    REQUIRE(engine.ssd_prefix_tokens == first.prompt.prefix);
    const auto state = engine.ssd_prefix_state;
    const auto first_recurrent = engine.context.recurrent;
    REQUIRE(!state.empty() && state.size() <= Engine::ssd_prefix_state_limit);

    // Clear the previous utterance exactly as serve() does, then use another
    // Prompt object and different audio. Only token-vector equality permits reuse.
    engine.serve();
    REQUIRE(engine.context.history.empty() && engine.context.recurrent == 7);
    Request second(3, 3, 1, 0);
    for (float& value : second.encoded.proj) value += 17;
    invoke(engine, second);
    REQUIRE(engine.context.restore_calls == 1 && engine.context.capture_calls == 1);
    REQUIRE(engine.context.restored == second.prompt.prefix);
    REQUIRE(engine.calls.size() == 3 && engine.calls.back().size() == 4);
    REQUIRE(engine.calls.back().front().token == LLAMA_TOKEN_NULL);
    REQUIRE(engine.calls.back().front().pos == 3);
    REQUIRE(engine.context.history.size() == 7);
    REQUIRE(engine.context.history_pos == std::vector<llama_pos>({0, 1, 2, 3, 4, 5, 6}));
    REQUIRE(engine.context.recurrent != first_recurrent);
    REQUIRE(engine.ssd_prefix_state == state);  // no audio may enter the snapshot
    REQUIRE(second.slot.pos == 7 && second.slot.pending.size() == 1);
    engine.check_unmixed();
    // A fresh full decode of the second utterance must reach exactly the same
    // attention rows and recurrent accumulator as restoring the prefix first.
    setenv("ASCOLTO_NEMOTRON_SSD_PREFIX_CACHE", "0", 1);
    Engine control(8, 8);
    Request control_request(3, 3, 1, 0);
    for (float& value : control_request.encoded.proj) value += 17;
    control.serve();
    invoke(control, control_request);
    REQUIRE(control.context.history == engine.context.history);
    REQUIRE(control.context.history_pos == engine.context.history_pos);
    REQUIRE(control.context.recurrent == engine.context.recurrent);
    setenv("ASCOLTO_NEMOTRON_SSD_PREFIX_CACHE", "1", 1);

    // A different prefix replaces the single entry, even at the same length.
    engine.serve();
    Request changed(3, 1, 1, 0);
    changed.prompt.prefix[1] = 900;
    invoke(engine, changed);
    REQUIRE(engine.context.capture_calls == 2 && engine.context.restore_calls == 1);
    REQUIRE(engine.ssd_prefix_tokens == changed.prompt.prefix);
    REQUIRE(engine.ssd_prefix_state != state);

    // Disabling the cache releases both payload and key allocations.
    setenv("ASCOLTO_NEMOTRON_SSD_PREFIX_CACHE", "0", 1);
    engine.serve();
    Request disabled(3, 2, 1, 0);
    const size_t before = engine.calls.size();
    invoke(engine, disabled);
    REQUIRE(engine.calls.size() == before + 1);
    REQUIRE(engine.context.restore_calls == 1 && engine.context.capture_calls == 2);
    REQUIRE(engine.ssd_prefix_state.empty() && engine.ssd_prefix_state.capacity() == 0);
    REQUIRE(engine.ssd_prefix_tokens.empty() && engine.ssd_prefix_tokens.capacity() == 0);
}
void cache_budget_and_capture_errors() {
    setenv("ASCOLTO_NEMOTRON_SSD_PACKED_PREFILL", "1", 1);
    setenv("ASCOLTO_NEMOTRON_SSD_PREFIX_CACHE", "1", 1);
    REQUIRE(Engine::ssd_prefix_state_limit == size_t(256) * 1024 * 1024);
    for (int mode = 0; mode < 4; ++mode) {
        Engine engine(8, 8);
        Request request(3, 2, 1, 0);
        if (mode == 0) engine.context.size_override = Engine::ssd_prefix_state_limit + 1;
        if (mode == 1) engine.context.zero_size = true;
        if (mode == 2) engine.context.capture_mode = 1;
        if (mode == 3) engine.context.capture_mode = 2;
        engine.serve();
        invoke(engine, request);
        REQUIRE(engine.ssd_prefix_state.empty() && engine.ssd_prefix_tokens.empty());
        REQUIRE(!engine.ssd_prefix_restore_failed && engine.calls.size() == 2);
        REQUIRE(request.slot.pending == std::vector<llama_token>{request.slot.last});
        REQUIRE(engine.context.capture_calls == (mode <= 1 ? 0 : 1));
        engine.serve();  // failed capture must not poison a valid decoder
        REQUIRE(engine.serve_calls == 2);
        engine.check_unmixed();
    }
    Engine empty(8, 8);
    Request no_prefix(0, 2, 1, 0);
    empty.serve();
    invoke(empty, no_prefix);
    REQUIRE(empty.calls.size() == 1 && empty.context.capture_calls == 0);
}
void cache_restore_errors() {
    setenv("ASCOLTO_NEMOTRON_SSD_PACKED_PREFILL", "1", 1);
    setenv("ASCOLTO_NEMOTRON_SSD_PREFIX_CACHE", "1", 1);
    for (int mode : {1, 2}) {
        Engine engine(8, 8);
        Request first(3, 2, 1, 0);
        engine.serve();
        invoke(engine, first);
        REQUIRE(!engine.ssd_prefix_state.empty());
        engine.serve();
        engine.context.restore_mode = mode;
        Request second(3, 4, 1, 0);
        const auto decodes = engine.calls.size();
        bool threw = false;
        try { invoke(engine, second); }
        catch (const std::runtime_error&) { threw = true; }
        REQUIRE(threw && engine.context.restore_calls == 1);
        REQUIRE(engine.ssd_prefix_restore_failed && engine.calls.size() == decodes);
        REQUIRE(engine.ssd_prefix_state.empty() && engine.ssd_prefix_tokens.empty());
        REQUIRE(second.slot.pending == std::vector<llama_token>{-9});
        engine.check_unmixed();
        const int clears = engine.context.clear_calls;
        threw = false;
        try { engine.serve(); }
        catch (const std::runtime_error&) { threw = true; }
        REQUIRE(threw && engine.context.clear_calls == clears);
        // Disabling the packed path cannot revive partially restored memory.
        setenv("ASCOLTO_NEMOTRON_SSD_PACKED_PREFILL", "0", 1);
        threw = false;
        try { invoke(engine, second); }
        catch (const std::runtime_error&) { threw = true; }
        REQUIRE(threw && engine.baseline_calls == 0);
        setenv("ASCOLTO_NEMOTRON_SSD_PACKED_PREFILL", "1", 1);
    }
}
void cache_defaults() {
    // Both optimizations run without environment overrides. Explicit 0 and
    // malformed overrides disable the cache conservatively; exact 1 enables it.
    unsetenv("ASCOLTO_NEMOTRON_SSD_PACKED_PREFILL");
    const char* values[] = {nullptr, "", "0", "false", "true", "01", "1", "2"};
    for (const char* value : values) {
        if (value) setenv("ASCOLTO_NEMOTRON_SSD_PREFIX_CACHE", value, 1);
        else unsetenv("ASCOLTO_NEMOTRON_SSD_PREFIX_CACHE");
        Engine engine(8, 8);
        Request first(3, 2, 1, 0);
        engine.serve();
        invoke(engine, first);
        const bool enabled = !value || std::strcmp(value, "1") == 0;
        REQUIRE(engine.baseline_calls == 0);
        REQUIRE(engine.calls.size() == (enabled ? 2u : 1u));
        REQUIRE(engine.context.capture_calls == (enabled ? 1 : 0));
        REQUIRE(engine.ssd_prefix_state.empty() == !enabled);
        engine.serve();
        Request second(3, 3, 1, 0);
        invoke(engine, second);
        REQUIRE(engine.context.restore_calls == (enabled ? 1 : 0));
        REQUIRE(engine.context.capture_calls == (enabled ? 1 : 0));
        engine.check_unmixed();
    }
}
int main(int argc, char** argv) {
    try {
        REQUIRE(argc == 2);
        setenv("ASCOLTO_NEMOTRON_SSD_PREFIX_CACHE", "0", 1);
        unsetenv("ASCOLTO_NEMOTRON_SSD_PROFILE");
        const std::string mode = argv[1];
        if (mode == "boundaries") boundaries();
        else if (mode == "exceptions") exceptions();
        else if (mode == "invalid") invalid();
        else if (mode == "gate") gate();
        else if (mode == "cache-roundtrip") cache_roundtrip();
        else if (mode == "cache-budget") cache_budget_and_capture_errors();
        else if (mode == "cache-restore-errors") cache_restore_errors();
        else if (mode == "cache-defaults") cache_defaults();
        else throw std::runtime_error("unknown fixture mode");
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
'''


class PinnedPatchGuards(unittest.TestCase):
    def test_baseline_body_and_unique_insertion(self):
        patched = PATCH.patch_ssd_packed_prefill(SOURCE)
        self.assertEqual(patched.count("void prefill_ssd_packed_host("), 1)
        self.assertEqual(patched.count("ASCOLTO_NEMOTRON_SSD_PACKED_PREFILL"), 1)
        baseline = SOURCE[SOURCE.index("        ++baseline_calls;"):SOURCE.index("    void serve()")]
        self.assertIn(baseline, patched)

    def test_reject_changed_or_ambiguous_pinned_boundaries(self):
        changed = (
            "", SOURCE + SOURCE,
            SOURCE.replace("    // Prefill freshly admitted slots:", "    // Changed prefix:"),
            SOURCE.replace('nvtx_r("salm.prefill")', 'nvtx_r("changed")'),
            SOURCE.replace("#if SALM_FAST_PATH", "#if CHANGED_FAST_PATH"),
            SOURCE.replace('run_range("salm.run")', 'run_range("changed")'),
        )
        for content in changed:
            with self.subTest(content=content[:60]), self.assertRaises(SystemExit):
                PATCH.patch_ssd_packed_prefill(content)

    def test_reject_repeated_application(self):
        with self.assertRaises(SystemExit):
            PATCH.patch_ssd_packed_prefill(PATCH.patch_ssd_packed_prefill(SOURCE))


class ExtractedCppPrefill(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        if not compiler or not shutil.which(compiler[0]):
            raise unittest.SkipTest("A C++17 compiler is required for the extracted prefill fixture")
        cls.temporary = tempfile.TemporaryDirectory(prefix="ascolto-prefill-test-")
        cls.addClassCleanup(cls.temporary.cleanup)
        folder = Path(cls.temporary.name)
        source, cls.binary = folder / "prefill.cpp", folder / "prefill"
        methods = PATCH.patch_ssd_packed_prefill(SOURCE)
        source.write_text(FIXTURE.replace("@METHODS@", methods))
        compiled = subprocess.run(
            [*compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror", "-O1",
             str(source), "-o", str(cls.binary)], text=True, capture_output=True, timeout=60)
        if compiled.returncode:
            raise AssertionError("Extracted production prefill failed to compile:\n" + compiled.stderr)

    def run_fixture(self, mode):
        result = subprocess.run([str(self.binary), mode], text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout + result.stderr

    def test_actual_cpp_boundaries_embeddings_positions_logits_and_resume(self):
        self.assertIn("packing cases=1949", self.run_fixture("boundaries"))

    def test_decode_and_sample_exceptions_preserve_unmixed_batch_fields(self):
        self.run_fixture("exceptions")

    def test_invalid_inputs_fail_before_decode_or_position_changes(self):
        self.run_fixture("invalid")

    def test_packed_gate_defaults_ablation_and_single_sequence_restrictions(self):
        self.run_fixture("gate")

    def test_prefix_snapshot_restores_full_state_for_new_audio_and_replaces_key(self):
        output = self.run_fixture("cache-roundtrip")
        for action in ("cold-prefill", "capture", "restore"):
            self.assertIn("action=" + action, output)
        self.assertIn("bytes=48", output)

    def test_prefix_snapshot_bounds_and_capture_failures_preserve_decoder(self):
        self.run_fixture("cache-budget")

    def test_partial_and_throwing_restore_poison_context_before_reuse(self):
        self.run_fixture("cache-restore-errors")

    def test_prefix_cache_default_and_explicit_ablation_overrides(self):
        self.run_fixture("cache-defaults")


if __name__ == "__main__":
    unittest.main()
