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

## Baseline feature comparison at import

| Area | Linux upstream | Existing macOS implementation | Integration implication |
| --- | --- | --- | --- |
| Automatic call detection | PipeWire graph polling, external capture streams, self/helper exclusions, debounce. | CoreAudio external input process inspection with self/daemon exclusions. | Keep native detection backends. |
| Capture | Two `pw-record` subprocesses, MIC and sink monitor, 200 ms reads. | AudioQueue MIC and CoreAudio process tap SYS, normalized to 16 kHz. | Feed a common sample contract from native capture. |
| Audio timeline | Each worker advances its own delivered-sample counter and invokes the core directly. | Timestamped paired queue, 20 ms consumption, missing-channel padding, represented overflow gaps, shutdown drain. | Port the queue and consumer discipline to Linux first. |
| Local transcription | Needle/Whistle with a serialized engine wrapper, keyword normalization, WAV resampling helpers. | Needle/Whistle with word timestamps, offline reprocessing, live hallucination/repetition filtering. | Define a small result adapter before sharing wrappers. |
| Acoustic echo | Cloud postprocessing compares MIC and SYS text after retranscription. No equivalent live cross-channel gate found in the Linux ingestion path. | Live SYS-to-MIC energy gate plus lexical checks, preserving quiet replies and local interruptions. | Share gate/filter code after aligning the two channels. |
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

## Integration implemented after the import

1. **Common audio contract and queue.** Both API families include
   `include/ascolto/audio_types.h` and `capture_buffer.h`. The existing queue
   algorithm is now portable C. Linux's PipeWire readers only enqueue; one
   consumer feeds paired 20 ms intervals, SYS first, and pads a missing channel
   after a bounded wait. Overflow is represented as silence on both timelines.
   Stop joins producers and drains the consumer before closing artifacts. Linux
   has an explicit STOPPING transition, rejects restart during drain, and makes
   duplicate stops wait for the same session. Direct PCM ingestion also pads
   timestamp gaps and ignores already-consumed overlap.
2. **Shared VAD and filtering.** `src/audio_processing.c` contains the streaming
   VAD, RMS calculation, timestamp-based SYS reference, lexical echo check, and
   hallucination/repetition filter. Both platforms call these functions. Linux
   adds the live energy gate and turn-level lexical check while retaining raw
   audio for postprocessing. Its existing mock speech commands use their
   explicit text hints with real encoder embeddings; synthetic formants are not
   a transcription-accuracy test.
3. **Shared recap worker and archive adaptation.** `scripts/recap.py` accepts
   Linux call directories and macOS stereo archives. It validates stereo channel
   identity, adapts audio through ffmpeg, accepts both JSONL timestamp schemas,
   and writes cloud results separately. Local Markdown, JSONL, and source audio
   survive both success and failure. A per-session lock prevents duplicate
   processing. macOS atomically publishes its compressed archive before starting
   opt-in automatic recap; shutdown waits for compression off the UI thread.
   Native process launches use argument arrays rather than shell interpolation.
4. **Compatible CLI on macOS.** The original `tools/ascoltoctl.c` builds on both
   platforms. The macOS server exposes status and session/settings/recap commands;
   state reads use the UI snapshot and mutations use the menu's existing serial
   action queue. Socket permissions are 0600, regular files and active sockets
   are preserved, slow mutations do not block other status clients, and shutdown
   stops the server before destroying session state.
5. **Measured diarization comparison.** `make compare-diarizers` runs both
   unchanged algorithms with the host's Needle library on identical PCM. On the
   five existing fixture turns (Alice, low male, Alice, low male, third voice),
   macOS matched **5/5** expected labels and Linux matched **4/5**: Linux assigned
   the third voice to SPEAKER_1. This is a small synthetic test, not a real-call
   accuracy estimate. Both algorithms and their own thresholds are retained.
   A common engine/diarizer API remains a future change that needs more labeled
   speaker recordings; no representation or threshold was silently replaced.

Native CoreAudio/AppKit and PipeWire/GNOME capture/UI remain separate. Shared
modules require C, pthreads, and libm; they do not require GLib on macOS. The two
engine/diarizer API families still have incompatible symbols and must not be
linked into the same binary. The comparison builds one executable per algorithm.

## Verification and limits

