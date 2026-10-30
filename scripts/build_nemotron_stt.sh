#!/bin/sh
# Native inference only: this build downloads source dependencies, never weights.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python3 "$root/scripts/generate_local_stt_manifest.py" --check
# CUDA's upstream scratch-allocation failure calls _Exit/abort. Do not embed
# that backend until it can report recoverable allocation errors to Ascolto.
case "${ASCOLTO_NEMOTRON_CUDA:-OFF}" in
    OFF|off|0|NO|no|FALSE|false) ;;
    *) echo 'Nemotron CUDA embedding is unavailable: upstream CUDA OOM terminates the host process. Use the native CPU/Metal build until recoverable errors are supported.' >&2; exit 1;;
esac
build="$root/build/local-stt"
fetch() {
    destination=$1 repository=$2 revision=$3
    if ! git -C "$destination" rev-parse --git-dir >/dev/null 2>&1; then
        git init "$destination"
        git -C "$destination" remote add origin "$repository"
        git -C "$destination" fetch --depth 1 origin "$revision"
        git -C "$destination" checkout --detach FETCH_HEAD
    fi
    test "$(git -C "$destination" rev-parse HEAD)" = "$revision"
}
mkdir -p "$build/sources" "$root/bin/local-stt/licenses"
nemo=${ASCOLTO_NEMOTRON_SOURCE:-"$build/sources/nemo-speech"}
sentencepiece=${ASCOLTO_SENTENCEPIECE_SOURCE:-"$build/sources/sentencepiece"}
fetch "$nemo" https://github.com/NVIDIA/NeMo-Speech.cpp.git 66d50d91b75a406becd95fbaf39d2c9c13e95d7c
fetch "$sentencepiece" https://github.com/google/sentencepiece.git 31646a467d2051eb904e0b45de3a73e91fe1c1e3
if ! test -f "$nemo/llama.cpp/ggml/CMakeLists.txt"; then
    git -C "$nemo" submodule update --init --depth 1 llama.cpp
fi
test "$(git -C "$nemo/llama.cpp" rev-parse HEAD)" = bd4f514db14d87fded667787a7a963bfbaa98e89
python3 "$root/scripts/prepare_nemotron_runtime.py" "$nemo" "$build/nemotron-source" "$sentencepiece"
bf16_recipe=$(cd "$root" && python3 -c 'import json; m=json.load(open("resources/local-stt-manifest.json")); print(next((x.get("preparation_recipe_sha256", "") for x in m["models"] if x["id"]=="nemotron-3.5-bf16-ssd"), ""))')
metal=OFF
if test "$(uname -s)" = Darwin; then metal=ON; fi
cmake -S "$build/nemotron-source" -B "$build/nemotron" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_C_VISIBILITY_PRESET=hidden -DCMAKE_CXX_VISIBILITY_PRESET=hidden \
    -DCMAKE_VISIBILITY_INLINES_HIDDEN=ON \
    -DASCOLTO_ROOT="$root" -DASCOLTO_OUTPUT_DIRECTORY="$root/bin/local-stt" \
    -DASCOLTO_BF16_RECIPE_SHA256="$bf16_recipe" \
    -DASCOLTO_SENTENCEPIECE_SOURCE="$sentencepiece" \
    -DNEMO_SPEECH_BUILD_SPEECHLM=ON -DNEMO_SPEECH_BUILD_ASR=ON \
    -DNEMO_SPEECH_BUILD_DIAR=OFF -DNEMO_SPEECH_BUILD_TTS=OFF \
    -DNEMO_SPEECH_BUILD_CLI=OFF -DNEMO_SPEECH_BUILD_MIC_CAPTURE=OFF \
    -DNEMO_SPEECH_BUILD_NMT=OFF -DNEMO_SPEECH_BUILD_S2S=OFF \
    -DNEMO_SPEECH_BUILD_HTTP=OFF -DNEMO_SPEECH_BUILD_GRPC=OFF \
    -DNEMO_SPEECH_BUILD_TESTS=OFF -DBUILD_TESTING=OFF \
    -DNEMO_SPEECH_GGML_PATCHED=ON -DGGML_BACKEND_DL=OFF -DGGML_NATIVE=OFF \
    -DGGML_OPENMP=OFF -DGGML_METAL="$metal" -DGGML_METAL_EMBED_LIBRARY=ON \
    -DGGML_CUDA=OFF
if test -n "$bf16_recipe"; then
    cmake --build "$build/nemotron" --target adapter-q8 adapter-bf16 -j "${ASCOLTO_BUILD_JOBS:-8}"
    cp -R "$root/resources/nemotron-bf16-preparation" "$root/bin/local-stt/"
else
    cmake --build "$build/nemotron" --target adapter-q8 -j "${ASCOLTO_BUILD_JOBS:-8}"
fi
cp "$nemo/LICENSE" "$root/bin/local-stt/licenses/nemotron.txt"
cp "$nemo/NOTICE" "$root/bin/local-stt/licenses/nemotron-NOTICE.txt"
cp "$nemo/THIRD_PARTY_NOTICES.md" "$root/bin/local-stt/licenses/nemotron-THIRD_PARTY_NOTICES.md"
cp "$nemo/llama.cpp/LICENSE" "$root/bin/local-stt/licenses/nemotron-llama.txt"
cp "$nemo/llama.cpp/LICENSE" "$root/bin/local-stt/licenses/nemotron-ggml.txt"
cp "$sentencepiece/LICENSE" "$root/bin/local-stt/licenses/nemotron-sentencepiece.txt"
for dependency in absl darts_clone protobuf-lite; do
    cp "$sentencepiece/third_party/$dependency/LICENSE" "$root/bin/local-stt/licenses/nemotron-$dependency.txt"
done

# Remove retired outputs so incremental builds cannot rebundle the 4-bit adapter.
rm -f "$root/bin/local-stt/nemotron-3.5-nvfp4.dylib" "$root/bin/local-stt/nemotron-3.5-nvfp4.so" "$root/bin/local-stt/licenses/nemotron-3.5-nvfp4.txt"
