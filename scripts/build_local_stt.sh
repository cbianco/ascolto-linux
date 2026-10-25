#!/bin/sh
# Build in-process native adapters. Sources and licenses are pinned, weights are not bundled.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
python3 "$root/scripts/generate_local_stt_manifest.py" --check
build="$root/build/local-stt"
mkdir -p "$build/sources" "$root/bin/local-stt/licenses"
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
whisper=${ASCOLTO_WHISPER_SOURCE:-"$build/sources/whisper"}
parakeet=${ASCOLTO_PARAKEET_SOURCE:-"$build/sources/parakeet"}
if test -z "${ASCOLTO_PARAKEET_SOURCE:-}" && test -f "$root/build/stt-bench/runtimes/parakeet/.git/HEAD"; then parakeet="$root/build/stt-bench/runtimes/parakeet"; fi
qwen=${ASCOLTO_QWEN_SOURCE:-"$build/sources/qwen"}
if test -z "${ASCOLTO_QWEN_SOURCE:-}" && test -f "$root/build/stt-bench/runtimes/qwen-asr/.git/HEAD"; then qwen="$root/build/stt-bench/runtimes/qwen-asr"; fi
fetch "$whisper" https://github.com/ggml-org/whisper.cpp.git d1be6fde11ac6e0407606b4e42fe72d34add8037
fetch "$parakeet" https://github.com/mudler/parakeet.cpp.git 5c9b0fbbd8e611d88e47d7e1ebae9bca9b447e8f
if ! test -f "$parakeet/third_party/ggml/CMakeLists.txt"; then
    git -C "$parakeet" submodule update --init third_party/ggml
fi
(cd "$parakeet" && bash scripts/apply_ggml_patches.sh)
fetch "$qwen" https://github.com/antirez/qwen-asr.git 924694251d9e0f18e5d86bbd06aa3ab5f870002d
for engine in whisper-turbo-q5 parakeet-v3-q8 qwen3-asr-1.7b; do
    case "$engine" in whisper*) source=$whisper;; parakeet*) source=$parakeet;; qwen*) source=$qwen;; esac
    cmake -S "$root/src/local_stt" -B "$build/$engine" -DCMAKE_BUILD_TYPE=Release \
        -DENGINE="$engine" -DRUNTIME_SOURCE="$source" -DOUTPUT_DIRECTORY="$root/bin/local-stt"
    cmake --build "$build/$engine" --target adapter -j "${ASCOLTO_BUILD_JOBS:-8}"
    cp "$source/LICENSE" "$root/bin/local-stt/licenses/$engine.txt"
    case "$engine" in
        whisper*) cp "$source/LICENSE" "$root/bin/local-stt/licenses/$engine-ggml.txt";;
        parakeet*) cp "$source/third_party/ggml/LICENSE" "$root/bin/local-stt/licenses/$engine-ggml.txt";;
    esac
done
cp "$root/resources/LOCAL_STT_NOTICES.md" "$root/bin/local-stt/"
cp "$root/resources/local-stt-manifest.json" "$root/bin/local-stt/"