- macOS: the 21 harness checks and eight acoustic rooms, four concurrency/timeline
  regressions, menu smoke test, and new CLI/socket/archive tests pass. The new
  server test reads STATUS while a mutation is blocked on the processing mutex,
  checks socket ownership behavior, and uses a fake interpreter to verify that
  automatic recap starts only after archive publication. It opens no microphone.
- Linux: all eight upstream suites, the common portable tests, and new
  drain/concurrency/FLAC-duration regressions pass in Ubuntu 24.04 x86_64. The
  controlled consumer blocks while a committed stop, duplicate stop, preference
  changes, and rejected restart compete; both completed FLAC channels retain the
  exact sample count including a partial final frame.
- Portable tests cover startup offsets, missing channels, repeated overflow while
  recovering a gap, blocked-consumer overflow, partial tails, quiet local replies,
  interruptions, gate release, repetition filtering, and lexical echo checks.
  AddressSanitizer and UndefinedBehaviorSanitizer also pass on this target.
- Recap tests use synthetic stereo audio and mock providers. They verify channel
  identity, both JSONL schemas, original-file retention, failure recovery, stereo
  validation, and concurrent-job rejection. No real cloud API or credentials
  are used by the tests. The transcription request contract was checked against
  [official OpenAI documentation](https://developers.openai.com/api/reference/resources/audio/subresources/transcriptions/methods/create).
- The container has no live PipeWire server, GTK display, or installed ALSA speech
  fixtures. Upstream tests fall back to default device entries and headless UI;
  these results do not validate physical Linux capture or GNOME rendering.
  Linux GTK menu mutations now use a serial background queue; see [native menu design](UI.md).
- The raw `pw-record` pipe has no hardware timestamps. Linux estimates each
  stream's initial onset from its first delivered block and subsequently counts
  PCM samples on a common monotonic epoch. This preserves helper startup offsets
  but is not hardware-clock synchronization or sample-clock drift correction.
- The optional real-microphone macOS suite was not run. No live cloud request or
  real-call diarization accuracy evaluation was performed.

## Setup and repeatable checks

Use plain `make` so `GNUmakefile` selects the host. Linux sources stay at upstream
paths; shared sources are explicitly added to its native Makefile. macOS sources
with colliding names remain under `src/macos/`. Build outputs are host-specific.

```sh
make test
make test-portable
make recap-deps
make test-recap
make compare-diarizers
```

`make recap-deps` creates the ignored `.venv` for NumPy, requests and OAuth dependencies. Both launchers
prefer it when available. `make compare-diarizers` needs the speech fixtures;
macOS can generate them with `make fixtures`, while Linux can receive the same
fixtures and override `FIXTURE_DIR` to their location. Recap setup and CLI details
are in [README.macos.md](../README.macos.md).

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

## Complexity reduction after integration

The refactor was checked against `04e9299` on 2026-10-15. Native behavior and
algorithms are preserved; this changes module ownership and removes duplication.

- macOS compiles shared objects once, tracks header/makefile dependencies, and
  packages the bundle incrementally. WAV utilities, simulated monitoring, offline
  processing, acoustic rooms and queue verification are excluded from the app.
- `offline.h` holds the harness/reprocessing contracts. The native umbrella header
  shrank from 499 to 367 lines. The unused device-running monitor API and
  write-only controller/UI fields were removed; active-call detection still uses
  external audio processes.
- `audio_processing.c` owns the shared recent-turn echo history, MIC activity
  counters and reset/matching operations. Both controllers retain their native
  VAD settings, channel order, timestamp handling and synchronization.
- The macOS transcript writer owns grouped file resources in `app.files`; the
  controller owns lifecycle transitions. `live_session.c` shrank from 816 to 551
  lines. Linux recap launching lives in `recap.c`, control commands in `control.c`,
  and socket transport/event storage in `observability.c` (570 to 156 lines).
- Directory creation, Linux device updates and boolean control commands share
  small helpers. macOS menu construction reads the snapshot directly and uses a
  common item constructor. Its UI module shrank from 737 to 650 lines.
- No common engine or diarizer replacement was introduced: the algorithms and
  ownership contracts differ. Legacy offline drift correction remains separate
  from the synchronized live pipeline. These are deliberate boundaries, not
  additional unification work required by this refactor.

### Verification

- macOS: 21/21 harness checks, all eight acoustic rooms, all four lifecycle/timeline
  regressions, CLI/socket/archive regressions and menu smoke test pass.
- Linux: all eight upstream suites and drain/concurrency/FLAC-duration regressions
  pass in an isolated Ubuntu 24.04 x86_64 container with both Clang and GCC. The container has
  no physical PipeWire server or GNOME display, as in the earlier integration
  verification. Transient GCC internal compiler crashes
  occurred under x86_64 emulation in the baseline and a refactored build. A retry
  completed the full default-GCC suite successfully; Clang supplies the complete
  baseline/refactor comparison.
- Portable tests, including added history rollover/reset/interruption and MIC activity
  accumulation/take/reset regressions, pass with AddressSanitizer and UndefinedBehaviorSanitizer.
- The diarizer comparison retains the baseline results: macOS 5/5 fixture labels,
  Linux 4/5. The existing Linux third-speaker mismatch is unchanged.
- All seven mocked recap tests pass. No cloud request or real-microphone capture
  was used for this refactor.
- The 43 live utterances across the eight rooms match the baseline exactly for
  text, channel, speaker identity, timestamps, similarity, F0 and RMS. All room
  metrics and the offline simulation trace also match; variable ASR timing fields
  are excluded from trace comparisons.
- A fresh macOS object build passes without warnings. Header dependency rebuilding,
  an unchanged build/bundle, app symbol exclusion and bundle signature were checked.

### Performance comparison

The same Apple Silicon host, model and eight-room workload were used for four
runs per revision, alternating execution order. Values below are medians; they
are synthetic pipeline measurements rather than a real-call accuracy benchmark.

| Metric | Baseline | Refactored |
| --- | --- | --- |
| Wall time | 2.892 s | 2.900 s |
| Process CPU time | 7.495 s | 7.500 s |
| Peak resident memory | 86.14 MiB | 84.56 MiB |

The elapsed/CPU differences are below 0.3%/0.1%, respectively. No processing
regression was observed at this measurement precision. An unchanged native
build/bundle takes about 0.036 s and runs no compiler or packaging commands.
There are 183 fewer source/header/script/test lines overall, including the new
regression; the main reduction is in duplicated state and mixed responsibilities.

## Native settings, SQLite and AI workflow integration

Both adapters now use the shared SQLite schema for native/AI settings, vocabulary,
sessions, utterances, job history and artifact references. Native capture backends,
Whistle defaults, acoustic gating and drain/overflow timeline rules are preserved.
Session actions and consumers commit before derived transcript publication;
background bootstrap adopts legacy data with immutable backups and recovers the
export outbox. No captured PCM is stored in SQLite. Native writes cover recording,
settings and exports; Python alone mutates AI connections, catalogs, preferences
and jobs. See [DATABASE.md](DATABASE.md) for commit, queue, adoption and recovery
contracts.

The native Settings windows expose independent recap and optional post-call cloud
transcription choices. Text recap works without audio saving; full-audio cloud
transcription requires a separate model and explicit upload consent. Provisional
and final notes use evidence-backed structured workflow outputs and independent
job phases. Each attempt retains its own artifact directory. Secure-store names
and OAuth host/account/workspace registrations survive metadata adoption.

Verification includes the existing macOS harness/rooms/lifecycle/socket/menu tests,
provider/workflow/legacy mocks, native and 24 Python DB checks, native storage
fault/recovery checks (also passing ASan/UBSan) and eleven end-to-end wiring tests. AppKit light/dark previews
cover all categories and provider profiles with a transcription fixture. GTK runs
real settings/session callbacks under Xvfb, including a SQLite writer-busy
responsiveness check. Linux builds run only in an isolated container. These tests
do not establish physical Linux capture, full GNOME Shell behavior, live provider
eligibility or real-call recognition accuracy. No real credentials, paid provider
request, production installation or user-data migration is part of integration QA.

Archive shortcuts are restored from ready SQLite artifact references before native
UI startup, without presenting historical speech as an active recording. Dedicated
AppKit and GTK regressions cover legacy import, restart and a changed future output
directory. GTK preview verification requires a GdkPixbuf SVG loader; on
Debian/Ubuntu install `librsvg2-common`. The fixture uses the built-in Adwaita theme
with its dark preference, rather than depending on an external Adwaita-dark theme.
