#!/bin/sh
# Compile the pinned CPU runtime and C benchmark without running inference.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
RUNTIME="$ROOT/build/stt-bench/runtimes/qwen-asr"
OUTPUT="$ROOT/build/stt-bench/runtimes/qwen-bench"
PIN=924694251d9e0f18e5d86bbd06aa3ab5f870002d
test "$(git -C "$RUNTIME" rev-parse HEAD)" = "$PIN"
make -C "$RUNTIME" -j8 blas CC=clang
clang -Wall -Wextra -O3 -march=native -ffast-math \
    -DUSE_BLAS -DACCELERATE_NEW_LAPACK \
    -I"$ROOT/tools/stt-bench" -I"$RUNTIME" \
    "$ROOT/tools/stt-bench/qwen/bench.c" \
    "$RUNTIME/qwen_asr.o" "$RUNTIME/qwen_asr_kernels.o" \
    "$RUNTIME/qwen_asr_kernels_generic.o" "$RUNTIME/qwen_asr_kernels_neon.o" \
    "$RUNTIME/qwen_asr_kernels_avx.o" "$RUNTIME/qwen_asr_audio.o" \
    "$RUNTIME/qwen_asr_encoder.o" "$RUNTIME/qwen_asr_decoder.o" \
    "$RUNTIME/qwen_asr_tokenizer.o" "$RUNTIME/qwen_asr_safetensors.o" \
    -framework Accelerate -lm -lpthread -o "$OUTPUT"
