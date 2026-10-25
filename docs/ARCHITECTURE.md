# Architecture

Ascolto has native Linux and macOS applications with a small portable audio core.
Keep native capture, UI, configuration and archive formats at their platform
boundaries. Linux source paths remain compatible with upstream merges. The
integration history and algorithm comparison are in [INTEGRATION.md](INTEGRATION.md).

## Data flow

```text
Native MIC/SYS capture
    -> timestamped paired PCM queue
    -> one audio consumer (SYS before MIC)
    -> energy gate and streaming VAD
    -> selected in-process local ASR + Whistle speaker embeddings
    -> native speaker identification
    -> committed SQLite transcript + Markdown/JSONL export
    -> optional text recap from local transcript
    -> optional audio archive, then separately opted-in cloud STT/legacy worker
```

Both channels carry 16 kHz mono float PCM. A processing frame contains 320 samples
(20 ms). Capture producers enqueue samples; they never run speech recognition.
The queue retains startup offsets, pads missing channels after a bounded wait,
and represents overflow as equal-duration silence on both channels. Stopping
capture drains every queued sample before transcript/audio resources close.

## Portable modules

- `audio_types.h`: sample rate, frame size, channel identity and naming.
- `capture_buffer.c`: paired sample timeline, padding and overflow accounting.
- `audio_queue.c`: threaded paired consumer used by the Linux backend.
- `audio_processing.c`: RMS, streaming VAD, recent SYS envelope lookup, transcript
  filtering, MIC activity statistics and recent SYS turn history. Both controllers
  use the same echo state and matching rules; native VAD tuning stays explicit.
- `local_stt.c` / `local_stt/`: copied model catalog state, background verified
  downloads, session-frozen engine selection and isolated native adapters. See
  [LOCAL_STT.md](LOCAL_STT.md).
- `filesystem.c`: directory and parent-directory creation.
- `database.c` / `storage.c`: native settings, committed recording data, copied
  pending turns, transcript exports and durable outbox.
- `scripts/ascolto_database.py` / `ascolto_runtime.py`: shared schema, legacy
  adoption/recovery and AI settings/catalog/job writes.
- `scripts/ai_providers.py` / `ai_workflows.py`: secure credentials/provider calls
  and independently selected text recap/cloud transcription tasks.
- `scripts/ascolto_files.py`: durable owner-only text publication for Python workers.
- `scripts/ascolto_job_diagnostics.py`: bounded, content-free per-attempt progress,
  validation locations and safe provider error metadata shared by both native UIs.
- `scripts/recap.py`: optional cloud transcription, recap and TTS. It adapts Linux
  FLAC folders and macOS stereo M4A archives without replacing local artifacts.

## Native responsibilities

| Responsibility | Linux | macOS |
| --- | --- | --- |
| Startup | `main.c` | `app_main.c` |
| Lifecycle and processing | `core.c` | `live_session.c` |
| Capture and monitoring | `audio_backend.c` | `capture_macos.c`, `mic_monitor.c` |
| Transcript writing | `core.c` | `macos/transcript.c` |
| Audio archive and recap launch | `audio_recorder.c`, `recap.c` | `macos/recap.c` |
| Control protocol | `control.c` | `macos/control.m` |
| Socket transport and telemetry | `observability.c` | `macos/control.m`, `diagnostics.c` |
| UI | `tray_ui.c` | `menubar_macos.m` |
| Settings UI and adapters | `linux/settings.inc`, `types_config.c` | `macos/settings.m`, `config.c` |
| Engine and diarization | `whistle_engine.c`, `diarizer.c` | Corresponding files under `macos/` |

The engine/diarizer API families have colliding names and different contracts;
they are not linked into one binary. Their numerical representations, thresholds,
word timing support and ownership rules are preserved. Likewise, pause semantics,
device identifiers, transcript schemas and archive codecs remain native.

On macOS the controller owns lifecycle state; its `files` member groups the
resources owned by the transcript writer. Only the controller changes recording
state. The transcript writer closes files and schedules archive finalization.
UI and CLI mutations share the menu's serial action queue. CoreAudio monitoring
remains independent and uses the controller's synchronized entry points.

## Concurrency invariants

- Audio processing is serialized. No UI read waits on the ASR processing lock.
- macOS UI/status reads copy a snapshot under a dedicated short-held mutex.
- A committed STOPPING transition completes its generation even if preferences
  change. Duplicate stops wait; a new start returns busy until draining finishes.
- Stop/join operations release the controller mutex before waiting for capture.
- Device inventory and call metadata queries run off the UI thread. AppKit owns
  an immutable device cache, and menu construction reads only that cache.
- Menu action completion and timers use common run-loop modes.
- Compression publishes completed archives atomically. Shutdown waits for archive
  jobs off the UI thread. Text recap needs committed text, including without
  audio saving; cloud STT needs a ready original archive and separate upload opt-in.

## Offline tools and verification

`offline.h` contains WAV, simulated-monitor, offline segmentation and report
contracts. `session.c` supports simulation and archive reprocessing, including
legacy recordings with channel startup/callback drift. Its alignment path is
intentionally separate from the timestamped live pipeline.

The macOS app excludes WAV utilities, the simulated monitor, offline session/VAD,
acoustic rooms and the deterministic queue verifier. The harness links these
modules explicitly. Linux keeps its mock/inspection CLI commands available.

Use `make test`, `make test-portable`, `make test-integration`, `make test-ai`,
`make test-recap`, and
`make compare-diarizers` for the relevant suites. `make test-capture` is the
separate opt-in hardware check. Acoustic fixtures and mocked text hints verify
pipeline behavior; they do not establish recognition accuracy on arbitrary calls.

`GNUmakefile` selects the native makefile. macOS shares compiled objects under
`build/macos`, tracks headers and makefile changes, and packages the bundle only
when an input changes. Linux retains its native object build. Use distinct
checkouts when sharing a filesystem between operating systems.
