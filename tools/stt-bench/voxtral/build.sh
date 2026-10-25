#!/bin/bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
runtime="$root/build/stt-bench/runtimes/voxtral.c"
if [[ "${SKIP_RUNTIME_BUILD:-0}" != 1 ]]; then
    make -C "$runtime" mps
fi
objects=(voxtral voxtral_kernels voxtral_audio voxtral_encoder voxtral_decoder voxtral_tokenizer voxtral_safetensors voxtral_mic_macos)
args=()
for object in "${objects[@]}"; do args+=("$runtime/$object.mps.o"); done
clang -O3 -Wall -Wextra -I"$runtime" "$root/tools/stt-bench/voxtral/bench.c" \
    "$root/tools/stt-bench/voxtral/gpu_memory.m" -fobjc-arc \
    "${args[@]}" "$runtime/voxtral_metal.o" -o "$root/build/stt-bench/voxtral-bench" \
    -lm -framework Accelerate -framework Metal -framework MetalPerformanceShaders \
    -framework MetalPerformanceShadersGraph -framework Foundation \
    -framework AudioToolbox -framework CoreFoundation
