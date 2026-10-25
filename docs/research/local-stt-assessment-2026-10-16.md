# Local STT models and native runtimes for Ascolto

Research date: 2026-10-16. Repository inspected at `e734aad645ec4bbcd373c7a38912153a3b9a8c5f`.

## Recommendation

**If the inference implementation must be written in C, evaluate Qwen3-ASR-0.6B with `antirez/qwen-asr` first, using its CPU BLAS/NEON backend and Ascolto's existing completed-turn segmentation.** Evaluate the 1.7B checkpoint next as the quality candidate. Both fit the verified Mac's memory capacity. This is a recommendation for a controlled trial, not a measured claim that either is faster or more accurate than Ascolto's current Whistle on Italian calls.

If the requirement instead means a C application calling an in-process native library without Python, add two distinct comparisons: **Whisper large-v3-turbo through whisper.cpp with Metal** for an established multilingual integration, and **Nemotron 3.5 ASR Streaming 0.6B through NVIDIA NeMo-Speech.cpp** for native incremental recognition. These have C APIs and C++ implementations; they do not satisfy a literal C-only implementation requirement. Nemotron's streaming architecture makes it particularly relevant if partial text during speech is a product requirement.

Do not change the production default on the basis of published leaderboards. Keep Whistle as the reference and preserve it initially for speaker embeddings. A model replacement should be promoted only after paired Italian-call measurements show the intended improvement in accuracy, latency, and sustained dual-channel throughput. The current default remains the decision documented in [AI_WORKFLOWS.md](../AI_WORKFLOWS.md).

## Scope and evidence

This investigation inspected source, existing local artifacts, hardware, primary model cards, runtime source/build files, and publisher benchmark methods. It created only this report. It did not install runtimes, download candidate model weights, open a microphone, submit audio to a service, run candidate inference, or change application settings. Repository status was clean before research.

Evidence is labeled by origin: **local observation**, **publisher measurement**, or **engineering recommendation**. Published numbers below were not reproduced on this machine, and the rows do not form a common benchmark.

## Verified hardware

Read-only `system_profiler SPHardwareDataType SPDisplaysDataType`, `sw_vers`, and `uname -m` reported:

| Item | Local observation |
| --- | --- |
| Machine | MacBook Pro, model identifier `Mac15,11` |
| SoC | Apple M3 Max |
| CPU | 14 cores: 10 performance, 4 efficiency |
| GPU | 30 cores; Metal 4 supported |
| Memory | 36 GB unified memory |
| Architecture | `arm64` |
| Operating system | macOS 26.7.1, build `25G241` |

