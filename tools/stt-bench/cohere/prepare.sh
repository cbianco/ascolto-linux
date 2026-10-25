#!/bin/bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
runtimes="$root/build/stt-bench/runtimes"
models="$root/build/stt-bench/models"
archive=cohere-sherpa-v1.13.8.tar.bz2
runtime=sherpa-onnx-v1.13.8-osx-arm64-shared-lib
model=cohere-transcribe-int8-hf
model_repo=csukuangfj2/sherpa-onnx-cohere-transcribe-14-lang-int8-2026-04-01
revision=156a470cf08eefe706a0004f3c52d9ee567ca7a0
mkdir -p "$runtimes/cohere-sherpa/include" "$models"
fetch() {
    if [[ ! -f "$2" ]]; then
        curl -fL --retry 3 --continue-at - "$1" -o "$2.partial"
        mv "$2.partial" "$2"
    fi
}
fetch "https://github.com/k2-fsa/sherpa-onnx/releases/download/v1.13.8/$runtime.tar.bz2" "$runtimes/$archive"
if [[ ! -f "$runtimes/$runtime/lib/libsherpa-onnx-c-api.dylib" ]]; then
    tar xjf "$runtimes/$archive" -C "$runtimes"
fi
fetch https://raw.githubusercontent.com/k2-fsa/sherpa-onnx/v1.13.8/sherpa-onnx/c-api/c-api.h \
    "$runtimes/cohere-sherpa/include/c-api.h"
mkdir -p "$models/$model"
for file in encoder.int8.onnx encoder.int8.onnx.data decoder.int8.onnx tokens.txt README.md; do
    fetch "https://huggingface.co/$model_repo/resolve/$revision/$file" "$models/$model/$file"
done
if [[ -f "$root/tools/stt-bench/cohere/resources.sha256" ]]; then
    cd "$root"
    shasum -a 256 --check tools/stt-bench/cohere/resources.sha256
fi
bash "$root/tools/stt-bench/cohere/build.sh"
