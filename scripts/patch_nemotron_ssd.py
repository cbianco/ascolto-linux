#!/usr/bin/env python3
"""Apply narrow SSD integration hooks to the pinned private llama source copy."""
import sys
from pathlib import Path

root = Path(sys.argv[1])

def patch(relative, edits):
    path = root / relative
    original = path.with_suffix(path.suffix + '.ascolto-ssd-original')
    current = path.read_text()
    if '// ASCOLTO_SSD_PRIVATE' not in current:
        original.write_text(current)
    content = original.read_text()
    for old, new in edits:
        if content.count(old) != 1:
            raise SystemExit(f'Pinned SSD patch context changed in {relative}: {old[:70]}')
        content = content.replace(old, new)
    content = '// ASCOLTO_SSD_PRIVATE\n' + content
    if content != current:
        path.write_text(content)

patch('src/llama-model-loader.h', [('    size_t size_data = 0;',
    '    size_t size_data = 0;\n    size_t ascolto_streamed_bytes = 0;')])
patch('src/llama-model-loader.cpp', [
    ('#include "llama-model-loader.h"', '#include "llama-model-loader.h"\n#include "streamed_moe.h"'),
    ('    ggml_context * ctx = ctx_for_buft(buft);\n\n    // if duplicated', '''    if (ascolto_moe_metadata(buft)) {
        if (flags & TENSOR_DUPLICATED) throw std::runtime_error("Duplicated SSD experts are unsupported");
        ggml_tensor *tensor = ascolto_moe_import(buft, &t_meta);
        ascolto_streamed_bytes += ggml_nbytes(&t_meta);
        n_created++;
        return tensor;
    }
    ggml_context * ctx = ctx_for_buft(buft);

    // if duplicated'''),
    ('        size_data += ggml_nbytes(it.second.tensor);\n    }\n}',
     '        size_data += ggml_nbytes(it.second.tensor);\n    }\n    size_data -= ascolto_streamed_bytes;\n}')])
patch('src/llama-model.cpp', [
    ('#include "llama-model.h"', '#include "llama-model.h"\n#include "streamed_moe.h"\n#include <cstdlib>'),
    ('    ml.init_mappings(true, use_mlock ? &pimpl->mlock_mmaps : nullptr);', '''    // The independent CPU test maps the complete checkpoint but does not
    // request eager paging of every weight. Normal loading retains prefetch.
    const char * cpu_reference = std::getenv("ASCOLTO_NEMOTRON_TEST_CPU_REFERENCE");
    const bool reference_no_prefetch = cpu_reference && std::strcmp(cpu_reference, "1") == 0;
    if (reference_no_prefetch && (params.load_mode != LLAMA_LOAD_MODE_MMAP ||
            params.n_gpu_layers != 0 || params.use_extra_bufts || !params.no_host || !devices.empty()))
        throw std::runtime_error("CPU reference loader requires plain CPU mmap without offload or repacking");
    ml.init_mappings(!reference_no_prefetch, use_mlock ? &pimpl->mlock_mmaps : nullptr);'''),
    ('    // populate tensors_by_name', '''    // Streamed experts have metadata only and are excluded from allocation/loading.
    if (params.tensor_buft_overrides) {
        for (auto o = params.tensor_buft_overrides; o->pattern; ++o) {
            auto ctx = ascolto_moe_metadata(o->buft);
            if (!ctx) continue;
            for (auto t = ggml_get_first_tensor(ctx); t; t = ggml_get_next_tensor(ctx, t))
                tensors_by_name.emplace_back(ggml_get_name(t), t);
        }
    }
    // populate tensors_by_name''')])
