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

`GNUmakefile` selects `Makefile.macos` on Darwin. The original Linux `Makefile`
is still selected on Linux. Use plain `make` on both platforms; explicitly
running `make -f Makefile` selects the Linux build even on macOS.
Build outputs are host-specific; use separate checkouts when switching operating
systems with a shared filesystem.

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
checks, concurrency regressions, and a menu smoke test. Fixture generation uses
macOS voices through `say`, plus `ffmpeg` and `rg`; install the named Italian and
English voices from `scripts/gen_fixtures.sh` when needed. macOS audio and speech
services must be accessible outside a restricted sandbox.
`make test-capture` explicitly starts real microphone sessions and requires
microphone permission. It is a separate, opt-in check.

Cloud recap and TTS are currently implemented by the Linux daemon only. Adding
those source files to this fork does not activate cloud processing in the macOS app.
