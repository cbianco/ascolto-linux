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
