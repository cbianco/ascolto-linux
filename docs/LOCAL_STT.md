# Optional local transcription engines

Whistle is included and selected by default. Settings → Transcription → On device
adds five optional local packages. Nemotron variants share one card with a
8 / 16-bit selector; browsing does not activate or download a variant. Downloading verifies every manifest artifact
before publication and does not activate it. Use model saves an independent
next-session preference. A running session keeps its decoder until its final
queued audio and VAD turns have drained.

| Engine | Complete download | Runtime | Initial text API |
| --- | --- | --- | --- |
| Whistle | Included, about 17 MB | Existing Needle C API | Existing word data |
| Whisper large-v3-turbo Q5 | 574,041,195 bytes | whisper.cpp v1.9.5 | Completed mono turn |
| Parakeet TDT v3 Q8 | 940,663,680 bytes | parakeet.cpp | Completed mono turn |
| Qwen3-ASR 1.7B | 4,703,112,789 bytes | antirez/qwen-asr | Completed mono turn |
| Nemotron 3.5 Transcribe Q8_0 SSD | 36,675,056,006 bytes | Native NeMo decoder with bounded SSD cache | Completed mono turn |
| Nemotron 3.5 Transcribe BF16 SSD | 67,496,016,428 bytes; 66,115,974,854 installed | Native NeMo decoder with bounded SSD cache | Completed mono turn; experimental high delay |

Optional decoders receive the same gated, VAD-completed 16 kHz mono turns as
Whistle. SYS precedes MIC, processing stays serialized, and capture only enqueues.
Whistle continues to extract speaker embeddings. These first adapters return
text and a language only when available. No word timestamps, probabilities or
first-token measurements are fabricated. Existing lexical echo and repetition
filters still run; silence never bypasses the production gate/VAD. Parakeet uses
automatic language detection and does not apply custom keyword prompts.

Whisper and Parakeet use Metal/CPU on macOS and CPU on Linux. Qwen uses native C
with Accelerate on macOS and OpenBLAS on Linux. Recognition uses `ascolto_stt_get_api`, the versioned C ABI. The BF16 adapter
also exports the private versioned `ascolto_stt_prepare_package_v1` installation
entry point; no runtime internals are exported. Its complete runtime and its own ggml
revision are statically linked with hidden symbols. The app loads one decoder at
a time with `RTLD_LOCAL`, releases the model at session end, then unloads the
adapter. Parakeet explicitly calls `pk::shutdown_backend()` after freeing its
model. There is no Python inference or child-process ASR protocol.

Nemotron offers Q8_0 SSD and BF16 SSD as independently downloadable packages.
Both use a bounded 4 GiB expert cache and native CPU/Metal operations. The Q8_0
package has an F16 perception encoder; BF16 preserves original perception values.
The former NVFP4 package and adapter have been retired and are no longer offered.
Existing transcript provenance is retained; a saved retired ID fails explicitly
until the user chooses a supported model, without silently switching engines.

The adapter uses one decoder slot, a 4,096-token context, a 512-token prefill batch
and one encoder window per batch. Speculative decoding is disabled. Greedy output
must finish its turn and fit the text API; exhausted generation/context fails
instead of committing truncated text. Finite mono 16 kHz audio is limited to
60 seconds per request; production VAD uses shorter turns. Language hints and
vocabulary prompts are supported. Whistle continues to own speaker identity.
CUDA embedding remains disabled because the pinned upstream allocation failures
can terminate the process rather than returning an inference error.

## Model storage and failure behavior

`resources/local-stt-manifest.json` records pinned source revisions, complete file
sizes and SHA-256 digests. `scripts/generate_local_stt_manifest.py` validates and
embeds that manifest. Update both files intentionally when changing a package.
The bundled [third-party notices](../resources/LOCAL_STT_NOTICES.md) record runtime
pins, model provenance, quantization and licenses.

