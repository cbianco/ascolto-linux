#!/bin/sh
# Build the native C benchmark against the prepared parakeet.cpp checkout.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
runtime="$root/build/stt-bench/runtimes/parakeet"
clang -O3 -std=c11 -c "$root/tools/stt-bench/parakeet/bench_parakeet.c" \
  -I"$runtime/include" -o "$runtime/bench-parakeet-main.o"
clang++ -O3 -std=c++17 -c "$root/tools/stt-bench/parakeet/shutdown.cpp" \
  -I"$runtime/src" -o "$runtime/bench-parakeet-shutdown.o"
clang++ "$runtime/bench-parakeet-main.o" "$runtime/bench-parakeet-shutdown.o" \
  -L"$runtime/build" -lparakeet \
  -Wl,-rpath,"$runtime/build" -o "$runtime/bench-parakeet"
