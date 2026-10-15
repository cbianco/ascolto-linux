# Ascolto

This repository is a personal fork of `Voloire/ascolto-linux` with both Linux and
macOS implementations. `GNUmakefile` selects the upstream `Makefile` on Linux and
`Makefile.macos` on macOS. Keep upstream Linux sources at their original paths.
The instructions below describe the macOS implementation; do not apply CoreAudio,
AppKit, or macOS capture rules to the PipeWire/GNOME backend. See
`docs/INTEGRATION.md` for the current boundaries and incremental sharing plan.

macOS Menu Bar application in C/Obj-C (`bin/Ascolto.app`) that automatically detects active calls, captures microphone (`MIC` / `ME`) and system audio (`SYS` / `SPEAKER_N`) in parallel, transcribes locally on CPU with **Whistle** (`vendor/needle/libneedle.a` + `models/whistle.cact`), and saves transcripts plus compressed dual-channel audio.

## Commands

- `make test`: builds everything, runs all 10 test suites (`./bin/ascolto-harness test`, 21 checks), including eight acoustic rooms and capture queue/gate boundary verification, plus four concurrency/timeline regressions (`./bin/ascolto-regression`), portable queue/VAD/echo checks, CLI/socket/archive regressions (`./bin/ascolto-control-test`), and the UI smoke test (`./bin/ascolto --smoke-test`). CoreAudio checks and fixture synthesis require access to macOS services outside a restricted sandbox.
- `make room-bench`: exercises the production live pipeline with deterministic headphones, speaker bleed, quiet replies, local interruptions, irregular callback sizes, long turns, and three remote speakers. Writes input WAVs, transcripts, and `tests/room-results/metrics.jsonl`; fails on false `ME` turns, lost expected speech, or incorrect speaker identities.
- `make test-capture`: opt-in real microphone verification using the actual menu start/stop actions and temporary artifacts. Covers three consecutive manual sessions, a five-second hardware startup delay, and a five-second device-inventory query. Checks menu construction within 100ms, action completion during menu tracking, cache population, responsive timers, and stop actions within 100ms. Requires microphone permission; single-session runs have an 18-second timeout and the three-session run has a 32-second timeout.
- `make run`: packages, ad-hoc signs (`codesign -s -`), and launches `bin/Ascolto.app`.
- `./bin/ascolto-harness probe-macos`: inspects CoreAudio devices, active `IN`/`OUT` apps, and `Accessibility`/`Calendar` permissions.
- `./bin/ascolto-harness simulate --mic <wav> --sys <wav> --lang it`: simulates a dual-channel call offline.
- `./bin/ascolto-harness reprocess <call.m4a> --lang it`: splits and re-transcribes a saved stereo `.m4a` call archive (`Ch1=MIC`, `Ch2=SYS`).

## File Map

- `include/ascolto/audio_types.h`, `capture_buffer.h`, and `audio_processing.h`: shared PCM/channel contract, timestamped queue, streaming VAD, echo history and MIC activity state. `src/audio_processing.c` is used by both platforms. `src/filesystem.c` provides shared directory creation. Queue verification lives in `tests/capture_buffer_verify.c`, linked only by test targets.
- `src/audio_queue.c`: portable paired consumer used by Linux; capture producers never run ASR. Linux stop remains STOPPING until all queued PCM and FLAC samples have drained.
- `src/macos/recap.c`: argument-based process spawning, background archive finalization, atomic archive publication, optional recap launch after compression, and background shutdown wait. Cloud recap and TTS are disabled by default. Original local artifacts are retained.
- `src/macos/control.m`: owner-only Unix socket; STATUS reads snapshots, mutations run on the menu's serial action queue. Stop this server off-main before freeing the app.
- `scripts/recap.py`: shared optional cloud worker with Linux directories and macOS M4A adaptation; cloud outputs are separate files. `make recap-deps` installs its isolated environment; `make test-recap` uses mock providers only.
- `tests/compare_diarizers.c`: repeatable comparison of both retained algorithms on identical fixtures (`make compare-diarizers`). Synthetic results do not establish real-call accuracy.

