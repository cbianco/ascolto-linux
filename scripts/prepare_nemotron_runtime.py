#!/usr/bin/env python3
"""Prepare a private static NeMo runtime without editing the pinned checkout."""
import argparse
import re
import shutil
from pathlib import Path
from patch_nemotron_prefill import patch_ssd_packed_prefill

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
parser.add_argument('destination', type=Path)
parser.add_argument('sentencepiece', type=Path)
args = parser.parse_args()

# Preserve unchanged overlay files for incremental native builds.
def copy_changed(source, destination):
    source, destination = Path(source), Path(destination)
    relative = source.relative_to(args.source) if source.is_relative_to(args.source) else None
    if relative and str(relative) in ('CMakeLists.txt', 'src/asr/CMakeLists.txt',
                                     'src/speechlm/CMakeLists.txt', 'cmake/sentencepiece.cmake',
                                     'src/runtime/ggml/CMakeLists.txt', 'src/speechlm/salm_engine.cpp',
                                     'src/speechlm/salm_engine.h', 'src/runtime/ggml/nn.cpp',
                                     'src/runtime/ggml/runtime.h'):
        return str(destination)
    data = source.read_bytes()
    # Avoid the global format() collision between the ASR and llama static archives.
    if relative and str(relative).startswith('src/runtime/ggml/') and source.suffix in ('.h', '.cpp'):
        data = re.sub(rb'(?<![\w:.])format\((?!gnu_printf,|printf,)', b'ascolto_nemo_format(', data)
    if not destination.exists() or data != destination.read_bytes():
        destination.write_bytes(data)
    return str(destination)

shutil.copytree(args.source, args.destination, dirs_exist_ok=True,
                ignore=shutil.ignore_patterns('.git', '.deps', 'build', 'llama.cpp'),
                copy_function=copy_changed)
llama = args.destination / 'llama.cpp'
if not llama.exists():
    llama.symlink_to((args.source / 'llama.cpp').resolve(), target_is_directory=True)

def write_changed(path, text):
    if not path.exists() or path.read_text() != text:
        path.write_text(text)

for relative, target in [('src/asr/CMakeLists.txt', 'nemo_speech_asr'),
                         ('src/speechlm/CMakeLists.txt', 'nemo_speech_speechlm')]:
    path = args.destination / relative
    content = (args.source / relative).read_text()
    old = f'add_library({target} SHARED '
    if content.count(old) != 1:
        raise SystemExit(f'Pinned NeMo target changed: {target}')
    write_changed(path, content.replace(old, f'add_library({target} STATIC '))

path = args.destination / 'src/runtime/ggml/CMakeLists.txt'
content = (args.source / 'src/runtime/ggml/CMakeLists.txt').read_text()
content = content.replace('add_library(nemo_speech_runtime_ggml STATIC',
    'list(REMOVE_ITEM RUNTIME_GGML_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/llama_log_shim.cpp")\n'
    'add_library(nemo_speech_runtime_ggml STATIC')
write_changed(path, content)

# Preserve F32 perception convolution weights in the private reference path.
# Existing F16 packages retain their original graph and storage types.
for relative in ('src/runtime/ggml/nn.cpp', 'src/runtime/ggml/runtime.h'):
    path = args.destination / relative
    content = (args.source / relative).read_text()
    content = re.sub(r'(?<![\w:.])format\((?!gnu_printf,|printf,)', 'ascolto_nemo_format(', content)
    if relative.endswith('nn.cpp'):
        old = 'weight_name, GGML_TYPE_F16, kernel_size, in_channels, out_channels);'
        if content.count(old) != 1:
            raise SystemExit('Pinned Conv1D allocation changed')
        content = content.replace(old, 'weight_name, stored, kernel_size, in_channels, out_channels);')
    else:
        old = 'a->type == GGML_TYPE_BF16 ? GGML_TYPE_F32 : GGML_TYPE_F16'
        if content.count(old) != 1:
            raise SystemExit('Pinned Conv1D graph changed')
        content = content.replace(old, 'a->type == GGML_TYPE_F16 ? GGML_TYPE_F16 : GGML_TYPE_F32')
    write_changed(path, content)

