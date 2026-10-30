# Nemotron parallel expert validation

## Changes under test

The cache reserves the complete requested union before selecting victims. In
particular, with A/B resident, requesting C/A performs one miss and one hit;
loading C cannot evict the later requested A. Duplicate misses share one read
but own separate pins. Capacity failures leave cache state unchanged.

Metal uses eight SSD readers and exact next-microbatch prefetch by default.
CPU defaults remain four readers with prefetch disabled. Private diagnostic
controls are `ASCOLTO_NEMOTRON_SSD_READERS=1..16` and
`ASCOLTO_NEMOTRON_SSD_PREFETCH=0|1`. GPU kernels already execute routed experts
and rows in parallel. Consecutive model layers depend on earlier outputs and
cannot be independently scheduled without changing inference semantics.

Prefetch overlaps reads for already known router IDs with current computation.
Both unions must fit the existing cache; otherwise acquisition is synchronous.
No predicted routing or additional weight bank is introduced. Current and next
leases remain pinned through graph completion and the asynchronous join, also
on exceptions. Read failure zeros the custom output, aborts downstream work and
poisons the context. Tests cover asynchronous failure after successful work.

## Reader tuning

M3 Max / 36 GB, installed original BF16 package, fixed 4 GiB expert cache,
3.409625 s English fixture. Each variant loads a fresh engine and transcribes
twice. The following elapsed times exclude model loading and verification.
No concurrent inference or heavy build was run during these measurements.

| Readers | Prefetch | First turn (s) | Second turn (s) |
| --- | --- | --- | --- |
| 4 | Off | 24.861 | 11.498 |
| 4 | On | 19.553 | 11.376 |
| 8 | Off | 17.626 | 13.610 |
| 8 | On | 17.386 | 10.136 |
| 16 | Off | 17.915 | 10.317 |
| 16 | On | 16.876 | 10.329 |

A reverse-order confirmation measured 16.900 / 9.931 / 9.886 s for eight
readers with prefetch, followed by 19.917 / 11.628 / 11.352 s for four readers
without it. Eight readers improve the two warm turns by about 14% while sixteen
show no consistent extra benefit. All outputs exactly match. First-use storage
and compilation effects make the first table's cold comparison less reliable.
These are small tuning fixtures, not statistically broad performance estimates.

## Independent native CPU reference

The diagnostic decoder supports a guarded ordinary CPU reference requiring both
`ASCOLTO_NEMOTRON_TEST_CPU_REFERENCE=1` and its private config flag. It requires
zero SSD cache, forces ordinary CPU operators, disables GPU/offload/repacking,
and maps the complete decoder read-only without whole-file WILLNEED. It cannot
be enabled through production preferences. Normal runtime defaults are unchanged.

The short English fixture returns the exact optimized transcript through this
ordinary native path in 130.859 s. Its observed RSS is 13.937 GB; private process
footprint is only 0.402 GB because mapped weights use different accounting.
Those figures must not be interpreted as a 0.402 GB total-memory implementation.
This checks the pinned native decoder without custom SSD operators; it is not
an independent NVIDIA/PyTorch reference or a corpus-wide numerical proof.

## Corpus protocol

`prepare_nemotron_eval.py` chooses public LibriSpeech clips deterministically
before inference: twelve test-clean and eight test-other clips, twenty distinct
speakers, plus two 50-second concatenations, two seeded 10 dB noise variants and
two overlap variants. Original archive checksums, clip hashes, selection and
tool versions are recorded. The 26 clips contain 309.545 seconds of audio;
24 have single-stream references with 774 words. Overlap is excluded from
WER/CER but retained for execution and latency checks.

Each variant processes the complete ordered manifest in one persistent engine.
The optimized variant uses current defaults. The ablation baseline uses the
same executable and corrected cache with one-row GPU work, original three-pass
prefill, no prefix snapshot, no prefetch, and four readers. Thus the comparison
tests combined optimization effects; it does not isolate this turn's prefetch.
The reader-tuning table above isolates that narrower change more directly.

The scorer retains failed, missing and empty scored outputs in the denominator,
reports normalized WER/CER, exact text differences, latency, and observed memory
trends. Its first/warm labels reflect run order, not measured cache-hit counters.
The corpus is a small public English subset; it does not establish Italian call
accuracy or performance on the full LibriSpeech test set.

## Sustained queue protocol

`replay_nemotron_queue.py` compiles unchanged `capture_buffer.c` and replays the
measured delays through its actual 30-second paired queue. Indexed synthetic
20 ms frames verify ordering, equal-duration overflow silence and final drain.
The default duration is thirty virtual minutes, with alternating prescribed
turns and 400 ms gaps. It does not run microphones, VAD or live ASR.

Warm delays repeat and omit other consumer work, so throughput is optimistic.
Fully lost turns skip inference; partly retained turns pay full measured delay.
Logical backlog includes already discarded time and can exceed the physical
30-second buffer. Correct timeline accounting does not mean speech survived
overflow: discarded audio remains lost. No buffer enlargement or unsolicited
audio persistence is introduced to conceal that limitation.

## Optimized corpus observations

All 26 clips completed without failed or empty outputs. On the 24 scored
clips, WER is 17/774 = 2.196% and CER is 34/3290 = 1.033%. The overlap clips
are not included in those denominators. Total inference time is 714.032 s
for 309.545 s of audio (weighted real-time factor 2.307). The two 50-second
clips take 90.039 and 88.883 s. This is not sustained real-time throughput.