Packages live in `<config-parent>/local-models/<model-id>/`. Downloads use a
private sibling `.download` directory, HTTPS-only libcurl requests, bounded
redirects, exact byte limits and SHA-256 verification. Completion publishes the
verified directory; cancellation/error removes partial files. Startup removes
known interrupted partials, checks installed packages on a worker, and publishes
copied progress/status snapshots. Model selection and disk work run on native
background action queues. UI reads never wait for model loading, hashing or ASR.
Startup verifies the selected package first. Every process performs a full
SHA-256 check before trusting an installed package. Later sessions reopen all
artifacts without following symlinks and may reuse that process-local result
only when directory/file identity, size, mode and nanosecond modification/change
times are unchanged. A changed package is hashed again. Download/removal
invalidates this memory-only cache; no persisted marker bypasses startup hashes.
Descriptors remain open through loading, followed by a namespace/identity check
before the session becomes active. This removes repeated multi-gigabyte hashing
at call start while retaining detection of replacement and in-place mutation.

macOS shows checking/loading explicitly while preparing the selected model.
The control socket reports model state/message/error, detected input activity,
MIC/SYS sample counts, capture overflow and transcription errors through copied
snapshots. Cold verification and model loading can still delay a first call;
keeping the app open allows verification to finish before microphone activation.
macOS/Linux regressions cover repeated-session hash reuse, selected-first scan,
same-size edits with restored modification time, directory/file replacement,
symlinks and mutation during hashing/loading (246 model-manager, 104 preparation
and 10 missing-ABI checks). macOS also verifies content-free status reads while
the processing lock is held and retains the stop/drain lifecycle regressions.

One download runs at a time. Cancel and retry target only that package. Removing
an optional model requires it to be both inactive and unselected; Whistle cannot
be removed. A missing, corrupt or incompatible selected package prevents session
start, preserves its saved ID, and offers explicit download retry or Whistle
selection. Inference errors remain visible, mark the call failed and suppress
automatic AI processing; retained audio can be recovered explicitly. There is no
silent engine or cloud fallback. Local model actions never mutate cloud upload
consent, cloud transcription choices or recap preferences.

## Build and verification

`make` prepares pinned native source dependencies and packages the adapters.
`make local-stt-deps` builds them explicitly. Source checkouts/builds are ignored
under `build/local-stt`; adapter outputs and license texts are under
`bin/local-stt`. The script can reuse pinned local source checkouts through
`ASCOLTO_WHISPER_SOURCE`, `ASCOLTO_PARAKEET_SOURCE` and `ASCOLTO_QWEN_SOURCE`.
Nemotron additionally accepts `ASCOLTO_NEMOTRON_SOURCE` and
`ASCOLTO_SENTENCEPIECE_SOURCE`. Its dependencies require CMake 3.26 or newer.
CPU is the Linux default and Metal the macOS default.
`ASCOLTO_NEMOTRON_CUDA=ON` fails with an explanation until the upstream CUDA
allocation path can return recoverable errors to the application.
It reuses the existing pinned benchmark sources when present. No optional weights are
downloaded by this build step. macOS bundles these libraries under Resources/local-stt,
with system framework dependencies only.

`make test-local-stt` uses tiny synthetic artifacts and a fake native adapter,
without network, microphones, speech weights or cloud credentials. It checks
integrity, progress, cancellation, explicit activation, session pinning, removal
protection, missing-model errors, unknown persisted IDs and cloud independence.
The explicit `make test-local-stt-download` smoke check downloads only one pinned
6 KB public model configuration to verify HTTPS transport, progress and integrity;
it never downloads speech weights or activates a model.
AppKit previews cover both tabs and all catalog states; GTK keeps the same native
controls and independent cloud surface.

For explicit verification with already installed packages in a temporary config
folder, build and run:

```sh
make bin/ascolto-local-stt-runtime bin/ascolto-local-stt-pipeline
./bin/ascolto-local-stt-runtime /absolute/temp/config.json /absolute/synthetic-speech.wav
./bin/ascolto-local-stt-pipeline /absolute/temp/config.json /absolute/synthetic-speech.wav /absolute/whistle.cact
```

