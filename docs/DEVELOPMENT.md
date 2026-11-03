# Development commands

`GNUmakefile` selects `Makefile` on Linux and `Makefile.macos` on macOS.
The commands below describe macOS verification and tooling.

- `make test`: builds everything, runs all 10 test suites (`./bin/ascolto-harness test`, 19 checks), including eight acoustic rooms and capture queue/original-audio verification, plus four concurrency/timeline regressions (`./bin/ascolto-regression`), portable queue/complete-audio checks, CLI/socket/archive regressions (`./bin/ascolto-control-test`), and the UI smoke test (`./bin/ascolto --smoke-test`). CoreAudio checks and fixture synthesis require access to macOS services outside a restricted sandbox.
- `make room-bench`: exercises the production live pipeline with deterministic headphones, speaker bleed, quiet replies, local interruptions, irregular callback sizes, long turns, and three remote speakers. Writes input WAVs, transcripts, and `tests/room-results/metrics.jsonl`; fails on altered PCM, processing errors or incomplete audio timelines.
  Keyword recovery, extra MIC text and speaker counts are reported observations:
  the model receives speaker playback unchanged, and each audio request can
  contain several speakers. Model recognition errors do not become transport
  failures or application text filters.
- `make test-capture`: opt-in real microphone verification using the actual menu start/stop actions and temporary artifacts. Covers three consecutive manual sessions, a five-second hardware startup delay, and a five-second device-inventory query. Checks menu construction within 100ms, action completion during menu tracking, cache population, responsive timers, and stop actions within 100ms. Requires microphone permission; single-session runs have an 18-second timeout and the three-session run has a 32-second timeout.
- `make run`: packages, ad-hoc signs (`codesign -s -`), and launches `bin/Ascolto.app`.
- `./bin/ascolto-harness probe-macos`: inspects CoreAudio devices, active `IN`/`OUT` apps, and `Accessibility`/`Calendar` permissions.
- `./bin/ascolto-harness simulate --mic <wav> --sys <wav> --lang it`: simulates a dual-channel call offline.
- `./bin/ascolto-harness reprocess <call.m4a> --lang it`: splits and re-transcribes a saved stereo `.m4a` call archive (`Ch1=MIC`, `Ch2=SYS`).


For shared persistence and AI checks, see [DATABASE.md](DATABASE.md) and
[AI_WORKFLOWS.md](AI_WORKFLOWS.md). Synthetic tests do not establish real-call
transcription or speaker accuracy.

`make test-portable` also compiles the Linux core on either host and instruments
model/output boundaries to verify exact PCM, sub-millisecond final fragments,
empty responses and error propagation through the actual consumer. This does not
replace a Linux build or physical PipeWire verification.

## Optional native speech engines

Default native builds prepare all five optional in-process adapters through
`make local-stt-deps`; this builds pinned C/C++ sources and never downloads optional weights.
The build requires Git, CMake 3.26+ and a C/C++ compiler. Linux also requires libcurl,
OpenSSL and OpenBLAS development packages (`libcurl4-openssl-dev libssl-dev
libopenblas-dev` on Debian/Ubuntu). macOS uses system libcurl, CommonCrypto,
Accelerate and Metal. The app bundle copies the adapters and license notices; it
has no Homebrew runtime dependency. `WITH_LOCAL_STT=0` builds the baseline without
preparing optional runtimes; missing adapters appear unavailable without changing
the saved model choice.

`make test-local-stt` checks synthetic download, cancellation, integrity, explicit
selection, active-model removal protection, missing-model errors and cloud
preference independence without network or speech weights. `make test-settings`
renders catalog/download/retry/recording/unsupported/cloud states in light/dark.
See [LOCAL_STT.md](LOCAL_STT.md) for opt-in native runtime and production-pipeline
verification with preinstalled weights and synthetic speech.

## Activity diagnostics checks

