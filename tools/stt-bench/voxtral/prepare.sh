#!/bin/bash
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
runtime="$root/build/stt-bench/runtimes/voxtral.c"
model="$root/build/stt-bench/models/voxtral-mini-4b-realtime"
commit=134d366c24d20c64b614a3dcc8bda2a6922d077d
revision=2769294da9567371363522aac9bbcfdd19447add
mkdir -p "$(dirname "$runtime")" "$model"
if [[ ! -d "$runtime/.git" ]]; then
    git init "$runtime"
    git -C "$runtime" remote add origin https://github.com/antirez/voxtral.c.git
    git -C "$runtime" fetch --depth 1 origin "$commit"
    git -C "$runtime" checkout --detach FETCH_HEAD
fi
[[ "$(git -C "$runtime" rev-parse HEAD)" == "$commit" ]]
for file in consolidated.safetensors params.json tekken.json; do
    if [[ ! -f "$model/$file" ]]; then
        curl -fL --retry 3 --continue-at - \
            "https://huggingface.co/mistralai/Voxtral-Mini-4B-Realtime-2602/resolve/$revision/$file" \
            -o "$model/$file.partial"
        mv "$model/$file.partial" "$model/$file"
    fi
done
if [[ -f "$root/tools/stt-bench/voxtral/resources.sha256" ]]; then
    cd "$root"
    shasum -a 256 --check tools/stt-bench/voxtral/resources.sha256
fi
bash "$root/tools/stt-bench/voxtral/build.sh"
