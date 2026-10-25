# Voxtral native benchmark

`prepare.sh` downloads pinned official Mistral BF16 weights and the MIT-licensed
`antirez/voxtral.c` runtime at `134d366c24d20c64b614a3dcc8bda2a6922d077d`.
`build.sh` builds the C / Objective-C Metal implementation. Inference does not
use Python. The harness requires successful Metal initialization; the upstream
runtime retains CPU fallbacks for individual operations if their GPU path fails.

Run from the repository root after preparation:

```sh
build/stt-bench/voxtral-bench \
  "$PWD/build/stt-bench/models/voxtral-mini-4b-realtime" \
  "$PWD/build/stt-bench/corpus/manifest.tsv" 3 \
  "$PWD/build/stt-bench/results/voxtral-extra.jsonl" \
  > build/stt-bench/results/voxtral.jsonl \
  2> build/stt-bench/results/voxtral.log
```

The process loads once, warms up with the first clip (`repeat=-1`), then runs
three measured corpus passes (`repeat=0,1,2`). Each clip creates independent
stream state; model weights and GPU caches remain loaded. Total call time
includes stream creation, feature extraction, feed, finish, result collection,
and stream destruction. PCM files are read before model loading and timing.

The public API has no token callback, so `first_token_ms` remains null. The
sidecar records the first retrieval from the token queue; batch queue retrieval
must not be interpreted as first-token generation latency. An optional final
argument `32000` supplies 2-second chunks as fast as processing permits and
records the first observed incremental text. Save that configuration to distinct
`voxtral-stream` result files. This is a compute replay, without realtime pacing
or simulated capture/endpointing delay.

The pinned runtime preallocates 1006 encoder KV positions in shared GPU memory.
A whole 30-second feed exceeds that capacity, prints an encoder allocation
error and silently returns success after discarding audio. Such batch results
are invalid. Feed 2-second chunks for the complete-corpus comparison.
`encoder-coverage-check.patch` is a proposed guard prepared after observing this
failure, and was **not applied to the recorded benchmark binary**. It checks
encoder position coverage after finalization and reports `error=2` when it
detects a gross omission. It is a sanity check, not comprehensive upstream error
propagation. Original batch API `error=0` values remain preserved; result analysis
must flag the failed 30-second cases separately. After applying the patch,
`SKIP_RUNTIME_BUILD=1 bash build.sh` relinks the checked harness without
recompiling the pinned runtime.

The sidecar samples `MTLDevice.currentAllocatedSize` after loading and each
clip, and records `vox_metal_memory_used()` separately. The latter counts weight
caches only. Neither sample is a transient peak. GPU allocations and RSS overlap
in unified memory and must not be added. BF16 files are converted to cached F16
GPU weights by this runtime. Default decoder KV cache is F16 and delay is 480ms.
Language is automatic; this port exposes no forced-language control.

Sources: [runtime](https://github.com/antirez/voxtral.c),
[official weights](https://huggingface.co/mistralai/Voxtral-Mini-4B-Realtime-2602).