These macOS tools never capture or upload audio. The runtime check switches
Whisper → Parakeet → Qwen → Whisper → Parakeet in one process. The production
check runs on a pthread with a 512 KiB stack, matching the macOS audio consumer,
and feeds synthetic speech and silence through VAD, the selected ASR, Whistle
speaker embeddings and SQLite. It also changes the next-session preference while
each alternative remains active. Install packages according to the manifest's
filenames under the temporary configuration's `local-models/` directory first.

On Apple Silicon, the Whisper, Parakeet and Qwen adapters and production pipeline checks passed,
including repeated switching, observed Whisper Metal and Parakeet MTL0, successful
backend teardown, retained speaker identities and zero turns on silence. Existing
macOS harness/rooms, lifecycle, socket, storage and mocked AI suites passed.
These are behavior checks with synthetic speech, not recognition accuracy results.
No real microphone or cloud request is part of this verification.

`make test-nemotron-native` checks the real Nemotron library's ABI and repeated
failed-load teardown with a 512 KiB consumer stack, without speech weights. The
synthetic model-manager suite checks both decoder and encoder integrity, complete
package progress, explicit selection and protection of the active package.
To exercise a single preinstalled model, append its ID to the existing tools:

```sh
./bin/ascolto-local-stt-runtime /absolute/temp/config.json /absolute/speech.wav nemotron-3.5-q8
./bin/ascolto-local-stt-pipeline /absolute/temp/config.json /absolute/speech.wav /absolute/whistle.cact nemotron-3.5-q8
```

### Historical NVFP4 validation (retired package)

The former NVFP4 package was downloaded and verified in an ignored temporary
validation directory. On an M3 Max with 36 GB RAM, two consecutive English and
two Italian synthetic-speech sessions passed, as did production VAD → ASR →
Whistle speaker identity → SQLite and the silence gate, on a 512 KiB thread
stack. The English fixture matched its expected sentence; the Italian check
required the expected vocabulary term, not a human accuracy score. Observed
inference times were 4.08–4.14 s for 3.41 s English audio and 4.96–6.01 s for
3.45 s Italian audio. The process peak memory footprint was about 19.45 GB.
These are small smoke measurements under development load, not a controlled
benchmark or evidence of sustained real-time performance. Model verification,
loading and first-use Metal compilation add startup time. No microphone,
cloud request or production settings were used by these checks.

Linux arm64 CPU build, isolated model-manager checks, native load/unload smoke
and the BF16 block prototype also passed. The x86_64 CPU adapter compiled;
full Linux desktop/capture and real-model recognition were not rerun. CUDA
remains excluded from embedding and was not tested.

### Current SSD verification

`make test-nemotron-ssd` checks the paired expert cache, multi-layer BF16 graph,
F32 convolution and native package preparation. It uses tiny synthetic weights,
not network downloads. The graph fixture checks separate and fused projections
at every stage against the resident backend and rejects a zero-output control.
Metal variants also cover slot 199, mapping failure with bounded-copy fallback,
and read failure after a successful shared-cache compute:

```sh
./build/local-stt/nemotron/bin/ascolto-nemotron-streamed-graph metal
./build/local-stt/nemotron/bin/ascolto-nemotron-streamed-graph metal-wide
./build/local-stt/nemotron/bin/ascolto-nemotron-streamed-graph metal-fallback
./build/local-stt/nemotron/bin/ascolto-nemotron-reference-conv metal
./build/local-stt/nemotron/bin/ascolto-nemotron-metal-mapping
```

The separate `ascolto-nemotron-constructor-fault` CMake target takes an installed
package directory and tests late constructor failures after decoder allocation;
it is excluded from normal builds and requires real weights.

Eight late constructor faults also passed with the real package. After an
autorelease-pool drain and a fixed settling interval, the final four process
footprint measurements stayed within about 18 MB; remaining Metal allocations
were constant at 475,136 bytes. Initial framework/allocator retention was about
0.78 GB, rather than a retained model. The test records immediate and settled
footprints and Metal `currentAllocatedSize`; residency behavior was not changed
to obtain a passing result. The user-requested GPT-6 Astra review (high reasoning)
reviewed the NVFP4 and successive BF16 milestones, including fault, lifecycle
and numerical-gate corrections.

