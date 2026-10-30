#!/usr/bin/env python3
"""Add host-packed prefill and a bounded prefix snapshot to private SALM."""


def patch_ssd_packed_prefill(content: str) -> str:
    """Keep upstream prefill available through private SSD ablation flags."""
    anchor = "    // Prefill freshly admitted slots: prefix tokens for all, then all audio\n"
    entry = '''        ggml_nvtx::range nvtx_r("salm.prefill");
#if SALM_FAST_PATH'''
    serve = '        ggml_nvtx::range run_range("salm.run");'
    if content.count(anchor) != 1 or content.count(entry) != 1 or content.count(serve) != 1:
        raise SystemExit("Pinned SALM host prefill context changed")
    if "void prefill_ssd_packed_host(" in content:
        raise SystemExit("SALM host-packed prefill was already applied")
    helper = r'''    // A single engine-local, host-owned prefix snapshot. It contains only the
    // immutable text prefix's full attention and recurrent memory, never audio.
    std::vector<llama_token> ssd_prefix_tokens;
    std::vector<uint8_t> ssd_prefix_state;
    bool ssd_prefix_restore_failed = false;
    static constexpr size_t ssd_prefix_state_limit = size_t(256) * 1024 * 1024;

    void clear_ssd_prefix_snapshot() {
        std::vector<llama_token>().swap(ssd_prefix_tokens);
        std::vector<uint8_t>().swap(ssd_prefix_state);
    }

    void check_ssd_prefix_snapshot() const {
        if (ssd_prefix_restore_failed)
            throw std::runtime_error("SSD prefix restore failed; its context must be destroyed before retrying");
    }

    // SSD path: text lookup and supplied audio share one causal
    // pass, avoiding separate whole-model sweeps at the modality boundaries.
    // Use the existing host embedding allocation and ordinary llama_decode:
    // this requires neither CUDA staging nor the fast serving graph.
    void prefill_ssd_packed_host(Slot* s, const Encoded& e) {
        check_ssd_prefix_snapshot();
        const Prompt& p = *s->prompt;
        const size_t cached = s->cached ? s->cached->tokens.size() : 0;
        if (cached > p.prefix.size() || e.frames < 0 || n_embd <= 0 ||
            static_cast<size_t>(e.frames) > e.proj.size() / static_cast<size_t>(n_embd))
            throw std::runtime_error("Invalid SSD packed prefill inputs");
        if (p.suffix.empty() && s->resume.empty())
            throw std::runtime_error("SSD packed prefill requires a final text row");
        const size_t capacity = std::min<size_t>(
            static_cast<size_t>(cfg.n_batch), llama_n_ubatch(ctx));
        if (!capacity)
            throw std::runtime_error("SSD packed prefill has no batch capacity");
        // SSD currently disables upstream prefix caching and speculation. Retain the
        // target's prefix-copy semantics if a cached prefix is supplied.
        s->dft_offset = 0;
        if (s->cached) {
            llama_memory_seq_cp(llama_get_memory(ctx), s->cached->seq, s->seq_id, -1, -1);
            s->pos = static_cast<llama_pos>(cached);
        }
        const char* cache_option = std::getenv("ASCOLTO_NEMOTRON_SSD_PREFIX_CACHE");
        const bool cache_prefix = ssd && cfg.max_sequences == 1 && !s->cached && s->pos == 0 &&
            !p.prefix.empty() && (!cache_option || std::strcmp(cache_option, "1") == 0);
        const bool profile = ssd && std::getenv("ASCOLTO_NEMOTRON_SSD_PROFILE");
        auto report_prefix = [&](const char* action, size_t bytes,
                                 std::chrono::steady_clock::time_point started) {
            if (profile)
                std::fprintf(stderr, "[ssd-prefix] action=%s tokens=%zu bytes=%zu seconds=%.6f\n",
                    action, p.prefix.size(), bytes,
                    std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count());
        };
        if (!cache_prefix || ssd_prefix_tokens != p.prefix)
            clear_ssd_prefix_snapshot();
        size_t prefix_ready = cached;
        bool restored = false;
        if (cache_prefix && !ssd_prefix_state.empty()) {
            const auto started = std::chrono::steady_clock::now();
            try {
                // Flags 0 save/restore both parts of the hybrid memory. The
                // partial-only API would omit attention and is unsuitable here.
                const size_t read = llama_state_seq_set_data(
                    ctx, ssd_prefix_state.data(), ssd_prefix_state.size(), s->seq_id);
                if (read != ssd_prefix_state.size())
                    throw std::runtime_error("Incomplete SSD prefix state restore");
                prefix_ready = p.prefix.size();
                s->pos = static_cast<llama_pos>(prefix_ready);
                restored = true;
                report_prefix("restore", read, started);
            } catch (...) {
                // Restore can mutate attention before failing on recurrent
                // state. Never continue or reuse a partially restored context.
                ssd_prefix_restore_failed = true;
                clear_ssd_prefix_snapshot();
                throw;
            }
        }
        size_t n = 0;
        std::vector<Flagged> flagged;
        auto flush = [&]() {
            // Borrow only the arrays: mixed-input fields never escape this
            // call, including when decode or sampling throws.
            llama_batch packed = tok_batch;
            packed.n_tokens = static_cast<int32_t>(n);
            packed.embd_input = emb_batch.embd;
            packed.embd_input_device = nullptr;
            decode(packed);
            if (!flagged.empty())
                sample(flagged);
            flagged.clear();
            n = 0;
        };
        auto add_rows = [&](size_t count, const llama_token* tokens,
                            const float* audio, bool output) {
            for (size_t done = 0; done < count;) {
                const size_t take = std::min(count - done, capacity - n);
                float* destination = emb_batch.embd + n * static_cast<size_t>(n_embd);
                const size_t elements = take * static_cast<size_t>(n_embd);
                if (audio)
                    std::memcpy(destination, audio + done * static_cast<size_t>(n_embd),
                                elements * sizeof(float));
                else
                    std::fill_n(destination, elements, 0.0f);
                for (size_t k = 0; k < take; ++k, ++n) {
                    tok_batch.token[n] = tokens ? tokens[done + k] : LLAMA_TOKEN_NULL;
                    tok_batch.pos[n] = s->pos++;
                    tok_batch.n_seq_id[n] = 1;
                    tok_batch.seq_id[n][0] = s->seq_id;
                    tok_batch.logits[n] = output && done + k + 1 == count;
                    if (tok_batch.logits[n])
                        flagged.push_back({s, static_cast<int32_t>(n)});
                }
                done += take;
                if (n == capacity)
                    flush();
            }
        };
        const auto prefix_started = std::chrono::steady_clock::now();
        if (p.prefix.size() != prefix_ready)
            add_rows(p.prefix.size() - prefix_ready, p.prefix.data() + prefix_ready, nullptr, false);
        if (cache_prefix && !restored) {
            // Mamba retains only its latest state: capture at the exact prefix
            // boundary, before any audio enters the decoder. A cold entry pays
            // this extra pass; later turns restore it and pack only the rest.
            if (n)
                flush();
            llama_synchronize(ctx);
            report_prefix("cold-prefill", 0, prefix_started);
            const auto started = std::chrono::steady_clock::now();
            size_t bytes = 0;
            try {
                bytes = llama_state_seq_get_size(ctx, s->seq_id);
                if (bytes && bytes <= ssd_prefix_state_limit) {
                    std::vector<llama_token> key(p.prefix);
                    std::vector<uint8_t> state(bytes);
                    if (llama_state_seq_get_data(ctx, state.data(), state.size(), s->seq_id) != bytes)
                        throw std::runtime_error("Incomplete SSD prefix state capture");
                    ssd_prefix_tokens.swap(key);
                    ssd_prefix_state.swap(state);
                    report_prefix("capture", bytes, started);
                } else {
                    report_prefix("skip-size", bytes, started);
                }
            } catch (...) {
                // Capture does not mutate decoder memory. An optional cache
                // failure must not discard the already-computed valid prefix.
                clear_ssd_prefix_snapshot();
                report_prefix("capture-failed", bytes, started);
            }
        }
        add_rows(static_cast<size_t>(e.frames), nullptr, e.proj.data(), false);
        add_rows(p.suffix.size(), p.suffix.data(), nullptr, s->resume.empty());
        if (!s->resume.empty())
            add_rows(s->resume.size(), s->resume.data(), nullptr, true);
        if (n)
            flush();
        s->pending.assign(1, s->last);
        s->cached.reset();
        s->resume.clear();
    }

'''
    gate = '''        ggml_nvtx::range nvtx_r("salm.prefill");
        (void)active;
        check_ssd_prefix_snapshot();
        const char* packed_ssd = std::getenv("ASCOLTO_NEMOTRON_SSD_PACKED_PREFILL");
        if (ssd && cfg.max_sequences == 1 && slots_in.size() == 1 && encs_in.size() == 1 &&
            (!packed_ssd || std::strcmp(packed_ssd, "1") == 0)) {
            prefill_ssd_packed_host(slots_in.front(), *encs_in.front());
            return;
        }
#if SALM_FAST_PATH'''
    return (content.replace(anchor, helper + anchor).replace(entry, gate)
            .replace(serve, serve + '\n        check_ssd_prefix_snapshot();'))
