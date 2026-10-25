# Development commands

`GNUmakefile` selects `Makefile` on Linux and `Makefile.macos` on macOS.
The commands below describe macOS verification and tooling.

- `make test`: builds everything, runs all 10 test suites (`./bin/ascolto-harness test`, 21 checks), including eight acoustic rooms and capture queue/gate boundary verification, plus four concurrency/timeline regressions (`./bin/ascolto-regression`), portable queue/VAD/echo checks, CLI/socket/archive regressions (`./bin/ascolto-control-test`), and the UI smoke test (`./bin/ascolto --smoke-test`). CoreAudio checks and fixture synthesis require access to macOS services outside a restricted sandbox.
- `make room-bench`: exercises the production live pipeline with deterministic headphones, speaker bleed, quiet replies, local interruptions, irregular callback sizes, long turns, and three remote speakers. Writes input WAVs, transcripts, and `tests/room-results/metrics.jsonl`; fails on false `ME` turns, lost expected speech, or incorrect speaker identities.
- `make test-capture`: opt-in real microphone verification using the actual menu start/stop actions and temporary artifacts. Covers three consecutive manual sessions, a five-second hardware startup delay, and a five-second device-inventory query. Checks menu construction within 100ms, action completion during menu tracking, cache population, responsive timers, and stop actions within 100ms. Requires microphone permission; single-session runs have an 18-second timeout and the three-session run has a 32-second timeout.
- `make run`: packages, ad-hoc signs (`codesign -s -`), and launches `bin/Ascolto.app`.
- `./bin/ascolto-harness probe-macos`: inspects CoreAudio devices, active `IN`/`OUT` apps, and `Accessibility`/`Calendar` permissions.
- `./bin/ascolto-harness simulate --mic <wav> --sys <wav> --lang it`: simulates a dual-channel call offline.
- `./bin/ascolto-harness reprocess <call.m4a> --lang it`: splits and re-transcribes a saved stereo `.m4a` call archive (`Ch1=MIC`, `Ch2=SYS`).


For shared persistence and AI checks, see [DATABASE.md](DATABASE.md) and
[AI_WORKFLOWS.md](AI_WORKFLOWS.md). Synthetic tests do not establish real-call
transcription or speaker accuracy.

## Optional native speech engines

Default native builds prepare all three in-process adapters through
`make local-stt-deps`; this builds pinned C/C++ sources and never downloads optional weights.
The build requires Git, CMake and a C/C++ compiler. Linux also requires libcurl,
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