Linux x86_64 was verified in an isolated Ubuntu 24.04 container: all baseline
harness suites, drain/concurrency regressions, the model manager and real GTK
callbacks/previews under Xvfb passed. All three CPU adapters compiled with Clang;
each ELF library exports only the versioned Ascolto entry point. An initial GCC
compiler process crashed under x86_64 emulation; the Clang builds completed. No
physical PipeWire/GNOME session or Linux recognition performance was measured.
The optional real-microphone macOS suite was not run.

## BF16 SSD package

`nemotron-3.5-bf16-ssd` downloads the pinned original NVIDIA BF16 checkpoint and
its license. The download worker invokes the native C++ repacker before READY;
the app never starts Python, Torch or a converter subprocess. Bundled metadata
and a small constant payload are bound to the reference checkpoint, converter
revisions and compiled recipe SHA. The installed decoder/encoder must match the
SHA-256 values produced by the audited reference conversion. Download inputs
are verified through retained descriptors, and prepared files are verified again
before the source is removed and staging is published. Cancellation, ENOSPC,
short reads/writes, corrupt output and an absent preparation ABI fail without
publishing READY. Restart removes known interrupted staging files.

The 52-layer transcription backbone retains original BF16 matrices; norms and
other required tensors use the reference converter's F32 representation. The
perception encoder promotes BF16 to F32 bitwise, including the subpixel Conv1D,
so values below F16's range are preserved. All 46 routed expert projection
arrays and 848 used perception tensors were compared exactly with the original
checkpoint. Unused MTP/speculation and CTC timestamp heads are omitted. Other
decoder transformations, including Q/K RoPE row permutation and Mamba A-log
folding, match the pinned converter's complete output hashes.

The installed package occupies 66.12 GB. Installation temporarily needs about
133.61 GB plus the normal free-space margin: the 67.50 GB download and prepared
files coexist until verification succeeds. The original safetensors is then
removed. The UI shows preparation separately from verification. Downloading
never selects the model or changes cloud preferences.

Only non-routed weights, states and graph scratch stay resident. A 4 GiB cache
holds 215 paired experts, indexed directly from GGUF offsets. Native parallel
pread fills selected slots; leases prevent eviction while the GPU reads them.
On supported Apple Silicon, Metal wraps the same page-aligned cache storage;
strided up/down tensor views and remapped slot IDs avoid duplicate GPU weights.
The exact ungated ReLU-squared expert branch is fused, with up to 31 rows per
shared-cache graph and the complete union of up to 128 distinct experts protected
per acquisition. Metal uses eight readers and overlaps known next-batch reads
with GPU computation within the same cache. Routing, weights,
aggregation, shared experts, attention and Mamba remain in the pinned decoder.
CPU and mapping-failure paths use bounded compact copies. Read/compute failure
aborts the graph and poisons the context; no failed turn is committed.

In the initial implementation on the M3 Max / 36 GB development machine, the native-prepared BF16 package
returned the expected English fixture sentence twice. With shared cache, the
3.410 s fixture took 29.538 / 27.860 s, with a 9,155,878,184-byte peak process
footprint. A separate memory diagnostic measured a 9,943,867,264-byte peak
footprint and 8,969,355,264-byte peak RSS: roughly 9–10 GB in these fixtures,
not a guaranteed process-memory limit. Repeated real-ABI loads on a 512 KiB thread also passed; production
VAD, Whistle speaker identity, SQLite, final drain and silence passed. The
fixture is small; no real-call accuracy or sustained live performance was
measured. SSD traffic was about 97 GB per transcript, so the mode remains
experimental with high delay. Model verification, loading and first-use Metal
compilation add startup time. Metal allocation counters and process footprint
can refer to the same shared pages and must not be added together.