# Upstream owns decoder resources as raw pointers. A constructor exception must
# release them before member destruction and before the adapter unloads backends.
path = args.destination / 'src/speechlm/salm_engine.cpp'
content = (args.source / 'src/speechlm/salm_engine.cpp').read_text()
constructor = ': cfg(std::move(c)), bm(make_params(cfg)), pee(bm, cfg.encoder_path, false, cfg.ctc_path) {'
if content.count(constructor) != 1 or content.count('    ~Impl() {') != 1:
    raise SystemExit('Pinned SALM constructor or destructor changed')
start = content.index(constructor) + len(constructor)
end = content.index('\n    }\n', start)
content = (content[:start] + '\n        try {' + content[start:end] +
           '\n        } catch (...) {\n            release_decoder();\n            throw;\n        }' + content[end:])
content = content.replace('    ~Impl() {', '''    ~Impl() { release_decoder(); }

    void release_decoder() {
        if (ctx) llama_synchronize(ctx);
        if (ctx_dft) llama_synchronize(ctx_dft);''')
# Optional SSD execution stays private and cannot be enabled by production settings yet.
content = '#include "streamed_moe.h"\n' + content
content = content.replace('embedding_overrides[2]', 'embedding_overrides[3]')
content = content.replace('    SalmConfig cfg;', '    SalmConfig cfg;\n    ascolto_streamed_moe *ssd = nullptr;')
model_anchor = '        model = llama_model_load_from_file'
context_anchor = '        ctx = llama_init_from_model(model, cp);'
params_anchor = '    static ggml_runtime::Params make_params(const SalmConfig& c) {'
for anchor in (model_anchor, context_anchor, params_anchor):
    if content.count(anchor) != 1:
        raise SystemExit('Pinned SALM CPU reference configuration context changed')
content = content.replace(params_anchor, params_anchor + r'''
        const char* reference = std::getenv("ASCOLTO_NEMOTRON_TEST_CPU_REFERENCE");
        const bool reference_requested = reference && std::strcmp(reference, "1") == 0;
        if (c.cpu_reference != reference_requested ||
            (c.cpu_reference && (c.gpu >= 0 || c.expert_cache_bytes != 0)))
            throw std::runtime_error("CPU reference requires its diagnostic flag, CPU-only execution and zero expert cache");''')
content = content.replace(model_anchor, r'''        if (cfg.cpu_reference) {
            // Explicit mmap and plain CPU buffers prevent a resident reference
            // from copying the full checkpoint into private or GPU allocations.
            decoder_devices[0] = nullptr;
            mp.devices = decoder_devices;
            mp.n_gpu_layers = 0;
            mp.load_mode = LLAMA_LOAD_MODE_MMAP;
            mp.lazy_mode = LLAMA_LAZY_MODE_OFF;
            mp.use_extra_bufts = false;
            mp.no_host = true;
            std::fprintf(stderr, "[cpu-reference] mmap=1 prefetch=0 gpu_offload=0 extra_bufts=0\n");
        }
        if (cfg.expert_cache_bytes) {
            if (fast || cfg.mtp_draft || !cfg.dflash_path.empty() || cfg.max_sequences != 1)
                throw std::runtime_error("SSD mode requires one sequence without speculation");
            ssd = ascolto_moe_open(cfg.llm_path.c_str(), cfg.expert_cache_bytes,
                                   cfg.threads, decoder_devices[0]);
            mp.load_mode = LLAMA_LOAD_MODE_NONE;
            mp.lazy_mode = LLAMA_LAZY_MODE_OFF;
            embedding_overrides[0] = {"ffn_(up|down)_exps\\.weight$", ascolto_moe_buft(ssd)};
            mp.tensor_buft_overrides = embedding_overrides;
        }
        model = llama_model_load_from_file''')
content = content.replace('        vocab = llama_model_get_vocab(model);',
    '        if (ssd) ascolto_moe_loaded(ssd);\n        vocab = llama_model_get_vocab(model);')
