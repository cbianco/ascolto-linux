# Nemotron BF16 expert streaming

Implemented and checked on 2026-10-29. The optional
`nemotron-3.5-bf16-ssd` package runs the complete transcription path natively,
with a bounded expert cache. It preserves the original checkpoint's BF16
weight values. It remains experimental because the measured SSD latency is
substantially longer than the audio duration.

The [2026-10-30 latency work](nemotron-ssd-optimization.md) adds grouped reads,
GPU microbatches, packed prefill and bounded prefix-state reuse. The historical
measurements below describe the original implementation.

## References and provenance

- [antirez/ds4](https://github.com/antirez/ds4/tree/ee7e8405232c8971000cc7ba2b945b00bf6d20d3):
  explicit parallel pread, fixed expert caches and pins through GPU completion.
  DS4's supported DeepSeek/GLM layouts and flags are not a Nemotron backend.
  No DS4 source is linked into Ascolto.
- [NVIDIA reference checkpoint](https://huggingface.co/nvidia/NVIDIA-Nemotron-3.5-Transcribe-30B-A4B/tree/a99e33f698257b8b69ca7834f19c73582eb1fa7e):
  the downloaded 67,496,013,734-byte safetensors was verified against SHA-256
  `03142dde5176c1d4df21ee829642745a1d670ea4d79e1a4684c64d53195acff2`.
- [NeMo-Speech.cpp](https://github.com/NVIDIA/NeMo-Speech.cpp/tree/66d50d91b75a406becd95fbaf39d2c9c13e95d7c),
  with its patch series over llama.cpp
  `bd4f514db14d87fded667787a7a963bfbaa98e89`.

Full precision here means retaining original BF16 weights, not expanding every
weight to FP32 or using a four-bit approximation. Required norms and transformed
parameters follow the reference converter. The perception encoder expands
BF16 bits into F32 so values outside F16's range survive; its subpixel Conv1D
also uses F32 storage and im2col. Unused MTP/speculation and CTC timestamp heads
are omitted. Attention, Mamba, routing, shared experts and the language-model
head remain part of the transcription engine.

## Storage and I/O scale

The backbone has 23 ordinary MoE blocks, 128 experts per block and six selected
experts per token. Each expert has up/down BF16 matrices with ReLU-squared:

```text
2 matrices × 2,688 × 1,856 elements × 2 bytes = 19,955,712 bytes/expert
23 blocks × 128 experts = 58,749,616,128 bytes of ordinary routed weights
23 blocks × 6 selected experts = 2,753,888,256 bytes/token with zero cache hits
```

A 4 GiB budget holds 215 complete pairs (4,290,478,080 payload bytes). This is
not a bound on total process memory: encoder, dense weights, states, scratch,
metadata and OS overhead are additional. Cache coverage is not a hit-rate
prediction. Audio/text prefill can touch nearly all experts in a layer.

At an illustrative effective 5 GB/s, a cold token's I/O alone limits decoding
to about 1.8 tokens/s. That arithmetic does not predict complete ASR latency.
The measured short English turn read about 97 GB through prefill and generation.
Memory capacity and real-time throughput are separate results.

## Optional native installation

The manifest separates downloaded inputs from installed files. The worker:

1. Downloads the pinned original safetensors and license into private staging.
2. Verifies every input; it retains the same source descriptor for repacking.
3. Invokes the adapter's private C `ascolto_stt_prepare_package_v1` entry point.
4. Copies/casts/transposes with bounded scratch using the pinned recipe, header
   files and 58,624-byte constant payload. Q/K RoPE permutation is a row reorder;
   small reference-transformed values, such as Mamba A-log, are precomputed.
5. Checks complete output sizes and SHA-256 values, synchronizes files, then
   independently verifies the installed manifest. Only then removes the input,
   synchronizes staging, checks its identity and publishes with renameat.

The repacker uses retained directory descriptors, openat/O_NOFOLLOW/O_EXCL,
checked extent arithmetic, deterministic zero padding and exclusive temporary
publication. Cleanup tracks only files created by that invocation. Cancellation
is checked during hashing, reading, transposition, writing and before commit.
No READY state is published after failure. The ordinary startup scan removes
known incomplete staging artifacts; unknown files are not traversed or deleted.

The installed decoder is 63,181,504,832 bytes; the F32 encoder is 2,934,467,328
bytes. With the license, installation occupies 66,115,974,854 bytes. Peak
installation storage is about 133.61 GB plus the normal margin because original
and prepared data coexist until verification. `preparation_bytes` means this
additional output storage beyond the download, not resident memory.

The reference development scripts use a separate Python/Torch environment, check
the original digest, reconstruct the pinned converter and compare the Python
sources actually executed. They verify every BF16 expert bit and 848 used
perception tensors, then record full output hashes. The C++ repacker produced
those same full-file hashes from the real checkpoint. App installation and
inference use no Python, Torch or converter subprocess.

## Decoder integration

`expert_cache.c` owns a read-only regular-file descriptor and a fixed allocation.
It validates expert extents, uses eight native readers on Metal (four elsewhere,
with a private override capped at sixteen), orders misses by
file offset and never marks a partial read valid. macOS uses F_NOCACHE; Linux
advises random reads and drops consumed file-cache extents. Acquisition is
serialized; release/statistics may run concurrently. Leases pin slot generations
until their owner finishes. Destruction requires external quiescence.

Acquisition preflights the complete request and pins every resident hit before
choosing eviction victims. Duplicate requests share a read while retaining
separate lease ownership. Insufficient capacity returns busy without changing
cache state. Shared GPU batches protect their entire union of up to 128 experts.
On Metal, exact next-batch reads overlap current GPU computation when both
unions fit the same fixed cache; no speculative expert selection or extra
weight bank is used. Read futures join before either batch releases its leases,
including on failure. Resource pressure falls back to synchronous acquisition.

The private pinned llama loader routes expert metadata into a separate no-alloc
context, outside `ctx_map`. Those tensors are excluded from buffer allocation,
load_all_data, mmap, lazy mapping and prefetch. Tensor names/counts and progress
bytes remain coherent. The loader asserts that all 46 ordinary projections have
metadata only and zero expert bytes have been read before decoding.

`streamed_moe.cpp` replaces only routed expert products. The exact ungated
Nemotron ReLU-squared branch, without bias/scale/LoRA, fuses
up → ReLU² → down per row and holds one pair lease across the branch. Other
combinations retain the separate products and upstream scale/bias/LoRA path.
Routing, tie behavior, expert weights, aggregation, shared experts, attention
and Mamba state remain in the existing graph.

On supported Apple Silicon, one non-owning Metal buffer wraps the cache's same
page-aligned storage. Up/down tensors use paired-slot strides, and IDs are remapped
to lease.slot, including slots above 127. No expert payload is recopied to GPU.
The rounded allocation must fit one device buffer; the multiview path is avoided.
All tensor spans are checked against its bounds. Mapping failure falls back to
bounded compact copies. CPU uses compact selected slots with its own backend
and threadpool. No callback reuses the waiting outer CPU pool.

Callbacks return immediately for ith != 0 and catch all C++ exceptions internally.
A failed read/compute sets a sticky atomic error. The CPU abort callback returns
GGML_STATUS_ABORTED through the scheduler, rather than relying on cb_eval=false
(which does not propagate an error in the pinned scheduler). SALM checks the
sticky state before/after decode and before serve: a partially advanced context
cannot be reused. The adapter rejects failed/incomplete text. Products are
synchronized and destroyed before the shared Metal wrapper, then cache storage.
Private guards fix the upstream mapped-buffer wrapper's missing null check
and release unregistered Metal views/residency sets when mapping fails. A real
Metal fault test checks allocation, view and residency failure stages without
retaining buffers.

## Validation and practical limits

- The independent single-block prototype passed 2,088 CPU/Metal checks, including
  cache hits, pin protection, cancellation after a real partial read, concurrent
  release/statistics, invalid extents and truncation.
- The multi-layer graph compares eight intermediate separate/fused results on
  the same resident backend and rejects a zero-output control. CPU and Metal
  errors were zero for these fixtures. Wide-cache Metal reaches slot 199;
  mapping failure, layer changes and reuse after synchronization also pass.
- Read faults after a successful shared Metal compute propagate ABORTED, skip
  downstream work, leave zero pins and poison the context. Fused and separate
  paths are covered. Full SALM short-read failure also rejects the next serve.
- F32 Conv1D matches independent calculations on CPU/Metal, including BF16 values
  far below the F16 range. Repacker fixtures cover special BF16 bits, RoPE and
  expert permutation, casts, padding, corrupt resources/input, symlinks,
  exclusive outputs, retained verified descriptors and short writes.
- Manager fixtures exercise actual preparation ABI calls, input removal only
  after verification, cancellation during preparation and verification, corrupt
  output, missing symbol, staging substitution, restart cleanup and removal.
- The real native BF16 package passed two direct transcription runs and two
  load/transcribe/unload cycles through the app ABI on a 512 KiB thread.
  Production VAD, Whistle identity, SQLite, drain and silence also passed.

On the M3 Max / 36 GB development machine, the 3.410 s English fixture produced
its expected sentence. Initial bounded copies took 40.135 / 36.925 s with a
10.06 GB peak. Shared-cache runs took 29.538 / 27.860 s with a
9,155,878,184-byte peak. A separate run measured a 9,943,867,264-byte peak
footprint and 8,969,355,264-byte peak RSS. Observed process memory is roughly
9–10 GB for these fixtures, not a guaranteed total limit. Under concurrent SSD
repacking, one switching run took 52.4 s. These are small development smoke measurements, not a
controlled benchmark or an accuracy evaluation. Startup verification, model
loading and shader compilation add time. Metal currentAllocatedSize may describe
the same pages as process footprint and must not be added to it.

The mode demonstrates native full-weight transcription within a small fraction
of the checkpoint's memory footprint. It does not demonstrate sustainable live
transcription: delay remains high, and prolonged inference can exhaust the
bounded capture queue. Existing overflow padding and final-drain rules remain
unchanged. No microphone or cloud traffic was used in these tests.

The Linux arm64 build passes the updated synthetic suites and real checkpoint
repacking, reproducing both complete installed-file hashes in 235.39 s with
63,700 KiB maximum RSS. CPU recognition performance and physical capture on
Linux remain unmeasured. The bundled adapter has only its two intended C entry
points and system runtime dependencies. The generated BUILD-ENVIRONMENT.json
records the Python/package versions used only for reference asset development.