The 2026-10-30 optimization combines host text/audio prefill, grouped expert
reads and GPU submissions, and a bounded prompt-prefix snapshot containing full
attention/Mamba state before any audio. The snapshot is keyed by exact prefix
tokens and is reused across turns in the same engine; it is invalidated when
the prompt changes. English measured 19.205 s on the first turn and 11.829 s
with the prefix cached; two different Italian utterances took 21.372 / 12.110 s.
The 4 GiB expert cache is unchanged, and observed process memory remains about
9–10 GB. Prefix caching adds about 50 MB in this fixture and trades some
first-turn latency for faster later turns. See [latency experiments and
correctness gates](design/nemotron-ssd-optimization.md) for ablations, research,
measurements and limits. Subsequent cache protection and exact prefetch tuning
reduced the repeated English fixture to 9.9 s using eight SSD readers. See
[parallel expert validation](design/nemotron-parallel-validation.md) for the
reader sweep, public corpus protocol and native queue replay. This mode remains
slower than real time on the measured development machine.

The reference-development tools are `scripts/prepare_nemotron_bf16.py` and
`scripts/build_nemotron_bf16_recipe.py`. They require a separate Python build
environment, verify the original checkpoint and executed converter sources,
and create the small pinned resources. They are not part of app inference or
installation. To test an already installed package in an isolated config:

```sh
./bin/ascolto-local-stt-runtime /absolute/temp/config.json /absolute/speech.wav nemotron-3.5-bf16-ssd en microphone
./bin/ascolto-local-stt-runtime /absolute/temp/config.json /absolute/speech.wav nemotron-switch en microphone
./bin/ascolto-local-stt-pipeline /absolute/temp/config.json /absolute/speech.wav /absolute/whistle.cact nemotron-3.5-bf16-ssd
```

Linux arm64 verification of the final BF16 integration passed the cache/graph,
F32 convolution, repacker and model-manager suites. The native Linux repacker
also reproduced the complete real-model hashes (235.39 s, 63,700 KiB maximum
RSS). The BF16 ELF exports only recognition and the preparation v1 entry point;
its dependencies are system libraries, including libcrypto. Full-model Linux
recognition and physical PipeWire capture were not measured.

`ascolto-nemotron-metal-mapping` is an excluded macOS target for real mapping
failure cleanup. It injects failures before allocation, after creating a Metal
view and after initializing its residency set; all 66 checks pass, with Metal
allocations returning to their 475,136-byte warm baseline. Its private fault
control is hidden from the adapter ABI.

## Q8_0 SSD package

`nemotron-3.5-q8` uses AtomicChat's Q8_0 decoder and F16 perception encoder from
revision `dbd06c5cad7a2f33c8e06be2fb3ef814dbd9aa59`, with the same pinned
NeMo/llama.cpp runtime as the other Nemotron variants. This is GGUF Q8_0, not
NVIDIA's FP8 ModelOpt checkpoint. The original BF16 variant remains separately
available, preserving its lossless perception weights.

The native Q8 adapter uses the same bounded 4 GiB routed-expert SSD cache as
BF16. Packed block sizes and tensor strides follow the actual Q8_0 format;
weights remain quantized in the cache and Metal expert operations. Dense weights,
encoder, states and scratch are additional memory, so 4 GiB is not a process
memory cap. The package downloads ready-to-load files without conversion.

Both native settings catalogs group Nemotron under one 8/16-bit selector.
Download, cancel, removal and explicit activation target the displayed package;
other installed variants remain intact. Historical transcription offers one
Nemotron entry with only installed precision choices enabled. Jobs retain the
exact package ID independently of future UI selections.

On the M3 Max / 36 GB development Mac, Q8 SSD completed the small comparison in
46.86 seconds versus BF16 SSD's 99.48 seconds (median total for 23.89 seconds of
input), with maximum observed footprints of 7.70 versus 9.94 GB. Both produced
the same normalized words, including the same public-clip proper-name error.
See [protocol, numerical checks and limits](design/nemotron-q8-comparison.md).

For an explicit same-audio comparison with the hosted model, see
[native Nemotron versus OpenRouter](design/nemotron-openrouter-validation.md).
Its offline fixtures and opt-in API driver use frozen public/synthetic references
and the shipped native C ABI without changing application settings.
