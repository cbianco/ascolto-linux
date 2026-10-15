# Linux and macOS integration assessment

Reviewed on 2026-10-15 against upstream
[`4be6fb4e7f83b90479cb09534bdb22825e5adf89`](https://github.com/Voloire/ascolto-linux/tree/4be6fb4e7f83b90479cb09534bdb22825e5adf89).
The macOS directory initially had no Git repository, so there is no historical
merge base for its previous development. The import preserves the upstream Git
history and adds the existing macOS sources as a descendant.

## What upstream added

The upstream repository has seven commits at the reviewed revision:

| Commit | Delivered behavior |
| --- | --- |
| `abdf04f` | Repository structure and architecture plan. |
| `4065b82` | Linux C daemon, PipeWire capture and microphone watcher, Whistle integration, diarizer, GNOME tray, Unix control socket, telemetry, and test harness. |
| `9fc438f` | GNOME desktop launchers, SVG icons, executable-relative model lookup. |
| `3e1cd9c` | Stop/restart deadlock fix, D-Bus menu correction, Wayland folder opening. |
| `1f6578d` | Per-channel FLAC recording, cloud retranscription and recap, optional TTS, and autostart target. |
| `c9412ae` | Removal of language submenus and compiler header dependency tracking. |
| `4be6fb4` | Removal of additional tray submenus rendered empty by GNOME. |

These are additions in Linux's own history, rather than measured changes relative
to a previous shared version of the macOS project.

## Feature comparison

| Area | Linux upstream | Existing macOS implementation | Integration implication |
| --- | --- | --- | --- |
| Automatic call detection | PipeWire graph polling, external capture streams, self/helper exclusions, debounce. | CoreAudio external input process inspection with self/daemon exclusions. | Keep native detection backends. |
| Capture | Two `pw-record` subprocesses, MIC and sink monitor, 200 ms reads. | AudioQueue MIC and CoreAudio process tap SYS, normalized to 16 kHz. | Feed a common sample contract from native capture. |
| Audio timeline | Each worker advances its own delivered-sample counter and invokes the core directly. | Timestamped paired queue, 20 ms consumption, missing-channel padding, represented overflow gaps, shutdown drain. | Port the queue and consumer discipline to Linux first. |
| Local transcription | Needle/Whistle with a serialized engine wrapper, keyword normalization, WAV resampling helpers. | Needle/Whistle with word timestamps, offline reprocessing, live hallucination/repetition filtering. | Define a small result adapter before sharing wrappers. |
| Acoustic echo | Cloud postprocessing compares MIC and SYS text after retranscription. No equivalent live cross-channel gate found in the Linux ingestion path. | Live SYS-to-MIC energy gate plus lexical/voiceprint checks, preserving quiet replies and local interruptions. | Share gate/filter code after aligning the two channels. |
| Diarization | 128-dimensional hybrid acoustic/Whistle representation, EMA centroids, up to 32 speakers. | 16-dimensional acoustic/Whistle projection, short-backchannel handling, centroid/pitch checks, up to 16 speakers. | Compare on the same PCM fixtures before choosing an algorithm. Thresholds are not interchangeable. |
| UI and lifecycle | GNOME/AppIndicator with D-Bus menus; stop joins workers outside the core mutex. | AppKit menu, serial action queue, dedicated snapshot mutex, immutable device cache, explicit STOPPING generation and condition variable. | Keep native UI; transfer lifecycle invariants and responsiveness tests. |
| Metadata | Active microphone application. | Active input/output applications, Accessibility window title, EventKit meeting metadata. | Platform metadata stays optional. |
| Audio archive | Separate lossless `mic.flac` and `sys.flac` per call directory. | Compact dual-channel AAC `.m4a`, plus offline reprocessing. | Support both through file adapters; avoid forcing a codec change. |
| Recap and voice | OpenAI retranscription, Claude CLI recap, optional OpenAI TTS; manual and automatic commands. | No cloud recap or TTS. | Reuse the Python postprocessing boundary, explicitly enabled by the user. |
| Control and diagnostics | `ascoltoctl`, Unix socket commands, JSONL telemetry, device/speaker/segment inspection. | Timing/state diagnostics written asynchronously, with bounded log rotation and no captured content. | Reuse command names; serve status from the macOS snapshot. Preserve diagnostics' content boundary. |
| Settings | `config.ini`, string device identifiers. | `config.json`, CoreAudio numeric device identifiers, separate keywords file. | Adapt configuration at the platform boundary. |
| Verification | Eight suites covering configuration, engine, diarization, daemon/IPC, PipeWire, start/stop, FLAC. Some synthetic cases supply expected transcript hints. | Ten harness suites/21 checks, eight acoustic rooms, four concurrency/timeline regressions, UI smoke, optional hardware capture checks. | Share fixtures and acceptance criteria. Synthetic tests do not establish real-call transcription or speaker accuracy. |

The macOS pipeline has additional protection and regression coverage for several
failure modes. This is a source-level comparison, not evidence that one platform
has better recognition accuracy or lower latency on real calls.

## Integration implemented in this fork

- `origin` points to the personal fork; `upstream` points to Voloire's repository.
- Upstream Linux source files, headers, tools, scripts, assets, and `Makefile`
  retain their paths and contents. README and ignore rules have small additions.
- `GNUmakefile` dispatches by the host OS. `Makefile.macos` contains the previous
  macOS build, with two adjusted source paths.
- The two conflicting macOS filenames now live under `src/macos/`: `diarizer.c`
  and `whistle_engine.c`. Their contents are preserved. Other macOS paths remain
  unchanged, including `include/ascolto.h` and the live/capture pipeline.
- Separate source lists keep the two incompatible C APIs out of the same binary.
  Linux uses `include/ascolto/*.h`; macOS uses `include/ascolto.h`. Some exported
  function names overlap with incompatible signatures, so linking both cores or
  including both API families in one translation unit is currently unsupported.
- Dependencies and generated audio, transcripts, diagnostic dumps, and binaries
  are excluded from the macOS import.

This establishes coexistence and upstream ancestry. It does not yet make the
Linux and macOS runtime pipelines share a core or expose Linux-only features in
the macOS menu.

## Small, ordered follow-up changes

1. **Share queue and timing first.** Extract `src/capture_buffer.c`'s portable C
   contract into a standalone header with a common channel type. Keep capture
   callbacks limited to enqueueing, and add one Linux consumer for paired 20 ms
   frames. Carry source timestamps into the queue and represent overflow as
   silence on both channels. Adapt stop to drain the consumer before finalizing
   artifacts. Reuse queue boundary and overflow regressions before adding the
   bleed gate, which requires aligned channels.
2. **Extract VAD and echo filtering as pure C modules.** Move frame gate state
   and transcript filters out of the macOS live controller with small explicit
   inputs: PCM/reference envelope, sample index, transcript, and prior SYS turns.
   Keep the current algorithms and thresholds initially. Run the same acoustic
   rooms against each platform adapter, including quiet replies and barge-in.
3. **Adapt recap at the artifact boundary.** Convert copies of the macOS stereo
   archive into `mic.flac`/`sys.flac`, map JSONL fields, and call the existing
   Python worker. macOS segment times use `start_s`/`end_s`; the Linux recap
   reader expects `start_sec`/`end_sec`. An adapter is required even though both
   formats use `channel` and `speaker`. Keep original local transcripts and
   JSONL, and write cloud results separately: upstream currently replaces
   `transcript.md` and deletes `transcript.jsonl` after a successful recap.
   Replace the compiled Linux `flac2wav` dependency with an optional `ffmpeg`
   decoder, and adapt desktop notifications. Keep credentials and Claude CLI
   dependencies outside the C capture process. No cloud call is needed to test
   conversion, field mapping, argument construction, or artifact preservation.
4. **Reuse CLI commands through a macOS adapter.** Keep `ascoltoctl`'s wire
   commands and implement a local command dispatcher. Read `STATUS` from
   `ascolto_live_app_read_snapshot()`; route mutations through the existing
   session action queue. Do not execute ASR or wait for capture shutdown on the
   AppKit thread. Add stop/drain concurrency tests before exposing commands.
5. **Converge engine and diarization last.** First establish common PCM,
   transcript-result, and ownership contracts. Compare both representations on
   the same speaker fixtures and labeled real calls. Do not transplant numeric
   similarity thresholds between the 16-D and 128-D representations.

Each step can be reviewed independently. Keep CoreAudio/AppKit and
PipeWire/GNOME native; avoid adding a general plugin framework or requiring GLib
in the macOS capture pipeline just to share algorithms.

## Import verification

- macOS: `make test` passed all 21 harness checks, all four concurrency/timeline
  regressions, and the menu UI smoke test. CoreAudio inspection required running
  outside the restricted sandbox. The opt-in real-microphone suite was not run
  because capture behavior was preserved rather than changed.
- Linux: a source-only archive compiled successfully with `make -j2 all` in an
  Ubuntu 24.04 x86_64 container, including the daemon, CLI, audio converter, and
  test harness, with no compiler warnings. `ascolto --help` and
  `ascoltoctl --help` exited successfully. The Linux test harness and live
  GNOME/PipeWire behavior were not exercised in this container.
- Preservation: all 19 original macOS source/header/script/test files checked
  matched the pre-import backup byte-for-byte after accounting for the two
  renamed sources. All 30 upstream files other than README and ignore rules
  matched the reviewed upstream checkout byte-for-byte.
- Both platform recipe selections, documentation links, and Git whitespace
  checks passed. Upstream's reviewed commit is an ancestor of the import.

## Updating from upstream

```sh
git fetch upstream
git log --oneline main..upstream/main
git merge upstream/main
make test
```

Run builds and relevant tests on both operating systems after an update.
`ASCOLTO_PLATFORM=Linux` can select Linux recipes for inspection on another host;
it does not provide a cross compiler or the Linux runtime. Preserve upstream
paths to keep ordinary merges useful.