patch('src/llama-graph.cpp', [
    ('#include "llama-graph.h"', '#include "llama-graph.h"\n#include "streamed_moe.h"'),
    ('    ggml_tensor * res = ggml_mul_mat_id(ctx0, w, cur, ids);',
     '    ggml_tensor * res = ascolto_moe_matmul(ctx0, w, cur, ids);\n'
     '    if (!res) res = ggml_mul_mat_id(ctx0, w, cur, ids);'),
    ('    ggml_tensor * experts = nullptr;\n\n    if (gate_up_exps)', '''    ggml_tensor * experts = nullptr;
    // Fuse only the exact ungated Nemotron ReLU-squared branch. All other
    // combinations retain the existing per-projection scale/bias/LoRA path.
    if (arch == LLM_ARCH_NEMOTRON_H_MOE && type_op == LLM_FFN_RELU_SQR && loras->empty() &&
        !gate_exps && !gate_up_exps && !up_exps_s && !down_exps_s && !gate_exps_s &&
        !up_exps_b && !down_exps_b && !gate_exps_b && !gate_up_exps_b) {
        experts = ascolto_moe_ffn(ctx0, up_exps, down_exps, cur, selected_experts);
        if (experts) cb(experts, "ffn_moe_down", il);
    }
    if (!experts) {
    if (gate_up_exps)'''),
    ('    if (!weight_before_ffn) {', '    } // unfused expert branch\n\n    if (!weight_before_ffn) {')])
patch('ggml/src/ggml-metal/ggml-metal.cpp', [
    ('    ggml_metal_buffer_t res = ggml_metal_buffer_map(ctx_dev, ptr, size, max_tensor_size);',
     '    ggml_metal_buffer_t res = ggml_metal_buffer_map(ctx_dev, ptr, size, max_tensor_size);\n'
     '    if (res == nullptr) return nullptr;')])

# Mapped cache pages remain owned by Ascolto. On map failure release only
# unregistered Metal views; no residency collection entry exists yet.
metal_path = root / 'ggml/src/ggml-metal/ggml-metal-device.m'
original = metal_path.with_suffix(metal_path.suffix + '.ascolto-ssd-original')
metal = original.read_text() if original.exists() else metal_path.read_text()
start = metal.index('ggml_metal_buffer_t ggml_metal_buffer_map(')
end = metal.index('\nvoid ggml_metal_buffer_free(', start)
old = metal[start:end]
new = old.replace('    res->dev = dev;', '    if (res == NULL) return NULL;\n\n    res->dev = dev;', 1)
new = new.replace('    ggml_metal_buffer_t res = calloc',
    '    const int test_failure = atomic_exchange(&ascolto_metal_map_test_stage, 0);\n'
    '    if (test_failure == 1) return NULL;\n'
    '    ggml_metal_buffer_t res = calloc', 1)
new = new.replace('    res->use_residency_sets = props_dev->use_residency_sets;',
    '    if (test_failure == 2) return ascolto_metal_map_fail(res);\n'
    '    res->use_residency_sets = props_dev->use_residency_sets;')
new = new.replace('    ggml_metal_device_rsets_add(dev, res->rset);',
    '    if (test_failure == 3) return ascolto_metal_map_fail(res);\n'
    '    ggml_metal_device_rsets_add(dev, res->rset);')
import re
new, count = re.subn(r'free\(res\);\n\s*return NULL;', 'return ascolto_metal_map_fail(res);', new)
if count != 3:
    raise SystemExit('Pinned Metal map failure paths changed')
helper = '''// Private fault control, hidden from the adapter ABI and used only by the
// excluded native mapping test. Normal calls always observe stage zero.
static atomic_int ascolto_metal_map_test_stage = 0;
void ascolto_metal_map_fault_for_test(int stage) {
    atomic_store(&ascolto_metal_map_test_stage, stage);
}

static ggml_metal_buffer_t ascolto_metal_map_fail(ggml_metal_buffer_t res) {
    for (int i = 0; i < res->n_buffers; ++i) [res->buffers[i].metal release];
    ggml_metal_buffer_rset_free(res);
    free(res);
    return NULL;
}

'''
patch('ggml/src/ggml-metal/ggml-metal-device.m', [
    (old, helper + new),
    ('        desc.initialCapacity = buf->n_buffers;\n\n        NSError * error;',
     '        desc.initialCapacity = buf->n_buffers;\n\n        NSError * error = nil;')])