The macOS bundle includes the approved listening-ear application icon. Rebuild
its standard and Retina representations with `sh scripts/build_app_icon.sh`
after updating `assets/ascolto-app-icon.png` (1024px, transparent corners).
The editable vector source is `docs/design/ascolto-app-icon.svg` and the
corresponding FigCraft identity board. Packaging adds `CFBundleIconFile` and
copies the ICNS resource before signing.
`make test-app-icon` renders the production icon loader against the bundle and
checks the approved green artwork, transparent corners and native-sized inset;
this rejects both a generic application placeholder and an oversized raw tile.

`make test-diagnostics` runs the metadata snapshot, guarded retry and cooperative
cancellation fixture tests without provider calls. `make test-activity` renders
and verifies the native AppKit window in light/dark appearance. GTK diagnostics
are covered by `make test-tray` under Xvfb, including filtering and the copy
preview; an SVG loader is required by the existing icon tests. See
[Activity and diagnostics](design/activity-diagnostics.md).

`make test-nemotron-ssd` runs synthetic bounded-cache, fused/separate BF16 and Q8_0 graphs,
reference F32 convolution, native repacker, corpus scorer and native capture-queue
replay tests. Metal, wide-slot and mapping fallback variants are documented in [LOCAL_STT.md](LOCAL_STT.md). The ordinary
model-manager suite includes native preparation, verified descriptors,
cancellation during preparation/verification, corrupt output, directory
replacement, missing preparation ABI and restart cleanup. No speech weights are
required for these suites. Full-model tests use separately installed packages in
a temporary config and never use the microphone or cloud.

Prepare the optional public English evaluation corpus with
`python3 scripts/prepare_nemotron_eval.py`. This downloads the checksum-verified
LibriSpeech test archives into ignored `build/nemotron-eval/`; it selects 20
speakers before inference and creates long, seeded noisy and overlap probes.
Score decoder JSONL with
`python3 scripts/score_nemotron_eval.py --manifest build/nemotron-eval/manifest.tsv --results RUN.jsonl --output REPORT.json`;
add `--baseline BASELINE.jsonl` for transcript and accuracy/performance deltas.
Overlap probes are excluded from WER/CER. This small read speech subset does not
establish conversational-call accuracy.

Replay measured decoder delays through the unchanged portable C capture queue
with `python3 scripts/replay_nemotron_queue.py --manifest build/nemotron-eval/manifest.tsv --results RUN.jsonl --duration-seconds 1800 --output build/nemotron-eval/queue.json`.
The accelerated replay verifies indexed PCM order, equal-duration overflow
silence on both channels and final drain. It does not exercise microphones, VAD,
or live recognition. Run just the lightweight scoring/replay fixtures with
`python3 -m unittest discover -s tests -p 'test_nemotron_eval.py'` and
`python3 -m unittest discover -s tests -p 'test_nemotron_queue_replay.py'`.

## Native post-call queue

`make test-echo` builds the pinned native SpeexDSP dependency and checks speaker
bleed cancellation, quiet double talk, headphone passthrough, deterministic replay
and exact partial-tail drainage. macOS links it statically; capture and archives
stay original. `make test-deferred` additionally verifies processed MIC/original
SYS at the model boundary, checkpoint resume and legacy version-2 compatibility.

`make test-deferred` verifies the durable native store, worker and production
capture/replay pipeline using synthetic PCM and model stubs. It covers checkpoint
resume without duplicate inference or utterances, cancellation, model reuse,
recording priority, corrupted/replaced sources, crash recovery and cleanup after
durable completion. No microphone, model download or cloud service is used.
See [post-call transcription](design/post-call-transcription.md) for lifecycle,
retention and recovery contracts. The shipped macOS app enables this mode;
existing live harnesses remain available for explicit algorithm comparisons.

`make test-retranscribe` verifies historical-call preparation with synthetic
16/48 kHz WAV and native M4A AAC, separate version artifacts, unchanged source
hashes, frozen model/language/vocabulary, duplicate protection, cancellation and
transaction rollback. macOS codec services must be available outside a restricted
sandbox. `make test-activity` exercises the actual Transcribe again sheet/buttons
with mocked enqueue and renders the overview in light/dark appearance.