Observed RSS ranges up to 8.327 GB and finishes at 8.267 GB, 13.6 MB below
the first-turn sample. Process footprint rises from 9.211 to 9.387 GB; its
recorded lifetime peak is 9.395 GB. Varying graph shapes and allocator caching
can affect these observations; one sequence does not prove absence of a leak.
The fixed 4 GiB expert cache is not a total process-memory cap.

## Public comparison and queue results

The ablation baseline and optimized runs both return all 26 outputs, with
identical normalized transcripts, WER and CER. Three raw outputs differ only
in punctuation/hyphenation. No accuracy regression is observed on this subset;
this is not a claim of bitwise full-model logits or general call accuracy.

| Metric | Ablation baseline | Optimized |
| --- | --- | --- |
| Total inference, 309.545 s audio | 1,365.096 s | 714.032 s |
| Median clip latency | 46.607 s | 24.213 s |
| Weighted real-time factor | 4.410 | 2.307 |
| Word error rate | 2.196% | 2.196% |
| Character error rate | 1.033% | 1.033% |

The combined optimizations reduce total elapsed inference by 47.7%. This
comparison includes batching, packed prefill, prefix caching and reader/prefetch
changes; the approximately 14% short-fixture gain above isolates the latest
reader/prefetch configuration more directly. Runs are sequential on one host,
not interleaved statistical performance trials.

The deterministic queue replay spans 1,809.72 virtual seconds (146 prescribed
turns, 400 ms gaps). Baseline overflow discards 1,526.58 s of timeline versus
1,235.68 s optimized, a 19.1% reduction. This loss includes gaps as well as
speech. Fully lost turns decrease from 118 to 91; remaining ASR calls increase
from 28 to 55. Final drain decreases from 150.49 to 90.04 s.

Both replay variants retain at most 30 s of audio, verify every indexed PCM
frame or corresponding paired silence, deliver exactly 28,955,520 samples on
both channels, and end with an empty queue. Despite correct accounting,
overflow is still severe: this BF16 SSD configuration is not suitable for
continuous live transcription on the measured machine. The replay uses fixed
measured timings and does not establish physical capture or thermal stability.

## Final native checks

The Metal defaults use registry identity, independent of whether the device's
display name is populated. Native GPU verification requires access to Metal;
the restricted test sandbox can expose a backend without a usable GPU queue.

macOS passes 2,824 cache checks, 93 CPU graph checks, 162 Metal prefetch checks,
167 Metal wide/prefetch checks and 102 Metal mapping-fallback checks. The graph
comparisons have zero observed error; fault cases leave no pins and prevent
downstream publication. F32 convolution, seven repacker tests, eleven generated
prefill tests, eleven scoring tests and eleven native queue replay tests pass.
Model-manager/preparation/ABI and portable capture regressions also pass.

Linux arm64 separately compiles the current decoder and BF16 adapter, passing
the cache, CPU graph (93 plus 98 wide/fault), convolution, prefill, scoring,
queue replay and preparation ABI checks. The final registry-identity change is
recompiled and its CPU/wide tests rerun. No Linux recognition timing is claimed.

The final macOS app is rebuilt and its signature verified. The BF16 adapter
retains only the two intended public C exports. Two complete real-model
load/transcribe/unload cycles through the application ABI pass on a 512 KiB
thread, using private temporary package clones and the expected English text.
These fresh-load cycles measure 17.601 and 19.543 s, not warm prefix reuse.
The temporary configuration and clones are removed after verification.

## User-provided real conversation samples

Six local Italian excerpts from four user-provided recordings cover three MIC
and three SYS segments: 9.280, 10.496, 7.520, 10.496, 29.952 and 29.952 s,
97.696 s total. Channels are separated according to the archive contract.
Selection, offsets, source/WAV hashes, transcripts and detailed results stay in
the ignored private evaluation directory with 0700 directories and 0600 files.
Original recordings are unchanged; no conversation text is copied into these
versioned notes. Existing machine transcripts are selection aids, not human
ground truth.

| Metric | Ablation baseline | Optimized |
| --- | --- | --- |
| Total inference | 364.631 s | 217.043 s |
| Median excerpt latency | 55.310 s | 35.100 s |
| Weighted real-time factor | 3.732 | 2.222 |
| Failed / empty outputs | 0 / 0 | 0 / 0 |

Total elapsed inference decreases by 40.5%. Five of six raw transcripts match
exactly. The remaining transcript renders an acronym joined rather than as
separate letters; the normalized character sequence with spaces removed is
identical. This is a consistency observation, not a human accuracy score.
WER/CER and error deltas are explicitly unavailable without a verified reference.
These are single sequential runs on a small selected sample, not full-call or
population-level accuracy/performance estimates.

The optimized run's peak footprint is 9.306 GB. Both decoder teardowns leave
zero expert pins. The cache remains 4 GiB. Short synthetic warm-turn timing
must not be generalized to arbitrary Italian speech or fresh model loads.

A separate native queue replay with these measured delays spans 1,818.48
virtual seconds. Baseline/optimized discarded timeline is 1,421.82/1,286.16 s,
with 75/47 wholly lost prescribed turns and 149.32/89.98 s final drain. Both
verify paired silence, exact sample timelines and an empty final queue. This
uses prescribed turns and gaps, not the recordings' original pause distribution
or live VAD. It again shows that faster inference does not yet make BF16 SSD
suitable for continuous live transcription on this machine.