content = content.replace(context_anchor,
    '        if (ssd) { cp.abort_callback = ascolto_moe_aborted; cp.abort_callback_data = ssd; }\n'
    '        if (cfg.cpu_reference) { cp.op_offload = false; cp.offload_kqv = false; }\n'
    '        ctx = llama_init_from_model(model, cp);')
content = content.replace('        const int rc = llama_decode(ctx, b);',
    r'''        ascolto_moe_check(ssd);
        const bool profile = ssd && std::getenv("ASCOLTO_NEMOTRON_SSD_PROFILE");
        ascolto_expert_cache_stats_t before{}, after{};
        if (profile) ascolto_moe_stats(ssd, &before);
        const auto decode_start = std::chrono::steady_clock::now();
        const int rc = llama_decode(ctx, b);
        ascolto_moe_check(ssd);
        if (profile) {
            llama_synchronize(ctx);
            ascolto_moe_stats(ssd, &after);
            std::fprintf(stderr, "[ssd-decode] tokens=%d audio=%d mixed=%d seconds=%.6f bytes=%llu misses=%llu\n",
                b.n_tokens, b.embd != nullptr, b.embd_input != nullptr,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - decode_start).count(),
                (unsigned long long)(after.bytes_read - before.bytes_read),
                (unsigned long long)(after.misses - before.misses));
        }''')
content = content.replace('                auto enc = encode(*batch, 0, batch->size());', r'''
                const auto encoder_start = std::chrono::steady_clock::now();
                auto enc = encode(*batch, 0, batch->size());
                if (ssd && std::getenv("ASCOLTO_NEMOTRON_SSD_PROFILE"))
                    std::fprintf(stderr, "[ssd-encoder] seconds=%.6f\n",
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - encoder_start).count());''')
content = patch_ssd_packed_prefill(content)
content = content.replace('        SalmStats st;', '        ascolto_moe_check(ssd);\n        SalmStats st;')
content = content.replace('            llama_model_free(model);',
    '            llama_model_free(model);\n'
    '        if (ssd) {\n'
    '            ascolto_expert_cache_stats_t stats{}; ascolto_moe_stats(ssd, &stats);\n'
    '            std::fprintf(stderr, "[ssd] cache=%llu read=%llu hits=%llu misses=%llu pinned=%zu\\n",\n'
    '                (unsigned long long)stats.capacity_bytes, (unsigned long long)stats.bytes_read,\n'
    '                (unsigned long long)stats.hits, (unsigned long long)stats.misses, stats.pinned_slots);\n'
    '            ascolto_moe_close(ssd); ssd = nullptr;\n        }')
write_changed(path, content)
path = args.destination / 'src/speechlm/salm_engine.h'
content = (args.source / 'src/speechlm/salm_engine.h').read_text()
if content.count('struct SalmConfig {') != 1:
    raise SystemExit('Pinned SALM configuration declaration changed')
content = content.replace('struct SalmConfig {', 'struct SalmConfig {\n'
    '    bool cpu_reference = false; // Private CPU mmap reference; requires the matching test flag.\n'
    '    unsigned long long expert_cache_bytes = 0; // Private experimental SSD cache budget.')
write_changed(path, content)

