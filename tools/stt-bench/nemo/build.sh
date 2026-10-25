#!/bin/sh
# Build the native C benchmark against the prepared NeMo-Speech.cpp checkout.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
runtime="$root/build/stt-bench/runtimes/nemo"
lib="$runtime/build/metal-asr/bin"
clang -O3 -std=c11 "$root/tools/stt-bench/nemo/bench_nemo.c" \
  -I"$runtime/include" -L"$lib" -lnemo_speech_asr_c \
  -Wl,-rpath,"$lib" -o "$runtime/bench-nemo"
