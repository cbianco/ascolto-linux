# macOS implementation rules

These rules apply to the CoreAudio/AppKit backend. Keep Linux PipeWire/GNOME
behavior at its native boundary; see [ARCHITECTURE.md](ARCHITECTURE.md).

## Capture and archives

1. **Never use `ScreenCaptureKit` for `SYS` audio**: it triggers the macOS purple screen-sharing menu bar indicator and spawns `replayd`. Always use CoreAudio `CATapDescription` + `AudioHardwareCreateProcessTap`.
2. **Call start/end detection**: do not rely on `kAudioDevicePropertyDeviceIsRunningSomewhere` while `Ascolto` is recording (it stays `true` because `Ascolto` itself holds the mic open). Always use `ascolto_macos_query_audio_apps()`, which checks `kAudioProcessPropertyIsRunningInput` excluding `getpid()` and system daemons (`CoreSpeech`, `replayd`, `audiomxd`, `controlcenter`).
3. **Speaker Bleed Gate (`SYS -> MIC`)**: on a MacBook without headphones, built-in speaker audio couples into the built-in mic at up to `sys_rms_db + 1 dB`, whereas user speech at desk distance is `-25` to `-35 dB`. Only gate `MIC` while `SYS` is actively speaking (`< 250ms`) and `mic_db < sys_db + 3.5 dB`; when `SYS` is silent, keep `MIC` VAD threshold at `-35.5 dB`.
4. **macOS `.app` bundle environment**: `open bin/Ascolto.app` runs with `cwd=/` and `PATH=/usr/bin:/bin`. Always resolve model paths via `_NSGetExecutablePath` and prepend `/opt/homebrew/bin:/usr/local/bin` to `PATH` when invoking `ffmpeg`.
5. **Call artifacts (`~/Documents/Ascolto/`)**: each new call owns a timestamped/title-based folder with `transcript.md`, `Data/transcript.jsonl`, `recap.md`, and `Audio/recording.m4a` (48kbps AAC stereo 16kHz: `Left=MIC`, `Right=SYS`, ~21 MB/hour).

## UI and lifecycle

- Session actions and capture shutdown run on the serial background action queue.
  Never wait for CoreAudio startup, draining or metadata queries on the UI thread;
  use monitor-cached metadata when opening a session.
- UI reads use `ascolto_live_app_read_snapshot()`, holding its mutex only to copy.
  Never acquire the ASR/session mutex from UI code.
- A committed STOPPING transition completes its generation despite preference
  changes. Duplicate stops wait on `session_cond`; new starts return busy until
  completion.
- Query device inventory on its dedicated background queue. The main thread owns
  an immutable cache; `rebuildMenu` never calls CoreAudio directly.
- Action completion and timers use common run-loop modes, including menu tracking.
- Stop the owner-only control socket off-main before freeing the app. STATUS reads
  snapshots; mutations use the menu action queue.
- Diagnostics contain no PCM, transcripts, application titles or calendar details.
  Write asynchronously beside the configuration, rotating at startup above 2 MiB.
  No diagnostic file I/O runs on the UI thread after initialization.

## Source boundaries

- `include/ascolto.h`: central API and state.
- `capture_macos.c` / `mic_monitor.c`: capture, device inventory and call metadata.
- `live_session.c`: lifecycle, VAD, bleed gate and ASR orchestration.
- `macos/transcript.c`: live transcript and raw-audio resources.
- `macos/recap.c`: background compression, atomic publication and optional worker
  launch using argument-based spawning; shutdown waits off-main. Retain originals.
- `macos/whistle_engine.c` / `macos/diarizer.c`: native engine and speaker identity.
  Keep their contracts separate from the Linux files at the original source paths.
- `menubar_macos.m` / `macos/settings.m` / `app_main.c`: native UI and bundle startup.
- `Makefile.macos`: `build/macos` objects, header dependencies and separate app/offline
  source lists. Offline harness and room synthesis stay out of the shipped app.

Paths above are relative to `src/` unless otherwise specified. Shared modules and
Linux responsibilities are mapped in [ARCHITECTURE.md](ARCHITECTURE.md); SQLite
commit/export boundaries are defined in [DATABASE.md](DATABASE.md).
