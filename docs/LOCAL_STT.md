# Optional local transcription engines

Whistle is included and selected by default. Settings → Transcription → On device
adds three optional local packages. Downloading verifies every manifest artifact
before publication and does not activate it. Use model saves an independent
next-session preference. A running session keeps its decoder until its final
queued audio and VAD turns have drained.

| Engine | Complete download | Runtime | Initial text API |
| --- | --- | --- | --- |
| Whistle | Included, about 17 MB | Existing Needle C API | Existing word data |
| Whisper large-v3-turbo Q5 | 574,041,195 bytes | whisper.cpp v1.9.5 | Completed mono turn |
| Parakeet TDT v3 Q8 | 940,663,680 bytes | parakeet.cpp | Completed mono turn |
| Qwen3-ASR 1.7B | 4,703,112,789 bytes | antirez/qwen-asr | Completed mono turn |

Optional decoders receive the same gated, VAD-completed 16 kHz mono turns as
Whistle. SYS precedes MIC, processing stays serialized, and capture only enqueues.
Whistle continues to extract speaker embeddings. These first adapters return
text and a language only when available. No word timestamps, probabilities or
first-token measurements are fabricated. Existing lexical echo and repetition
filters still run; silence never bypasses the production gate/VAD. Parakeet uses
automatic language detection and does not apply custom keyword prompts.

Whisper and Parakeet use Metal/CPU on macOS and CPU on Linux. Qwen uses native C
with Accelerate on macOS and OpenBLAS on Linux. Each adapter exports only
`ascolto_stt_get_api`, a versioned C ABI. Its complete runtime and its own ggml
revision are statically linked with hidden symbols. The app loads one decoder at
a time with `RTLD_LOCAL`, releases the model at session end, then unloads the
adapter. Parakeet explicitly calls `pk::shutdown_backend()` after freeing its
model. There is no Python inference or child-process ASR protocol.

## Model storage and failure behavior

`resources/local-stt-manifest.json` records pinned source revisions, complete file
sizes and SHA-256 digests. `scripts/generate_local_stt_manifest.py` validates and
embeds that manifest. Update both files intentionally when changing a package.
The bundled [third-party notices](../resources/LOCAL_STT_NOTICES.md) record runtime
pins, model provenance, quantization and licenses.

Packages live in `<config-parent>/local-models/<model-id>/`. Downloads use a
private sibling `.download` directory, HTTPS-only libcurl requests, bounded
redirects, exact byte limits and SHA-256 verification. Completion publishes the
verified directory; cancellation/error removes partial files. Startup removes
known interrupted partials, checks installed packages on a worker, and publishes
copied progress/status snapshots. Model selection and disk work run on native
background action queues. UI reads never wait for model loading, hashing or ASR.
The complete selected package is verified again before each session loads it.

One download runs at a time. Cancel and retry target only that package. Removing
an optional model requires it to be both inactive and unselected; Whistle cannot
be removed. A missing, corrupt or incompatible selected package prevents session
start, preserves its saved ID, and offers explicit download retry or Whistle
selection. Inference errors remain visible, mark the call failed and suppress
automatic AI processing; retained audio can be recovered explicitly. There is no
silent engine or cloud fallback. Local model actions never mutate cloud upload
consent, cloud transcription choices or recap preferences.

## Build and verification

`make` prepares pinned native source dependencies and packages the adapters.
`make local-stt-deps` builds them explicitly. Source checkouts/builds are ignored
under `build/local-stt`; adapter outputs and license texts are under
`bin/local-stt`. The script can reuse pinned local source checkouts through
`ASCOLTO_WHISPER_SOURCE`, `ASCOLTO_PARAKEET_SOURCE` and `ASCOLTO_QWEN_SOURCE`.
It reuses the existing pinned benchmark sources when present. No optional weights are
downloaded by this build step. macOS bundles these libraries under Resources/local-stt,
with system framework dependencies only.

`make test-local-stt` uses tiny synthetic artifacts and a fake native adapter,
without network, microphones, speech weights or cloud credentials. It checks
integrity, progress, cancellation, explicit activation, session pinning, removal
protection, missing-model errors, unknown persisted IDs and cloud independence.
The explicit `make test-local-stt-download` smoke check downloads only one pinned
6 KB public model configuration to verify HTTPS transport, progress and integrity;
it never downloads speech weights or activates a model.
AppKit previews cover both tabs and all catalog states; GTK keeps the same native
controls and independent cloud surface.

For explicit verification with already installed packages in a temporary config
folder, build and run:

```sh
make bin/ascolto-local-stt-runtime bin/ascolto-local-stt-pipeline
./bin/ascolto-local-stt-runtime /absolute/temp/config.json /absolute/synthetic-speech.wav
./bin/ascolto-local-stt-pipeline /absolute/temp/config.json /absolute/synthetic-speech.wav /absolute/whistle.cact
```

These macOS tools never capture or upload audio. The runtime check switches
Whisper → Parakeet → Qwen → Whisper → Parakeet in one process. The production
check runs on a pthread with a 512 KiB stack, matching the macOS audio consumer,
and feeds synthetic speech and silence through VAD, the selected ASR, Whistle
speaker embeddings and SQLite. It also changes the next-session preference while
each alternative remains active. Install packages according to the manifest's
filenames under the temporary configuration's `local-models/` directory first.

On Apple Silicon, all three real adapters and production pipeline checks passed,
including repeated switching, observed Whisper Metal and Parakeet MTL0, successful
backend teardown, retained speaker identities and zero turns on silence. Existing
macOS harness/rooms, lifecycle, socket, storage and mocked AI suites passed.
These are behavior checks with synthetic speech, not recognition accuracy results.
No real microphone or cloud request is part of this verification.

Linux x86_64 was verified in an isolated Ubuntu 24.04 container: all baseline
harness suites, drain/concurrency regressions, the model manager and real GTK
callbacks/previews under Xvfb passed. All three CPU adapters compiled with Clang;
each ELF library exports only the versioned Ascolto entry point. An initial GCC
compiler process crashed under x86_64 emulation; the Clang builds completed. No
physical PipeWire/GNOME session or Linux recognition performance was measured.
The optional real-microphone macOS suite was not run.