- `include/ascolto.h`: central C API and state structs.
- `src/capture_macos.c`: CoreAudio process monitor, `MIC` capture (`AudioQueue` 16kHz float), and `SYS` capture (`CATapDescription` + `AudioHardwareCreateProcessTap` 48kHz->16kHz).
- `src/capture_buffer.c`: timestamped dual-channel queue, paired 20ms consumption, missing-channel padding, overload diagnostics, and deterministic capture stress verification. Capture callbacks enqueue samples; ASR runs on the consumer thread. Session shutdown drains queued samples before closing artifacts.
- Queue overflow is emitted as equal-duration silence on both channels before retained PCM, including repeated overflow during recovery. Never drop elapsed time or join speech across an unrepresented gap.
- `src/mic_monitor.c`: CoreAudio device enumeration, active `IN`/`OUT` app detection (`kAudioHardwarePropertyProcessObjectList`), window title (`AXUIElement`), and calendar metadata (`EventKit`).
- `src/live_session.c`: macOS session lifecycle, streaming VAD, Speaker Bleed Gate (`SYS->MIC`), and ASR orchestration. `src/macos/transcript.c` owns live `.md`/`.jsonl` and raw-audio resources; `src/macos/recap.c` finalizes `.m4a` archives. Lifecycle state stays in the controller.
- `src/macos/whistle_engine.c`: macOS `needle.h` wrapper (`needle_load`, `needle_transcribe`, `needle_embed`, `needle_stream_*`). The upstream Linux wrapper remains at `src/whistle_engine.c`.
- `src/macos/diarizer.c`: 16-D voiceprint extraction (F0 autocorrelation, Goertzel spectral centroid/rolloff/sub-bands, Whistle 512-d projections) and online cosine clustering (`ME` on `MIC`, `SPEAKER_1..N` on `SYS`). The upstream Linux diarizer remains at `src/diarizer.c`.
- `src/menubar_macos.m` / `src/app_main.c`: `NSStatusBar` UI (`NSApplicationActivationPolicyAccessory`, no Dock icon) and bundle path resolution.
- Menu session actions and capture shutdown run on a serial background queue. Never wait for CoreAudio startup, stream draining, or metadata queries on the main UI thread; use monitor-cached metadata when opening a session.
- All menu reads use `ascolto_live_app_read_snapshot()`, whose mutex is held only during copying. Never acquire the ASR/session mutex from UI code. A committed `STOPPING` transition finishes the current generation even if auto-trigger or pause preferences change; duplicate stops wait on `session_cond` and new starts return busy until it completes.
- CoreAudio device inventory is queried on a dedicated background queue. The main thread owns an immutable cache; `rebuildMenu` must never call CoreAudio directly. Session-action completion and timers use common run-loop modes so menu tracking cannot leave the busy title stale.
- `src/diagnostics.c`: asynchronous timing/state/HAL-operation log beside the configuration file (`~/.config/ascolto/diagnostics.log` by default). Contains no captured PCM, transcripts, application titles, or calendar details. Rotates the previous log on startup when it exceeds 2 MiB. No diagnostic file I/O runs on the UI thread after initialization.
- `src/config.c` / `src/types_config.c`: native settings adapters backed by `~/.config/ascolto/ascolto.sqlite3`. `src/storage.c` owns native commit/export boundaries; `scripts/ascolto_runtime.py` adopts former JSON/INI/keywords/AI metadata with immutable backups, recovers exports, and runs persisted AI jobs.
- `include/ascolto/offline.h`, `src/harness_main.c`, `src/session.c`, `src/vad.c`, `src/wav_audio.c`, and `src/simulated_monitor.c`: autonomous test harness and offline pipeline, excluded from the shipped macOS app.
- `src/core.c`: Linux lifecycle and audio orchestration. `src/control.c` implements its control protocol, `src/observability.c` owns socket transport and telemetry, and `src/recap.c` launches its optional recap worker.
- `Makefile.macos`: shared objects under `build/macos`, generated header dependencies, separate app/offline source lists, and incremental bundle packaging. Preserve both native build paths.
- `src/room_bench.c`: acoustic room synthesis, known dialogue timelines, content/speaker checks, and machine-readable feedback from the live pipeline. Synthetic voices do not establish accuracy on arbitrary real-world speakers.
- `tests/regression_main.c`: production session/VAD/queue regressions with a controllable hardware-drain replacement. Verifies UI snapshots during blocked processing, preference changes and duplicate stops during drain, and preserved VAD/raw-audio time after overflow; opens no real microphone.

## Critical Technical Rules

1. **Never use `ScreenCaptureKit` for `SYS` audio**: it triggers the macOS purple screen-sharing menu bar indicator and spawns `replayd`. Always use CoreAudio `CATapDescription` + `AudioHardwareCreateProcessTap`.
2. **Call start/end detection**: do not rely on `kAudioDevicePropertyDeviceIsRunningSomewhere` while `Ascolto` is recording (it stays `true` because `Ascolto` itself holds the mic open). Always use `ascolto_macos_query_audio_apps()`, which checks `kAudioProcessPropertyIsRunningInput` excluding `getpid()` and system daemons (`CoreSpeech`, `replayd`, `audiomxd`, `controlcenter`).
3. **Speaker Bleed Gate (`SYS -> MIC`)**: on a MacBook without headphones, built-in speaker audio couples into the built-in mic at up to `sys_rms_db + 1 dB`, whereas user speech at desk distance is `-25` to `-35 dB`. Only gate `MIC` while `SYS` is actively speaking (`< 250ms`) and `mic_db < sys_db + 3.5 dB`; when `SYS` is silent, keep `MIC` VAD threshold at `-35.5 dB`.
4. **macOS `.app` bundle environment**: `open bin/Ascolto.app` runs with `cwd=/` and `PATH=/usr/bin:/bin`. Always resolve model paths via `_NSGetExecutablePath` and prepend `/opt/homebrew/bin:/usr/local/bin` to `PATH` when invoking `ffmpeg`.
5. **Call artifacts (`~/Documents/Ascolto/`)**: each call writes `<timestamp>_call.md`, `<timestamp>_call.jsonl`, and `<timestamp>_call.m4a` (48kbps AAC stereo 16kHz: `Left=MIC`, `Right=SYS`, ~21 MB/hour).
