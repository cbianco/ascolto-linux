#!/bin/sh
# Build only the pinned native echo canceller and residual-echo processor.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build="$root/build/echo"
source="$build/speexdsp"
revision=1b28a0f61bc31162979e1f26f3981fc3637095c8
if ! git -C "$source" rev-parse --git-dir >/dev/null 2>&1; then
    git init "$source"
    git -C "$source" remote add origin https://github.com/xiph/speexdsp.git
    git -C "$source" fetch --depth 1 origin "$revision"
    git -C "$source" checkout --detach FETCH_HEAD
fi
test "$(git -C "$source" rev-parse HEAD)" = "$revision"
cmake -S "$root/src/echo" -B "$build/native" -DCMAKE_BUILD_TYPE=Release \
    -DSPEEX_SOURCE="$source" -DCMAKE_ARCHIVE_OUTPUT_DIRECTORY="$build"
cmake --build "$build/native" -j "${ASCOLTO_BUILD_JOBS:-4}"
cp "$source/COPYING" "$build/SPEEXDSP-LICENSE.txt"