# SentencePiece and its bundled protobuf are compiled into the same private adapter.
# Expose only the installed header: SentencePiece's internal common.h shadows llama-common.
headers = args.destination / 'ascolto-sentencepiece-include'
headers.mkdir(exist_ok=True)
copy_changed(args.sentencepiece / 'src/sentencepiece_processor.h', headers / 'sentencepiece_processor.h')
write_changed(args.destination / 'cmake/sentencepiece.cmake', '''function(nemo_speech_link_sentencepiece target)
    target_link_libraries(${target} PRIVATE sentencepiece-static)
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/ascolto-sentencepiece-include")
endfunction()
''')
path = args.destination / 'CMakeLists.txt'
content = (args.source / 'CMakeLists.txt').read_text().replace('add_subdirectory(src/runtime/ggml)', '''set(SPM_ENABLE_SHARED OFF CACHE BOOL "" FORCE)
set(SPM_ENABLE_TCMALLOC OFF CACHE BOOL "" FORCE)
set(SPM_BUILD_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${ASCOLTO_SENTENCEPIECE_SOURCE}" sentencepiece EXCLUDE_FROM_ALL)
add_subdirectory(src/runtime/ggml)''')
content += '''
execute_process(COMMAND python3 "${ASCOLTO_ROOT}/scripts/patch_nemotron_ssd.py" "${NEMO_SPEECH_LLAMA_CPP_DIR}" COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ASCOLTO_ROOT}/scripts/patch_nemotron_ssd.py")
target_sources(llama PRIVATE "${ASCOLTO_ROOT}/src/local_stt/streamed_moe.cpp" "${ASCOLTO_ROOT}/src/local_stt/expert_cache.c")
target_include_directories(llama PRIVATE "${ASCOLTO_ROOT}/src/local_stt" "${NEMO_SPEECH_LLAMA_CPP_DIR}/ggml/src")
target_include_directories(nemo_speech_speechlm PRIVATE "${ASCOLTO_ROOT}/src/local_stt")
add_library(adapter-q8 SHARED "${ASCOLTO_ROOT}/src/local_stt/nemotron.cpp")
target_include_directories(adapter-q8 PRIVATE "${ASCOLTO_ROOT}/include" "${NEMO_SPEECH_LLAMA_CPP_DIR}/include")
target_link_libraries(adapter-q8 PRIVATE nemo_speech_speechlm)
target_compile_definitions(adapter-q8 PRIVATE ASCOLTO_NEMOTRON_Q8=1)
set_target_properties(adapter-q8 PROPERTIES PREFIX "" OUTPUT_NAME "nemotron-3.5-q8"
    LIBRARY_OUTPUT_DIRECTORY "${ASCOLTO_OUTPUT_DIRECTORY}")
if(APPLE)
    target_link_options(adapter-q8 PRIVATE "-Wl,-exported_symbol,_ascolto_stt_get_api")
else()
    file(WRITE "${CMAKE_BINARY_DIR}/ascolto-q8-exports.map" "{ global: ascolto_stt_get_api; local: *; };\\n")
    target_link_options(adapter-q8 PRIVATE "-Wl,--version-script=${CMAKE_BINARY_DIR}/ascolto-q8-exports.map" "-Wl,-Bsymbolic")
endif()
if(ASCOLTO_BF16_RECIPE_SHA256)
    set(bf16_resources "${ASCOLTO_ROOT}/resources/nemotron-bf16-preparation")
    file(SHA256 "${bf16_resources}/recipe.json" actual_bf16_recipe)
    if(NOT actual_bf16_recipe STREQUAL ASCOLTO_BF16_RECIPE_SHA256)
        message(FATAL_ERROR "Native BF16 preparation resource checksum mismatch")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${bf16_resources}/recipe.json")
    add_library(adapter-bf16 SHARED "${ASCOLTO_ROOT}/src/local_stt/nemotron.cpp" "${ASCOLTO_ROOT}/src/local_stt/prepare_bf16.cpp")
    target_include_directories(adapter-bf16 PRIVATE "${ASCOLTO_ROOT}/include" "${NEMO_SPEECH_LLAMA_CPP_DIR}/include" "${NEMO_SPEECH_LLAMA_CPP_DIR}/vendor/nlohmann")
    target_compile_definitions(adapter-bf16 PRIVATE ASCOLTO_NEMOTRON_BF16=1 ASCOLTO_NEMOTRON_BF16_RECIPE_SHA256="${ASCOLTO_BF16_RECIPE_SHA256}")
    target_link_libraries(adapter-bf16 PRIVATE nemo_speech_speechlm)
    set_target_properties(adapter-bf16 PROPERTIES PREFIX "" OUTPUT_NAME "nemotron-3.5-bf16-ssd" LIBRARY_OUTPUT_DIRECTORY "${ASCOLTO_OUTPUT_DIRECTORY}")
    if(APPLE)
        target_link_options(adapter-bf16 PRIVATE "-Wl,-exported_symbol,_ascolto_stt_get_api" "-Wl,-exported_symbol,_ascolto_stt_prepare_package_v1")
    else()
        find_package(OpenSSL REQUIRED)
        target_link_libraries(adapter-bf16 PRIVATE OpenSSL::Crypto)
        file(WRITE "${CMAKE_BINARY_DIR}/ascolto-bf16-exports.map" "{ global: ascolto_stt_get_api; ascolto_stt_prepare_package_v1; local: *; };\\n")
        target_link_options(adapter-bf16 PRIVATE "-Wl,--version-script=${CMAKE_BINARY_DIR}/ascolto-bf16-exports.map" "-Wl,-Bsymbolic")
    endif()
endif()
add_executable(ascolto-nemotron-moe-test EXCLUDE_FROM_ALL
    "${ASCOLTO_ROOT}/tests/nemotron_moe.cpp" "${ASCOLTO_ROOT}/src/local_stt/expert_cache.c")
target_include_directories(ascolto-nemotron-moe-test PRIVATE "${ASCOLTO_ROOT}/src/local_stt")
target_link_libraries(ascolto-nemotron-moe-test PRIVATE ggml ggml-base ggml-cpu)
set_property(TARGET ascolto-nemotron-moe-test PROPERTY C_STANDARD 11)
add_executable(ascolto-nemotron-prepare EXCLUDE_FROM_ALL
    "${ASCOLTO_ROOT}/tests/nemotron_prepare_main.cpp" "${ASCOLTO_ROOT}/src/local_stt/prepare_bf16.cpp")
target_include_directories(ascolto-nemotron-prepare PRIVATE "${ASCOLTO_ROOT}/src/local_stt" "${NEMO_SPEECH_LLAMA_CPP_DIR}/vendor/nlohmann")
if(NOT APPLE)
    find_package(OpenSSL REQUIRED)
    target_link_libraries(ascolto-nemotron-prepare PRIVATE OpenSSL::Crypto)
endif()
if(APPLE)
    add_executable(ascolto-nemotron-metal-mapping EXCLUDE_FROM_ALL "${ASCOLTO_ROOT}/tests/nemotron_metal_mapping.cpp")
    target_compile_options(ascolto-nemotron-metal-mapping PRIVATE -x objective-c++)
    target_link_libraries(ascolto-nemotron-metal-mapping PRIVATE ggml ggml-metal)
endif()
add_executable(ascolto-nemotron-reference-conv EXCLUDE_FROM_ALL "${ASCOLTO_ROOT}/tests/nemotron_reference_conv.cpp")
target_link_libraries(ascolto-nemotron-reference-conv PRIVATE nemo_speech_runtime_ggml)
add_executable(ascolto-nemotron-streamed-graph EXCLUDE_FROM_ALL "${ASCOLTO_ROOT}/tests/nemotron_streamed_graph.cpp")
target_include_directories(ascolto-nemotron-streamed-graph PRIVATE "${ASCOLTO_ROOT}/src/local_stt" "${NEMO_SPEECH_LLAMA_CPP_DIR}/ggml/src")
target_link_libraries(ascolto-nemotron-streamed-graph PRIVATE llama)
add_executable(ascolto-nemotron-decoder EXCLUDE_FROM_ALL "${ASCOLTO_ROOT}/tests/nemotron_decoder.cpp")
target_link_libraries(ascolto-nemotron-decoder PRIVATE nemo_speech_speechlm)
target_include_directories(ascolto-nemotron-decoder PRIVATE "${NEMO_SPEECH_LLAMA_CPP_DIR}/vendor/nlohmann")
add_executable(ascolto-nemotron-constructor-fault EXCLUDE_FROM_ALL
    "${ASCOLTO_ROOT}/tests/nemotron_constructor_fault.cpp")
target_include_directories(ascolto-nemotron-constructor-fault PRIVATE "${NEMO_SPEECH_LLAMA_CPP_DIR}/include")
target_link_libraries(ascolto-nemotron-constructor-fault PRIVATE nemo_speech_speechlm)
if(APPLE)
    target_compile_options(ascolto-nemotron-constructor-fault PRIVATE -x objective-c++)
endif()
'''
write_changed(path, content)
