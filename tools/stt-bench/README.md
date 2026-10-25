# Local native STT benchmark

The benchmark keeps the application unchanged. Native C harnesses call each
recognizer in a persistent process; C++ runtimes remain explicitly identified as
C++ behind a C API. Python is used only for preparation and orchestration.

## Private files

Downloaded runtimes, models, corpus extracts and raw measurements live under the
ignored `build/stt-bench/` directory. The standalone HTML report and JSON export
live under ignored `reports/local-stt-2026-10-16/`. They embed private call audio
and derived transcripts. Owner-only directories protect these artifacts. Source
recordings are read-only inputs. The initial benchmark was entirely local. A
later explicit user request authorized uploading only the eight benchmark
extracts (95 seconds) to OpenRouter for a prioritized automatic reference.

## Protocol

`prepare_corpus.py` requires the repository Python environment with NumPy and
local ffmpeg/ffprobe. It selects three longest readable stereo archives and
extracts sustained-activity windows independently per channel. The manifest
records source offsets, PCM SHA-256, channel, duration and RMS. Six 10-second
clips, one partially overlapping 30-second clip and five seconds of digital
silence form the 95-second screening corpus. It is intentionally small and is
not a recognition-accuracy or long-session certification dataset.

```sh
.venv/bin/python3 tools/stt-bench/prepare_corpus.py \
  /absolute/recordings build/stt-bench/corpus
```

Each process reads matched float32 16 kHz PCM before model loading, loads once,
warms up on the first clip, and performs three ordered corpus passes. Warm-up
rows have `repeat=-1`. `bench_common.h` records monotonic wall time, process CPU
time and cumulative peak RSS in MiB (the historical field is `peak_rss_mb`).
Model loading and first output events are separate fields. A missing callback
remains null. First-token semantics differ by runtime and must remain labeled.

Runtime-specific preparation/build instructions are in `qwen/`, `nemo/`,
`parakeet/`, `voxtral/`, and `cohere/`. Whisper uses the installed Homebrew
whisper.cpp 1.9.5 / ggml 0.27.0 C API with explicitly loaded dynamic backends.
Whistle uses the repository's existing Needle library and model.

```sh
clang -O3 -std=c11 -Itools/stt-bench -Ivendor/needle \
  tools/stt-bench/whistle_bench.c vendor/needle/libneedle.a -lc++ \
  -framework Accelerate -o build/stt-bench/whistle-bench
clang -O3 -std=c11 -Itools/stt-bench -I/opt/homebrew/include \
  tools/stt-bench/whisper_bench.c -L/opt/homebrew/lib \
  -lwhisper -lggml -lggml-base -o build/stt-bench/whisper-bench
```

`run_serial.py` takes a process-wide filesystem lock for inference and captures
stdout JSONL, stderr logs, command arguments, selected environment settings,
return status and elapsed time. `run_plan.py` runs a JSON list containing `id`,
`command_argv`, and optional `environment`. On this machine GPU benchmarks
required native access outside the restricted execution sandbox. A sandbox
allocation failure must not be reported as model memory exhaustion.

```sh
python3 tools/stt-bench/run_serial.py --id whistle -- \
  build/stt-bench/whistle-bench models/whistle.cact \
  build/stt-bench/corpus/manifest.tsv 3
python3 tools/stt-bench/run_plan.py build/stt-bench/plan-qwen.json
```

Run plans, actual command ledgers and model/runtime hashes are retained with
the private results. Do not run benchmarks concurrently or compile heavy
dependencies while measurements are in progress. The current run did not
terminate background desktop applications or standardize OS/shader cache state.
Loading is therefore an observed startup cost, not a matched cold-cache test.

## Validating interpretation

- Whistle's ASR-plus-embedding row reuses the same measured ASR call and adds
  embedding extraction. It is not an independent ASR repetition or full
  diarization measurement.
- Parakeet first fell back to CPU because its registry device is `MTL0`, not
  `Metal`. Those raw measurements are preserved and labeled CPU. The Metal
  harness needs a small C++ shutdown shim matching the official CLI lifecycle;
  failed teardown attempts are archived separately.
- Voxtral's whole-clip 30-second feed exceeds its preallocated shared encoder
  KV cache. The runtime discards audio without propagating an API failure.
  `build_report.py` annotates these three records as validation failures while
  preserving their original API status. Incremental 2-second feeds complete the
  same audio correctly. The optional coverage-guard patch was not applied to
  the runtime used for these measurements.
- Streaming replay is unpaced compute, not elapsed microphone time. A partial
  text poll is not exact token-generation time. An offline final return is not
  a first token.
- No human verbatim reference was available. Existing STT transcripts are not
  ground truth; the report intentionally does not invent WER/CER scores.
- RSS and sampled Metal allocation overlap in unified memory. Never add them
  as though they were disjoint RAM/VRAM.

## Report and browser verification

The report is Italian by the user's explicit request; tooling and instructions
are English. The HTML includes inline data, audio, charts and filtering with no
remote dependencies. Source call names and paths are omitted from the public
corpus view, but the report is still private because it contains audio/text.

```sh
python3 tools/stt-bench/build_report.py \
  --output reports/local-stt-2026-10-16
node tools/stt-bench/check_report.cjs \
  reports/local-stt-2026-10-16/report.html build/stt-bench/qa
```

The browser check uses the desktop's bundled Playwright dependency path; adapt
that import when running elsewhere. It verifies desktop/narrow/mobile widths,
finite chart geometry, document overflow, filters, empty selections, embedded
audio metadata and JavaScript errors. Screenshots support manual visual review.

## Optional user-authorized cloud reference

`cloud_reference.py` retrieves only the configured OpenRouter credential through
Ascolto's existing Keychain abstraction. It holds the key in memory, never
prints/stores it or changes provider settings, checks the dedicated STT catalog,
and requests exactly `openai/whisper-large-v3-turbo`. Do not invoke the upload
step without explicit user authorization. Completed responses are cached to
avoid repeated uploads on a resumed run; no model/provider fallback is used.

```sh
python3 tools/stt-bench/cloud_reference.py --config /absolute/ascolto/config.json --check
python3 tools/stt-bench/cloud_reference.py --config /absolute/ascolto/config.json
python3 tools/stt-bench/compare_cloud_reference.py
python3 tools/stt-bench/build_report.py --output reports/local-stt-2026-10-16
```

The reference is the primary anchor by user preference, not human ground truth.
Local agreement cannot overwrite it and no calibrated confidence percentage is
invented. The comparison uses normalized word/character Levenshtein distance,
explicitly labeled disagreement against an automatic reference. Whisper turbo
locals share its base checkpoint and are not independent accuracy evidence.
The default textual comparison excludes the overlapping 30-second clip and
uses six independent 10-second windows (138 cloud-normalized words in this run).
Network timings remain separate from native inference measurements. The API's
reported cost and the successful cloud transcripts are retained in the private
report; no secret is included.
