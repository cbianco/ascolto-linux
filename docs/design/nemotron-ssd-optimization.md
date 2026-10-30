# Nemotron SSD latency work

The subsequent cache correction, eight-reader Metal default, exact asynchronous
prefetch and expanded validation are described in
[parallel expert validation](nemotron-parallel-validation.md). The measurements
below retain the earlier optimization stages for comparison.

## Method

Keep the original BF16 package and the 4 GiB expert cache fixed. Compare the
same public/synthetic WAV and prompt on the same device, without concurrent
inference or conversion. Record repeated transcription, process footprint,
expert bytes read, and time in encoder, decoder, cache acquisition and expert
compute. Loading and package verification are outside the inference timer.
These fixtures measure implementation behavior, not general recognition accuracy.

## Research informing the experiments

- [FlashMoE](https://arxiv.org/html/2601.17063v1) groups prefill work by expert and
  studies SSD cache replacement. Our cache already holds all 128 experts of one
  layer, so grouping within a callback primarily saves dispatch overhead;
  reducing separate whole-model prefill passes can also save SSD reads.
- [MoE-Infinity](https://arxiv.org/html/2401.14361) distinguishes request-local
  expert reuse in prefill and decode. Its RAM-to-GPU results do not predict SSD
  performance on unified memory. Speculative prefetch must account for wasted
  bandwidth; demand routing remains authoritative.
- [LLM in a flash](https://arxiv.org/html/2312.11514v3) motivates temporal reuse,
  larger reads and measuring storage throughput. Its approximate neuron
  prediction is not adopted here.

## Experiments

The original executable is retained in the ignored validation directory.
The 2026-10-30 English reference measured 28.421 / 27.317 s, with identical
transcripts, 194,129,166,336 bytes read over both runs and a 9.175 GB peak
process footprint. No expert pins remained at teardown.

Private diagnostic controls allow ablation of GPU row batching and packed
prefill. `ASCOLTO_NEMOTRON_SSD_PROFILE=1` reports content-free phase and layer
measurements. `ASCOLTO_NEMOTRON_SSD_BATCH_ROWS=1` retains one-row expert
submissions. The shared-cache implementation caps groups at 31 rows to retain
the pinned Metal matrix-vector kernel: 32 rows would select a different
matrix-matrix kernel with different activation rounding. Cache leases span
all rows and survive synchronous graph completion and result download.

The shared path acquires each distinct expert once per microbatch, protecting
the complete union of up to 128 experts, and maps all routed rows to those leases. This improves native
reader utilization without additional expert storage. The compact CPU/mapping
fallback retains its one-row execution.

Packed host prefill combines text and supplied audio through the pinned
decoder's existing mixed-input mask, preserving positions and causal order.
A single engine-local prefix snapshot stores both attention and Mamba state
at the exact text-prefix boundary, before audio. It is keyed by the exact token
vector and capped at 256 MiB; the English fixture needs 50,201,844 bytes.
Subsequent turns restore this state after the normal sequence reset. Different
prompts invalidate it, capture failure skips caching, and partial restore
poisons the context. No utterance audio state is retained in the snapshot.

Packed prefill and prefix caching are enabled by default for the SSD decoder.
`ASCOLTO_NEMOTRON_SSD_PACKED_PREFILL=0` restores the original three-pass prefill;
`ASCOLTO_NEMOTRON_SSD_PREFIX_CACHE=0` keeps packed prefill without a snapshot.
These are private diagnostic ablations, not persisted app preferences.

## Measured results

M3 Max / 36 GB, same 3.410 s English fixture, original BF16 package, 4 GiB cache:

| Variant | First / second inference | Expert bytes read, both runs |
| --- | --- | --- |
| Original executable | 28.421 / 27.317 s | 194,129,166,336 |
| Grouped GPU rows only | 26.465 / 25.347 s | 194,129,166,336 |
| Packed prefill and grouped rows | 20.819 / 18.991 s | 131,148,939,264 |
| Grouped reads, packed prefill, no prefix snapshot | 15.341 / 13.895 s | 131,148,939,264 |
| Grouped reads and prefix snapshot | 19.205 / 11.829 s | 142,064,713,728 |

All return the same English sentence. Profiling attributes about 20.23 s of the
original prefill to cache acquisition and 2.51 s to expert compute. Grouped
rows reduce that compute component to 0.83 s; packed prefill reduces prefill
reads from about 78 to 46 GB; grouped reads further reduce acquisition time.
The encoder takes roughly 0.04–0.08 s on this fixture.

Prefix caching trades a slower first turn for faster subsequent turns with the
same prompt, which suits call transcription. Its first turn must stop at the
prefix boundary to capture Mamba state. One-shot workloads can use the private
no-prefix-cache ablation when measuring this tradeoff. Snapshot capture/restore
took about 2 / 1 ms. Observed peak footprint remained about 9–10 GB, including
the additional snapshot; the expert cache size is not a process-memory limit.

Separate Italian audio with the same prefix also passed: the first turn took
21.372 s and a different second utterance took 12.110 s. The second utterance's
transcript exactly matched both runs without prefix caching (15.855 / 15.455 s).
These small fixtures
are not an accuracy benchmark, and even the optimized mode is not real time.

## Correctness gates

The 67-row synthetic graph spans multiple GPU batches and compares eight
intermediate results against the resident one-row kernel, bit for bit on Metal.
Coverage includes duplicate expert IDs, slots above 127, compact mapping fallback,
and read failures after earlier acquisitions succeeded.
Failures abort downstream work, release all pins and poison the context.

The generated prefill method itself is compiled in a small C++ fixture covering
packing boundaries, vectors and masks, positions, final logits, resume, exact
prefix keys, two different audios, cache budgets, capture and partial-restore
failures, and default/ablation controls. It does not merely reimplement the
packing algorithm in Python. Real-model repeated serves validate the native
hybrid state serialization in addition to these control-flow tests.

The final macOS suites pass the 2,088 cache checks, 74 CPU graph checks,
88 Metal wide/fault checks, 83 mapping-fallback checks, 93 F32 convolution
checks, seven native repacker tests and eleven generated-prefill tests.
Linux arm64 separately rebuilt the decoder and BF16 adapter, passing cache,
CPU graph/wide-fault, convolution, generated-prefill and preparation ABI tests.
Linux recognition performance was not measured. The final app bundle passes
signature verification and the BF16 adapter retains only its two public C APIs.
Two complete load/transcribe/unload cycles through the final application ABI
also pass on a 512 KiB thread, with the expected English sentence and zero
remaining expert pins. Their cold-turn times were 30.473 / 19.947 s while other
verification work overlapped the run; these are correctness checks, not the
uncontended measurements in the table. Storage contention remains significant.