Apple specifies **300 GB/s** memory bandwidth for the 14-core CPU/30-core GPU M3 Max configuration. The 16-core/40-core configuration has 400 GB/s. Consequently, a published benchmark that merely says “M3 Max” may describe materially different hardware. The bandwidth is a manufacturer specification, not a measurement made here. [Apple technical specifications](https://support.apple.com/en-us/117737).

36 GB is installed capacity, not currently available model memory. Browsers, conferencing, the display compositor, audio archives, the OS, CPU tensors, and GPU allocations share it. There is no NVIDIA CUDA device on this Mac. The existence of an Apple Neural Engine does not mean an arbitrary C runtime uses it: a compatible Core ML execution path is required. No physical Linux host was available for hardware verification.

## What Ascolto actually runs today

The live backend is **Cactus Whistle through Needle's C ABI**, not OpenAI Whisper. The app's C-facing integration does not make the inference engine pure C: Cactus explicitly describes its engine as C++, and the macOS link includes `-lc++`. [Needle/Whistle implementation description](https://github.com/cactus-compute/needle#whistle), [macOS build](../../Makefile.macos).

Local observations:

| Component | Evidence |
| --- | --- |
| macOS model | `models/whistle.cact`, 16,919,407 bytes |
| Model SHA-256 | `b6e02f048568ac5d01a2042556c658061e699acbc0aa2a1439f52f3d461dffeb` |
| macOS runtime | `vendor/needle/libneedle.a`, 1,540,960 bytes |
| Runtime SHA-256 | `c1b955778dcb79c92efaf5606f94e9d3a4ed85e093096b41e502bc9748a90614` |
| Runtime acquisition | macOS: `Cactus-Compute/needle3` platform artifacts; Linux: matching x86-64 artifacts and shared library extracted during dependency setup |
| Python in local inference | None. Linux's setup script uses Python to extract a wheel, which is different from a Python inference dependency. |

The current fetch URLs use mutable `main` paths and existence checks rather than pinned checksums. These hashes identify the files examined; they do not establish an upstream engine version. See [Makefile.macos](../../Makefile.macos), [fetch_whistle.sh](../../scripts/fetch_whistle.sh), and the separate [Linux Makefile](../../Makefile).

Both channels carry 16 kHz mono float PCM in 20 ms frames. Capture enqueues; a serialized consumer processes SYS before MIC. On macOS, live VAD closes MIC turns after 0.32 s of silence with a 12 s maximum, and SYS turns after 0.38 s with a 10 s maximum. Each completed turn calls `needle_transcribe`, requesting word timestamps. **The production path is turn-based recognition over live audio, not repeated calls to Needle's streaming recognizer.** Streaming wrapper functions exist but are not used by this live orchestration. [Architecture](../ARCHITECTURE.md), [live_session.c](../../src/live_session.c), [macOS engine wrapper](../../src/macos/whistle_engine.c).

The Linux wrapper similarly transcribes segments, caps a call at 30 s, and serializes Needle access. Its result contract differs from macOS. Do not silently unify these wrappers or replace upstream Linux source paths. [Linux wrapper](../../src/whistle_engine.c), [integration boundaries](../INTEGRATION.md).

The same model also supplies `needle_embed` encoder frames to speaker identification. These are pooled/projected differently on Linux and macOS. Replacing ASR without preserving or deliberately replacing this embedding path can change speaker labels even when text improves. Initial comparison should replace **only the text recognizer** and continue extracting Whistle embeddings, reporting that additional cost separately. This isolates the ASR experiment; retaining Needle would still leave C++ neural inference in the overall speaker-identification pipeline. A requirement covering all neural inference would also need a separate C speaker-embedding solution. Generic ASR encoder features are not automatically equivalent speaker embeddings. [macOS diarizer](../../src/macos/diarizer.c), [Linux diarizer](../../src/diarizer.c).

Whistle's publisher targets tiny CPU deployments, lists Italian among seven languages, and publishes Apache-2.0 weights. Its speed comparison uses an M4 Pro and different default runtimes/precisions, including Python `openai-whisper` on CPU; it is not a comparison against Metal whisper.cpp on this M3 Max. It cannot establish the fastest replacement here. [Whistle model card and methodology](https://huggingface.co/Cactus-Compute/whistle).

## The C requirement needs a precise boundary

| Category | Examples | Meets literal C inference source requirement? |
| --- | --- | --- |
| Application and inference source in C, calling system math libraries | qwen-asr CPU, Accelerate on macOS/OpenBLAS on Linux | Yes, if system BLAS and SIMD intrinsics are permitted |
| C inference core plus Objective-C host code and Metal shaders | voxtral.c `make mps` | No, if every inference component must be C; potentially acceptable for a native C/Obj-C application |
| C ABI over C++/ggml or C++/ONNX Runtime | whisper.cpp, NeMo-Speech.cpp, parakeet.cpp, sherpa-onnx, current Needle | No; callable from C is a different property |
| Python orchestration over compiled GPU kernels | official Transformers/NeMo/vLLM paths, common MLX Python tools | No for this requested integration |
| Swift/Core ML application integration | WhisperKit and similar Apple-only stacks | Native, but not C inference and not a shared Linux backend |

The qwen-asr [Makefile](https://github.com/antirez/qwen-asr/blob/924694251d9e0f18e5d86bbd06aa3ab5f870002d/Makefile) builds CPU inference from `.c` files, with Accelerate or OpenBLAS. Its optional CUDA and ROCm implementations introduce CUDA/HIP C++ toolchains. Its author explicitly excludes an MPS backend. [CPU/GPU scope](https://github.com/antirez/qwen-asr#qwen3-asr-pure-c-implementation).

“Every dependency must itself be available as ISO C source” is stricter still: this report has not established that property for vendor BLAS. Ascolto already uses native Objective-C and platform frameworks, but that does not authorize relabeling a C++ recognizer as C.

C is not inherently faster than equivalent optimized C++. Model architecture, repeated encoder work, memory bandwidth, SIMD, quantization, batching, and accelerator kernels dominate. On this Mac, rejecting every Metal/Obj-C/C++ component can leave the 30-core GPU idle. A well-optimized CPU C engine may still meet the latency budget, but that must be measured.

## Shortlist

The memory figures describe different quantities and are intentionally labeled. Model-file size is not peak process or unified-memory use.

| Model and runtime | Italian and live behavior | Storage/memory evidence | Integration assessment |
| --- | --- | --- | --- |
| Qwen3-ASR 0.6B / qwen-asr CPU | Italian supported; completed-turn decode or buffered streaming with rollback | Publisher CPU segmented peak RSS around 2.7–2.8 GiB | First literal-C experiment; no Metal |
| Qwen3-ASR 1.7B / qwen-asr CPU | Same language coverage; larger quality candidate | Publisher segmented peak RSS around 6.6–6.8 GiB | Fits 36 GB; more CPU/latency exposure |
| Whisper large-v3-turbo / whisper.cpp Metal | Multilingual; existing VAD turns fit its offline decode contract | 1.5 GiB model file; Q5_0 547 MiB | Strong native C-API baseline; C++ core |
| Whisper large-v3 / whisper.cpp Metal | Multilingual accuracy comparator; slower decoder than turbo | 2.9 GiB file; Q5_0 1.1 GiB | Include if turbo quality is insufficient |
| Nemotron 3.5 ASR Streaming 0.6B / NeMo-Speech.cpp | Italian `it-IT`; cache-aware streaming | 0.6B model; exact Mac peak memory unmeasured | Best architectural fit to test for incremental live text; C ABI/C++ |
| Parakeet TDT 0.6B v3 / native C++ or sherpa-onnx | Italian among 25 European languages; offline/VAD-segmented use | Sherpa INT8 export about 640 MB on disk | Throughput challenger; do not confuse with English-only v2 |
| Voxtral Mini 4B Realtime / voxtral.c Metal | Italian; causal streaming | 8.9 GB BF16 file, plus GPU cache/workspace | Larger and less proven dual-channel margin; second stage |
| Moondream Parakeet Redux/Ultra / parakeet.cpp | Italian inherited from multilingual v3; offline | Redux packed GGUF about 213 MB; not peak RAM | Recent experimental comparison after the core shortlist |

Storage sources: [whisper.cpp model inventory](https://github.com/ggml-org/whisper.cpp/blob/0c7be696371069e0baa2515ed7aa08d5a5473aa6/models/README.md), [Sherpa v3 export](https://k2-fsa.github.io/sherpa/onnx/pretrained_models/offline-transducer/nemo-transducer-models.html#sherpa-onnx-nemo-parakeet-tdt-0-6b-v3-int8-25-european-languages), [Qwen CPU measurements](https://github.com/antirez/qwen-asr#cpu-memory-requirements), [Voxtral runtime memory](https://github.com/antirez/voxtral.c#memory-requirements), [Redux runtime notes](https://github.com/mudler/parakeet.cpp/blob/5c9b0fbbd8e611d88e47d7e1ebae9bca9b447e8f/docs/ultra-redux.md).

Runtime and model licenses are separate. The [whisper.cpp runtime](https://github.com/ggml-org/whisper.cpp/blob/0c7be696371069e0baa2515ed7aa08d5a5473aa6/LICENSE) and OpenAI Whisper weights are MIT; [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx/blob/fa29eb91c80335fc70af869a7bb459578f90e5ad/LICENSE) is Apache-2.0; [parakeet.cpp](https://github.com/mudler/parakeet.cpp/blob/5c9b0fbbd8e611d88e47d7e1ebae9bca9b447e8f/LICENSE) is MIT. Their Parakeet/Redux/Ultra weights retain CC-BY-4.0. The Qwen, Voxtral, and Nemotron license pairs are specified below. Retain dependency notices and the selected converted model's provenance when packaging.

### Qwen3-ASR: the direct answer to a literal C constraint

Official Qwen weights are Apache-2.0; the third-party C runtime is MIT. The model supports Italian and contextual prompting, but the official Python/vLLM feature set must not be assumed to exist in this port. Qwen's multilingual evaluation reports 1.7B ahead of 0.6B across aggregate sets; its 12-language FLEURS WER is 4.90 versus 7.57, with Whisper large-v3 at 5.27. These are publisher aggregate results including Italian, **not Italian-only scores or C-port results**. [Official model card](https://huggingface.co/Qwen/Qwen3-ASR-1.7B).

The C [public header](https://github.com/antirez/qwen-asr/blob/924694251d9e0f18e5d86bbd06aa3ab5f870002d/qwen_asr.h) accepts 16 kHz mono float samples through `qwen_transcribe_audio`; it exposes total/encoder/decoder timing and token callbacks. It does not expose word timestamps or a forced-aligner API. The separate official Qwen forced aligner is therefore not a drop-in capability of this C runtime. Turn timestamps can remain valid, but missing word timing must be represented explicitly.

Start with the current 10–12 s VAD turns and language forced to `Italian`, then test automatic language detection and mixed Italian/English separately. Do not enable silence removal that compresses Ascolto's timeline. Do not start with rollback streaming: it adds state/commit semantics and potentially much more compute. A token callback on a complete clip is output streaming, not evidence of incremental audio processing.

### Whisper: mature native comparison if C++ is acceptable

Turbo reduces large-v3's decoder from 32 layers to 4, trading some accuracy for speed. Both remain multilingual; avoid `.en` checkpoints. OpenAI's published relative speeds are framework/hardware-specific and do not predict performance here. [Turbo model card](https://huggingface.co/openai/whisper-large-v3-turbo).

whisper.cpp provides a C API with native CPU/Metal acceleration; Core ML can optionally execute the encoder through Apple's stack. Core ML conversion uses Python tooling during asset preparation, while packaged inference need not run Python. Begin with Metal alone, then compare Core ML only if the first result warrants another configuration. [Runtime documentation](https://github.com/ggml-org/whisper.cpp#readme), [source/build language evidence](https://github.com/ggml-org/whisper.cpp/blob/0c7be696371069e0baa2515ed7aa08d5a5473aa6/src/CMakeLists.txt).

The supplied microphone streaming example repeatedly decodes windows; Whisper itself is not a cache-aware streaming encoder. Integrate `whisper.h` on Ascolto PCM rather than substituting SDL microphone capture. Token/word timestamps are available, but alignment accuracy and confidence semantics still need comparison with the existing output contract. [C API](https://github.com/ggml-org/whisper.cpp/blob/0c7be696371069e0baa2515ed7aa08d5a5473aa6/include/whisper.h), [streaming example limitations](https://github.com/ggml-org/whisper.cpp/blob/0c7be696371069e0baa2515ed7aa08d5a5473aa6/examples/stream/README.md).

### Nemotron and Parakeet: distinguish true streaming from fast offline ASR

Nemotron 3.5 uses a cache-aware FastConformer/RNN-T with Italian in its transcription-ready tier. Its model card reports Italian FLEURS WER of 4.83 with 320 ms chunks and forced language, versus 4.25 with 1.12 s chunks. Those numbers demonstrate a latency/quality tradeoff, not a cross-model ranking: the normalizer, chunking, and runtime differ from other publishers' tests. The model's stated release date is June 4, 2026, and its weights use **OpenMDW-1.1**, not Apache-2.0. [Model card](https://huggingface.co/nvidia/nemotron-3.5-asr-streaming-0.6b).

NVIDIA's NeMo-Speech.cpp has a C++17/ggml core, an Apache-2.0 runtime license, and native CPU/Metal/Linux backend options. Its C SDK accepts float PCM and supports offline and streaming results, word timing, and per-request language. The SDK documents independent streams sharing one recognizer; each individual stream is driven from one thread. This is promising for MIC/SYS, but platform-specific allocation and concurrency must still be verified. [Runtime](https://github.com/NVIDIA/NeMo-Speech.cpp), [SDK contract](https://github.com/NVIDIA/NeMo-Speech.cpp/blob/083861c241061562b8ec29eb06d7f6cdc3af8775/docs/sdk.md), [ASR header](https://github.com/NVIDIA/NeMo-Speech.cpp/blob/083861c241061562b8ec29eb06d7f6cdc3af8775/include/nemo_speech/asr.h), [runtime license](https://github.com/NVIDIA/NeMo-Speech.cpp/blob/083861c241061562b8ec29eb06d7f6cdc3af8775/LICENSE).

Parakeet TDT v3 is a different 0.6B model: multilingual, automatic language detection, punctuation, and duration-based timestamps, with CC-BY-4.0 weights. Its fast offline encoder does not automatically become native incremental streaming by feeding it short chunks. NeMo-Speech.cpp, `mudler/parakeet.cpp`, and sherpa-onnx provide native routes; compare one route first rather than benchmarking every runtime combination. [Parakeet model card](https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3).

parakeet.cpp offers a flat PCM C API over C++/ggml and Metal. Sherpa offers C APIs over C++/ONNX Runtime and a documented v3 INT8 export; its VAD microphone example is explicitly simulated streaming. Neither route is pure C, and ONNX/Core ML operator offload must be observed rather than assumed. [parakeet.cpp](https://github.com/mudler/parakeet.cpp), [Sherpa export and examples](https://k2-fsa.github.io/sherpa/onnx/pretrained_models/offline-transducer/nemo-transducer-models.html).

### Voxtral: attractive streaming, a more expensive native implementation

Voxtral Mini 4B Realtime has Apache-2.0 weights and Italian support. Its publisher reports Italian FLEURS WER of 3.27% at 480 ms model delay, 2.46% at 960 ms, and 2.37% at 2.4 s. Model delay is not end-to-end UI latency or a guarantee for this runtime. [Official model card](https://huggingface.co/mistralai/Voxtral-Mini-4B-Realtime-2602).

The MIT `voxtral.c` core is C. Its fast `make mps` path compiles Objective-C host code and Metal shaders; the target name does not mean Python/PyTorch MPS. CPU BLAS is available on Linux and macOS but is much slower in the author's measurements. Its API feeds float PCM incrementally, making native integration possible. The author's own readiness warning calls for more production and long-stream testing. [Runtime and API](https://github.com/antirez/voxtral.c), [actual build inputs](https://github.com/antirez/voxtral.c/blob/134d366c24d20c64b614a3dcc8bda2a6922d077d/Makefile).

The published allocation inventory lists an 8.9 GB mapped file, roughly 8.4 GB GPU weight cache, up to 1.8 GB decoder KV cache, and work buffers. Unified memory does not make converted GPU copies free. These categories are not a measured peak-RSS sum: mapped pages may be resident or reclaimable, and two streams may need separate state. Measure total memory pressure before considering dual-channel production.

### Recent alternatives and exclusions

Moondream released Parakeet Redux and Ultra on September 22, 2026. Redux's advertised 178 MB weights and CPU throughput come from its Photon implementation; parakeet.cpp has a distinct 213 MB packed GGUF path. The latter is CPU/offline only and has ARM dot-product kernels, but its headline speed measurement uses x86 AVX-512 VNNI. The model card also shows worse noisy/business-speech scores than original v3 despite better aggregate FLEURS. This warrants a later experiment, not automatic preference for a smaller model. Ultra is another quality comparison. Both use CC-BY-4.0. [Publisher announcement](https://moondream.ai/blog/introducing-parakeet-redux-and-ultra), [Redux model card](https://huggingface.co/moondream/parakeet-redux), [native port details](https://github.com/mudler/parakeet.cpp/blob/5c9b0fbbd8e611d88e47d7e1ebae9bca9b447e8f/docs/ultra-redux.md).

Cohere Transcribe 2B supports Italian and has Apache-2.0 weights. A sherpa-onnx INT8 C-API route exists, with about 2.5 GB encoder data plus a 146 MB decoder; it is offline and requires an explicit input language. It is a useful post-call quality candidate, but adds no demonstrated advantage under the literal C requirement or this live latency target. [Model card](https://huggingface.co/CohereLabs/cohere-transcribe-03-2026), [native export](https://k2-fsa.github.io/sherpa/onnx/cohere_transcribe/pretrained.html), [offline contract](https://k2-fsa.github.io/sherpa/onnx/cohere_transcribe/index.html).

Moonshine's current official available-model list does not list Italian, so its English streaming scores do not justify selecting it here. [Published language inventory](https://moonshine-voice.readthedocs.io/en/latest/models/available-models/). English-only Parakeet v2, English Nemotron Speech, and English Distil-Whisper are similarly not substitutes for Italian. Python-first faster-whisper, MLX, WhisperX, NeMo, and vLLM may be useful reference implementations but are outside the requested native C inference path.

## What the published speed evidence actually establishes

`RTF = processing wall seconds / audio seconds`; `RTFx = 1 / RTF`. Lower RTF is better. These are **publisher measurements on different workloads**, not estimated results for the verified Mac.

| Source/workload | Published result | Practical limit |
| --- | --- | --- |
| qwen-asr, M3 Max/128 GB, CPU BLAS, 45 s clip, segmented offline | 0.6B: 3.4 s, 13.38×; 1.7B: 5.9 s, 7.63× | One run per row; different RAM/possibly CPU bin; English clip |
| Same Qwen 45 s clip, interactive streaming with encoder cache | 0.6B: 4.69×; 1.7B: 2.54× | Different mode, much less margin |
| Same Qwen report, 135 s clip, 0.6B | Streaming 0.96×; segmented offline 9.64× | Streaming can fall behind despite a good short example |
| parakeet.cpp, Apple M4, v3 Q4_K, one 7.4 s clip | Metal 54.3× versus CPU 19.8× | Best of six; excludes load/initial compilation; not Italian-call accuracy |
| NeMo-Speech.cpp, i7-11700K, English Nemotron, Q8_0, 160 ms chunks | 27 ms average compute/chunk; 5.6× throughput | English checkpoint, x86 CPU, subset; not multilingual M3 Max |
| voxtral.c, M3 Max 40-GPU/128 GB | Approximately 2.5× realtime described for a 60 s clip | Larger GPU/bandwidth than this Mac; little dual-channel headroom |

Sources: [Qwen method and timings](https://github.com/antirez/qwen-asr#how-fast-is-it), [Parakeet Metal method](https://github.com/mudler/parakeet.cpp/blob/5c9b0fbbd8e611d88e47d7e1ebae9bca9b447e8f/benchmarks/BENCHMARK.md#apple-metal-m4), [NeMo-Speech.cpp benchmark](https://github.com/NVIDIA/NeMo-Speech.cpp/blob/083861c241061562b8ec29eb06d7f6cdc3af8775/BENCHMARK.md), [Voxtral benchmark](https://github.com/antirez/voxtral.c#how-fast-is-it). Voxtral's separate [speed log](https://github.com/antirez/voxtral.c/blob/134d366c24d20c64b614a3dcc8bda2a6922d077d/SPEED.md) also shows substantial variation by corpus and revision.

For a serialized consumer and two continuously active channels, equal per-channel RTF `r` requires roughly `2r < 1` just to avoid growing backlog, before embedding, VAD, persistence, and scheduling overhead. This arithmetic is a workload bound, not a performance prediction. Set a stricter experimental acceptance target of aggregate compute below 0.6 s per wall second under worst-case dual speech to preserve headroom.

There is no evidence here that a larger candidate beats the tiny Whistle in raw speed. The likely opportunity is better transcription while remaining comfortably realtime, or lower perceived delay through incremental computation. Ascolto's `ttft_ms` and `decode_tps` exclude important work and cannot stand in for total ASR RTF. End-of-turn latency also includes VAD silence, queue backlog, embeddings, database commit, and UI delivery.

Italian quality must be evaluated on conversations. A primary 50-hour Italian television study found broadly comparable overall results across tested systems, with large variation by program type. It used edited subtitles rather than verbatim references and mixed T4/L4 hardware for Parakeet timing. It supports domain-specific evaluation, not a numerical ranking for Ascolto or the newer models. [Lucca and Pierri, 2025](https://arxiv.org/html/2512.19161v1).

## Integration requirements before a replacement can be promoted

1. Keep CoreAudio/PipeWire capture and paired-timeline handling intact. All inference remains on the consumer or a bounded processing stage, never a capture callback. Preserve drain-before-close and equal-duration overflow silence.
2. Add a small C-facing recognizer boundary with explicit lifecycle, PCM input, language/vocabulary options, result ownership, errors, available timing fields, and end-of-stream flush. Adapt each existing platform result contract rather than equating incompatible structs.
3. First use completed turns. Keep one loaded model per process and avoid launching a CLI or loading weights for each turn. Streaming later needs independent MIC/SYS state, provisional/final events, bounded buffers, context resets, and a drain protocol that commits the tail exactly once.
4. Preserve source sample offsets. A recognizer's silence skipping, resampling, context overlap, or chunk-relative timestamps must not compress or duplicate the call timeline. Do not average MIC and SYS into one channel.
5. Keep Whistle embedding extraction and current speaker thresholds initially. A later speaker-model migration requires its own labeled comparison. Report ASR-only and ASR-plus-embedding costs.
6. Missing word timestamps/confidences are absent data, not zeros presented as measurements. Test Unicode, proper names, numerical formatting, punctuation, turn-boundary truncation, and mixed-language phrases.
7. Retune hallucination and repetition checks against actual new output distributions. Raw token probability, acoustic confidence, and calibrated correctness probability are different quantities. Keep noise/echo rejection and test quiet replies and overlap.
8. Load/warm models and query devices off the UI thread. Use copied status snapshots. Package arm64 libraries, backend assets, models, licenses, and absolute executable-relative paths; verify signed app launch with `cwd=/` and minimal `PATH`.
9. Preserve native Linux builds and upstream paths. Record separate CPU/OpenBLAS results on a real Linux target; this Mac cannot validate PipeWire, Linux GPU drivers, or x86 throughput.

These requirements follow the existing [macOS lifecycle rules](../MACOS.md), [architecture](../ARCHITECTURE.md), and [development verification limits](../DEVELOPMENT.md).

## Reproducible benchmark plan

### Stage 1: fix the workload and reference

Create a local manifest with `clip_id`, PCM SHA-256, sample count, channel, language, acoustic condition, source start/end, and a human verbatim reference. Include full Italian FLEURS test as a read-speech sanity check and a separate held-out real-call set with multiple speakers, accents, interruptions, quiet acknowledgments, English technical terms, names, dates, numbers, laptop speakers, and headset audio. Public read speech cannot replace conversational coverage. Keep calls and references local; no captured call was accessed for this research.

Use exactly the same decoded 16 kHz float PCM for all engines. For archived AAC, decode once; a codec difference must not become an engine difference. Freeze normalization rules for Italian apostrophes, accents, punctuation, case, and numbers. Report raw and normalized WER/CER, plus names/numbers separately. Do not tune on the held-out set.

Freeze the baseline Whistle artifacts by the hashes above. For the first accuracy experiment, give every recognizer the **same current VAD turns**, without mocked hints. Run a second full-timeline evaluation including silence and missed speech so VAD omissions are visible rather than excluded from the score. Existing synthetic room tests verify pipeline invariants, not recognizer accuracy.

### Stage 2: a small, ordered configuration matrix

| Order | Configuration | Purpose |
| --- | --- | --- |
| 0 | Current Whistle and existing pipeline | Establish actual local speed, WER, and latency |
| 1 | Qwen3-ASR 0.6B, CPU Accelerate/NEON, completed turns, forced Italian | Literal-C primary candidate |
| 2 | Qwen3-ASR 1.7B, same setup | Measure quality gained per latency/memory cost |
| 3, if C++ accepted | Whisper turbo F16 and Q5_0, Metal | Native quality/throughput baseline and quantization comparison |
| 4, if C++ accepted | Nemotron 3.5 Q8_0, NeMo-Speech.cpp, 320/560 ms settings | Incremental latency versus quality; check supported converted configuration |
| 5, only if useful | Parakeet v3 Q8_0 native Metal, then large-v3 or Voxtral | Resolve remaining accuracy/throughput tradeoffs |

Start CPU thread sweeps at 4, 8, and 10 rather than blindly using all 14 cores; measure conferencing responsiveness and BLAS oversubscription. Pin the model precision, beam/search settings, language policy, prompt, vocabulary, segment size, context reset policy, library commit, compiler, and backend. Do not compare one engine's greedy run with another's beam-search result without labeling it.

### Stage 3: measure the native persistent process

Use a small offline C harness consuming the manifest, with the engine loaded once. This harness is future work; it was not written here. Capture monotonic timestamps around preprocessing, encode/decode, embedding, result adaptation, and the complete recognizer call. Report:

- Load time and first-result cold latency separately from warm inference.
- Total ASR seconds divided by actual supplied audio seconds, weighted across clips; p50/p95/p99 turn processing and worst outliers.
- End of acoustic speech to first provisional text and durable final transcript. Include endpointing, queue time, and commit/UI delivery.
- WER, CER, substitutions/deletions/insertions, false words per minute of silence/noise, entity/number errors, and word-timing error where available.
- CPU time, peak RSS, GPU/unified-memory pressure, swap change, and sustained power/thermal behavior. Do not infer total memory from a weight file or sum CPU/GPU counters that overlap.
- Speaker-label stability while retaining Whistle embeddings, with its cost separated from ASR.

On macOS, `/usr/bin/time -l <command>` can provide CLI wall/CPU/peak-RSS screening, but a fresh process per clip measures loading too and is not the final integration benchmark. Example future smoke invocations, after explicitly preparing pinned runtimes and weights:

```sh
./qwen_asr -d /absolute/models/qwen3-asr-0.6b -i /absolute/corpus/it-001.wav --language Italian -S 0
./build/bin/whisper-cli -m /absolute/models/ggml-large-v3-turbo.bin -f /absolute/corpus/it-001.wav -l it -t 8
```

Record actual backend initialization logs; a successful build flag alone does not prove GPU execution. Never substitute Qwen's file-input `--stream --silent` shortcut for its interactive streaming benchmark: the upstream README documents different execution behavior.

Run one warm-up and at least five alternating measured passes, retaining all runs and reporting median plus spread. Separate warm file-cache tests from cold-process/load tests. Use AC power and record power mode, OS/compiler versions, background conferencing workload, and thermal state. Avoid extrapolating from a single short clean clip.

### Stage 4: production-shaped replay and acceptance

Replay two channels at real-time pace for at least one hour, including concurrent speech, startup offsets, partial final frames, silence, and long turns. Report aggregate compute, maximum queued audio, any represented overflow, final drain time, UI responsiveness, and memory growth after warm-up. Repeat with a normal video call/browser workload. Single-channel RTF alone is insufficient.

Proposed promotion criteria are engineering targets, not existing guarantees: material held-out Italian WER/entity improvement; no lost timeline; no increased hallucination on silence; bounded memory without growing swap; no sustained backlog; aggregate worst-case dual-channel processing comfortably below realtime; responsive UI; and correct flush/drain. Choose the smallest candidate meeting them. If none does, retain Whistle and use a separate local post-call pass rather than degrading capture reliability.

## Source freshness and reproducibility

GitHub APIs were queried on the research date to distinguish branch state from released artifacts. Some README prose described older “latest” releases; the table records API results rather than copying that wording. A release tag does not prove every branch feature cited above is present in that release.

| Runtime | Observed branch HEAD | Latest published GitHub release observed |
| --- | --- | --- |
| qwen-asr | `924694251d9e0f18e5d86bbd06aa3ab5f870002d` | No published release returned |
| voxtral.c | `134d366c24d20c64b614a3dcc8bda2a6922d077d` | No published release returned |
| whisper.cpp | `0c7be696371069e0baa2515ed7aa08d5a5473aa6` | `v1.9.5`, 2026-10-06 |
| NeMo-Speech.cpp | `083861c241061562b8ec29eb06d7f6cdc3af8775` | `v0.2.0`, 2026-10-02 |
| parakeet.cpp | `5c9b0fbbd8e611d88e47d7e1ebae9bca9b447e8f` | `v0.6.1`, 2026-10-07 |
| sherpa-onnx | `fa29eb91c80335fc70af869a7bb459578f90e5ad` | `v1.13.8`, 2026-09-10 |

Primary API endpoints: [qwen-asr HEAD](https://api.github.com/repos/antirez/qwen-asr/commits?per_page=1), [voxtral.c HEAD](https://api.github.com/repos/antirez/voxtral.c/commits?per_page=1), [Whisper release](https://api.github.com/repos/ggml-org/whisper.cpp/releases/latest), [NeMo release](https://api.github.com/repos/NVIDIA/NeMo-Speech.cpp/releases/latest), [Parakeet release](https://api.github.com/repos/mudler/parakeet.cpp/releases/latest), [Sherpa release](https://api.github.com/repos/k2-fsa/sherpa-onnx/releases/latest). These live endpoints are provenance for this observation and will change.

Observed model repository revisions, which must be paired with the selected runtime-specific conversion and file SHA-256 during benchmarking:

| Model | Hugging Face revision observed |
| --- | --- |
| Qwen3-ASR-0.6B | `5eb144179a02acc5e5ba31e748d22b0cf3e303b0` |
| Qwen3-ASR-1.7B | `7278e1e70fe206f11671096ffdd38061171dd6e5` |
| Nemotron 3.5 ASR Streaming 0.6B | `ea30d66debe3740a08b573244286791d423d6b3e` |
| Parakeet TDT 0.6B v3 | `541d1f99c6b0c3cd0b11a95167540bb8edefd82b` |
| Voxtral Mini 4B Realtime 2602 | `2769294da9567371363522aac9bbcfdd19447add` |
| Parakeet Redux | `2bf128600aac4b16946f7ed8372e56117fe5e23b` |
| Parakeet Ultra | `73175eb7aeb0d82f1e2a6b53b3aabc10a90bcd0b` |

These are repository metadata revisions, not proof that a third-party GGUF/ONNX export reproduces the original checkpoint. Benchmark logs must identify the conversion revision, tensor precision, exact files, and backend. Runtime and model licenses are separate: MIT/Apache runtime licensing does not replace CC-BY or OpenMDW terms for its weights.

## Remaining uncertainty

There is no matched local accuracy/performance result yet, no verified Linux hardware benchmark, no candidate packaging/signing test, and no long-call stability test. Publisher results use different datasets, normalizers, decoding settings, accelerators, and timing boundaries. They narrow the experiments but cannot select a numerical winner. The concrete next step is the completed-turn Qwen 0.6B versus Whistle comparison on this verified M3 Max, followed by the larger model only if quality and headroom justify it.
