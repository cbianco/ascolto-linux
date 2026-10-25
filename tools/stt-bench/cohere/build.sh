#!/bin/bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
runtime="$root/build/stt-bench/runtimes/sherpa-onnx-v1.13.8-osx-arm64-shared-lib"
clang -O3 -Wall -Wextra -I"$root/build/stt-bench/runtimes/cohere-sherpa/include" \
    "$root/tools/stt-bench/cohere/bench.c" -L"$runtime/lib" -lsherpa-onnx-c-api \
    -Wl,-rpath,"$runtime/lib" -o "$root/build/stt-bench/cohere-bench"
