# Ascolto for macOS

Native menu bar application for local, dual-channel call transcription with
Whistle. The microphone is labeled `ME`; system audio is clustered into
`SPEAKER_1`, `SPEAKER_2`, and subsequent speakers.

This application lives alongside the original Linux daemon in the personal fork
of [Voloire/ascolto-linux](https://github.com/Voloire/ascolto-linux).
See [the integration assessment](docs/INTEGRATION.md) for the feature comparison
and the boundaries between the two implementations.

## Requirements

- Apple Silicon Mac with an SDK and runtime supporting CoreAudio process taps.
- Xcode Command Line Tools (`clang`, `make`, and `codesign`).
- `ffmpeg` available in `/opt/homebrew/bin`, `/usr/local/bin`, or `PATH`.
- Microphone and system audio capture permissions; Accessibility and Calendar
  permissions are optional and supply call metadata.

## Build and run

From the repository root:

```sh
make
make run
```

`GNUmakefile` selects `Makefile.macos` on Darwin. The Linux `Makefile` is selected on Linux. Use plain `make` on both platforms; explicitly
running `make -f Makefile` selects the Linux build even on macOS.
Build outputs are host-specific; use separate checkouts when switching operating
systems with a shared filesystem.

The native build compiles each source once into `build/macos`, tracks header
dependencies, and repackages the app only when its inputs change. Offline
simulation, WAV utilities, acoustic rooms and queue verification are linked into
the harness rather than the shipped app. `make clean` removes native build
objects and binaries.

The macOS dependency target downloads the Apple Silicon Needle library and
Whistle model on first use. They remain outside Git. The app bundle is
`bin/Ascolto.app` and resolves its model relative to the executable when launched
from Finder.

Settings are saved in `~/.config/ascolto/config.json`, with custom vocabulary in
`~/.config/ascolto/keywords.txt`. Call artifacts are saved under
`~/Documents/Ascolto/` as `<timestamp>_call.md`, `<timestamp>_call.jsonl`, and,
when audio saving is enabled, `<timestamp>_call.m4a`.
The AAC archive is stereo at 16 kHz and 48 kbps: left is MIC, right is SYS.

## Verification and offline tools

```sh
make test
make room-bench
make test-capture
./bin/ascolto-harness probe-macos
./bin/ascolto-harness simulate --mic mic.wav --sys sys.wav --lang it
./bin/ascolto-harness reprocess call.m4a --lang it
```

`make test` includes speech synthesis fixtures, acoustic rooms, capture queue
checks, concurrency regressions, CLI/socket and archive-finalization regressions,
and a menu smoke test. Fixture generation uses
macOS voices through `say`, plus `ffmpeg` and `rg`; install the named Italian and
English voices from `scripts/gen_fixtures.sh` when needed. macOS audio and speech
services must be accessible outside a restricted sandbox.
`make test-capture` explicitly starts real microphone sessions and requires
microphone permission. It is a separate, opt-in check.

## Optional cloud recap and TTS

Cloud recap and TTS are disabled by default. Local capture and Whistle
transcription continue to work without Python or cloud credentials.

```sh
make recap-deps
make test-recap
```

The source checkout and its app bundle automatically find the project's `.venv`
when `recap_python` has its default value `python3`. For a standalone app copied
elsewhere, set `recap_python` in `~/.config/ascolto/config.json` to the absolute
path of a Python interpreter with `scripts/requirements-recap.txt` installed.
Quit the app before manually editing its configuration file.

The cloud pipeline needs an OpenAI API key and an authenticated `claude` CLI.
The existing Linux credential lookup is retained: `OPENAI_API_KEY`, or an
explicit `op://` reference in `ASCOLTO_SECRETS_ENV` / `~/dotfiles/secrets/secrets.env`
resolved by 1Password. A Finder-launched app does not inherit shell environment
variables. Credentials are not copied into the app configuration.

Enable **Riepilogo nel cloud (OpenAI + Claude)** in the menu, or use
`ascoltoctl SET_CLOUD_RECAP on`. The pipeline sends per-channel audio to OpenAI,
then sends the scrubbed transcript to Claude CLI for the recap. Optional TTS
sends the scrubbed spoken summary to OpenAI. No cloud request runs merely from
building the app, installing dependencies, or running the tests.
Audio required for recap is saved for sessions started with cloud recap enabled,
even if the separate audio-save setting is off. Audio-save changes take effect
on the next session. Disabling cloud recap before stop prevents automatic recap.

Compression runs in the background and atomically publishes the completed
archive before starting automatic recap. Shutdown waits for archive jobs off the
UI thread. Failed compression retains the raw audio for recovery. Original
`.md`, `.jsonl`, and `.m4a` files are preserved; cloud results go into the sibling
`<timestamp>_call_recap/` directory:

- `mic.flac`, `sys.flac`, copied `transcript.md` / `transcript.jsonl`;
- `transcript.cloud.md`, `transcript.cloud.jsonl`, `recap.md`, `recap.spoken.txt`;
- optional `recap.mp3`.

Run adaptation without credentials or network requests:

```sh
.venv/bin/python3 scripts/recap.py --archive /path/to/call.m4a --prepare-only
```

Recap process output is appended to `recap.log` beside the configuration file.
Diagnostics remain limited to states and timings. The script rejects concurrent
recaps for the same session. Local artifact adaptation and mocked provider
success/failure are covered by `make test-recap`.

## Local CLI

The app exposes a private Unix socket and builds `bin/ascoltoctl` from the same
CLI source used on Linux. `STATUS` reads the short-lived snapshot lock;
mutations run on the menu's serial session-action queue. Client disconnects
cannot terminate the app, and an occupied socket or existing regular file is
never overwritten.

```sh
./bin/ascoltoctl STATUS
./bin/ascoltoctl START
./bin/ascoltoctl STOP
./bin/ascoltoctl SET_LANG it
./bin/ascoltoctl SET_CLOUD_RECAP on
./bin/ascoltoctl RECAP
```

Supported commands are `STATUS`, `GET_TRAY_STATE`, `START`, `STOP`, `PAUSE`,
`RESUME`, `SET_AUTO_MIC`, `SET_SAVE_AUDIO`, `SET_CLOUD_RECAP`, `SET_RECAP_TTS`,
`SET_LANG`, `RECAP`, `MAKE_MP3`, and `PREPARE_RECAP`.
`RECAP`, `MAKE_MP3`, and `PREPARE_RECAP` accept an optional path to the original
`*_call.md`; without a path they use the current app's last session. Explicit
paths also support sessions from before the app was restarted.
`PREPARE_RECAP` performs local adaptation only. Recap commands require a stopped
session, and audio adaptation requires the completed M4A. Wait for finalization
if invoked immediately after stopping.

The default socket is `$TMPDIR/ascolto.sock`, with owner-only access; the fallback
is `/tmp/ascolto-<uid>.sock`. Override it with `--socket <path>` on the app and
`-s <path>` on the CLI. Linux-specific device and synthetic-injection commands
remain Linux commands; the macOS CLI help lists its supported subset.
